[Русский](README.md) | **English**

# FastMD — the `qt` prototype (Qt 6.12 Widgets, C++)

**Summary:** Qt Widgets + a `QTextBrowser` whose `QTextDocument` is built by md4c directly (no HTML/CSS) shows
a spec-styled document in **~180–205 ms** median `t_content` (small/medium/large are almost the same —
the large file thanks to "first screen first"). The floor of this machine — the empty Win32 window `baseline-win32` — is **~91 ms**
under the same conditions, i.e. Qt adds **~90–110 ms**. Naive Qt (`setMarkdown`, default settings) takes
**650–1160 ms**. The best variant, `qt`, is a statically built Qt inside a DLL behind a small exe stub; with the official
Qt DLLs (`qt-dll`) it is 0–25 ms slower.

All numbers were taken on a shared machine where other agents were running in parallel (stdev 20–50 ms, occasional outliers
up to 2–3 s). This is a rough comparison; the final measurement is up to the orchestrator.

---

## 1. Stack and versions

| Component | Version / where |
|---|---|
| Qt, official binaries (`qt-dll`, `qt-stock`) | **6.12.0** `win64_msvc2022_64`, aqtinstall → `bench/.tools/Qt/6.12.0/msvc2022_64` |
| Qt static (`qt`, `qt-full`) | qtbase 6.12.0 from source (`aqt install-src … --archives qtbase`, archive `qtbase-everywhere-src-6.12.0-rc.zip`), built by `build-qt-static.cmd` → `bench/.tools/Qt/static-6.12.0`: `-static -static-runtime -release`, without OpenGL/Vulkan/SQL/Network/DBus/PrintSupport/Concurrent/XML |
| aqtinstall | 3.3.1.dev166 (master branch from GitHub, venv `bench/.tools/aqt-venv`); 3.3.0 from PyPI does not understand the Qt 6.11+ repository layout |
| Qt modules | QtCore, QtGui, QtWidgets + the `qwindows` platform and the `qmodernwindowsstyle` style (windows11); nothing else |
| Markdown parser | md4c (C, `third_party/md4c`, the same copy as in the `cpp-*` prototypes), `MD_DIALECT_GITHUB` |
| Compiler | MSVC 14.44 (VS 2022), CMake + Ninja from VS, `/O2`, `/utf-8`, C++17 |
| Runtime | `qt`/`qt-full`: static CRT; `qt-dll`/`qt-stock`: system UCRT/VCRUNTIME (we do not ship the redist) |
| Distribution | `out-static/` **20.1 MB** (2 files: 107 KB exe + 20 MB `fastmd_core.dll`); `out/` 26.8 MB (6 files) |

## 2. How to build

```powershell
pwsh -File bench/protos/qt/build.ps1
```

The script is non-interactive and installs everything it needs into `bench/.tools` itself (no admin rights):

1. If Qt is missing — venv `bench/.tools/aqt-venv` with aqtinstall (master) and fontTools, then Qt 6.12.0 msvc2022_64
   (~1 min download) and the qtbase sources (~30 s).
2. `build/` → `out/`: CMake/Ninja against the official DLLs and `windeployqt` without translations, software OpenGL,
   the D3D/DXC compilers, the VC redist and the imageformats/iconengines/generic/tls/networkinformation plugins (PNG is built into QtGui).
3. If static Qt is missing — `build-qt-static.cmd` (configure + ninja + install; ≈ 6 min on a 7950X, 1438 steps).
4. `build-static/` → `out-static/`: the same application as `fastmd_core.dll` (application + static Qt) and
   a tiny `fastmd-qt-static.exe` that only calls the exported `fastmd_run()`.

Rebuilding the application with Qt already installed takes 35–45 s. The script cleans and recreates only its own folders (`build*`, `out*`).

Auxiliary (not part of the build):
- `tools/gen_coverage.py` — generates `src/fontcoverage.inc` (cmap coverage of the Windows 11 fonts; requires fontTools);
- `tools/shot_scroll.py <variant> <doc> <px> <out.png>` — a screenshot scrolled by N DIP;
- `tools/marks.py <run.json>` — mark medians per variant/document;
- `tools/loadlog.ps1 -Doc large [-Exe …] [-Extra …]` — loading of the whole document and the maximum UI pause (§6).

## 3. How it works

```
main ─ fmb_init ─ QApplication ─ ReadFile(.md) ─ md4c(первые ~8 КБ) → QTextCursor → QTextDocument
     ─ MdView(QTextBrowser) ─ resize(1000×800) ─ setDocument ─ show ─ paintEvent ─ (queued) DwmFlush → t_content
     ─ после первого кадра: рабочий поток строит ВЕСЬ документ во втором QTextDocument
       → подмена через setDocument (позиция прокрутки сохраняется) → раскладка остатка шагами по ~16k символов
```

