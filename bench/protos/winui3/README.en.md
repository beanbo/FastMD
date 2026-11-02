[Русский](README.md) | **English**

# winui3 — C# .NET 8 + WinUI 3 (Windows App SDK 1.8, unpackaged) + Markdig

A native XAML viewer: Markdig parses the Markdown on a background thread, and the AST is turned **in code** (no XAML markup)
into a tree of `TextBlock` / `Border` / `Grid` / `Image`; only the first screen of the document goes into the first frame, the rest
is appended after the frame. The `winui3-webview2` variant is the same exe, but the document is shown in the WinUI WebView2 control
(Markdig → HTML + `bench/common/style.css`).

**Bottom line:** the best honest configuration (`winui3`: NativeAOT `OptimizationPreference=Speed` + WinAppSDK
self-contained, no XamlControlsResources/Mica) gives **t_content ≈ 370–385 ms**, the same for small/medium/large
(3.7 MB), against a machine floor of `baseline-win32` ≈ 73 ms. Of that, ≈ 140 ms is waiting for the first layout/frame, mostly
creation of the D3D11 device on the NVIDIA GPU inside the XAML compositor, ≈ 100 ms is fixed WinUI initialization (XAML core,
window, `Activate`), ≈ 65 ms is process start up to `main`, and 35–65 ms is rendering the first frame. Parsing the Markdown and building the
first screen take < 10 ms. Without a resident process it cannot get faster on this stack; "like every
WinUI app" (Mica + Fluent resources) adds another +125…170 ms, WebView2 inside WinUI +220 ms, JIT+R2R instead of AOT
+40…65 ms.

---

## 1. Stack and versions

| Component | Version |
|---|---|
| .NET SDK / runtime | 8.0.424 / 8.0.30; NativeAOT — `Microsoft.DotNet.ILCompiler` 8.0.30, linked with MSVC 14.44 |
| TFM | `net8.0-windows10.0.22621.0`, `win-x64`, Windows SDK projection 10.0.22621.56 |
| Windows App SDK | component package `Microsoft.WindowsAppSDK.WinUI` **1.8.260803003** (not the metapackage, see §9); for the framework-dependent build — `Microsoft.WindowsAppSDK.Runtime` 1.8.260804001 (framework package 8000.946.1701.0 installed on the machine) |
| WebView2 | SDK `Microsoft.Web.WebView2` 1.0.3179.45 (comes with WinAppSDK), Runtime 153 |
| Markdown | Markdig **1.3.2** (pipe tables, task lists, strikethrough, autolinks) |
| Code highlighting | custom single-pass lexer (`src/Highlighter.cs`, ~110 lines, no regex) |
| Packaging | unpackaged (`WindowsPackageType=None`), no MSIX, xcopy folder |

Machine: Ryzen 9 7950X, 64 GB, NVIDIA RTX 5070 + AMD iGPU, Windows 11 26200, Defender enabled.

## 2. How to build

```
pwsh -File bench/protos/winui3/build.ps1                 # все 5 конфигураций с нуля, ~55–70 с
pwsh -File bench/protos/winui3/build.ps1 -Only aot,r2r   # только нужные
```

- NuGet packages go to `bench/.tools/winui3/packages` (`nuget.config`); nothing global.
- Each configuration: `dotnet publish -c Release -r win-x64` into `out/<name>`, intermediate files in `obj/<name>`;
  both folders are deleted before the build (the script only cleans folders inside the prototype, via `[IO.Directory]::Delete`).
- NativeAOT linking needs `vswhere.exe` on `PATH` (vcvarsall calls it without a path) — `build.ps1` adds
  `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer` itself.

| Folder | MSBuild properties | Size | Used for |
|---|---|---|---|
| `out/aot` | `FmAot=true`, `FmIlcPref=Speed`, `WindowsAppSDKSelfContained=true` | 58.1 MB, 235 files (exe 7.0 MB + 87 WinUI MUI localization folders) | `winui3`, `winui3-webview2` |
| `out/aot-pri` | same as `aot` + `EnableMsixTooling=true` (generates `fastmd-winui3.pri`) | 59.4 MB | `winui3-fluent` (XamlControlsResources does not work without a PRI) |
| `out/aot-fd` | `FmAot=true`, `WindowsAppSDKSelfContained=false` (bootstrapper + installed WinAppSDK 1.8) | **8.2 MB, 5 files** | knob measurement only |
| `out/r2r` | JIT + ReadyToRun, .NET 8 framework-dependent, WinAppSDK self-contained | 100.8 MB (R2R CsWinRT projections) | `winui3-r2r` |
| `out/jit` | pure JIT, .NET framework-dependent | 87.4 MB | knob measurement only |

## 3. How it works (what makes startup fast)

1. **Custom `Main`** (`DISABLE_XAML_GENERATED_MAIN`, WinAppSDK auto-initializers disabled): the first line is
   `FastMdBench.Init()` (the `main` mark), the `parse` thread starts immediately (file read + Markdig), then we do by hand what
   `UndockedRegFreeWinRT-AutoInitializer` does (`MICROSOFT_WINDOWSAPPRUNTIME_BASE_DIRECTORY` +
   `WindowsAppRuntime_EnsureIsLoaded`); for framework-dependent — `Bootstrap.TryInitialize(1.8)`.
   `InvariantGlobalization=true` (no ICU), `UseSystemResourceKeys=true`.
