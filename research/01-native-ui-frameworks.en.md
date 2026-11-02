[Русский](01-native-ui-frameworks.md) | **English**

# 01. Native Windows UI frameworks for a fast and stylish Markdown reader

> Research for FastMD. Date: 2026-09-19. Machine: Windows 11 Pro 26200, Ryzen 9 7950X, 64 GB, NVMe,
> NVIDIA RTX 5070 (driver 32.0.16.1656) + AMD iGPU, Defender real-time ON, non-admin session.
>
> Legend: **[measured]** — our own measurements in `research/lab/native-ui/` (method in §1),
> **[inspection]** — analysis of binaries and manifests on this machine, **[source]** — a publication (link alongside),
> **[estimate]** — a conclusion drawn from measurements and sources, not a direct measurement.
>
> Full prototypes (WinUI 3, WPF, Avalonia, cpp-d2d, RichEdit, etc.) are built by other agents in `bench/protos/*`.
> This report covers the anatomy of startup, hands-on checks of the "folklore", and a comparison of the stacks.

---

## 0. Summary

1. **On this machine the main tax is the GPU driver, not the framework.** [measured] `D3D11CreateDevice(HARDWARE)` on the RTX 5070
   takes **~185 ms in every new process**, and warm caches do not help. WARP (software D3D11) takes
   **3.5–4 ms**, the AMD iGPU 45–60 ms. Stack sampling shows that ~40 % of this time is spent by the NVIDIA loader
   `nvldumdx.dll` verifying the Authenticode signatures of its DLLs (`imagehlp`/`wintrust`/`crypt32`). 212 MB of images
   are mapped into the process (`nvgpucomp64.dll` 106 MB, `nvwgf2umx.dll` 86 MB). Independent confirmation
   (RTX 3090, 2026-09-17): 159 ms versus 9.5 ms on WARP, and there is no way to speed the call up.
   Consequence: **any stack that creates a hardware D3D device before the first frame** (WinUI 3, WPF, Avalonia/ANGLE,
   Uno/OpenGL, egui/wgpu, a custom D2D on hardware) **loses 180–250 ms here**.
