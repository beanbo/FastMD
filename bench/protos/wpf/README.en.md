[Русский](README.md) | **English**

# wpf — C# .NET 8 WPF + Markdig → FlowDocument

A FastMD prototype on WPF: Markdig parses the Markdown, a custom builder constructs a `FlowDocument`
(headings, lists, checkboxes, blockquotes, tables with borders and zebra stripes, highlighted code, images),
and the document is shown in a `FlowDocumentScrollViewer`. No XAML/BAML: the whole UI is built in code.

**Bottom line (medians, pool of 2 × 5 runs on a noisy machine):** the best variant, `wpf`, shows the first screen
in **~310–330 ms** (small 321 / medium 308 / large 329). The same rendering code with a "standard" WPF startup
(`wpf-plain`) takes **~505–530 ms**. Key findings:

1. **The WPF render thread takes ~150 ms to start.** Internally it loads the GPU's D3D9 driver: on this machine
   that is 147 MB of NVIDIA DLLs, and they are loaded even in software mode. This startup runs on a separate thread
   in parallel with everything else: −140 ms.
2. **A native launcher** starts loading this driver in the first millisecond of the process, before .NET starts,
   and then brings up .NET in the same process via `hostfxr`: another −60 ms.
3. No Aero2 theme (templates in code), no IME, font warm-up in the background and software rendering of the first frame:
   another −100 ms in total. **Right after the first frame the window switches to the GPU.** If software rendering is kept,
   scrolling a window maximized on a wide monitor drops to ~30 fps.

The floor of this stack on this machine is about 300 ms. For comparison, `baseline-win32` shows 70–90 ms.

## Stack and versions

| Component | Version |
|---|---|
| .NET SDK | 8.0.424 |
| Runtime (framework-dependent) | Microsoft.NETCore.App 8.0.30 + Microsoft.WindowsDesktop.App 8.0.30 (installed system-wide) |
| UI | WPF (`net8.0-windows`, `UseWPF`), no `App.xaml`, no `Application`, `Dispatcher.Run()` |
| Markdown | Markdig 1.3.2 (NuGet; pipe tables, task lists, strikethrough, autolinks) |
| Container | `FlowDocumentScrollViewer` (PTS bottomless mode), templates built in code (`LeanTemplates.cs`) |
| Code highlighting | custom single-pass tokenizer (`Highlighter.cs`): rust, c#, c/c++, js/ts, python, powershell, json |
| Launcher | C++ (MSVC 14.44, `/O2 /MT`), `launcher/launcher.cpp` → `out/r2r/fastmd-wpf.exe` (135 KB) |
| Build | `dotnet publish -c Release -r win-x64`, ReadyToRun (crossgen2 from NuGet) |
| Protocol | `bench/common/FastMdBench.cs` (+ `src/FastMdBenchExt.cs` for marks taken on other threads) |

The NuGet cache is per-project: `bench/.tools/wpf/packages` (`RestorePackagesPath` in the csproj).
`bench/.tools/wpf/dotnet-trace` is only needed for profiling (`tools/profile.ps1`).

## Build

```powershell
pwsh -File bench/protos/wpf/build.ps1               # r2r + лаунчер, jit, sc-r2r
pwsh -File bench/protos/wpf/build.ps1 -Experiments  # + sc-single и r2r-icu (только для таблицы ручек)
pwsh -File bench/protos/wpf/build.ps1 -Only r2r     # одна сборка (r2r собирает и лаунчер)
```

A clean build of everything takes 10–40 s. The script only cleans its own directories (`out/`, `src/obj`, `src/bin`) via
`[IO.Directory]::Delete`, checking that the path is inside the prototype folder. Each build uses its own
`obj/<flavor>` / `bin/<flavor>`. The launcher is built by `launcher/build-launcher.cmd` via `bench/tools/msvc.cmd`,
with the same `app.manifest` as the .NET app (PerMonitorV2).

| Build (`out/…`) | Parameters | `dist_dir` size |
|---|---|---|
| `r2r` | framework-dependent, `PublishReadyToRun=true` (FastMdWpf.dll + Markdig.dll) + `fastmd-wpf.exe` | 1.75 MB |
| `jit` | framework-dependent, pure IL (what `dotnet publish` produces by default) | 0.67 MB |
| `sc-r2r` | self-contained folder, everything R2R | 145.7 MB |
| `sc-single` (exp.) | self-contained single-file, R2R, uncompressed | 155.3 MB |
| `r2r-icu` (exp.) | like `r2r`, but `InvariantGlobalization=false` | 1.62 MB |