2. **Minimal XAML infrastructure:** `Application` without `App.xaml`, without `XamlControlsResources`, without Mica; brushes and
   fonts are created once (`Theme.cs`) and shared by all elements (every WinRT object creation is an interop call).
3. **The document is code, not markup** (`MdRenderer.cs`): a paragraph/heading = one `TextBlock` (lighter than `RichTextBlock`),
   a plain paragraph goes in as a single `TextBlock.Text`; inline code is a `Run` in a monospace font + a `TextHighlighter` with a background;
   a code block = `Border` + `TextBlock` with colored `Run`s; a table = `Grid`; lists are a `Grid` with a "hanging" marker;
   a blockquote is a `Border` with a left bar. CSS-like margin collapsing is emulated.
4. **First viewport first (allowed by the protocol):** the first frame gets only the top-level blocks covering
   ≈1.5 window heights according to a rough height estimate (`MdRenderer.Estimate`) — on medium that is 13 of 391 blocks. The rest
   is appended after the frame in chunks of ≤12 ms at `DispatcherQueuePriority.Low` (scrolling works right away).
5. **Head-first parse for large files (> 256 KB):** first a head of ≥32 KB is parsed, cut at a blank line
   outside fenced code; all head blocks except the last one match the full parse (the last one is not used).
   The full parse starts **after the first frame** (its ~100 MB of allocations trigger GC, which also stops the
   UI thread); then the head blocks are matched by `Span` and the tail is appended. If there are link
   reference definitions after the head, the first screen is redrawn from the full parse.
6. **Order on the UI thread:** first `new Window()` (starts the compositor), then waiting for the parse (ready by this point),
   building the first screen (~2–4 ms), `Content`, `Activate()`.
7. **t_content (PROTOCOL §3):** root `Loaded` → first `CompositionTarget.Rendered` → `DwmFlush()` → writing the
   result. `t_window` is the `Window.Activated` event. `blocks_in_first_frame=13/391` is written to `notes`.
8. **WebView2 variant** (`WebHost.cs`): `CoreWebView2Environment` is created first thing in `OnLaunched`
   (in parallel with creating the XAML window), the first-screen HTML is built on the parse thread, the page = `style.css` + article +
   `rAF(rAF(postMessage))`; the tail is sent via `PostWebMessageAsString` after the frame (which also sidesteps the 2 MB limit of
   `NavigateToString`). Images go through `SetVirtualHostNameToFolderMapping`. Browser arguments are the "lean" set
   found by the `cpp-webview2` prototype (`--disable-gpu --in-process-gpu`, network service in-process, no background
   services): on the same runtime −84 ms and 5 processes instead of 7 (§5).

## 4. Variants (`proto.json`)

| id | exe / args | What it is |
|---|---|---|
| **`winui3`** (best) | `out/aot/fastmd-winui3.exe --id winui3` | NativeAOT (Speed), WinAppSDK self-contained, everything from §3 |
| `winui3-webview2` | `out/aot/... --webview` | same exe, document in WinUI WebView2 + `style.css`, lean browser args |
| `winui3-r2r` | `out/r2r/fastmd-winui3.exe` | "classic .NET": JIT + ReadyToRun, system .NET 8 — the cost of JIT |
| `winui3-fluent` | `out/aot-pri/... --fluent --mica` | "a typical good-looking WinUI app": `XamlControlsResources` + `MicaBackdrop` + content in the window title bar |

All knobs remain command-line flags (to reproduce §5):

| Flag | Effect |
|---|---|
| `--id <id>` | variant id in the window title |
| `--webview` | WebView2 instead of native XAML |
| `--wvdefault` | WebView2 with the default browser args (without the lean set) |
| `--fluent` | `XamlControlsResources` (requires the `out/aot-pri` build, otherwise it crashes — see §9) |
| `--mica` | `MicaBackdrop` + `ExtendsContentIntoTitleBar` + custom title bar (light theme) |
| `--full` | the whole document before the first frame (no first-viewport-first and no head-first) |
| `--nohead` | no head-first parse (large files wait for the full parse) |
| `--noselect` | `IsTextSelectionEnabled=false` |
| `--d3dwarm` | "warm up" a D3D11 device on a background thread from the first line of `Main` |
| `--dwwarm` | "warm up" DirectWrite (layout of a Latin/Cyrillic/CJK/emoji string + a monospace one) on a background thread |
| `--scrollto <dip>` | debugging: load the rest of the document and scroll (for screenshots of the middle of the document) |

## 5. Knobs tested

Measured with `bench.py run`, interleaved order, median `t_content`, ms. Other agents were loading the machine in parallel —
differences < ~20 ms are treated as noise; only rows from the same run are comparable. Raw data — `out/*.json`.

**Run 1 — `knobs-medium.json`** (medium, n=9; build still with "blended" `-O`, see run 3):