- **No HTML round trip.** md4c callbacks create blocks, character formats, lists and tables directly through `QTextCursor`
  (`src/mdbuilder.cpp`). No `setHtml`/CSS parser at all.
- **What `QTextDocument` cannot do** (rounded corners, code block background with padding, the blockquote bar, the rule under h1/h2,
  rounded inline-code background, GitHub-style list markers, table zebra stripes, `<hr>`) is marked with flags in
  `QTextBlock::userState()` and drawn by `MdView::paintDecorations` beneath the text (`src/mdview.cpp`). Task checkboxes are
  a custom `QTextObjectInterface`.
- **Custom font fallback** (`src/fontfallback.cpp`): the text is split into runs by script, and each run is explicitly
  assigned a family (Segoe UI Variable / Segoe UI / Segoe UI Symbol / Segoe UI Emoji / Yu Gothic UI /
  Microsoft YaHei UI / Malgun Gothic) using pre-generated coverage tables. Otherwise, at the very first character
  outside the main font, Qt iterates over **all** ~350 installed families (≈230 ms on this machine, see `qt-stock`).
- **First screen first** (allowed by protocol §3): if the file is > 16 KB, only the chunk up to the first ATX heading after 8 KB
  is built before the first paint (the boundary is always between top-level blocks and outside fenced code);
  link reference definitions `[x]: url` are appended to every chunk. `t_content` is taken at the frame where the first screen
  is already **fully styled**.