Common runtime settings (csproj): `InvariantGlobalization=true` + `PredefinedCulturesOnly=false`
(otherwise WPF crashes on `CultureInfo("en-US")`), `TieredPGO=false`, `ConcurrentGarbageCollection=false`
(no background GC thread), `UseSystemResourceKeys=true`, `SatelliteResourceLanguages=en`.
Manifest: PerMonitorV2, supportedOS Windows 10/11.

## Variants (`proto.json`)

| id | exe | Arguments | What it is |
|---|---|---|---|
| **`wpf`** | `out/r2r/fastmd-wpf.exe` (launcher) | — | the best honest configuration: launcher with GPU driver preloading, R2R framework-dependent, all knobs below enabled; first frame in software, then GPU |
| `wpf-hw` | `out/r2r/fastmd-wpf.exe` | `--hw` | the same, but GPU rendering from the very first frame (D3D9) |
| `wpf-apphost` | `out/r2r/FastMdWpf.exe` | — | the same knobs, but the standard .NET apphost: pure .NET without native code |
| `wpf-plain` | `out/jit/FastMdWpf.exe` | `--plain` | the reference point: a "textbook" WPF startup — apphost, IL/JIT, Aero2 theme, IME, GPU, everything sequential (same rendering code) |

All knobs are command-line arguments. The best configuration is enabled by default, so when
double-clicked the app behaves like `wpf`.

| Switch (default → disable) | Where | What it does |
|---|---|---|
| D3D preloading → `--no-d3d-prewarm` | launcher | in the first millisecond of the process a separate thread calls `Direct3DCreate9Ex` and, for each adapter, `GetAdapterIdentifier`, `GetDeviceCaps(HAL)`, `CheckDeviceType`. This loads `d3d9.dll` and the GPU user-mode driver. The `IDirect3D9Ex` object lives until the process ends so that the driver DLLs are not unloaded. Meanwhile the main thread starts .NET via `hostfxr_main_startupinfo`, the way the standard apphost does |
| `--sw` → `--hw` | .NET | `RenderOptions.ProcessRenderMode = SoftwareOnly` during startup: the first frame is rendered into a DIB, and the render thread does not need a D3D9Ex device and swap chain on the critical path |
| `--gpu-later` → `--sw-only` | .NET | (with `--sw`) right after `t_content`, `ProcessRenderMode = Default` is set and `HwndTarget.RenderMode` is recomputed: the window switches to the GPU. The device is created only after the first frame |
| `--warm` → `--no-warm` | .NET | a second STA thread at the start of `Main` creates its own `Dispatcher` and `MediaContext`. This starts WPF's process-global render thread in parallel with type loading and document building on the main thread |
| `--warm-text` → `--no-warm-text` | .NET | a third thread formats the first 4 KB of the file via `FormattedText` (regular / semibold / bold / italic / monospace). This gets fonts, CJK and emoji fallback, and LineServices into WPF's shared caches |
| `--lean` → `--theme` | .NET | `OverridesDefaultStyle` + Window / FlowDocumentScrollViewer / ScrollViewer templates via `FrameworkElementFactory` and a custom thin scroll bar. The Aero2 theme dictionary (BAML) is not loaded before the first frame |
| `--no-ime` → `--ime` | .NET | `InputMethod.IsInputMethodEnabled=false`: no TSF text store is registered on the first `WM_SETFOCUS` (~20 ms). A reader has no text input |
| `--plain` | .NET | = `--hw --no-warm --no-warm-text --theme --ime` |
| experiments | .NET | `--full` (the whole document before the first frame), `--prelayout` / `--pretemplate` (layout or templates before `Show()`), `--first=N` (how many estimated screens to build before the first frame, default 2), `--early-hwnd`, `--app` (with an `Application` object), `--scroll=N` (scroll after the first frame, for screenshots), `--scrolltest[=max]` (scroll smoothness test, see below) |

Regardless of the switches, the following always apply:
- reading and Markdig parsing on a separate thread, in parallel with WPF startup;
- **first screen first**: blocks covering ≥2 estimated screens are built before the first frame. The height estimate
  is deliberately low, so the screen is always filled; for medium that is 16 of 391 blocks. The rest is appended
  in sections at `Background` priority after `t_content`, with section size growing from 64 to 4096 blocks.
  For files larger than 96 KB only the first ~24 KB are parsed before the first frame (cut at an ATX heading outside fenced code);
  the full parse runs in the background. This is allowed by the protocol (§3);
- first-screen images are decoded on the parse thread: a frozen `BitmapImage` from a `FileStream`.