| Configuration | t_content | Δ vs `winui3` | What the marks show |
|---|---|---|---|
| **`winui3`** (AOT, WinAppSDK self-contained) | **416** | — | — |
| + app PRI (`aot-pri`, no Fluent) | 408 | noise | having a `.pri` costs nothing |
| + `--noselect` | 410 | noise | text selection in `TextBlock` does not affect startup |
| + `XamlControlsResources` | 423 | +7 (noise) | `xcr_merged` ≈ 6 ms; the style dictionary is loaded lazily |
| AOT + WinAppSDK **framework-dependent** (`aot-fd`) | 427 | +11 (noise) | bootstrapper ~2 ms; dist 8 MB instead of 58 |
| + `--d3dwarm` | 434 | noise | the warm-up device is ready at ~280 ms, but XAML still waits for its own — no gain (driver/loader lock serialize) |
| JIT, .NET framework-dependent (`jit`) | 487 | +71 | `app_start` +37…60 ms (JIT of CsWinRT projections), parse 20–45 ms, screen build +20…30 ms |
| JIT + ReadyToRun (`r2r`) | 506 | +90 | R2R barely helps: CsWinRT projections are partly JIT-compiled; `main` ~7 ms later (hostfxr + coreclr), although `CreateProcess` is 16 ms faster (0.26 MB apphost vs the 7 MB AOT exe) |
| + `--mica` | 594 | **+178** | assigning `SystemBackdrop` synchronously creates a GPU device (+240…280 ms on the UI thread) |
| `winui3-fluent` (XamlControlsResources + Mica) | 669 | +253 | same + the window appears ~250 ms later |
| `--full` (whole document in the first frame) | **1042** | **+626** | 391 blocks: layout +340 ms, first frame +430 ms — first-viewport-first gives ×2.5 |
| WebView2 with default browser args | 786 | +370 | 7 processes, CPU ~1.9 s |

**Run 2 — `knob-wvargs.json`** (medium, n=8): WebView2 default args **722** → lean args **638** (−84 ms,
5 processes instead of 7, CPU 1.9 → 1.6 s). The lean set is kept in `winui3-webview2`.

**Run 3 — `knob-ilcpref.json`** (n=11). The previous agent set `IlcOptimizationPreference=Speed`, which in .NET 8
**has no effect** (the property is called `OptimizationPreference`; `.ilc.rsp` had `-O`, i.e. blended):

| NativeAOT | medium | small | exe |
|---|---|---|---|
| blended `-O` (before) | 374 | 349 | 6.61 MB |
| **`OptimizationPreference=Speed`** (`--Ot` + static PGO), chosen | **366** | **344** | 6.96 MB |
| `OptimizationPreference=Size` (`--Os`) | 375 | 356 | 6.36 MB |

Speed is consistently 5–9 ms faster (less time on the UI thread: `window_created`/`activate_ret`); Size is within noise.

**Run 4 — `knob-dwwarm.json`** (n=13): `--dwwarm` 379 / 347 vs 376 / 354 (medium / small) — no effect:
the whole DirectWrite warm-up takes ~1 ms (font cache of the system service), i.e. the first-frame difference between medium (≈65 ms)
and small (≈35 ms) is not fallback fonts but the amount of rasterization.

**Large file** (large.md, 3.7 MB), `knob-head-large2.json`, n=7:

| Configuration | t_content | Comment |
|---|---|---|
| `winui3` (head-first, full parse after the frame) | **428** | head parsed in 1.4 ms; large ≈ medium |
| `--nohead` (wait for the full Markdig parse) | 523 | full parse of 3.7 MB ≈ 280 ms in the background + its GC pauses slow down the UI thread (`window_created` +108 ms instead of +45) |
| head-first, but full parse in parallel with the UI (intermediate version, `knob-head-large.json`) | 469 | GC of the full parse still interfered with the UI thread → parse moved to after the frame |

Result for `winui3`: NativeAOT + Speed (−40…65 ms vs R2R), WinAppSDK self-contained (framework-dependent is equally
fast within noise, but requires the runtime to be installed), no Fluent resources and no Mica, first-viewport-first +
head-first parse.

## 6. Quick numbers (`out/quick.json`, `bench.py run … --runs 5`, interleaved, median of 6 launches)

| Variant | small | medium | large | t_window | CPU, ms | Processes | WS, MB | Commit, MB | dist, MB |
|---|---|---|---|---|---|---|---|---|---|
| **`winui3`** | **372** | **384** | **384** | 143–157 | 390–485 | 1 | 96–110 | 154–175 | 58.1 |
| `winui3-r2r` | 414 | 436 | 447 | 226–238 | 530–595 | 1 | 127–142 | 161–182 | 100.8 |
| `winui3-fluent` | 497 | 537 | 556 | 395–408 | 470–500 | 1 | 109–122 | 166–185 | 59.4 |
| `winui3-webview2` | 593 | 607 | 622 | 163–176 | 1405–1440 | 5 | 95–108 (host only) | 276–291 | 58.1 + Runtime |
| `baseline-win32` (calibration, in the same hour, `baseline-ref.json`) | — | 73 | — | 65 | 47 | 1 | 14 | 3 | 0.15 |

