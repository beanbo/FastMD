[Русский](README.md) | **English**

# litehtml-sciter — "lightweight HTML engines" for FastMD

Two prototypes for one question: can we reuse ready-made HTML/CSS layout (and the shared `bench/common/style.css`) without
the weight and startup cost of WebView2/Chromium?

* **(b) litehtml 0.10 + a custom `document_container` on Direct2D/DirectWrite** (C++20, CMake + Ninja) — the main variant.
* **(a) Sciter.JS 6.0.5.0 + a thin C++ host** (sciter.dll).

In both cases Markdown is converted to HTML by **md4c-html 0.5.3** (GFM: tables, strikethrough, task lists,
autolinks); the page = `style.css` (embedded into the exe at build time) + a few engine-specific rules (see below).

**Summary** (medians from `results/quick-3.json`; the machine was shared with other agents, so the orders of magnitude are reliable,
ms ± 10–20 %):

| variant | small | medium | large | distribution |
|---|---|---|---|---|
| **`litehtml-sciter-litehtml`** (best) | **108 ms** | **120 ms** | **121 ms** | 1.79 MB (a single exe) |
| `litehtml-sciter-litehtml-gpu` | 291 ms | 303 ms | 306 ms | 1.79 MB |
| `litehtml-sciter-sciter` | 426 ms | 426 ms | 428 ms | 18.75 MB (exe + sciter.dll) |

`baseline-win32` (an empty window + GDI) takes ≈ 70–90 ms on this machine, `cpp-d2d` (custom renderer) ≈ 70 ms. litehtml gives an almost
browser-grade GitHub layout from the same CSS for ≈ +30–40 ms over the floor; the critical path is software rasterization of the first
screen (≈ 25 ms of which is color emoji). Sciter is ≈ 4 times slower, 10 times heavier, its CSS is non-standard, and its main
knob (the graphics layer) breaks rendering in 6.0.5.0 — not suitable for FastMD.

The main lesson from litehtml: **layout of a large document is superlinear and memory-hungry** (3.7 MB as a single document
does not fit in 30 s and would take gigabytes). That is why the prototype keeps the document as a **stack of independent
litehtml documents, one per section** (split at headings): the first screen is the first chunk of ≈ 8 KB, the remaining 228 chunks
of large are laid out by 4 background threads within ≈ 1.5–2.3 s after startup, and chunks far from the viewport are unloaded.

## Stack and versions

| component | version / source | license |
|---|---|---|
| litehtml | 0.10 (tag `v0.10`, github.com/litehtml/litehtml), static library, with gumbo (HTML5 parser) inside | BSD-3 (+ Apache-2.0 gumbo) |
| md4c / md4c-html | 0.5.3 (tag `release-0.5.3`, github.com/mity/md4c) | MIT |
| Sciter.JS SDK | 6.0.5.0 (`sciter-js-sdk` main, gitlab.com/sciter-engine), `bin/windows/x64/sciter.dll` = 19,292,160 bytes | binary free, source code paid |
| compiler | MSVC 19.44.35209 (VS 2022, toolset 14.44), Windows SDK 10.0.26100 | |
| build | CMake 3.31.6-msvc6 + Ninja 1.12.1 (bundled with VS), `/O2 /GL /LTCG /MT` (static CRT), `/utf-8` | |
| graphics | Direct2D 1.x (software DC render target = WARP, or `ID2D1HwndRenderTarget`), DirectWrite (shared factory) | system |

Dependencies are downloaded by `build.ps1` into `bench/.tools/litehtml-sciter/` (zip archives in `dl/`); the prototype directory
contains only our own code (≈ 1,750 lines of C++).

## How to build

```
pwsh -File bench/protos/litehtml-sciter/build.ps1
```

The script: downloads litehtml / md4c / the Sciter headers / sciter.dll (curl) if missing, deletes its own `obj/` and `out/`,
runs `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release` + `cmake --build` via `bench/tools/msvc.cmd`. A clean build takes ≈ 20 s.

```
out/litehtml/fastmd-litehtml.exe   1 878 016 байт (litehtml + gumbo + md4c + наш код, без своих DLL)
out/sciter/fastmd-sciter.exe          373 248 байт
out/sciter/sciter.dll              19 292 160 байт
```