`t_content` = `Window.ContentRendered` → a `Dispatcher` callback at `Background` priority → `DwmFlush()`,
strictly per PROTOCOL §3. In WPF, `t_window` coincides with the first frame, which already contains the document.
There is no cache between launches and no resident processes. All "warm-ups" are parallel
work within a single launch.

## Quick numbers

Pool of two runs of the final build: `runs/final-quick-2.json` + `final-quick-3.json`, each with 1 cold launch
(discarded) + 5 measurements, n=10 per cell in total. Other people's prototypes are running on the machine in parallel.

| Variant | small, ms | medium, ms | large, ms | observed (medium) | CPU, ms | WS, MB | peak commit, MB | dist |
|---|---|---|---|---|---|---|---|---|
| **wpf** | **321** | **308** | **329** | 261 | 508 | 94 | 110 | 1.75 MB |
| wpf-hw | 336 | 327 | 343 | 284 | 594 | 108 | 135 | 1.75 MB |
| wpf-apphost | 360 | 371 | 370 | 323 | 469 | 94 | 56 | 1.75 MB |
| wpf-plain | 505 | 516 | 532 | 463 | 625 | 112 | 81 | 0.67 MB |

The launcher raises peak commit by +54 MB: that is the NVIDIA driver and its shader compiler, which the live
`IDirect3D9Ex` keeps in memory. It does not affect the working set. Exact numbers will come from the orchestrator's final sequential
run.

### Scroll smoothness: why the GPU is needed after the first frame

`--scrolltest[=max]`, after building the whole document, scrolls by 40 DIP per frame and writes the intervals between
frames (`CompositionTarget.Rendering`) to `trace.log`: `pwsh -File tools/trace-run.ps1 -Doc medium -Extra '--scrolltest=max'`.
The monitor here is ~160 Hz; the maximized window is 3456×1408 DIP.

| Mode | 1000×800 window: p50 / p95, ms | maximized 3456×1408: p50 / p95, ms |
|---|---|---|
| software for the whole session (`--sw-only`) | 12.1 / 15.3 (~80 fps) | **31–32 / 35–37 (~30 fps)** |
| GPU from the first frame (`--hw`) | 6.7 / 12.2 | 6.2 / 9.8 (vsync-bound) |
| **software first frame → GPU (`wpf`)** | 7.8 / 11.9 | **6.2 / 9.4** |

### Knobs one at a time, baseline — launcher (medium, 2 passes × 5, pool n=10: `runs/knobs2-medium-*.json`, `tools/combine.py`)

| Configuration (one knob changed relative to `wpf`) | median, ms | min, ms | Δ |
|---|---|---|---|
| **wpf** | **321** | 308 | — |
| `--no-warm` (no second STA thread) | 337 | 306 | +16 (with the launcher the render thread starts quickly anyway) |
| `--ime` | 346 | 308 | +25 |
| `--no-warm-text` | 353 | 328 | +32 |
| `--hw` | 363 | 314 | +42 (+10…17 in other runs) |
| `--theme` | 370 | 331 | +49 |
| standard apphost instead of the launcher (`wpf-apphost`) | 378 | 344 | +57 |
| launcher without D3D preloading (`--no-d3d-prewarm`) | 381 | 345 | +60 (the launcher by itself gives nothing) |
| launcher + `--plain` | 441 | 391 | +120 |
| **`wpf-plain`** | **534** | 485 | +213 |

### Build and runtime knobs, baseline — apphost (medium, pool n=10: `runs/knobs-medium*.json`)

Measured before the launcher existed, on the standard apphost with all other knobs (back then this configuration was called `wpf`).

| Configuration | median, ms | min, ms | Δ |
|---|---|---|---|
| **apphost, R2R, framework-dependent** (baseline) | **394** | 356 | — |
| + `--prelayout` (layout before `Show()`) | 392 | 360 | ≈0 |
| `DOTNET_TieredCompilation=0` | 400 | 378 | +6 |
| `DOTNET_TieredPGO=1` | 403 | 368 | +9 |
| ICU instead of InvariantGlobalization (`r2r-icu`) | 413 | 367 | +19 |
| IL/JIT for own code and Markdig (`jit`) | 414 | 373 | +20 |
| `--hw` | 420 | 392 | +26 |
| self-contained (`sc-r2r`) | 424 | 358 | +30 (noise; −12 in another run, i.e. ≈ the same as fd) |
| `--no-warm-text` / `--theme` / `--no-warm` / `--ime` | 445 / 449 / 475 / 480 | | +51 / +55 / +81 / +86 |
| `--full` (whole document before the first frame) | 711 | 595 | +317 |
| self-contained **single-file** (`sc-single`) | 726 | 683 | +332 (`Main` ~330 ms later: 155 MB EXE) |
| `DOTNET_ReadyToRun=0` (JIT for the framework too) | 736 | 622 | +342 |

