//! Layered graph drawing - the algorithm behind dot and dagre, and so behind Mermaid itself.
//!
//! Everything here works top-down: rows of nodes (ranks) going down the page, order inside a row going right. The
//! caller turns the result around for `LR`, `BT` and `RL`. Nothing here knows about subgraphs: a subgraph is laid out
//! on its own and handed to its parent as one box, which is how Mermaid keeps boxes from swallowing each other.
//!
//! The stages are the classic four: break cycles, put every node on a rank, order the rows so few edges cross, then
//! give every node an x. Edges that skip ranks get invisible nodes on the way, so a long edge is still a chain of
//! one-rank steps and its bends are real positions instead of a guess.

use super::Cfg;

pub struct SNode {
    pub w: f32,
    pub h: f32,
}

pub struct SEdge {
    pub from: usize,
    pub to: usize,
    pub label_w: f32,
    pub label_h: f32,
}

pub struct SOut {
    /// Centre of every node the caller asked for.
    pub pos: Vec<(f32, f32)>,
    /// One polyline per edge, from the source centre to the target centre. The caller trims the ends to the borders.
    pub paths: Vec<Vec<(f32, f32)>>,
    /// Where an edge label goes, if it has one.
    pub label_at: Vec<Option<(f32, f32)>>,
    pub width: f32,
    pub height: f32,
}

struct Graph {
    w: Vec<f32>,
    h: Vec<f32>,
    real: usize,             // nodes [0, real) are the caller's; the rest are ours
    rank: Vec<i32>,
    succ: Vec<Vec<usize>>,   // after ranking these are one-rank steps only
    pred: Vec<Vec<usize>>,
    x: Vec<f32>,
    y: Vec<f32>,
}

impl Graph {
    fn add_node(&mut self, w: f32, h: f32, rank: i32) -> usize {
        self.w.push(w);
        self.h.push(h);
        self.rank.push(rank);
        self.succ.push(Vec::new());
        self.pred.push(Vec::new());
        self.x.push(0.0);
        self.y.push(0.0);
        self.w.len() - 1
    }
    fn link(&mut self, u: usize, v: usize) {
        self.succ[u].push(v);
        self.pred[v].push(u);
    }
}

