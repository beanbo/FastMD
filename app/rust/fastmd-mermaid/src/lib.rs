//! Mermaid diagrams → SVG, for FastMD (plan 4.2).
//!
//! A renderer in pure Rust: no browser, no WebView2, no process to start. The SVG it writes is drawn by the viewer's
//! own SVG renderer, and the work happens on a background thread after the first frame.

use std::panic::{catch_unwind, AssertUnwindSafe};

/// Renders a Mermaid diagram and hands back a UTF-8 SVG document. 1 = done, 0 = this diagram could not be drawn.
///
/// # Safety
/// `src` must point at `len` readable bytes; `out` and `out_len` must be writable. Release with `fastmd_mermaid_free`.
#[no_mangle]
pub unsafe extern "C" fn fastmd_mermaid_svg(src: *const u8, len: usize, dark: i32, out: *mut *mut u8,
                                            out_len: *mut usize) -> i32 {
    if src.is_null() || len == 0 || out.is_null() || out_len.is_null() {
        return 0;
    }
    let bytes = std::slice::from_raw_parts(src, len);
    let Ok(text) = std::str::from_utf8(bytes) else { return 0 };
    let result = catch_unwind(AssertUnwindSafe(|| render(text, dark != 0)));
    let Ok(Some(svg)) = result else { return 0 };

    let mut buf = svg.into_bytes();
    buf.shrink_to_fit();
    let n = buf.len();
    let p = buf.as_mut_ptr();
    std::mem::forget(buf);
    *out = p;
    *out_len = n;
    1
}

/// Releases a buffer handed out by `fastmd_mermaid_svg`.
///
/// # Safety
/// `p` and `len` must be exactly what one call to `fastmd_mermaid_svg` returned, and only once.
#[no_mangle]
pub unsafe extern "C" fn fastmd_mermaid_free(p: *mut u8, len: usize) {
    if !p.is_null() && len != 0 {
        drop(Vec::from_raw_parts(p, len, len));
    }
}

/// Version of the library, so the viewer can tell which build it loaded.
#[no_mangle]
pub extern "C" fn fastmd_mermaid_version() -> u32 {
    1
}

fn render(src: &str, dark: bool) -> Option<String> {
    let mut opts = mermaid_rs_renderer::RenderOptions::default();
    opts.theme = if dark { mermaid_rs_renderer::Theme::dark() } else { mermaid_rs_renderer::Theme::mermaid_default() };
    opts.theme.background = "none".to_string();  // the page behind the diagram is the viewer's, not the renderer's
    let svg = mermaid_rs_renderer::render_with_options(src, opts).ok()?;
    if svg.is_empty() {
        None
    } else {
        Some(svg)
    }
}
