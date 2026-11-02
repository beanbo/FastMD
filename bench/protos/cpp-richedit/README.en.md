[Русский](README.md) | **English**

# cpp-richedit — Win32 + built-in RichEdit 8 (msftedit.dll) + md4c → RTF

The "zero dependencies, only what already ships with Windows" path: markdown is parsed by md4c, converted to RTF and
loaded into the system RichEdit control (`msftedit.dll`, class `RICHEDIT50W`) via `EM_STREAMIN`.
Two ways of hosting RichEdit: a classic child window and **windowless** (`CreateTextServices` + a custom
`ITextHost2`); windowless can draw both via GDI (`TxDraw`) and via Direct2D/DirectWrite (`TxDrawD2D`).

**TL;DR**

* The best variant, `cpp-richedit-wl-mt`: **median t_content 82 / 93 / 97 ms** (small / medium / large) against
  a calibration floor of `baseline-win32` at 104 / 96 / 92 ms in the same run (the machine was loaded by parallel
  builds, so the numbers are rough). Inside the process (from `main` to t_content): **47–58 ms versus 58–69 ms for the baseline**:
  the markdown viewer is faster than "an empty window with one line" because it disables IME/TSF (see below).
* The most valuable finding for all prototypes: **`ImmDisableIME(-1)` saves ~20 ms at startup of any Win32 GUI**.
  Verified on an unmodified copy of the baseline: t_content 84.0 → 62.0 ms (median, n=15).
* Distribution size: a single **0.26 MB** exe, static CRT, no DLLs except system ones.
* Quality: headings, tables with borders and zebra, code blocks with background, blockquotes with a bar, lists/tasks, PNG/JPEG
  images, all per the style spec. What didn't work out: rounded corners, color emoji, syntax highlighting, links without underline.
* A D2D/DirectWrite mode is **achievable** (windowless + `TXTBIT_D2DDWRITE`), variant `cpp-richedit-d2d`, 101–114 ms.
  The windowed RichEdit has no switch to D2D (verified by trying every style bit).

## Stack and versions

| Component | Version |
|---|---|
| RichEdit | `C:\Windows\System32\msftedit.dll` 10.0.26100.8875 (RichEdit 8.x, Windows 11 Pro 26200) |
| Markdown parser | md4c 0.5.2 (`third_party/md4c`, MIT), flags `TABLES | STRIKETHROUGH | TASKLISTS | PERMISSIVEAUTOLINKS` |
| Compiler | MSVC 14.44 (VS 2022), Windows SDK 10.0.26100, `/O2 /MT /GS- /GL`, LTCG, no exceptions/RTTI |
| Rendering | GDI/Uniscribe (windowed and windowless-GDI) or Direct2D 1.x + DirectWrite (windowless-D2D, `d2d1`/`dwrite` delay-load) |
| Code | ~2.5 K lines of C++: `src/md2rtf.cpp` (md4c → RTF), `src/main_gdi.cpp` (windowed host), `src/main_wl.cpp` (windowless host), `src/common.h` |

## Build

```
pwsh -File bench/protos/cpp-richedit/build.ps1
```

The script builds from scratch `out/windowed/fastmd-richedit.exe` (windowed RichEdit) and
`out/windowless/fastmd-richedit-wl.exe` (windowless; the render mode is chosen by a flag). Manifest (`app.manifest`):
PerMonitorV2, supportedOS Win10+. Imports: `user32, gdi32, imm32, dwmapi, kernel32` (+ `d2d1`, `dwrite` delay-loaded).

## How it works

1. **`main`**: `ImmDisableIME(-1)` (the viewer needs no text input), start of the worker thread.
2. **Worker thread**: `ReadFile` → md4c → RTF (`md2rtf.cpp`). md4c first marks up the blocks of the whole document
   (~4.5 ms on 3.7 MB), then come the callbacks. The generator estimates the layout height (a table of real GDI character widths for
   Segoe UI Variable) and, at the boundary of a top-level block where the estimate exceeded 1400 px (≈1.75 screens), hands off
   the **first chunk** of RTF (header + prefix + `}`); the rest of the document is generated in parallel (large: the whole 25 MB RTF
   is ready ~45 ms after `main`).