How the improvements went (medium, ms). The previous agent left R2R + theme + sequential startup + GPU: 588
(`jit` 633, `sc` 597, `single` 916, `--sw` 634, `--full` 1148; `runs/initial-medium.json`). Then:
second thread for the render thread 603 → 460 → no IME 417 → fixed image pre-decoding (a Markdig bug, see Pitfalls)
and Hyperlink without the theme → `--lean` 377 → font warm-up + software rendering ~370–390 → native launcher
with D3D preloading **~305–340** (`runs/exp*.json`, `runs/native-*.json`).

### Breakdown by marks (`wpf`, medium, medians; `python tools/marks.py runs/final-quick-2.json`)

| Mark | ms | What happened |
|---|---|---|
| `main` | 83 | CreateProcess (+ Defender), launcher, hostfxr/hostpolicy, CoreCLR, first line of `Main`. In parallel the launcher thread loads D3D9 and the NVIDIA driver |
| `file_read` / `parsed` | 87 / 105 | reading 53 KB and Markdig (parse thread) |
| `window_new` | 151 | main thread: WPF cctors (`FrameworkElement`, `Dispatcher`, `ScrollViewer` …) + window and templates in code |
| `warm_media` | 162 | **second thread: the WPF render thread is ready** (~240 here without the launcher) |
| `doc_built` | 166 | 16 blocks → `FlowDocument` (~15 ms) |
| `warm_text` | 178 | the third thread has warmed up fonts, fallback and LineServices |
| `measure_begin` | 205 | `Show()`: HWND, `HwndSource`, `HwndTarget` |
| `measure_end` | 246 | FDSV template + PTS/LS contexts + formatting 16 blocks (41 ms) |
| `arrange_end` / `hwnd_created` | 257 / 258 | arrange, PTS visuals |
| (observed) | 261 | the harness sees a visible window |
| `loaded` | 285 | ShowWindow, activation, focus, first `Rendering` |
| `content_rendered` | 295 | the first frame is drawn and handed to DWM |
| **`t_content`** | **307** | `Background` callback + `DwmFlush()` |

For comparison, `wpf-apphost` (small): `main` 82 → `warm_media` **238** → `measure_begin` 262 → `t_content` 362.
`wpf-plain` (medium, early runs): `main` 86 → `parsed` 135 (Markdig JIT) → `window_new` 210 (the Aero2 theme is loaded here)
→ `doc_built` 230 → `measure_begin` 400 (**170 ms** — `Show()` blocks on `MediaContext` → `SyncFlush`, i.e. waits for the
render thread) → `measure_end` 490 → `loaded` 545 (TSF) → `t_content` 571.

## Where the time goes

Sources: a `dotnet-trace` profile (`runs/prof/*.speedscope.json`, `tools/speedscope_top.py`), the process module list
(`tools/modules.ps1`) and the marks.

1. **Process and .NET before `Main`: ~80 ms.** In that time the native baseline manages to open its whole window.
   R2R, self-contained and the launcher do not change this part. Single-file makes it worse: +330 ms.
2. **WPF render thread: ~150 ms without the launcher.** On the first `MediaContext`, WPF synchronously waits
   (`DUCE.Channel.SyncFlush`) for the compositor to create the display set: `d3d9.dll`, adapter enumeration
   (two here: RTX 5070 and AMD iGPU), the NVIDIA user-mode driver. The process ends up with
   **`nvgpucomp64.dll` 106 MB + `nvd3dumx.dll` 41 MB** and `D3DCompiler_47_cor3.dll` mapped, **even in software mode**.
   In addition, `RTSSHooks64.dll` (RivaTuner) and Windhawk are injected into the process: that is the machine's environment.
   The prototype fights this in two ways: `--warm` starts this initialization on a second thread from `Main`,
   and the launcher loads the driver even before .NET. With the launcher the render thread is ready at ~162 ms instead of ~240.
3. **The window's GPU device (~50–80 ms, `--hw` only).** After the HWND is created, the render thread creates a D3D9Ex device
   and swap chain. With or without the launcher this partly overlaps with measure, hence the 10–40 ms `--hw`/`--sw` difference.
   In `wpf` the device is created only after `t_content`, when switching to the GPU.