The very first launch after a rebuild can take up to ~2.7 s (Defender scans the 235 new files); subsequent ones are
as in the table. The numbers include noise from neighboring agents; the final comparison is the orchestrator's sequential run.

### Breakdown by marks (medians, ms from `t0`, excluding the first launch)

**`winui3`, medium** (large is within ±10 ms; for small the last stage is 34 ms instead of 66):

| Mark | ms | Δ | What happens |
|---|---|---|---|
| `CreateProcess` returned | 39 | 39 | Defender + mapping the 7 MB exe (24 ms for the 0.26 MB apphost of the JIT builds) |
| `main` | 65 | 26 | loader, NativeAOT runtime init |
| `parsed` (`parse` thread) | 67 | 2 | read + Markdig on 53 KB — off the critical path |
| `winappsdk_init` | 69 | 2 | `WindowsAppRuntime_EnsureIsLoaded` |
| `app_start` | 91 | 22 | `Application.Start`: XAML core (Microsoft.ui.xaml.dll 15 MB etc.) |
| `launched` | 97 | 6 | `OnLaunched` |
| `window_created` | 138 | 41 | `new Window()`: HWND, AppWindow, in-proc compositor (dcompi/dwmcorei), input |
| `first_viewport_built` | 145 | 7 | waiting for the parse (already done) + 13 XAML blocks |
| `activate_ret` | 176 | 31 | `Content` + `Activate()` |
| `loaded` | 316 | **141** | first layout (≈7 ms for 13 blocks, per `--full` data) + **waiting for the NVIDIA D3D11 device** (§7) |
| `frame` (`CompositionTarget.Rendered`) | 383 | 66 | rendering the first frame: text rasterization, emoji, checkboxes |
| `t_content` | 384 | ~2–8 | `DwmFlush()` |

**`winui3-webview2`, medium:** `main` 63 → first-screen HTML 66 → `wv_env_started` 102 → `window_created` 139 →
`wv_env_ready` 144 → `activate_ret` 174 → **`wv_core_ready` 452 (+278: browser/renderer processes, controller)** →
`NavigateToString` 453 → rAF×2 → `frame` 599 (+146: renderer, HTML/CSS parsing, layout, paint) → 607.

**`winui3-r2r`, medium:** `main` 67 → `parsed` 92 (Markdig gets JIT-compiled) → `app_start` 148 (**+56** vs +22 for AOT) →
`window_created` 189 → `first_viewport_built` 219 (+19 renderer JIT vs +2) → `loaded` 366 → `frame` 431 → 436.

**`winui3-fluent`, medium:** `window_created` 142 → `SystemBackdrop = new MicaBackdrop()` → 386 (**+244**) →
`activate_ret` 428 → `loaded` 472 (+43: the device already exists) → `frame` 532 → 537.

## 7. Where the time goes

1. **GPU device (≈100–140 ms on the critical path, the main item).** `tools/d3dprobe.c` (fresh process,
   `D3D11CreateDevice`): NVIDIA RTX 5070 — **190–213 ms** for the first device in a process (loading and initializing
   the driver UMD), 26–45 ms for subsequent ones; AMD iGPU — 44–48 / 23–31 ms; WARP — 15–21 / 1–2 ms. The WinUI 3
   XAML compositor creates the device on the default adapter (NVIDIA here) asynchronously after the window is created and waits for it
   before the first frame; there is no API to pick the adapter or WARP. With Mica this is directly visible: +244 ms synchronously in
   `SystemBackdrop`. Warming up on a background thread did not help (§5).
2. **Fixed WinUI initialization ≈ 100 ms:** XAML core 22 ms, window 41 ms, `Activate` 31 ms, other ~6.
3. **First frame 35–65 ms:** rasterizing the text of the first screen (DirectWrite warm-up has no effect, §5).
4. **Process start ≈ 65 ms before `main`** — almost as much as the whole `baseline-win32`: Defender + the 7 MB AOT image.
5. Parsing the Markdown and building the first-screen XAML — **< 10 ms** in total, including for 3.7 MB (head).
6. JIT/R2R (outside AOT) adds 40–70 ms, WebView2 ≈ +220 ms over native XAML (starting the browser and renderer processes).

## 8. Spec support (PROTOCOL §5)

Screenshots (all reviewed): `out/shots/winui3-{medium,small,large}.png`, `winui3-webview2-{medium,small}.png`,
`winui3-r2r-{medium,small}.png`, `winui3-fluent-{medium,small}.png`. The middle of medium (C# code, table, h5/h6,
image, blockquote) was checked with debug screenshots using `--scrollto`.