pub fn run(nodes: &[SNode], edges: &[SEdge], cfg: &Cfg) -> SOut {
    let n = nodes.len();
    let mut g = Graph {
        w: nodes.iter().map(|s| s.w).collect(),
        h: nodes.iter().map(|s| s.h).collect(),
        real: n,
        rank: vec![0; n],
        succ: vec![Vec::new(); n],
        pred: vec![Vec::new(); n],
        x: vec![0.0; n],
        y: vec![0.0; n],
    };

    // ---- 1. edges we cannot rank: a loop on one node, and cycles.
    let mut loops: Vec<usize> = Vec::new();
    let mut es: Vec<(usize, usize, usize, bool)> = Vec::new(); // from, to, original index, reversed
    for (i, e) in edges.iter().enumerate() {
        if e.from == e.to || e.from >= n || e.to >= n {
            loops.push(i);
        } else {
            es.push((e.from, e.to, i, false));
        }
    }
    let back = back_edges(n, &es);
    for (k, rev) in back.iter().enumerate() {
        if *rev {
            let (u, v, i, _) = es[k];
            es[k] = (v, u, i, true);
        }
    }

    // ---- 2. a rank for every node.
    let minlen: Vec<i32> = es
        .iter()
        .map(|&(_, _, i, _)| if edges[i].label_h > 0.0 { 2 } else { 1 })
        .collect();
    rank_nodes(&mut g, n, &es, &minlen);

    // ---- 3. invisible nodes so that every edge is a chain of one-rank steps.
    let mut chains: Vec<Vec<usize>> = Vec::with_capacity(es.len());
    let mut label_node: Vec<Option<usize>> = vec![None; edges.len()];
    for &(u, v, i, _) in es.iter() {
        let (r0, r1) = (g.rank[u], g.rank[v]);
        let mut chain = vec![u];
        for r in (r0 + 1)..r1 {
            // The label rides on the first spare rank, which is where a two-rank edge puts it: in the middle.
            let labelled = edges[i].label_h > 0.0 && label_node[i].is_none() && r == r0 + 1;
            let (w, h) = if labelled {
                (edges[i].label_w + cfg.label_pad, edges[i].label_h)
            } else {
                (1.0, 0.0)
            };
            let d = g.add_node(w, h, r);
            if labelled {
                label_node[i] = Some(d);
            }
            chain.push(d);
        }
        chain.push(v);
        for w in chain.windows(2) {
            g.link(w[0], w[1]);
        }
        chains.push(chain);
    }

    // ---- 4. order inside every rank, so that few edges cross.
    let layers = order_ranks(&g);

    // ---- 5. coordinates.
    let (width, height) = place(&mut g, &layers, cfg);

    // ---- 6. hand back what the caller asked for.
    let mut paths: Vec<Vec<(f32, f32)>> = vec![Vec::new(); edges.len()];
    let mut label_at: Vec<Option<(f32, f32)>> = vec![None; edges.len()];
    for (k, &(_, _, i, reversed)) in es.iter().enumerate() {
        let mut pts: Vec<(f32, f32)> = chains[k].iter().map(|&v| (g.x[v], g.y[v])).collect();
        if reversed {
            pts.reverse();
        }
        paths[i] = pts;
        label_at[i] = label_node[i].map(|d| (g.x[d], g.y[d]));
    }
    for &i in &loops {
        paths[i] = Vec::new();
    }
    let pos: Vec<(f32, f32)> = (0..n).map(|v| (g.x[v], g.y[v])).collect();
    SOut { pos, paths, label_at, width, height }
}

/// Depth-first search; an edge back into the stack would make ranking impossible, so it is turned around.
fn back_edges(n: usize, es: &[(usize, usize, usize, bool)]) -> Vec<bool> {
    let mut out = vec![false; es.len()];
    let mut adj: Vec<Vec<usize>> = vec![Vec::new(); n];
    for (k, &(u, _, _, _)) in es.iter().enumerate() {
        adj[u].push(k);
    }
    let mut state = vec![0u8; n]; // 0 = untouched, 1 = on the stack, 2 = done
    let mut stack: Vec<(usize, usize)> = Vec::new();
    for start in 0..n {
        if state[start] != 0 {
            continue;
        }
        state[start] = 1;
        stack.push((start, 0));
        while let Some((v, i)) = stack.pop() {
            if i < adj[v].len() {
                stack.push((v, i + 1));
                let k = adj[v][i];
                let to = es[k].1;
                match state[to] {
                    0 => {
                        state[to] = 1;
                        stack.push((to, 0));
                    }
                    1 => out[k] = true, // back into the stack: a cycle
                    _ => {}
                }
            } else {
                state[v] = 2;
            }
        }
    }
    out
}

