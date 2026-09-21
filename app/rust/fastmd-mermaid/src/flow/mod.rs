//! Flowcharts (`graph` / `flowchart`), drawn the way Mermaid draws them.
//!
//! The library we use for the other twenty-odd diagram kinds places flowchart nodes by ranking them once and then
//! shoving things apart until nothing overlaps. On a real architecture diagram - nested subgraphs, edges that point
//! at a subgraph rather than a node - that leaves boxes lying across each other and a picture several times wider
//! than it needs to be. So flowcharts are laid out here instead, and everything else still goes to the library.
//!
//! The shape of it follows Mermaid: every subgraph is laid out on its own, then handed to its parent as a single box
//! (`layout_scope` calls itself), and edges that cross a box boundary are drawn between the boxes and then carried on
//! to the node that really owns the end. That is what keeps a subgraph's contents inside its own frame.

mod sugiyama;
mod svg;
mod text;

use mermaid_rs_renderer::ir::{Direction, Graph as IrGraph, NodeShape, NodeStyle};
use mermaid_rs_renderer::Theme;
use std::collections::{HashMap, HashSet};

pub struct Cfg {
    pub font_size: f32,
    pub line_height: f32,
    pub node_hpad: f32,
    pub node_vpad: f32,
    pub nodesep: f32,
    pub ranksep: f32,
    pub cluster_pad: f32,
    pub label_pad: f32,
    pub max_label: f32,
    pub min_node_w: f32,
    pub min_node_h: f32,
    pub margin: f32,
}

impl Default for Cfg {
    fn default() -> Self {
        Cfg {
            font_size: 16.0,
            line_height: 1.5,
            node_hpad: 16.0,
            node_vpad: 9.0,
            nodesep: 40.0,
            ranksep: 50.0,
            cluster_pad: 14.0,
            label_pad: 10.0,
            max_label: 220.0,
            min_node_w: 44.0,
            min_node_h: 38.0,
            margin: 8.0,
        }
    }
}

enum Kind {
    Node(NodeShape),
    Cluster { children: Vec<usize>, dir: Option<Direction> },
}

struct Item {
    label: text::Label,
    kind: Kind,
    parent: Option<usize>,
    depth: usize,
    written: usize,     // where it stood in the source, so a row keeps the order the author typed
    w: f32,
    h: f32,
    x: f32, // centre: local to the parent's content box while laying out, absolute afterwards
    y: f32,
    title_h: f32,       // clusters: the strip at the top that holds the name
    content_dx: f32,    // clusters: where the children's origin sits inside the box
    content_dy: f32,
    style: NodeStyle,
}

struct Edge {
    ir: usize,
    from: usize,
    to: usize,
    scope: Option<usize>,
    a: usize, // the child of `scope` the edge starts at (`from` itself, or the box that holds it)
    b: usize,
    label: text::Label,
    points: Vec<(f32, f32)>,
    label_at: Option<(f32, f32)>,
}

struct Model<'a> {
    items: Vec<Item>,
    roots: Vec<usize>,
    edges: Vec<Edge>,
    graph: &'a IrGraph,
    cfg: Cfg,
}

pub fn render(graph: &IrGraph, theme: &Theme) -> Option<String> {
    let cfg = Cfg { font_size: theme.font_size.max(8.0), ..Cfg::default() };
    let font = text::Font::load();
    let mut model = build(graph, &font, cfg)?;
    model.layout_scope(None);
    model.place(None, model.cfg.margin, model.cfg.margin);
    model.finish_edges();
    let (w, h) = model.bounds();
    Some(svg::write(&model, theme, w + model.cfg.margin, h + model.cfg.margin))
}

// ---------------------------------------------------------------------------------------------------- the model

