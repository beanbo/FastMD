[Русский](README.md) | **English**

# cpp-d2d — C++20 · raw Win32 · DirectWrite (+Direct2D) · md4c

A FastMD prototype on "bare" Win32 with its own Markdown renderer on top of DirectWrite.
The "maximum speed" candidate.

## TL;DR

* **The best honest `cpp-d2d` variant**: DirectWrite rasterizes glyphs straight into a GDI DIB (no Direct2D/Direct3D and no
  GPU driver load); the first viewport is parsed, laid out and drawn on a worker thread *in parallel* with
  window creation. On a noisy machine (other agents kept the CPU at 55–65 %) the median `t_content` was
  **≈ 64–80 ms** for small/medium/large. Under the same conditions `baseline-win32` (an empty window + one GDI line) showed
  96.8 ms and `cpp-d2d` 81.2 ms: the prototype is **faster than the calibration "floor"**.
* The whole critical path belongs to the OS: CreateProcess, the loader, CreateWindowEx, ShowWindow and DwmFlush. The
  document pipeline itself (read, md4c, DirectWrite, layout, render) takes ~10–15 ms and is completely hidden behind
  window creation.
* **The GPU only gets in the way here.** Creating a hardware D3D11 device on the RTX 5070 costs ≈ 200 ms, and on the iGPU the
  swap chain is expensive (the display is attached to the dGPU). `cpp-d2d-gpu` gives ≈ 300–400 ms, WARP (software D3D) ≈ 100–140 ms.
* **`cpp-d2d-resident`** (a pre-started process, not directly comparable with a from-scratch start): **≈ 30–42 ms**.
  This includes a full launch of a second process, `fastmd-d2d.exe --forward`, which hands the path to the resident.
* Quality: GitHub-like typography per the PROTOCOL §5 spec, color emoji, CJK, zebra-striped tables, checkboxes,
  syntax highlighting (16 languages), local images via WIC.

## Stack and versions

| Component | Version / details |
|---|---|
| Compiler | MSVC 19.44.35209 (VS 2022 Community, toolset 14.44), C++20, `/O2 /Oi /GL /Gy /GS- /MT`, link `/LTCG /OPT:REF /OPT:ICF` |
| SDK | Windows SDK 10.0.26100 |
| Parser | md4c **0.5.3** (github.com/mity/md4c) in a UTF-16 build (`MD4C_USE_UTF16`), vendored in `third_party/md4c/`. One patch in `md4c.c`: `md_analyze_marks` uses `ISANYOF_(mark->ch, _T("*_~"))` instead of a narrow literal, otherwise the UTF-16 build breaks (upstream bug) |
| Text | DirectWrite (`IDWriteFactory3`, `IDWriteFactory4::TranslateColorGlyphRun` for color emoji), `IDWriteBitmapRenderTarget` (GDI interop) |
| Graphics | `gdi` back-end: DirectWrite → 32-bpp DIB + a custom SDF shape rasterizer; `dxgi` back-end: Direct2D 1.1 `ID2D1DeviceContext` on top of D3D11 (HW or WARP) + DXGI flip-model swap chain; plus `hwnd` (ID2D1HwndRenderTarget) and `dcrt` (ID2D1DCRenderTarget, software) |
| Images | WIC (via `CoCreateInstance`) on a background thread after the first frame; PNG/JPEG/GIF dimensions are read from the file header before decoding |
| exe imports | statically only `KERNEL32`, `USER32`, `GDI32`, `dwmapi`; delay-load: `d2d1`, `DWrite`, `d3d11`, `dxgi`, `ole32`, `SHELL32`, `IMM32` |
| Size | `fastmd-d2d.exe` 416 KB (static CRT, no dependencies): this is the entire distribution |

There are no other dependencies: no vcpkg, no NuGet, no runtime.

## Build

```
pwsh -File bench/protos/cpp-d2d/build.ps1        # ~20 с, неинтерактивно, с нуля
```

The script deletes `out/`, `out-resident/` and `obj/`, then compiles `md4c.c` (C, UTF-16) and six `.cpp` files and links
`out/fastmd-d2d.exe`. The same file is copied to `out-resident/fastmd-d2d.exe` (for the reason, see the "Resident" section).
The experimental `obj/fastmd-open.exe` is not part of any variant. The compiler is invoked through
`bench/tools/msvc.cmd`, so a Developer Prompt PATH is not needed.

## Startup architecture

```
UI-поток   : fmb_init → SetProcessDpiAwarenessContext(PMv2) → разбор argv → старт doc-потока
             → ImmDisableIME(0) → RegisterClassEx → CreateWindowEx (+DWMWA_TRANSITIONS_FORCEDISABLED)
             → ShowWindow → первый WM_PAINT: join(doc) → BitBlt готового кадра → EndPaint → DwmFlush → t_content
doc-поток  : ReadFile → UTF-8→UTF-16 → md4c → плоская модель (blocks + inline runs)
             → DWriteCreateFactory + 8 IDWriteTextFormat → оценка высот всех блоков
             → IDWriteTextLayout только для блоков ПЕРВОГО ЭКРАНА → рендер кадра в DIB (back-end gdi)
dev-поток  : (только dxgi/hwnd) D3D11CreateDevice + D2D device/context
после кадра: N потоков (до 8, BELOW_NORMAL) считают точные высоты остальных блоков → точный scroll extent;
             WIC-декодирование картинок; для больших файлов подмена префиксной модели на полную
```