/// Longest path from the sources, then every source is pulled down next to what it points at - a node hanging one
/// rank above its target reads better than the same node parked at the top of the page.
fn rank_nodes(g: &mut Graph, n: usize, es: &[(usize, usize, usize, bool)], minlen: &[i32]) {
    let mut indeg = vec![0usize; n];
    let mut out: Vec<Vec<usize>> = vec![Vec::new(); n];
    for (k, &(u, v, _, _)) in es.iter().enumerate() {
        out[u].push(k);
        indeg[v] += 1;
    }
    let mut queue: Vec<usize> = (0..n).filter(|&v| indeg[v] == 0).collect();
    let mut topo: Vec<usize> = Vec::with_capacity(n);
    let mut head = 0;
    while head < queue.len() {
        let v = queue[head];
        head += 1;
        topo.push(v);
        for &k in &out[v] {
            let to = es[k].1;
            indeg[to] -= 1;
            if indeg[to] == 0 {
                queue.push(to);
            }
        }
    }
    if topo.len() < n {
        // A tangle the back-edge pass could not undo. Rank what is left in node order rather than dropping it.
        let mut seen = vec![false; n];
        for &v in &topo {
            seen[v] = true;
        }
        for v in 0..n {
            if !seen[v] {
                topo.push(v);
            }
        }
    }
    for &v in &topo {
        for &k in &out[v] {
            let (_, to, _, _) = es[k];
            g.rank[to] = g.rank[to].max(g.rank[v] + minlen[k]);
        }
    }
    let has_in = {
        let mut f = vec![false; n];
        for &(_, v, _, _) in es {
            f[v] = true;
        }
        f
    };
    for &v in topo.iter().rev() {
        if has_in[v] || out[v].is_empty() {
            continue;
        }
        let pulled = out[v].iter().map(|&k| g.rank[es[k].1] - minlen[k]).min().unwrap_or(g.rank[v]);
        g.rank[v] = pulled.max(g.rank[v]);
    }
    let lowest = g.rank[..n].iter().copied().min().unwrap_or(0);
    for v in 0..n {
        g.rank[v] -= lowest;
    }
}

fn order_ranks(g: &Graph) -> Vec<Vec<usize>> {
    let top = g.rank.iter().copied().max().unwrap_or(0);
    let mut layers: Vec<Vec<usize>> = vec![Vec::new(); (top + 1) as usize];
    for v in 0..g.w.len() {
        layers[g.rank[v] as usize].push(v);
    }
    if layers.len() < 2 {
        return layers;
    }
    // A big diagram is mostly invisible nodes carrying long edges, and every stage here costs more per node than the
    // last. Rather than let one generated graph freeze a worker thread, the sweeps get fewer as the graph grows.
    let n = g.w.len();
    let (sweeps, passes, weigh) = match n {
        0..=1_200 => (8, 4, true),
        1_201..=6_000 => (4, 2, false),
        _ => (2, 1, false),
    };
    let mut pos: Vec<u32> = vec![0; n];
    for layer in &layers {
        for (i, &v) in layer.iter().enumerate() {
            pos[v] = i as u32;
        }
    }
    let mut best = layers.clone();
    let mut best_cross = if weigh { count_all(g, &layers, &pos) } else { 0 };
    let mut keyed: Vec<(f32, usize, usize)> = Vec::new();
    for iter in 0..sweeps {
        let down = iter % 2 == 0;
        let range: Vec<usize> = if down { (1..layers.len()).collect() } else { (0..layers.len() - 1).rev().collect() };
        for r in range {
            keyed.clear();
            for (i, &v) in layers[r].iter().enumerate() {
                let ns: &Vec<usize> = if down { &g.pred[v] } else { &g.succ[v] };
                let key = median(ns.iter().map(|&u| pos[u] as f32), i as f32);
                keyed.push((key, i, v));
            }
            keyed.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap().then(a.1.cmp(&b.1)));
            for (i, k) in keyed.iter().enumerate() {
                layers[r][i] = k.2;
                pos[k.2] = i as u32;
            }
        }
        transpose(g, &mut layers, &mut pos, passes);
        if !weigh {
            continue;
        }
        let c = count_all(g, &layers, &pos);
        if c < best_cross {
            best_cross = c;
            best = layers.clone();
        }
        if best_cross == 0 {
            break;
        }
    }
    if weigh { best } else { layers }
}

/// The middle of what a node is joined to in the next row, or where it already stands if it is joined to nothing.
fn median(values: impl Iterator<Item = f32>, fallback: f32) -> f32 {
    let mut vs: Vec<f32> = values.collect();
    if vs.is_empty() {
        return fallback;
    }
    vs.sort_by(|a, b| a.partial_cmp(b).unwrap());
    if vs.len() % 2 == 1 { vs[vs.len() / 2] } else { (vs[vs.len() / 2 - 1] + vs[vs.len() / 2]) / 2.0 }
}