fn build<'a>(graph: &'a IrGraph, font: &text::Font, cfg: Cfg) -> Option<Model<'a>> {
    if graph.nodes.is_empty() && graph.subgraphs.is_empty() {
        return None;
    }
    let mut items: Vec<Item> = Vec::new();
    let mut by_id: HashMap<String, usize> = HashMap::new();

    // Subgraphs first: a subgraph lists every node under it, including the ones its own children claim, so the one
    // that holds a subgraph is the smallest other subgraph whose set covers it.
    let subs = &graph.subgraphs;
    let sets: Vec<HashSet<&str>> = subs.iter().map(|s| s.nodes.iter().map(|n| n.as_str()).collect()).collect();
    let mut cluster_of_sub: Vec<usize> = Vec::with_capacity(subs.len());
    for (i, sub) in subs.iter().enumerate() {
        let name = sub.id.clone().unwrap_or_else(|| format!("__sub{i}"));
        let label = text::measure(font, &sub.label, cfg.font_size, cfg.line_height, 1.0e6);
        let title_h = if label.is_empty() { cfg.cluster_pad } else { label.height + 12.0 };
        let style = resolve_style(graph, &graph.subgraph_classes, &graph.subgraph_styles, &name);
        items.push(Item {
            label,
            kind: Kind::Cluster { children: Vec::new(), dir: sub.direction },
            parent: None,
            depth: 0,
            written: sub.nodes.iter().filter_map(|n| graph.node_order.get(n)).copied().min().unwrap_or(usize::MAX),
            w: 0.0,
            h: 0.0,
            x: 0.0,
            y: 0.0,
            title_h,
            content_dx: 0.0,
            content_dy: 0.0,
            style,
        });
        cluster_of_sub.push(items.len() - 1);
        by_id.entry(name).or_insert(items.len() - 1);
    }
    for (i, _) in subs.iter().enumerate() {
        let mut best: Option<usize> = None;
        for j in 0..subs.len() {
            if i == j || !sets[i].iter().all(|n| sets[j].contains(n)) {
                continue;
            }
            // Equal sets: the one declared first is the outer one.
            if sets[j].len() == sets[i].len() && j > i {
                continue;
            }
            if best.is_none_or(|b| sets[j].len() < sets[b].len()) {
                best = Some(j);
            }
        }
        items[cluster_of_sub[i]].parent = best.map(|b| cluster_of_sub[b]);
    }

    // Then the nodes, in the order they were written: `graph.nodes` is sorted by name, and a diagram that comes out
    // alphabetised is not the diagram the author typed.
    let mut written: Vec<(&String, &mermaid_rs_renderer::ir::Node)> = graph.nodes.iter().collect();
    written.sort_by_key(|(id, _)| graph.node_order.get(*id).copied().unwrap_or(usize::MAX));
    for (id, node) in written {
        if by_id.contains_key(id) {
            continue;
        }
        let owner = subs
            .iter()
            .enumerate()
            .filter(|(i, _)| sets[*i].contains(id.as_str()))
            .min_by_key(|(i, _)| sets[*i].len())
            .map(|(i, _)| cluster_of_sub[i]);
        let label = text::measure(font, &node.label, cfg.font_size, cfg.line_height, cfg.max_label);
        let (w, h) = shape_size(node.shape, &label, &cfg);
        let style = resolve_style(graph, &graph.node_classes, &graph.node_styles, id);
        items.push(Item {
            label,
            kind: Kind::Node(node.shape),
            parent: owner,
            depth: 0,
            written: graph.node_order.get(id).copied().unwrap_or(usize::MAX),
            w,
            h,
            x: 0.0,
            y: 0.0,
            title_h: 0.0,
            content_dx: 0.0,
            content_dy: 0.0,
            style,
        });
        by_id.insert(id.clone(), items.len() - 1);
    }

    // Children, in the order they were written.
    let mut roots: Vec<usize> = Vec::new();
    let parents: Vec<Option<usize>> = items.iter().map(|it| it.parent).collect();
    for (i, parent) in parents.iter().enumerate() {
        match parent {
            Some(p) => {
                if let Kind::Cluster { children, .. } = &mut items[*p].kind {
                    children.push(i);
                }
            }
            None => roots.push(i),
        }
    }
    let written: Vec<usize> = items.iter().map(|it| it.written).collect();
    roots.sort_by_key(|&i| written[i]);
    for i in 0..items.len() {
        if let Kind::Cluster { children, .. } = &mut items[i].kind {
            children.sort_by_key(|&c| written[c]);
        }
    }
    for i in 0..items.len() {
        let mut depth = 0;
        let mut at = items[i].parent;
        while let Some(p) = at {
            depth += 1;
            at = items[p].parent;
            if depth > 64 {
                break; // a subgraph cycle cannot happen, but never hang on one
            }
        }
        items[i].depth = depth;
    }

    // Edges. Each one is laid out in the innermost box that holds both of its ends.
    let mut edges: Vec<Edge> = Vec::new();
    for (i, e) in graph.edges.iter().enumerate() {
        let (Some(&from), Some(&to)) = (by_id.get(&e.from), by_id.get(&e.to)) else { continue };
        let label = match &e.label {
            Some(t) if !t.trim().is_empty() => text::measure(font, t, cfg.font_size, cfg.line_height, cfg.max_label),
            _ => text::Label::default(),
        };
        let Some((scope, a, b)) = common_scope(&items, from, to) else { continue };
        edges.push(Edge { ir: i, from, to, scope, a, b, label, points: Vec::new(), label_at: None });
    }

    Some(Model { items, roots, edges, graph, cfg })
}

