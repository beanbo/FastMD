//! TeX formulas → SVG, for FastMD (plan 4.1).
//!
//! RaTeX (a KaTeX-compatible engine in Rust) parses and lays the formula out; the SVG it writes carries glyph
//! outlines, not font references, so the viewer's own SVG renderer draws it with nothing else installed. The caller
//! gets the natural size and the baseline, which is what an inline formula needs to sit on the text line.
//!
//! The whole C ABI is three functions, all of them panic-safe: a broken formula must never take the viewer with it.

use std::panic::{catch_unwind, AssertUnwindSafe};

use ratex_types::color::Color;
use ratex_types::math_style::MathStyle;

/// Renders `tex` and hands back a UTF-8 SVG document. 1 = done, 0 = the formula could not be rendered.
///
/// `display` picks display style (a formula on its own line) over inline style; `font_px` is the size the surrounding
/// text is drawn at; `rgb` is 0xRRGGBB. `out` must be released with `fastmd_tex_free`. `out_w` / `out_h` come back in
/// the same units as `font_px`, and `out_ascent` is the part of the height that stands above the baseline.
///
/// # Safety
/// `tex` must point at `len` readable bytes; every out pointer must be writable.
#[no_mangle]
pub unsafe extern "C" fn fastmd_tex_svg(tex: *const u8, len: usize, display: i32, font_px: f32, rgb: u32,
                                        out: *mut *mut u8, out_len: *mut usize, out_w: *mut f32, out_h: *mut f32,
                                        out_ascent: *mut f32) -> i32 {
    if tex.is_null() || len == 0 || out.is_null() || out_len.is_null() {
        return 0;
    }
    let src = std::slice::from_raw_parts(tex, len);
    let Ok(src) = std::str::from_utf8(src) else { return 0 };
    let font = if font_px.is_finite() && font_px > 1.0 { font_px as f64 } else { 16.0 };
    let result = catch_unwind(AssertUnwindSafe(|| render(src, display != 0, font, rgb)));
    let Ok(Some((svg, w, h, ascent))) = result else { return 0 };

    let mut bytes = svg.into_bytes();
    bytes.shrink_to_fit();
    let n = bytes.len();
    let p = bytes.as_mut_ptr();
    std::mem::forget(bytes);
    *out = p;
    *out_len = n;
    if !out_w.is_null() { *out_w = w as f32; }
    if !out_h.is_null() { *out_h = h as f32; }
    if !out_ascent.is_null() { *out_ascent = ascent as f32; }
    1
}

/// Releases a buffer handed out by `fastmd_tex_svg`.
///
/// # Safety
/// `p` and `len` must be exactly what one call to `fastmd_tex_svg` returned, and only once.
#[no_mangle]
pub unsafe extern "C" fn fastmd_tex_free(p: *mut u8, len: usize) {
    if !p.is_null() && len != 0 {
        drop(Vec::from_raw_parts(p, len, len));
    }
}

/// Version of the library, so the viewer can tell which build it loaded.
#[no_mangle]
pub extern "C" fn fastmd_tex_version() -> u32 {
    1
}

fn render(src: &str, display: bool, font_px: f64, rgb: u32) -> Option<(String, f64, f64, f64)> {
    let ast = ratex_parser::parse(src).ok()?;
    let mut opts = ratex_layout::LayoutOptions::default();
    opts.style = if display { MathStyle::Display } else { MathStyle::Text };
    opts.color = Color {
        r: ((rgb >> 16) & 0xff) as f32 / 255.0,
        g: ((rgb >> 8) & 0xff) as f32 / 255.0,
        b: (rgb & 0xff) as f32 / 255.0,
        a: 1.0,
    };
    let boxes = ratex_layout::layout(&ast, &opts);
    let list = ratex_layout::to_display_list(&boxes);
    let mut svg = ratex_svg::SvgOptions::default();
    svg.font_size = font_px;
    svg.padding = 0.0;       // the viewer places the formula itself; a margin here would only shift it
    svg.embed_glyphs = true; // outlines, not <text> with KaTeX font names
    let out = ratex_svg::render_to_svg(&list, &svg);
    if out.is_empty() {
        return None;
    }
    let w = list.width * font_px;
    let h = (list.height + list.depth) * font_px;
    if !(w.is_finite() && h.is_finite()) || w <= 0.0 || h <= 0.0 {
        return None;
    }
    Some((out, w, h, list.height * font_px))
}