2. **The Win32 floor without a GPU is about 90 ms** (the orchestrator's calibration). Text via GDI or D2D (software/WARP) plus
   deferred window activation gives 92–150 ms on a loaded machine [measured]. This is the only class of solutions
   that can reach the "bullet" level of 100–150 ms.
3. **Window activation (IME/TSF) costs ~25–60 ms** right on the critical path [measured]. If the window is shown without activation
   (`SW_SHOWNOACTIVATE`), the first frame is drawn, and only then `SetForegroundWindow` is called, the GDI variant
   speeds up from 119 to 92 ms and the WARP variant from 173 to 151 ms. The technique carries over to any framework
   (`AppWindow.Show(false)` in WinUI, `ShowActivated=false` in WPF and Avalonia). Caveat: this was measured under
   harness conditions, where the window does not get foreground; launching from Explorer has to be checked separately (§2.3).
4. **Color emoji cost ~30 ms on the first draw** in DirectWrite/D2D (Segoe UI Emoji COLR glyphs).
   Without color — 3 ms. CJK fallback is cheap: 1–2 ms [measured].
5. **Exe size directly slows down startup.** Every +55 MB of "dead" data at the end of the exe adds +130 ms to every
   start: Defender scans the exe at process creation and does not cache the result. For a DLL the scan is cached and paid
   only on the first load [measured]. WinForms NativeAOT on .NET 8 produces a 56.7 MB exe and starts slower than JIT.
6. **WinUI 3 / Windows App SDK is alive and getting faster.** There is a SemVer 2.x line: 2.0.1 of 29.04.2026, 2.5.1 of 16.09.2026.
   2.3.1 added opt-in XAML optimizations. WinUI itself is cheap: the bootstrapper 3–9 ms, loading `Microsoft.UI.Xaml.dll`
   6–15 ms, the `Application` factory 13–29 ms [measured]. What is expensive is something else: XAML resource initialization, the hardware D3D device, and
   the lack of "window shown ↔ first frame" synchronization (issue #11677, August 2026).
7. **The inbox Notepad 11.2607 [inspection]** is built like this: a Win32 host, WinUI 3 (Windows App SDK **2.x**) via
   XAML Islands for the "chrome" only, **its own copy of Office RichEdit** (D2D/DirectWrite, `msptls.dll` for tables)
   as the document surface, **cmark-gfm 0.29.0.gfm.13** statically linked into `Notepad.exe`. At Microsoft, Markdown
   is rendered by RichEdit, not by XAML.
8. **WPF (a minimal window with a TextBlock) — 630–670 ms [measured]**. `RenderMode.SoftwareOnly` does not help: the NVIDIA
   D3D9 driver is loaded anyway. There is no NativeAOT for WPF (dotnet/wpf#3811, milestone Future). WPF is not suitable
   for the "bullet". MAUI on Windows = WinUI 3 + a MAUI layer, and NativeAOT does not work there out of the box.
   WinForms — only as a host, with no gain over Win32.
9. **Recommendation.** The FastMD core: C/C++ Win32 + DirectWrite/Direct2D. The first frame is drawn on WARP or software D2D
   via a DComp flip swapchain, the window is activated after the first frame, the parser is md4c, one small exe. The hardware GPU,
   the WinUI "chrome" and WebView (if needed) are loaded **after** the first frame. Avalonia 12 is worth
   considering only if cross-platform support is a requirement, and it must be tested with `RenderingMode=Software`.

---

## 1. Method and caveats

- **The runner** `research/lab/native-ui/runner/labrun.py` imports the primitives of `bench/harness/bench.py` unchanged:
  CreateProcess with a job object, `t0` right before `CreateProcessW`, results in the PROTOCOL.md format.
  The apps do what the protocol requires: first frame → `DwmFlush()` → `t_content`. The runner's output
  lives only in `research/lab/native-ui/`.
- **Lab apps.** These are not markdown viewers but the "floor" of each stack: a 1000×800 DIP window and the first
  2.5 KB of `medium.md` as plain wrapped text (Segoe UI Variable Text, 15 px). This fragment includes
  Cyrillic, CJK and emoji.
- **Runs.** 11 interleaved launches per variant, preceded by one warm-up launch. The median and the minimum are shown.
  Warm start: files in the cache.
- **Noise.** During the measurements other agents' builds were running in parallel, CPU load 35–100 %. In the final run
  `baseline-win32` gave a median of **117 ms** against the calibration's **90 ms** on an idle machine. Absolute numbers are therefore inflated
  by roughly 25–30 ms. **Conclusions rely on the differences between variants within the same run.**
- **Cold start** (right after a reboot or after clearing the standby list) was not measured: clearing it requires administrator
  rights. Wherever it is mentioned, it is an **[estimate]**.
- **Machine confounders** [inspection]:
  - **Windhawk 1.7.3 injects 4 modules into every new process**, including processes launched from the python harness
    (checked with `w32/out/modlist.exe`): `windhawk.dll`, the `explorer-details-better-file-sizes_1.5` mod,
    `libc++.whl`, `libunwind.whl`. This cost sits inside the 90 ms "floor" for **all** prototypes.
  - NVIDIA App/ShadowPlay loads `nvspcap64.dll` into every D3D process.
  - MSI Afterburner is running.
  - `TextInputHost.exe` has accumulated an abnormally large amount of CPU time, which may inflate the cost of IME activation (§2.3).

---

## 2. Anatomy of "time to first content" on this machine [measured]

### 2.1 Summary table of the final run

Run `runner/final.json` → `runner/results/final.json`, 11 launches, `medium.md`, machine under 35–50 % load.
Marks are medians in ms from `t0`.

| Variant | What it does | Median | Min | Breakdown (medians, ms) |
|---|---|---:|---:|---|
| `baseline-win32` | the orchestrator's reference (GDI, one line) | **117** | 88 | main 44 |
| `gdi` | Win32 + GDI `DrawTextW` 2.5 KB | **119** | 102 | main 47 → window 67 → **ShowWindow 98** → drawn 110 |
| `gdi-noime` | + `ImmDisableIME(-1)` | **94** | 77 | window 59 → **ShowWindow 74** → drawn 87 |
| `gdi-lateact` | `SW_SHOWNOACTIVATE`, activation after the frame | **92** | 74 | window 64 → **ShowWindow 72** → drawn 86 |
| `d2d-hwnd-sw` | D2D HwndRenderTarget SOFTWARE + DirectWrite | **176** | 142 | shown 96 → RT 120 → layout 123 → drawn 168 |
| `warp-dcomp` | D3D11 **WARP** + DXGI flip + DComp + D2D/DWrite | **173** | 128 | shown 99 → device 119 → layout 126 → EndDraw 157 → present 164 |
| `warp-dcomp-lateact` | the same + deferred activation | **151** | 119 | shown 70 → device 92 → layout 100 → EndDraw 135 → present 142 |
| `d2d-hwnd-hw` | D2D HwndRenderTarget, default (NVIDIA) | **358** | 327 | shown 104 → **RT 306 (+202)** → drawn 349 |
| `d3d-dcomp-hw` | D3D11 HARDWARE (NVIDIA) + DComp + D2D | **459** | 369 | shown 107 → **device 362 (+254)** → EndDraw 433 → present 453 |
| `igpu-dcomp` | D3D11 on the AMD iGPU (MINIMUM_POWER) | **447** | 383 | device 171 (+50) → **swapchain 375 (+205)** → present 438 |
| `con-aot` | .NET 8 NativeAOT, `Main` only (no window) | **51** | 45 | main 51 |
| `con-jit` | .NET 8 JIT (framework-dependent), `Main` only | **70** | 61 | main 70 |
| `con-aot+55MB-overlay` | the same exe + a 55 MB "tail" in the file | **182** | 161 | main 182 |
| `wf-jit` | WinForms .NET 8, JIT, `TextRenderer` | **248** | 199 | main 89 → form 133 → painted 237 |
| `wf-sc-r2r` | WinForms self-contained ReadyToRun | **243** | 186 | main 96 → painted 237 |
| `wf-aot` | WinForms NativeAOT (56.7 MB exe) | **305** | 272 | **main 199** → painted 296 |
| `wpf-r2r-hw` | WPF R2R, `Window` + `TextBlock` | **672** | 516 | main 103 → window 232 → **ContentRendered 648** |
| `wpf-r2r-sw` | the same + `RenderMode.SoftwareOnly` | **632** | 524 | main 92 → window 222 → ContentRendered 611 |

What the table shows:

- **The process up to `main` costs ~45 ms**, ~70 ms with .NET JIT, ~51 ms with NativeAOT. This figure includes Defender and
  the Windhawk injection.
- **`ShowWindow` with activation costs ~31 ms**; without IME or with deferred activation, ~9–15 ms (§2.3).
- GDI text (2.5 KB, with font linking) is drawn in ~12 ms. DirectWrite layout of the same text takes 2–4 ms, but the
  first draw with color emoji takes ~30–45 ms (§2.4).
- **A hardware D3D device adds 200–250 ms** in any combination (HwndRenderTarget, DComp, flip).
  The iGPU variant does not help: the device is created quickly (50 ms), but the swapchain for a monitor connected to the NVIDIA card
  costs 205 ms. **Hypothesis:** a cross-adapter presentation is required, and the NVIDIA driver is loaded anyway.

### 2.2 The GPU driver is the main tax [measured]

`w32/d3dprobe.cpp`: a console process without a window, each measurement in a new process (`results/d3dprobe.txt`).

| What | Time | Comment |
|---|---:|---|
| `CreateDXGIFactory2` | 11–20 ms | already expensive on its own: dxcore, directxdatabasehelper |
| `D3D11CreateDevice(HARDWARE)`, NVIDIA RTX 5070 | **181–237 ms, median ~187** | 9 processes; the same on the first and the tenth launch |
| the same, a second device in the same process | 23–33 ms | even with the driver already loaded |
| `D3D11CreateDevice(WARP)` | **3.4–4.2 ms** | the D3D11 software rasterizer (SIMD, multithreaded) |
| `D3D11CreateDevice` on the AMD iGPU | 44–60 ms | |
| `D3D12CreateDevice` (NVIDIA) | 201–285 ms | |

After the NVIDIA device is created, **30 new modules with a total size of 211.8 MB** appear in the process:
`nvgpucomp64.dll` 105.7 MB, `nvwgf2umx.dll` 86.0 MB, `nvppex.dll` 7.2 MB, `NvMemMapStoragex.dll`, `nvldumdx.dll`,
`nvspcap64.dll`. Besides them — **`wintrust.dll`, `CRYPT32.dll`, `imagehlp.dll`, `wldp.dll`, `drvstore.dll`**.

**Where the time goes.** `w32/stackprobe.cpp` is a home-made module-level sampling profiler: it
suspends the thread every 0.25 ms, copies the stack and attributes it to modules (`results/stackprobe.txt`,
two runs of 847 and 955 samples):

| Module | Exclusive (executing) | Inclusive (on the stack) |
|---|---:|---:|
| `imagehlp.dll` (Authenticode PE hash, `ImageGetDigestStream`) | **39–40 %** | 40 % |
| `ntdll.dll` (loader, page faults) | 37–38 % | 98 % |
| `win32u.dll` (D3DKMT syscalls) | 11–12 % | 14 % |
| code of the `nv*.dll` themselves | ~7 % | — |
| `nvldumdx.dll` (NVIDIA UMD loader) | 0.1 % | **65–66 %** |
| `CRYPT32.dll` / `wintrust.dll` | — | **47 % / 40–41 %** |

Interpretation [estimate based on measurement]: roughly **40 % of device creation is verification of the digital signatures of
~200 MB of driver DLLs**, which `nvldumdx.dll` performs via WinVerifyTrust in every new process. Another
~40 % is mapping and binding the images themselves. Nothing on the application side can fix this.

**Independent confirmation** [source]: the nene-nib project (a C++ editor, D3D11/D2D/DComp), measured on 2026-09-16/17
on an RTX 3090 with driver 32.0.16.1088. `device_created` = **159 ms out of a 184 ms total startup**, 9.5 ms on WARP.
Limiting feature levels, an explicit adapter, `SINGLETHREADED` and a repeated launch did not help (all deviations within
±6 ms). The project decided to show the window before creating the device:
<https://github.com/hideyukiMORI/nene-nib/issues/24>,
`docs/quality/speed-reference.md` in the same repository.

**Parallelizing did not help** [measured]. The `d3d-dcomp-bgdev` variant created the device on a background thread starting from
the first line of `main`, and became slower: the window was created at 132 ms instead of 92, `ShowWindow` took 193 ms instead of 138.
Loading the driver holds the loader lock and slows down DLL loading on the main thread (uxtheme, IME, etc.).

**What this means for the stacks** [estimate]:

| Stack | How it renders by default | NVIDIA tax on the path to the first frame |
|---|---|---|
| WinUI 3 / UWP XAML | D3D11 + DComp (lifted compositor in-process: `dcompi.dll`, `dwmcorei.dll`) | yes, there is no public software mode |
| WPF | D3D9Ex (`nvd3dumx.dll` is loaded even with `SoftwareOnly`, see §5) | yes |
| Avalonia | `RenderingMode = [AngleEgl, Software]` → ANGLE → D3D11 | yes by default, **no with `Software`** |
| Uno Skia (Win32) | Skia + OpenGL (default), Vulkan optional | yes (NVIDIA OpenGL ICD) |
| egui / iced (wgpu) | D3D12 / Vulkan | yes, 200–285 ms for the device alone |
| Chromium / WebView2 / Electron | a separate GPU process | in parallel, but the compositor's first frame waits for it [hypothesis] |
| Custom D2D on HARDWARE | D3D11 | yes |
| Custom D2D on WARP / software, GDI | CPU | **no** |

### 2.3 Window activation, IME and TSF [measured]

In the `gdi` variant the `ShowWindow(SW_SHOWNORMAL)` call takes ~31 ms at the median (42–62 ms in noisier runs).
With `ImmDisableIME(-1)` — 15 ms. With `SW_SHOWNOACTIVATE` and `SetForegroundWindow` after the first frame — **9 ms**.
`ImmAssociateContextEx(hwnd, NULL, 0)` does not help (run `runner/w32.json`). So what is expensive is not the window's IME context
but the activation of the thread/window in TSF: WM_ACTIVATE → WM_SETFOCUS → Text Services Framework initialization and the link with
`TextInputHost`.

The **"first frame before activation"** technique: the window is shown inactive, drawn, `DwmFlush`, then
`SetForegroundWindow`. A process launched by a double click from Explorer is entitled to foreground. Savings on this
machine: **−27 ms (GDI) and −23 ms (WARP)** at the median. IME in the search box keeps working, unlike
with `ImmDisableIME`. Equivalents in frameworks: WinUI — `AppWindow.Show(activateWindow: false)` followed by
`Activate()`, WPF — `Window.ShowActivated = false`, Avalonia — `Window.ShowActivated = false`.
Whether it works in each framework has to be checked in the prototypes.

**An important caveat** [measured, `runner/fgcheck.py`, `results/fgcheck.txt`]. A window launched from a background
python process **does not become foreground in any variant**: 400 ms after the frame `GetForegroundWindow`
points to another process. The foreground lock kicks in: the harness has no foreground rights to pass on to the child
process. So "activation" in the harness is activation within its own input queue without an actual focus
switch. On a double click from Explorer the process gets foreground rights, and the cost of activation with TSF/focus may be
**different, probably no smaller**. The −25 ms effect has to be rechecked by launching from Explorer.

> For the orchestrator: PROTOCOL.md §5 requires "shown activated". Activation ~1 frame after the content
> should be explicitly allowed or forbidden for all prototypes alike.

### 2.4 Fonts: fallback and color emoji [measured]

`w32/dwprobe.cpp`: DirectWrite layout and drawing via D2D into a WIC bitmap (software, no window and no GPU),
the first 2.5 KB of `medium.md`, 5 new processes per variant (`results/dwprobe.txt`):

| Variant | Layout (`GetMetrics`) | **First draw** | Second draw |
|---|---:|---:|---:|
| Full text (Cyrillic + CJK + emoji), `ENABLE_COLOR_FONT` | 1.7–2.4 ms | **29.9–41.4 ms** | 3.1–5.2 ms |
| The same text, without color fonts | 1.9–2.6 ms | 2.6–4.1 ms | 0.4–1.0 ms |
| Without CJK and emoji ("Latin + Cyrillic") | 1.3–1.7 ms | 2.1–2.9 ms | 0.6–0.9 ms |

Conclusions:

- Fallback to CJK fonts in DirectWrite is almost free: the system font cache and the FontCache service do their job.
- **Segoe UI Emoji (COLR) color emoji cost ~30 ms on the first draw** and 3–5 ms on each subsequent one.
  In the first screen of `medium.md` there is an emoji as early as line 3.
- Possible measures: cache emoji rasters in an atlas; on the first frame draw emoji in monochrome and redraw them
  in color on the next frame (the technique is allowed, but it has to be mentioned in the README); rasterize emoji in the background.
- GDI (`DrawTextW`) does not draw color emoji at all: only monochrome outlines via font linking. So GDI
  is cheap, but also ugly.
- Setting up D2D itself (factory + WIC factory + render target) takes 20–29 ms. A noticeable part of that is the WIC
  `CoCreateInstance`. WIC is not needed for the first frame; images can be decoded later.

### 2.5 Exe size and Defender [measured]

| Experiment | Median to `main` |
|---|---:|
| `conlab.exe` NativeAOT, 1.3 MB | 51 ms |
| the same exe + 55 MB of random bytes at the end of the file (not mapped into memory) | **182 ms (+131)** |
| the same exe + 55 MB of zeros | 173 ms (+122) |
| WinForms NativeAOT, 56.7 MB | 199 ms |

Every launch costs the same: 11 out of 11 runs, plus 10 consecutive launches give 164–181 ms versus 42–44 ms.
The job's CPU time does not grow, i.e. the work happens outside the process. The most likely culprit is Defender's
real-time scan of the exe at process creation. Smart App Control is off (`VerifiedAndReputablePolicyState=0`).

A DLL behaves differently: the same +55 MB "payload" costs 35–55 ms **only on the first load**, then 0.4–0.7 ms
(`results/dllload.txt`).

Practical consequences:

- **Keep the exe small**, move rarely needed parts into DLLs loaded later.
- Do not make single-file bundles of tens of megabytes.
- The harness's `cpu_ms` figure does not see this cost, but `content_ms` does.
- **Open question:** does code signing (Authenticode with reputation) remove the rescan of the exe on every launch?
  There is nothing to test it with: no certificate.

### 2.6 The "bullet" budget

With a 90 ms floor [source: calibration `bench/results/calib-baseline.json`], for a native Win32 reader on this machine
[estimate]:

- CreateProcess → `main`: ~35–45 ms (Defender, Windhawk);
- window without activation: +15–20 ms;
- reading the file and parsing the first screen with md4c: 1–3 ms;
- D2D on WARP (device 4 ms, DComp/swapchain 3–5 ms) and DirectWrite layout of the visible blocks: 3–10 ms;
- first draw: 5–15 ms, plus ~30 ms if the first screen contains color emoji;
- `Present` + `DwmFlush`: ≤ 1 frame.

**Total on an idle machine ~110–150 ms**, i.e. the floor + 20–60 ms. Any stack with a hardware GPU on the critical path cannot
be faster than ~300 ms on this machine. A stack with a managed runtime and XAML — ~500+ ms.

---

## 3. WinUI 3 / Windows App SDK

### 3.1 State as of September 2026 [source + inspection]

- **Versions.** The 1.8 branch is serviced (Foundation 1.8.260803002 — August 2026). Since 2.0 the project has switched to
  SemVer: 2.0.1 (29.04.2026), 2.1.3 (21.05), 2.2.0 (09.06), 2.3.1 (16.07), 2.4.0 (13.08), 2.5.1 (16.09.2026).
  The package family name now contains only the major version: `Microsoft.WindowsAppRuntime.2`.
  Source: <https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0>.
- **Performance in 2.3.1.** Opt-in XAML optimizations arrived via `XamlOptionalChanges` / `XamlChangeId`:
  `DefaultStyleOptimizations`, `OptimizeApplyStyles`, `Xaml_FasterResourceDictionary`,
  `Xaml_ActivationFactoryFastPaths`, `Xaml_FasterXbfIntLoading` and others. The dependency on `version.dll` was removed from the startup path.
  Contention for the D3D lock during background device creation was reduced.
  This also shows that **WinUI creates the D3D device in the background**, but the first frame still waits for it.
- **Microsoft's direction.** Windows Insider Blog, 2026-03-20: "moving core Windows experiences to WinUI3".
  Discussion microsoft-ui-xaml#11096 (2026-05-11): File Explorer and Notepad were chosen as the launch time benchmarks.
  For the WinUI part of Explorer's launch they report −41 % allocations, −45 % function calls, **−25 % time in WinUI code**.
  How much this gives the full launch is not stated: people ask about it directly in the comments.
  Links: <https://github.com/microsoft/microsoft-ui-xaml/discussions/11096>,
  <https://blogs.windows.com/windows-insider/2026/03/20/our-commitment-to-windows-quality/>.
- **Installed on this machine** [inspection]: runtimes 1.4–1.8, **2.2–2.5.1**, plus
  `Microsoft.WindowsAppRuntime.CBS.1.8`. The CBS package is the inbox copy that the WinUI part of File Explorer runs on.
- **Who is already on 2.x** [inspection]:
  - Notepad 11.2607.14.0 depends on `Microsoft.WindowsAppRuntime.2 ≥ 2.1.3`.
  - PowerToys 0.100.2: Windows App SDK 2.0.1, .NET 10.0.9 self-contained, `Microsoft.ui.xaml.dll` 3.2.0.

### 3.2 Startup anatomy of an unpackaged app

| Stage | Cost | Basis |
|---|---|---|
| CreateProcess → `main` | ~35–45 ms (C++), ~70 ms (C# JIT), ~50 ms (C# NativeAOT) | [measured] §2.1 |
| `MddBootstrapInitialize2` (Dynamic Dependencies, adding the framework package to the package graph) | **3–9 ms** typically (1.8: 3.5–5 ms; 2.x: 3.1–4 ms at a quiet moment; up to 20–60 ms under load). There is no DDLM child process on 26200 | [measured] `wasdk/bootprobe.cpp`, `results/bootprobe.txt` |
| `LoadLibrary("Microsoft.UI.Xaml.dll")` (14.6 MB) | **6–15 ms** | [measured] |
| `RoGetActivationFactory("Microsoft.UI.Xaml.Application")` | **13–29 ms** | [measured] |
| `Application.Start`, parsing `XamlControlsResources` (generic/themeresources XBF), styles | tens of ms, depends on the number of elements; Microsoft: "rough benchmark ~1 ms per element" | [source] Learn: <https://learn.microsoft.com/en-us/windows/apps/develop/performance/app-startup-performance> (ms.date 2026-03-16) |
| Creating the window HWND with a GDI redirection surface (an extra ~7.9 MB at 1080p in DWM) | little in time, but the window appears empty | [source] microsoft-ui-xaml#11523 (2026-08-13) |
| **D3D11 device** for composition and rendering | **~185 ms on this machine (NVIDIA)**, in the background, but the first frame waits for it | [measured] §2.2; [source] release notes 2.3.1 |
| Layout, render, first frame; **the window is shown before the frame** — a flash and an empty frame | +1–3 frames | [source] microsoft-ui-xaml#11677 (2026-08-27): "Every WinUI 3 app flickers on launch today" |

**[Estimate] for this machine (warm start, a simple page with a RichTextBlock):**

| Variant | Estimate |
|---|---|
| C++/WinRT | ~350–600 ms |
| C# NativeAOT | ~400–700 ms |
| C# JIT (framework-dependent) | ~600–1200 ms |
| Cold start | +100–400 ms: ~50 MB of the 2.x runtime's "UI core" DLLs, plus .NET if present |

The exact numbers will come from the `bench/protos/winui3` prototype (C# .NET 8, WinAppSDK 1.8, unpackaged, NativeAOT).

**Packaged (MSIX) or unpackaged.**

- Packaged: no bootstrapper needed, the OS resolves the framework package, there is an identity (share target, notifications,
  file association via the manifest). Activation goes through AppX activation (COM/broker).
- Unpackaged: the bootstrapper costs only 3–9 ms [measured].
- We could not find any published numbers on MSIX activation overhead. It cannot be measured here: Developer Mode
  is off and there are no administrator rights. **Open question.**

**Self-contained or framework-dependent.**

- Framework-dependent requires the runtime of the right major/minor version to be installed (an installer or a Store dependency).
- Self-contained puts the DLLs next to the exe. The 2.5.1 runtime package weighs **115.8 MB / 301 files** [inspection], of which
  the "UI core" is ~47.5 MB in 24 DLLs (`Microsoft.ui.xaml.dll` 14.6 MB, `Microsoft.UI.Xaml.Controls.dll` 6.6 MB,
  `dcompi.dll`, `dwmcorei.dll`, `DWriteCore.dll` 3.1 MB, `WinUIEdit.dll` 3.3 MB…), plus AI/ML (onnxruntime 20.7 MB,
  DirectML 17.8 MB), which can be excluded.
- Defender scans a DLL once and caches the result afterwards (§2.5), so on a warm start the difference is small.

**NativeAOT.**

- C# (CsWinRT 2.x): official since WinAppSDK 1.6. Microsoft reports a "50% reduction in start time" for the
  Contoso Camera sample and a ~8× smaller package when framework-dependent:
  <https://blogs.windows.com/windowsdeveloper/2024/09/04/whats-new-in-windows-app-sdk-1-6/>.
- CsWinRT 3.0 — rewritten for .NET 10 and AOT. Still in preview (`3.0.0-preview.260319.2`, March 2026, requires .NET ≥ 10.0.5).
- C++/WinRT carries no managed runtime at all, but development in it is noticeably slower.

### 3.3 Markdown in WinUI

- **`RichTextBlock`** supports: `Paragraph`, `Run`, `Bold`, `Italic`, `Underline`, `Span`, `Hyperlink`, `LineBreak`,
  `InlineUIContainer`, `TextDecorations` (including strikethrough), `TextHighlighters`,
  `IsTextSelectionEnabled` (selection across the whole block), `RichTextBlockOverflow`.
  Limitations:
  - **No tables** and no block background: code blocks and tables are built from a `Border`/`Grid` inside an `InlineUIContainer`, and such
    content **does not take part in text selection and copying**.
  - **No virtualization.** The 3.7 MB `large.md` in a single RichTextBlock means seconds of layout. Splitting into
    blocks in an `ItemsRepeater` breaks selection across block boundaries.
- **CommunityToolkit Labs `MarkdownTextBlock`** [source + inspection]:
  - The latest build is `0.1.251217-build.2433` (December 2025), there is no stable release. The "Road to 8.3 stable" tracker #643
    has been open since February 2025.
  - Markdig inside. Everything is rendered into **a single `RichTextBlock`**, tables via `MyTableUIElement` in an
    `InlineUIContainer` (sources in `components/MarkdownTextBlock/src/TextElements`).
  - Open problems: memory leaks (#759, #772), a crash under AOT (#780, closed 2026-08-15), nothing about syntax
    highlighting "out of the box".
  - Used in PowerToys 0.100 (Settings, QuickAccess, KeyboardManagerEditor, PowerDisplay), but not for
    file previews.
  - Link: <https://github.com/CommunityToolkit/Labs-Windows/issues/606>.
- **WebView2 inside WinUI.** Maximum fidelity (HTML/CSS), but it brings the whole cost of WebView2 along:
  see the `02-web-and-html-engines.md` report.

### 3.4 Style

WinUI's strong side:

- Fluent controls, Mica/Acrylic (2.0 added `SystemBackdropElement` with `CornerRadius`), a custom title bar
  (`TitleBar` with smart drag regions in 2.1.3), dark and light themes "for free".
- Typography: DirectWrite / DWriteCore, Segoe UI Variable, color fonts, OpenType `Typography` properties.

For a reader all of this is needed only in the "chrome" — search, settings, menus. The document itself is text.

### 3.5 WinUI 3 risks

1. Hardware D3D is always on the critical path: there is no public software/WARP switch. `Microsoft.UI.Xaml.dll`
   contains the strings `ForceWarp`/`SoftwareRendering`, but these are internal names, there is no API.
2. An empty window before the first frame (#11677) and the GDI redirection surface (#11523): both are in proposal status, not
   fixed.
3. Toolchain: the XAML compiler, CsWinRT, AOT warnings, slow builds. These are typical complaints in #11096.
4. Complaints about lag when resizing and dragging the window (#11096) are not critical for a reader.
5. Performance is improving, but mainly for large apps (Explorer): for the hello-world class the gain
   is limited by the D3D and runtime tax.

---

## 4. UWP / WinUI 2 and how the inbox apps are built

- **UWP** means CoreWindow and the system XAML (`Windows.UI.Xaml` in the OS). UWP had a real system splash screen,
  prelaunch and **deferred window display until the first frame** (`NotifyFirstFrameDrawn`, see #11677). WinUI 3
  does not have these mechanisms yet.
- **UWP on modern .NET with NativeAOT is GA** and the default template in VS 2026 (document dated 2026-01-26). According to
  Microsoft, startup shows a "~5% improvement" over .NET Native, and the package is about 4 MB larger:
  <https://learn.microsoft.com/en-us/windows/uwp/dotnet-native/modernize-uwp-apps-with-dotnet>.
- UWP is not suitable for a new desktop reader: the AppContainer sandbox makes access to arbitrary files and
  relative images difficult, and the strategic direction is WinAppSDK.

How the inbox apps are built:

| App | Stack | Basis |
|---|---|---|
| **Notepad 11.2607.14.0** | Win32 host `Notepad.exe` (3.3 MB, imports `Microsoft.UI.Windowing.Core.dll`). UI chrome — **WinUI 3 via XAML Islands** (`NotepadXamlUI.dll` 6.4 MB, C++/WinRT, `DesktopWindowXamlSource`, `XamlIslandsManager`, 70+ `.xbf`). **The document is its own copy of Office RichEdit** `riched20.dll` 4.0 MB (imports `d2d1`, `DWrite`, `msptls.dll` = Page/Table Layout Services, `MSO*.dll`), a windowless host (`RichEditHostMessageFilter`, `RichEditOleCallback`…). **Markdown parser: cmark-gfm 0.29.0.gfm.13**, statically in `Notepad.exe`. The package depends on `Microsoft.WindowsAppRuntime.2 ≥ 2.1.3` | [inspection] dumpbin + strings + AppxManifest |
| Notepad: markdown history | 11.2504.50.0 (May 2025): bold/italic/links/lists/headings; 11.2510: tables; 11.2512.10.0 (Jan 2026): strikethrough, nested lists; then images | [source] <https://blogs.windows.com/windows-insider/2025/05/30/text-formatting-in-notepad-begin-rolling-out-to-windows-insiders/>, <https://blogs.windows.com/windows-insider/2026/01/21/notepad-and-paint-updates-begin-rolling-out-to-windows-insiders/> |
| Calculator | classic UWP + **.NET Native** (`UseDotNetNativeToolchain`) + WinUI 2.8.7, commits in August 2026 | [source] github.com/microsoft/calculator `src/Calculator/Calculator.csproj` |
| Windows Terminal | WinUI 2.8.4 in XAML Islands. Text — **AtlasEngine**: DirectWrite/D2D only rasterize glyphs, placement and blending are D3D11 + HLSL, a glyph atlas | [source] <https://github.com/microsoft/terminal/pull/11623> |
| File Explorer | WinUI 3 via the inbox `WindowsAppRuntime.CBS` package (command bar, Home, address bar), the rest is classic Win32/DirectUI | [inspection] CBS package; [source] #11096 |

**Notepad's lesson for FastMD.** For a "fast" inbox editor with markdown, Microsoft does not render the document with XAML.
The document is RichEdit (DirectWrite/D2D, selection, copying, tables, incremental layout), and XAML
is responsible only for the chrome. This is a direct argument for "a native document core + optional XAML chrome".

---

## 5. WPF (.NET 8/9/10)

- **Measurement** (`dotnet/wpf`, window + `ScrollViewer` + `TextBlock` 2.5 KB, no XAML files, .NET 8.0.30):

  | Variant | Median | Breakdown |
  |---|---:|---|
  | R2R | **672 ms** | main 103 → `Application` 147 → `Window` 232 → `ContentRendered` 648 (+415 ms) |
  | R2R + `RenderOptions.ProcessRenderMode = SoftwareOnly` | 632 ms | the NVIDIA DLLs are loaded anyway (§below) |
  | JIT (noisier run `runner/wpf.json`) | 822 ms | with `SoftwareOnly` — 907 ms |

  Modules in the process with `SoftwareOnly`: `d3d9.dll`, `nvd3dumx.dll`, `nvgpucomp64.dll`, `nvldumdx.dll`…,
  `RenderCapability.Tier = 2`. WPF creates a D3D9 device to detect capabilities even in software mode,
  so the software mode does not remove the NVIDIA tax.
- Draft runs of the `bench/protos/wpf` prototype with FlowDocument and markdown (not final, machine under load):
  700–1270 ms.
- **No NativeAOT.** dotnet/wpf#3811 "WPF is not trim-compatible" has been open since 2020, milestone **Future**,
  last activity in November 2025. Partial trimming is the community's workaround.
  Link: <https://github.com/dotnet/wpf/issues/3811>.
- **ReadyToRun** is available. On this machine R2R gave a small gain: what dominates is not JIT but the initialization of
  WPF/milcore/D3D9.
- **Content.** `FlowDocument` is the richest native document model in .NET: `Table`, `List`, `Section`,
  `BlockUIContainer`, `Figure`/`Floater`, selection and copying across the whole document, printing.
  Downsides: `FlowDocumentScrollViewer` lays out the whole document (heavy for 3.7 MB), tables have
  quirks (dotnet/wpf#9166).
  Renderers: Markdig.Wpf (pipe/grid tables are partially supported), MdXaml, Markdig.FlowDocument.
- **Markdown Monster** (Rick Strahl) — a WPF shell + **WebView2** for the preview, one WebView2 per document.
  The author has written a lot about the problems of asynchronous initialization and flicker:
  <https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization>.
- **Style.** .NET 9 added a Fluent theme (Windows 11). There is WPF UI (lepoco). Mica is available via
  `DwmSetWindowAttribute`. Typography is good (OpenType via `Typography.*`), but WPF has its own text rendering stack.
- **Verdict:** good as an "IDE-like" editor, but not suitable for the "bullet": ~0.6–1 s on this machine and no AOT.

---

## 6. WinForms

- **Measurement** (`dotnet/wf`, `Form` + `TextRenderer.DrawText`):

  | Variant | Median | Comment |
  |---|---:|---|
  | JIT | 248 ms | |
  | self-contained R2R | 243 ms | |
  | **NativeAOT** (`_SuppressWinFormsTrimError`) | **305 ms** | exe **56.7 MB**: trimming does not work, the WPF natives `wpfgfx_cor3`, `PresentationNative_cor3`, `D3DCompiler_47_cor3` end up in the build. `main` at 199 ms because of the exe scan (§2.5) |
  | For comparison: C + GDI with the same text | 119 ms | |

- **NativeAOT status.** Officially unsupported in .NET 8/9/10: WinForms relies on built-in COM
  (dotnet/winforms#4649, dotnet/sdk#34129). There is the third-party WinFormsComInterop.
- **Content.** `RichTextBox` (RichEdit via msftedit), WebView2 or custom GDI+ drawing.
- **Verdict:** no gain over bare Win32. Appropriate only as a quick host for RichEdit or WebView2 if
  the team writes in C#.

---

## 7. .NET MAUI

- On Windows MAUI is WinUI 3 + a layer of MAUI handlers, so by definition it does not start faster than WinUI C#.
- **NativeAOT on Windows does not work out of the box.** dotnet/maui#31227 is open in the Backlog: "NativeAOT only worked with
  packaged apps", manual tweaks are required. Link: <https://github.com/dotnet/maui/issues/31227>.
- Versions: 10.0.101 stable, 11.0.0-rc.1 [NuGet].
- For comparison [source]: Uno.Chefs on Windows is the slowest of all platforms (1.605 s), see §9.
- **Verdict:** do not consider. Cross-platform support is needed by mobile apps, not by a Windows md reader.

---

## 8. Avalonia 11/12

- **Versions.** Avalonia 12.0 was released on 2026-04-07: .NET 10, SkiaSharp 3, the Direct2D backend removed, Skia only.
  Currently 12.1.2.
  Links: <https://avaloniaui.net/blog/avalonia-12>,
  <https://docs.avaloniaui.net/docs/avalonia12-breaking-changes>.
- **NativeAOT is supported**: `IsAotCompatible`, compiled bindings; the `BuiltInComInteropSupport=false` flag is needed
  only before 12.0.
  Published numbers exist only for Android: **1960 → 460 ms** with NativeAOT. No official numbers
  were found for Windows desktop.
- **Rendering on Windows.** By default `Win32PlatformOptions.RenderingMode = [AngleEgl, Software]` and
  `CompositionMode = [WinUIComposition, DirectComposition, RedirectionSurface]`
  (<https://api-docs.avaloniaui.net/docs/P_Avalonia_Win32PlatformOptions_RenderingMode>).
  ANGLE runs on top of D3D11, so **by default Avalonia pays the NVIDIA tax** (§2.2).
  **Hypothesis for the `avalonia` prototype:** `RenderingMode = [Software]` (Skia CPU raster) +
  `CompositionMode = [RedirectionSurface]` or DirectComposition should remove the 180–250 ms. The price is CPU load
  when scrolling large windows.
- **Text.** HarfBuzz + Skia, not DirectWrite. No ClearType, its own fallback. Text looks "almost native",
  but not identical to Windows. Compare the harness screenshots.
- **Markdown.**
  - Markdown.Avalonia 11.0.3 (May 2026); the 12.0.0-a3 branch is an alpha from April 2026. It builds `Grid`/`TextBlock`/`Border`,
    highlighting via AvaloniaEdit (heavy), selection is limited to individual `SelectableTextBlock`s.
  - LiveMarkdown.Avalonia targets LLM streaming.
  - There is a commercial Markdown component in Avalonia Accelerate.
- **Size** [estimate]: NativeAOT exe + `libSkiaSharp` + `libHarfBuzzSharp` + ANGLE `av_libglesv2` — tens of MB.
  There are static linking projects (avalonia-static). A large exe is expensive on every launch (§2.5).
- **Cross-platform:** real (Windows/macOS/Linux) — **the main argument for Avalonia**.
- **Verdict:** the best of the .NET options if macOS/Linux is needed. For a Windows-only "bullet" it loses to native on
  the runtime (~50–70 ms to `main`), on Skia initialization and on the GPU.

---

## 9. Uno Platform

- 6.7.30 stable, 7.0 in development.
- Since Uno 6.0 there is its own **Win32 host with Skia** on Windows, with no dependency on WPF. Self-contained became smaller
  by ~100 MB.
- NativeAOT for all desktop targets since 6.6 (2026-07-29). Published numbers for the full
  Uno.Chefs app (.NET 10), no minimal sample [source]:

  | Platform | Default runtime | NativeAOT |
  |---|---:|---:|
  | Windows | 1.605 s | **0.824 s** (−49 %) |
  | Linux | 0.870 s | 0.350 s |
  | Android | 0.895 s | 0.348 s |

  Source: <https://platform.uno/blog/uno-platform-6-6/>.
- Rendering on Windows: Skia + OpenGL by default, Vulkan optional. So the GPU driver is on the critical path.
- Upside: the API is WinUI-compatible, code can be shared with a WinUI chrome.
- **Verdict:** a heavy stack, overkill for a reader.

---

## 10. Bare Win32 + DirectWrite / Direct2D (+ DirectComposition)

### 10.1 What the platform provides

- **DirectWrite.**
  - `IDWriteTextLayout`: per-range formatting (font, weight, style, size, underline,
    strikethrough, drawing effects for color).
  - `IDWriteInlineObject`: images, inline code "pills", task list checkboxes.
  - Hit-testing (`HitTestPoint`, `HitTestTextRange`) for selection.
  - System fallback, color fonts, OpenType features, variable axes (`DWRITE_FONT_AXIS_VALUE`).
  - The font cache is system-wide: laying out 2 KB of text takes 1–3 ms [measured].
- **Direct2D.** Rounded rectangles (code block radius 6, inline code radius 4), table lines, bitmaps via
  WIC. Modes: HwndRenderTarget (hardware or software) or a D2D device context on top of a DXGI swapchain.
- **DirectComposition + flip-model swapchain.** No redirection bitmap (`WS_EX_NOREDIRECTIONBITMAP`), visual
  layers for smooth scrolling.
- **Windows 11 style** without a framework: `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)` for Mica,
  `DWMWA_USE_IMMERSIVE_DARK_MODE`, `DWMWA_WINDOW_CORNER_PREFERENCE`. A custom title bar — via
  `DwmExtendFrameIntoClientArea` and `WM_NCHITTEST`.

### 10.2 What "instant" apps do

| App | Stack | What to take for FastMD |
|---|---|---|
| **Windows Terminal (AtlasEngine)** | DirectWrite/D2D only rasterize glyphs, D3D11 + HLSL place them from an atlas | a glyph atlas for scrolling; but Terminal has XAML Islands and hardware D3D, so it is not "instant" |
| **Notepad++** | Win32 + Scintilla; rendering via GDI or DirectWrite (`SCI_SETTECHNOLOGY`, an option in the settings) | a small exe, no D3D device at startup |
| **Sublime Text** | C++, its own UI toolkit (skyline/px), Skia rasterization on the CPU. OpenGL acceleration appeared in 2021 and **was off by default on Windows** (as of the 2021 blog post); the authors ran into "~8 separate driver bugs" | <https://www.sublimetext.com/blog/articles/hardware-accelerated-rendering>, <https://thume.ca/2016/12/03/disassembling-sublime-text/>. CPU raster by default, GPU as an option |
| **File Pilot** | C, **its own OpenGL renderer + IMGUI**, exe ~1.8 MB. In the beta, text was Latin and Cyrillic only, no CJK | <https://news.ycombinator.com/item?id=43091466>. Its instantness is partly due to the absence of complex Unicode |
| **RemedyBG** | Dear ImGui + D3D11, shaders baked into the exe | <https://remedybg.handmade.network/> |
| **10x Editor** | its own GPU renderer (DX11) | <https://10xeditor.com/> |
| **nene-nib** (2026) | D3D11 + D2D + DComp + Mica, its own editor | measured the 159 ms NVIDIA tax and decided to **show the window before creating the device** |

The common pattern: one small exe, no runtime, its own renderer and its own text layout. The GPU is either not
used at startup (Sublime, Notepad++), or its cost is accepted or hidden behind showing the window early.
**None of them is a ready-made "native markdown with GFM tables and selection"** — that is FastMD's job.

### 10.3 What we will have to write ourselves (effort: high)

- **Parser.** md4c (C, CommonMark + GFM tables, strikethrough, task lists, SAX callbacks) or cmark-gfm, as in Notepad.
- **Block layout.** Headings, paragraphs, nested lists with markers, quotes with a bar, code blocks with a background and
  horizontal scrolling or wrapping, tables with column alignment, `<hr>`, images (WIC, asynchronously).
- **Virtualization** for `large.md`: 3.7 MB, 97 thousand lines. The blocks of the first screen are laid out first, the rest
  lazily or in the background. The protocol explicitly allows this.
- **Selection across blocks and copying** (plain text and, preferably, HTML/RTF), Ctrl+F search with IME, links,
  context menu, zoom, dark theme.
- **Accessibility.** A UI Automation provider with `ITextProvider` for screen readers — a large and often forgotten
  piece of work. WinUI, WPF and RichEdit give it for free.
- **DPI.** Per-monitor v2, `WM_DPICHANGED`, recreating resources.

Measured potential: **92–150 ms** on a loaded machine and **~110–150 ms** [estimate] on an idle one. No other class of solutions
does better.

---

## 11. RichEdit as the "native middle ground"

Details are in the `cpp-richedit` prototype; here is only its position in the comparison.

- `msftedit.dll` (inbox) or Office RichEdit (as in Notepad): DirectWrite/D2D rendering (D2D mode in RichEdit 8+),
  tables, hyperlinks, images (`ITextRange2::InsertImage`), selection and copying, IME, UIA — all
  "for free", incremental layout.
- Downsides: styling is limited by the RTF/TOM model. Rounded code block backgrounds, quote bars and table zebra striping "like
  GitHub" are hard or impossible to do, so the style comes out "office-like".
- Draft runs of `cpp-richedit` (not final, under load): 256–389 ms with `baseline` at 120–194 ms in the same
  runs.
- `WinUIEdit.dll` (3.3 MB) in the WinAppSDK runtime — judging by its name and size, this is the `RichEditBox` engine in WinUI, i.e. the same
  RichEdit technology line [inspection: file name and size, no disassembly was done].

---

## 12. Explorer preview handlers and PowerToys Peek [inspection]

- **Windows has no built-in preview handler for `.md`.** On this machine, registered under
  `HKCR\.md\shellex\{8895b1c6-b41f-4c1c-a562-0d564250836f}` is
  `{60789D87-9C3C-44AF-B18C-3DE2C2820ED3}` "Markdown Preview Handler" from PowerToys 0.100.2.
- **The PowerToys Markdown preview handler** has three layers:
  - the COM shim `PowerToys.MarkdownPreviewHandlerCpp.dll` (C++, loaded into prevhost);
  - it launches **a separate .NET 10 process** `PowerToys.MarkdownPreviewHandler.exe` (WinForms + **WebView2**
    1.0.3719.77 + **Markdig** 0.34, `NavigateToString`) and embeds its window;
  - every preview pays for starting .NET and the WebView2 environment.
- **PowerToys Peek** — WinUI 3 (WinAppSDK 2.0.1, .NET 10). Markdown is shown via `WebBrowserPreviewer`
  (WebView2 + `MarkdownHelper` → HTML). **Peek is a resident process:** the module launches `PowerToys.Peek.UI.exe`
  when enabled (`enable()` → `launch_process()`, `src/modules/peek/peek/dllmain.cpp`), and the hotkey only
  signals the already running process. Peek is disabled on this machine, so its display speed was not measured.
- **Conclusion.** Even Microsoft keeps the process resident for an "instant" preview in WinUI/WebView2. For FastMD this is
  an argument for an optional `-resident` variant (tray), but the main path must be fast without it.

---

## 13. Summary table

Times are **warm start to the first screen of the document on this machine (RTX 5070)**. "Measured" is the lab floor
of the stack without markdown under load, "estimate" accounts for a typical markdown page. The final numbers will come from a serial
run of the prototypes.

| Stack | Time to content, warm (cold) | Basis | GFM fidelity: tables / code / images / selection | Style (Fluent, Mica, typography) | Effort | Distribution size | Cross-platform | Main risks |
|---|---|---|---|---|---|---|---|---|
| **Win32 + D2D (WARP/software) + DirectWrite + md4c** | **~110–150 ms** (≈ the same) | measured: floor 92–151 ms | everything is possible, everything by hand; selection and copying are our own | full control; Mica via DWM; the best DirectWrite typography | **high** | **0.3–3 MB**, one exe | no (the layout core can be abstracted) | the amount of custom layout, UIA/accessibility, 4K scrolling on the CPU |
| Win32 + D2D **hardware** | ~330–460 ms | measured: 358–459 ms | the same | the same | high | the same | no | NVIDIA tax of 180–250 ms |
| Win32 + GDI | ~90–120 ms | measured: 92–119 ms | tables and code by hand, **no color emoji**, worse antialiasing | dated look | high | < 1 MB | no | "ugly" |
| Win32 + RichEdit (the Notepad way) | ~150–300 ms | draft runs of `cpp-richedit` | tables, images, selection, IME, UIA — "out of the box" | "office-like", the GitHub style is hard to achieve | medium | < 1 MB + inbox `msftedit` | no | styling limitations |
| WinUI 3, C++/WinRT, unpackaged | ~350–600 ms (+100–300) | estimate; bootstrap and DLL loading measured | RichTextBlock: tables only as UIElement, no selection in them, no virtualization | **the best Fluent**: Mica, TitleBar, themes | high (C++/WinRT, XAML) | 2–5 MB + runtime (FD) or +50–120 MB (SC) | no (Uno — partially) | D3D on the path, window before the frame, toolchain |
| WinUI 3, C# NativeAOT | ~400–700 ms (+150–400) | estimate; see `bench/protos/winui3` | the same + Labs MarkdownTextBlock (preview) | the same | medium | ~15–40 MB + runtime | no | AOT compatibility of libraries, preview controls |
| WPF (.NET 8/9/10, R2R) | **~630–900 ms** (+200–500) | measured: 632–672 ms for a TextBlock | FlowDocument: tables, selection, printing; no virtualization | .NET 9 Fluent theme, WPF UI | low–medium | 1.6 MB FD + .NET Desktop runtime / ~160 MB SC | no | no AOT, D3D9 + NVIDIA, slow |
| WinForms (.NET) | ~240–310 ms (+100–300) | measured: 243–305 ms | RichTextBox or WebView2 | dated | low | 1 MB FD / 57 MB AOT (bad) | no | AOT not supported |
| .NET MAUI (Windows) | ≥ WinUI C# JIT, ~0.8–2 s | estimate | as WinUI | Fluent via WinUI | medium | 100+ MB | yes (mobile/mac) | no AOT on Windows, an extra layer |
| Avalonia 12 NativeAOT | ~300–600 ms (ANGLE); **~200–350 ms in Software** — hypothesis | estimate; see `bench/protos/avalonia` | Markdown.Avalonia: tables and code, per-block selection | its own Fluent theme, non-native text (HarfBuzz/Skia) | medium | ~30–60 MB | **yes** | GPU by default, large exe, alpha versions of md packages |
| Uno Skia (Win32) NativeAOT | ~0.4–0.9 s | source: Uno.Chefs 0.824 s (full app) | WinUI-like API | Fluent-like | medium | 50–100 MB | yes | heavy, OpenGL on the path |

---

## 14. Conclusions for FastMD

### 14.1 Stack recommendation

1. **The core is C or C++ Win32 + DirectWrite + Direct2D, the first frame without a hardware GPU.**
   - A D3D11 **WARP** device (4 ms) + DXGI flip + DirectComposition, or a D2D software HwndRenderTarget.
   - If profiling of 4K scrolling or large documents shows a CPU shortfall, the hardware device is created **after**
     the first frame in the background, and rendering switches to it. The user does not wait 185 ms for the NVIDIA driver.
   - A separate question is how background device creation coexists with the loader lock (§2.2): do it after the first
     frame, not in parallel with it.
2. **The window is shown without activation**, activation comes right after the first `Present`/`DwmFlush` (−25–60 ms, §2.3).
   The search box and IME are created lazily.
3. **The first screen before everything else.**
   - md4c parses the whole file: parsing runs at hundreds of MB/s, so it is cheap. Only the blocks visible in the
     first screen are laid out. The rest goes to the background or happens on scroll; the protocol allows this, and it has to be described in the README.
   - Images are decoded asynchronously (WIC), space is reserved for them.
   - Color emoji: an atlas, or monochrome on the first frame with a redraw afterwards (§2.4).
4. **One small exe (< 3 MB), no runtimes.** Static CRT, no single-file bundles. Rarely needed parts go into DLLs
   loaded late (§2.5).
5. **XAML only if a "rich chrome" is needed** (settings, dialogs), and only via XAML Islands **after** the first
   frame, as in Notepad. The document is never rendered with XAML.
6. **The fallback with less effort is RichEdit**, if the `cpp-richedit` prototype shows an acceptable style:
   that is how Microsoft built Notepad.
7. **Do not use** WPF, WinForms, MAUI and Uno for the main product. **Avalonia** — only if cross-platform support
   becomes a requirement, and measure it in `RenderingMode=Software` first.
8. **Resident mode** (a tray process, passing the path via `WM_COPYDATA` or a named pipe) is a separate, honest
   `-resident` option. PowerToys Peek does this. The base path must be fast without it.

Expected time: **~110–150 ms to the first screen on this machine when idle**. That is 1.2–1.7 times slower than the empty
reference window and 4–8 times faster than WPF or WinUI-C# in the same configuration.

### 14.2 Risks

| Risk | Severity | Mitigation |
|---|---|---|
| The effort of a custom layout (tables, nested lists, selection across blocks, HTML copying) | high | incrementally: first the visuals of the first screen, then selection; md4c for structure; if it fails, the RichEdit path |
| Accessibility (UIA `ITextProvider`, screen readers) | medium–high | build the UIA provider into the architecture from the very start |
| Scrolling large documents on WARP/CPU at 4K and 144 Hz | medium | glyph atlas + DComp layers; switch to a hardware device after the first frame |
| Color emoji (~30 ms on the first frame) | low | an atlas or a deferred redraw |
| IME/TSF in the search box and deferred activation (foreground rights) | low | lazy creation; test launching from Explorer, not only from the harness |
| Defender rescans the exe on every launch | medium | a small exe; check the effect of code signing (open question) |
| Machine diversity: other GPUs, iGPU-only laptops, RDP (WARP) | medium | the WARP path is the same everywhere, which is a plus; the hardware path is only an option |
| Third-party injectors (Windhawk, GPU overlays) inflate the "floor" | external | take into account when comparing; state in the user-facing report that the measurement was on a machine with Windhawk |

### 14.3 Notes on the harness and the protocol (for the orchestrator; I made no changes)

1. **Windhawk 1.7.3** injects 4 modules into all processes, including those launched by the harness (§1). The 90 ms floor includes this.
   If desired, a one-off measurement could be made with Windhawk suspended — only with the user's consent.
2. **`cpu_ms` does not see Defender's time** (it is outside the process), but `content_ms` does. For large exes (Electron, AOT bundles)
   the difference from "pure" CPU will be large; `dist_mb` should be looked at together with `content_ms`.
3. **Deferred activation** (§2.3) — a legitimate optimization or a violation of "shown activated"? Decide explicitly for everyone.
   A related harness problem: apps launched from a background python process **do not get foreground**
   (`runner/fgcheck.py`). The activation path (focus, TSF) therefore differs from a real double click in Explorer.
   It is worth measuring, at least selectively, a launch via `ShellExecuteEx` from a foreground process or via Explorer itself.
4. **The choice of GPU** (hardware / WARP / iGPU) matters more than the choice of framework. I recommend that every prototype
   state in `notes` which device it creates before the first frame. For prototypes with a software rendering option
   (Avalonia `Software`, Chromium `--disable-gpu` [hypothesis for the web report]) a variant with it is useful.
5. No bugs were found in `bench.py`. All the lab runner did was import `bench.py` unchanged.

### 14.4 Open questions

- MSIX activation overhead versus unpackaged: not measured, Developer Mode or a certificate is needed.
- The effect of an Authenticode signature on Defender rescanning the exe.
- Real numbers for WinUI 3 (C++ and C# AOT) and Avalonia (Software and ANGLE) on this rig — waiting for the serial run of
  the prototypes.
- Cold start after a reboot (administrator rights are needed to clear the standby list).
- Whether WinUI has a supported way to render via WARP. The strings `ForceWarp`/`SoftwareRendering` exist in
  `Microsoft.UI.Xaml.dll`, but there is no public API.

---

## Appendix A. Reproduction (everything is in `research/lab/native-ui/`)

| File | What it is |
|---|---|
| `runner/labrun.py` | the runner: imports `bench/harness/bench.py`, interleaves launches, writes `runner/results/<config>.json` |
| `runner/final.json`, `w32.json`, `ime.json`, `dotnet.json`, `wpf.json`, `size.json` | run configurations; results are in `runner/results/` |
| `w32/w32lab.cpp` → `w32/out/w32lab.exe` | the Win32 lab: `gdi \| d2d-hwnd \| d2d-hwnd-sw \| d3d-flip \| d3d-dcomp \| warp-dcomp \| igpu-dcomp` + flags `--noime --noimc --lateact --notrans --latin --bgdevice --cloak` |
| `w32/d3dprobe.cpp` | the cost of creating DXGI/D3D11/D3D12 devices + the list of loaded modules (`results/d3dprobe.txt`) |
| `w32/stackprobe.cpp` → `stackprobe2.exe` | a module-level sampling profiler for `D3D11CreateDevice` (`results/stackprobe.txt`) |
| `w32/dwprobe.cpp` | DirectWrite/D2D: layout, first and second draw, color, fallback (`results/dwprobe.txt`) |
| `w32/dllload.c`, `w32/tinydll.c`, `w32/modlist.c` | Defender's DLL scan, the list of injected modules (Windhawk) |
| `wasdk/bootprobe.cpp` + `wasdk/pkgs/` (NuGet Foundation 1.8.260803002 and 2.3.12) | `MddBootstrapInitialize2`, loading `Microsoft.UI.Xaml.dll`, the `Application` factory (`results/bootprobe.txt`) |
| `dotnet/con`, `dotnet/wf`, `dotnet/wpf` | .NET 8: console (JIT/AOT), WinForms (JIT/R2R/SC-R2R/AOT), WPF (JIT/R2R, hw/sw); publish output in `dotnet/out/` |
| `runner/defender_cpu.py` | exe + a 55 MB "tail": 10 consecutive launches (`results/defender_cpu.txt`) |
| `runner/fgcheck.py` | checks whether the window becomes foreground after a launch from the harness (`results/fgcheck.txt`) |

C/C++ build: `bench/tools/msvc.cmd cl /nologo /utf-8 /O2 /MT /EHsc /DUNICODE /D_UNICODE <file> /link ...`.
.NET AOT: `bench/tools/msvc.cmd dotnet publish ... -p:PublishAot=true`. It goes through `msvc.cmd` because ILCompiler
looks for `vswhere.exe` in PATH.

Run: `python research/lab/native-ui/runner/labrun.py research/lab/native-ui/runner/final.json --runs 11`.

## Appendix B. Sources

- Windows App SDK 2.0 release notes (2.0.1 … 2.5.1, 2026): <https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0>
- WinUI 3 Performance: A Leap Forward, 2026-05-11: <https://github.com/microsoft/microsoft-ui-xaml/discussions/11096>
- Our commitment to Windows quality, 2026-03-20: <https://blogs.windows.com/windows-insider/2026/03/20/our-commitment-to-windows-quality/>
- WinUI: smooth launch / window before the first frame, #11677 (2026-08-27): <https://github.com/microsoft/microsoft-ui-xaml/issues/11677>
- WinUI: GDI redirection surface, #11523 (2026-08-13): <https://github.com/microsoft/microsoft-ui-xaml/issues/11523>
- WinUI startup best practices (ms.date 2026-03-16): <https://learn.microsoft.com/en-us/windows/apps/develop/performance/app-startup-performance>
- Why is WinUI so slow (2021, historical): <https://github.com/microsoft/microsoft-ui-xaml/discussions/8595>
- XamlMetadataProvider startup blowup, #8281 (fixed in 1.3.2): <https://github.com/microsoft/microsoft-ui-xaml/issues/8281>
- What's new in Windows App SDK 1.6 (NativeAOT, −50% startup): <https://blogs.windows.com/windowsdeveloper/2024/09/04/whats-new-in-windows-app-sdk-1-6/>
- CsWinRT releases (3.0 preview): <https://github.com/microsoft/CsWinRT/releases>
- Bootstrapper API: <https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/use-windows-app-sdk-run-time>
- CommunityToolkit Labs MarkdownTextBlock: <https://github.com/CommunityToolkit/Labs-Windows/issues/606>, #643, #750, #759, #772, #780
- UWP + modern .NET + NativeAOT GA (2026-01-26): <https://learn.microsoft.com/en-us/windows/uwp/dotnet-native/modernize-uwp-apps-with-dotnet>
- Notepad formatting (2025-05-30): <https://blogs.windows.com/windows-insider/2025/05/30/text-formatting-in-notepad-begin-rolling-out-to-windows-insiders/>
- Notepad 11.2512.10.0 (2026-01-21): <https://blogs.windows.com/windows-insider/2026/01/21/notepad-and-paint-updates-begin-rolling-out-to-windows-insiders/>
- WPF trimming/AOT, #3811: <https://github.com/dotnet/wpf/issues/3811>
- WinForms AOT: <https://github.com/dotnet/winforms/issues/4649>, <https://github.com/dotnet/sdk/issues/34129>
- MAUI Windows NativeAOT, #31227: <https://github.com/dotnet/maui/issues/31227>
- Avalonia 12 (2026-04-07): <https://avaloniaui.net/blog/avalonia-12>
- Avalonia Win32 RenderingMode: <https://api-docs.avaloniaui.net/docs/P_Avalonia_Win32PlatformOptions_RenderingMode>
- Avalonia NativeAOT: <https://docs.avaloniaui.net/docs/deployment/native-aot>
- Uno Platform 6.6 (2026-07-29): <https://platform.uno/blog/uno-platform-6-6/>
- Uno Skia desktop: <https://platform.uno/docs/articles/features/using-skia-desktop.html>
- Markdown Monster / WebView2: <https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization>
- Windows Terminal AtlasEngine, PR #11623: <https://github.com/microsoft/terminal/pull/11623>
- Sublime Text hardware acceleration (2021): <https://www.sublimetext.com/blog/articles/hardware-accelerated-rendering>
- Disassembling Sublime Text: <https://thume.ca/2016/12/03/disassembling-sublime-text/>
- File Pilot (HN, the author on the stack): <https://news.ycombinator.com/item?id=43091466>; <https://filepilot.tech/about>
- RemedyBG: <https://remedybg.handmade.network/>
- nene-nib — the D3D11CreateDevice tax on NVIDIA (2026-09-16/17): <https://github.com/hideyukiMORI/nene-nib/issues/24>
- PowerToys Peek, resident launch: `src/modules/peek/peek/dllmain.cpp` in <https://github.com/microsoft/PowerToys>
- Calculator (UWP + .NET Native + WinUI 2.8.7): <https://github.com/microsoft/calculator>