Files: `CMakeLists.txt`; `src/common/mdpage.{h,cpp}` (file reading, md4c-html, splitting at headings, argv),
`src/common/highlight.cpp` (syntax highlighting);
`src/litehtml/main.cpp` (window, threads, chunk model, scrolling), `src/litehtml/container_d2d.{h,cpp}`
(document_container); `src/sciter/main.cpp`; `src/app.manifest` (PerMonitorV2). `style.css` is turned into
`obj/gen/style_css.h` at configure time.

## Variants (`proto.json`)

| id | exe / arguments | what it is |
|---|---|---|
| **`litehtml-sciter-litehtml`** | `fastmd-litehtml.exe --rt=sw --no-ime` | the best one, pipeline below |
| `litehtml-sciter-litehtml-gpu` | `fastmd-litehtml.exe --rt=hwnd --no-ime` | the same layout, but with a classic `ID2D1HwndRenderTarget` (hardware D3D on an RTX 5070), drawing on the UI thread |
| `litehtml-sciter-sciter` | `fastmd-sciter.exe --no-ime` | Sciter.JS: `SciterCreateWindow` + `SCITER_APP_LOOP`, the first ≈ 4 KB of markdown, the rest is appended after the first frame |

### Pipeline of the best variant

```
UI-поток:    fmb_init → argv → запуск worker → ImmDisableIME → CreateWindow → ShowWindow → цикл сообщений
worker:      чтение файла → md4c-html первого куска (markdown до первого заголовка после 8 KB) → DirectWrite →
             litehtml createFromString + render(ширина окна известна заранее: 1000 DIP − полоса прокрутки)
графпоток:   (параллельно раскладке) D2D factory + software DC render target + DIB-секция, прогрев цветных эмодзи
worker:      draw() первого экрана в DIB (Direct2D software/WARP)
WM_PAINT:    BitBlt DIB → EndPaint → DwmFlush → t_content          (PROTOCOL §3 для GDI/D2D)
после кадра: остальной markdown режется по заголовкам на куски ≈ 16 KB; 4 потока раскладывают их по порядку,
             UI вставляет каждый готовый кусок (высоты ещё не разложенных оцениваются по байтам, позиция чтения
             не прыгает); далёкие от экрана документы выгружаются (остаётся высота), при прокрутке — пересоздаются
```

Each chunk is a separate litehtml document with the same CSS plus two "seam" rules (`padding-top:0` while keeping the
24 px of the heading that always starts a chunk; `padding-bottom:0` and a zero bottom margin on the last block) —
the layout at the seam matches the whole document pixel for pixel (`shots/detail-litehtml-medium-chunk-seam-5100.png`).
The container is shared: fonts are deduplicated by description (with a reference count), the "word → `IDWriteTextLayout`" cache is shared
and thread-safe (`shared_mutex`), and the root element's background is not drawn (otherwise the next chunk would paint over the previous one).

Fairness: t_content is taken after `EndPaint` of the frame with the rendered document and `DwmFlush()`. The first screen is the **real
layout of the real document** (its first chunk, split at a heading, so the first screen is identical to the
full document); if the chunk is shorter than the window, it is grown ×4. This is the allowed "first viewport first" technique (PROTOCOL §3).
No resident processes, no caches between runs.

### Knobs (A/B script with interleaved configurations, medium, n = 5–6 in one session)

