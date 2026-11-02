[Русский](README.md) | **English**

# blitz — FastMD on DioxusLabs Blitz (Stylo + Taffy + Parley + Vello)

A "real CSS without Chromium" prototype: Markdown → HTML (comrak) → DOM (html5ever) → CSS (Stylo, the Firefox
engine) → layout (Taffy) → text (Parley/fontique/HarfRust) → raster (vello_cpu) → window (winit + softbuffer).
It packages the `research/lab/blitz` study (the `rdme` app, see `research/02-web-and-html-engines.md` §10)
into a prototype on the shared harness, with its own startup pipeline.

## Summary

| Variant | small | medium | large | WS at t_content | dist |
|---|---|---|---|---|---|
| **`blitz`** (CPU raster, first screen as a chunk) | **110** | **121** | **116** | 33–50 MB | 8.0 MB, 1 file |
| `blitz-full` (whole document before the first frame) | 114 | 121 | 2 654 | 33 MB … 776 MB | 〃 |
| `blitz-gpu` (vello_hybrid, wgpu DX12, RTX 5070) | 770 | 789 | 785 | 99–118 MB | 10.5 MB |
| for comparison: `baseline-win32` (bare window + GDI) | 75 | 76 | 74 | 14 MB | 0.15 MB |

t_content, ms, median of 5 runs after the first one (`bench.py run … --runs 5`, shared machine, see "Quick measurements").

- The original `rdme` from the study took 375–564 ms (CPU) and 1.2–1.5 s (GPU). Now it is **~110–120 ms**, i.e.
  **+35–45 ms over the calibration floor** (bare Win32 window). Of this difference, ~18 ms is Defender checking the 8 MB
  exe during `CreateProcess`; the rest is winit and the first frame.
- Three main findings, without which nothing else helped:
  1. **On Windows, winit builds its keyboard layout cache synchronously inside the first `ShowWindow`**
     (≈2 450 `ToUnicodeEx` calls, **150–350 ms** on this machine). Fixed with the `vendor/winit-win32` patch.
  2. **Defender takes ~2.2 ms per MB of exe on every launch.** Verified experimentally: +16 MB of zeros at the end of
     `baseline.exe` add +35 ms. Hence fat LTO + `opt-level="s"`: 17.7 → 8.0 MB, launch −25 ms, t_content
     −25…−40 ms (paired runs).
  3. **A pipeline across threads.** The UI thread is busy only with the window; everything else runs in parallel: DirectWrite fonts, parsing,
     DOM and styles/layout of the first screen. The document is ready ~30 ms before the window is ready to accept it.
- Large document: the first screen is the first ~12 KB of markdown (the same document, not a placeholder). Then, in the background,
  256 KB is built (ready after ~0.2–0.3 s), then the full document (after ~2.5–4 s). Each stage
  is swapped in with the scroll position preserved. The bottleneck is not startup but **the frame cost on a huge DOM**:
  3.7 MB of markdown gives ~100–140 ms per frame, so scrolling "large" stutters (see "Pitfalls").
- Layout quality: this is real CSS (`bench/common/style.css` + 3 workaround rules, see below), so
  the look matches the spec almost one to one. Color emoji, CJK, Cyrillic, tables with
  alignment and "zebra" rows, images. No syntax highlighting.

Screenshots: `shots/blitz-medium.png`, `shots/blitz-small.png`, `shots/blitz-large.png`, `shots/blitz-gpu-medium.png`,
`shots/blitz-full-medium.png`.

## Stack and versions

| Component | Version |
|---|---|
| Rust | 1.98.1 stable, `x86_64-pc-windows-msvc`, MSVC 14.44 |
| Blitz (`blitz-dom`, `blitz-html`, `blitz-paint`, `blitz-shell`, `blitz-traits`) | 0.3.0-beta.2, github.com/DioxusLabs/blitz @ `e7bf7bca` (checkout `research/lab/blitz/src`, packages unchanged) |
| Stylo (CSS) | `stylo` 0.21.0 |
| Taffy (layout) | 0.14.0, git rev `4863877b` |
| Parley / fontique / HarfRust / skrifa (text) | 0.11.1 / 0.11.1 / 0.12.0 / 0.44.0 |
| html5ever | 0.39.0 |
| comrak (Markdown → HTML, GFM) | 0.52.0 |
| CPU raster | `anyrender_vello_cpu` 0.17.0 (`vello_cpu` 0.1.0, 8 threads) + `softbuffer` 0.4.8 (GDI) |
| GPU raster | `anyrender_vello_hybrid` 0.10.0 (`vello_hybrid` 0.1.0) + `wgpu` 29.0.4 |
| Window | `winit` 0.31.0-beta.3, **`winit-win32` 0.31.0-beta.3 with a patch** (`vendor/winit-win32`) |
| Images | `image` 0.25.10 (png, jpeg) |

