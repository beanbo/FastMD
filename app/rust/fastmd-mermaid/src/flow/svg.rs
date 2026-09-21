//! Turning the finished layout into SVG the viewer can draw.
//!
//! Deliberately a small subset: shapes, straight and rounded polylines, arrow heads as real polygons, and `<text>`
//! with one `<tspan>` per line. No markers, no CSS, no foreignObject - lunasvg draws all of this and nothing here
//! depends on a feature we would have to test for.

use super::text::{escape, Label};
use super::{Item, Kind, Model};
use mermaid_rs_renderer::ir::{EdgeDecoration, EdgeStyle, NodeShape, NodeStyle};
use mermaid_rs_renderer::Theme;

fn n(v: f32) -> String {
    let r = (v * 100.0).round() / 100.0;
    if r == r.trunc() { format!("{}", r as i64) } else { format!("{r}") }
}

pub fn write(m: &Model, theme: &Theme, width: f32, height: f32) -> String {
    let mut s = String::with_capacity(4096);
    s.push_str(&format!(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{}\" height=\"{}\" viewBox=\"0 0 {} {}\">",
        n(width), n(height), n(width), n(height)
    ));
    if theme.background != "none" && !theme.background.is_empty() {
        s.push_str(&format!(
            "<rect x=\"0\" y=\"0\" width=\"{}\" height=\"{}\" fill=\"{}\"/>",
            n(width), n(height), escape(&theme.background)
        ));
    }

    // Boxes first, outermost first, so a nested box sits on top of the one that holds it.
    let mut clusters: Vec<usize> = (0..m.items.len())
        .filter(|&i| matches!(m.items[i].kind, Kind::Cluster { .. }))
        .collect();
    clusters.sort_by_key(|&i| m.items[i].depth);
    for i in clusters {
        cluster(&mut s, &m.items[i], theme);
    }
    for e in &m.edges {
        edge(&mut s, m, e, theme);
    }
    for it in &m.items {
        if matches!(it.kind, Kind::Node(_)) {
            node(&mut s, it, theme);
        }
    }
    s.push_str("</svg>");
    s
}

// ---------------------------------------------------------------------------------------------------- boxes

fn cluster(s: &mut String, it: &Item, theme: &Theme) {
    let (x, y) = (it.x - it.w / 2.0, it.y - it.h / 2.0);
    let fill = it.style.fill.clone().unwrap_or_else(|| theme.cluster_background.clone());
    let stroke = it.style.stroke.clone().unwrap_or_else(|| theme.cluster_border.clone());
    s.push_str(&format!(
        "<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"8\" ry=\"8\" fill=\"{}\" stroke=\"{}\" \
         stroke-width=\"{}\"/>",
        n(x), n(y), n(it.w), n(it.h), escape(&fill), escape(&stroke),
        n(it.style.stroke_width.unwrap_or(1.0))
    ));
    if !it.label.is_empty() {
        let colour = it.style.text_color.clone().unwrap_or_else(|| theme.text_color.clone());
        let line = theme.font_size * 1.5;
        let top = y + 8.0;
        text_lines(s, &it.label, it.x, top + line * 0.5 + theme.font_size * 0.35, line, theme.font_size, &colour);
    }
}

// ---------------------------------------------------------------------------------------------------- nodes