4. **The main thread — now the critical path (~225 ms after `Main`):** WPF cctors and `Dispatcher` ~45–65;
   `FlowDocument` ~15; `HwndSource` / `HwndTarget` ~40, including stylus registration (WISP),
   `RegisterDropTarget` (OLE) and `WTSQuerySessionInformation`; FDSV template + `AttachTextEditor` → PTS/LineServices contexts
   plus formatting the first 16 blocks ~41; arrange ~11; ShowWindow, activation and first `Rendering` ~27; frame ~10;
   Background + DwmFlush ~12. Already removed by knobs: the Aero2 theme — BAML dictionary 25–30 ms, Window style 15–20,
   FDSV template 15, `EnsureResourceChangeListener` ~10; fonts and fallback ~15; TSF 20–30; image decoding
   with urlmon ~27.

## What is supported per the spec (PROTOCOL §5)

| Element | Status |
|---|---|
| Window with a 1000×800 DIP client area, PerMonitorV2 (manifest in both the launcher and the apphost), title `<file> — FastMD (<id>)` | ✅ |
| Font `Segoe UI Variable Text` → `Segoe UI`, 15 px, line-height 24 (1.6), `#1f2328` | ✅ |
| Centered column max 860 DIP, margins 32 DIP, background `#ffffff`, mouse wheel and keyboard scrolling (focus is set after the first frame) | ✅ custom thin scroll bar, 6→10 px on hover (classic Aero2 with `--theme`) |
| h1–h6: 30/24/20/17/15/14, semibold, h1/h2 with a `#d1d9e0` rule, h6 `#59636e` | ✅ |
| **bold** / *italic* / ~~strikethrough~~ / links `#0969da` (underline on hover, click via ShellExecute, relative paths resolved from the .md folder) | ✅ (strikethrough text is additionally dimmed) |
| `inline code`: Cascadia Mono, background `#eff1f3` | ⚠️ no rounded corners; inner padding imitated with thin spaces (`Run` has no padding/radius) |
| Bulleted / numbered (with `start`) / nested lists, markers disc → circle → square | ✅ |
| Task lists: drawn GitHub-style checkboxes | ✅ |
| Blockquotes: 4 px `#d1d9e0` bar, text `#59636e` | ✅ |
| Horizontal rule | ✅ (2 px `#d1d9e0`) |
| Code: Cascadia Mono 13.5, background `#f6f8fa`, padding 16, **long lines wrap** | ⚠️ corners not rounded (`Paragraph` has no CornerRadius) |
| Syntax highlighting (nice to have) | ✅ 7 languages, custom tokenizer, GitHub light colors |
| GFM tables: 1 px `#d1d9e0` borders, semibold header, `#f6f8fa` zebra stripes, padding 6/13, column alignment | ✅ column widths by content (like `max-content`), computed from glyph advance widths; if the table is wider than the column, cell text wraps |
| Images (local PNG, 1 px = 1 DIP, scaled down to fit the column) | ✅; remote URLs are not loaded, alt is shown |
| Cyrillic, CJK | ✅ (fallback via the WPF composite font) |
| Emoji | ⚠️ **monochrome**: WPF does not support color fonts (COLR/CPAL) |
| Text selection and copy | ✅ (built into FlowDocumentScrollViewer) |
| Search (Ctrl+F), zoom, dark theme, raw HTML, footnotes, math, mermaid | ❌ (HTML blocks are shown as dimmed monospace text) |
| Large files | ⚠️ first screen of large (3.7 MB) in ~0.35 s, but appending the whole document (26,580 blocks) takes another **~10.6 s** (`tools/trace-run.ps1`). FlowDocument has no virtualization; the UI may stutter while large sections are appended. Medium is appended in ~0.4 s |

Screenshots (`bench.py shot`, reviewed): `shots/wpf-medium.png`, `wpf-small.png`, `wpf-large.png`,
`wpf-hw-medium.png`, `wpf-plain-medium.png` (Aero2 theme), scrolled `wpf-medium-scroll1..4.png`
(code, blockquotes, tables, image, h5/h6, lists). `wpf`, `wpf-hw` and `wpf-plain` look the same;
only the scroll bar differs.

## Ideas for further speedups

1. **The WPF main thread is the last big chunk (~225 ms after `Main`).** Realistic reserves:
   - disable stylus/touch (`Switch.System.Windows.Input.Stylus.DisableStylusAndTouchSupport`): ~5 ms, but touch scrolling is lost;
   - remove `FlowDocumentScrollViewer`/`TextEditor` from the first screen: draw the first screen directly
     (`DrawingVisual` + `GlyphRun`/`FormattedText`) and swap in the FlowDocument later. That is ~40–60 ms, but needs custom layout,
     and at that point WPF as a stack stops making sense.