| knob | effect |
|---|---|
| `--no-ime` (`ImmDisableIME(-1)` before the window is created) | `ShowWindow` ≈ 45–60 → ≈ 20 ms: **−30…40 ms** (TSF/IME is initialized on the window's first activation). A viewer does not need IME. For Sciter it is within noise (the GPU dominates there) |
| The D2D target is created on a separate thread in parallel with layout (default; `--serial-gfx` turns it off) | ≈ −10 ms: software D2D = loading d3d11 + d3d10warp + the WARP device ≈ 15–30 ms |
| `--first=8192` vs `--first=0` (the whole file as one document before the first frame) | medium 130 → 186 ms; large — **timeout > 30 s**. `--first=4096` ≈ same as 8192 |
| emoji warm-up (`--no-warm` turns it off) | within noise (≈ −5 ms): the common path is warmed up, but each new glyph still costs ≈ 4–5 ms |
| `--rt=hwnd` (hardware D2D) | **+200…250 ms**: creating the NVIDIA D3D11 device inside `CreateHwndRenderTarget` (same as `cpp-d2d-gpu`) |
| `--chunk=N`, `--pool=N` (background chunk size, number of threads) | no effect on the first frame; large: with 4 threads all chunks are done in 1.5–2.3 s |
| Sciter `--no-dcomp` (`SCITER_ENABLE_DIRECT_COMPOSITION=FALSE`) | within noise |
| Sciter `SCITER_SET_GFX_LAYER` (WARP, D2D, Skia, AUTO — any value) | **the window stays blank**, even though rAF×2 and `xcall` fire. The knob does not work; the default layer (GPU) is used |
| Sciter `--first=0` (the whole document in `SciterLoadHtml`) | medium ≈ 0.8–1.4 s instead of ≈ 0.42 s (Sciter spends ≈ 12–15 ms per KB of markdown) |

### Sciter

Page: `<html window-width="1000dip" window-height="800dip">` + `<meta charset=utf-8>` + style.css + the article. A script
at the end of `body`: `requestAnimationFrame(→ xcall('fastmd_frame') → requestAnimationFrame(→ xcall('fastmd_ready')))`;
the host catches `HANDLE_SCRIPTING_METHOD_CALL` (a window handler via `SciterWindowAttachEventHandler`), calls `DwmFlush` and
writes the result. t_window = the first rAF. `SciterLoadHtml` receives the first ≈ 4 KB of markdown (up to a heading); the rest
is appended after the first frame in 16 KB chunks (`SciterSetElementHtml(article, SIH_APPEND_AFTER_LAST)`, one
per message). sciter.dll is loaded on a separate thread in parallel with reading and converting the file.

## Spec support (PROTOCOL §5)

| element | litehtml + D2D | Sciter |
|---|---|---|
| 1000×800 DIP window, PerMonitorV2, title `<file> — FastMD (<id>)` | yes (client area 1000 DIP minus the scrollbar) | yes, via `window-width/height` (without them Sciter uses 800×600) |
| fonts Segoe UI Variable Text / Cascadia Mono, 15 px, line-height 1.6, colors | yes (DirectWrite; the first installed family from the CSS list) | yes |
| centered 860 DIP column, 32 DIP margins | yes (`max-width` + `margin: 0 auto`) | after a fix: Sciter ignores `margin: auto` → `width:860px; margin-left:*; margin-right:*` (its own flex units) |
| h1–h6, rule under h1/h2, gray h6 | yes | yes |
| bold / italic / `code` / ~~strikethrough~~ / colored links | yes (we draw underline/strikethrough ourselves from font metrics) | yes; inline code has no padding/rounding, italic is synthetic |
| lists: bulleted, numbered, nested | yes (disc/circle/square as shapes, numbers as litehtml text) | yes |
| task lists | yes: md4c `<input>` → `<span class="task-cb">` (litehtml does not draw form controls), a blue square with ✓ | yes, native checkboxes |
| blockquotes, hr | yes | yes |
| code blocks: monospace, `#f6f8fa`, radius 6, padding 16, **no wrapping** (long lines are clipped, the block has no horizontal scrolling) | yes | yes |
| GFM tables: borders, bold header, zebra stripes, alignment | yes, content-sized width (as on GitHub) | yes, but the table is stretched to the full column width and the right border is clipped by 1–2 px |
| images (local PNG) | yes: size from the PNG/GIF header at layout time, WIC decoding on first draw | yes |
| Cyrillic, CJK, color emoji | yes (DirectWrite fallback, `D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT`, fixed baseline) | yes |
| scrolling | wheel, scrollbar, keys (arrows, PgUp/PgDn, Space, Home/End), Esc closes | Sciter's own |
| large documents | yes (chunk model, unloading of distant chunks) | yes (appending in chunks; the UI stutters on each chunk) |
| syntax highlighting | yes, simple: a custom lexer (`src/common/highlight.cpp`) driven by md4c's `language-xxx` — keywords, strings, comments, numbers, function calls, `$variables` → `<span class="hl-*">` with GitHub colors; c/cpp, csharp, js/ts, rust, python, powershell, json, go, bash | yes, the same lexer |
| selection/copy, search, clicking links | no | selection by the engine, the rest no |
| dark theme | no | no |

Differences from `style.css` (for litehtml — `kExtraCss` in `src/litehtml/main.cpp`, for Sciter — `kExtraCss` in
`src/sciter/main.cpp`): `.task-list-item {list-style:none}` (md4c does not emit `ul.contains-task-list`); litehtml:
inline code without `:not(pre) > code`, `table {display:table}` (no "block tables"), tables via
`border-collapse: separate; border-spacing: 0` with right/bottom borders on cells (with `collapse` litehtml rounds rows
unevenly — 1 px and 2 px lines mixed). md4c leaves a `\n` before `</code></pre>`: a browser swallows it, litehtml and Sciter
draw an extra blank line — it is stripped after conversion.

Screenshots: `shots/<variant>-{medium,small}.png` (`bench.py shot`, 2 s to settle), litehtml details:
`shots/detail-litehtml-medium-scroll800.png` (code, blockquote, table, image), `detail-litehtml-medium-python-3500.png`
(lists, task list, Python highlighting), `detail-litehtml-medium-chunk-seam-5100.png` (seam between two chunks),
`detail-litehtml-large-end.png` (end of large after background layout of 229 chunks).
Visually litehtml is nearly identical to GitHub/WebView2; flaws: an extra ≈ 5 px after inline code before a comma (litehtml
counts the inline element's right padding twice), italic is synthesized by DirectWrite.

## Quick numbers

`python bench/harness/bench.py run litehtml-sciter-litehtml,litehtml-sciter-litehtml-gpu,litehtml-sciter-sciter
--doc small,medium,large --runs 5` → `results/quick-3.json` (final build, with syntax highlighting).
Earlier series from the same session: `quick-2.json` (no highlighting: litehtml 100/121/106, gpu 289/307/318, Sciter
409/416/439 ms) and `quick-1.json` (before the chunk model, at a noisier moment: 117/128/190 and 449/535/612 ms).

| variant | doc | first | median | p90 | min | win | obs | cpu | commitMB | wsMB | distMB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **litehtml-sciter-litehtml** | small | 92.9 | **107.9** | 125.8 | 100.1 | 77.2 | 68.7 | 78 | 80.3 | 39.9 | 1.79 |
| **litehtml-sciter-litehtml** | medium | 113.3 | **119.6** | 141.3 | 112.6 | 77.8 | 69.3 | 125 | 87.1 | 48.0 | 1.79 |
| **litehtml-sciter-litehtml** | large | 119.7 | **120.5** | 136.4 | 113.2 | 79.7 | 67.8 | 125 | 92.4 | 51.9 | 1.79 |
| litehtml-sciter-litehtml-gpu | small | 297.5 | 291.3 | 298.1 | 289.2 | 286.8 | 66.7 | 234 | 74.0 | 52.7 | 1.79 |
| litehtml-sciter-litehtml-gpu | medium | 337.5 | 302.8 | 412.2 | 290.8 | 295.6 | 62.6 | 250 | 85.2 | 58.2 | 1.79 |
| litehtml-sciter-litehtml-gpu | large | 332.1 | 306.2 | 323.4 | 300.6 | 65.9 | 59.5 | 266 | 89.1 | 62.3 | 1.79 |
| litehtml-sciter-sciter | small | 377.3 | 425.6 | 449.6 | 389.4 | 407.2 | 400.6 | 313 | 136.6 | 60.7 | 18.75 |
| litehtml-sciter-sciter | medium | 412.0 | 426.2 | 442.2 | 386.4 | 412.0 | 406.9 | 375 | 137.6 | 61.1 | 18.75 |
| litehtml-sciter-sciter | large | 416.9 | 428.1 | 494.6 | 385.6 | 396.9 | 387.7 | 297 | 139.8 | 64.7 | 18.75 |

After a fresh build, the first launch of any exe takes 0.6–10 s to reach `main` (Defender scans the new binary); the harness
counts the first run separately (`first`), the median is over the remaining five.

### Breakdown by marks (medians from `quick-3.json`, ms from CreateProcess)

```
litehtml medium: main 42.2 | file_read 44.0 | layout_done 71.1 | d2d_ready 75.5 | emoji_warm 85.1 | rasterized 111.4 | content 119.6
                 UI-поток: window_created 55.5 | shown 77.4 | t_window (белый фон, документ ещё не готов) 77.8
litehtml large:  main 42.1 | file_read 45.3 | d2d_ready 72.5 | layout_done 75.6 | emoji_warm 86.2 | rasterized 109.3 | content 120.5
litehtml small:  main 41.7 | file_read 43.4 | layout_done 63.6 | d2d_ready 75.8 | emoji_warm 85.5 | rasterized 92.7 | content 107.9
litehtml-gpu medium: main 40.7 | layout_done 68.5 | shown 69.8 | d2d_target (CreateHwndRenderTarget) 261.8 | content 302.8
sciter medium:   main 40.2 | html_built 40.4 | sciter_api (LoadLibrary+SciterAPI в потоке) 45.5 | app_init 86.9
                 | window_created 308.6 | doc_parsed 317.5 | html_loaded 400.0 | t_window (1-й rAF) 412.0 | shown 419.7 | content 426.2
```

The background part of litehtml (dev variable `FASTMD_LH_STATS=<file>`, screenshot mode):

| document | chunks | height | all laid out after | text layouts | WS at the end |
|---|---|---|---|---|---|
| medium (53 KB) | 4 | 36,078 px | 107–140 ms after start | 755 | 77 MB |
| large (3.7 MB) | 229 | 2,411,640 px | 1.55–2.3 s (4 threads) | 10,192 | 293 MB with unloading (2.5 GB if all documents are kept) |

## Where the time goes

**litehtml (medium ≈ 110–125 ms):**

* ≈ 35–42 ms before `main` — process creation (CreateProcess + Defender + loader). 1.9 MB exe with static CRT;
  d2d1/dwrite/imm32/WIC/ole32 are delay-loaded, so only user32/gdi32/kernel32/dwmapi are loaded at startup.
* Layout of the first 8 KB: md4c < 1 ms, gumbo + CSS + `render` ≈ 20–25 ms (≈ 380 `IDWriteTextLayout` objects for unique
  words), in parallel with window creation (CreateWindow ≈ 15 ms, ShowWindow ≈ 18 ms with `--no-ime`) and D2D initialization.
* **The critical path is rasterizing the first screen in software Direct2D: ≈ 25–35 ms**, of which ≈ 20–25 ms is color
  emoji (there are 6 on the first screen of medium; the same file without emoji takes ≈ 10 ms for the whole screen). Segoe UI Emoji (COLR) in
  the WARP rasterizer costs ≈ 4–5 ms per new glyph.
* BitBlt + EndPaint + DwmFlush ≈ 8–15 ms.

**litehtml-gpu:** all of the above + ≈ 200 ms for `CreateHwndRenderTarget` (the NVIDIA D3D11 device).

**Sciter (≈ 410–440 ms):** `SciterExec(SCITER_APP_INIT)` ≈ 35–50 ms; `SciterCreateWindow` ≈ 200–300 ms (the engine creates
the GPU device itself; once it took 12 ms — could not be reproduced); `SciterLoadHtml` of 4 KB ≈ 70–100 ms (parsing ≈ 8 ms,
the rest is styles and layout; all of medium takes 0.7–1.1 s, tables are the most expensive at ≈ 14 ms per 6-row table); showing and
two frames until rAF ≈ 35–55 ms. Working set ≈ 60 MB, commit ≈ 137 MB, 18.7 MB on disk.

## Further speed-up ideas (litehtml)

1. **Emoji off the critical path**: right after reading the file (the emoji in the first chunk are known before layout)
   rasterize them on 2–4 threads into small bitmaps and draw them with `DrawBitmap` — ≈ −20 ms on medium.
2. **Rasterization without WARP**: `IDWriteBitmapRenderTarget` (GDI DIB) + `TranslateColorGlyphRun`/`DrawPaintGlyphRun`
   for emoji and hand-made AA primitives, as in `cpp-d2d` (there it gave ≈ −70 ms versus WARP) — ≈ −15…25 ms.
3. Split the first screen into 2–3 bands and rasterize them in parallel (pass the drawing context via litehtml's `hdc`
   / thread_local; litehtml's clips come without an `hdc`).
4. A smaller exe: ≈ 0.6 MB is litehtml's encoding tables (`encodings.cpp`), not needed for UTF-8.
5. Background chunks: prefetch neighbors while scrolling (currently an unloaded chunk is recreated synchronously, ≈ 20–40 ms
   per 16 KB — a noticeable "step" when dragging the scrollbar thumb quickly through large); store glyph runs instead of
   `IDWriteTextLayout` (memory of the word cache).
6. A "last frame" cache / resident process — a different category (`-resident`, see `cpp-d2d-resident`).

## Effort estimate for a product on litehtml

The prototype (≈ 1,750 lines of our code) covers display, scrolling, large files and simple code highlighting. For a product:

| task | estimate |
|---|---|
| Finishing the chunk model: prefetch, background re-layout on resize, anchors/link navigation across chunks | 1–1.5 weeks |
| Text selection and copy (hit-testing over `render_item`, our own structures on top of litehtml, selection across chunks) | 1.5–2 weeks |
| In-document search with highlighting (over the markdown/chunk text, scrolling to the match) | 1 week |
| GitHub-grade syntax highlighting (currently a simple lexer; tree-sitter/grammars → `<span class>`), KaTeX/Mermaid — only as background rendering to SVG/PNG | 1–2 weeks (without math/diagrams) |
| Links, table of contents, drag&drop, `.md` association, auto-reload when the file changes | 1 week |
| Dark theme, settings, HiDPI/multi-monitor, printing | 1 week |
| Fixes/patches to litehtml (inline padding, border-collapse, speed) and maintaining the fork | 1–2 weeks + maintenance |
| **Total** | **≈ 2–2.5 months** for one developer to a quality viewer |

I do not recommend Sciter for the product: 0.4–0.6 s to start, 19 MB, a closed engine (source code paid, attribution
in About), non-standard CSS, a non-working graphics layer knob.

## Pitfalls

* **litehtml lays out large documents superlinearly and at a high memory cost.** A single `render()` for the whole document:
  100 KB — 0.16 s, 200 KB — 0.36 s, 400 KB — 0.69 s, 800 KB — 1.8 s, 1.6 MB — 5.6 s, 3.7 MB — did not finish in 30 s;
  memory ≈ 700 bytes per byte of markdown (DOM + computed styles + render tree). `draw()` walks the whole tree even with
  `clip` (on 1.6 MB — ≈ 0.4 s per frame). Hence the chunk model with unloading; chunks are split only at top-level
  headings (`fmd::heading_cut`, outside fenced code): a document without headings stays a single chunk.
* Splitting the md at headings breaks only link reference definitions (`[x]: url`) declared in another chunk.
* Baseline: `SetLineSpacing(UNIFORM, ascent+descent, ascent)` on every format, otherwise fallback fonts
  (emoji, CJK) shift the line. litehtml positions are fractional — borders and fills are snapped to pixels, otherwise 1 px lines
  blur across two rows.
* litehtml calls `text_width` for every word: a "word → `IDWriteTextLayout`" cache with heterogeneous lookup
  (`std::string_view`, no allocations) is needed; with several documents it must be shared and thread-safe.
* In litehtml the root element's background fills the whole canvas — in a stack of documents it must not be drawn.
* litehtml does not draw `<input>` → task-list checkboxes are replaced with markup.
* `ImmDisableIME` saves 30–40 ms for any Win32 window on this machine (TSF initialization on first activation).
* Sciter 6.0.5.0: a window created by the host and driven via `SciterProcND` stays blank — only
  `SciterCreateWindow` + `SCITER_APP_LOOP` works; any `SCITER_SET_GFX_LAYER` → a blank window, yet rAF×2 and `xcall`
  fire — a "frame ready" signal from the page does not prove that anything was drawn (check by eye/screen capture).
  Without `<meta charset>` Sciter reads the page as cp1251; without `window-width/height` it uses 800×600 and lays out
  the document twice.
* Screenshots: other agents open windows on top — screen capture is unreliable; `PrintWindow` (harness) works for these
  windows. The first launch of a freshly built exe takes up to 10 s (Defender).

## Audit (independent review, 2026-09-19)

**Verdict: the comparison is fair, all three variants are fit for the final sequential measurement.** One "hygiene"
fix (emoji warm-up, below); it does not affect the numbers.

### What was checked

| item | result |
|---|---|
| t_content per PROTOCOL §3 (GDI/D2D) | `litehtml`: the first screen is drawn by the worker into a DIB (software D2D), in `WM_PAINT` — `BitBlt` → `EndPaint` → `fmb_content_presented_ex` (`DwmFlush()` inside, then the mark). `-gpu`: `ID2D1HwndRenderTarget::EndDraw` (= Present) → `DwmFlush` → mark. The mark is set only when `g_chunks` is non-empty, i.e. the frame contains a real laid-out document, not just the background |
| t_content per §3 (web engines) | Sciter: rAF → rAF → `Window.this.xcall('fastmd_ready')` → host `DwmFlush` → mark — exactly the protocol procedure |
| "first viewport first" | legitimate: the first chunk is real markdown up to the heading after 8 KB (Sciter — 4 KB), laid out by litehtml in full; if it is shorter than the window it is grown ×4. The first screen matches the snapshot after background layout pixel for pixel |
| resident / caches / pre-rendering | none: no cache files, no registry, no named objects, no thread priorities; all caches (fonts, words) live inside the process. Nothing from the corpus is embedded at build time except `bench/common/style.css` (as intended) |
| build | `CMAKE_BUILD_TYPE=Release`, `/O2 /Ob2 /GL /Oi /Gy /GS- /MT`, linking `/LTCG /INCREMENTAL:NO`, delay-load of d2d1/dwrite/WIC/ole32/imm32 (checked in `obj/build.ninja`); `build.ps1` rebuilds from scratch in ≈ 20 s — rebuilt, everything compiled |
| `bench.py check` medium + large, all variants | CHECK OK, no warnings; `observed_ms` ≤ `window_ms` ≤ `content_ms` everywhere (litehtml: obs 57–63, win 66–70, content 107–110 ms) |

**Independent pixel check** (a custom script outside the repository: polling the window contents via
`PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT)` with timestamps, compared against the settled frame):
for `litehtml` the last blank frame falls at t_content − 9…33 ms, the first frame with the document at
t_content + 15…46 ms (a single capture takes 20–50 ms), i.e. t_content lies inside the transition window — **the mark is not early**.
`-gpu` — the same (the document is visible 10–20 ms after t_content). For Sciter `PrintWindow` blocks while the UI thread
is loading the page (≈ 170 ms) and itself skews the measurement — the pixels could not be confirmed precisely, but there is no contradiction either
(in two runs the document was already in the window before the mark). A screen capture through the desktop DC is
unsuitable for this purpose: the Windows window-open animation (≈ 150–250 ms of "fade-in") and other agents' windows on top.

### Fixed

* **The color emoji warm-up drew exactly the glyph that appears on the corpus's first screen.** `gfx_worker` drew
  U+1F680 🚀 into the same software render target that is later used to draw the document; 🚀 is the most frequent emoji in the corpus
  (small ×1, medium ×41, large ×2890) and appears on the first screen of all three files, i.e. its rasterization went into
  the glyph cache in advance. That is a corpus dependency "baked" into the code. Replaced it with U+1F600 😀 — which is not in the corpus (the corpus
  only has U+1F680, U+2728, U+1F4C4, U+2705, U+274C, U+26A1). The general COLR path warm-up remains.
  Effect, interleaved A/B, n = 10, t_content medians: old exe (🚀) medium 119.2 / small 104.2 ms,
  new (😀) 114.2 / 102.4 ms, new with `--no-warm` 113.9 / 96.6 ms — all within noise, **the "guessed"
  glyph gave no gain**, the fix is hygiene. File: `src/litehtml/main.cpp` (`gfx_worker`). Rebuilt with `build.ps1`.

### Worth knowing for the final comparison

* `--no-ime` (`ImmDisableIME(-1)`) is a legitimate technique, but it is an application choice, not a stack property: A/B n = 8 (medium) —
  `ShowWindow` 18.6 vs 39.6 ms, t_window 65 vs 92 ms, but **t_content only −8.6 ms** (105.7 vs 114.3),
  because the critical path is rasterization on the worker. A product with a search box and CJK input will have to bring IME back
  (or create the search window on another thread).
* `-gpu`: if the document is ready before the first `WM_PAINT`, t_window ≈ t_content (the window is visible from ≈ 60 ms, but until the
  hardware target is created, ≈ 200 ms, nothing is drawn in it). By the definition of t_window this is fair; for the user it is
  ≈ 200 ms of an unpainted window.
* `litehtml` large = medium in time (≈ 110 ms): before the first frame only the first chunk of ≈ 8.7 KB is processed,
  the 3.7 MB are merely read from disk (≈ 3 ms). This is an allowed technique; the whole layout of large completes in the background within 1.5–2.3 s,
  and the large document scrolls (snapshot `shots/audit-litehtml-sciter-litehtml-large.png`).
* Sciter: known layout flaws (table spanning the full column width with a clipped right border, inline code without
  padding/rounding, gray native checkboxes, synthetic italic) confirmed from the snapshots.

### Visual assessment (snapshots `shots/audit-*.png`, 2 s to settle)

| variant | score 1–10 (10 = GitHub) | remarks |
|---|---|---|
| `litehtml-sciter-litehtml` / `-gpu` | **9** | nearly indistinguishable from GitHub: h1–h6 with rules and gray h6, lists/nesting/task checkboxes (blue), blockquote, hr, code with highlighting and `#f6f8fa` background, content-sized table with zebra stripes (pixels checked: even rows `#f6f8fa`), column alignment, PNG, Cyrillic/CJK/color emoji, links `#0969da`. Minuses: ≈ 5 px extra after inline code, long code lines are clipped, no text selection |
| `litehtml-sciter-sciter` | **7** | all content is there, but the table is stretched and lacks the right border, inline code has no background pill, native gray checkboxes, synthetic italic |

### Auditor's sanity run (noisy, shared machine)

`python bench/harness/bench.py run litehtml-sciter-litehtml,litehtml-sciter-litehtml-gpu,litehtml-sciter-sciter
--doc small,medium --runs 5 --out bench/protos/litehtml-sciter/results/audit-run.json`

| variant | doc | first | median | p90 | min | win | obs | cpu | commitMB | wsMB | distMB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| litehtml-sciter-litehtml | small | 94.4 | **101.3** | 105.2 | 96.1 | 69.6 | 62.3 | 94 | 79.0 | 39.6 | 1.79 |
| litehtml-sciter-litehtml | medium | 108.3 | **112.4** | 125.7 | 107.7 | 70.1 | 62.7 | 109 | 88.0 | 48.0 | 1.79 |
| litehtml-sciter-litehtml-gpu | small | 277.8 | 277.7 | 305.8 | 262.3 | 274.8 | 62.8 | 250 | 73.7 | 52.6 | 1.79 |
| litehtml-sciter-litehtml-gpu | medium | 289.9 | 289.9 | 305.9 | 287.1 | 283.1 | 60.5 | 281 | 84.3 | 58.1 | 1.79 |
| litehtml-sciter-sciter | small | 493.2 | 390.0 | 401.0 | 365.2 | 373.4 | 367.5 | 297 | 137.6 | 60.5 | 18.75 |
| litehtml-sciter-sciter | medium | 371.4 | 419.6 | 434.6 | 348.7 | 408.8 | 402.5 | 297 | 136.0 | 61.0 | 18.75 |

Separately, `check` on large: litehtml 109.8 ms, `-gpu` 316 ms, Sciter 342 ms. The builder's numbers (`quick-3.json`)
are confirmed within noise.