fn node(s: &mut String, it: &Item, theme: &Theme) {
    let Kind::Node(shape) = it.kind else { return };
    let fill = it.style.fill.clone().unwrap_or_else(|| theme.primary_color.clone());
    let stroke = it.style.stroke.clone().unwrap_or_else(|| theme.primary_border_color.clone());
    let text_colour = it.style.text_color.clone().unwrap_or_else(|| theme.primary_text_color.clone());
    let paint = paint_attrs(&it.style, &fill, &stroke);
    let (x, y, w, h) = (it.x, it.y, it.w, it.h);
    let (l, t, r, b) = (x - w / 2.0, y - h / 2.0, x + w / 2.0, y + h / 2.0);

    match shape {
        NodeShape::Text => {}
        NodeShape::Circle => {
            s.push_str(&format!("<circle cx=\"{}\" cy=\"{}\" r=\"{}\" {paint}/>", n(x), n(y), n(w / 2.0)));
        }
        NodeShape::DoubleCircle => {
            s.push_str(&format!("<circle cx=\"{}\" cy=\"{}\" r=\"{}\" {paint}/>", n(x), n(y), n(w / 2.0)));
            s.push_str(&format!(
                "<circle cx=\"{}\" cy=\"{}\" r=\"{}\" fill=\"none\" stroke=\"{}\"/>",
                n(x), n(y), n(w / 2.0 - 4.0), escape(&stroke)
            ));
        }
        NodeShape::Diamond => {
            polygon(s, &[(x, t), (r, y), (x, b), (l, y)], &paint);
        }
        NodeShape::Hexagon => {
            let c = (h / 2.0).min(w / 3.0);
            polygon(s, &[(l + c, t), (r - c, t), (r, y), (r - c, b), (l + c, b), (l, y)], &paint);
        }
        NodeShape::Parallelogram => {
            let c = (h * 0.6).min(w / 3.0);
            polygon(s, &[(l + c, t), (r, t), (r - c, b), (l, b)], &paint);
        }
        NodeShape::ParallelogramAlt => {
            let c = (h * 0.6).min(w / 3.0);
            polygon(s, &[(l, t), (r - c, t), (r, b), (l + c, b)], &paint);
        }
        NodeShape::Trapezoid => {
            let c = (h * 0.7).min(w / 3.0);
            polygon(s, &[(l + c, t), (r - c, t), (r, b), (l, b)], &paint);
        }
        NodeShape::TrapezoidAlt => {
            let c = (h * 0.7).min(w / 3.0);
            polygon(s, &[(l, t), (r, t), (r - c, b), (l + c, b)], &paint);
        }
        NodeShape::Asymmetric => {
            let c = (h * 0.4).min(w / 4.0);
            polygon(s, &[(l, t), (r, t), (r, b), (l, b), (l + c, y)], &paint);
        }
        NodeShape::Cylinder => {
            let ry = 7.0f32.min(h / 4.0);
            s.push_str(&format!(
                "<path d=\"M {} {} L {} {} A {} {} 0 0 0 {} {} L {} {} A {} {} 0 0 0 {} {} Z\" {paint}/>",
                n(l), n(t + ry), n(l), n(b - ry), n(w / 2.0), n(ry), n(r), n(b - ry),
                n(r), n(t + ry), n(w / 2.0), n(ry), n(l), n(t + ry)
            ));
            s.push_str(&format!(
                "<path d=\"M {} {} A {} {} 0 0 0 {} {}\" fill=\"none\" stroke=\"{}\"/>",
                n(l), n(t + ry), n(w / 2.0), n(ry), n(r), n(t + ry), escape(&stroke)
            ));
        }
        NodeShape::Subroutine => {
            s.push_str(&format!(
                "<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" {paint}/>",
                n(l), n(t), n(w), n(h)
            ));
            for dx in [8.0f32, w - 8.0] {
                s.push_str(&format!(
                    "<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" stroke=\"{}\"/>",
                    n(l + dx), n(t), n(l + dx), n(b), escape(&stroke)
                ));
            }
        }
        NodeShape::ForkJoin => {
            s.push_str(&format!(
                "<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" fill=\"{}\" stroke=\"{}\"/>",
                n(l), n(t), n(w), n(h), escape(&stroke), escape(&stroke)
            ));
        }
        NodeShape::Stadium => {
            s.push_str(&format!(
                "<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"{}\" ry=\"{}\" {paint}/>",
                n(l), n(t), n(w), n(h), n(h / 2.0), n(h / 2.0)
            ));
        }
        NodeShape::RoundRect | NodeShape::MindmapDefault => {
            s.push_str(&format!(
                "<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"6\" ry=\"6\" {paint}/>",
                n(l), n(t), n(w), n(h)
            ));
        }
        _ => {
            s.push_str(&format!(
                "<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"3\" ry=\"3\" {paint}/>",
                n(l), n(t), n(w), n(h)
            ));
        }
    }

    if !it.label.is_empty() {
        let line = theme.font_size * 1.5;
        let first = y - it.label.height / 2.0 + line * 0.5 + theme.font_size * 0.35;
        text_lines(s, &it.label, x, first, line, theme.font_size, &text_colour);
    }
}

fn paint_attrs(style: &NodeStyle, fill: &str, stroke: &str) -> String {
    let mut out = format!(
        "fill=\"{}\" stroke=\"{}\" stroke-width=\"{}\"",
        escape(fill), escape(stroke), n(style.stroke_width.unwrap_or(1.0))
    );
    if let Some(dash) = &style.stroke_dasharray {
        out.push_str(&format!(" stroke-dasharray=\"{}\"", escape(dash)));
    }
    out
}