2. The launcher could preload other heavy DLLs too. Tested with WPF's own native DLLs (`wpfgfx_cor3`,
   `PresentationNative_cor3`, `D3DCompiler_47_cor3`): no effect, removed.
3. Virtualization for large files (one element per block in a virtualizing panel, or custom layout). Mandatory for a product.
4. Composite R2R, `DOTNET_TC_*`: expected effect under 10–20 ms. Tiered/PGO/ICU have already been measured; they are worth single to tens of ms.
5. A resident process (`-resident`) would remove ~80 + 150 ms, but it is a separate category under the protocol.
6. NativeAOT and trimming are officially unsupported for WPF (COM, BAML, reflection); this is a dead end.

## Effort estimate for a full product on WPF

| Area | Estimate |
|---|---|
| Rendering polish: footnotes, heading anchors and navigation, an HTML subset, color emoji (rasterized via DirectWrite), rounded code corners | 2–3 weeks |
| Reader features: search, zoom, dark theme, settings, reload on file change, printing, `.md` association, history | 2–3 weeks |
| Large files: a virtualized renderer instead of FlowDocument | 2–4 weeks |
| Production-grade launcher: runtime lookup like the apphost's (registry, `DOTNET_ROOT`), an "install .NET" dialog, signing | ~3–5 days |
| Installer / MSIX, auto-update, signing | ~1 week |
| **Total** | **≈ 2–3 person-months** for a good v1 |

Startup will stay around **300–350 ms** on this machine, 3–4 times slower than native Win32/D2D.
The WPF floor: .NET ~80 ms plus ~225 ms of WPF work on the main thread, provided the GPU driver is loaded
in parallel. On a machine with a different GPU the driver's contribution will differ. Typography is good (DirectWrite,
ClearType, real tables), but emoji are monochrome, and FlowDocument is slow on large files.

## Pitfalls (worth knowing)

- **`CompositionTarget.Rendering +=` creates a `MediaContext`**, and with it ~150 ms of synchronous render thread startup.
  Subscribing in the window constructor just moves this cost elsewhere. For marks, subscribe
  only after `SourceInitialized`.
- **Even `RenderMode.SoftwareOnly` loads D3D9 and the GPU driver:** the WPF display set always enumerates adapters.
- **Software rendering is for the first frame only.** In a maximized 3440 px window, SW scrolling runs at ~30 fps.
  `RenderOptions.ProcessRenderMode` can be set back to `Default` at runtime. To make the window recompute its mode,
  `HwndTarget.RenderMode` has to be "toggled" (SoftwareOnly → Default): while the process is in SW, the window will not leave it
  on its own, even with `RenderMode.Default`.
- **Markdig `Descendants<LinkInline>()` on a `ParagraphBlock` silently returns nothing**: the method only walks
  `ContainerBlock` and `ContainerInline`. Because of this the previous agent's image pre-decoding did not work,
  and decoding (with urlmon) ran on the UI thread, ~27 ms.
- `BitmapImage.UriSource` for a local file calls `MapUrlToZone` (urlmon, ~14 ms on the first call);
  `StreamSource` from a `FileStream` does not.
- **`OverridesDefaultStyle` on TextElements** saves loading the theme dictionary (~20 ms), but for the `Bold` and `Italic` classes
  it is the theme that sets bold and italic. With ODS they become plain text, so `Span`s with an explicit
  `FontWeight`/`FontStyle` are used here. `Hyperlink` also takes its cursor and colors from the theme. ODS must be set in the initializer,
  before adding the first child: adding a child "initializes" the element and triggers the style lookup
  (`new TableCell(para)` is already too late).
- The Aero2 theme is loaded by any element with a `DefaultStyleKey`: Window, FDSV, ScrollBar, Hyperlink, Run, Paragraph…
  One forgotten element and the dictionary gets loaded anyway.
- Warming up the theme on a second thread (`new Window()` + FDSV there) is **slower**: the main thread waits on the
  cctor and dictionary locks (505 vs 460 ms). Only work that does not contend for the same locks is worth moving to a parallel thread.
- `--prelayout` (measure before `Show()`) gives nothing. Arrange creates the visuals and still waits for `MediaContext`,
  and with `--hw` the creation of the window's D3D device stops overlapping with measure and lands on the critical path (471 vs 377 ms).