| Requirement | `winui3` (native XAML) | `winui3-webview2` |
|---|---|---|
| Per-monitor DPI v2, client 1000×800 DIP, title per the protocol | yes (`app.manifest`, `AppWindow.ResizeClient` × DPI) | yes |
| Body font Segoe UI Variable Text 15 px, line-height 1.6, `#1f2328`, column 860 / margins 32 | yes (`LineHeight=24`, `BlockLineHeight`) | yes (`style.css`) |
| h1–h6 sizes, semibold, h1/h2 with a `#d1d9e0` rule, h6 `#59636e` | yes | yes |
| **bold** / *italic* / ~~strike~~ / links `#0969da` (clickable, open in the browser) | yes (semibold 600; strikethrough is dimmed) | yes |
| `inline code`: background `#eff1f3`, Cascadia Mono | yes, background via `TextHighlighter`; **no rounded corners**, padding emulated with thin spaces | yes |
| Lists: bulleted / numbered / nested / task lists | yes (markers • ◦ ▪, custom checkboxes) | yes (native checkboxes) |
| Blockquotes (4 px bar, text `#59636e`), hr | yes | yes |
| Fenced code: mono 13.5 px, background `#f6f8fa`, radius 6, padding 16 | yes; **long lines wrap** (no horizontal scrolling — cheaper) | yes (scrolling via CSS) |
| Syntax highlighting | **yes**, custom lexer: C/C++/Java, C#, JS/TS, Rust, Python, PowerShell, Bash, JSON, Go | no (Markdig HTML without highlighting) |
| GFM tables: borders, bold header, zebra stripes, alignment, padding 6/13 | yes (`Grid`) | yes |
| Local PNGs | yes, if the image is a paragraph of its own (size is reserved from the PNG header, decoding is asynchronous); **inline images inside text are a "🖼 alt" placeholder** (there are none in the corpus) | yes, all |
| Cyrillic / CJK / emoji | yes (color emoji via fallback) | yes |
| Scrolling | yes; keyboard (arrows/PgDn/Home/End) right after the frame | yes |
| Text selection | within a single block (paragraph/cell); **not across multiple blocks** | yes |
| Raw HTML blocks | shown as text | rendered |

**Prototype limitation — no virtualization.** Each block is a live XAML subtree (~100+ KB including selection
support). Sequentially appending all 26,580 blocks of large.md caused the process to hang (0 % CPU,
"not responding") at about 5,200 blocks / 680 MB, plus `StackPanel` measures all children on every pass (O(n²)).
Therefore the rest of the document is appended **as you scroll** (12 screens ahead, `ScrollViewer.ViewChanged`) and is
capped at 3,000 top-level blocks with a visible notice at the end (medium has 391 blocks, shown in full). Verified by
scrolling large.md via UI Automation: the process stays responsive, ~260 MB. A product needs real virtualization
(`ItemsRepeater` + `IElementFactory` or custom "chunks" with height estimation).

Not supported in any variant (not in the Markdig pipeline): footnotes, math, mermaid, heading anchors and
navigation via internal links; also no dark theme, zoom, or search.

## 9. Pitfalls (where the time went)

- **The `Microsoft.WindowsAppSDK` 1.8 metapackage** pulls in AI/ML components (onnxruntime, DirectML, ~40 MB): dist 105 MB
  vs 58 MB with the component package `Microsoft.WindowsAppSDK.WinUI`. Framework-dependent WinAppSDK additionally needs an explicit
  reference to `Microsoft.WindowsAppSDK.Runtime`, otherwise the build fails.
- **`OptimizationPreference`, not `IlcOptimizationPreference`** (the latter is silently ignored in .NET 8).
- **`XamlControlsResources` in an unpackaged app without `App.xaml`:** (1) `Application.Resources` in the constructor of the
  code-only `App` throws `E_UNEXPECTED` — the dictionary is added in `OnLaunched`; (2) without an app PRI
  (`EnableMsixTooling=true` → `fastmd-winui3.pri`) — `COMException: Cannot locate resource from
  'ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml'`, and the process crashes with `0xC000027B`. An
  `IXamlMetadataProvider` on the `Application` (`FluentApp`) is also required.
- **Mica is expensive**: +180…240 ms on this machine (synchronous GPU device creation). With the Windows dark theme, Mica and
  the title bar buttons are dark — `TitleBarTheme.Light` and `RequestedTheme=Light` are set. Programmatic focus on the
  `ScrollViewer` drew a border around the whole document — `UseSystemFocusVisuals=false`.
- **NativeAOT + MSVC:** `findvcvarsall.bat` → `vcvarsall.bat` calls a bare `vswhere.exe` — it has to be on `PATH`.
- Builds with different properties must have separate `obj` folders (`BaseIntermediateOutputPath`), and `Compile` must be an explicit
  list: otherwise a stale `src/obj/**/*.cs` gets compiled in.
- **No virtualization → hang on large documents** (§8): the previous version of the prototype appended the whole document and
  on large.md hung ~12 s after opening (this did not affect the measurement: in bench mode it exits right after the frame).
- One `TextBlock` per paragraph ⇒ no selection across paragraphs; a `RichTextBlock` with `Paragraph`s would provide it, but
  it is heavier and cannot hold tables/code blocks without `InlineUIContainer`. `TextHighlighter` cannot do rounded corners/padding.
- The large AOT exe (7 MB) costs ~15 ms in `CreateProcess` vs the 0.26 MB apphost (Defender), but JIT eats more.

