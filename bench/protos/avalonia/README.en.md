[Русский](README.md) | **English**

# avalonia — C# .NET 8 + Avalonia 12 (Skia) + Markdig, NativeAOT

A FastMD prototype on Avalonia: Markdig AST → plain Avalonia controls (TextBlock/SelectableTextBlock with Inlines,
Border, Grid, Image), no XAML, no themes, no bindings. Protocol: `bench/PROTOCOL.md` v1.

**Summary in one line:** the best honest configuration (`avalonia`) shows the content in **~150–170 ms**
(median over small/medium/large, noisy machine), which is ≈ 2× the `baseline-win32` floor (~85–90 ms).
Three levers gave almost everything: **NativeAOT** (−195 ms vs JIT/R2R), **Skia software rendering instead of ANGLE/D3D11**
(−250 ms) and **a small exe launcher + NativeAOT DLL** (−15…45 ms, usually ~35; because of Defender, see §7). Stock Avalonia
(JIT + FluentTheme + ANGLE) on the same code takes ~660 ms.

---

## 1. Stack and versions

| Component | Version |
|---|---|
| .NET SDK / runtime | 8.0.424 / 8.0.30 (NativeAOT: Microsoft.DotNet.ILCompiler 8.0.30) |
| Avalonia (+ .Win32, .Skia, .HarfBuzz, .Themes.Fluent) | **12.1.2** (works with net8.0; the package contains lib/net8.0) |
| SkiaSharp / HarfBuzzSharp | 3.119.4 / 8.3.1.3 |
| ANGLE (GPU variants only) | Avalonia.Angle.Windows.Natives 2.1.27548.20260419 (`av_libglesv2.dll`) |
| Markdig | 1.3.2 (pipe tables, task lists, strikethrough, autolinks) |
| Launcher | C, MSVC 14.44 (`/O1 /MT`), 100 KB |

NuGet packages live in `bench/.tools/avalonia/packages` (see `nuget.config`); nothing is installed globally.

## 2. Build

```
pwsh -File bench/protos/avalonia/build.ps1                 # варианты из proto.json: out/aot-lib, out/aot, out/jit-fluent (~1.5 мин)
pwsh -File bench/protos/avalonia/build.ps1 -Only aot-lib   # только лучший вариант (~25 с)
pwsh -File bench/protos/avalonia/build.ps1 -Only aot-fluent,jit,aot-size   # сборки "ручек", которые мерились при разработке
```

Each build goes to its own `src/obj/<flavor>`, `src/bin/<flavor>`, `out/<flavor>` (cleaned before the build).
Requires .NET SDK 8 and VS 2022 C++ (the NativeAOT linker; the launcher is built via `bench/tools/msvc.cmd`).

| Folder | What it is |
|---|---|
| `out/aot-lib` | NativeAOT **shared library** `fastmd-core.dll` (13.8 MB) + launcher `fastmd-avalonia.exe` (100 KB) + `libSkiaSharp.dll`, `libHarfBuzzSharp.dll`, `av_libglesv2.dll` = 31.3 MB |
| `out/aot` | regular NativeAOT `fastmd-avalonia.exe` (13.8 MB) + the same native DLLs = 31.2 MB |
| `out/jit-fluent` | JIT + ReadyToRun, framework-dependent (requires an installed .NET 8 runtime), FluentTheme = 37.5 MB |
| `out/aot-fluent`, `out/jit`, `out/aot-size` | only for knob measurements (not in proto.json) |

## 3. How startup works (`src/Program.cs`)

1. `Main` (or the `fastmd_main` export in the DLL build): `FastMdBench.Init()` → starts the `md-parse` thread
   (`File.ReadAllText` + `Markdig.Parse`) **in parallel** with Avalonia initialization on the UI thread.
2. `AppBuilder.Configure<App>().UseWin32().UseSkia().UseHarfBuzz()` + `Win32PlatformOptions.RenderingMode`
   (`--render software|angle|wgl|vulkan`), classic desktop lifetime.