- Launcher: DPI awareness is taken from the manifest of the **process exe**, so `app.manifest` is embedded in `fastmd-wpf.exe` as well.
  `[STAThread]` on `Main` works even when hosted via `hostfxr_main_startupinfo`, as long as the launcher thread
  has not initialized COM earlier. The preloading thread does not touch COM.
- `InvariantGlobalization=true` without `PredefinedCulturesOnly=false` crashes WPF: the font code creates `CultureInfo("en-US")`.
- Measurements are very noisy: other people's prototypes run on the machine in parallel, and medians vary by 20–60 ms between passes.
- **Harness.** `bench.py run all` runs the variants of **all** prototypes, not just your own (I mistakenly
  launched other people's apps this way). If another variant's exe disappears in the middle of a run (because it is being rebuilt), `run`
  crashes with `FileNotFoundError`: `resolve_exe()` in `run_once` is called outside the `try`, and the results of the entire run are lost.
- Safety hook: directory deletion in `build.ps1` is done via `[System.IO.Directory]::Delete($p, $true)`.

## Files

| Path | What |
|---|---|
| `launcher/launcher.cpp`, `launcher/build-launcher.cmd` | native launcher: preloading D3D9 and the GPU driver + hosting .NET via hostfxr |
| `src/Program.cs` | `Main`, knobs (`Options`), parse thread (`DocLoader`), window (`ViewerWindow`: first screen, progressive appending, marks), `Warmup` (render thread, fonts) |
| `src/MdFlowBuilder.cs` | Markdig AST → FlowDocument per the §5 style; `ImageCache`; block height estimation; table column widths |
| `src/LeanTemplates.cs` | Window / FDSV / ScrollViewer templates in code + `SlimScrollBar` |
| `src/Highlighter.cs` | code highlighting |
| `src/FastMdBenchExt.cs` | `FastMdBench.MarkAt` (a mark with a timestamp taken on another thread) |
| `tools/marks.py` | mark medians from a run file |
| `tools/combine.py` | pooling several runs (median, min, p90) |
| `tools/profile.ps1`, `tools/speedscope_top.py` | startup profile via dotnet-trace → call tree (`... <file> 3 80 "" <frame>` shows the paths to a frame) |
| `tools/modules.ps1` | which DLLs are loaded into the process |
| `tools/trace-run.ps1` | run with `FASTMD_WPF_TRACE=1`: how long appending the rest of the document takes; with `-Extra '--scrolltest[=max]'` — the scroll smoothness test |
| `runs/*.json` | this session's runs: `exp1..9` and `native-1..3` — experiments in order, `knobs*` — the knob tables, `final-quick-2/3` — the final result (in `final-quick.json` and `knobs-medium*.json` the id `wpf` refers to the configuration without the launcher) |

## Audit (independent review, 2026-09-19)

**Verdict: the prototype is honest; no changes were made to the code, the build or `proto.json`.** All four variants
(`wpf`, `wpf-hw`, `wpf-apphost`, `wpf-plain`) are fit for the final sequential measurement.

### What was checked

| Item | Result |
|---|---|
| `t_content` mark | `Window.ContentRendered` → `Dispatcher.BeginInvoke(Background)` → `DwmFlush()` → `Now()` (`FastMdBench.ContentPresented`). This is exactly the PROTOCOL §3 procedure for WPF. `t_window` is set in `ContentRendered`, i.e. on the same frame |
| Resident process, cache, corpus data | None. The launcher loads `d3d9.dll` and the GPU driver inside the same process; nothing is persisted between launches. The font warm-up formats the beginning of the file *being opened* in the same launch. The sources contain nothing from the corpus (grep; `Highlighter` and `Estimate` are generic rules) |
| First screen | 16 of 391 medium blocks are built before the first frame (estimate ≥ 1600 DIP for an 800 DIP window). For large a prefix of 24,931 of 3,008,117 characters is parsed and 16 of 221 blocks are built. On all screenshots and screen captures the first screen is completely filled. The first-screen image is decoded before the first frame (`md-parse`) and is not "loaded in" later |
| Build | `pwsh -File build.ps1` rebuilt from scratch (r2r + launcher, jit, sc-r2r), no errors. `-c Release`. PE check: in `out/r2r`, `FastMdWpf.dll` and `Markdig.dll` have a ReadyToRun header (`RTR` signature); `out/jit` is pure IL. The launcher is built with `/O2 /MT` |
| `bench.py check` (medium and large, all 4 variants) | OK, no warnings, clean exit with code 0. `observed_ms` is always 40–58 ms **earlier** than `t_content`: WPF calls `ShowWindow` before the first frame, and DWM shows the window only after the first frame |
| Window | Client area 1000 × 800 px at 96 DPI (checked via `GetClientRect`), title `medium.md — FastMD (wpf)`, the window is active |

### Independent pixel check

The auditor's script launched the variant with `FASTMD_BENCH_EXIT=0` and repeatedly captured from the screen (`BitBlt` from the screen DC,
~11 ms per strip) two strips of the client area, top and bottom, 160 px each.