**Harness observation (not fixed):** `bench.py shot` hangs forever if the app's UI thread is hung —
`capture_window` (apparently `PrintWindow`/`SendMessage` without a timeout) waits for the window. That happened with the hang from §8 before
it was fixed; the harness was released only after I killed my process. Suggestion: `SendMessageTimeout`
or capturing on a thread with a timeout.

## 10. Ideas for further speedups

1. **GPU adapter:** on hybrid machines, a per-app GPU preference of "power saving" (what Settings → Display → Graphics
   («Параметры → Дисплей → Графика») writes, key `HKCU\Software\Microsoft\DirectX\UserGpuPreferences`) would make DXGI give the compositor the
   iGPU: per `d3dprobe`, the first device takes 45 ms instead of ~200 — potentially **−100…150 ms**. Not tested here
   (changing user settings/the registry is forbidden by the rules); an installer could do it. It will not help on machines with a single
   discrete GPU.
2. **Resident variant (`-resident`, PROTOCOL §6):** keep a process with XAML initialized and the
   D3D device ready; a new document window = `new Window()` + first frame, estimated 100–150 ms. The only way to
   get "bullet-fast" on WinUI; not done.
3. **Virtualization** (`ItemsRepeater`/chunks) — will not speed up the first frame, but removes the 3,000-block limit and the memory cost.
4. Less work in the first frame: a budget of 1.0 screens instead of 1.5 — a few ms (layout ≈0.5 ms/block).
5. Size: drop the 87 WinUI MUI localization folders if not needed; framework-dependent WinAppSDK (8 MB) — speed within
   noise, but requires an installed Windows App Runtime 1.8.

## 11. Effort estimate for a full product on this stack

| Area | Estimate |
|---|---|
| Document virtualization (`ItemsRepeater`), anchors, link navigation, text search | 2–3 weeks |
| Selection and copy across blocks (custom selection layer or a `RichTextBlock` per section) | 1–2 weeks |
| Images (inline, remote, SVG), highlighting via TextMate grammars, footnotes, math | 2–3 weeks |
| Dark theme, zoom, settings, print/export, auto-reload on file change | 2 weeks |
| `.md` association, installer (MSIX or xcopy + registration), signing, auto-update | 1 week |
| Total MVP / polished product | **~6–8 / 12–14 person-weeks** (a C# developer familiar with WinUI) |

Pros of the stack: native Fluent look, good DirectWrite typography, C# and Markdig (fast, extensible), accessibility
(UIA) out of the box. Cons: a floor of ~370–400 ms cold start on this machine (mostly GPU/XAML initialization,
which code cannot fix), 58 MB self-contained or a dependency on an installed Windows App Runtime, no ready-made
browser-grade rich-text control (selection across blocks, inline objects).

## 12. Files

| Path | What |
|---|---|
| `src/Program.cs` | custom `Main`, arguments, parse thread (head-first), WinAppSDK initialization, `--d3dwarm`, `--dwwarm` |
| `src/App.cs` | window, first-viewport-first, loading more on scroll, marks, Mica/Fluent |
| `src/MdRenderer.cs` | Markdig AST → XAML |
| `src/Highlighter.cs` | code highlighting lexer |
| `src/Theme.cs` | colors/fonts per the spec |
| `src/WebHost.cs` | WebView2 variant |
| `src/FastMdWinUI.csproj`, `src/app.manifest`, `nuget.config` | project (MSBuild knobs are described in the csproj header) |
| `build.ps1`, `proto.json` | building all configurations, variants |
| `tools/d3dprobe.c` (+ `.exe`) | measures the cost of creating D3D11 devices (`bench/tools/msvc.cmd cl /nologo /utf-8 /O2 d3dprobe.c d3d11.lib dxgi.lib dxguid.lib`; run `d3dprobe hw|warp|<adapter> [count]`) |
| `out/quick.json`, `out/baseline-ref.json` | final quick run of 4 variants × 3 documents + calibration |
| `out/knobs-medium.json` (+ the earlier `knobs-medium-1.json`), `knob-wvargs.json`, `knob-ilcpref.json`, `knob-dwwarm.json`, `knob-head-large*.json`, `knob-warm.json` | raw knob data |
| `out/shots/*.png` | screenshots |
| `tools/pixelprobe.py`, `out/audit-run.json`, `out/shots/audit-*.png` | audit tools and data (§13) |

## 13. Audit (independent review, 2026-09-19)

Reviewed by a separate auditor agent: sources, build, marks, screenshots, harness runs and a separate pixel-level
check of "when the document is actually on screen". Verdict: **the measurement is honest, all 4 variants are fit for the final
run**; one defect was fixed (`winui3-webview2` crashing on exit); it did not affect startup time.

### 13.1 What was checked

| Item | Result |
|---|---|
| `t_content` per PROTOCOL §3 | OK. Native XAML: root `Loaded` → first `CompositionTarget.Rendered` → `DwmFlush()` → write (`App.OnRootLoaded` / `OnFirstRendered` / `Presented`, `FastMdBench.ContentPresented`). WebView2: page `rAF(rAF(postMessage))` → `WebMessageReceived` → the same `Presented` → `DwmFlush()`. Nothing marks earlier (the `parsed`, `window_created`, `activate_ret` marks are breakdown only) |
| No resident / cache / pre-render | OK. A single process (5 processes in a Job for WebView2), nothing is started ahead of time, no file cache. The only shared asset embedded in the build is `bench/common/style.css` (the protocol requires it for web variants). Nothing from the corpus is embedded (grep over `src/`: neither document names nor contents). The WebView2 profile (`%LOCALAPPDATA%\FastMD\winui3-webview2`) persists across launches — it is a normal UDF; the document goes through `NavigateToString`, not from a cache |
| First viewport first and head-first parse | legitimate (§3 of the protocol allows it). The file is read in full (`File.ReadAllText`, including 3.7 MB), the first frame is a real render of the first 13–14 blocks; the head of large.md is cut at a blank line outside fenced code, the last head block is not used, and if there are reference definitions after the cut the screen is redrawn |
| Release / optimization | OK. `dotnet publish -c Release`; `obj/aot/.../fastmd-winui3.ilc.rsp` contains `--Ot` + 6 `--mibc` (i.e. `OptimizationPreference=Speed` takes effect); `r2r` has `obj/r2r/.../R2R`, and `Markdig.dll` 1.4 MB and `Microsoft.WinUI.dll` 16.7 MB are R2R images. The auditor rebuilt `build.ps1` completely from scratch: 5 configurations in 52 s, no errors |
| `bench.py check` medium / large, all 4 variants | all `CHECK OK`, no warnings (except the fixed exit code, see 13.2). `observed_ms` ≈ `window_ms` − 3 ms (the window is visible at the moment of `Activated`) and always < `content_ms` — no contradictions, but `t_content` cannot be verified this way either (see 13.3) |
| Window, DPI, title | client 1000×800 at 100 % (screenshot 1002×832 with the frame), PerMonitorV2 in the manifest, title `medium.md — FastMD (winui3)` |

### 13.2 Found and fixed: `winui3-webview2` crashed on exit (0xC000027B)

In bench mode, after writing the result, `Presented()` calls `Application.Exit()`, but the
`WebMessageReceived` handler (`WebHost.cs`) still queued sending the rest of the document via
`PostWebMessageAsString` into the already closing WebView2 on the `DispatcherQueue` → exception in the dispatcher callback → stowed exception;
the process exited with code **0xC000027B** (`CoreMessagingXP.dll`, Application log, event 1000) **and created
a WER report**. In the builder's `out/quick.json`, **12 of 18** `winui3-webview2` launches ended this way (all small/medium;
large did not, since there the rest is ready later). This did not affect `t_content` (the result was already written), but it violated PROTOCOL §4
("exit cleanly, exit code 0"), and `WerFault` runs outside the Job Object and could add noise to neighboring measurements in the final
sequential run.