- **Loading the rest** (after `t_content`; not done in bench mode with exit): a worker thread builds the full
  document in a separate `QTextDocument` not attached to the widget (no layout, only md4c + insertions,
  ~0.17 ms/KB), then `moveToThread` to the GUI thread and a swap via `setDocument` with the scroll position restored
  (verified pixel by pixel: the frame scrolled to 800 px matches `qt-full`). Layout below the viewport is driven by a custom
  `LayoutDriver`: it suppresses Qt's lazy layout timer (which doubles its step up to 200,000 characters and freezes the UI
  for 0.5–0.7 s) and advances the layout itself in steps of ~16k characters, updating the scroll range after each step.
  For comparison the flags `--slices` (the old way: 32 KB portions directly into the displayed document) and
  `--qtlazy` (Qt's stock lazy layout) are kept.
- **Honest `t_content` mark:** the first `MdView::paintEvent` → after returning from paint (the backing store has already been flushed
  to the window) `fmb_content_presented_ex()` = `DwmFlush()` + writing the result is called via
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. `t_window` is the moment of that same first paint.
- **Static build in a DLL:** the whole application + static Qt live in `fastmd_core.dll` (`FASTMD_AS_DLL`, export
  `fastmd_run`), the exe is a 107 KB stub (`src/stub.cpp`). System DLLs rarely needed at startup (d3d9/d3d11/d3d12/dxgi,
  UIAutomationCore, setupapi, comdlg32, winmm, ws2_32, etc.) are linked via `/DELAYLOAD`.
- DPI: PerMonitorV2 manifest, 1000×800 DIP window, title `<file> — FastMD (<id>)`.

## 4. Variants (`proto.json`)

| id | exe | What it is |
|---|---|---|
| **`qt`** (best) | `out-static/fastmd-qt-static.exe` | Static Qt in `fastmd_core.dll` + 107 KB stub, all the knobs below. The previous agent called it `qt-sdll` (sdll = static-in-DLL) |
| `qt-dll` | `out/fastmd-qt.exe` | The same code and knobs, but with the official Qt DLLs (as any project would build it). No Qt build needed, LGPL-friendly. 0–25 ms slower than `qt` (for the previous agent this was the `qt` variant) |
| `qt-full` | `out-static/fastmd-qt-static.exe --chunk=0` | Like `qt`, but the whole document is built before the first paint — shows the cost and benefit of "first screen first" |
| `qt-stock` | `out/fastmd-qt.exe --stock --window --ime` | Naive Qt with the official DLLs: `QTextDocument::setMarkdown(..., MarkdownDialectGitHub)` (Qt's built-in md4c importer), default look, the whole document, Qt's font fallback, a regular `Qt::Window`, IME enabled |

Knobs enabled in `qt` / `qt-dll` / `qt-full` (flags in `src/main.cpp::parseArgs`; the effects were measured in pairs on
the build with the official DLLs, interleaved, 4–8 runs — noisy):

| Knob | Off flag | Effect |
|---|---|---|
| A `Qt::Dialog` window (with min/max/close buttons) instead of `Qt::Window`. For `Qt::Window` the Qt plugin applies the QTBUG-50371 workaround in `calcPosition()`: `GpuDescription::detect()` → `Direct3DCreate9` + `GetAdapterIdentifier`, which loads and probes the NVIDIA user-mode driver right on the UI thread | `--window` | the `attached→shown` step 25 → 175 ms; `t_content` 190 → 348 ms (`results/pair-qt-t-win.json`) |
| Custom font fallback + custom document builder | `--stock` | `t_content` 234 → 452 ms (`results/pair-qt-t-stk.json`) |
| `ImmDisableIME(-1)` — a reader does not need IME | `--ime` | ≈ −20 ms (203 vs 221, at the edge of noise) |
| First screen first (8 KB) | `--chunk=0` | large: 835 → 205 ms; small/medium — no difference (§6) |
| Loading on a worker thread + swap + `LayoutDriver` | `--slices`, `--qtlazy` | no effect on `t_content`; all of large is available after ~0.9 s instead of 6.3–6.8 s, maximum UI pause ≤ 60 ms instead of 0.5–0.7 s (§6) |
| Static Qt in a DLL + exe stub (`qt` vs `qt-dll`) | — | −19 / −26 / 0 ms on small / medium / large (summary of 36 runs, §6); −50 MB peak commit, −6.7 MB of distribution |
| `resize()` + a synchronous `QResizeEvent` before `setDocument`, skipping empty resizes, `ScrollBarAlwaysOn` | — | the document is laid out once, straight at the final width |
| PNG sizes from the file header (24 bytes), decoding only on paint | — | layout does not decode images |
| Minimal file set (2 plugins, no translations/software OpenGL/D3D compilers) | — | less loading and less Defender scanning |

Tried and **not** kept:

| Knob | Result |
|---|---|
| A single 21 MB static exe (`-DFASTMD_QT_STATIC=ON`, not a variant) | the `main` mark at 79.6 ms vs 40.7 (static in DLL) and 53.7 (official DLLs) in the same run: Defender synchronously scans a large exe inside `CreateProcess`. `t_content` 231.8 vs 191.5 / 218.4 ms (`results/knobs-single-static-exe.json`). That is why the static build was moved into a DLL behind a stub |
| Style `--style=fusion` / `--style=windows` instead of windows11 (no style plugin load) | 216.0 / 231.5 ms vs 218.6 — no difference (`results/knobs-style.json`) |
| Warming up the GPU description on a background thread (`--warm`) | only needed with `--window`; with `Qt::Dialog` there is no probe at all |
| Warming up `QFontDatabase` on a background thread (`--fontwarm`) | no gain (the previous agent's measurement) |

## 5. Spec support (PROTOCOL §5)

Screenshots (`shots/`): `qt-medium.png`, `qt-small.png`, `qt-large.png`, scrolled `qt-medium-s700.png`,
`qt-medium-s1450.png`, `qt-medium-s5200.png`, `qt-dll-medium.png`, `qt-full-medium.png`, `qt-stock-medium.png`,
`qt-stock-small.png`. All inspected.

| Element | `qt` / `qt-dll` / `qt-full` | `qt-stock` |
|---|---|---|
| Body: Segoe UI Variable Text 15 px, line 24 px (1.6), `#1f2328` | yes | Segoe UI Variable, tighter |
| Centered 860 DIP column, 32 DIP margins (text 796 DIP) | yes | yes |
| h1–h6 semibold 30/24/20/17/15/14, h6 `#59636e`, `#d1d9e0` rule under h1/h2 | yes (h1/h2 in Segoe UI Variable Display) | bold, no rules |
| **bold** / *italic* / ~~strikethrough~~ / links `#0969da` | yes (semibold italic via Segoe UI Semibold — Variable has no italic) | links in the palette color (pink) |
| `inline code`: Cascadia Mono 0.85 em, background `#eff1f3`, radius 4 | yes | monospace without background |
| Code block: Cascadia Mono 13.5 px, background `#f6f8fa`, radius 6, padding 16 | yes; **long lines wrap**, no horizontal scrolling | Courier without background |
| Syntax highlighting | yes, a custom single-pass lexer, GitHub light colors: rust, c/cpp, csharp, js/ts, json, python, powershell, bash | no |
| Lists: bulleted (● ○ ■ by level), numbered, nested, tasks (custom checkboxes) | yes | Qt markers, ☒/☐ checkboxes |
| Blockquotes: 4 px `#d1d9e0` bar, text `#59636e`, nesting | yes | indent without a bar |
| `---` | yes (2 px `#d1d9e0`) | yes |
| GFM tables: 1 px `#d1d9e0` borders, semibold header, `#f6f8fa` zebra stripes, padding 6/13, column alignment | yes | double borders, no zebra stripes |
| Images (local PNG) | yes, width ≤ 100 % of the column | yes |
| Unicode: Cyrillic, CJK, emoji | yes (color COLRv1 emoji from Segoe UI Emoji) | yes (through Qt's slow fallback) |
| Scrolling, selection, Ctrl+C, link click | yes (`QDesktopServices::openUrl`) | yes |

Not supported: footnotes (md4c does not know them), `#heading` anchor links, raw HTML blocks (shown as code;
inline HTML is ignored except `<br>`), math (`$...$` is drawn as inline code), JPEG/GIF/SVG (need the imageformats
plugins — deliberately not shipped), dark theme, search, zoom. Wide code and tables have no horizontal scrolling
(code wraps, a table is squeezed to the column width).

## 6. Quick measurements

Machine: Ryzen 9 7950X, Windows 11 26200, Defender RTP on, **other agents were running in parallel**.
`bench.py run … --order interleaved`, the first run of each series discarded, median `t_content`, ms (min / p90):

| Variant | small (3.4 KB) | medium (53 KB) | large (3.7 MB) | WS, MB | peak commit, MB | distribution |
|---|---|---|---|---|---|---|
| `baseline-win32` (floor, same run) | — | 91.4 (81.8 / 101.6) | — | 14 | 3 | 0.15 MB |
| **`qt`** (36 runs, 5 series) | **179.7** (147.5 / 222.9) | **190.0** (166.6 / 251.0) | **205.3** (172.4 / 271.3) | 37–42 | 12–17 | 20.1 MB |
| `qt-dll` (36 runs, same series) | 198.3 (155.2 / 255.5) | 216.1 (176.2 / 304.8) | 205.3 (174.9 / 305.2) | 42–48 | 66–72 | 26.8 MB |
| `qt-full` (8 runs) | 180.8 | 214.6 | **834.7** | 36–84 | 12–70 | 20.1 MB |
| `qt-stock` (5 runs, separate series) | 702.8 | 651.8 | 1162.7 | 95–143 | 104–154 | 26.8 MB |

The `qt`/`qt-dll` summary combines the series `results/run-ab.json`, `run-quick.json`, `run-final.json`, `run-final2.json`
(where the static build was called `qt-static` and the official DLLs `qt`) and `run-final3.json` (final names). The path to
`t_content` is identical in all series; after them only the post-first-frame document loading changed.
The probability that a random `qt` run is faster than a random `qt-dll` run: 0.63 / 0.67 / 0.54 — the static build's gain is
small but consistent on small/medium. Individual series vary a lot: e.g. in `run-final.json`
(heavy background load) `qt-dll` on large has p90 = 2.2 s. `qt-full`: `run-final3.json`; `qt-stock`:
`run-quick-stock.json` (in `run-final-stock.json`, under load — 883/892/1408). The first launch after a rebuild takes
up to **2.5 s** (Defender scans the new exe/DLLs).

### Breakdown by marks (medians, ms from `t0` = before `CreateProcessW`)

| mark | what has happened by this mark | `qt` medium | `qt` large | `qt-dll` medium | `qt-full` large | `qt-stock` medium |
|---|---|---|---|---|---|---|
| `main` | CreateProcess + Defender + loader + Qt static initialization | 40.1 | 41.7 | 44.8 | 40.9 | 48.3 |
| `qapp` | `QApplication` constructor: qwindows platform, DPI, observer windows (power/clipboard/screen/theme), screens and color space, theme | 80.0 | 85.4 | 93.7 | 83.1 | 112.5 |
| `read` | `ReadFile` of the whole file | 80.2 | 86.8 | 93.8 | 84.5 | 112.6 |
| `parsed` | md4c + building the `QTextDocument` (first chunk / whole document) | 81.5 | 89.1 | 95.3 | **696.1** | 118.2 |
| `view` | `QTextBrowser`: style + populating `QFontDatabase` (DirectWrite enumerates ~350 families) | 94.6 | 101.8 | 110.0 | 709.5 | 223.8* |
| `attached` | `setDocument`: layout of the first screen, font engines, HarfBuzz shaping | 125.9 | 130.2 | 145.9 | 753.4 | **456.3** |
| `shown` | `show()`: native window, backing store | 153.9 | 160.6 | 177.2 | 788.2 | **600.9** |
| `paint_begin` / `painted` | first `paintEvent`: decorations, DirectWrite glyph rasterization, flush | 166.7 / 183.8 | 172.2 / 192.3 | 191.3 / 207.7 | 801.2 / 816.7 | 609.0 / 639.8 |
| `t_content` | queued call + `DwmFlush()` | **190.0** | **205.3** | 216.1 | 834.7 | 651.8 |

\* for `qt-stock` the `view` step is inflated by the "neighbor effect" (see §9, item 8): after another variant has run, the same step
takes ~16 ms. The rest of `qt-stock`: +233 ms on laying out the first screen (Qt's font fallback iterates over all
fonts) and +145 ms on `show()` (the `Qt::Window` GPU probe).

### Loading the whole document (after `t_content`, not a bench mark)

`pwsh -File tools/loadlog.ps1 -Doc large [-Exe out/fastmd-qt.exe] [-Extra --slices|--qtlazy|--chunk=0]` — the window
stays open, the application writes `FASTMD_QT_LOADLOG`: when the whole document is built, when layout is finished, and
the longest event loop pause within 10 s after the first frame (2 ms timer). Times are from the `main` mark:

| Method (large, 3.7 MB) | whole document available | layout finished | max UI pause |
|---|---|---|---|
| **`qt`: thread + swap + `LayoutDriver`** | **0.86–0.97 s** (swap 30–37 ms) | 6.5–7.2 s | **51–59 ms** |
| thread + swap + Qt's stock lazy layout (`--qtlazy`) | 1.14 s | ~4.7 s | 494–739 ms |
| 32 KB portions into the displayed document (`--slices`, the previous agent's method) | 6.3–6.8 s | together with building | 77–78 ms |
| whole document before the first frame (`qt-full`) + `LayoutDriver` | 0.86 s (that is the first frame) | 5.7 s | 48 ms |

Measured on the build with the official DLLs; a control run of the static build: document at 0.92 s, layout done by
6.58 s, pause 57 ms. medium: the whole document 272 ms after `main` (layout done by 363 ms, pause ≤ 38 ms);
`--slices` — 234–340 ms, pause up to 61 ms. WS after fully loading large ≈ 327 MB. While layout is in progress, scrolling
is smooth; jumping with the scrollbar thumb to the very end of large forces Qt to lay out everything up to that point at once (seconds) — as in
any `QTextEdit`.

## 7. Where the time goes (`qt`, medium, ≈ 190 ms)

| Stage | ms | Comment |
|---|---|---|
| Process and image loading | ~40 | the `baseline-win32` floor has the `main` mark at ≈ 36 ms; the 20 MB DLL adds ~4 ms (official DLLs — ~9 ms) |
| `QApplication` | ~40 | Qt's main "tax", independent of the application (with the official DLLs ~49: plugin loading from disk on top) |
| Reading + parsing + building the first screen | ~1.5 | md4c + QTextCursor ≈ 0.17 ms/KB; all of large — ~610 ms |
| Font database + widget | ~13 | full enumeration of system fonts on the first `QFont` |
| Layout of the first screen | ~31 | creating 5–6 font engines (text, headings, mono, emoji, CJK) + shaping |
| `show()` | ~28 | native window, DWM, backing store |
| First frame | ~30 | waiting for the paint event ~13 + paint (decorations + glyphs + flush) ~17 |
| `DwmFlush` | ~6 | waiting for DWM composition |

Main takeaway: Markdown parsing and file size barely matter (with "first screen first"); the cost is
~100 ms of fixed Qt initialization (QApplication + fonts + layout + window) on top of the ~90 ms Windows floor.

## 8. Further speed-up ideas

1. **In parallel with `QApplication` (~40 ms):** on a background thread, create the DirectWrite font faces for the needed
   fonts in advance, read the file, build the first chunk (building only needs `QFont`) — potentially −10…20 ms.
2. **Trimmed platform initialization** (the static build makes this possible): patch the `qwindows` plugin to skip the clipboard viewer,
   the power/theme observers and the Advanced Color query, plus a custom `QPlatformFontDatabase` registering only
   the 7 families in use instead of ~350 — an estimated −15…25 ms. Qt private APIs.
3. **`QGuiApplication` + `QRasterWindow`** without QtWidgets and styles, drawing the `QTextDocument` directly via
   `QAbstractTextDocumentLayout::draw` — minus QtWidgets and style creation.
4. **Even less Qt** in the static build (`-no-feature-*` for unused parts of Gui/Widgets) — a smaller image,
   less static initialization.
5. **Layout of large documents** is the bottleneck once the first frame is out: Qt's own layout of large takes
   4.7–6 s (≈1.4 ms per 1000 characters). What has been done (thread + swap + `LayoutDriver`) removes freezes but does not
   speed it up. Next step — virtualization: several small per-section `QTextDocument`s and custom scrolling with an estimated
   height.
6. **Resident mode** (`qt-resident`, protocol §6): an already running process receives the path via `WM_COPYDATA` —
   expected 40–70 ms. Not implemented.

## 9. Pitfalls

1. **aqtinstall 3.3.0 (PyPI) does not install Qt 6.11+** — the master branch from GitHub is needed (`build.ps1` does exactly that).
   The 6.12.0 sources in the repository are published as `qtbase-everywhere-src-6.12.0-rc.zip` (`.cmake.conf` says
   `PRERELEASE alpha1`), i.e. the static Qt is formally not built from the same snapshot as the official DLLs.
2. **Qt's font fallback is the main brake of the naive approach:** the very first CJK/emoji character makes Qt iterate over
   all installed fonts (≈230 ms). The custom itemizer avoids this, but `src/fontcoverage.inc` was generated from the fonts of
   **this** Windows 11; on another OS build the coverage may differ (not critical: an unknown character goes to
   Qt's stock fallback).
3. **Qt's hidden GPU probe:** any top-level window of type `Qt::Window` loads the graphics driver via Direct3D9
   (~150 ms with an RTX 5070). It can only be turned off by changing the window type (`Qt::Dialog` + button hints); the window looks and
   behaves like a normal one.
4. **`QTextDocument` is not a browser:** no rounded corners, block padding, blockquote bars, zebra stripes — all of it is drawn by hand
   from block flags. The "Segoe UI Variable Text" family in Qt has only Regular/Bold: semibold is set via the
   named instance "… Semibold", and semibold italic via the static "Segoe UI Semibold".
5. **`QTextDocumentLayout`'s lazy layout** doubles its step from 1,000 to 200,000 characters every 10 ms
   (`QTextDocumentLayoutPrivate::layoutStep`), so after display a large document freezes the UI for 0.5–0.7 s
   per step. There is no public API for the step; the workaround is a timer event filter on the layout object, custom small steps via
   `blockBoundingRect()` and a manual `documentSizeChanged` (QTextEdit extrapolates the height from `layoutStatus()` itself).
   This relies on Qt's internal behavior — re-check when upgrading Qt. Appending to an attached document
   is expensive too: every insertion triggers `documentChanged` → layout up to the insertion point, so the full document
   is built separately and swapped in as a whole.
6. **A bug found during tuning:** `QAbstractTextDocumentLayout::hitTest(FuzzyHit)` for a point in the top margin of
   the page, while lazy layout was not finished, returned a block inside a not-yet-positioned table far
   below the viewport → in `qt-full` on medium the first frame had no list markers, heading rules or inline-code
   background. Fixed: near the top of the page decorations are drawn from the first block, otherwise hitTest is only a hint
   checked against `blockBoundingRect` (`MdView::paintDecorations`).
7. **Defender:** the first launch after a build takes ~2.5 s; a large exe is scanned inside `CreateProcess` (+35–40 ms on
   every launch for a 21 MB exe), which is why the static build was moved into a DLL behind a small stub.
8. **Cross-run interference (important for the orchestrator):** after a process that has iterated over all system fonts
   (`qt-stock`), the **next** launch of any variant spends 40–100 ms more on populating the font database
   (the `parsed→view` step 13 → 56…118 ms), and a 2 s pause between launches does not help (`results/order1.json`,
   `order3.json`, `pair-qt-t-stk.json`; no effect for `--window`/`--ime`). Probably the Windows Font Cache
   Service is involved. With `--order interleaved` (the default) the variant that runs right after such a "font hog"
   is penalized: in the first run of this session the static variant ran after `qt-stock` and looked 60–130 ms
   slower than it really is. Recommendation: measure `qt-stock` (and similar variants of other stacks) separately or with
   `--order grouped`.
9. **LGPL:** static linking of Qt requires giving the user the ability to relink (object files) or a
   commercial license. If that is unacceptable — `qt-dll` (official DLLs) is only 0–25 ms slower.

## 10. Effort estimate for a full product on this stack

| Work | Estimate |
|---|---|
| Already done: GFM parsing, spec typography, PNG, highlighting for 8 languages, checkboxes, tables, first screen first, background loading without freezes | — |
| Dark theme, zoom (Ctrl+wheel), search (Ctrl+F via `QTextDocument::find`), anchors and table of contents | 4–6 days |
| Reload on file change (`QFileSystemWatcher`) preserving the position, drag&drop, recent files, multiple windows | 3–4 days |
| JPEG/GIF/SVG (load the plugins lazily), remote images (no network, or via QtNetwork) | 2–3 days |
| Footnotes, HTML blocks via `QTextDocumentFragment::fromHtml`, simplified math | 1–2 weeks |
| Layout virtualization for multi-megabyte files (item 8.5), so that jumping to the end is instant too | 1–2 weeks |
| Printing/PDF (QtPrintSupport), settings, icon, `.md` association, installer, code signing (removes part of the Defender delays), CI build of static Qt | 1.5–2 weeks |
| Testing on different DPIs and monitors, accessibility (`QTextBrowser` has UIA) | 1 week |
| **Total** | **MVP ≈ 3–5 weeks for one C++/Qt developer; a "polished" product ≈ 2–3 months** |

Stack risks: the fixed ~100 ms of Qt on top of the Windows floor cannot be removed without private APIs; `QTextDocument` is limited
in layout (no CSS model: float, horizontal code scrolling, complex tables), everything "pretty" is drawn by hand.

## 11. Files

| Path | What |
|---|---|
| `src/main.cpp` | entry point, knobs, "first screen first", background loading, `LayoutDriver`, bench marks |
| `src/mdbuilder.cpp/.h` | md4c → `QTextDocument`, theme (all sizes and colors from the spec) |
| `src/mdview.cpp/.h` | `QTextBrowser` + decorations, list markers, checkboxes |
| `src/fontfallback.cpp/.h`, `src/fontcoverage.inc` | custom font fallback |
| `src/highlight.cpp/.h` | syntax highlighting |
| `src/stub.cpp` | exe stub for the static build |
| `CMakeLists.txt`, `app.manifest`, `build.ps1`, `build-qt-static.cmd` | build |
| `shots/`, `results/` | screenshots and JSON measurement results from this session |

---

## Audit (independent review, 2026-09-19)

The code, build and marks were checked by a separate agent. **The code and binaries were not changed, no rebuild was needed.**
Only the notes in `proto.json` (`qt-dll`, `qt-stock`) were changed and this section was added. New files:
`shots/audit-*.png`, `results/audit-*.json`. The machine was shared, the numbers are noisy.

### What was checked

| Item | Result |
|---|---|
| Release build | `build/` and `build-static/`: `CMAKE_BUILD_TYPE=Release`, `/O2 /Ob2 /DNDEBUG`, the static one with `-MT`. Static Qt (`bench/.tools/Qt/build-static`): Release, `-O2 -Ob3`, no LTCG. `/Zi` + `/DEBUG /OPT:REF /OPT:ICF` only produce a PDB, the code does not change. The binaries (03:54) are newer than the sources (`main.cpp` 03:53) |
| `t_content` per §3 | The viewport's first `MdView::paintEvent` → a queued call (runs after `paintEvent` and after the backing store is flushed to the window) → `DwmFlush()` → `t_content` (`fastmd_bench.h`). The mark is not set at window creation or after parsing |
| Independent frame check | The auditor's script captured the screen with DWM composition (`BitBlt` of the screen DC, ~10–15 ms per frame) until the `qt` window on medium closed. The window is visible from 155.8 ms, `painted`/`t_window` 199.8 ms, `t_content` 214.9 ms. The frame captured at 192.8–207.9 ms: the window is still blank (the Windows open animation is running). The frame at 208.0–217.8 ms: the document is already there. **The mark is not early**, the DwmFlush margin is ~15 ms |
| The harness's `observed_ms` | In every `check` the window is visible before `t_window`/`t_content`: `qt` medium obs 159.7 / win 193.0 / content 202.8; `qt` large 130.4 / 161.2 / 173.2 |
| Resident, caches, corpus data | None. No background processes, the process writes no files (except diagnostics behind env variables). `fontcoverage.inc` comes from the cmap of Windows fonts (`tools/gen_coverage.py`), not from the corpus. No qrc/resources. PNG sizes are read from the file header at run time |
| "First screen first" | An allowed technique (§3), described in the README. The `qt` frame on large captured right after `t_content` (`shot --settle 0`, before the document swap) **matches pixel for pixel** the `qt-full` frame (whole document). `qt`/`qt-dll`/`qt-full` on medium also match pixel for pixel in the client area |
| Window | 1000×800 client area, title `<file> — FastMD (<id>)`, PerMonitorV2 manifest. `Qt::Dialog` gives the style `0x96CF0000` (WS_POPUP + caption, sysmenu, thickframe, min/max), no owner and no `WS_EX_TOOLWINDOW`: there is a taskbar button, the window is resizable. The window does not end up in the foreground, but `baseline-win32` behaves the same: when launched from a background process the foreground lock applies, Qt has nothing to do with it |

### Findings

1. **`qt-stock` spoils the measurement of the next Qt process (confirmed).** In `results/audit-run.json` (interleaved;
   `qt` ran right after `qt-stock` in every round) the `parsed→view` step of `qt` took 45–147 ms, for all the others
   11–15 ms. `qt` medians were 283.9/297.9 ms (small/medium) versus 201.8/192.8 ms for **the same exe** in the `qt-full` variant.
   The effect fires once: only the first Qt process after `qt-stock` suffers. After 30 s it still persists
   (`view` 49.0 ms vs 12.4–13.0). Non-Qt stacks are not affected: `cpp-d2d` after `qt-stock` 70.9 ms vs 67.4 in
   the control, `avalonia` 149.7 vs 152.7. **Therefore `qt-stock` is excluded from the final set.** If its
   reference point is needed, measure it in a separate series after everything else. It is kept in `proto.json` and marked.
2. **The gain of the static build (`qt`) over the official DLLs (`qt-dll`) was not reproduced.** In clean series without `qt-stock`:
   `qt` 155.7/172.8 ms, `qt-dll` 160.3/169.1 ms (`results/audit-run-nostock.json`), on medium 174.9 vs 175.7
   (`results/audit-vs-baseline.json`). The difference is no more than 5 ms either way, i.e. within noise. The real
   differences are only in peak commit (14 vs 68 MB) and distribution size (20.1 vs 26.8 MB). Treat the "−19/−26 ms" figures in §6
   as unconfirmed.
3. Honest limitations that do not affect `t_content`:
   - chunking relies only on ATX headings (`#`). A file larger than 16 KB without `#` headings after 8 KB
     is built in full, as in `qt-full` (~0.17 ms/KB, i.e. ~0.6 s for 3.7 MB);
   - `ImmDisableIME(-1)` disables IME for the whole process. For a future search box (CJK input) it will have to be brought back;
   - `Qt::Dialog` instead of `Qt::Window` is a workaround to skip Qt's GPU probe. Outwardly the window is normal.

### Visual assessment (screenshots `shots/audit-qt-{small,medium,large}.png`, scrolled `audit-qt-medium-s{750,1500,2300}.png`)

Colors match the spec to the pixel: code background `#f6f8fa`, borders/rules/blockquote bar `#d1d9e0`, h6
`#59636e`, blue links `#0969da` (ClearType). h1–h6, rules under h1/h2, bold/italic/bold italic/strikethrough,
inline code with a rounded background, code blocks with highlighting (rust, csharp, etc.), ● ○ markers and nested numbering,
GitHub-style task checkboxes, blockquotes with a bar, `---`, tables (borders, semibold header, `#f6f8fa` zebra stripes
right up to the border, per-column alignment, content-sized width), local PNG, Cyrillic, CJK, color emoji —
everything is in place, the text is crisp.

Minuses:
- long code lines wrap without a hanging indent (as stated, no horizontal scrolling);
- the scrollbar is not visible at all: the windows11 style hides it until hover, so the document length cannot be seen;
- Japanese (Yu Gothic UI) is noticeably thinner than Chinese (Microsoft YaHei UI) on the same line;
- Segoe UI Variable italic is synthetic;
- no footnotes, HTML blocks and math are shown as code.

**Score: look 8.5/10, GitHub fidelity 8/10.** `qt-stock` is the naive reference: pink links, code without background,
double table borders, table header pressed to the bottom. About 4/10.

### Audit conclusion

The measurement is fair: `t_content` follows protocol §3, there are no caches, resident processes or corpus-specific tuning, the builds are Release.
**For the final sequential measurement: `qt`, `qt-dll`, `qt-full`.** Measure `qt-stock` only separately.
The audit's control numbers (noisy, 5 runs, median `t_content`, ms): `baseline-win32` medium 82.3; `qt`
155.7 / 172.8–174.9; `qt-dll` 160.3 / 169.1–175.7; `qt-full` 154.8 / 177.6 (small / medium). One-off `check` runs on
large: `qt` 173.2, `qt-dll` 177.1, `qt-full` 875.7, `qt-stock` 1187.7.
