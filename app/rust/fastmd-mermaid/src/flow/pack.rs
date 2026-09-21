//! Parts of a picture that nothing joins, fitted together instead of stretched across one row.
//!
//! Inside a subgraph it is common for most of the contents to be unconnected - a list of libraries, a set of tools -
//! and for one part to be a long chain. Laid out as one graph they all share the chain's rows: the unconnected ones
//! stand in a single row as wide as the whole picture, and the space beside the chain stays empty. So each part is
//! laid out on its own and the rectangles are then packed, which is what fills that space.

use super::sugiyama::{run, SEdge, SNode, SOut};
use super::Cfg;

/// Groups of nodes that are joined to each other, each group in the order the nodes were written.
pub fn parts(n: usize, edges: &[SEdge]) -> Vec<Vec<usize>> {
    let mut owner: Vec<usize> = (0..n).collect();
    fn root(owner: &mut Vec<usize>, mut v: usize) -> usize {
        while owner[v] != v {
            owner[v] = owner[owner[v]];
            v = owner[v];
        }
        v
    }
    for e in edges {
        if e.from >= n || e.to >= n {
            continue;
        }
        let (a, b) = (root(&mut owner, e.from), root(&mut owner, e.to));
        if a != b {
            owner[a.max(b)] = a.min(b); // the earliest node names the part
        }
    }
    let mut groups: Vec<Vec<usize>> = Vec::new();
    let mut index: Vec<Option<usize>> = vec![None; n];
    for v in 0..n {
        let r = root(&mut owner, v);
        match index[r] {
            Some(g) => groups[g].push(v),
            None => {
                index[r] = Some(groups.len());
                groups.push(vec![v]);
            }
        }
    }
    groups
}

/// Lays out every part on its own and fits the results together.
pub fn run_parts(nodes: &[SNode], edges: &[SEdge], groups: &[Vec<usize>], cfg: &Cfg, aspect: f32) -> SOut {
    let mut laid: Vec<(SOut, &Vec<usize>, Vec<usize>)> = Vec::with_capacity(groups.len());
    for group in groups {
        let mut local = vec![usize::MAX; nodes.len()];
        for (i, &v) in group.iter().enumerate() {
            local[v] = i;
        }
        let sub_nodes: Vec<SNode> = group.iter().map(|&v| SNode { w: nodes[v].w, h: nodes[v].h }).collect();
        let mut mine: Vec<usize> = Vec::new(); // which of the caller's edges ended up in this part
        let mut sub_edges: Vec<SEdge> = Vec::new();
        for (i, e) in edges.iter().enumerate() {
            if e.from >= nodes.len() || local[e.from] == usize::MAX {
                continue;
            }
            mine.push(i);
            sub_edges.push(SEdge {
                from: local[e.from],
                to: if e.to < nodes.len() && local[e.to] != usize::MAX { local[e.to] } else { local[e.from] },
                label_w: e.label_w,
                label_h: e.label_h,
            });
        }
        laid.push((run(&sub_nodes, &sub_edges, cfg), group, mine));
    }

    let sizes: Vec<(f32, f32)> = laid.iter().map(|(o, _, _)| (o.width, o.height)).collect();
    let (spots, width, height) = fit(&sizes, cfg.nodesep, aspect);

    let mut pos = vec![(0.0, 0.0); nodes.len()];
    let mut paths: Vec<Vec<(f32, f32)>> = vec![Vec::new(); edges.len()];
    let mut label_at: Vec<Option<(f32, f32)>> = vec![None; edges.len()];
    for (k, (out, group, mine)) in laid.iter().enumerate() {
        let (ox, oy) = spots[k];
        for (i, &v) in group.iter().enumerate() {
            pos[v] = (out.pos[i].0 + ox, out.pos[i].1 + oy);
        }
        for (sub, &orig) in mine.iter().enumerate() {
            paths[orig] = out.paths[sub].iter().map(|p| (p.0 + ox, p.1 + oy)).collect();
            label_at[orig] = out.label_at[sub].map(|p| (p.0 + ox, p.1 + oy));
        }
    }
    SOut { pos, paths, label_at, width, height }
}