3. **The RichEdit host** streams the first chunk (`EM_STREAMIN`, `SF_RTF`) and draws the first frame. This is the "first viewport first"
   technique allowed by PROTOCOL §3; screenshots confirm that the first chunk fully covers the viewport
   (`shots/firstchunk-medium.png`, `shots/firstchunk-large.png`; for medium the first frame is pixel-identical to the
   final one).
4. **t_content**: after `EndPaint`/`EndDraw` of the first frame with content + `DwmFlush()` (helper `fastmd_bench.h`).
   In the windowed variant this is done by subclassing `WM_PAINT` of the RichEdit control itself.
5. **Phase 2** (after t_content, not executed in bench mode): the whole document.
   * `cpp-richedit-wl-mt`: a helper thread creates a **second** text services instance, loads the whole RTF into it
     (large: ~0.9 s) and hands it to the UI thread, which just swaps the pointer, preserving the scroll position. The UI stays
     responsive all this time (`shots/scroll-wlmt-bg-*.png`).
   * windowed variant: one full reload on the UI thread (medium ~40 ms, large ~0.9 s, during which the UI is busy).

### How RTF imitates the GitHub style (what msftedit actually draws)

Verified with probes (`probe/`, screenshots `probe/probe1.png` … `probe9c.png`):

| CSS technique | What is done in RTF | Why |
|---|---|---|
| code block background, 16 px padding | a 1×1 table: `\clcbpat` + `\trgaph240` + `\sb240\sa240` in the cell | paragraph shading `\cbpat` and paragraph borders `\brdrX` **are parsed but not drawn**; cell shading/borders are drawn |
| 1 px rule under h1/h2 | a 1×1 table with a `#d1d9e0` bottom border, the other borders white | same reason; a borderless table draws a gray grid, so "invisible" borders = background color |
| 4 px blockquote bar | a 1×1 table with a `\brdrw60` left border | — |
| 2 px `<hr>` | an empty table with a `\brdrw30` top border | a row with `\trrh` + shading is not visible |
| line-height 1.6 | exact spacing `\sl-360` (24 px) | `\slmult1` multiplies by the font's *natural* line height (~1.33 em) |
| spacing between blocks (margin collapse) | separate "spacer" paragraphs of exact height | character shading (`\highlight`, inline code) fills the **whole** line together with `\sb` |
| cell padding 6/13 | `\trgaph195` + `\trrh` (min. height) + `\clvertalc` | `\trpaddt/b` don't work, `\sb/\sa` in a cell inflate the inline code background |
| column widths (max-content) | estimated from the GDI character width table, proportionally shrunk to 796 px | RichEdit RTF tables have no auto-layout |
| semibold 600 | a separate family `Segoe UI Variable Text Semibold` (GDI sees the named instances of the variable font) | `\b` gives 700 |
| links | `{\field{\*\fldinst HYPERLINK "url"}{\fldrslt{\cf3 …}}}`, click → `EN_LINK` → `ShellExecuteW` (shell32 is loaded lazily) | RichEdit always draws the underline |
| task checkboxes | glyphs ☑/☐ (Segoe UI Symbol) | — |
| images | `\pict\pngblip`/`\jpegblip`, `\picw` in HIMETRIC, scaled to the column width | **loaded only if the control is NOT read-only during `EM_STREAMIN`** (otherwise silently dropped; `EM_INSERTIMAGE` returns `E_ACCESSDENIED`) |

## Variants (proto.json)

| id | exe / flags | What it is |
|---|---|---|
| **`cpp-richedit-wl-mt`** (best) | `windowless/fastmd-richedit-wl.exe --render=gdi --ts-thread` | windowless RichEdit, GDI `TxDraw` into a memory DC + `BitBlt`. Text services are created and the first chunk is streamed **on a helper thread** while the UI thread creates and shows the (still white) window; then ownership is handed over to the UI thread. Phase 2: background load into a second instance and swap. |
| `cpp-richedit` | `windowed/fastmd-richedit.exe` | a classic child `RICHEDIT50W` (the "inbox control" path from the task); md4c on a worker thread, first chunk, `ImmDisableIME`, read-only via input filtering (no `EM_SETREADONLY`), focus after the first frame. |
| `cpp-richedit-d2d` | `windowless/... --render=d2d-sw --ts-thread --warm-d2d` | windowless + `TXTBIT_D2DDWRITE`, `ITextServices2::TxDrawD2D` into a **software** `ID2D1DCRenderTarget`; a third thread creates the D2D factory/RT in advance and warms up the shared DirectWrite factory. |
| `cpp-richedit-full` | `windowed/... --full` | windowed control, the **whole** document is streamed before the first paint; shows the cost of the naive approach (large: 1.1 s). |