/// The innermost box that holds both ends, and the two children of it the edge runs between.
fn common_scope(items: &[Item], from: usize, to: usize) -> Option<(Option<usize>, usize, usize)> {
    let chain = |mut v: usize| -> Vec<usize> {
        let mut out = vec![v];
        while let Some(p) = items[v].parent {
            out.push(p);
            v = p;
            if out.len() > 64 {
                break;
            }
        }
        out.reverse(); // outermost first
        out
    };
    let (ca, cb) = (chain(from), chain(to));
    let mut shared = 0;
    while shared < ca.len() && shared < cb.len() && ca[shared] == cb[shared] {
        shared += 1;
    }
    if from == to {
        // A loop on one node: it belongs to whatever box the node is in, and is drawn beside the node.
        return Some((items[from].parent, from, to));
    }
    if shared >= ca.len() || shared >= cb.len() {
        return None; // one end is the box that holds the other: nothing sensible to draw
    }
    let scope = if shared == 0 { None } else { Some(ca[shared - 1]) };
    Some((scope, ca[shared], cb[shared]))
}

fn resolve_style(
    graph: &IrGraph,
    classes: &HashMap<String, Vec<String>>,
    direct: &HashMap<String, NodeStyle>,
    id: &str,
) -> NodeStyle {
    let mut style = NodeStyle::default();
    if let Some(d) = graph.class_defs.get("default") {
        merge_style(&mut style, d);
    }
    if let Some(list) = classes.get(id) {
        for name in list {
            if let Some(d) = graph.class_defs.get(name) {
                merge_style(&mut style, d);
            }
        }
    }
    if let Some(d) = direct.get(id) {
        merge_style(&mut style, d);
    }
    style
}

fn merge_style(into: &mut NodeStyle, from: &NodeStyle) {
    if from.fill.is_some() {
        into.fill = from.fill.clone();
    }
    if from.stroke.is_some() {
        into.stroke = from.stroke.clone();
    }
    if from.text_color.is_some() {
        into.text_color = from.text_color.clone();
    }
    if from.stroke_width.is_some() {
        into.stroke_width = from.stroke_width;
    }
    if from.stroke_dasharray.is_some() {
        into.stroke_dasharray = from.stroke_dasharray.clone();
    }
    if from.line_color.is_some() {
        into.line_color = from.line_color.clone();
    }
}

/// How big a box has to be to hold its text, which depends on what the box looks like.
fn shape_size(shape: NodeShape, label: &text::Label, cfg: &Cfg) -> (f32, f32) {
    let (tw, th) = (label.width, label.height);
    let (mut w, mut h) = (tw + 2.0 * cfg.node_hpad, th + 2.0 * cfg.node_vpad);
    match shape {
        NodeShape::Diamond => {
            w = tw + th + 2.0 * cfg.node_hpad;
            h = th * 1.6 + 2.0 * cfg.node_vpad;
        }
        NodeShape::Circle | NodeShape::DoubleCircle => {
            let d = (tw.max(th) + 2.0 * cfg.node_vpad) * 1.25;
            w = d;
            h = d;
        }
        NodeShape::Hexagon | NodeShape::Parallelogram | NodeShape::ParallelogramAlt => {
            w = tw + 2.0 * cfg.node_hpad + th * 0.8;
        }
        NodeShape::Trapezoid | NodeShape::TrapezoidAlt => {
            w = tw + 2.0 * cfg.node_hpad + th;
        }
        NodeShape::Cylinder => {
            h = th + 2.0 * cfg.node_vpad + 14.0;
        }
        NodeShape::Subroutine => {
            w = tw + 2.0 * cfg.node_hpad + 16.0;
        }
        NodeShape::Asymmetric => {
            w = tw + 2.0 * cfg.node_hpad + 12.0;
        }
        NodeShape::ForkJoin => {
            w = tw.max(70.0);
            h = 8.0;
        }
        _ => {}
    }
    (w.max(cfg.min_node_w), h.max(if matches!(shape, NodeShape::ForkJoin) { 8.0 } else { cfg.min_node_h }))
}

