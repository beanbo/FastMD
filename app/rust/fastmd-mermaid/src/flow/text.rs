//! How wide a label is going to be when the viewer finally draws it.
//!
//! The viewer draws SVG text with its own renderer (lunasvg, see src/svg_dll.cpp), and the only families it registers
//! are the ones in `RegisterFonts` - Segoe UI among them, as the default. So we measure with the very file that will
//! be drawn with, `segoeui.ttf`, and the diagram says `font-family="Segoe UI"`. Measuring one font and drawing another
//! is what makes labels stick out of their boxes.

use std::sync::OnceLock;
use ttf_parser::Face;

static FONT_DATA: OnceLock<Option<Vec<u8>>> = OnceLock::new();

fn font_bytes() -> Option<&'static [u8]> {
    FONT_DATA
        .get_or_init(|| {
            let root = std::env::var("SystemRoot").unwrap_or_else(|_| String::from("C:\\Windows"));
            std::fs::read(format!("{root}\\Fonts\\segoeui.ttf")).ok()
        })
        .as_deref()
}

/// Advance widths of one font. Without the file we fall back to a rough average: labels then get a little more room
/// than they need, which is the harmless direction to be wrong in.
pub struct Font {
    face: Option<Face<'static>>,
    upem: f32,
}

impl Font {
    pub fn load() -> Font {
        let face = font_bytes().and_then(|d| Face::parse(d, 0).ok());
        let upem = face.as_ref().map(|f| f.units_per_em() as f32).unwrap_or(2048.0);
        Font { face, upem }
    }

    fn advance(&self, c: char) -> f32 {
        if let Some(face) = &self.face {
            if let Some(a) = face.glyph_index(c).and_then(|g| face.glyph_hor_advance(g)) {
                return a as f32 / self.upem;
            }
        }
        // No glyph (or no font at all): guess by the kind of character.
        match c {
            'i' | 'j' | 'l' | 't' | 'f' | 'I' | '.' | ',' | ':' | ';' | '\'' | '!' | '|' => 0.28,
            'm' | 'w' | 'M' | 'W' => 0.85,
            ' ' => 0.26,
            c if (c as u32) >= 0x1100 => 1.0, // CJK and friends are square
            _ => 0.55,
        }
    }

    /// Width of one line, in pixels, at `size`.
    pub fn width(&self, text: &str, size: f32) -> f32 {
        text.chars().map(|c| self.advance(c)).sum::<f32>() * size
    }
}

/// `<br>` in any spelling, and a real newline, are where the author asked for a new line.
fn split_hard_breaks(text: &str) -> Vec<String> {
    let mut out = vec![String::new()];
    let bytes: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == '<' {
            // <br>, <br/>, <br />
            let rest: String = bytes[i..].iter().take(6).collect::<String>().to_ascii_lowercase();
            if rest.starts_with("<br") {
                if let Some(end) = bytes[i..].iter().position(|&c| c == '>') {
                    out.push(String::new());
                    i += end + 1;
                    continue;
                }
            }
            // any other tag: drop it, keep the text inside
            match bytes[i..].iter().position(|&c| c == '>') {
                Some(end) if end <= 24 => {
                    i += end + 1;
                    continue;
                }
                _ => {}
            }
        }
        if bytes[i] == '\\' && i + 1 < bytes.len() && bytes[i + 1] == 'n' {
            out.push(String::new());
            i += 2;
            continue;
        }
        if bytes[i] == '\n' {
            out.push(String::new());
            i += 1;
            continue;
        }
        out.last_mut().unwrap().push(bytes[i]);
        i += 1;
    }
    out.into_iter().map(|s| unescape(s.trim())).collect()
}

/// The few entities Mermaid authors actually type, plus its own `#nn;` spelling.
fn unescape(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '&' || chars[i] == '#' {
            let tail: String = chars[i..].iter().take(10).collect();
            let named: &[(&str, char)] = &[
                ("&amp;", '&'), ("&lt;", '<'), ("&gt;", '>'), ("&quot;", '"'), ("&apos;", '\''),
                ("&nbsp;", ' '), ("&#35;", '#'), ("#quot;", '"'), ("#hash;", '#'), ("#semi;", ';'),
                ("#colon;", ':'),
            ];
            if let Some((pat, ch)) = named.iter().find(|(p, _)| tail.starts_with(p)) {
                out.push(*ch);
                i += pat.chars().count();
                continue;
            }
        }
        out.push(chars[i]);
        i += 1;
    }
    out
}

/// A measured label: the lines as they will be drawn, and the box they need.
#[derive(Clone, Debug, Default)]
pub struct Label {
    pub lines: Vec<String>,
    pub width: f32,
    pub height: f32,
}

impl Label {
    pub fn is_empty(&self) -> bool {
        self.lines.iter().all(|l| l.is_empty())
    }
}

/// Measures a label, wrapping at `max_width` on word boundaries - the author's own `<br>` always wins.
pub fn measure(font: &Font, text: &str, size: f32, line_height: f32, max_width: f32) -> Label {
    let mut lines: Vec<String> = Vec::new();
    for hard in split_hard_breaks(text) {
        if font.width(&hard, size) <= max_width || hard.is_empty() {
            lines.push(hard);
            continue;
        }
        let mut current = String::new();
        for word in hard.split(' ') {
            let candidate = if current.is_empty() { word.to_string() } else { format!("{current} {word}") };
            if font.width(&candidate, size) <= max_width || current.is_empty() {
                current = candidate;
            } else {
                lines.push(std::mem::take(&mut current));
                current = word.to_string();
            }
        }
        if !current.is_empty() {
            lines.push(current);
        }
    }
    if lines.is_empty() {
        lines.push(String::new());
    }
    let width = lines.iter().map(|l| font.width(l, size)).fold(0.0f32, f32::max);
    let height = lines.len() as f32 * size * line_height;
    Label { lines, width, height }
}

/// Text going into an SVG attribute or element.
pub fn escape(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for c in text.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            _ => out.push(c),
        }
    }
    out
}