* **Document model**: flat arrays of `Block` (text, code, hr, table, image; indent, vertical gap,
  list/checkbox marker, muted color) and `Run` (bold/italic/code/strike/link + color). All text is a single
  `std::wstring`. One `IDWriteTextLayout` per text block; a table gets a grid of layouts (one per cell).
* **Virtualization (first-viewport-first)**: before the first frame only the visible blocks are laid out (in medium that is
  21 of 679). For the rest the height is first estimated (exactly for code and hr, from the file header for
  images) and refined by background threads after the first frame. The top visible block is "anchored", so
  the page doesn't jump. This is a technique the protocol allows (PROTOCOL §3), and `t_content` is honest: at the moment of the
  mark the first viewport is fully and finally drawn.
* **Prefix parsing of large files** (> 256 KB): for the first viewport md4c parses a prefix of ≥ 48 K characters up to
  the nearest top-level heading outside fenced code, so the resulting blocks are exactly the same. The full parse
  runs in parallel on its own thread, and the full model replaces the prefix model right after the first frame.
* **`gdi` back-end**: `IDWriteBitmapRenderTarget` draws glyphs (ClearType/grayscale, as configured in the system) into the
  DIB. Color emoji are drawn layer by layer (COLR) via `TranslateColorGlyphRun`, because
  `IDWriteBitmapRenderTarget3::DrawGlyphRunWithColorSupport` is missing on Win11 26200. Rounded
  rectangles, circles and checkbox ticks are drawn by a small anti-aliased SDF rasterizer. The frame reaches the window
  with a single `BitBlt`.
* Wheel scrolling is animated (smoothed), and there is keyboard support (↑↓, PgUp/PgDn/Space, Home/End, Esc/Ctrl+W to
  close). The scrollbar is custom: overlay, "hot" on hover, thumb dragging, click on the track. Links show a
  hand cursor and open in the browser on click (`ShellExecute`). Resizing the window triggers an anchored relayout,
  and `WM_DPICHANGED` is handled.

## Variants (`proto.json`)

| id | exe / args | What it is |
|---|---|---|
| **`cpp-d2d`** (best) | `out/fastmd-d2d.exe --backend=gdi --no-ime --no-anim` | DirectWrite → GDI DIB. The frame is ready on the doc thread before ShowWindow. IME is disabled on the UI thread, the DWM open animation is disabled |
| `cpp-d2d-warp` | `--backend=dxgi --warp --no-ime --no-anim` | Direct2D DeviceContext on D3D11 **WARP** (CPU) + flip swap chain; the device is created on the dev thread |
| `cpp-d2d-gpu` | `--backend=dxgi --no-ime --no-anim` | "Classic" Direct2D on hardware D3D11 (default adapter = RTX 5070) + flip swap chain |
| `cpp-d2d-resident` | `out-resident/fastmd-d2d.exe --forward`; resident: `out-resident/fastmd-d2d.exe --resident --backend=gdi --no-ime --no-anim` | **RESIDENT.** The process is started in advance (PROTOCOL §6); a new instance hands it the path via `WM_COPYDATA` and exits |

The ids `cpp-d2d-exp`, `cpp-d2d-shot`, `cpp-d2d-trace` appear only in the dev scripts under `tools/`. A variant with a
separate launcher (`fastmd-open.exe`) did not make it into `proto.json`: it is slower, see the "Resident" section.

### Command-line switches (for experiments)

| Switch | Effect |
|---|---|
| `--backend=gdi\|dxgi\|hwnd\|dcrt` | back-end (default `dxgi`) |
| `--warp` / `--igpu` | for `dxgi`: WARP device / hardware device on the `MINIMUM_POWER` adapter (AMD iGPU) |
| `--no-ime` | `ImmDisableIME(0)` for the UI thread (a viewer has no text input on that thread) |
| `--no-anim` | `DWMWA_TRANSITIONS_FORCEDISABLED`: the window appears immediately at full opacity, without fade/zoom |
| `--serial` | everything on the UI thread, no worker threads (for comparison) |
| `--no-virt` | lay out the whole document before the first frame |
| `--no-prefix` | don't use prefix parsing for large files |
| `--show-late` | show the window only once the first frame is ready |
| `--signed-only` | `ProcessSignaturePolicy MicrosoftSignedOnly` (blocks injected third-party DLLs) |
| `--trace` | log of DLL loads, first window messages and marks → `%TEMP%\fastmd-d2d-trace.txt` |
| `--scroll-test=N` | after the first frame draw N scroll frames, statistics → `%TEMP%\fastmd-d2d-scroll.txt` |
| `--scroll=Y`, `--size=WxH`, `--show-cmd=N` | debugging: initial scroll position, client area size in DIP, ShowWindow command |
| `--resident` / `--forward` / `--no-preshow` | resident mode / hand the document to the resident if it is running / don't keep the spare window shown (cloaked) |
| `--id=<variant>` | variant id for the window title |

## Spec support (PROTOCOL §5)