3. `App.OnFrameworkInitializationCompleted`: a `Window` is created with **code-only minimal templates**
   (`Theme.cs/MinimalTemplates`: Window, ScrollViewer, ScrollBar, Thumb, ItemsControl — everything a reader needs;
   FluentTheme is not loaded), then the parser is joined.
4. Top-level blocks → an `ItemsControl` with a `VirtualizingStackPanel` (`MdBuilder.BuildVirtualized`): controls
   are created, shaped and laid out **only for visible blocks** (first-viewport-first; this is an allowed
   technique, the rest is built on scroll).
5. **Large files (> 256 KB)**: the parse thread first parses only a prefix (~64 KB, cut at an empty line),
   hands it to the first frame, then parses the whole text; after the first frame `MdBuilder.ApplyFull` replaces
   the last (possibly truncated) block and appends the tail. The first blocks are not recreated (a top-level block is closed
   as soon as the next one starts). If the file has link reference definitions, the whole list is replaced.
6. `Window.Opened` → `TopLevel.RequestAnimationFrame` ×2 (+ waiting for `Compositor.RequestCompositionBatchCommitAsync().Rendered`,
   whichever comes later) → `DwmFlush()` → `t_content` (PROTOCOL §3 for Avalonia). In bench mode with `FASTMD_BENCH_EXIT=1`
   the window closes and the process exits with code 0.

**Launcher (`launcher/launcher.c`)**: `CoInitializeEx(STA)` (same as `[STAThread]`), `LoadLibraryExW("fastmd-core.dll")`,
a call to the `fastmd_main` export (it takes the arguments via `CommandLineToArgvW`). The manifest (PerMonitorV2) is embedded in the launcher.
No resident process and no cache — every launch loads and initializes everything from scratch.

## 4. Variants (proto.json)

| id | exe | parameters | meaning |
|---|---|---|---|
| **`avalonia`** (best) | `out/aot-lib/fastmd-avalonia.exe` | `--render software` | NativeAOT DLL + launcher, Skia software, minimal theme, virtualization, prefix parsing of large files |
| `avalonia-exe` | `out/aot/fastmd-avalonia.exe` | `--render software` | the same, but a regular single 13.8 MB NativeAOT exe — shows the cost of exe size |
| `avalonia-gpu` | `out/aot-lib/fastmd-avalonia.exe` | `--render angle` | the same, but Avalonia's standard GPU rendering (ANGLE/EGL over D3D11) |
| `avalonia-stock` | `out/jit-fluent/fastmd-avalonia.exe` | `--render angle` | "Avalonia by default": JIT + R2R, FluentTheme, ANGLE (same Markdown builder) |

All command-line knobs (to reproduce the knob table below):
`--id <variant>` (window title), `--render software|angle|wgl|vulkan`, `--comp default|winui|dcomp|lowlat|redir`
(Win32CompositionMode), `--ui-thread-render`, `--layout virtual|stack`, `--full` (stack: the whole document before the first
frame), `--text selectable|plain`, `--chunk off` (no prefix parsing), `--warm` (preloading DLLs and fonts
in a background thread), `--scroll <dip>` (debugging: scroll after the first frame, for screenshots).

Common project settings (`src/FastMdAvalonia.csproj`): `InvariantGlobalization`, `UseSystemResourceKeys`,
`ConcurrentGarbageCollection=false`, `EventSourceSupport/DebuggerSupport/MetadataUpdaterSupport=false`,
`BuiltInComInteropSupport=false`; for AOT: `OptimizationPreference=Speed`, `IlcInstructionSet=x86-x64-v3`,
`StackTraceSupport=false`, `TrimMode=full`.

## 5. What is supported (vs PROTOCOL §5)