// ---------------------------------------------------------------------------------------------------- placing

impl Model<'_> {
    fn children_of(&self, scope: Option<usize>) -> Vec<usize> {
        match scope {
            None => self.roots.clone(),
            Some(c) => match &self.items[c].kind {
                Kind::Cluster { children, .. } => children.clone(),
                Kind::Node(_) => Vec::new(),
            },
        }
    }

    fn direction_of(&self, scope: Option<usize>) -> Direction {
        let mut at = scope;
        while let Some(c) = at {
            if let Kind::Cluster { dir: Some(d), .. } = &self.items[c].kind {
                return *d;
            }
            at = self.items[c].parent;
        }
        self.graph.direction
    }

    /// Lays out one box: its children first (a child subgraph is laid out whole and then treated as one node), then
    /// the children among themselves. Returns the size of the content.
    fn layout_scope(&mut self, scope: Option<usize>) -> (f32, f32) {
        let children = self.children_of(scope);
        for &c in &children {
            if matches!(self.items[c].kind, Kind::Cluster { .. }) {
                let (cw, ch) = self.layout_scope(Some(c));
                let pad = self.cfg.cluster_pad;
                let title = &self.items[c].label;
                let title_w = if title.is_empty() { 0.0 } else { title.width + 2.0 * pad + 16.0 };
                let w = (cw + 2.0 * pad).max(title_w).max(self.cfg.min_node_w);
                let h = ch + 2.0 * pad + self.items[c].title_h;
                self.items[c].w = w;
                self.items[c].h = h;
                self.items[c].content_dx = (w - cw) / 2.0;
                self.items[c].content_dy = pad + self.items[c].title_h;
            }
        }
        if children.is_empty() {
            return (self.cfg.min_node_w, self.cfg.min_node_h);
        }

        let index: HashMap<usize, usize> = children.iter().enumerate().map(|(i, &c)| (c, i)).collect();
        let dir = self.direction_of(scope);
        let sideways = matches!(dir, Direction::LeftRight | Direction::RightLeft);
        let nodes: Vec<sugiyama::SNode> = children
            .iter()
            .map(|&c| {
                let (w, h) = (self.items[c].w, self.items[c].h);
                if sideways { sugiyama::SNode { w: h, h: w } } else { sugiyama::SNode { w, h } }
            })
            .collect();
        let mut picked: Vec<usize> = Vec::new();
        let mut es: Vec<sugiyama::SEdge> = Vec::new();
        for (i, e) in self.edges.iter().enumerate() {
            if e.scope != scope {
                continue;
            }
            let (Some(&a), Some(&b)) = (index.get(&e.a), index.get(&e.b)) else { continue };
            let (lw, lh) = if e.label.is_empty() {
                (0.0, 0.0)
            } else if sideways {
                (e.label.height, e.label.width + self.cfg.label_pad)
            } else {
                (e.label.width, e.label.height)
            };
            es.push(sugiyama::SEdge { from: a, to: b, label_w: lw, label_h: lh });
            picked.push(i);
        }

        let out = sugiyama::run(&nodes, &es, &self.cfg);
        let (mut width, mut height) = (out.width, out.height);
        if sideways {
            std::mem::swap(&mut width, &mut height);
        }
        let flip_x = matches!(dir, Direction::RightLeft);
        let flip_y = matches!(dir, Direction::BottomTop);
        let map = move |p: (f32, f32)| -> (f32, f32) {
            let (mut x, mut y) = if sideways { (p.1, p.0) } else { p };
            if flip_x {
                x = width - x;
            }
            if flip_y {
                y = height - y;
            }
            (x, y)
        };

        for (i, &c) in children.iter().enumerate() {
            let (x, y) = map(out.pos[i]);
            self.items[c].x = x;
            self.items[c].y = y;
        }
        for (k, &ei) in picked.iter().enumerate() {
            self.edges[ei].points = out.paths[k].iter().map(|&p| map(p)).collect();
            self.edges[ei].label_at = out.label_at[k].map(map);
        }
        (width, height)
    }

    /// Turns the local positions of one box into positions on the page.
    fn place(&mut self, scope: Option<usize>, ox: f32, oy: f32) {
        for &c in &self.children_of(scope) {
            self.items[c].x += ox;
            self.items[c].y += oy;
            if matches!(self.items[c].kind, Kind::Cluster { .. }) {
                let it = &self.items[c];
                let (cx, cy) = (it.x - it.w / 2.0 + it.content_dx, it.y - it.h / 2.0 + it.content_dy);
                self.place(Some(c), cx, cy);
            }
        }
        for e in &mut self.edges {
            if e.scope != scope {
                continue;
            }
            for p in &mut e.points {
                p.0 += ox;
                p.1 += oy;
            }
            if let Some(l) = &mut e.label_at {
                l.0 += ox;
                l.1 += oy;
            }
        }
    }

    /// An edge laid out between two boxes has to reach the node that really owns each end, and stop at its border.
    fn finish_edges(&mut self) {
        for i in 0..self.edges.len() {
            let mut pts = std::mem::take(&mut self.edges[i].points);
            let e = &self.edges[i];
            if pts.len() < 2 {
                self.edges[i].points = self.self_loop(e.from);
                continue;
            }
            let (from, to, a, b) = (e.from, e.to, e.a, e.b);
            if a != from {
                pts[0] = self.leave_box(a, pts[0], pts[1], from);
                pts.insert(0, (self.items[from].x, self.items[from].y));
            }
            if b != to {
                let last = pts.len() - 1;
                pts[last] = self.leave_box(b, pts[last], pts[last - 1], to);
                pts.push((self.items[to].x, self.items[to].y));
            }
            let start = self.border_point(from, pts[1]);
            pts[0] = start;
            let last = pts.len() - 1;
            let end = self.border_point(to, pts[last - 1]);
            pts[last] = end;
            self.edges[i].points = pts;
        }
    }

    /// Where a line leaves a box on its way out. Straight out of the middle would be right for the box but wrong for
    /// the node that owns the end of the edge, so the crossing slides along that side towards the node - unless
    /// sliding it there would take the line across one of the node's neighbours, in which case straight out wins.
    fn leave_box(&self, box_item: usize, centre: (f32, f32), towards: (f32, f32), node: usize) -> (f32, f32) {
        let it = &self.items[box_item];
        let (hw, hh) = (it.w / 2.0, it.h / 2.0);
        let plain = rect_border(centre, towards, hw, hh);
        let inset = 10.0f32.min(hw.min(hh));
        let (nx, ny) = (self.items[node].x, self.items[node].y);
        let horizontal = (plain.1 - (centre.1 - hh)).abs() < 0.5 || (plain.1 - (centre.1 + hh)).abs() < 0.5;
        let (lo, hi) = if horizontal {
            (centre.0 - hw + inset, centre.0 + hw - inset)
        } else {
            (centre.1 - hh + inset, centre.1 + hh - inset)
        };
        let aligned = (if horizontal { nx } else { ny }).clamp(lo, hi);
        let at = |v: f32| if horizontal { (v, plain.1) } else { (plain.0, v) };
        // Straight at the node is the first choice; if that line would run over the neighbours, slide along the side
        // of the box until it does not.
        let mut best = (usize::MAX, 0.0f32, at(aligned));
        for k in 0..=8 {
            let v = if k == 0 { aligned } else { lo + (hi - lo) * (k - 1) as f32 / 7.0 };
            let score = (self.crossed_siblings(node, at(v)), (v - aligned).abs());
            if score.0 < best.0 || (score.0 == best.0 && score.1 < best.1) {
                best = (score.0, score.1, at(v));
            }
        }
        best.2
    }

    /// How many boxes beside `node` a straight line from it to `target` would run over.
    fn crossed_siblings(&self, node: usize, target: (f32, f32)) -> usize {
        let from = (self.items[node].x, self.items[node].y);
        let siblings: Vec<usize> = match self.items[node].parent {
            Some(p) => match &self.items[p].kind {
                Kind::Cluster { children, .. } => children.clone(),
                Kind::Node(_) => Vec::new(),
            },
            None => self.roots.clone(),
        };
        siblings
            .into_iter()
            .filter(|&s| s != node && segment_hits_box(from, target, &self.items[s]))
            .count()
    }

    fn border_point(&self, item: usize, towards: (f32, f32)) -> (f32, f32) {
        let it = &self.items[item];
        let centre = (it.x, it.y);
        match it.kind {
            Kind::Node(NodeShape::Circle) | Kind::Node(NodeShape::DoubleCircle) => {
                let (dx, dy) = (towards.0 - centre.0, towards.1 - centre.1);
                let len = (dx * dx + dy * dy).sqrt().max(0.001);
                let r = it.w / 2.0;
                (centre.0 + dx / len * r, centre.1 + dy / len * r)
            }
            Kind::Node(NodeShape::Diamond) => diamond_border(centre, towards, it.w / 2.0, it.h / 2.0),
            _ => rect_border(centre, towards, it.w / 2.0, it.h / 2.0),
        }
    }

    fn self_loop(&self, item: usize) -> Vec<(f32, f32)> {
        let it = &self.items[item];
        let (x, y, hw, hh) = (it.x, it.y, it.w / 2.0, it.h / 2.0);
        let out = 26.0;
        vec![
            (x + hw, y - hh * 0.45),
            (x + hw + out, y - hh * 0.45),
            (x + hw + out, y + hh * 0.45),
            (x + hw, y + hh * 0.45),
        ]
    }

    fn bounds(&self) -> (f32, f32) {
        let mut w = 0.0f32;
        let mut h = 0.0f32;
        for it in &self.items {
            w = w.max(it.x + it.w / 2.0);
            h = h.max(it.y + it.h / 2.0);
        }
        for e in &self.edges {
            for p in &e.points {
                w = w.max(p.0);
                h = h.max(p.1);
            }
        }
        (w, h)
    }
}