/// Swapping neighbours while it helps: the cheap finishing move of the ordering stage.
fn transpose(g: &Graph, layers: &mut [Vec<usize>], pos: &mut [u32], passes: usize) {
    for _ in 0..passes {
        let mut improved = false;
        for r in 0..layers.len() {
            for i in 0..layers[r].len().saturating_sub(1) {
                let before = pair_cross(g, layers, pos, r, i);
                layers[r].swap(i, i + 1);
                let after = pair_cross(g, layers, pos, r, i);
                if after < before {
                    pos[layers[r][i]] = i as u32;
                    pos[layers[r][i + 1]] = (i + 1) as u32;
                    improved = true;
                } else {
                    layers[r].swap(i, i + 1);
                }
            }
        }
        if !improved {
            break;
        }
    }
}

/// Crossings between the edges of two nodes standing side by side - all the transpose pass needs to know.
fn pair_cross(g: &Graph, layers: &[Vec<usize>], pos: &[u32], r: usize, i: usize) -> usize {
    let (v, w) = (layers[r][i], layers[r][i + 1]);
    let mut total = 0;
    if r > 0 {
        total += cross_of_pair(&g.pred[v], &g.pred[w], pos);
    }
    if r + 1 < layers.len() {
        total += cross_of_pair(&g.succ[v], &g.succ[w], pos);
    }
    total
}

/// `v` stands to the left of `w`, so every neighbour of `v` that sits right of a neighbour of `w` is one crossing.
fn cross_of_pair(left: &[usize], right: &[usize], pos: &[u32]) -> usize {
    let mut count = 0;
    for a in left {
        for b in right {
            if pos[*a] > pos[*b] {
                count += 1;
            }
        }
    }
    count
}

fn count_all(g: &Graph, layers: &[Vec<usize>], pos: &[u32]) -> usize {
    let mut total = 0;
    let mut ends: Vec<(u32, u32)> = Vec::new();
    for r in 1..layers.len() {
        ends.clear();
        for &u in &layers[r - 1] {
            for &v in &g.succ[u] {
                if g.rank[v] == g.rank[u] + 1 {
                    ends.push((pos[u], pos[v]));
                }
            }
        }
        for a in 0..ends.len() {
            for b in (a + 1)..ends.len() {
                if (ends[a].0 < ends[b].0 && ends[a].1 > ends[b].1)
                    || (ends[a].0 > ends[b].0 && ends[a].1 < ends[b].1)
                {
                    total += 1;
                }
            }
        }
    }
    total
}