fn polygon(s: &mut String, pts: &[(f32, f32)], paint: &str) {
    let body: Vec<String> = pts.iter().map(|p| format!("{},{}", n(p.0), n(p.1))).collect();
    s.push_str(&format!("<polygon points=\"{}\" {paint}/>", body.join(" ")));
}

fn text_lines(s: &mut String, label: &Label, cx: f32, first: f32, line: f32, size: f32, colour: &str) {
    s.push_str(&format!(
        "<text x=\"{}\" y=\"{}\" text-anchor=\"middle\" font-family=\"Segoe UI\" font-size=\"{}\" fill=\"{}\">",
        n(cx), n(first), n(size), escape(colour)
    ));
    for (i, l) in label.lines.iter().enumerate() {
        s.push_str(&format!(
            "<tspan x=\"{}\" dy=\"{}\">{}</tspan>",
            n(cx), if i == 0 { String::from("0") } else { n(line) }, escape(l)
        ));
    }
    s.push_str("</text>");
}

// ---------------------------------------------------------------------------------------------------- edges

fn edge(s: &mut String, m: &Model, e: &super::Edge, theme: &Theme) {
    let mut pts = e.points.clone();
    if pts.len() < 2 {
        return;
    }
    let ir = &m.graph.edges[e.ir];
    let over = m.graph.edge_styles.get(&e.ir).or(m.graph.edge_style_default.as_ref());
    let colour = over
        .and_then(|o| o.stroke.clone())
        .unwrap_or_else(|| theme.line_color.clone());
    let width = over
        .and_then(|o| o.stroke_width)
        .unwrap_or(if ir.style == EdgeStyle::Thick { 3.0 } else { 1.7 });
    let dash = over.and_then(|o| o.dasharray.clone()).or(match ir.style {
        EdgeStyle::Dotted => Some(String::from("4 4")),
        _ => None,
    });

    // Leave room for the head, so the line does not show through its tip.
    let head = 9.0f32;
    // `--o` and `--x` put a mark on the end without an arrow head, so a decoration counts as an ending too.
    let tail_end = (ir.arrow_end && ir.directed) || ir.end_decoration.is_some();
    let tail_start = ir.arrow_start || ir.start_decoration.is_some();
    if tail_end {
        shorten(&mut pts, head * 0.8, true);
    }
    if tail_start {
        shorten(&mut pts, head * 0.8, false);
    }

    let mut attrs = format!(
        "fill=\"none\" stroke=\"{}\" stroke-width=\"{}\" stroke-linecap=\"round\" stroke-linejoin=\"round\"",
        escape(&colour), n(width)
    );
    if let Some(d) = &dash {
        attrs.push_str(&format!(" stroke-dasharray=\"{}\"", escape(d)));
    }
    s.push_str(&format!("<path d=\"{}\" {attrs}/>", path_d(&pts, 9.0)));

    if tail_end {
        let tip = e.points[e.points.len() - 1];
        let from = pts[pts.len() - 1];
        arrow(s, m, tip, from, &colour, ir.end_decoration, ir.arrow_end_kind.is_some());
    }
    if tail_start {
        let tip = e.points[0];
        let from = pts[0];
        arrow(s, m, tip, from, &colour, ir.start_decoration, ir.arrow_start_kind.is_some());
    }

    if let (Some((lx, ly)), false) = (e.label_at, e.label.is_empty()) {
        let (w, h) = (e.label.width + 10.0, e.label.height + 4.0);
        s.push_str(&format!(
            "<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" fill=\"{}\" stroke=\"none\"/>",
            n(lx - w / 2.0), n(ly - h / 2.0), n(w), n(h), escape(&theme.edge_label_background)
        ));
        let colour = over.and_then(|o| o.label_color.clone()).unwrap_or_else(|| theme.text_color.clone());
        let line = theme.font_size * 1.5;
        text_lines(s, &e.label, lx, ly - h / 2.0 + line * 0.5 + theme.font_size * 0.35, line, theme.font_size, &colour);
    }
}