| Element | Status |
|---|---|
| h1–h6: sizes 30/24/20/17/15/14, semibold, h6 `#59636e`, rule under h1/h2 | ✅ |
| Paragraphs, word wrap, line-height 24 (15 × 1.6), 860 DIP centered column, 32 margins | ✅ |
| **bold** / *italic* / ~~strike~~ (muted color) / links `#0969da` | ✅ links are clickable (http/https/mailto → browser), hand cursor |
| `inline code` | ⚠️ `#eff1f3` background and Cascadia Mono are there; **no rounding and no padding** (Run can't do it) — padding is imitated with spaces |
| Bullet / ordered (with start) / nested / task lists | ✅ markers • ◦ ▪ by level, checkboxes are drawn |
| Blockquotes (4 px bar `#d1d9e0`, text `#59636e`) | ✅ |
| Horizontal rule (2 px, as in style.css) | ✅ |
| Code blocks: Cascadia Mono 13.5, background `#f6f8fa`, radius 6, padding 16 | ✅ **no wrap**, horizontal scrolling inside the block |
| Syntax highlighting | ✅ bonus: own single-pass lexer (c/cpp, csharp, js/ts, json, powershell, python, rust), GitHub light colors |
| GFM tables: borders, bold header, zebra, padding 6/13, column alignment | ✅ a wide table scrolls horizontally |
| Local PNG | ✅ (Skia, scaled down to the column width); remote URLs and SVG — no |
| Cyrillic, CJK, color emoji | ✅ (Skia/DirectWrite font fallback, emoji in color) |
| Scrolling: wheel, ↑↓, PgUp/PgDn/Space, Home/End; Esc closes | ✅ |
| Selection and copy | ⚠️ only **within a single paragraph/block** (one SelectableTextBlock per block); no selection across blocks |
| HTML blocks | ⚠️ shown as monospace text; inline HTML is dropped (except `<br>`) |
| Per-monitor DPI v2, 1000×800 DIP window, title `<file> — FastMD (<id>)` | ✅ |
| Missing: footnotes, heading anchors/TOC, math, mermaid, search, zoom, dark theme | ❌ |

Screenshots (reviewed): `shots/avalonia-medium.png`, `shots/avalonia-small.png`, `shots/avalonia-large.png`,
scrolled `shots/avalonia-dev-s1-medium.png` (code/quote/table/image), `shots/avalonia-dev-s2-small.png`,
`shots/avalonia-dev-s3-large.png` (tail of large after merging the full parse),
`shots/avalonia-gpu-medium.png`, `shots/avalonia-stock-medium.png` (FluentTheme — visually the same, different scrollbar).

## 6. Quick numbers

`python bench/harness/bench.py run avalonia,avalonia-exe,avalonia-gpu,avalonia-stock --doc small,medium,large --runs 5`
(interleaved, 5 measurements + 1 first; **the machine was shared with other agents — noise ±20–40 ms**; raw data
`results/quick-final.json`). Time is from `t0` before CreateProcessW, ms.

| variant | small median (min) | medium median (min) | large median (min) | window visible (obs, medium) | CPU, ms (medium) | WS, MB (s/m/l) | dist |
|---|---|---|---|---|---|---|---|
| **avalonia** | **152.8** (138.8) | **169.9** (151.3) | **158.1** (153.5) | 129 | 109 | 48 / 66 / 163 | 31.3 MB |
| avalonia-exe | 168.9 (164.7) | 200.3 (182.3) | 203.6 (186.1) | 164 | 125 | 48 / 67 / 168 | 31.2 MB |
| avalonia-gpu | 395.6 (370.0) | 404.9 (380.1) | 386.1 (346.9) | 331 | 391 | 93 / 112 / 214 | 31.3 MB |
| avalonia-stock | 661.1 (623.9) | 654.5 (650.2) | 670.9 (631.3) | 563 | 688 | 120 / 139 / 240 | 37.5 MB |
| *baseline-win32 (for comparison, small)* | *88.1 (84.2)* | | | | | | *0.15 MB* |

Cold first launch right after a rebuild (Defender scans the new binaries for the first time): `avalonia` 309 ms,
`avalonia-exe` 332, `avalonia-gpu` 444, `avalonia-stock` 1210 ms (medium, one measurement each).

### Breakdown by marks (medians, `avalonia`, ms from t0)

| mark | small | medium | large | what happens before it |
|---|---|---|---|---|
| CreateProcess returned | 27 | 27 | 25 | process creation (100 KB launcher) |
| `main` | 57 | 60 | 55 | loading the 13.8 MB `fastmd-core.dll`, NativeAOT runtime init, CoInitialize |
| `parsed` / `parsed_prefix` | 58 | 62 | 66 | parallel thread: read + Markdig (large: only the 64 KB prefix) |
| `platform_ready` | 74 | 74 | 71 | Avalonia Win32 + Skia (software) platform init |
| `window_created` | 93 | 91 | 96 | Application, Window object, templates |
| `content_set` | 93 | 92 | 96 | parser join, ItemsControl with blocks |
| `layout_done` | 105 | 120 | 113 | first Measure/Arrange: controls for visible blocks, HarfBuzz shaping, font fallback (CJK/emoji) |
| `opened` (= t_window) | 131 | 144 | 135 | Show(): HWND, activation |
| `raf2` | 148 | 164 | 153 | two frames: Skia software rasterization of 1000×800 + present |
| **t_content** | **153** | **170** | **158** | DwmFlush |

For comparison, `avalonia-gpu` (medium): `main` 54 → `platform_ready` **279** (ANGLE/EGL/D3D11 init ≈ 220 ms) →
`opened` 344 → `raf2` 400 (first GPU frame ≈ 55 ms) → 405. `avalonia-stock`: `main` 66 → `platform_ready` 352
(JIT + ANGLE) → `layout_done` 555 (JIT of layout/text code + Fluent) → 654.

### Knobs (all measured; medium, 6 interleaved measurements, `results/knobs-medium.json`)

| configuration | median | min | conclusion |
|---|---|---|---|
| **avalonia** (best) | **161** | 158 | |
| + `--ui-thread-render` (render on the UI thread) | 161 | 142 | within noise; in other runs sometimes better, sometimes worse — not enabled |
| + `--text plain` (TextBlock instead of SelectableTextBlock) | 175 | 151 | no gain → Selectable kept |
| `--layout stack` (≈1400 DIP of blocks synchronously, the rest after the 1st frame) | 183 | 168 | virtualization is slightly better and is needed for large |
| single 13.8 MB exe instead of the launcher (`avalonia-exe`) | 200 | 177 | **+38 ms** (see §7) |
| FluentTheme (single exe, software) | 245 | 223 | **+45 ms** over `avalonia-exe` |
| JIT + ReadyToRun (single, software, minimal theme) | 394 | 365 | **+195 ms** over `avalonia-exe` |
| ANGLE/D3D11 (`avalonia-gpu`) | 410 | 381 | **+250 ms** |
| JIT + Fluent + ANGLE (`avalonia-stock`) | 671 | 649 | +510 ms |
| `--layout stack --full` (the whole document before the first frame) | 713 | 673 | first-viewport-first saves ~550 ms on medium |
| WGL (OpenGL) | 762 | 668 | worst of all |

More (`results/knobs-large.json`, large): `avalonia` 208 ms, `--chunk off` 293 (+85: the full Markdig parse of 3.7 MB ≈ 150 ms
becomes the critical path), `--layout stack` 288.
Early runs (`results/dev-knobs-*.json`, partly on an old build): `--warm` (preloading Skia/HarfBuzz DLLs and
system fonts in the background) — 170.5 vs 172.1, no effect; `OptimizationPreference=Size` (exe −1.9 MB) — within noise;
`--comp dcomp|lowlat|redir|winui` with ANGLE — within noise (470–550). Composition does not apply to software.

## 7. Finding: EXE size costs ~2.5 ms/MB on every launch (Defender)

Experiment `experiments/exe-size/` (`pwsh -File run.ps1`): a NativeAOT hello (WinExe, 1.3 MB) prints the time on the first line of
Main; a 12 MB "tail" (overlay — never mapped or executed) is appended to copies of it.

| exe | size | CreateProcess, median | until Main, median |
|---|---|---|---|
| hello.exe | 1.3 MB | 22.9–30.2 ms | 37.8–45.2 ms |
| hello + 12 MB random | 13.3 MB | 54.2–58.1 ms | 68.6–73.1 ms |
| hello + 12 MB of zeros | 13.3 MB | 56.6 ms | 71.9 ms |

That is, **CreateProcess grows by ~30 ms for +12 MB of file, even if it is zeros** — on every launch, not only the
first. Most likely this is Defender real-time protection reading/hashing the process image at creation
(can't be verified by disabling it — no admin). Loading the same 13.8 MB code base as a **DLL** carries no such
cost: for `avalonia` (100 KB launcher + DLL) CreateProcess is 25–27 ms, for `avalonia-exe` 55–57 ms; `main` 57–60 vs 84–88.
**This applies to all prototypes with a large exe** (NativeAOT, Rust, static Qt): a small exe + main DLL is
an honest technique (no resident process, no cache) that is worth trying there too.

## 8. Where the time goes (avalonia, medium, ~170 ms)

- ~27 ms — CreateProcess (same as baseline-win32).
- ~33 ms — from CreateProcess to `main`: mapping/relocations of the 13.8 MB DLL, NativeAOT init (≈ 13 ms for hello-world), CoInitialize.
- ~31 ms — Avalonia initialization (Win32 + Skia platform ≈ 14, Application/Window ≈ 17).
- ~29 ms — first layout of visible blocks (control creation, HarfBuzz, font fallback for CJK/emoji on medium; 12 ms on small).
- ~24 ms — Show(): HWND, activation, DWM.
- ~20 ms — two frames (software rasterization of 1000×800 + present), ~6 ms — DwmFlush.
- Markdig parsing runs in parallel and is not on the critical path on small/medium (1–2 ms); on large the 64 KB prefix takes ≈ 2 ms.

## 9. Ideas for further speedup

1. Remove `av_libglesv2.dll` from the software variant's distribution (−5.4 MB of distribution; no effect on startup — it isn't loaded).
2. Shrink `fastmd-core.dll` (13.8 MB): trim Avalonia functionality (Automation/UIA, Dialogs, OpenGL/Vulkan paths
   are not needed), `IlcDisableReflection`/rd.xml — fewer pages and relocations before `main` (potential ~10 ms).
3. Show the window earlier: currently the HWND is shown after the first layout; the background could be shown right away and the
   layout finished by the next frame (earlier t_window, t_content about the same).
4. Lighten the first screen: don't create a `ScrollViewer` for every code block/table until hover/overflow,
   simplify the tree (one `Border` instead of `Border+ScrollViewer+TextBlock`).
5. A custom lightweight "document" control (one `Control` with a `TextLayout` per block and its own hit-test) instead of a control tree —
   fewer objects/styles/bindings; it also gives selection across blocks.
6. DirectPInvoke for user32/gdi32/dwmapi in NativeAOT (remove lazy binding) — a few ms.
7. If acceptable for the product, a `-resident` variant (not done: forbidden without the suffix, and it is a different class of solution).

## 10. Effort estimate for a full product on this stack

The basic reader (this prototype) ≈ 2–3 days. To get to a product:
- selection and copy across blocks + search (Ctrl+F) with highlighting — the most expensive part: a custom document control, **2–3 weeks**;
- dark theme, zoom, font settings — 3–5 days; .md association, drag&drop, recent files, live reload (FileSystemWatcher) — 3–5 days;
- footnotes, anchors/TOC panel, relative links to other .md files, remote images and SVG (Svg.Skia — check AOT) — 1–2 weeks;
- proper syntax highlighting (TextMateSharp or own lexers — check AOT compatibility) — 1 week; math/mermaid are separate big tasks;
- installer (MSIX/Inno), signing, auto-update, tests — 1–2 weeks.

Total: MVP ≈ **4–6 weeks** for one developer, a "polished" product ≈ **2.5–3.5 months**. Pros of the stack: a single code base in
C#, fast software rendering, good Skia/HarfBuzz typography, cross-platform. Cons: ~26–31 MB distribution,
~2× the native floor on startup, NativeAOT + Avalonia require care (no XAML reflection), text selection
has to be written by hand.

## 11. Pitfalls (all actually encountered)

1. **Relative `BaseIntermediateOutputPath`/`MSBuildProjectExtensionsPath` on the command line** (MSBuild global properties
   are not re-normalized) → `nuget.g.props/targets` are not imported → `PublishAot=true` **silently** produces a regular
   self-contained 95 MB JIT output (without ILCompiler and the trimmer). The paths in `build.ps1` are absolute.
2. NativeAOT linking: `findvcvarsall.bat` → `vcvarsall.bat` calls `vswhere.exe` from PATH → «vswhere.exe не является
   командой» ("vswhere.exe is not recognized as a command") and error MSB3073. Fix: add `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer` to PATH (this is also done in `bench/tools/msvc.cmd`).
3. Several flavors in one project: the default `**/*.cs` glob picks up `AssemblyInfo.cs` from `obj/<another flavor>` →
   CS0579 «повторяющийся атрибут» ("duplicate attribute"). Fix: `DefaultItemExcludes += obj/**;bin/**`.
4. `IlcOptimizationPreference` does nothing in .NET 8 — ILCompiler reads `OptimizationPreference` (Speed → `--Ot`, Size → `--Os`).
5. A minimal code-only Window template must draw `Background` (binding `ContentPresenter.Background`), otherwise in software mode the window is **black**.
6. `pwsh -File build.ps1 -Only a,b` passes a single string `"a,b"`, not an array — the script splits it on commas.
7. Avalonia 12: text shaping was moved to `Avalonia.HarfBuzz` — `.UseHarfBuzz()` is needed. Avalonia 12 analyzers are built for
   Roslyn 4.14, SDK 8.0.4xx gives CS9057 warnings (the `x:Name` generators don't work; XAML is not used here).
8. ANGLE on this machine (RTX 5070 + AMD iGPU) initializes in ~220 ms, WGL even longer — for a reader, Skia software rendering is faster and visually identical.
9. The DLL build has no `[STAThread]` — STA is set up by the launcher (`CoInitializeEx`), otherwise OLE (clipboard, drag&drop) in Avalonia is not initialized;
   the arguments are taken from `GetCommandLineW` (a NativeAOT library has no `argv`).
10. The virtualized list estimates the height of blocks not yet built → the scrollbar thumb "floats" while scrolling a long document.
11. A large file with links like `[text][ref]` where `ref` is defined after 64 KB: for the first ~100 ms such links show as text, then the full replacement happens.

## 12. Files

- `src/Program.cs` — startup, arguments, parallel parsing, prefix parsing, marks, rAF×2 → DwmFlush, `fastmd_main` export.
- `src/MdBuilder.cs` — Markdig AST → Avalonia controls, virtualization, `ApplyFull`, `MdText` (selection + link clicks).
- `src/Theme.cs` — style per the spec + minimal code-only templates; `src/CodeHighlighter.cs` — highlighting lexer.
- `launcher/launcher.c` — launcher for the DLL build; `src/app.manifest` — DPI PerMonitorV2 (embedded in both the exe and the launcher).
- `build.ps1`, `proto.json`, `nuget.config`; `results/*.json` — raw runs; `shots/*.png` — screenshots;
  `experiments/exe-size/` — the "exe size vs CreateProcess" experiment.

---

## Audit (independent review, 2026-09-19)

**Verdict: the prototype is honest; no changes to the sources, build or `proto.json` were needed. All four variants are fit
for the final measurement.** Below is what exactly was checked and which caveats to keep in mind when comparing.

### What was checked

1. **The `t_content` mark** (`src/Program.cs` → `common/FastMdBench.cs`): the document enters the tree (`content_set`) before
   `Show()`; then `Window.Opened` → `RequestAnimationFrame` ×2 **and** waiting for
   `Compositor.RequestCompositionBatchCommitAsync().Rendered` (whichever is later) → `DwmFlush()` → `t_content`
   in the process that owns the window. This is PROTOCOL §3 for Avalonia plus an extra safeguard. Waiting for `Rendered` is stricter
   than the protocol: at 165 Hz `DwmFlush` takes ≤ 6 ms, while `raf2 → t_content` over 15 measurements is 4.6–25.5 ms (median ~13),
   i.e. the safeguard adds ~5–10 ms. It is justified: by `raf2` the render thread may not yet have drawn the batch with the document.
   All marks are ≤ `t_content`, `check` gives no warnings on all four variants (medium and large).
2. **Independent pixel check** (the auditor's script `audit/probe.py`). It launches a variant the way the harness does and roughly
   every 12 ms captures the **composited screen** (`GetDC(NULL)` + `BitBlt`, the app window is not touched). 17 launches
   of `avalonia` on medium:
   - if DWM plays the open animation (fade-in ~50–80 ms; `SPI_GETCLIENTAREAANIMATION` = on), the text first becomes
     visible in the semi-transparent window in the capture intervals [212..223] ms with `t_content` 219 (in the previous capture
     [200..212] the window is still blank white), [210..222] with 219 (previous [201..210] blank) and [180..203] with 211
     (frames inspected by eye);
   - if there is no animation, the whole document is on screen **already in the first capture**, 23–54 ms **before** `t_content`
     (5 launches: ≤ 158 ms with 182, ≤ 143 with 173, ≤ 147 with 178, ≤ 220 with 250, ≤ 248 with 301).

   The content never appeared on screen noticeably later than `t_content`, i.e. **the mark is not early, if anything conservative**
   (by 0…~50 ms, ~1–2 frames on average). For comparison, `baseline-win32` under the same probe: the white frame with text
   appears 26–45 ms after its `t_content` (the same DWM animation). The animation effect is common to all prototypes.
   `PrintWindow` is not suitable for such a probe: it blocks on the app's UI thread (~40 ms) and skews the measurement.
3. **`observed_ms` (the harness saw the window) < `t_window`** in all `check` runs (medium 184 vs 203, large 136 vs 170):
   `Show()` makes the window visible before `Opened` fires. `t_window` is conservative too.
4. **No resident process, cache or corpus data.** The launcher (`launcher/launcher.c`) only does `CoInitializeEx` +
   `LoadLibraryExW(fastmd-core.dll)` + the export call. Imports are only ole32/user32/kernel32 (dumpbin); there is exactly
   1 process in the job; only the result file is written to disk. The build has no `EmbeddedResource`; `CodeHighlighter` is a generic
   lexer for 7 languages, with no corpus text in it. The "small exe + NativeAOT DLL" technique is honest: every launch loads
   and initializes everything from scratch.
5. **Prefix parsing of large** (file > 256 KB: the first frame is built from 64 KB up to an empty line, then a full parse
   and merge) is a legitimate form of first-viewport-first. The prefix's top-level blocks, except the last one, match the full
   parse; the last one is replaced; the corpus has no link reference definitions. Verified with captures:
   `shots/audit-avalonia-large-scroll400000.png` (the merge boundary, sections 36–37, rendered correctly) and
   `shots/audit-avalonia-large-end.png` (after End, the tail of `large.md` is visible: section 1659.3, h5/h6, hr — the document
   is merged in full). In check, the full parse of 3.7 MB finished before `t_content` (`parsed_full` 173 ms with `t_content` 201),
   but the first frame is built from the prefix.
6. **Build.** `build.ps1` was rebuilt by the auditor from scratch (`pwsh -File build.ps1`, 57 s, exit 0; the artifacts in `out/` come from
   this rebuild). `out/aot-lib` and `out/aot` are real NativeAOT: there are no managed assemblies, `fastmd-core.dll`
   exports exactly `fastmd_main`. `out/jit-fluent` really is ReadyToRun: `Avalonia.Base.dll` 2.35 MB IL →
   7.15 MB, `Markdig.dll` 0.49 → 1.40 MB. Settings: Release, `OptimizationPreference=Speed`,
   `IlcInstructionSet=x86-x64-v3`, `TrimMode=full`; launcher `/O1 /MT`, GUI subsystem.
7. **Window**: client area 1000×800 DIP (capture with frame 1002×832 at 100 %), title `<file> — FastMD (<id>)`,
   PerMonitorV2 (manifest embedded in both the launcher and the exe), exit code 0.

### Visual quality (screenshots `shots/audit-*.png`)

Reviewed: medium/small of all variants plus the scrolled `audit-avalonia-medium-scroll780.png`,
`audit-avalonia-medium-scroll1500.png` and `audit-avalonia-large-end.png`. Very close to GitHub:
- h1–h6 headings with correct sizes, rules under h1/h2, h6 muted;
- paragraphs with line-height 24, 860 column;
- links `#0969da`, muted strikethrough;
- • ◦ markers and nested numbering, task-list checkboxes;
- blockquote with a bar;
- `#f6f8fa` code block with rounding and highlighting in GitHub colors;
- GFM table with borders, bold header, zebra and alignment;
- local PNG, Cyrillic, CJK, color emoji.

Differences:
- inline code has no rounding, and padding is imitated with spaces — an extra space is visible before a comma (`inline code ,`);
- comments in code are italic (GitHub's are not);
- task checkboxes sit in the marker column.

The GPU and Fluent variants are visually identical to software; Fluent has a different scrollbar. **Rating: visuals 8/10, completeness 8.5/10.**

### Caveats (do not affect honesty)

- The old `shots/avalonia-dev-s*.png` were taken at 03:30, **before** the last edit of `MdBuilder.cs` (03:31) and the rebuild;
  the current ones are `shots/audit-*.png`.
- `avalonia-stock` is not quite "stock" Avalonia. The code is the same optimized one: virtualization, parallel parse,
  `InvariantGlobalization`, `TieredPGO=false`. Only JIT+R2R, FluentTheme and ANGLE are "stock". The variant is
  framework-dependent: the 37.5 MB distribution excludes the .NET runtime.
- `dist` of `avalonia` includes `av_libglesv2.dll` (5.4 MB), which only `avalonia-gpu` needs.
- `IlcInstructionSet=x86-x64-v3` requires AVX2. That's fine for measuring on a 7950X; a product needs v2 or a CPU check.
- `--scroll` (debugging) is applied before the full parse of large is merged, so scrolling is limited to the prefix.
- Harness: `observed_ms` only registers window visibility, not document pixels, so it would not have caught an early `t_content`
  with an early-shown window. That is why the auditor needed a pixel probe. No harness bugs were found.

### Auditor's numbers (noisy machine, other agents running in parallel)

`bench.py run avalonia,avalonia-exe,avalonia-gpu,avalonia-stock --doc small,medium --runs 5`
(`results/audit-run.json`), median (min), ms:

| variant | small | medium | obs (medium) | CPU, ms (medium) | peak commit, MB (medium) |
|---|---|---|---|---|---|
| avalonia | 157.3 (142.6) | 182.6 (159.1) | 132.8 | 125 | 42.6 |
| avalonia-exe | 205.2 (165.2) | 191.5 (169.0) | 153.8 | 125 | 42.2 |
| avalonia-gpu | 405.7 (358.9) | 440.9 (413.7) | 357.8 | 406 | 184.8 |
| avalonia-stock | 696.0 (637.9) | 672.4 (630.6) | 565.4 | 625 | 186.0 |

A quieter moment, interleaved with the floor (`results/audit-vs-baseline.json`, 4+1 launches): `baseline-win32` —
80.4 / 89.0 ms (small / medium), `avalonia` — 129.3 / 140.8 ms, i.e. ≈ +50 ms (≈1.6×) over the floor.

Auditor's scripts: `audit/probe.py` (pixel probe: `python audit/probe.py <variant> <doc> [runs] [--save]`) and
`audit/scrollshot.py` (screenshot with extra arguments / the End key).