Other flags (for experiments, see `src/common.h`): `--render=d2d` (hardware `ID2D1HwndRenderTarget`),
`--ime` (don't disable IME), `--ro-style` (classic `ES_READONLY` + `EM_SETREADONLY`), `--early-focus`,
`--no-advtypo`, `--serial`, `--first-px=N`, `--chunk=N` (phase 2 in chunks via `SFF_SELECTION`), `--prerender`.

## Style spec support (PROTOCOL §5)

| Element | Status |
|---|---|
| Segoe UI Variable Text 15 px, line-height 1.6, `#1f2328` | ✔ (RTF stores half-points: `\fs23` = 15.33 px; GDI rounds to 15 px, DirectWrite draws 15.33 px) |
| Centered 860 DIP column, 32 DIP margins, white background, scrolling | ✔ (`EM_SETRECT` in windowed, `TxGetClientRect` in windowless); top 24 px, bottom 64 px |
| h1–h6 semibold 30/24/20/17/15/14, h6 `#59636e`, rule under h1/h2 | ✔ |
| Word-wrapped paragraphs | ✔ |
| **bold** / *italic* / ~~strike~~ (muted color) / `inline code` | ✔; the inline code background covers the full line height (24 px), no rounded corners |
| Links `#0969da` | ✔, but RichEdit always underlines them |
| Bulleted, numbered, nested lists, task list | ✔ (markers •◦▪, checkboxes ☑/☐) |
| Blockquotes: 4 px bar `#d1d9e0`, text `#59636e` | ✔; nested blockquotes only by indent, without a second bar |
| `<hr>` | ✔ 2 px |
| Code blocks: Cascadia Mono ~13.5 px, background `#f6f8fa`, padding 16 | ✔; **no radius 6**; long lines **wrap** (no horizontal scrolling); code inside a blockquote is a monospace paragraph without background (nested tables not done) |
| GFM tables: borders, bold header, zebra, alignment, padding 6/13 | ✔ (column widths are an estimate) |
| Images (local PNG/JPEG) | ✔, scaled to the column width; remote URLs → alt text |
| Syntax highlighting | ✘ (easy to add with `\cf` colors in the RTF generator) |
| Cyrillic, CJK | ✔ (RichEdit font binding) |
| Emoji | ⚠ monochrome Segoe UI Emoji glyphs in both modes (GDI can't do color; RichEdit's D2D renderer doesn't enable `ENABLE_COLOR_FONT`, and there is no documented switch) |
| Raw HTML | `<br>` → line break; other inline tags are dropped; HTML blocks as muted monospace text |
| Selection and copying, wheel, PgUp/PgDn/Space/arrows, Home/End, Esc to close | ✔ |
| Per-monitor DPI v2 | manifest + API; tested only at 100 % (96 DPI) |

Screenshots: `shots/final-<variant>-{small,medium}.png`, corpus fragments in `shots/slice1..3.png`,
`shots/wl-slice1.png`; GDI vs D2D close-up in `shots/cmp_gdi_d2d.png`, emoji in `shots/cmp_emoji.png`.

## Quick numbers (noisy: other agents' builds were running in parallel)

`python bench/harness/bench.py run baseline-win32,cpp-richedit-wl-mt,cpp-richedit,cpp-richedit-d2d,cpp-richedit-full --doc small,medium,large --runs 5`
(interleaved, `results/final-quick.json`), median t_content, ms:

| variant | small | medium | large | window visible (obs, medium) | WS medium / large, MB | peak commit medium, MB |
|---|---|---|---|---|---|---|
| baseline-win32 (floor) | 104.3 | 95.8 | 92.2 | 51 | 15.8 | 3.3 |
| **cpp-richedit-wl-mt** | **82.1** | **92.8** | **96.5** | 56 | 16.1 / 43.6 | 4.5 |
| cpp-richedit-d2d | 113.5 | 107.6 | 101.0 | 55 | 27.4 / 54.7 | 61.2 |
| cpp-richedit (windowed) | 100.6 | 116.4 | 125.3 | 89 | 16.3 / 43.5 | 4.4 |
| cpp-richedit-full | 126.9 | 143.0 | **1106.5** | 104 | 19.4 / 81.2 | 5.1 |

On a less loaded machine (A/B, 15 runs, medium, `results/ab-pre.json`): **wl-mt 78.8**, baseline 85.3,
windowless without the thread 94.7 ms. D2D with a hardware `HwndRenderTarget`: ~275–310 ms (`results/ab-d2d.json`).
The wl-mt/large p90 outlier of 1448 ms is the harness's own `CreateProcessW` (launch_overhead 2270 ms); inside the
process it was 58 ms.

### Where the time goes (medians relative to `main`, `results/final-quick.json`)

`main` itself arrives ~31–39 ms after `CreateProcessW` (the same as the baseline: process creation,
Defender, loader); the prototype has no influence on that.

| variant / medium | window created | text services | first chunk in RichEdit | first frame | t_content |
|---|---|---|---|---|---|
| baseline-win32 | — | — | — | +53 | +62 |
| **wl-mt** | +17.6 (UI) | +29.4 (helper) | +38.8 (helper) | +48.9 | **+56.9** |
| windowed | +25.0 | (in the window) | +38.1 | +57.2 | +68.4 |
| d2d (sw, mt, warm) | +16.4 | +15.6 | +19.3 | +63.2 | +68.1 |
| full | +33.0 | (in the window) | +69.2 | +95.9 | +112.1 |

From "first frame" to t_content, 5–8 ms is `DwmFlush` (waiting for DWM composition), as the protocol requires.

### Cost of individual decisions: UI-thread CPU from `main` to the first frame (thread cycles, 12 runs, medium)

Measured with `tools/prof.py` (`QueryThreadCycleTime`, independent of other load):

| configuration | CPU, ms | wall, ms |
|---|---|---|
| windowed RichEdit, all defaults | 37.6 | 50.9 |
| + `--ime` (IME/TSF not disabled) | 50.6 (**+13**) | 70.8 |
| + `--ro-style` (`ES_READONLY` + `EM_SETREADONLY` around the stream) | 45.6 (**+8**) | 61.3 |
| + `--early-focus` (focus before the first frame) | 40.2 (+2.6) | 56.1 |
| "naive": IME + ro-style + early focus | 61.6 | 89.1 |
| `--no-advtypo` (without `TO_ADVANCEDTYPOGRAPHY`) | 38.8 (≈0) | 53.9 |
| `--full` (all of medium before the first frame) | 61.1 | 77.0 |
| windowless GDI, single thread | 38.3 | 52.0 |
| **windowless GDI `--ts-thread`** | **25.7** | **41.5** |
| windowless D2D-sw, single thread | 51.5 | 78.4 |
| windowless D2D-sw `--ts-thread --warm-d2d` | 29.1 | 58.2 |
| windowless D2D, hardware `HwndRenderTarget` | 234.4 | 273.9 |

Breakdown of the windowed variant (CPU): creating the main window ~11 ms, creating `RICHEDIT50W` ~8.6 ms,
`EM_STREAMIN` of the first medium chunk ~9.2 ms (a two-line document: 1.8 ms; CJK font fallback +2.2 ms, emoji
+0.8 ms, Cascadia/table: tenths of a millisecond), `ShowWindow` ~5–6 ms, painting ~3 ms. The whole UI thread for a
two-line document is ~28 ms of CPU, for medium ~36 ms: the main cost is the windows and RichEdit initialization, not
the amount of text.
Windowless text services are cheaper than the window (5–7 ms versus 8.6), and `EM_STREAMIN` in D2D mode is cheaper (≈5 ms: fonts
are realized later, in DirectWrite). Hardware D2D spends ~180 ms of CPU creating the D3D device (loading the
RTX 5070 driver), which is a death sentence for a "bullet"; the software DC RT costs ~14–19 ms (WARP rasterizer), and it can be
hidden in a parallel thread.

Parallelism works worse than the cycle counts promise: `CreateTextServices` on the helper thread takes ~25–34 ms of wall time
at ~6 ms of CPU, because it competes with window creation on the UI thread for shared USER/GDI locks. Still, the net result is
−12 ms to t_content (A/B: 78.8 versus 94.7 ms).

### Large documents

* md4c + RTF generation, 3.7 MB md → 25.3 MB RTF: ~45 ms (worker thread, in parallel with the UI).
* RTF size: small 3.4 KB → 34 KB, medium 53 KB → 396 KB, large 3.7 MB → 25.3 MB (every non-ASCII character becomes
  `\uNNNN?`, plus table markup).
* Full load of large into RichEdit: ~0.9–1.1 s (`EM_STREAMIN` in one chunk is linear).
* **Trap**: appending chunks at the end (`EM_EXSETSEL` to the end + `EM_STREAMIN|SFF_SELECTION`) becomes O(chunk × document) after ~300 K
  characters in the document: a 256 KB chunk at 320 K characters takes 0.54 s, at 845 K
  1.4 s; a 2 MB chunk at 440 K takes 7 s. That is why phase 2 is one full load (or a background second instance).
* RichEdit accepts UTF-8 RTF (`SF_USECODEPAGE | CP_UTF8<<16`, raw bytes instead of `\u`), and the text is identical,
  but it is **slower**: large 2.5 s versus 0.96 s, medium 144 versus 86 ms; the RTF is only 15 % smaller.
* RichEdit lays out a large document lazily (background recalculation on a timer), so the scroll thumb "grows", and
  `SB_BOTTOM` only reaches what has been laid out so far; End/Home are implemented via the caret + `EM_SCROLLCARET`
  (`shots/end_large.png`).

## D2D/DirectWrite: investigation

* **The windowed `RICHEDIT50W` draws only via GDI/Uniscribe.** The probe `probe/probe_d2dswitch.cpp`, in a separate
  process per bit, tried all 32 bits of `EM_SETEDITSTYLEEX`, `EM_SETEDITSTYLE` and `EM_SETTYPOGRAPHYOPTIONS`
  on a visible window: none of them loaded `d2d1.dll`/`dwrite.dll` (msftedit loads them lazily, so this is a reliable
  indicator). The UTF-16 strings of `msftedit.dll` contain no window class name other than `RICHEDIT50W` (classes with D2D in
  the name, as far as is known, exist only in Office's `riched20.dll`, which isn't on this system; not checked).
* **The windowless path works on the first try**: a custom `ITextHost2`, the `TXTBIT_D2DDWRITE` bit in `TxGetPropertyBits`,
  drawing via `ITextServices2::TxDrawD2D(rt, &bounds, NULL, TXTVIEW_ACTIVE)`; `CreateTextServices` and
  `IID_ITextServices2/IID_ITextHost2` are taken from `msftedit.dll` exports. The host is ~250 lines (`src/main_wl.cpp`).
  Judging by the `Windows.UI.Text.RichEditTextDocument` strings in `msftedit.dll`, XAML's
  `RichEditBox` uses the same engine.
* D2D quality: DirectWrite, fractional font sizes, ClearType in the DC RT; visually almost the same as GDI (`shots/cmp_gdi_d2d.png`).
  No color emoji there either.
* Speed: software RT + warm-up on a separate thread: 101–114 ms; hardware: ~275–310 ms.

## Traps I stepped into

1. **Paragraph** shading/borders are not drawn → everything "block-level" is done with tables; a borderless table draws a gray grid.
2. Character shading fills the whole line box together with `\sb` → spacing is moved into spacer paragraphs.
3. `\slmult` is relative to the font's line height, not the font size → exact spacing only.
4. Images are silently not loaded into a read-only control; and every `EM_SETREADONLY` transition on a fresh control costs
   5–10 ms of CPU → the control stays "editable", while editing is filtered out (WM_CHAR, Del/Backspace/Enter,
   Ctrl+V/X/Z/Y and formatting hotkeys, `WM_PASTE/CUT/CLEAR/UNDO`, `ES_NOOLEDRAGDROP`, IME off; in windowless the
   host simply doesn't pass these messages on).
5. The default RichEdit text limit is 32 K characters → `EM_EXLIMITTEXT`.
6. IME/TSF initialization (~20 ms) on window creation/activation/focus → `ImmDisableIME(-1)`.
   The flip side: the process will have no IME (for a future search field with CJK input: a separate thread/window).
7. Appending in chunks is quadratic (see above).
8. Windowless from two threads: while the helper works with text services, the host must not touch the window
   (a cross-thread `SendMessage` to a UI thread that is waiting for the handoff = deadlock) and must refuse `TxSetTimer`;
   the helper thread stays alive (RichEdit TLS caches).
9. A bug of my own, found and fixed: a double `DwmFlush` (profiling + protocol) added up to a frame to
   t_content of the windowed variant.
10. RichEdit can't be asked to draw links without underline or rounded backgrounds.

## Ideas for further speedups

* **`ImmDisableIME(-1)` for all prototypes** (−20 ms on the baseline). For Win32/D2D/Rust prototypes with their own
  window it is one line; for managed/XAML stacks I didn't check the effect.
* Create the window as `WS_VISIBLE` right away and/or show it even earlier (currently the empty window is shown at ~+39 ms).
* Reduce thread contention for USER/GDI (for example, the helper does the CPU work first and `CreateTextServices`
  once the UI thread has created the window); on an idle machine the effect will be smaller than under load.
* A smaller first chunk (`--first-px=900`: −1 ms CPU) with coverage checked via `EM_POSFROMCHAR`; don't pull images below the first viewport
  into the first chunk (the WIC decoder adds +10–15 ms).
* A `-resident` variant (a process with an already created RichEdit + passing the path): not done; going by the breakdown above, the resident
  process would only have to stream the first chunk, paint and `DwmFlush` (an estimate, not a measurement: ~20–30 ms plus
  the launcher start).
* Syntax highlighting is cheap: RTF colors from a simple lexer (almost no effect on startup).

## Effort estimate to a product

Turning the prototype into a decent v1 on this stack: **~3–5 weeks** for one Win32/C++ developer: opening files,
drag&drop, reload on change, search (`EM_FINDTEXTEX`), zoom (`EM_SETZOOM`), dark theme (a different palette
in the RTF generator), printing (RichEdit has `EM_FORMATRANGE`), table of contents, syntax highlighting (~1 week),
testing at 125–200 % DPI and on DPI changes, file association/installer. The quality ceiling is limited by RichEdit:
rounded corners, color emoji, exact CSS table layout, horizontal scrolling of code, `text-decoration: none` are
unreachable without moving to a custom renderer (DirectWrite), i.e. a different prototype (cpp-d2d).

## Files and tools

* `src/md2rtf.cpp`: md4c → RTF (all the techniques from the table above), height estimation for the first chunk.
* `src/main_gdi.cpp`: windowed host; `src/main_wl.cpp`: windowless host (GDI/D2D, helper thread, background
  load); `src/common.h`: command-line parsing without shell32, worker thread, EM_STREAMIN, profiler.
* `tools/prof.py <exe> <doc> [runs] [args]`: CPU/wall per UI-thread phase (via `FASTMD_PROF`).
* `tools/scrolltest.py <variant> <doc> <prefix>`: launches a variant through the harness module, sends PgDn/wheel/End,
  takes screenshots.
* `probe/`: probes of RichEdit's RTF capabilities (`mkprobe*.py`, `probe*.rtf`, screenshots), images
  (`probe_img.cpp`) and the D2D switch (`probe_d2dswitch.cpp`).
* Debug environment variables: `FASTMD_PROF`, `FASTMD_TRACE=<file>` (full load time),
  `FASTMD_DUMP_RTF=<file>`, `FASTMD_DUMP_TEXT=<file>` (windowed), `FASTMD_LOAD_RTF=<file>` (show a ready-made RTF),
  `FASTMD_NO_MORE` (windowed: skip phase 2).
* `results/*.json`: raw harness runs (`final-quick.json` is the final one, the others are A/B along the way).

## Audit (independent auditor, 2026-09-19)

Checked: `PROTOCOL.md` §3–§7, `proto.json`, `build.ps1`, all host code (`main_wl.cpp`, `main_gdi.cpp`,
`common.h`, `md2rtf.cpp`). Other agents were loading the machine in parallel, so all numbers below are noisy.

### Honesty of t_content: verdict, honest

* **Build.** `build.ps1` was rebuilt from scratch (twice: before and after the fixes): `/O2 /MT /GS- /GL /DNDEBUG` + `/LTCG
  /OPT:REF,ICF`. `dumpbin /dependents`: only `USER32, GDI32, dwmapi, IMM32, KERNEL32` (+ delay-loaded `d2d1`,
  `DWrite`), no debug CRTs. exe 262 / 272 KB.
* **Mark.** In all three hosts `t_content` is taken in `fmb_content_presented_ex()` (the shared
  `bench/common/fastmd_bench.h`): `DwmFlush()` → `GetSystemTimePreciseAsFileTime`. The call comes right after `EndPaint`
  of the frame with the document: windowed, after the regular `WM_PAINT` of `RICHEDIT50W` itself (subclassing); windowless-GDI, after
  `TxDraw` into a memory DC + `BitBlt` + `EndPaint`; windowless-D2D, after `TxDrawD2D` + `EndDraw` + `EndPaint`. Everything is
  synchronous; nothing marks "frame ready" earlier.
* **First viewport first.** Before `t_content` only the first RTF chunk is loaded into RichEdit (height estimate ≥ 1400 px with an
  800 px window); this is a technique §3 allows. I checked that the chunk really covers the window: the windowed variant with
  `FASTMD_NO_MORE=1` (phase 2 is not executed): `shots/audit2-firstchunk-windowed-{medium,large}.png`; for medium
  the frame from the first chunk alone is pixel-identical to the frame of the whole document (3 caret pixels differ).
  wl-mt on large with `--settle 0` (before the background swap, which happens after ~1 s):
  `shots/audit2-firstchunk-wlmt-large-settle0.png`: the screen is fully filled.
* **No dishonest techniques.** No resident process, no on-disk caches (files are written only under the debug
  `FASTMD_DUMP_*`/`FASTMD_TRACE`/`FASTMD_PROF`, which the harness doesn't set), `--prerender` is not used in `proto.json`.
  Nothing from the corpus is baked into the binary: the only table (`kAdv`) holds glyph widths of the Segoe UI
  Variable font, not corpus text. `tests/slice*.md` are just input files for manual checks and are not part of the build.
  `ImmDisableIME(-1)` is a legitimate technique for a viewer (the flip side is described in "Traps", item 6).
* **observed_ms** (the harness's independent observation of the window) is **earlier** than `t_content` in all 16 checks
  (the window becomes visible at `ShowWindow`, the content is drawn later), i.e. the mark is not too early.

`bench.py check` after the fixes (1 run, noisy), ms from `CreateProcessW`:

| variant | medium: content / window / observed | large: content / window / observed |
|---|---|---|
| cpp-richedit-wl-mt | 129.7 / 110.1 / 74.6 | 92.0 / 79.0 / 57.2 |
| cpp-richedit | 112.6 / 105.1 / 86.9 | 211.9 / 203.1 / 163.9 |
| cpp-richedit-d2d | 151.5 / 131.4 / 73.4 | 117.9 / 102.0 / 67.3 |
| cpp-richedit-full | 127.2 / 119.9 / 99.0 | 1160.5 / 1150.1 / 1121.7 |

All `CHECK OK`, no warnings, `clean_exit=true`, exit code 0. (Before the fixes, at a quieter moment: wl-mt
100.4 / 81.2, windowed 130.8 / 95.3, d2d 90.8 / 99.8, full 116.4 / 1022.7 ms for medium / large.)

### What was changed (minimally) and why

1. **`src/main_wl.cpp`: phase 2 (background load of the whole document, `bg_load_full`) now starts only after
   the first shown frame** (the `g_ev_presented` event is set in `on_first_frame()` after `t_content`; the helper thread
   waits for it). Previously, in normal mode (without `FASTMD_BENCH_OUT` or with `FASTMD_BENCH_EXIT=0`), the second text
   services instance was created and loaded **in parallel with the first paint**, while in measurement mode (`EXIT=1`) it didn't run
   at all. So the measurement wasn't quite measuring the path the user sees. The first A/B showed +13 ms (medium) /
   +30 ms (large) in normal mode, but a repeated A/B with alternating order (old binary vs new, both modes,
   n=8, in-process interval `t_content − main`) didn't confirm a difference above noise: old, large 58.6 (EXIT=1)
   versus 55.4 (EXIT=0), medium 46.8 versus 52.0 ms; new, large 57.5 / 53.8, medium 51.6 / 51.8 ms. The first
   discrepancy was apparently an effect of the fixed run order. The fix was kept as a safeguard: now, before
   `t_content`, normal mode by construction does exactly the same work as measurement mode. **The path the
   harness measures did not change** (only a `CreateEventW` at startup and a `SetEvent` after `t_content` were added).
   Item 5 of the "How it works" section should be read this way for wl-mt: the second instance is loaded *after* the first frame.
2. **`src/main_wl.cpp`, D2D mode: `t_window` was taken on the frame with content** rather than on the first (white) frame of the window;
   for `cpp-richedit-d2d`, `window_ms` coincided with the `painted` mark. Added `fmb_window_shown()` after `EndPaint`
   of every D2D frame. No effect on `t_content`; now d2d `window_ms` ≈ `shown_blank` (131.4 versus 151.5 ms in the
   check above).

`proto.json` was not changed: all four variants work and are meaningful.

### Visual assessment (PROTOCOL §5)

Audit screenshots: `shots/audit2-<variant>-{small,medium}.png`, corpus fragments below the first viewport in
`shots/audit2-wlmt-slice{1,2}.png`. Colors were checked by pixel: link `#0969da`, code background and zebra `#f6f8fa`,
borders `#d1d9e0`, text `#1f2328`, exactly per spec.

* Good: h1–h6 with the right size hierarchy, semibold 600, rules under h1/h2; 15 px paragraphs, 24 px line spacing;
  centered 860 DIP column; tables with borders, bold header, zebra and column alignment; Cascadia Mono code
  blocks on `#f6f8fa` with 16 px padding; blockquotes with a 4 px bar and muted text; nested
  bulleted/numbered lists (• ◦ ▪), task list (☑/☐); `<hr>`; local PNGs scaled to the column width;
  clean Cyrillic and CJK, ClearType.
* Worse than GitHub: links are always underlined; emoji are monochrome (Segoe UI Emoji without color, including in D2D); no
  rounded corners (code, inline code); the inline code background spans the full line height; no syntax highlighting; code inside a blockquote
  has no background; a nested blockquote has no second bar; table column widths are an estimate.
* A cosmetic defect of the windowed variant (`cpp-richedit`, `-full`): a tiny (1×3 px) blinking caret is visible in the top-left corner,
  in the top spacer paragraph (`HideCaret` doesn't keep it hidden after focus/phase 2). The windowless variants
  have no caret. It doesn't affect the measurement; not fixed.
* The D2D variant is visually almost identical to GDI (DirectWrite, fractional 15.33 px size, slightly different line breaking).

Rating: **visual quality 7/10, GitHub fidelity 7/10**: the best "native" result without a custom layout
engine, but the RichEdit ceiling (underlined links, monochrome emoji, no rounded corners) is noticeable.

### Control run

`python bench/harness/bench.py run baseline-win32,cpp-richedit-wl-mt,cpp-richedit,cpp-richedit-d2d,cpp-richedit-full
--doc small,medium --runs 5` → `results/audit-sanity.json` (median t_content, ms; noisy, other agents were running
in parallel):

| variant | small | medium | window (medium) | observed (medium) | WS medium, MB | peak commit medium, MB |
|---|---|---|---|---|---|---|
| baseline-win32 (floor) | 86.2 | 84.0 | 75.7 | 55.4 | 14.0 | 3.1 |
| **cpp-richedit-wl-mt** | **79.6** | **79.2** | 65.0 | 52.9 | 16.2 | 4.5 |
| cpp-richedit (windowed) | 94.3 | 102.6 | 93.6 | 76.0 | 16.0 | 4.3 |
| cpp-richedit-d2d | 96.4 | 101.8 | 84.3 | 55.6 | 28.6 | 61.9 |
| cpp-richedit-full | 119.7 | 129.6 | 123.8 | 104.8 | 19.4 | 5.1 |

Audit verdict: the README's claims hold up: wl-mt is at the level of the baseline-win32 calibration floor (and slightly
faster than it thanks to `ImmDisableIME`). All four variants are fit for the final sequential measurement;
`cpp-richedit-full` is the "naive" reference variant (the whole document before the first frame), and on large it is, as expected, ~1 s.