/// Pulls the end of a polyline back along its last segment.
fn shorten(pts: &mut [(f32, f32)], by: f32, at_end: bool) {
    let len = pts.len();
    if len < 2 {
        return;
    }
    let (i, j) = if at_end { (len - 1, len - 2) } else { (0, 1) };
    let (dx, dy) = (pts[i].0 - pts[j].0, pts[i].1 - pts[j].1);
    let d = (dx * dx + dy * dy).sqrt();
    if d <= by + 0.5 {
        return;
    }
    pts[i] = (pts[i].0 - dx / d * by, pts[i].1 - dy / d * by);
}

fn arrow(
    s: &mut String,
    _m: &Model,
    tip: (f32, f32),
    from: (f32, f32),
    colour: &str,
    decoration: Option<EdgeDecoration>,
    open: bool,
) {
    let (dx, dy) = (tip.0 - from.0, tip.1 - from.1);
    let d = (dx * dx + dy * dy).sqrt().max(0.001);
    let (ux, uy) = (dx / d, dy / d);
    let (px, py) = (-uy, ux);
    match decoration {
        Some(EdgeDecoration::Circle) => {
            let c = (tip.0 - ux * 5.0, tip.1 - uy * 5.0);
            s.push_str(&format!(
                "<circle cx=\"{}\" cy=\"{}\" r=\"5\" fill=\"none\" stroke=\"{}\" stroke-width=\"1.7\"/>",
                n(c.0), n(c.1), escape(colour)
            ));
        }
        Some(EdgeDecoration::Cross) => {
            let c = (tip.0 - ux * 5.0, tip.1 - uy * 5.0);
            let arm = 5.0;
            for sign in [1.0f32, -1.0] {
                let (ax, ay) = (ux + px * sign, uy + py * sign);
                let l = (ax * ax + ay * ay).sqrt();
                s.push_str(&format!(
                    "<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" stroke=\"{}\" stroke-width=\"1.7\"/>",
                    n(c.0 - ax / l * arm), n(c.1 - ay / l * arm),
                    n(c.0 + ax / l * arm), n(c.1 + ay / l * arm), escape(colour)
                ));
            }
        }
        _ => {
            let (len, half) = (10.0f32, 4.2f32);
            let base = (tip.0 - ux * len, tip.1 - uy * len);
            let a = (base.0 + px * half, base.1 + py * half);
            let b = (base.0 - px * half, base.1 - py * half);
            if open {
                s.push_str(&format!(
                    "<path d=\"M {} {} L {} {} L {} {}\" fill=\"none\" stroke=\"{}\" stroke-width=\"1.7\" \
                     stroke-linecap=\"round\" stroke-linejoin=\"round\"/>",
                    n(a.0), n(a.1), n(tip.0), n(tip.1), n(b.0), n(b.1), escape(colour)
                ));
            } else {
                s.push_str(&format!(
                    "<polygon points=\"{},{} {},{} {},{}\" fill=\"{}\" stroke=\"{}\"/>",
                    n(tip.0), n(tip.1), n(a.0), n(a.1), n(b.0), n(b.1), escape(colour), escape(colour)
                ));
            }
        }
    }
}

/// A polyline with its corners rounded off - close enough to Mermaid's curves, and cheap to draw.
fn path_d(pts: &[(f32, f32)], radius: f32) -> String {
    if pts.len() < 2 {
        return String::new();
    }
    let mut d = format!("M {} {}", n(pts[0].0), n(pts[0].1));
    for i in 1..pts.len() - 1 {
        let (p, c, q) = (pts[i - 1], pts[i], pts[i + 1]);
        let (v1x, v1y) = (c.0 - p.0, c.1 - p.1);
        let (v2x, v2y) = (q.0 - c.0, q.1 - c.1);
        let l1 = (v1x * v1x + v1y * v1y).sqrt().max(0.001);
        let l2 = (v2x * v2x + v2y * v2y).sqrt().max(0.001);
        // A straight-through point needs no corner.
        if (v1x / l1 - v2x / l2).abs() + (v1y / l1 - v2y / l2).abs() < 0.02 {
            d.push_str(&format!(" L {} {}", n(c.0), n(c.1)));
            continue;
        }
        let r = radius.min(l1 * 0.45).min(l2 * 0.45);
        d.push_str(&format!(
            " L {} {} Q {} {} {} {}",
            n(c.0 - v1x / l1 * r), n(c.1 - v1y / l1 * r),
            n(c.0), n(c.1),
            n(c.0 + v2x / l2 * r), n(c.1 + v2y / l2 * r)
        ));
    }
    let last = pts[pts.len() - 1];
    d.push_str(&format!(" L {} {}", n(last.0), n(last.1)));
    d
}