Networking (`blitz-net`: reqwest + tokio + native-tls) is dropped: it is replaced by a 40-line custom `FileNet`
(`app/src/net.rs`). It reads only `file:` URLs and never touches the internet. Clicks on http(s)/mailto go
to `ShellExecuteW`.

## How to build

```
pwsh -File bench/protos/blitz/build.ps1          # ~4 мин инкрементально, ~8–10 мин с нуля
pwsh -File bench/protos/blitz/build.ps1 -Clean   # cargo clean + полная сборка
```

- Requires Rust stable (msvc) and MSVC Build Tools. Crates come from crates.io per `app/Cargo.lock` (`--locked`).
  Blitz is wired in via path dependencies to `research/lab/blitz/src`. If the checkout is missing, `build.ps1` clones
  DioxusLabs/blitz at commit `e7bf7bca` into that location.
- Target dir: `bench/.tools/blitz/target` (~3 GB, outside the prototype directory).
- `dist` profile: fat LTO, 1 CGU, `panic=abort`, `opt-level="s"`, while the hot crates (`vello_cpu`,
  `vello_common`, `fearless_simd`, `taffy`, `harfrust`, `skrifa`) are built with `opt-level=3`, as in Blitz's own `tiny`
  profile. In `app/.cargo/config.toml`: static CRT and `/DELAYLOAD` for shell32/ole32/oleaut32/dwrite.
- Output: `out/cpu/fastmd-blitz.exe` (variants `blitz`, `blitz-full`) and `out/gpu/fastmd-blitz-gpu.exe`
  (`blitz-gpu`). Each is a single file with no DLLs; nothing else is needed next to it.

Layout:

```
app/Cargo.toml, Cargo.lock, .cargo/config.toml
app/src/main.rs     конвейер запуска, приложение поверх blitz-shell, клавиатурная прокрутка
app/src/md.rs       comrak (GFM) -> HTML, обёртка страницы + style.css, нарезка первого экрана
app/src/net.rs      FileNet (картинки с диска), ExternalLinks (ShellExecuteW)
app/src/fmb.rs      протокол bench (метки, DwmFlush, ws_bytes, атомарная запись результата)
app/src/bin/showprobe.rs   проба стоимости ShowWindow в winit (feature "probe", в dist не входит)
vendor/winit-win32  winit-win32 0.31.0-beta.3 + патч (см. «Грабли»)
experiments/        сырые json замеров опытов, devlog-*.txt, showprobe.log, marks.py, pad_exe.py
results/quick-run.json   финальный быстрый прогон
shots/              скриншоты (bench.py shot)
```

## How startup works

```
t=0 CreateProcess ─ ~40 мс (из них ~18 мс Defender на 8 МБ exe) ─ main (~50 мс)
UI-поток:   EventLoop (17) ─ CreateWindowExW, скрытое, с пустым документом-заглушкой (15)
            ─ ShowWindow + активация (21) ─ забрать готовый документ (0 мс ожидания)
            ─ softbuffer resume (2) ─ первый кадр: paint_scene + vello_cpu + GDI present (9.5) ─ DwmFlush (5.6) = t_content
поток font: DirectWrite: коллекция системных шрифтов fontique + семейства из style.css (~13 мс)
поток doc:  чтение .md (1) ─ первые ~12 КБ до пустой строки вне ```-блока ─ comrak (1) ─ ждёт шрифты
            ─ DOM html5ever (2) ─ Stylo + Taffy + Parley для предсказанного вьюпорта (9) ─ готов на ~74 мс