/// Where the segment from the middle of a box to a point outside crosses the box.
fn rect_border(centre: (f32, f32), towards: (f32, f32), hw: f32, hh: f32) -> (f32, f32) {
    let (dx, dy) = (towards.0 - centre.0, towards.1 - centre.1);
    if dx.abs() < 0.001 && dy.abs() < 0.001 {
        return (centre.0, centre.1 + hh);
    }
    let sx = if dx.abs() > 0.001 { hw / dx.abs() } else { f32::INFINITY };
    let sy = if dy.abs() > 0.001 { hh / dy.abs() } else { f32::INFINITY };
    let s = sx.min(sy);
    (centre.0 + dx * s, centre.1 + dy * s)
}

/// Does the segment run through the box? A handful of crossing tests, which is all this needs to be.
fn segment_hits_box(a: (f32, f32), b: (f32, f32), it: &Item) -> bool {
    if matches!(it.kind, Kind::Node(NodeShape::Circle) | Kind::Node(NodeShape::DoubleCircle)) {
        return point_to_segment(it.x, it.y, a, b) < it.w / 2.0;
    }
    let (l, t) = (it.x - it.w / 2.0, it.y - it.h / 2.0);
    let (r, bo) = (it.x + it.w / 2.0, it.y + it.h / 2.0);
    let inside = |p: (f32, f32)| p.0 > l && p.0 < r && p.1 > t && p.1 < bo;
    if inside(a) || inside(b) {
        return true;
    }
    let sides = [((l, t), (r, t)), ((r, t), (r, bo)), ((r, bo), (l, bo)), ((l, bo), (l, t))];
    sides.iter().any(|&(p, q)| segments_cross(a, b, p, q))
}