| Prototype | Launches | `t_content`, ms | First capture with a change started at |
|---|---|---|---|
| `wpf`, medium | 4 | 286–296 | **6–7 ms** after `t_content`; the last capture that finished before `t_content` is empty |
| `baseline-win32` (calibration) | 3 | 80–90 | ~15 ms after `t_content` |

After the first frame, DWM plays the window-appear animation for another ~100 ms. This animation is the same for all stacks.
Conclusion: the WPF mark is not earlier than the frame actually appearing on screen. The check's accuracy is ~20 ms, the same as for the
native baseline.

### Caveats for comparison (do not violate the protocol)

1. **large.md: the window is barely responsive for ~10 s after the first frame.** Appending the rest takes 9.4 s (`tools/trace-run.ps1`).
   The UI thread is busy the whole time. Checked by pinging with `WM_NULL` via `SendMessageTimeout` every 5 ms for 12 s:
   31 pauses longer than 50 ms, 11.8 s in total, the longest **1.09 s**. The README above says "may stutter";
   in reality it is a freeze with pauses of up to a second. Under §3 appending the rest after the first screen is allowed, but `t_content` for
   large shows the time to the first screen, not to the moment the document becomes usable.
   For medium there were 3 pauses in the first 0.3 s after `t_content`, the longest 126 ms (switching to the GPU and appending);
   after that the window responds immediately.
2. **The native launcher** with GPU driver preloading is a legitimate technique within a single launch. The gain (~60 ms)
   depends on the machine's graphics driver (NVIDIA here, 147 MB of user-mode DLLs). It also adds +54 MB to peak commit.
3. `wpf-plain` is not "bare" WPF. It also parses the file on a separate thread and builds the first screen first.
   Only the startup knobs differ: theme, IME, GPU, IL/JIT, no parallel warm-ups.

### Visual assessment (screenshots `shots/audit-wpf-medium.png`, `audit-wpf-small.png`, `audit-wpf-plain-medium.png`)

All corpus elements are rendered correctly:
- h1–h6 with a rule under h1 and h2;
- bold, italic, bold italic, strikethrough text, colored links;
- inline code;
- bulleted, numbered and nested lists (disc, circle, 1. markers);
- task lists with drawn checkboxes;
- blockquote with a bar;
- highlighted code, GFM tables with column alignment and zebra stripes;
- PNG image;
- Cyrillic and CJK.

DirectWrite typography with ClearType, spacing rhythm like GitHub's. Differences from GitHub:
- emoji are **monochrome** (confirmed by screen capture in both software and GPU mode);
- no rounded corners on code blocks and inline code;
- the circle marker in nested lists is too heavy;
- strikethrough text is additionally dimmed.

Auditor's score: **visual quality 8/10, markup fidelity 8/10.**

### Auditor's control run (noisy, other agents running in parallel)

`bench.py run wpf,wpf-hw,wpf-apphost,wpf-plain --doc small,medium --runs 5` → `runs/audit-sanity.json`. Medians over 5 launches, ms:

| Variant | small | medium | observed (medium) | CPU, ms | peak commit, MB |
|---|---|---|---|---|---|
| `wpf` | 310 | 295 | 249 | 438 | 110 |
| `wpf-hw` | 333 | 313 | 269 | 547 | 136 |
| `wpf-apphost` | 344 | 376 | 330 | 453 | 56 |
| `wpf-plain` | 509 | 508 | 456 | 609 | 82 |

One `wpf-apphost`/medium launch took 2679 ms: `main` was reached only at 2377 ms, i.e. the delay happened before
the app's code (system noise). It does not affect the median. The numbers match the author's table within noise.

### Harness remarks (not fixed, not my files)

- `run_once` calls `resolve_exe()` outside the `try`. If another variant's exe disappears in the middle of `run`, the whole run crashes
  (the prototype author already reported this).
- The result file `results/tmp/<variant>.json` is named only by the variant id. Two simultaneous `check`/`run` invocations
  of the same variant (author and auditor) will overwrite each other.
- The default build also builds `sc-r2r` (145.7 MB), although no variant uses it. This does not affect honesty,
  it only makes the build longer.