поток rest: comrak → DOM → стили/раскладка сначала 256 КБ, потом всего документа → подмена (прокрутка сохраняется)
после кадра: фоновый прогрев кэша раскладки клавиатуры winit (патч), чтобы не тормозило первое нажатие
```

The numbers in parentheses are medians for medium from `results/quick-run.json`.

- The document is built and laid out outside the UI thread and then handed over to it. `HtmlDocument` is not `Send` only because of
  two trait objects without `Send` (`FontMetricsProvider` in Stylo, `HtmlParserProvider` in Blitz). The
  `SendDoc` wrapper with `unsafe impl Send` and a justification in `main.rs`: the types behind them are thread-safe, and ownership
  is transferred exactly once.
- The viewport for layout is predicted in advance from `GetDpiForSystem` (1000×800 DIP). If the window opens on a
  monitor with a different DPI, `View::init` sets the real one and Blitz re-lays out.
- `View::init` / `resume` from blitz-shell are called by hand (as in `BlitzApplication::can_create_surfaces`,
  but step by step): this makes it possible to place marks and slip in the ready document via `replace_document`.
- Protocol (`fmb.rs`): `t_content` is taken after the `present` of the first frame with the document (softbuffer:
  `SetDIBitsToDevice`/`BitBlt`; GPU: wgpu present) and `DwmFlush`. The frame is caught in `ResumeReady`→`complete_resume`
  or in `RedrawRequested`, whichever comes first, and only with an active renderer. `t_window` is the same
  present: before it the window draws nothing. `ws_bytes` = `K32GetProcessMemoryInfo`. Exit is `exit(0)` right
  after the atomic write of the json.
- Honesty: no resident processes and no caches. The font collection is built on every start, the corpus
  is read from disk, the first screen is the real beginning of the document, rendered by the same engine. If you
  scroll to the end of the chunk before the swap, you'll see "…" there.

## Variants (`proto.json`)

| id | exe / arguments | What it is |
|---|---|---|
| `blitz` | `out/cpu/fastmd-blitz.exe --variant blitz` | Best configuration: CPU raster, early window show, first screen as a 12 KB chunk, stages 256 KB → whole document in the background, dist profile |
| `blitz-full` | same exe, `--full` | Whole document before the first frame, no chunks. For documents >256 KB, styles are computed in parallel (Stylo `Parallel`) |
| `blitz-gpu` | `out/gpu/fastmd-blitz-gpu.exe`, env `WGPU_BACKEND=dx12`, `WGPU_POWER_PREF=high` | vello_hybrid on wgpu, same pipeline |

Command-line switches (for experiments; the defaults are the best values): `--chunk <bytes>` (12288), `--stage <bytes>`
(262144, `0` = no intermediate stage), `--full`, `--late-window` (window after the document is ready),
`--kbd-prewarm start|after|off` (after), `--dnd` (winit OLE drag&drop, off), `--bg-seq` (background document without
parallel Stylo), `--scroll-y <px>` (only for inspecting via screenshots). The `FASTMD_BLITZ_LOG=<file>` variable
writes the timings of background stages and of frames after `t_content` to a file. It plays no part in the protocol.

## What is supported (vs PROTOCOL §5)

| Element | Status |
|---|---|
| Window | PMv2 DPI (API before anything else), 1000×800 DIP client area, light theme (forced), title `<file> — FastMD (<variant>)`, activated |
| Headings h1–h6 | yes, sizes and semibold per the spec, h1/h2 have a bottom rule, h6 is gray |
| Paragraphs, word wrap | yes (Parley, ICU line breaking) |
| **bold** / *italic* / `code` / ~~strike~~ / links | yes. Italic is set in Segoe UI (Segoe UI Variable has no italic, see "Pitfalls") |
| Lists: bullet, ordered, nested, tasks | yes. Task checkboxes are blue. They are not `disabled`, so a click apparently toggles the check mark (on screen only, the file is not changed; not checked by hand) |
| Blockquotes, horizontal rule | yes |
| Code blocks | yes: Cascadia Mono 13.5 px, background `#f6f8fa`, radius 6, padding 16, **no wrap** (`white-space:pre`, `overflow:auto`; horizontal scrolling of long lines inside the block not checked by hand) |
| GFM tables | yes: borders, semibold header, "zebra" rows, column alignment. `display:table` instead of GitHub's `display:block`, so a wide table does not scroll horizontally |
| Images (local PNG/JPEG) | yes, asynchronously via `FileNet`: they may appear a frame later, with a re-layout |
| Syntax highlighting | **no** |
| Unicode | Cyrillic, CJK (fallback via DirectWrite), color emoji (Segoe UI Emoji) — yes |
| Scrolling | wheel, overlay scrollbar (Blitz). PageUp/PageDown/Space/arrows/Home/End keys — custom handler, Blitz doesn't have this |
| Zoom | Ctrl+= / Ctrl+− / Ctrl+0 (blitz-shell) |
| Links | http(s)/mailto open in the default browser. `#anchors` and relative .md links are not handled |
| Text selection and copy | Blitz has it (`clipboard` enabled), not checked in detail |
| Dark theme, search, printing, TOC | no |

Rules on top of `style.css` (`BLITZ_CSS` in `app/src/md.rs`, all of them work around Blitz quirks):
`table{display:table;width:auto}`, `em,i{font-family:"Segoe UI"…}`, color and offset of task checkboxes.
`disabled=""` on checkboxes and the trailing `\n` in `<pre><code>` are stripped from comrak's HTML.

## Quick measurements