The fix (minimal, after `t_content`): `App.Exiting` is set before `Exit()`; `WebHost` does not queue
sending the rest if the app is exiting, and wraps `PostWebMessageAsString` in `try/catch`. All
configurations were rebuilt (`build.ps1`). Verification: 4 `check`s in a row → exit 0; in `out/audit-run.json` all 60 launches
(5 variants × 2 documents × 6) have `exit_code 0`, `clean_exit true`.

### 13.3 Independent check: when the document is actually on screen (`tools/pixelprobe.py`)

The harness's `observed_ms` only sees the window appearing (WinUI shows the window at `Activate`, ~140 ms, and the document
~230 ms after that), so I wrote a separate probe: it launches the variant as the harness does (Job,
`FASTMD_BENCH_EXIT=0`), polls the **composited** screen (`BitBlt` from the screen DC = what DWM shows) over a strip
of the client area every ~7 ms; 1.2 s after the report a reference capture is taken, and the probe looks for the first capture that matches it
(≥85 % of the reference's "dark" text pixels are dark, ≥95 % of the white ones are white, a shift of ±8/±3 px is allowed). Appearance
interval: (start of the last non-matching capture; end of the first matching one]. Monitor 165 Hz (frame 6 ms).

| Variant / document | n | document NOT yet on screen at … after `t_content` | already on screen no later than … after `t_content` |
|---|---|---|---|
| `winui3` / medium | 8 | +5.6 … +15.7 ms | +24.8 … +37.4 ms |
| `winui3` / small | 3 | +5.9 … +7.5 | ≈ +27 (median) |
| `winui3` / large | 3 | +6.5 … +29.6 (median +7.0) | ≈ +33 (median) |
| `winui3-r2r` / medium | 3 | +7.9 … +13.4 | ≈ +33 (median) |
| `winui3-webview2` / medium | 3 | +0.8 … +7.4 | +19.7 … +25.2 |
| `winui3-fluent` / medium | 1 (timeline) | +19.4 (the document appears gray and "fades in" over ~80 ms — a DWM animation) | +37 (full brightness ≈ +100) |
| **reference from another stack:** `cpp-webview2` / medium | 2 | −7.6 / +19.9 | +12.8 / +45.7 |

Conclusion: for all compositor-based stacks (XAML, WebView2), photons appear 1–4 frames (≈ +6…+35 ms) after
`DwmFlush()`, and `winui3` does not stand out here among the stacks that follow the same §3 procedure (`cpp-webview2`,
`winui3-webview2`). The mark is **not earlier than the protocol requires and not systematically earlier than the others** — not changed (shifting
the mark only for WinUI would make the comparison unfair in the other direction). For `winui3-fluent` (Mica) the lag is ~15 ms
larger, i.e. its "Fluent cost" in the tables is still slightly understated. For `baseline-win32` (an empty window without a document)
the probe only measures the end of the DWM window-open animation (+42…+68 ms after its `t_content`) — a window manager delay
that is the same for everyone, not the app's. Run: `python bench/protos/winui3/tools/pixelprobe.py winui3 --doc
medium --runs 3 [--timeline] [--save-dir DIR]`.

### 13.4 Remarks (do not affect honesty)

- **Black window before the first frame.** The probe timeline: from the moment the window appears (~140 ms) until the document (~370 ms)
  the client area is **black** (0 % white pixels), then the document appears at once. For a "stylish" viewer this is a noticeable
  flash; `t_window` (the `Activated` event) is formally "something painted", but it is DWM's black background, not a white
  page. A product should not show the window before the first frame (cloak/`ShowWindow` after `Rendered`) or should set a background.
- **3 px horizontal layout shift** a frame or two after the first frame (the column re-centers, probably
  because of the scroll bar without Fluent styles). The probe sees it (`shift_px=[3,0]`); it is barely noticeable to the eye.
- The scroll bar in the native variants auto-hides (WinUI style), so it is not visible on still frames; scrolling was
  checked with `--scrollto` (780/1560/2400 DIP) — C#/Rust code, blockquotes, tables, image, h5/h6, hr are fine.
- `winui3-webview2` uses the system WebView2 Runtime 153 (not included in `dist_mb`); `winui3-r2r` uses the installed
  .NET 8 (not included either).

### 13.5 Auditor's control run (`out/audit-run.json`, `--runs 5`, interleaved, median of 5 launches excluding the first, noisy machine)

| Variant | small | medium | t_window | CPU, ms | WS, MB | processes | dist, MB |
|---|---|---|---|---|---|---|---|
| **`winui3`** | **339.6** | **364.1** | 144–151 | 375–422 | 96–99 | 1 | 58.08 |
| `winui3-r2r` | 393.8 | 414.5 | 220–238 | 484–547 | 127–130 | 1 | 100.77 |
| `winui3-fluent` | 480.0 | 503.4 | 380–385 | 438–516 | 109–110 | 1 | 59.37 |
| `winui3-webview2` | 574.9 | 569.2 | 149–160 | 1297–1313 | 95–97 | 5 | 58.08 |
| `baseline-win32` (calibration) | 78.1 | 75.6 | 70–73 | 47–63 | 14 | 1 | 0.15 |

Matches the builder's numbers (§6) within noise. `check` large: `winui3` 377/424 ms, `winui3-r2r` 455,
`winui3-fluent` 536, `winui3-webview2` 611/561 — large ≈ medium, as claimed.

### 13.6 Visual assessment (PROTOCOL §5; screenshots `out/shots/audit-*.png` + scrolling)

- **`winui3` / `winui3-r2r` / `winui3-fluent` (native XAML): 8/10.** Very close to GitHub: Segoe UI Variable
  15/24, h1–h6 headings with rules, links `#0969da`, tables with borders/zebra stripes/alignment, code blocks with background,
  radius and **syntax highlighting** (Rust, C#, Python, JS, JSON, PowerShell, C), blockquotes with a bar, task lists with
  neat checkboxes, color emoji, Cyrillic and CJK without "tofu", local PNGs. Downsides: inline code without
  rounded corners, long code lines wrap (no horizontal scrolling), selection only within a block,
  inline images are a placeholder, black flash before the first frame (not visible on a still frame).
- **`winui3-webview2`: 9/10 for layout** (exactly `style.css`: inline code with rounded corners, native checkboxes, selection
  across blocks), but **no syntax highlighting** and with the browser's CJK fallback.

### 13.7 What the auditor changed

| File | Change |
|---|---|
| `src/App.cs` | `Exiting` field, set before `Exit()` in `Presented` |
| `src/WebHost.cs` | do not send the rest of the document after `Exit()`; `try/catch` around `PostWebMessageAsString` |
| `tools/pixelprobe.py` | new: independent pixel-level check of when the document appears on screen |
| `out/*` | `build.ps1` rebuild; `audit-run.json`, `shots/audit-*.png` |

`proto.json` was not changed: all 4 variants are valid for the final run.

**Harness (not fixed, for the orchestrator):** (1) `observed_ms` only checks window visibility and, for WinUI (the window
is shown black ~230 ms before the document), cannot catch an early `t_content`; a pixel-level check like
`tools/pixelprobe.py` would be useful in the harness itself, with a caveat about the DWM window-open animation. (2) `run` does not
show non-zero `exit_code`s in the summary — crashes on exit (as in 13.2) are visible only in `check` or in the raw JSON.
(3) The builder's remark about `shot` hanging on a hung window (`PrintWindow` without a timeout) is plausible from the code;
not reproduced.