/// Rows go down the page with a fixed gap; inside a row every node is pulled towards the middle of what it is joined
/// to, as far as its neighbours allow. Invisible nodes pull hardest, which is what keeps a long edge straight.
fn place(g: &mut Graph, layers: &[Vec<usize>], cfg: &Cfg) -> (f32, f32) {
    let mut y = 0.0f32;
    for layer in layers {
        let tall = layer.iter().map(|&v| g.h[v]).fold(0.0f32, f32::max);
        for &v in layer {
            g.y[v] = y + tall / 2.0;
        }
        y += tall + cfg.ranksep;
    }
    let height = (y - cfg.ranksep).max(0.0);

    for layer in layers {
        let mut x = 0.0f32;
        for &v in layer {
            g.x[v] = x + g.w[v] / 2.0;
            x += g.w[v] + cfg.nodesep;
        }
    }

    let sep = |g: &Graph, a: usize, b: usize| -> f32 {
        // Invisible nodes are thin; a real node still needs room to breathe beside them.
        if a >= g.real && b >= g.real { cfg.nodesep * 0.5 } else { cfg.nodesep }
    };

    // As in the ordering stage, the number of passes comes down as the graph grows.
    let rounds = match g.w.len() {
        0..=1_200 => 10,
        1_201..=6_000 => 5,
        _ => 3,
    };
    let mut desired: Vec<Option<f32>> = vec![None; g.w.len()];
    let mut prio: Vec<i32> = vec![0; g.w.len()];
    for iter in 0..rounds {
        if layers.len() < 2 {
            break;
        }
        let down = iter % 2 == 0;
        let order: Vec<usize> = if down { (1..layers.len()).collect() } else { (0..layers.len() - 1).rev().collect() };
        for r in order {
            for &v in &layers[r] {
                let ns: &Vec<usize> = if down { &g.pred[v] } else { &g.succ[v] };
                desired[v] = if ns.is_empty() { None } else { Some(median(ns.iter().map(|&u| g.x[u]), g.x[v])) };
                prio[v] = if v >= g.real { 1_000 } else { (g.pred[v].len() + g.succ[v].len()) as i32 };
            }
            shift_layer(g, &layers[r], &desired, &prio, &sep);
        }
    }

    let mut min_x = f32::INFINITY;
    let mut max_x = f32::NEG_INFINITY;
    for v in 0..g.w.len() {
        min_x = min_x.min(g.x[v] - g.w[v] / 2.0);
        max_x = max_x.max(g.x[v] + g.w[v] / 2.0);
    }
    if min_x.is_finite() {
        for v in 0..g.w.len() {
            g.x[v] -= min_x;
        }
    }
    ((max_x - min_x).max(0.0), height)
}

/// Moves nodes of one row towards where they want to be, most important first; a node can push lighter neighbours
/// aside but never passes one that matters more.
fn shift_layer(
    g: &mut Graph,
    layer: &[usize],
    desired: &[Option<f32>],
    prio: &[i32],
    sep: &dyn Fn(&Graph, usize, usize) -> f32,
) {
    let mut by_prio: Vec<usize> = (0..layer.len()).collect();
    by_prio.sort_by(|&a, &b| prio[layer[b]].cmp(&prio[layer[a]]));
    for k in by_prio {
        let v = layer[k];
        let Some(want) = desired[v] else { continue };
        let delta = want - g.x[v];
        if delta.abs() < 0.5 {
            continue;
        }
        if delta > 0.0 {
            let mut room = f32::INFINITY;
            let mut free = 0.0f32;
            for j in (k + 1)..layer.len() {
                let a = layer[j - 1];
                let b = layer[j];
                free += (g.x[b] - g.w[b] / 2.0) - (g.x[a] + g.w[a] / 2.0) - sep(g, a, b);
                if prio[b] >= prio[v] {
                    room = free.max(0.0);
                    break;
                }
            }
            let step = delta.min(room);
            if step <= 0.0 {
                continue;
            }
            g.x[v] += step;
            for j in (k + 1)..layer.len() {
                let a = layer[j - 1];
                let b = layer[j];
                let least = g.x[a] + g.w[a] / 2.0 + sep(g, a, b) + g.w[b] / 2.0;
                if g.x[b] < least {
                    g.x[b] = least;
                } else {
                    break;
                }
            }
        } else {
            let mut room = f32::INFINITY;
            let mut free = 0.0f32;
            for j in (0..k).rev() {
                let a = layer[j];
                let b = layer[j + 1];
                free += (g.x[b] - g.w[b] / 2.0) - (g.x[a] + g.w[a] / 2.0) - sep(g, a, b);
                if prio[a] >= prio[v] {
                    room = free.max(0.0);
                    break;
                }
            }
            let step = (-delta).min(room);
            if step <= 0.0 {
                continue;
            }
            g.x[v] -= step;
            for j in (0..k).rev() {
                let a = layer[j];
                let b = layer[j + 1];
                let most = g.x[b] - g.w[b] / 2.0 - sep(g, a, b) - g.w[a] / 2.0;
                if g.x[a] > most {
                    g.x[a] = most;
                } else {
                    break;
                }
            }
        }
    }
}