`python bench/harness/bench.py run blitz,blitz-full,blitz-gpu,baseline-win32 --doc small,medium,large --runs 5
--out bench/protos/blitz/results/quick-run.json`. Median of 5 runs (the first one discarded), interleaved across variants.
Shared machine with other agents running in parallel, so the numbers are approximate.

| variant | doc | median | min | p90 | t_window | observed | CPU ms | commit MB | WS MB |
|---|---|---|---|---|---|---|---|---|---|
| blitz | small | **110.2** | 109.0 | 118.6 | 106.3 | 79.6 | 78 | 15.5 | 32.9 |
| blitz | medium | **120.5** | 115.1 | 124.2 | 114.9 | 84.2 | 109 | 28.6 | 45.4 |
| blitz | large | **115.6** | 110.8 | 129.3 | 108.7 | 80.8 | 94 | 33.8 | 49.7 |
| blitz-full | small | 113.8 | 107.4 | 118.1 | 110.3 | 83.3 | 78 | 15.6 | 33.0 |
| blitz-full | medium | 120.7 | 114.4 | 150.2 | 116.1 | 81.0 | 125 | 27.8 | 45.3 |
| blitz-full | large | 2 653.7 | 2 598.5 | 2 723.4 | 2 649.8 | 81.8 | 3 563 | 808.0 | 775.7 |
| blitz-gpu | small | 770.3 | 764.2 | 801.5 | 762.7 | 105.2 | 656 | 221.1 | 98.5 |
| blitz-gpu | medium | 788.6 | 769.4 | 798.1 | 779.5 | 106.5 | 672 | 236.0 | 113.2 |
| blitz-gpu | large | 784.9 | 771.0 | 816.0 | 777.7 | 106.7 | 703 | 240.6 | 117.6 |
| baseline-win32 | small / medium / large | 75.1 / 75.8 / 74.4 | | | | ~50 | 47 | 3.1 | 14.0 |

`observed` is the moment the harness saw a visible window (earlier than `t_content`: the window is shown before
the frame is drawn). `bench.py check` passes without warnings for all 9 variant/document pairs.

### Breakdown by marks (medians, ms from `t0` = before `CreateProcessW`)

| mark | small | medium | large | blitz-full large | blitz-gpu medium | what it is |
|---|---|---|---|---|---|---|
| launch (harness) | 38.5 | 39.8 | 39.0 | 37.9 | 44.9 | `CreateProcess`→`ResumeThread` (Defender) |
| `main` | 48.9 | 49.7 | 48.9 | 48.3 | 57.0 | first line of our code |
| `file_read` | 49.9 | 50.9 | 52.6 | 51.5 | 57.7 | doc thread |
| `parsed` | 50.2 | 51.8 | 53.2 | 221.0 | 58.4 | comrak for the first screen (full: whole document) |
| `fonts_ready` | 62.7 | 63.0 | 63.2 | 221.0 | 70.3 | DirectWrite font collection (font thread) |
| `dom_built` | 64.3 | 64.8 | 65.6 | 503.7 | 72.4 | html5ever DOM |
| `resolved` | 68.8 | 74.0 | 73.8 | 2 362.8 | 80.8 | Stylo + Taffy + Parley |
| `event_loop` | 64.3 | 66.9 | 66.8 | 64.0 | 72.9 | UI: winit EventLoop (for GPU, after `wgpu::Instance`) |
| `run_app` | 64.6 | 67.1 | 67.0 | 64.3 | 90.9 | entering `can_create_surfaces` |
| `window_created` | 77.4 | 82.2 | 79.0 | 79.6 | 104.4 | CreateWindowExW (hidden) |
| `window_shown` | 100.0 | 103.0 | 100.0 | 105.2 | 125.6 | ShowWindow + activation |
| `doc_received` | 100.0 | 103.0 | 100.0 | 2 362.9 | 125.6 | document handed to the UI thread |
| `window_ready` | 100.9 | 105.4 | 101.2 | 2 540.8 | 768.1 | resolve (no-op) + renderer resume (GPU: wgpu device + pipelines) |
| `presented` | 106.3 | 114.9 | 108.7 | 2 649.8 | 779.5 | present of the first frame |
| **t_content** | **110.2** | **120.5** | **115.6** | **2 653.7** | **788.6** | + DwmFlush |

Background work for large (variant `blitz`, `FASTMD_BLITZ_LOG`, files `experiments/devlog-*.txt`). The 256 KB stage:
comrak 10–22 ms, DOM 20–27 ms, styles+layout 134–204 ms, ready **200–330 ms** after `main`. Full
3.7 MB document: comrak+HTML 0.17–0.22 s, DOM 0.29–0.40 s, styles+layout 1.8–3.4 s (parallel Stylo),
ready **2.5–4.3 s** after `main`. The swap on the UI thread takes 1–30 ms.