| Requirement | Status |
|---|---|
| Per-monitor DPI v2 | ✔ `SetProcessDpiAwarenessContext` first thing in `wWinMain`, crisp text, `WM_DPICHANGED` handled |
| Client area 1000×800 DIP, normal activated window | ✔ (`AdjustWindowRectExForDpi`) |
| Title `<file> — FastMD (<id>)` | ✔ |
| Light theme, centered 860 DIP column, 32 DIP margins, background `#ffffff` | ✔ |
| Body font: Segoe UI Variable Text (fallback Segoe UI), 15 DIP, line-height 1.6 (24 DIP), `#1f2328` | ✔ (DirectWrite line spacing is uniform; the baseline follows the CSS half-leading rule) |
| h1–h6: 30/24/20/17/15/14, semibold, h6 `#59636e`, 1 px `#d1d9e0` rule under h1/h2 | ✔ |
| Word-wrapped paragraphs | ✔ (emergency break for very long words/URLs) |
| **bold** / *italic* / `code` / ~~strike~~ / links `#0969da` | ✔ (inline code: Cascadia Mono 85 %, background `#eff1f3`, radius 4, inner padding ≈ 0.2em/0.4em) |
| Bulleted, numbered, nested lists, task lists | ✔ (per-level markers ● ○ ■, numbers right-aligned, checkboxes: filled blue with a tick / empty) |
| Blockquotes: 4 px bar `#d1d9e0`, text `#59636e` | ✔ (nested ones get several bars) |
| Horizontal rule | ✔ (2 px, `#d1d9e0`) |
| Fenced code: Cascadia Mono (fallback Consolas) 13.5, background `#f6f8fa`, radius 6, padding 16 | ✔ **no wrapping**: long lines are clipped at the block's right edge, there is no horizontal scrolling |
| Syntax highlighting | ✔ a simple single-pass lexer (comments, strings, numbers, keywords, types, calls, `$vars`) with the GitHub light palette for c, cpp, csharp, js/ts, rust, python, powershell, json, go, java, bash, sql, css, xml/html, yaml, toml. It is not a real grammar |
| GFM tables: 1 px borders, bold header, zebra on even rows, padding 6/13, column alignment | ✔ Column widths are computed from the natural and minimum content width. A table that is too wide is clipped (no horizontal scrolling) |
| Unicode: Cyrillic, CJK, emoji | ✔ DirectWrite system font fallback; color emoji (Segoe UI Emoji, COLR) in all back-ends |
| Images (local PNG) | ✔ decoded via WIC in the background after the first frame. For PNG/JPEG/GIF space is reserved immediately from the size in the file header, so the layout doesn't jump. Other WIC formats are shown too, but their space appears only after decoding. Remote (`http…`) and `data:` images are not loaded: a placeholder with the alt text is shown instead |
| Scrolling | ✔ wheel (smooth), keyboard, custom overlay scrollbar |
| HTML blocks / inline HTML | ~ tags are stripped, text is kept, `<br>` becomes a line break |
| Not supported | text selection and copying, search, zoom, dark theme, `#…` anchor navigation, footnotes (md4c doesn't have them), math, mermaid, horizontal scrolling of code and tables, printing, UI Automation (screen readers) |

Screenshots are in `shots/`: `cpp-d2d-{medium,small}.png`, `cpp-d2d-warp-*`, `cpp-d2d-gpu-*`, `cpp-d2d-resident-*`
(taken with `bench.py shot`), plus `detail-medium-scroll{900,1700,2500}.png` (tables, an image, h5/h6, hr, C#),
`detail-medium-after-wheel.png` (after 25 wheel notches) and `detail-large-resized-narrow.png` (window shrunk to 640 px).
All were reviewed. Rendering is visually identical across all back-ends.

## Quick measurements

**Conditions:** a shared machine with other agents running in parallel (CPU load 55–65 %, CreateProcess ≈ 20–28 ms
versus ≈ 6 ms at quiet moments). These are rough numbers; the final sequential measurement is done by the orchestrator.
`bench.py run cpp-d2d,cpp-d2d-warp,cpp-d2d-gpu,cpp-d2d-resident --doc small,medium,large --runs 5`
(interleaved, n = 6 including the warm-up). Results file: `results/quick-run-2.json`.

| variant | doc | median `t_content`, ms | p90 | min | `t_window` | observed | CPU, ms | commit, MB | WS, MB |
|---|---|---|---|---|---|---|---|---|---|
| **cpp-d2d** | small | **70.7** | 91.8 | 61.4 | 63.3 | 56.8 | 31 | 7.0 | 17.2 |
| **cpp-d2d** | medium | **70.0** | 80.2 | 67.0 | 61.2 | 54.8 | 47 | 7.0 | 17.5 |
| **cpp-d2d** | large | **80.4** | 88.1 | 64.4 | 71.9 | 63.2 | 78 | 27.1 | 32.3 |
| cpp-d2d-warp | small | 98.6 | 132.1 | 96.7 | 70.7 | 63.4 | 63 | 72.9 | 34.7 |
| cpp-d2d-warp | medium | 139.6 | 151.5 | 122.0 | 71.3 | 65.4 | 109 | 76.8 | 39.4 |
| cpp-d2d-warp | large | 121.7 | 137.6 | 113.9 | 68.4 | 62.3 | 141 | 93.0 | 53.8 |
| cpp-d2d-gpu | small | 306.8 | 327.7 | 289.0 | 64.3 | 57.2 | 281 | 123.5 | 54.4 |
| cpp-d2d-gpu | medium | 393.9 | (3.6 s outlier) | 351.2 | 74.0 | 65.4 | 344 | 134.2 | 55.9 |
| cpp-d2d-gpu | large | 395.1 | 422.8 | 338.1 | 95.0 | 80.4 | 375 | 151.6 | 70.4 |
| cpp-d2d-resident | small | 34.8 | 44.5 | 27.3 | 28.0 | 28.8 | 16¹ | 1.8¹ | 18.5 |
| cpp-d2d-resident | medium | 41.6 | 42.9 | 28.1 | 31.0 | 32.6 | 16¹ | 1.8¹ | 18.5 |
| cpp-d2d-resident | large | 42.3 | 55.5 | 38.0 | 35.3 | 37.3 | 16¹ | 1.8¹ | 30.4 |

¹ For resident, CPU and commit cover only the forwarding process in the job. WS is the resident's working set.

Under the same conditions, back to back (`results/quick-vs-baseline.json`, medium, n = 8): `baseline-win32` 96.8 ms, `cpp-d2d` 81.2 ms.
Single `bench.py check` runs at the end of the session (machine slightly quieter): cpp-d2d 63/85/71 ms (s/m/l), warp 101/122/125, gpu
322/318/386, resident 29/31/36.

### Breakdown by marks (medians, ms from CreateProcess; `tools/marks.py results/quick-run-2.json`)

`cpp-d2d`, medium: `launch` (CreateProcess return) 25.3 → `main` 38.0 → `file_read` 38.7 → `parsed` 39.3 →
`dwrite_ready` 41.0 → `layout_first_viewport` 50.6 → `window_created` 52.3 → `rendered_offscreen` 54.5 →
`shown` 62.7 → `first_present` 63.1 → **`t_content` 70.0** (DwmFlush ≈ 7).

`cpp-d2d`, large (3.7 MB): `main` 42.4 → `file_read` 44.2 → `parsed_prefix` 48.5 → `dwrite_ready` 56.3 →
`layout_first_viewport` 58.4 → `window_created` 60.7 → `rendered_offscreen` 66.1 → `shown` 73.8 →
`first_present` 74.7 → **80.4**. The full parse (3.7 MB, ≈ 27 ms) finishes after the first frame. Exact
heights of all blocks (`measured_all`, 8 threads) are ready about 690 ms after start (per `--trace`); until then
the scrollbar works from estimated heights.

`cpp-d2d-warp`, medium: … `window_created` 62.7, `shown` 72.8, `d3d_device` 77.1, `swapchain` 86.7, **`rendered`
123.5** (the first Direct2D frame on WARP ≈ 37 ms: d2d1 initialization and the software rasterizer), `first_present` 130.2.

`cpp-d2d-gpu`, medium: … `shown` 74.9, **`d3d_device` 280.4** (D3D11CreateDevice on NVIDIA ≈ 200–240 ms:
loading the UMD driver), `swapchain` 295.1, `rendered` 366.8 (first frame ≈ 70 ms), `first_present` 388.5.

`cpp-d2d-resident`, medium: `launch` 5.6 → `launcher_main` (main of the forwarding process) 20.2 → `resident_recv` 21.5 →
`file_read` 22.0 → `parsed` 22.9 → `layout_first_viewport` 24.0 → `rendered_offscreen` 28.0 → `shown` 30.0 →
`blitted` 30.8 → `uncloaked` 31.0 → **41.6** (≈ 31 at a quiet moment).

### Knob A/B (`tools/exp.py`, medium, n = 5 per configuration, one session: compare within the table)

| Configuration | median `t_content`, ms | Conclusion |
|---|---|---|
| `--backend=gdi --no-ime --no-anim` (= cpp-d2d) | **64.0** | best |
| … `--serial` (everything on the UI thread) | 70.1 | the parallel pipeline gives −6 ms |
| … `--no-virt` (lay out the whole document) | 83.5 | virtualization gives −20 ms on medium; **on large 2128 ms versus 72.5** |
| `--backend=gdi` without knobs | 75.9 | the knobs together give −12 ms |
| `--backend=gdi --no-anim` (IME enabled) | 85.0 (the reference in this series was 75.5) | ShowWindow with IME: `shown − window_created` 22.6 versus 12.4 ms, i.e. **IME costs ≈ 10 ms** (TSF/IME on activation) |
| `--backend=gdi --no-ime` (animation enabled) | 71.4 | `--no-anim` barely affects the mark, but without it the window fades in over ~150–200 ms, so it helps perceptually |
| … `--show-late` | 70.6 (reference 75.5) | no gain |
| … `--signed-only` | 68.1 (reference 67.8) | no gain (the attempt to inject the RTSS hook costs time anyway) |
| large: prefix parsing / `--no-prefix` | 72.5 / 86.0 | the prefix gives −13 ms on 3.7 MB |
| `--backend=dcrt` (D2D DCRenderTarget, software) | 103.0 | first software D2D frame ≈ 50 ms |
| `--backend=hwnd` (ID2D1HwndRenderTarget) | 300.2 | a HW device is created internally |
| `--backend=dxgi` (HW, RTX 5070) | 323.8 | D3D11CreateDevice ≈ 190–200 ms |
| `--backend=dxgi --igpu` (HW, AMD iGPU) | 347.9 | device ≈ 40–60 ms, but swap chain ≈ 190 ms (display on the dGPU, cross-adapter) |

### Scrolling (steady state, `--scroll-test=240`, medium, 1000×800 px, 3440×1440 @ 165 Hz display)

| Back-end | render+present p50 / p95 / max, ms | frame with DwmFlush, p50 |
|---|---|---|
| gdi | 2.12 / 2.56 / 2.84 | 6.05 |
| dxgi WARP | 2.65 / 4.44 / 7.97 | 6.58 |
| dxgi HW (RTX) | 11.10 / 13.94 / 16.49 | 17.96 (doesn't fit 165 Hz) |

A previous agent measured with a 3300×1300 px window: gdi 6.4 ms, WARP 3.9 ms, HW 9.0 ms. So the software back-end
holds 165 Hz even in a nearly full-screen window on this display, while hardware Direct2D is slower here than both
software ones. The likely cause is synchronization and uploading glyphs to the GPU on every frame; this was not investigated further.

## Where the time goes (cpp-d2d, medium)

| Segment | ≈ ms | What happens |
|---|---|---|
| CreateProcessW | 20–28 (quiet: 6) | image section creation, Defender callbacks (real-time protection is on) |
| up to `main` | 12–13 | loader (statically only kernel32/user32/gdi32/dwmapi), static CRT initialization |
| `main` → `window_created` | 12–15 | RegisterClassEx + the process's first CreateWindowEx: loading `uxtheme.dll` and **injection of global hooks** (on this machine `RTSSHooks64.dll` from RivaTuner plus SETUPAPI/SHLWAPI/WINMM, ≈ 10 ms, visible in `--trace`) |
| `window_created` → `shown` | 8–12 | ShowWindow: non-client frame, activation, DWM surface (+10 more with IME) |
| `shown` → `first_present` | < 1 | WM_PAINT: the frame is already in the DIB, only one BitBlt is left |
| DwmFlush | 6–7 | waiting for the next DWM composition (165 Hz, ≤ 6 ms) |
| *doc thread, in parallel* | *≈ 15* | read 0.5, md4c 0.5 (53 KB), DirectWrite factory and formats ≈ 2, first-viewport layout 5–10 (loading `TextShaping.dll`, font fallback for CJK/emoji), render into the DIB ≈ 3. Everything is ready **before** ShowWindow |

Conclusion: for a cold start of this stack there is almost no headroom left inside the application. The time is eaten by process start
(Defender), the first CreateWindowEx (third-party hooks), ShowWindow and DWM vsync. Beyond that only a
resident or OS-side changes help (a Defender exclusion, no global hooks).

## Resident: what was done and what was learned

`fastmd-d2d.exe --resident` warms up at startup everything that doesn't depend on the document: the DirectWrite factory and formats,
font fallback, the glyph cache (it runs a sample with Cyrillic, CJK, emoji, code and a table through the full pipeline),
the GDI canvas and the window class. It also creates a **spare document window**, shows it but keeps it DWM-cloaked, so the
window isn't composited and is invisible to the user and to the harness (`list_windows` skips cloaked windows). The resident itself
listens on the message-only window `FastMDd2dResident`.

`fastmd-d2d.exe --forward <file>`, if a resident is found, sends it `WM_COPYDATA` (the path,
`FASTMD_BENCH_OUT/EXIT`, its own `main` mark) and exits. If there is no resident, it is a normal cold start. The resident
starts the doc thread and activates the spare window (`SetForegroundWindow` ≈ 8 ms runs in parallel with the doc thread). The frame
is put into the still-cloaked window, the window is uncloaked, then DwmFlush and `t_content`. After the result is written
the document window is closed, and a new spare one is prepared while idle (PROTOCOL §6).

A/B results (`tools/exp_launcher.py`, medium, n = 9):

* **A separate tiny launcher turned out slower.** `fastmd-open.exe` (4 KB, no CRT, only kernel32 and user32)
  gave 84.4 ms, while `fastmd-d2d.exe --forward` gave 66.1 ms. The difference is entirely in CreateProcess: 38 ms versus 7.5 ms.
  While the exe file is mapped by a live process (the resident), its image section already exists, and a new process from
  **the same file** is created several times faster. Launcher variants with a manifest (UAC, supportedOS) and with a
  static CRT made no difference. So the launcher was kept only as an experiment (`src/launcher.cpp`,
  built into `obj/`).
* **A spare window shown cloaked in advance** gave 34.5 ms versus 41.7 ms with a hidden window: the expensive first
  ShowWindow moves into idle time.
* An early `SetForegroundWindow` (before the frame, while the window is cloaked) removes ≈ 5 ms from the critical
  path in single breakdowns, but over the median of 9 runs it is within noise.
* **Measurement hygiene:** the resident and the "cold" variants use **different copies** of the exe (`out-resident/` and `out/`).
  Otherwise a live resident would speed up CreateProcess of the cold variants by ≈ 15 ms (see below).

## Notes on the harness (for the orchestrator)

1. **`bench.py run` keeps all residents alive for the whole run.** Any cold variant whose exe is the same
   file as the resident's gets a "warm" CreateProcess. Measured here: `launch_overhead` 6 ms versus 20–25
   ms, `main` 7–15 ms earlier. In the first quick run (`results/quick-run-1.json`, the resident still on
   `out/fastmd-d2d.exe`) the cold `cpp-d2d` looked ≈ 10–15 ms faster because of this. In this prototype it is
   fixed with a separate copy of the exe. Other prototypes with `-resident` should check the same thing, and the harness could
   start residents only for the duration of their own variants.
2. `observed_ms` is usually 6–10 ms earlier than `t_content`: the observer sees the window as visible before DwmFlush. This is expected.
3. RivaTuner Statistics Server (`RTSS.exe`) runs on this machine, and its `RTSSHooks64.dll` is injected into every
   GUI process on the first CreateWindowEx (≈ 10 ms). This is a common tax on all prototypes, including the baseline.

## Ideas for further speedups

* Cold start: there is practically no headroom inside the application (< 2 ms: the UI thread from `main` to ShowWindow is busy
  only with OS calls, and the document pipeline already runs in parallel). The substantial part lies outside the application:
  * signing the exe (Authenticode) and SmartScreen/Defender reputation may make the scan at CreateProcess cheaper (not tested);
  * without RTSS and other global hooks CreateWindowEx would be ≈ 10 ms faster.
* Resident: a shared process for all windows (no CreateProcess at all) is possible only via a shell extension or
  DDE/COM activation, and that is already product architecture. The alternative is a "tray resident" with autostart, which is exactly
  the `-resident` variant.
* Large documents: preparing layout without `IDWriteTextLayout` (a custom line breaker on top of `IDWriteTextAnalyzer`)
  would speed up the background height measurement (currently ≈ 0.6 s on 3.7 MB). A height cache keyed by file hash would speed up repeat
  opens.
* Images: decode only the visible ones and with downscaling (`IWICBitmapScaler`): less memory on large documents.

## Effort estimate for a full product

For one experienced C++/Win32 developer, building on this prototype:

* **MVP viewer** (selection and copying, Ctrl+F search, zoom, dark theme, reload on file change,
  anchors and table of contents, horizontal scrolling of code and tables, settings, `.md` association, installer/MSIX,
  signing): **≈ 6–8 weeks**. The most laborious parts are text selection on top of per-block `IDWriteTextLayout` (hit-testing
  across blocks and cells) and search with highlighting.
* **Full product** (UI Automation text pattern for screen readers, printing and PDF, remote images, proper
  highlighting such as tree-sitter, footnotes or a different parser, an HTML subset, math, mermaid): another
  **≈ 3–4 months**. Math and mermaid without a web engine are the riskiest items.

The main risk of the stack: everything a browser gives you for free (selection, accessibility, HTML, math) has to be written
by hand here.

## Pitfalls (found along the way)

* `IDWriteBitmapRenderTarget3::DrawGlyphRunWithColorSupport` is unavailable on Win11 26200 (QI fails). Color
  emoji on the GDI back-end are drawn manually layer by layer (COLRv0 via `TranslateColorGlyphRun`).
* The `IDWriteBitmapRenderTarget` DIB is top-down even though `GetObject` returns a positive height. The orientation
  is determined by probing a pixel via `SetPixelV`.
* md4c 0.5.3 in UTF-16 mode doesn't build or works incorrectly without the `_T("*_~")` patch (see "Stack").
* Hardware D3D11 on this machine costs ≈ 200 ms: two GPUs, NVIDIA UMD loading. A swap chain for a window on the
  dGPU display created from an iGPU device is also ≈ 190 ms. For an "instant" window the GPU is contraindicated.
* `ImmDisableIME(0)` applies per thread and wins ≈ 10 ms. But a product with a search field will need IME: the input
  field has to live on a separate thread, or its window has to be created lazily on another thread.
* Without `DWMWA_TRANSITIONS_FORCEDISABLED` the mark doesn't change, but the user sees a fade-in. For perceived speed
  the flag matters.
* Resident: see the single-exe-file effect (image section) and the benefit of a window pre-shown cloaked.
  `SetForegroundWindow` from a background launch may fail to activate the window (foreground lock). So the forwarding
  process calls `AllowSetForegroundWindow(resident pid)`.
* On large files without virtualization, DirectWrite layout of the whole document takes ≈ 1.8 s: virtualization
  is mandatory.

## Files

| Path | What |
|---|---|
| `build.ps1` | full rebuild (out/, out-resident/, obj/) |
| `proto.json` | 4 variants |
| `src/main.cpp` | window, startup pipeline, threads, D3D/D2D back-ends, input, resident/forward |
| `src/doc.cpp` | file reading, md4c → block and run model, prefix cut, palette |
| `src/layout.cpp` | typography (formats, baseline), height estimation, `IDWriteTextLayout` for blocks and tables, image sizes |
| `src/canvas_gdi.cpp` | DirectWrite → GDI DIB back-end + SDF shapes + COLR emoji |
| `src/canvas_d2d.cpp` | Direct2D back-end (DeviceContext / HwndRT / DCRT) |
| `src/highlight.cpp` | syntax highlighting |
| `src/app.h`, `src/ipc.h` | shared declarations, WM_COPYDATA protocol |
| `src/launcher.cpp` | experiment: a 4 KB launcher without CRT (not used by the variants) |
| `third_party/md4c/` | md4c 0.5.3 (MIT) + UTF-16 patch |
| `tools/exp.py`, `tools/exp_launcher.py` | knob and launcher A/B using the same measurement code as bench.py |
| `tools/marks.py` | mark medians from a `bench.py run` file |
| `tools/shot.py`, `tools/interact.py`, `tools/resize.py` | screenshots: with scrolling, after input (wheel/keys), after resizing |
| `tools/probe/*.cpp` | probes: DirectWrite interop capabilities and DXGI adapter timing |
| `results/*.json` | raw results of the quick runs |
| `shots/*.png` | screenshots |

## Audit (independent review, 2026-09-19)

The auditor re-checked the prototype against PROTOCOL §3/§5/§6. During the audit the machine was **heavily loaded**
(other agents were running in parallel, their windows popped up on top, CreateProcess was 25–77 ms at times, once 4.2 s).
All numbers below are therefore rough.

### What was checked

| Item | Result |
|---|---|
| Where `t_content` is taken | `OnPaint`: `WaitReady()` (join of doc/dev threads) → `Present()` (GDI: `BitBlt` of the ready DIB into the window; DXGI: `Render` + `IDXGISwapChain::Present`) → `EndPaint` → `fmb_content_presented_ex` → **`DwmFlush()`** → mark. Matches §3. The mark is taken only on the first document frame, nothing is written before it |
| The first viewport is not "fake" | The first frame draws real `IDWriteTextLayout`s of the first blocks (21 of 679 in medium). The remaining blocks get an estimated height, which is the virtualization §3 allows. Images are decoded after the frame, but the first viewports of small/medium/large contain no images |
| No caches or residents in cold variants | Search over `src/`: files are opened only to read the document and image headers; writes go only to `%TEMP%` with `--trace`/`--scroll-test`. No registry. Nothing runs before `main` |
| No corpus data in the build | There are no corpus strings/sizes/paths in `src/`. `FindPrefixCut` and the height estimates are generic. **One finding about the resident, fixed** (see below) |
| Resident (§6) | What is measured is the launch of `out-resident/fastmd-d2d.exe --forward`: the process sends `WM_COPYDATA` (path, `FASTMD_BENCH_OUT/EXIT`) and exits. Reading, md4c, layout, rendering and `DwmFlush` are done by the resident **after** it receives the path. The resident writes the result, then closes the document window and keeps running. The document model is reset between opens (`ResetDoc`). The spare window is cloaked, the harness doesn't see it. Matches §6 |
| Release build | `build.ps1` was rebuilt from scratch twice: ≈ 20 s, exit 0. Flags `/O2 /Oi /GL /Gy /GS- /MT`, link `/LTCG /OPT:REF /OPT:ICF`. `dumpbin /dependents`: statically only USER32/GDI32/dwmapi/KERNEL32, delay-load d2d1/DWrite/d3d11/dxgi/ole32/SHELL32/IMM32. Exe 416 768 bytes, `out-resident/` is a byte-for-byte copy |
| §5 (window, style) | Client area 1000×800 (screenshot 1002×832 with the frame), title `medium.md — FastMD (cpp-d2d)`, PMv2, light theme, 860 column, fonts and colors per spec, the document scrolls |

### Independent check that "the document is actually on screen at `t_content`"

The harness's internal `observed_ms` is not enough for this: it only catches the window's visibility flag, and that is
set before drawing. So a separate tool was written, **`tools/audit_onscreen.py`**. It launches the
variant the way the harness does, but with `FASTMD_BENCH_EXIT=0`, and in a loop captures the client area **from the screen** via the desktop DC,
i.e. the frame that DWM actually composited. The captures run in step with composition, ≈ 6 ms at 165 Hz. The tool then
finds the first capture in which the heading strip matches the frame at 1.5 s, and compares that whole first
frame with the settled one. The script also works for other prototypes in self mode.

Results (ms from CreateProcess; runs where other windows covered the window were discarded):

| Variant / doc | `t_content` | first capture with the document finished at | first frame vs settled |
|---|---|---|---|
| cpp-d2d / medium | 77.9 · 84.7 · 67.2 · 71.2 | 78.0 · 84.8 · 67.4 · 71.5 | 0 differing pixels (1000×800) |
| cpp-d2d / large | 72.7 · 94.6 · 78.9 · 81.9 | 72.9 · 94.8 · 79.2 · 85.9 | 0 px |
| cpp-d2d / small | 80.7 · 100.9 | capture [86.0..100.9] · [88.1..103.9] (a capture of the whole 1000×800 area takes 12–20 ms, so precision is lower; the previous capture without the document finished at 87.9, i.e. before the 100.9 mark) | 0 px / 13 px (corner of the scrollbar thumb: heights are refined in the background) |
| cpp-d2d-warp / medium | 134.7 · 126.8 | 134.7 · 130.0 | 0 px |
| cpp-d2d-gpu / medium | 372.5 · 385.9 | 355.6 · 360.9 (document on screen **before** the mark) | scrollbar thumb |
| cpp-d2d-resident / medium | 35.2 · 41.7 · 47.7 | 35.5 · 43.4 · 43.2 | 0 px |

**Conclusion:** the mark is not early. The document appears on screen in the same composition frame that
`DwmFlush` waits for, within ≈ 0.1–0.4 ms of `t_content`. For the DXGI variants the mark is even 10–20 ms conservative.
The first shown frame is pixel-identical to the settled one (except for the scrollbar thumb), so nothing is
"finished drawing" after the mark. The harness's `observed_ms` is consistently 6–20 ms earlier than `t_content`, and that is expected:
the window is visible before the first composition.

### Fixed

1. **The resident warm-up was tailored to the corpus.** The sample in `ResidentMain` contained exactly the emoji from the corpus
   (🚀 ✨ ✅ ❌) and the same CJK words (日本語, 中文), so the resident pre-rasterized glyphs that later
   appear in the document. The sample was replaced with characters that don't occur in `bench/corpus/*.md`: 😀 👍 🎉 ⭐, 東京 你好,
   «Пример» (verified by searching the corpus). Fonts, font fallback, COLR and shaping are warmed up the same way. The change is
   one literal in `src/main.cpp` with a comment. After the rebuild `cpp-d2d-resident` showed check 48.6/41.0/44.1 ms
   (s/m/l) and sanity medians 47.5/37.5 ms (s/m): the same as before the fix, within noise. The resident's screenshot
   is pixel-identical to the cold variant.

Nothing else was changed: the cold variants, the marks, `proto.json` and `build.ps1` were not touched.

### Remarks (not fixed)

* **"Faster than the calibration floor" is within noise.** In the audit `cpp-d2d` versus `baseline-win32` gave 86.5 versus
  90.5 ms (medium, n = 8 interleaved, `results/audit-vs-baseline.json`). It is more accurate to say "at floor level".
  Part of the gain comes from `--no-ime` (≈ 10 ms), which the baseline doesn't have. In the product IME is needed for the search field, and
  `ImmDisableIME(0)` applies to the whole thread, so the search field will have to move to a separate UI thread. This is an honest
  application-level technique, but it should be taken into account in the comparison.
* An image in the first viewport would be shown as a placeholder: space for it is reserved, and decoding happens
  after the frame. This doesn't affect the corpus. Web engines behave the same way.
* Prefix parsing (large, > 256 KB): link reference definitions declared below the cut point won't apply in the first frame
  (per CommonMark they apply to the whole document). This doesn't show up on the corpus; the first frame matches the
  settled one.
* `/GS-` (no buffer checks) is acceptable for a benchmark. Its effect on startup is negligible; in the product the flag should be restored.
* Code and tables have no horizontal scrolling; long lines are clipped. This is documented.

### For the orchestrator (harness)

* `bench.py run` brings up **all** residents at the start and keeps them until the end of the run. In this prototype the
  "warm" image section effect is closed off with a copy of the exe. But with `run all` the residents of other prototypes (for example, WebView2)
  keep the shared runtime warm for their cold variants. Recommendation: measure `-resident` variants in a separate `run`.
* `observed_ms` reflects only window visibility, not content. To check "content on screen" you can use
  `tools/audit_onscreen.py` from this prototype.

### Rating

* Visual quality **9/10**, completeness **9/10**. Typography, spacing, h1–h6 headings with rules, lists with
  per-level markers, checkboxes, blockquotes, tables (alignment, zebra, semibold header), code highlighting in the GitHub
  palette, color emoji, CJK, Cyrillic and images practically match the `cpp-webview2` reference (the same
  `style.css` in Chromium). Minor points: hr is 2 px (GitHub has 4 px), no footnotes or HTML, no horizontal scrolling of
  code and tables. Audit screenshots: `shots/audit-*.png`. Back-ends differ by less than 0.06 % of pixels
  (anti-aliasing); the resident matches cold pixel for pixel.
* **All 4 variants are fit for the final measurement:** `cpp-d2d`, `cpp-d2d-warp`, `cpp-d2d-gpu`,
  `cpp-d2d-resident`. warp and gpu are meaningful "negative" results about the cost of a GPU start. resident
  should be compared only with other `-resident` variants.

Audit sanity run (`results/audit-run.json`, `run cpp-d2d,cpp-d2d-warp,cpp-d2d-gpu,cpp-d2d-resident --doc
small,medium --runs 5`, machine loaded), median `t_content` in ms:

| variant | small | medium | p90 medium | window / observed (medium) |
|---|---|---|---|---|
| cpp-d2d | 81.2 | 80.7 | 107.5 | 68.3 / 59.7 |
| cpp-d2d-warp | 97.2 | 125.0 | 130.8 | 67.8 / 60.0 |
| cpp-d2d-gpu | 318.6 (2.8 s outlier) | 337.3 | 353.3 | 65.7 / 58.3 |
| cpp-d2d-resident | 47.5 | 37.5 | 56.6 | 27.4 / 28.3 |