/// Packs the parts a few times over, at several widths, and keeps the tidiest result. Guessing one width from the
/// total area alone goes wrong on small groups: three wide, short boxes have no arrangement anywhere near the shape
/// the guess asks for, and they end up in a single column.
fn fit(sizes: &[(f32, f32)], gap: f32, aspect: f32) -> (Vec<(f32, f32)>, f32, f32) {
    let area: f32 = sizes.iter().map(|(w, h)| (w + gap) * (h + gap)).sum();
    let widest = sizes.iter().map(|(w, _)| *w).fold(0.0f32, f32::max);
    let base = (area * aspect.max(0.05)).sqrt().max(widest);
    if sizes.len() > 60 {
        return fit_at(sizes, gap, base); // too many parts to try several shapes; the guess will do
    }
    // Never so narrow that the tallest part has nothing beside it: a lone node stacked under a chain reads as the
    // step after it, which is exactly what it is not.
    let tallest = (0..sizes.len()).max_by(|&a, &b| sizes[a].1.partial_cmp(&sizes[b].1).unwrap()).unwrap_or(0);
    let others = (0..sizes.len()).filter(|&i| i != tallest).map(|i| sizes[i].0).fold(0.0f32, f32::max);
    let side_by_side = sizes[tallest].0 + gap + others;
    let mut best: Option<(f32, (Vec<(f32, f32)>, f32, f32))> = None;
    for step in [0.55f32, 0.7, 0.85, 1.0, 1.2, 1.45, 1.75, 2.1, 2.6, 3.2] {
        let out = fit_at(sizes, gap, (base * step).max(widest).max(side_by_side));
        let shape = (out.1 / out.2.max(1.0)) / aspect;
        let score = out.1 * out.2 * (1.0 + 0.25 * shape.ln().abs()); // compact first, the asked-for shape second
        if best.as_ref().is_none_or(|(b, _)| score < *b) {
            best = Some((score, out));
        }
    }
    best.unwrap().1
}

/// Bottom-left packing against a skyline: each rectangle goes where the ground is lowest, leftmost on a tie. Tall
/// parts go down first, so the short ones have somewhere to tuck into.
fn fit_at(sizes: &[(f32, f32)], gap: f32, target: f32) -> (Vec<(f32, f32)>, f32, f32) {
    let mut order: Vec<usize> = (0..sizes.len()).collect();
    order.sort_by(|&a, &b| sizes[b].1.partial_cmp(&sizes[a].1).unwrap().then(a.cmp(&b)));

    let mut sky: Vec<(f32, f32)> = vec![(0.0, 0.0)]; // (x, ground height) until the next entry
    let mut spots = vec![(0.0f32, 0.0f32); sizes.len()];
    let (mut width, mut height) = (0.0f32, 0.0f32);
    for &i in &order {
        let (w, h) = sizes[i];
        let mut best: Option<(f32, f32)> = None; // (y, x)
        for k in 0..sky.len() {
            let x = sky[k].0;
            if x > 0.0 && x + w > target + 0.5 {
                continue;
            }
            let y = ground(&sky, x, x + w);
            if best.is_none_or(|(by, bx)| y < by - 0.5 || (y < by + 0.5 && x < bx)) {
                best = Some((y, x));
            }
        }
        let (y, x) = best.unwrap_or((ground(&sky, 0.0, w), 0.0));
        spots[i] = (x, y);
        raise(&mut sky, x, x + w + gap, y + h + gap);
        width = width.max(x + w);
        height = height.max(y + h);
    }
    (spots, width, height)
}

fn ground(sky: &[(f32, f32)], x0: f32, x1: f32) -> f32 {
    let mut top = 0.0f32;
    for (k, &(x, y)) in sky.iter().enumerate() {
        let end = sky.get(k + 1).map(|s| s.0).unwrap_or(f32::INFINITY);
        if end > x0 + 0.001 && x < x1 - 0.001 {
            top = top.max(y);
        }
    }
    top
}

fn raise(sky: &mut Vec<(f32, f32)>, x0: f32, x1: f32, y: f32) {
    let after = ground(sky, x1, x1 + 0.001);
    sky.retain(|&(x, _)| x < x0 - 0.001 || x > x1 + 0.001);
    sky.push((x0, y));
    sky.push((x1, after));
    sky.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap());
    let mut clean: Vec<(f32, f32)> = Vec::with_capacity(sky.len());
    for &(x, h) in sky.iter() {
        if let Some(last) = clean.last_mut() {
            if (last.0 - x).abs() < 0.001 {
                last.1 = h; // the entry just written wins over the one it replaces
                continue;
            }
            if (last.1 - h).abs() < 0.001 {
                continue; // same ground as the segment before: one segment, not two
            }
        }
        clean.push((x, h));
    }
    *sky = clean;
}