## Where the time goes

For medium, ~120 ms vs 76 for the bare window:

1. **~40 ms until `ResumeThread`** (baseline: 22). The difference is almost entirely due to exe size: Defender checks
   the file on every `CreateProcess`. Experiment `experiments/pad_exe.py` (baseline + 16 MB of zeros in an overlay that is not even
   mapped into memory): launch 26 → 62 ms, t_content 98 → 126 ms. Hence ~2.2 ms/MB, which is ~18 ms for 8 MB.
2. **~10 ms loader/CRT** before `main` (static CRT and delay-load gave no noticeable gain, see the experiments).
3. **UI thread, ~53 ms of pure winit/OS.** EventLoop 17 ms: the thread's first GUI initialization, a hidden
   receiver window, TSF/IME, raw input. Window creation 15 ms. ShowWindow with activation 21 ms: TSF, DWM; a bare
   window activates in 30–45 ms according to the `rust-webview` probes. All of this is sequential and depends only on the window.
   For `baseline-win32` the path from `main` to `t_content` is ~43 ms, for us ~71 ms.
4. **First frame ~9.5 ms**: `complete_resume` (a new vello_cpu `RenderContext`, an 8-thread pool), `paint_scene`,
   1000×800 raster, RGBA→BGRX byte swizzle and `SetDIBitsToDevice`. Then **DwmFlush ~5.6 ms**, which is a
   protocol requirement.
5. **The engine is not on the critical path.** Fonts (13 ms), comrak (1 ms), DOM (2 ms), styles and layout of the first
   screen (9 ms) are computed on other threads and are ready at ~74 ms, 29 ms before the window is ready.
   That is why `blitz-full` is no slower than `blitz` on medium: even the whole document (54 KB, ~25 ms) makes it in time. Chunks
   are needed for large files.
6. **GPU**: `wgpu::Instance` with all backends ~315 ms (Vulkan/GL ICDs of both graphics cards get loaded). With
   `WGPU_BACKEND=dx12` ~20 ms. After that, ~640–700 ms go to the D3D12 device and vello_hybrid pipelines/shaders inside
   `resume`. For startup, the GPU raster loses by 6–7×.

### What was tried (paired comparisons from a single interleaved run, medium)

| Experiment | Result | File |
|---|---|---|
| First version: stock winit, everything sequential on the UI thread, release 17.7 MB | t_content 246–350 ms, `View::init` 135–225 ms | — (check runs) |
| Splitting `View::init`: window creation / `set_visible(true)` | creation 25–100 ms, **show 185–470 ms** | — |
| `showprobe`: winit hidden → `set_visible(true)` / `with_active(false)` | 115–260 ms vs 7 ms without activation | `showprobe.log` |
| 2 448 × `MapVirtualKeyExW`+`ToUnicodeEx`, as in `prepare_layout` | **153–352 ms** (60–140 µs per call) | `showprobe.log` |
| Patching only `get_agnostic_mods` | didn't help: `synthesize_kbd_state` also builds the cache on `WM_SETFOCUS` | `showprobe.log` |
| Patching both places (v2) | show with activation **30–40 ms** (like a bare window), t_content 155–195 ms | `showprobe.log` |
| Early window with a placeholder document vs late window | 168 vs 173 ms (within noise), early kept | `r1.json` |
| Keyboard layout prewarm at startup vs after the frame | 164.5 vs 168.1 (noise), "after the frame" kept (doesn't compete) | `r1.json` |
| baseline + 16 MB overlay | launch 26 → 62 ms, t_content 98 → 126 | `r2.json` |
| release 17.7 MB vs fat LTO 10.1 MB | launch 68 → 49, t_content 178 → 150 | `r2.json` |
| fat LTO `opt-level=3` vs `"s"` (8.1 MB) | launch 47.6 → 42.3, t_content 165.7 → 150.5 (small: 147.6 → 137.8) | `r3.json` |
| + static CRT + delay-load (dist) vs without | 117.5 vs 114.8, no difference (kept, doesn't hurt the product) | `r4.json` |
| vello_cpu multi-threaded vs single-threaded | first frame 9.3 vs 7.3 ms, t_content 141.9 vs 142.1: no difference | `r6-st.json` |
| GPU: all backends / DX12 / DX12+high / DX12+low (iGPU) | 1 004 / 870 / **851** / 1 051 ms | `r5-gpu.json` |
| opt-s vs opt-3 on large background layout | 2.6–3.4 s vs 2.35–3.3 s (noise), not noticeably slower | `devlog-*.txt` |

## Ideas for further speedup

- **A custom Win32 host instead of winit.** blitz-shell is thin; what's needed is a window, a softbuffer surface and translation of
  mouse/keyboard/wheel into Blitz events, ~1.5–2 KLOC. A bare window takes ~43 ms from `main` to `t_content`, we take
  ~71 ms, of which ~15 ms is frame+DwmFlush and ~50 ms is winit/OS. I expect −10…−20 ms.
- **Code signing and reputation.** May remove part of Defender's check on every launch (can't be verified here).
  Compressing the exe (UPX) won't help: AV unpacks it, plus there is the unpacking at startup.
- Shrink the exe further: no jpeg, pulldown-cmark instead of comrak, dropping `image`. On the order of −1…−2 MB, i.e. −2…−4 ms.
- Don't recreate the vello_cpu `RenderContext` in `resume` and `complete_resume` (in `anyrender_vello_cpu` this is
  a new thread pool every time). About −1…−2 ms.