fn point_to_segment(px: f32, py: f32, a: (f32, f32), b: (f32, f32)) -> f32 {
    let (dx, dy) = (b.0 - a.0, b.1 - a.1);
    let len2 = dx * dx + dy * dy;
    let t = if len2 < 0.0001 { 0.0 } else { (((px - a.0) * dx + (py - a.1) * dy) / len2).clamp(0.0, 1.0) };
    let (cx, cy) = (a.0 + dx * t, a.1 + dy * t);
    ((px - cx).powi(2) + (py - cy).powi(2)).sqrt()
}

fn segments_cross(a: (f32, f32), b: (f32, f32), c: (f32, f32), d: (f32, f32)) -> bool {
    let side = |p: (f32, f32), q: (f32, f32), r: (f32, f32)| {
        (q.0 - p.0) * (r.1 - p.1) - (q.1 - p.1) * (r.0 - p.0)
    };
    let (d1, d2) = (side(a, b, c), side(a, b, d));
    let (d3, d4) = (side(c, d, a), side(c, d, b));
    (d1 * d2 < 0.0) && (d3 * d4 < 0.0)
}

fn diamond_border(centre: (f32, f32), towards: (f32, f32), hw: f32, hh: f32) -> (f32, f32) {
    let (dx, dy) = (towards.0 - centre.0, towards.1 - centre.1);
    let denom = dx.abs() / hw + dy.abs() / hh;
    if denom < 0.0001 {
        return (centre.0, centre.1 + hh);
    }
    (centre.0 + dx / denom, centre.1 + dy / denom)
}