- **Large documents:** virtualization, i.e. keep only the neighborhood of the viewport in the DOM (sections by headings)
  and append via a DOM mutator instead of a full rebuild. Currently Blitz walks the whole tree on every frame.
- GPU only after the first frame: the first frame with the CPU raster, while the `wgpu` device and vello_hybrid pipelines warm up
  in the background and are picked up for smooth scrolling at 4K (like `rust-gpu-egui-softfirst`). Requires a fork of
  `anyrender_vello_hybrid` (currently there's no API to hand it a ready `WGPUContext`/device).
- A resident mode (`-resident`) would remove ~50 ms of process and EventLoop, but it is a different class of solution.

## Effort estimate to a product

An MVP reader on this stack: **5–7 weeks** for one developer, plus ongoing maintenance of the forks.

- 1 week: `.md` association, single instance, drag&drop, reload on file change (`notify`),
  anchors and relative links, settings, installer.
- 1–1.5 weeks: syntax highlighting (syntect/tree-sitter → classes in HTML), dark theme (CSS), in-page
  search (Blitz has no search API: a custom pass over the DOM text plus highlighting), checking selection and copy,
  printing/PDF (Blitz has no printing, a custom export is needed).
- 1–2 weeks: virtualization of large documents (see above). Without it, files >1 MB scroll in jerks.
- 1 week: a custom Win32 host or maintaining the winit patch, accessibility (Blitz has AccessKit, it is disabled
  in the prototype).
- Risks: Blitz is in beta (the API changes from commit to commit, pin the revision), three local workarounds
  (`SendDoc`, the winit patch, CSS workarounds). There is no JS, so Mermaid/KaTeX only by native means or via
  MathML/SVG. Memory on huge documents: ~0.8 GB WS for 3.7 MB of markdown.

## Pitfalls

1. **winit (and tao) on Windows: 150–350 ms in the first `ShowWindow`.** On `WM_SETFOCUS`, the calls are
   `update_modifiers` → `LAYOUT_CACHE.get_agnostic_mods()` and `synthesize_kbd_state` →
   `get_current_layout()`. Both build the layout cache: 16 modifier states × 256 VKs via `ToUnicodeEx`.
   On this machine (ru+en) a call costs 60–140 µs. The `vendor/winit-win32` patch (4 places, marked
   `FastMD patch`):
   - `get_agnostic_mods` builds the cache only if Right Alt is pressed;
   - `synthesize_kbd_state` gets the HKL via `GetKeyboardLayout(0)`;
   - `WM_INPUTLANGCHANGE` warms the cache in the background;
   - a public `prewarm_keyboard_layout(thread_id)` is called in the background after the first frame.
   The `rust-webview` prototype found the same thing for tao.
2. **Exe size = launch time** (Defender, ~2.2 ms/MB, see the overlay experiment). Hence fat LTO + opt-s.
   An LTO build takes ~2.5 min per variant.
3. **`HtmlDocument` is not `Send`**, although it is in fact thread-safe. A `SendDoc` wrapper (`unsafe impl Send`) is needed.
   The proper fix is `Send` bounds on `FontMetricsProvider`/`HtmlParserProvider` upstream.
4. **`DocumentConfig.ua_stylesheets` replaces the default UA stylesheet**, while `HtmlDocument::from_html` appends
   `DEFAULT_CSS` at the end, and it overrides rules of the same specificity. That is why `style.css` is inserted as
   an author `<style>` in `<head>`, as the protocol requires.
5. **`display:block` on the table** (GitHub style in `style.css`): Blitz doesn't build the anonymous table wrapper and
   dumps all rows into one. `display:table` is needed; upstream `rdme` does the same.
6. **Segoe UI Variable Text has no italic**, and Parley/Vello's synthetic slant produces broken glyphs
   ("ıtalıc"). Italic is set in Segoe UI.
7. Checkboxes: with `disabled` the color is hard-coded light gray, and `vertical-align:middle` is ignored. So
   `disabled` is removed, and a `color` and a downward offset are set.
8. blitz-shell draws the first frame in `complete_resume` on the `ResumeReady` event, not in `RedrawRequested`.
   The "first `RedrawRequested`" mark in the research `fmb.rs` could have fired on a different frame. Here
   both paths are checked, and only with an active renderer.
9. Stylo's parallel traversal can't be run for two documents at once (blitz #430). That is why
   `Parallel` is used only for the final background document (and for `--full` on large files), while the first screen and the
   intermediate stage are `Sequential`.
10. Blitz has no keyboard scrolling, so we had to write our own. The sign of `scroll_viewport_by` is the same as for the wheel:
    positive means up.
11. **Frame cost grows with DOM size**: medium ~8 ms, the 256 KB stage ~11–13 ms, full large ~100–140 ms
    (resolve walks the tree for ~99 ms even with no changes, paint ~40 ms; `experiments/devlog-frames.txt`).
12. GPU: without `WGPU_BACKEND=dx12`, wgpu brings up the Vulkan/GL drivers of both graphics cards (~300 ms).
    `WGPU_POWER_PREF=low` (AMD iGPU) is ~200 ms slower than the RTX.
13. Shared-machine noise: single launches wander by ±30–50 ms, the first launch of a new exe costs +250–350 ms (Defender
    scans it for the first time). Compare only with interleaved runs.

## Audit (independent review, 2026-09-19)

**Verdict: the prototype is honest; no fixes were needed.** The code, `proto.json` and `build.ps1` were not changed. All three
variants (`blitz`, `blitz-full`, `blitz-gpu`) are fit for the final sequential measurement.

### What was checked

| Item | Result |
|---|---|
| Where `t_content` is taken | `fmb::content_presented()` is called from `App::check_presented` after `ResumeReady`→`complete_resume` or after `RedrawRequested`, only when `renderer.is_active()`. The real document is slipped in by `replace_document` **before** `view.resume()`, and the renderer is inactive before `resume`. So the placeholder is never drawn, and the first frame already contains the document. Verified against the sources: `blitz-shell/src/window.rs` (`complete_resume` draws a frame) and `softbuffer_window_renderer-0.8.0` (`render` → `surface_buffer.present()`, a synchronous GDI blit). For GPU: `anyrender_vello_hybrid-0.10.0`, `render` → `maybe_blit_and_present` + `device.poll(wait)`, the native init is blocking (`pollster::block_on`) |
| DwmFlush | called right before taking `t_content`, in the process that owns the window (`fmb.rs`) |
| What is in the first frame | The real beginning of the document (the first ~12 KB of markdown, swapped later), laid out by the same engine and CSS. This is the allowed "first screen first" technique (§3). Checked with a screenshot of large with `--stage 0` and a 200 ms settle: the window still shows the first chunk (the full document is ready after 2.5–4 s), and it is fully styled (`shots/audit-blitz-large-firstframe.png`). There are no images in the first viewport of medium/large; they load asynchronously |
| Resident / cache / pre-render | None. The DirectWrite font collection is built on every launch, the `.md` is read from disk, there are no disk caches. `include_str!` is used only for `bench/common/style.css`; nothing from the corpus is baked into the exe (grep over `app/src`) |
| Build | `pwsh -File build.ps1` rebuilt by the auditor: exit 0, `dist` in 1 min 47 s (CPU) and 2 min 19 s (GPU), incrementally. The sizes matched the previous ones byte for byte: 8 384 000 and 11 019 264 bytes; the hashes differ because of PE timestamps. `dist` profile: fat LTO, opt-s + opt-3 for hot crates, `panic=abort`, static CRT, vs 18.0 MB for `release` |
| winit-win32 patch | An engineering fix, not a protocol workaround: the keyboard layout cache is built lazily or in the background rather than in the first `WM_SETFOCUS`. Its only effect on `t_content` is removing 150–350 ms of unnecessary work |
| DPI / window / title | PMv2 via `SetProcessDpiAwarenessContext(-4)` before anything else. Client area 1000×800 at 96 DPI (screenshot 1002×833 with the frame). Title `medium.md — FastMD (blitz)` |

### `bench.py check` (single runs, shared machine)

| variant | medium: content / window / observed, ms | large: content / window / observed, ms |
|---|---|---|
| blitz | 152.2 / 145.4 / 108.2 (noisy run) | 137.3 / 132.6 / 84.4 |
| blitz-full | 121.1 / 117.3 / 80.4 | 2 731.9 / 2 725.8 / 81.1 (WS 777 MB) |
| blitz-gpu | 842.9 / 839.1 / 124.9 | 2 628.5 / 2 618.3 / 1 891.6 (`launch_overhead` 1 793 ms, CreateProcess stalled because of neighbors — noise) |

`CHECK OK` everywhere, no warnings. `observed_ms` is consistently **35–45 ms earlier** than `t_content`, not later.
The window is shown (`window_shown`) before the first frame, while the UI thread picks up the document and draws. The mark is not early:
an early mark would show up as `observed` much later than `t_content`.

### Auditor's quick run

`bench.py run blitz,blitz-full,blitz-gpu,baseline-win32 --doc small,medium --runs 5`
→ `results/audit-run.json`. Median of 5 runs after the first. Shared machine with other agents running in parallel.

| variant | small median (min) | medium median (min) | obs medium | WS medium |
|---|---|---|---|---|
| blitz | 126.0 (108.7) | **118.8** (114.3) | 84.0 | 45.3 MB |
| blitz-full | 114.9 (108.5) | 125.2 (114.8) | 84.5 | 45.4 MB |
| blitz-gpu | 818.4 (770.8) | 788.5 (769.8) | 103.6 | 112.5 MB |
| baseline-win32 | 77.0 (69.6) | 75.1* (69.9) | 49.1 | 13.9 MB |

On small (3.4 KB), `blitz` and `blitz-full` do exactly the same work: the document is under 18 KB, so there is no chunk. The difference
of 126 vs 115 ms is pure noise, a guide to the error margin of ±10 ms. \*For baseline-win32/medium one run was
corrupted by the harness (see below). The median without it is 75.1; the summary shows 74.9 and `min 2.7`.

### Remarks that do not affect honesty

1. `check_presented` checks only `renderer.is_active()`, not whether a frame was actually drawn. In theory the mark
   could be taken without a frame in two cases: (a) `View::redraw` skips a frame when
   `has_pending_critical_resources`, which happens only with a `<link rel=stylesheet>` in `<head>`, and our
   page has none; (b) `vello_hybrid` skips a frame on a `get_current_texture` error. Neither happens on the
   corpus: the screenshots and `observed` agree. Not fixed, because without a fork of anyrender there's no way to know
   whether the frame was drawn.
2. UX: winit's window class has `hbrBackground = null`, and the window is visible before the first frame, so the client area
   stays unpainted for ~35 ms. The product should show the window in `complete_resume` or fill the background.
3. The first-screen chunk differs from the beginning of the full document only in rare cases: footnote links and
   reference links defined below the chunk (the corpus has none), and list "looseness" if the cut falls inside a
   loose list.
4. `blitz-gpu` works but is not competitive (~790 ms). It is kept in the final measurement as a GPU raster reference point.

### Visual assessment (vs §5)

Screenshots: `shots/audit-blitz-medium.png`, `audit-blitz-small.png`, `audit-blitz-medium-y750/1500/2300.png`
(scrolled via `--scroll-y`), `audit-blitz-full-medium.png`, `audit-blitz-gpu-medium.png` (identical to CPU).

- Good: an 860 DIP centered column with 32 padding. h1–h6 sized correctly, h1/h2 have a rule, h6 is gray. Tables:
  borders, "zebra" rows, right and center alignment, semibold header. Code blocks: Cascadia Mono, background, radius.
  Blockquotes with a bar, links `#0969da`, inline code with a background, strikethrough, bold+italic. Color emoji, CJK,
  Cyrillic, local PNG.
- Worse than GitHub: **no syntax highlighting**. Task checkboxes are larger than GitHub's (~20 px vs ~13 px) and
  stick out to the left. Grayscale text antialiasing, no ClearType or hinting: at 100 % slightly softer than
  DirectWrite, but clean. Wide tables don't scroll (`display:table`).
- Rating: visual quality **8/10**, spec fidelity **8/10**.

### Suspected harness bug (reported to the orchestrator, harness not touched)

`run_once` writes the result to `bench/results/tmp/<variant id>.json`, and this name is shared by all parallel
harness invocations. If two agents run the same variant at the same time, one reads the other's result. This
happened in my run: baseline-win32/medium #2 gave `content 2.67 мс`, `main −36.5 мс`, `window −2.6 мс`, and the window
was not observed. `run` doesn't reject such records, unlike `check`, which warns about marks outside
`[0, t_content]`. Proposal: a unique file name per launch (pid/uuid) and rejecting records with negative
marks or without `observed_ms` in `self` mode. This does not affect the final sequential measurement.
