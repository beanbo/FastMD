[Русский](05-windows-startup-engineering.md) | **English**

# 05 — Process startup engineering on Windows: where the milliseconds go and how to cut them legitimately

> Research for FastMD (a fast .md reader). Date: 2026-09-19. Machine: Windows 11 Pro 25H2 (26200.8037), Ryzen 9 7950X, 64 GB, NVMe, NVIDIA RTX 5070 (driver 32.0.16.1656, monitor 3440×1440@165 Hz, 96 DPI) + AMD iGPU (no monitor attached), Defender RTP on, non-admin session.
> Everything marked **[measured]** was measured by me on this machine with the lab programs in `research/lab/startup/` (sources, raw JSONL and reproduction scripts are there too). Everything else comes from linked sources.
> **Noise caveat:** the main series ran while 11 other agents were building prototypes (CPU load 80–92 %); the tables give median (min–p90), and min is the best estimate of the "own" cost. Some groups were repeated on a "quiet" machine (load 13–20 %) — they are marked **[quiet]**. The orchestrator's final serial run on an idle machine is authoritative; my numbers are for relative conclusions.

---

## 0. TL;DR

**The most important things the experiments showed (all of them [measured]):**

| # | Finding | Cost on this machine | What FastMD should do |
|---|---|---|---|
| 1 | `D3D11CreateDevice` on NVIDIA (UMD ≈195 MB of DLLs: `nvwgf2umx.dll` 87 MB + `nvgpucomp64.dll` 107 MB) | **≈205 ms [quiet]** (up to 280 under load) per process + first `Present` ≈18 ms; flags don't help. First D2D frame: NVIDIA 314 ms vs WARP 100 ms [quiet] | First frame without a hardware D3D device: WARP (≈15 ms), DirectWrite→DIB→GDI, or GDI. The HW device lazily/in the background, or never |
| 2 | Defender synchronously "processes" **every** process creation in the kernel, on the parent's thread; the cost is ∝ EXE size | **≈1.2–1.9 ms/MB on every launch** [quiet] (150 MB → ≈300 ms, Electron 235 MB → ≈465 ms, VS Code 193 MB → ≈245 ms); a DLL of any size costs ≈1 ms | Keep the EXE small (hundreds of KB to a few MB), put the heavy parts in DLLs. No single-file .NET (64 MB → +130 ms) |
| 3 | First window: `ShowWindow` activates the window → TSF initialization (TextInputFramework, CoreMessaging, CoreUIComponents) inside `WM_ACTIVATE` | **≈12–15 ms [quiet]**, 25–35 ms under load (+ rare hangs of up to 2 s) | `ImmDisableIME(-1)` before creating the window, or show with `SW_SHOWNOACTIVATE` → activate after the first frame |
| 4 | Windows 11 DWM window-open animation | the window is "fully visible" **40–60 ms later** than t_content | `DWMWA_TRANSITIONS_FORCEDISABLED` = TRUE |
| 5 | Color emoji (Segoe UI Emoji — COLRv1) in Direct2D | 5 distinct emoji (like the first screen of medium.md): **+22 ms (WARP) / +35 ms (NVIDIA) [quiet]**, up to 90 ms under load; a repeat frame ~1–3 ms | viewport-first; our own emoji bitmap cache; if needed, monochrome in the first frame |
| 6 | Handing "open this file" to an **already running** process via `DelegateExecute`/`IExecuteCommand` (COM local server) | Explorer→window shown: **32–57 ms** vs **98 ms** for a classic new process (trivial GDI viewer) | An optional resident mode (the gain grows with how heavy the engine's initialization is) |
| 7 | A new (just built/installed) EXE | +20–40 ms (4 KB) … +150 ms (150 MB) before the process object is created, on the **first** launch | Account for "first launch after install"; don't rebuild before measuring |
| 8 | COM/WinRT apartment: `CoInitializeEx(STA)` / `RoInitialize` | 10–20 ms each | Don't call on the critical path unless the engine requires it |

**The process floor on this machine [measured, quiet]:** the process object is created ≈0.7 ms after `CreateProcessW`, but the call itself returns after ≈21 ms (synchronous Defender processing of an "unknown" EXE; for known small EXEs, 4–8 ms), and the first line of `main` of an empty EXE runs after ≈31 ms (min 28). A "Win32+GDI window with text" takes ≈77 ms, of which ≈12–15 ms is TSF; with `ImmDisableIME` + showing without activation, ≈58 ms; the DWM animation adds another 40–60 ms "until the window is fully visible" (t_content does not see it).

**Architecture recommendation (details in §12):** a small native EXE (C/C++ or Rust, static CRT), DirectWrite layout of the first screen only, the first frame through WARP-D2D or DWrite→DIB, `ImmDisableIME`/no-activate-first, the DWM animation disabled, no HW D3D/COM/WinRT on the critical path; optionally, a resident mode with `DelegateExecute` (no new process at all).

---

## 1. Methodology and tools

### 1.1 The `research/lab/startup/` lab

| File | Purpose |
|---|---|
| `src/labmark.h` | child marks into the named shared memory `Local\FastMDLabShm` (kernel32 only, works under `/NODEFAULTLIB` too), a "ready" event |
| `src/launch.cpp` → `bin/launch.exe` | launcher: `t0 = GetSystemTimePreciseAsFileTime()` immediately before the launch; modes `cp` (CreateProcessW), `cpjob` (like `bench.py`: SUSPENDED+Job+Resume), `shell` (ShellExecuteExW), `explorer` (ShellExecute **inside explorer.exe** via `IShellWindows→IShellDispatch2`, i.e. the double-click path), `susp` (CreateProcessW(SUSPENDED)+TerminateProcess: the child executes not a single instruction). Options `--msonly` (the `BLOCK_NON_MICROSOFT_BINARIES` policy — cuts off Windhawk injection), `--self-msonly`, `--fresh` (a copy of the EXE with a new hash), `--console`. Records: the call's return time, the process object's creation time (`GetProcessTimes`), the child's marks, the child's CPU (`QueryProcessCycleTime`), the parent's CPU inside `CreateProcessW` (cycles + kernel/user) |
| `src/trivial.c` | empty EXEs: GUI/console, `/MT`, `/MD`, no CRT, with/without a manifest, with a user32 import, with 5/50/150 MB of ballast (random data and zeros in RCDATA) |
| `src/probe.cpp` → `bin/probe.exe` | a step "construction kit": statically imports **only kernel32**, everything else is `/DELAYLOAD` → a step's time includes loading the DLLs it needs. Steps: `load:`/`map:` DLL, COM/WinRT, window (with a trace of the first `WndProc` messages), D3D11 hw/warp/iGPU/dGPU, D2D, DWrite, swap chain hwnd/DComp, drawing, Present, DwmFlush, GDI text, DWrite→DIB, D2D DC RT, emoji pre-warm… |
| `src/dup.cpp` → `bin/dup.exe` | a "photon" measurement via DXGI Desktop Duplication: every composed desktop frame with its QPC present time, a 24×24 grid of points in the window → when the window's pixels actually appeared and when the animation finished |
| `src/srv.cpp`, `src/handoff.c` | a stub viewer in several roles: a new process; a COM local server `IExecuteCommand` (DelegateExecute); a resident (+ a pre-created/"cloaked" window); a tiny `WM_COPYDATA` hand-off launcher |
| `src/bstest.c` | Windows App SDK 1.8 bootstrapper (`MddBootstrapInitialize2`) |
| `dotnet/` | the same `Program.cs` in 8 .NET 8 publishing variants + the .NET 9 runtime |
| `lab.py`, `handoff.py`, `reg.ps1`, `build.ps1` | building, interleaved rounds, reports; registration of the test associations `.fmdtest*` (HKCU) |

Reproduction: `pwsh -File research/lab/startup/build.ps1`, then `python research/lab/startup/lab.py run <proc|fresh|dll|gfx|win|frame|susp|map|dotnet|dotnet2> [--rounds N] [--tag _x]`, `python research/lab/startup/handoff.py` (requires registering with `reg.ps1 install` **through explorer**, see §11.1). Raw data is in `results/*.jsonl`, summaries in `results/*.txt`.

### 1.2 Three quirks of this particular machine that affect ALL bench measurements

1. **Windhawk 1.7.3** injects itself into every process (the `windhawk.dll` engine + the `explorer-details-better-file-sizes` mod with `Include=*`, which pulls in `libc++.whl`, `SHELL32`, `windows.storage`, `OLEAUT32`, `combase`…). Mechanism: hooking `CreateProcessInternalW` in the parent and APC injection into the suspended child ([m417z, 2022](https://m417z.com/Implementing-Global-Injection-and-Hooking-in-Windows/), [Windhawk wiki](https://github.com/ramensoftware/windhawk/wiki/Injection-targets-and-critical-system-processes)). **[measured]** +≈5 ms of CPU per process; before `main`, +≈5 ms under load and ≈0 when quiet (the work happens in an APC before the entry point, but in parallel with the loader); for processes that don't need shell32/COM this is pure overhead. In the lab I cut it off with the "Microsoft-signed images only" policy (`--msonly`); ordinary users with Windhawk or similar tools will pay these ms in every application.
2. **Defender RTP** (engine 1.1.26080.3, `EnableFileHashComputation=False`, SAC off, VBS running without HVCI) — see §2.2: the cost of creating a process depends on the EXE size.
3. **Claude Desktop is an MSIX package** (`Claude_2.2553.1.0_x64__pzs8sxrjxfjjc`), and everything launched from this session (including `bench.py` and all the prototypes!) gets that package's **AppData and HKCU virtualization** — see §11.1. This affected the COM experiment (§8) and affects the web prototypes.

---

## 2. Anatomy of `CreateProcessW` → `main`

### 2.1 Timeline of an empty process [measured]

`t0` → **≈0.7–1 ms**: the process object is created (time from `GetProcessTimes`) → **≈21 ms [quiet]** (18–30 under load): `CreateProcessW` returns → **≈31 ms [quiet]** (32–40): the first instruction of `main` in an EXE without a CRT (ntdll+kernel32+kernelbase) → then whatever the application itself does.

The key experiment is to separate "before the process object" from "after". The `susp` mode creates the process suspended and kills it immediately (not a single child instruction), while the launcher measures the CPU of **its own thread** inside the call:

| EXE (CreateProcessW SUSPENDED, launcher outside Windhawk) | call, ms | parent thread CPU inside the call | of which kernel |
|---|---|---|---|
| our `t_gui_nocrt.exe`, 4 KB, unsigned, "unknown to anyone" | 20.7 | 19.3 | 17.3 |
| `t_gui_mt_pad150.exe`, 150 MB, unsigned | 367 | 365 | 356 |
| `MRT.exe`, 211 MB, **signed by Microsoft** | 285 | 282 | ≈300 |
| `msedge.exe`, 5 MB, signed by Microsoft | 4.6 | 3.8 | ≈5 |

→ Almost all of `CreateProcessW`'s time is **kernel-mode CPU on the caller's thread**, proportional to the EXE size — that is, a synchronous process-creation callback, not waiting on I/O or a service.

**Proof that this is Defender [measured]:** the ETW providers `Microsoft-Antimalware-Engine`/`-RTP` are available to the "Performance Log Users" group without admin rights. For every process creation the engine writes event 73 `SyncStart`, and the `SyncStart → Termination` interval (the launcher kills the process right after the call returns) matches the measured duration of `CreateProcessW` (≈330 ms for pad150, ≈280 ms for MRT); after that comes Behavior Monitoring `IsKnownFriendly()`; on the first launch, `USN Cache | MISS` and `NRIOnForProcess`. Excerpt: `results/defender_events.txt`. The `Microsoft-Windows-CodeIntegrity` provider isn't available without admin, so I didn't see the kernel stack (WdFilter vs CI) — "the Defender process-creation path" is the most likely attribution, but not a 100 % proven one.

### 2.2 EXE size is a "tax" on every launch [measured]

`susp`, 10 rounds, launcher outside Windhawk; median (min–p90), ms:

| EXE | size | signature | under load | **[quiet]** |
|---|---|---|---|---|
| python.exe | 0.1 MB | PSF | 3.8 (3.6–22) | — |
| git-bash.exe | 0.1 MB | **none** | 5.0 (4.8–49) | — |
| cmd.exe | — | MS (catalog) | 4.3 (3.6–14) | — |
| msedge.exe | 5 MB | MS | 4.9–7.3 | 7.8 (6.7–23) |
| dotnet.exe | 0.2 MB | MS | 6.2 (5.7–10) | — |
| **our** t_gui_nocrt | 4 KB | none, new | 18.7–23.8 | 22.3 (19.4–25.9) |
| our pad5 (random data) | 5 MB | none | 30.1 (26.8–42.8) | 30.3 (28.8–38.3) |
| our pad50 | 50 MB | none | 119 (113–129) | 113 (111–127) |
| node.exe | 88 MB | OpenJS | 127 (min 8.2!) | 116 (112–162) |
| WPF single-file (copy of the wpf prototype) | 147 MB | none | 316 (298–373) | 299 (291–500) |
| our pad150 **random** | 150 MB | none | 314 (305–348) | 299 (293–353) |
| our zero150 **zeros** | 150 MB | none | 329 (310–376) | 302 (293–424) |
| Code.exe (VS Code) | 193 MB | MS | 259 (255–270) | 244 (233–311) |
| Cursor.exe | 201 MB | Anysphere | 270 (269–292) | 252 (248–300) |
| MRT.exe | 211 MB | MS | 280 (273–295) | 263 (254–293) |
| electron.exe (copy from the electron prototype) | 235 MB | none | **481** (458–490) | **464** (451–775) |

(The system `notepad.exe` is no good as a reference: 25 ms to the process object — in Windows 11 it is a stub that redirects to the Store version.)

Conclusions:
- **The tax is ≈1.85 ms per MB for unsigned and ≈1.2 ms/MB for Microsoft-signed EXEs, on every launch** [quiet]; it doesn't depend on entropy (random ≈ zeros) and isn't removed by a signature (Code.exe, MRT.exe). The exceptions are "known" small files (msedge.exe, python.exe, git-bash.exe: 4–6 ms); node.exe is bimodal (sometimes 8 ms, usually 127) → there is a cache of "friendly" files with restrictions, which our fresh 4 KB EXE doesn't get into (18–24 ms, 15–17 ms of kernel time).
- **DLLs pay no tax:** `LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES)` of a 150 MB unsigned DLL takes **1.2 ms**, the 332 MB WebView2 `msedge.dll` <1 ms, the 107 MB `nvgpucomp64.dll` <1 ms (`results/map.txt`). This is exactly why Chromium keeps `chrome.exe`/`msedge.exe` small and the code in `chrome.dll`/`msedge.dll`.
- **Consequences for the candidates:** Electron pays ≈0.46–0.48 s *per process creation* (browser, GPU, renderer — all from the same 235 MB EXE; the time is spent on the thread that creates the process); VS Code/Cursor ≈0.25 s per process; .NET single-file 64 MB, +≈130 ms (§7); NativeAOT 2–10 MB, +3–20 ms; native C++/Rust 0.2–3 MB, ≈0.

### 2.3 First launch of a freshly written EXE [measured]

`--fresh`: a copy of the EXE with a unique tail (a new hash), launched immediately or after 3 s:

| | process object created, ms | CreateProcessW returned | main |
|---|---|---|---|
| 4 KB, immediately / +3 s | 40.7 / 14.6 | 66.8 / 47.4 | 79.6 / 59.7 |
| 5 MB, immediately / +3 s | 50.7 / 30.1 | 87.5 / 70.3 | 100.8 / 83.5 |
| 50 MB, immediately / +3 s | 69.6 / 52.0 | 193 / 200 | 209 / 211 |
| 150 MB, immediately / +3 s | 151 / 116 | 487 / 504 | 501 / 516 |
| for comparison: the same 4 KB, n-th launch | 0.9 | ≈27 | ≈40 |

The first launch of a new binary pays another +20–150 ms of scanning **before** the process object is created (Defender sees a new file via the USN cache); a 3 s pause after writing barely helps. For the bench: `first_ms` in `bench.py` is "the first launch after a build", and it can't be compared with the median.

### 2.4 Subsystem, CRT, manifest [measured]

`proc`: median ms to `main`; "CPU" is the child's cycles. Under load, 15 rounds; **[quiet]**, 10 rounds:

| variant | CreateProcessW (load / quiet) | main (load / quiet) | child CPU (quiet) |
|---|---|---|---|
| GUI, no CRT, Windhawk injected | 32.1 / 21.0 | 47.9 / 31.8 | 15.3 |
| GUI, no CRT (`--msonly`) | 30.2 / 21.9 | 42.8 / 31.7 | 10.5 |
| same, launcher also outside Windhawk | 28.5 / 21.6 | 39.1 / 31.2 | 10.3 |
| like `bench.py` (SUSPENDED+Job+Resume) | 27.5 / 20.9 | 39.3 / 30.2 | 10.2 |
| console, inherits the console | 26.7 / 20.5 | 39.8 / 30.3 | 10.0 |
| console, `DETACHED_PROCESS` | 29.2 / 21.3 | 42.4 / 31.5 | 10.2 |
| **console, `CREATE_NEW_CONSOLE`** | 27.3 / 21.4 | **729 / 503** | 12.9 |
| static CRT `/MT` GUI | 39.1 / 21.7 | 56.0 / 31.7 | 10.9 |
| dynamic CRT `/MD` GUI | 36.9 / 22.4 | 52.5 / 34.0 | 12.5 |
| no manifest / full manifest (comctl6, PMv2) | 32.2 / 21.9 ; 28.3 / 22.1 | 47.4 / 32.0 ; 42.1 / 31.6 | ≈10 |
| + static user32 import | 27.9 / 21.5 | 47.5 / 33.5 | 13.3 |
| + 5 / 50 / 150 MB of ballast (static CRT) | — / 31.9 ; 113.9 ; 299.5 | — / 42.5 ; 124.6 ; 310.2 | ≈11.5 |
| ShellExecuteEx(path to EXE) from a process | 82.0 / 64.0 | 94.8 / 73.0 | 15.2 |

- The console subsystem, launched without a console (as from Explorer), spawns a new console in Windows 11 via Windows Terminal/`conhost` — **0.5–1.6 s**. FastMD is `/SUBSYSTEM:WINDOWS` only.
- Static vs dynamic CRT and the manifest are within noise (±2 ms when quiet). Importing user32 costs +≈2 ms and +3 ms of CPU (hooking up win32k), and it's needed anyway. When quiet, Windhawk doesn't shift `main`, but adds ≈5 ms of CPU per process.
- `ShellExecuteEx` on an EXE from an ordinary process adds ≈40 ms *before the process is created* (shell32 initialization in the calling process + associations) — in Explorer this is already warm, see §8.
- The interval "CreateProcessW returned → `main`" is ≈9–10 ms, with ≈10 ms of child CPU even for an EXE without a CRT: loader initialization, image callbacks and the like — not something the application can optimize.

---

## 3. DLLs and runtime initialization [measured]

A process that statically imports only kernel32; step = `LoadLibrary`/call (median…min range, ms, under load; **[quiet]** in the paragraph below):

| step | ms |
|---|---|
| user32 (+GUI thread) | 2.2–3.9 |
| gdi32 | 2.1–3.4 |
| uxtheme / dwmapi / comctl32 v5 / shell32 | 3–5.6 |
| imm32 + msctf | ≈3.5 + 1.5 |
| d3d11 / dxgi / dcomp | 3.3–5.5 |
| d2d1 / dwrite / windowscodecs | 0.7–1.9 |
| **`CoInitializeEx(STA)`** (with ole32) | **12.6–17.4** |
| **`OleInitialize`** | **16.3–20.7** |
| **`RoInitialize`** | **15.3–21.5** |
| + `CreateDispatcherQueueController` | +2.6–5.3 |
| + `RoActivateInstance(Windows.UI.Composition.Compositor)` | +2.9–3.9 |
| `D2D1CreateFactory` | 0.6–1.0 |
| `DWriteCreateFactory(SHARED)` / `GetSystemFontCollection` | 1.4–1.9 / 0.8–0.9 |
| `CreateTextFormat` + `CreateTextLayout` (Latin / mixed Cyrillic+CJK+emoji) | 1.0 + 2.2–2.7 / 4.4–4.8 |

System DLLs from KnownDLLs/the cache cost a few ms; what's expensive isn't the DLLs but **COM/WinRT apartment initialization** (10–20 ms) and what comes after it (WebView2, WinUI, WIC…). DirectWrite is cheap: the shared factory uses the system font cache.

**[quiet]** (10 rounds, median/min): user32 2.2/1.8; gdi32 2.1/1.9; uxtheme 3.4/3.2; dwmapi 3.4/2.9; comctl32 3.1/2.7; shell32 3.4/2.9; imm32+msctf 2.2+1.2; d3d11 3.6; dxgi 3.2; dcomp 4.2; d2d1 0.8; dwrite 1.6; WIC 1.5; **`CoInitializeEx(STA)` 11.7/11.1; `OleInitialize` 15.6/14.6; `RoInitialize` 15.2/14.2**; DispatcherQueue +2.5; WinRT Compositor +3.1; an "empty" process to `main` 31.0 (min 29.4).

---

## 4. Graphics: the device, the first frame, emoji [measured]

### 4.1 Devices (no window)

| step | median (min), ms |
|---|---|
| `D3D11CreateDevice` default adapter = **NVIDIA RTX 5070** | **258 (224)** |
| … with `SINGLETHREADED`+`PREVENT_INTERNAL_THREADING_OPTIMIZATIONS` / with FL 10_0 | 222 (205) / 214 (205) |
| … explicitly the dGPU (`EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`) | 270 (246) (+ DXGI factory 18–26) |
| … AMD iGPU (MINIMUM_POWER) | 84 (65) (+ DXGI factory 16–25) |
| **WARP** | **22–29 (15–20)** |

Modules pulled in by the NVIDIA device (`opt:moddiff`): `nvldumdx.dll` → WinTrust/Crypt32/`cryptnet`/`drvstore`/`devobj` (UMD signature check) → `nvwgf2umx.dll` (86.8 MB) + `nvgpucomp64.dll` (106.7 MB, the shader compiler) + `nvppex.dll`, `NvMemMapStoragex.dll`, `SHELL32`, `windows.storage`… This is the cost for **every process** that creates a hardware D3D11 device on this machine: WebView2/Chromium (the GPU process), WinUI 3, WPF (D3D9), Avalonia/Skia-GL, wgpu/egui, a default D2D HwndRenderTarget.

**[quiet]** (10 rounds, median/min, ms): NVIDIA default **207.5/200.5**, dGPU explicitly 206.3/199.2 (+DXGI factory 16.4), iGPU 58.5/53.7 (+DXGI factory 15.9), **WARP 18.2/17.0**; `D2D1CreateFactory` 0.9; `DWriteCreateFactory` 1.6 + `GetSystemFontCollection` 0.8 (shared and isolated are the same); `CreateTextFormat` 0.8 + `CreateTextLayout` Latin 2.1 / mixed 4.0. Process CPU with the NVIDIA device is 230 ms vs 30 ms with WARP.

### 4.2 The whole first frame of text (window + text + Present + DwmFlush)

`frame`, 10 rounds, 15 px @96 DPI, under load; "done" = from `t0` to the frame on screen:

| path | done, median (min) | bottleneck |
|---|---|---|
| GDI `DrawTextW`, Latin | 116 (102) | — |
| GDI `DrawTextW`, mixed Cyr.+CJK+emoji | 186 (160) | GDI font linking 69–88 ms |
| DirectWrite → `IDWriteBitmapRenderTarget` (DIB) → BitBlt, Latin | 130 (96) | all of the text ≈2 ms |
| D2D, **WARP**, flip swap chain, Latin | **134 (106)** | device 18, drawing 1.7, Present 0.3 |
| D2D, WARP, DirectComposition, Latin | 139 (118) | — |
| D2D `ID2D1DCRenderTarget` (software) | 146 (120) | RT creation 24 ms |
| D2D `ID2D1HwndRenderTarget` (default = hardware) | 381 (321) | internal HW device 247 ms |
| D2D, **NVIDIA**, flip | 431 (333) | device 278, **first Present 20** |
| D2D, NVIDIA, DComp | 390 (332) | device 248, Present 19 |
| D2D, AMD iGPU, flip | 484 (404) | **swap chain 252 ms** (cross-adapter: the monitor is on the NVIDIA) |
| D2D, NVIDIA, two frames in a row | 411 | the second Present another 16 ms |

**[quiet]** (10 rounds; window without `ImmDisableIME`, i.e. with ≈16 ms of `ShowWindow`):

| path | frame on screen, median (min) ms | key steps |
|---|---|---|
| **DirectWrite → DIB → BitBlt**, Latin | **81.8 (76.1)** | factory 0.8, format 0.6, layout 1.0, RT 1.3, drawing 1.9 |
| DirectWrite → DIB, 5 emoji (monochrome) | 84.1 (74.7) | drawing 1.4 |
| GDI `DrawTextW`, Latin / mixed | 90.3 / 143.8 | text 10.3 / **67.4** (font linking) |
| D2D DC RT (software) | 97.2 (88.8) | RT creation 15.5, drawing 20.4 |
| **D2D WARP flip** | **100.2 (87.7)** | device 14.9, swap chain 2.6, drawing 1.4 |
| D2D WARP DComp | 101.9 (87.0) | swap chain+DComp 3.9 |
| D2D WARP flip, 5 color emoji / monochrome | 121.0 / 97.0 | drawing **22.8** / 1.0 |
| D2D `HwndRenderTarget` (default HW) | 287.9 (267.2) | RT creation 207 |
| D2D NVIDIA DComp | 311.9 (301.5) | device 209.6, Present 18.8 |
| **D2D NVIDIA flip** | **313.8 (290.9)** | device 205.3, **Present 18.6** |
| D2D NVIDIA, 5 color emoji | 335.8 (328.3) | drawing 37.0 |
| D2D NVIDIA, 2 frames | 324.6 | second Present 16.3 |
| D2D iGPU flip | 348.8 (339.1) | device 54.2 + factory 13.3, **swap chain 194** |

**Conclusion:** on a machine with a discrete NVIDIA GPU, the hardware D2D path adds **≈+210 ms [quiet]** to the first frame (+250…+300 under load) relative to the WARP/CPU path, and the process CPU grows 4× (281 vs 70 ms). For a page of text, WARP (a multithreaded software rasterizer) or DirectWrite→DIB is more than enough; the iGPU is the worst of all (a cross-adapter swap chain ≈190–250 ms, because the monitor is connected to the NVIDIA). The fastest full path when quiet is **DirectWrite→DIB→BitBlt: 82 ms to the frame**, on par with "bare" GDI and better than it on mixed text (GDI font linking costs 67 ms).

### 4.3 Color emoji — a hidden 20–90 ms [measured]

Segoe UI Emoji (`seguiemj.ttf`, 12.4 MB) in this build contains a **COLR version 1** table (plus 3372 v0 records), and `IDWriteFactory8::TranslateColorGlyphRun` returns `COLR_PAINT_TREE`. Measurements of the first `DrawTextLayout(..., ENABLE_COLOR_FONT)`:

| text | WARP 1st / 2nd frame, ms | NVIDIA 1st / 2nd, ms |
|---|---|---|
| Latin | 2.3–4.1 / ≈1 | 2.9–7.4 / ≈0.6 |
| 1 emoji | 12–17 / ≈1 | 17–29 / ≈1 |
| 5 distinct emoji (like the first screen of medium.md: ✅ ✨ ❌ 📄 🚀) | **26–49 / ≈3** | **59–88 / ≈2.5** |
| 4 distinct × 12 = 48 emoji | 98–117 / **19–33** | 120–188 / ≈20 |
| the same text without `ENABLE_COLOR_FONT` (monochrome) | 1.8–3.6 | — |
| CJK + Cyrillic (no emoji) | 2.4–4.8 | — |

- The cost is rendering the COLRv1 paint tree in D2D (`TranslateColorGlyphRun` itself is ≈17 ms per 48 runs); pre-warming with a single emoji doesn't help (the cache is per glyph+size). Even a repeat frame with 48 emoji takes 20 ms (there is no raster cache).
- GDI draws emoji in monochrome, and mixed text through font linking costs 70–90 ms; `IDWriteBitmapRenderTarget3::DrawGlyphRunWithColorSupport` isn't available on this build (QI → no), i.e. the DWrite→DIB path gives monochrome emoji in ≈2 ms.
- **For FastMD:** render only the first screen; keep our own atlas of emoji rasters (draw each unique glyph once); if the first screen contains many distinct emoji, monochrome in the first frame with "coloring in" on the next frame is acceptable.

### 4.4 DwmFlush and refresh rate [measured]

165 Hz monitor: consecutive `DwmFlush` calls take 6.0–6.4 ms (a 6.06 ms period). The protocol's `DwmFlush()` after Present adds 3–9 ms (waiting for the nearest composition).

---

## 5. The first window: where the 40 ms between "window created" and "WM_PAINT" go [measured]

`win`, 12 rounds under load; a trace of the first entries of messages into `WndProc` (median, ms):

| variant | CreateWindowEx | ShowWindow | of which WM_ACTIVATE (incl. IME_SETCONTEXT) | done (to DwmFlush) |
|---|---|---|---|---|
| baseline (like `baseline-win32`) | 23.9 | **41.2** | **23.5** (7.9) | 156 (including 5 extra `DwmFlush` ≈30 → ≈126) |
| **`ImmDisableIME(-1)`** before the window | 16.8 | **10.8** | 0.4 | **78** |
| `SW_SHOWNOACTIVATE` | 21.2 | 6.7 | — | 80 |
| `WS_POPUP` | 21.8 | 31.5 | 24.4 | 107 |
| no visual styles (`SetThemeAppProperties(0)`) | 19.7 | 38.1 | 22.4 | 119 |
| Mica / dark title bar / custom frame (Extend+NCCALCSIZE) | 22–24 | 29–44 | 19–27 | 107–134 |
| `DWMWA_TRANSITIONS_FORCEDISABLED` | 25.1 | 37.4 | 20.6 | 127 |
| noime + noactivate + popup | 15.9 | **4.6** | — | **76** |

**[quiet]** (10 rounds, median, ms; "frame" = to `DwmFlush` after GDI drawing; for the baseline variant, 5 extra `DwmFlush` calls ≈30 ms are subtracted from "done"):

| variant | CreateWindowEx | ShowWindow | WM_ACTIVATE (IME_SETCONTEXT) | frame |
|---|---|---|---|---|
| baseline | 16.5 | **17.4** | 10.9 (3.1) | ≈77 |
| + Windhawk | 14.8 | 21.8 | 13.5 (4.8) | 82 |
| **`ImmDisableIME(-1)`** | 12.6 | **5.7** | 0.2 | **63.6** |
| `SW_SHOWNOACTIVATE` | 15.4 | 3.8 | — | 63.9 |
| `WS_POPUP` | 14.6 | 14.5 | 11.1 | 71.8 |
| no visual styles | 13.7 | 15.3 | 10.1 | 72.0 |
| Mica / dark title bar / custom frame / no background | 15.7–16.6 | 16.8–18.2 | 10.7–11.8 | 75–81 |
| **noime + noactivate + popup** | 13.1 | **2.5** | — | **58.2** |

When quiet, TSF costs ≈12–15 ms (≈25–35 under load), `DwmFlush` after drawing 5–10 ms, a series of `DwmFlush` calls 5.2–6.1 ms (165 Hz).

- On the thread's first focus, Windows 11 loads `textinputframework.dll`, `CoreMessaging.dll`, `CoreUIComponents.dll`, `wintypes`, `OLEAUT32`… (visible via `opt:moddiff`) and talks to `TextInputHost`/`ctfmon`. On this machine `TextInputHost.exe` had accumulated 127,736 s of CPU over 5 days — clearly broken; one `ShowWindow` in the experiment took **2.0 s** (looks like a TSF cross-process call timeout). `ImmDisableIME(-1)` ([docs](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immdisableime): call it before the thread's top-level window gets its first `WM_CREATE`) removes this entirely.
- This also explains the `baseline-win32` calibration (window visible at ≈55 ms, first `WM_PAINT` at ≈85): 17–30 ms inside `ShowWindow` is activation+TSF while the window is already `WS_VISIBLE` (the bench observer already sees it), plus `UpdateWindow`.
- The price of `ImmDisableIME`: there's no IME on this thread (CJK input in the search box). The RU/EN layouts aren't IMEs, so you can type. **Correction (03.10.2026, measured in FastMD 1.3.1):** you can type, but you can't *switch* the layout with the hotkey from the settings — Ctrl+Shift (like Alt+Shift) doesn't reach a thread without TSF (experiment: a window with `ImmDisableIME(0)` stays on ru, an ordinary one switches ru → en → ru); Win+Space, the shell's switcher, works anyway. Our own `CoCreateInstance(CLSID_TF_ThreadMgr)` + `ITfThreadMgr::Activate` after the first frame brings switching back (≈5 ms in FastMD); no TSF document is needed for this, and there's still no IME on the thread. A lossless alternative: `SW_SHOWNOACTIVATE` → draw/`Present` → `SetForegroundWindow` (a process launched by the user has foreground rights).
- `CreateWindowEx` of the first window (15–24 ms) is loading uxtheme/combase/rpcrt4/msctf and registering themes; `WS_POPUP`/no themes barely help.

---

## 6. What the user sees: pixels and the DWM animation [measured]

`dup.exe` (Desktop Duplication, 165 Hz): a `probe` window filled with #3A7BD5 (topmost, so that a background launch doesn't hide it under the active window); "first pixels" = ≥10 % of the grid points are "blue", "frame ready" = ≥95 % of the points are exactly #3A7BD5. 7 launches per variant, under load, median, ms from `t0` (raw data: `results/dup.json`):

| variant | t_content (DwmFlush in the app) | first pixels | window fully visible |
|---|---|---|---|
| default | 122 | 116 | **159** |
| `DWMWA_TRANSITIONS_FORCEDISABLED` | 98 | 94 | **97** |
| `ImmDisableIME` | 78 | 77 | 140 |
| both | 79 | 79 | **79** |

- t_content (the protocol's method) ≈ "first pixels" (±1 frame) — **the protocol is correct**.
- The Windows 11 open animation (fade+scale) pushes back the moment "the window is fully on screen" by **40–63 ms**; `DwmSetWindowAttribute(DWMWA_TRANSITIONS_FORCEDISABLED, TRUE)` ([docs](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)) is a free perceptual win. The benchmark won't see it; the user will.

---

## 7. Managed runtimes: .NET and Windows App SDK

### 7.1 Anatomy of a .NET launch (sources)
apphost `.exe` → `hostfxr.dll` (finding the runtime/frameworks, `runtimeconfig.json`) → `hostpolicy.dll` (parsing `deps.json`, assembly/path lists) → `coreclr.dll` (VM, AppDomain, GC initialization) → JIT/ReadyToRun `Main` ([native-hosting.md](https://github.com/dotnet/runtime/blob/main/docs/design/features/native-hosting.md), [a walkthrough of the startup sequence](https://mihai-albert.com/2020/03/08/startup-sequence-of-a-dotnet-core-app/)). Framework-dependent apps use the already R2R-compiled `System.Private.CoreLib`; R2R for the app itself matters when there's a lot of its own code. NativeAOT has no host/VM/JIT, just one EXE ([Native AOT](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)). Public numbers differ by an order of magnitude (for example, 64 ms AOT vs 588 ms JIT for a console CLI in [dev.to, 2025](https://dev.to/lzocate-li/net-native-aot-parte-2-jit-r2r-e-aot-em-benchmarks-367i)) — they depend on how much code runs at startup, so I measured it myself.

### 7.2 [measured] `Program.cs`: a mark on the first line of `Main` + a bit of work (50 `string.Format` + `List`), `WinExe`

Two runs: with `--msonly` (without Windhawk; FDD variants with the unsigned `LabDotnet.dll` can't be launched this way — the policy blocks loading it) and without (all with Windhawk):

| variant (.NET 8.0.30, SDK 8.0.424) | size | main, min (median) ms | CPU ms (median) |
|---|---|---|---|
| C, static CRT (reference) | 0.1 MB | 30–31 (32–71) | 11–22 |
| **NativeAOT** | 1.9 MB EXE | **35–39 (44–76)** | 22–30 |
| FDD apphost, JIT tiered | 0.2 MB + shared runtime | 46.7 (84) | 59 |
| FDD + ReadyToRun | | 45.7 (61) | 46 |
| FDD `TieredCompilation=false` | | 48.7 (90) | 58 |
| self-contained / + R2R | 70 MB, 187 files | 45–46 (61–72) | 44–52 |
| .NET 9.0.14 runtime (rollForward) | | 48.6 (102) | 63 |
| single-file | 64 MB EXE | **163 (174–204)** | 40–48 |
| single-file + compression | 34 MB EXE | 142 (152–201) | 77–99 |

- A "bare" CLR (hostfxr+hostpolicy+coreclr+JIT `Main`) costs **≈+15 ms** over the native minimum, NativeAOT **≈+4 ms**. R2R/TC are indistinguishable for hello-world.
- Single-file is the worst thing you can do for startup: it pays the Defender tax for a 64 MB EXE (≈+130 ms), and compression trades that for decompression (≈+50 ms of CPU).
- These numbers are the runtime only. A GUI framework on top (WPF: D3D9+XAML; WinUI 3: WinRT+XAML+D3D11; Avalonia: Skia) adds hundreds of ms — that is what the wpf/winui3/avalonia prototypes measure. The official WinUI guidance ([MS Learn, 2026-03](https://learn.microsoft.com/en-us/windows/apps/develop/performance/app-startup-performance)): "≈1 ms per XAML element", defer everything not needed for the first frame, `x:Load`.

### 7.3 Windows App SDK bootstrap [measured]
An unpackaged app with a framework-dependent WinAppSDK 1.8 (`Microsoft.WindowsAppRuntime.Bootstrap.dll` from the winui3 prototype): `LoadLibrary` ≈1 ms, **`MddBootstrapInitialize2(1.8)` ≈3 ms** (on Windows 11 it goes through the OS dynamic dependencies API; no DDLM helper process appeared), `RoInitialize` ≈10–14 ms, the first activation factory of a WinAppSDK class ≈4.5 ms. **The bootstrap is not the problem**; self-contained WinAppSDK removes it altogether. In WinAppSDK 2.0 an unnecessary `version.dll` dependency was removed from WinUI's startup path ([release notes 2.0](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0)); runtimes 1.4–1.8 and 2.x are installed on the machine.

### 7.4 Packaged (MSIX) apps
This can't be checked without admin rights: Developer Mode is off (`AllowDevelopmentWithoutDevLicense=0`) → neither `Add-AppxPackage -Register` nor a sparse package without a trusted certificate. From the documentation ([MSIX behind the scenes](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-behind-the-scenes)): a full-trust packaged app gets AppData and HKCU virtualization (copy-on-write into the package's private store), and activation goes through the AppX infrastructure. An indirect **[measured]** cost of virtualization is in §11.1: file operations in AppData from a process "inside" the Claude package are 1.3–2.4× slower. For FastMD, packaging brings nothing in speed and carries overhead — only worth it if package identity is needed (Store, some APIs).

---

## 8. Double-click in Explorer: the request path and how to avoid creating a process at all

### 8.1 The path
Explorer (`CDefView` → `IContextMenu::InvokeCommand` → `ShellExecuteEx`): extension → `UserChoice` (Hash-protected; for http/https/.pdf also the UCPD driver — [kolbi.cz 2024](https://kolbi.cz/blog/2024/04/03/userchoice-protection-driver-ucpd-sys/), [2025](https://kolbi.cz/blog/2025/07/15/ucpd-sys-userchoice-protection-driver-part-2/)) or ProgID → `shell\open\command`, where three mechanisms are possible:
1. **`command` (a command line)** → `CreateProcess` from explorer.exe (+ AppCompat, Windhawk in explorer, Defender).
2. **`DelegateExecute` = the CLSID** of an `IExecuteCommand` + `IObjectWithSelection` object ([docs](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexecutecommand), [Raymond Chen, 2010](https://devblogs.microsoft.com/oldnewthing/20100312-01/?p=14623), [MS sample](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/winui/shell/appshellintegration/ExecuteCommandVerb/ExecuteCommandVerb.cpp)) — supports **out-of-proc (LocalServer32)**. If the class object is already registered by a running process (`CoRegisterClassObject(..., REGCLS_MULTIPLEUSE)`), COM connects Explorer to it — **no new process is created at all**; if not, COM (DcomLaunch) starts `server.exe -Embedding`.
3. **DDE (`ddeexec`)** — an obsolete broadcast, "please feel free to stop using DDE" ([Raymond Chen, 2007](https://devblogs.microsoft.com/oldnewthing/20070226-00/?p=27863)). Don't use it.

The user's `.md` association can't be changed programmatically: in Windows 10/11 the user picks the default app (the "Open with" dialog); the app registers a ProgID + `OpenWithProgids` (HKCU, no admin).

### 8.2 [measured] All paths through a real `ShellExecute` inside explorer.exe
Test extensions `.fmdtest*` (HKCU, installed/removed through the explorer context — see §11.1; after the experiment everything was removed and verified with `reg query` from explorer). `t0` is before the RPC call to `IShellDispatch2::ShellExecute` in explorer. The viewer is a trivial GDI window `srv.exe` (156 KB, `/MT`, without `ImmDisableIME`). 10 launches (2 interleaved blocks of 5), moderate machine load, median (min); raw data: `results/handoff.jsonl`:

| path | request reached the app | window shown | frame on screen (DwmFlush) | new process |
|---|---|---|---|---|
| classic: association → new `srv.exe` | 59.4 (55.0) | 90.8 | **98.3** (92.3) | yes (srv.exe) |
| DelegateExecute, server not running → COM starts `srv.exe -Embedding` | 50.1 (46.7) | 95.6 | 104.3 (96.8) | yes (srv.exe via DcomLaunch) |
| tiny `handoff.exe` (5 KB, no CRT) → `WM_COPYDATA` → resident | 59.0 (55.2) | 77.3 | 82.6 (79.6) | yes (handoff.exe) |
| **DelegateExecute → running resident** | **28.2** (24.3) | 49.6 | **57.0** (46.4) | **no** (process snapshot: no srv/handoff/dllhost) |
| … + window created in advance (hidden) | 26.9 | 32.6 | **38.0** (32.4) | no |
| … + window shown in advance and "cloaked" with `DWMWA_CLOAK` | 24.9 | 25.5 | **32.1** (25.8) | no |
| reference: `CreateProcess` from a test process (no shell) | 32.4 | 69.9 | 76.1 | yes |

- Explorer→association→`CreateProcess` adds ≈27 ms to a "bare" CreateProcess (59 vs 32 ms to `main`).
- **DelegateExecute to a live process is the only way around the process "floor"**: ≈25 ms from the call in Explorer to `Execute()` in our process, ≈32 ms to a finished frame with a pre-created "cloaked" window. For a trivial viewer the gain is 98→32 ms; for an engine with heavy initialization (WebView2, WinUI), hundreds of ms.
- Cold DelegateExecute (server not running) is barely worse than the classic path (+6 ms) — so a single registration can serve both modes: "resident is there → instant; not there → COM will start it".
- A hand-off launcher `handoff.exe` gives almost nothing for a lightweight app: creating a process (even a 5 KB one) is itself the main cost.
- Foreground: a process launched by the user has the right to `SetForegroundWindow`; a resident gets it handed over via `AllowSetForegroundWindow(pid)` (in `handoff.exe`) or `CoAllowSetForegroundWindow` ([docs](https://learn.microsoft.com/en-us/windows/win32/api/objbase/nf-objbase-coallowsetforegroundwindow), [AllowSetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-allowsetforegroundwindow)). On my rig (a background launch source) activation can't be checked properly — **an implementation risk**; check it manually with a double click.

### 8.3 Explorer preview handler for .md
An `IPreviewHandler` always lives outside the Explorer process: an in-proc server in the `prevhost.exe` surrogate (or its own local server); per-user registration in HKCU is possible ([Preview Handlers](https://learn.microsoft.com/en-us/windows/win32/shell/preview-handlers), [registration](https://learn.microsoft.com/en-us/windows/win32/shell/how-to-register-a-preview-handler)). PowerToys already ships a Markdown preview on WebView2 ([previewpane](https://github.com/microsoft/PowerToys/tree/main/src/modules/previewpane)) with known WebView2 lifecycle problems in `prevhost` ([#30828](https://github.com/microsoft/PowerToys/issues/30828)). For FastMD this is a separate "showcase" (a quick look in the pane) that can be built on the same native engine (a DLL, D2D/DWrite in someone else's HWND) — but it isn't the double-click path. Not a v1 priority.

---

## 9. Resident strategies: precedents and risks

- **Precedents from Microsoft:** Office "Startup Boost" — the Task Scheduler tasks "Office Startup Boost"/"…Logon" that preload Word (requirements: ≥8 GB of RAM, disabled in energy saver mode; rolled out in 2025) ([MS Learn](https://learn.microsoft.com/en-us/troubleshoot/microsoft-365/admin/miscellaneous/new-startup-boost-tasks-windows-task-scheduler), [The Register](https://www.theregister.com/2025/05/01/microsoft_will_preload_office_apps/)); File Explorer preloading in Windows 11 (Insider 26220.7271, November 2025, the "Enable window preloading for faster launch times" checkbox) ([Windows Central](https://www.windowscentral.com/microsoft/windows-11/windows-11-may-soon-preload-file-explorer-in-the-background-in-an-attempt-to-speed-it-up)) — and an independent test showed that even with preloading Explorer is slower than on Windows 10 and uses extra memory ([Windows Latest](https://www.windowslatest.com/2025/11/28/tested-windows-11s-faster-file-explorer-preloaded-is-still-slower-than-windows-10-and-uses-additional-ram/)).
- **Options for FastMD:** (a) a resident in the tray/windowless, started via `HKCU\...\Run` or an "at logon" task; (b) a resident + `DelegateExecute` (no new process); (c) a resident + a thin launcher (named pipe/`WM_COPYDATA`) — only if (b) is impossible; (d) a pre-created hidden/"cloaked" window with ready D2D/DWrite resources (−20…−25 ms on top of (b)).
- **Risks:** constant memory use (for a native D2D process, a few to tens of MB; for WebView2, 100+ MB and several processes), the working set being trimmed after a long idle period (hard page faults on the first open), background throttling/EcoQoS for windowless processes (disabled via `SetProcessInformation(ProcessPowerThrottling)`), users perceiving "yet another background process", updating the binary while the resident is running, correct foreground hand-off, the bench protocol's constraints (`-resident` variants are measured separately, §6 of PROTOCOL.md).

---

## 10. ETW/WPR: how to profile startup on this machine

- **Available without admin** (the user is in the "Performance Log Users" group): `logman start <s> -p <provider> -ets` for user-mode providers — `Microsoft-Windows-Win32k`, `Microsoft-Windows-DxgKrnl`, `Microsoft-Windows-Dwm-Core`, `Microsoft-Windows-DotNETRuntime`, **`Microsoft-Antimalware-Engine`/`-RTP`** (that's how the attribution in §2.1 was obtained); reading: `Get-WinEvent -Path x.etl`.
- **Requires admin [measured]:** `wpr -start CPU` → `0xc5585011`, `xperf -on PROC_THREAD+LOADER` → "Access is denied", `Microsoft-Windows-Kernel-Process` and `Microsoft-Windows-CodeIntegrity` — Access denied. So CPU stack sampling, ReadyThread, Image Load and Disk I/O are only possible in an elevated session.
- **Recommended profile for the orchestrator (in an admin console):** `wpr -start GeneralProfile -start CPU -start DiskIO -start FileIO -filemode` → launch the prototype → `wpr -stop start.etl`; in WPA: *CPU Usage (Precise)* with the Ready Time column, *Images* (DLL load times), *Process Lifetimes*, *Generic Events* from the providers above; your own markers as TraceLogging events in the code (`TraceLoggingWrite`) and a *Regions of Interest* file for the "main→window→first frame" phases ([WPR/WPA](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-recorder), [Regions of Interest](https://needleinathreadstack.wordpress.com/2014/04/08/writing-a-wpr-regions-of-interest-file/)); a ready-made ETW-based tool for attributing process startup: [launchlab](https://github.com/SP42K/launchlab). For the "present → screen" latency: PresentMon (also requires the Performance Log Users group/admin) or my `dup.exe`.
- **Without ETW**, what I used works well: marks in shared memory, `QueryProcessCycleTime`/`GetThreadTimes` around a call, tracing messages in `WndProc`, per-step module diffs, Desktop Duplication for pixels.

---

## 11. Notes on the bench and the protocol (important for the final run)

### 11.1 MSIX virtualization for everything launched from Claude Desktop [measured]
Claude Desktop is an MSIX package; the session's child processes (for them `GetCurrentPackageFullName` = no package) nevertheless get **virtualization of writes to `%LOCALAPPDATA%`/`%APPDATA%` and `HKCU`**: a write to `HKCU\Software\...` and a file in `%LOCALAPPDATA%` are visible from the session but **not visible** from the explorer.exe context (`reg query`/`if exist` via `launch.exe --mode explorer`). The data lives in `…\Packages\Claude_pzs8sxrjxfjjc\LocalCache\Local` and `SystemAppData\Helium\UserClasses.dat`. The profiles `FastMD\WebView2-cpp-webview2-*`, `FastMD\tauri`, `FastMD\wry`, `AvaloniaUI`… are already there.
- Cost **[measured]** (400 files of 4 KB in `%LOCALAPPDATA%`, 3 repeats): read **50 vs 21 ms (×2.3)**, create+write 140 vs 112 ms (×1.25), stat+enumerate 110 vs 79 ms (×1.4), delete 121 vs 70 ms (×1.7) — "Claude session" vs "explorer context".
- Consequences: (1) the web prototypes (WebView2/Tauri/wry/Electron), which write their profile heavily at startup, are somewhat slower on the bench than for the user; (2) any HKCU registrations made from the session (COM, associations) are invisible to the system — which is exactly why my first DelegateExecute experiment gave `REGDB_E_CLASSNOTREG`, even though `CoCreateInstance` to an already running server worked.
- **Recommendation:** run the final serial `bench.py` run outside Claude's process tree — from the user's ordinary terminal, via Task Scheduler, or through explorer: `research/lab/startup/bin/launch.exe --mode explorer --timeout 3600000 -- C:\path\run_bench.cmd` (explorer executes the `.cmd` in its own, non-virtualized context). At least once, compare the results "from the session" and "outside".

### 11.2 Other
- **Windhawk** (§1.2): a constant overhead of ≈5 ms of CPU per process (under load, also ≈5 ms before `main`) and shell32/COM loaded into every process; it's the same in nature for all prototypes, but it distorts the "floor". Worth telling the user; if desired, they can exclude FastMD processes in the mod/engine settings (I didn't touch it).
- **TextInputHost** on this machine is anomalous (35 CPU hours) → rare multi-second `ShowWindow` calls in every prototype with an activated window. Before the final measurement, restart `TextInputHost.exe` (user) or sign out/in.
- **`first_ms`** in the `bench.py` report = the first launch after a build; it includes Defender's initial scan (+20…+150 ms) — not to be confused with an "OS cold start".
- **`launch_overhead_ms`** (CreateProcess+Job+Resume) honestly includes Defender's EXE-size tax (§2.2) — it's a real cost for the user and should stay, but it should be shown in a separate column (it explains "why Electron is like that").
- **t_content doesn't see the DWM animation** (§6): for a perceptual comparison, you can additionally measure "window fully visible" with `dup.exe` (it accepts any command line and rectangle).
- A minor point: in `run_once` the Python observer thread polls `EnumWindows` every 1 ms while holding the GIL — on 32 threads this doesn't affect the child, but `observed_ms` has an error of ~1–2 ms.

---

## 12. Conclusions for FastMD

### 12.1 Ranked list of techniques (estimates are for this machine; on a "clean" machine the absolute numbers are smaller, the ratios the same)

| rank | technique | expected gain | cost/risk |
|---|---|---|---|
| 1 | **Don't create a hardware D3D device before the first frame** (WARP-D2D, DWrite→DIB or GDI; HW lazily/in the background/never) | **−210 ms [quiet]** (up to −300 under load) on a PC with NVIDIA: device ≈205 + first Present ≈18; process CPU ×4 | WARP loads the CPU when scrolling large documents — acceptable for text; if needed, switch to HW after the first frame |
| 2 | **A small EXE** (<≈2–5 MB), heavy parts in DLLs; no single-file .NET/no Electron | **−1.2…−1.9 ms per MB** on every launch (150 MB → −300 ms) | — |
| 3 | **`DelegateExecute` (IExecuteCommand) + a resident process** with a pre-created/"cloaked" window | Explorer→frame path **98 → 32…57 ms** for a lightweight viewer; for a heavy engine, hundreds of ms | a persistent process/memory, the installer registers COM+ProgID, foreground hand-off, the bench's "-resident" mode |
| 4 | **`ImmDisableIME(-1)`** or showing without activation and activating after the first frame | **−12…−18 ms [quiet]** (−25…−35 under load) and protection from multi-second TSF hangs | no IME on the thread (CJK input in search) — or no-activate-first |
| 5 | **`DWMWA_TRANSITIONS_FORCEDISABLED`** | the window is fully visible **40–60 ms** earlier (perceptually) | no open animation (a plus for a "bullet") |
| 6 | **Viewport-first**: layout/render only the first screen, the rest after Present | depends on the document: large.md (3.7 MB), hundreds of ms; emoji/CJK/tables beyond the screen cost nothing | scrolling/search are harder until background layout finishes |
| 7 | **Emoji:** our own raster cache, monochrome in the first frame when there are many unique emoji | 22–37 ms [quiet] (up to 90 under load) on the first screen of medium.md (5 unique emoji) | a slight flicker on "coloring in" |
| 8 | **No COM/WinRT on the critical path** (`CoInitializeEx`/`RoInitialize`) | −11…−16 ms each [quiet] | WebView2/WinUI are impossible without COM → an argument against them |
| 9 | **GUI subsystem**, static CRT, no .NET host (or NativeAOT) | console: −0.5…−1.6 s; CLR: −11…−15 ms vs NativeAOT | — |
| 10 | Code signing and file "reputation" | a small known EXE: 4–6 ms vs 18–24 ms for an unknown one | depends on how widespread it is; can't be had right away |
| 11 | Don't package as MSIX for speed | AppData/HKCU virtualization: I/O ×1.3–2.4 | only if package identity is needed |

### 12.2 The "bullet" budget (native variant, from my measurements [quiet])
`CreateProcessW` ≈21 → `main` ≈31 → `CreateWindowEx` (+noime) ≈13 → `ShowWindow` without TSF ≈3–6 → DirectWrite: factory+format+layout of the first screen ≈3–5 → (a) DIB path: drawing ≈2 + BitBlt, or (b) WARP device ≈15 + swap chain ≈3 + D2D drawing ≈1.5 (+≈22 with 5 color emoji) → `DwmFlush` ≈5–8 ⇒ **≈65–80 ms to the frame** (measured combinations: noime+popup GDI, 58 ms; DWrite→DIB, 82 ms; and WARP-D2D, 100 ms with an ordinary `ShowWindow` ≈16 ms), without the DWM animation. Under load ×1.3–1.5. A double click adds ≈25–30 ms of Explorer work (§8.2). Through `DelegateExecute` to a resident, **≈30–40 ms** from the call in Explorer. For comparison: the bench's calibration floor (`baseline-win32`) of ≈90 ms includes TSF (≈12–30 ms) and Windhawk.

### 12.3 Risks
- The main series were taken at 80–90 % CPU load, the [quiet] repeats at 13–39 %; they confirm the conclusions and orders of magnitude, but the final comparisons are up to the orchestrator's serial run (preferably outside Claude's process tree, §11.1). The Explorer hand-off experiments (§8.2) and the "photon" ones (§6) were done under load only.
- Attributing the EXE-size tax to Defender rests on the engine's ETW events and the parent's kernel CPU; without an admin kernel trace, the exact driver (WdFilter vs CI) isn't confirmed. On machines with a different AV/EDR the numbers will differ — what matters to me here is the conclusion "small EXE", which doesn't depend on the AV.
- The cost of the NVIDIA device depends on the driver; on Optimus laptops the default device is the iGPU (Intel/AMD), with its own numbers (the AMD iGPU here: 65–84 ms + a 250 ms cross-adapter swap chain if the monitor is on the dGPU).
- `ImmDisableIME` changes input behavior; a "cloaked" pre-created window and a resident require careful handling of the foreground and Alt-Tab.
- COLRv1 emoji and `IDWriteBitmapRenderTarget3` depend on the Windows build (on 26200 the interface isn't available via QI).

---

## Sources
- Windhawk: [Implementing Global Injection and Hooking in Windows (m417z, 2022)](https://m417z.com/Implementing-Global-Injection-and-Hooking-in-Windows/); [Injection targets (wiki)](https://github.com/ramensoftware/windhawk/wiki/Injection-targets-and-critical-system-processes)
- Defender: [Set-MpPreference (EnableFileHashComputation etc.)](https://learn.microsoft.com/en-us/powershell/module/defender/set-mppreference); [Windows 11 + Device Guard slows down CreateProcess (Q&A)](https://learn.microsoft.com/en-us/answers/questions/5862177/windows-11-device-guard-slows-down-createprocess-p)
- DWM/windows: [DWMWINDOWATTRIBUTE](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute); [ImmDisableIME](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immdisableime)
- Emoji: [Bringing new emoji to Windows 11 (Microsoft Design)](https://microsoft.design/articles/bringing-new-emoji-to-windows-11/); [Segoe UI Emoji](https://learn.microsoft.com/en-us/typography/font-list/segoe-ui-emoji)
- .NET: [native-hosting.md](https://github.com/dotnet/runtime/blob/main/docs/design/features/native-hosting.md); [Startup sequence of a .NET Core app](https://mihai-albert.com/2020/03/08/startup-sequence-of-a-dotnet-core-app/); [Native AOT deployment](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/); [JIT vs R2R vs AOT benchmarks (dev.to)](https://dev.to/lzocate-li/net-native-aot-parte-2-jit-r2r-e-aot-em-benchmarks-367i)
- WinUI/WinAppSDK: [WinUI app startup performance (2026-03)](https://learn.microsoft.com/en-us/windows/apps/develop/performance/app-startup-performance); [MddBootstrapInitialize2](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/win32/mddbootstrap/nf-mddbootstrap-mddbootstrapinitialize2); [Windows App SDK 2.0 release notes](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0); [Single-instance WinUI (AppInstance)](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/applifecycle/applifecycle-single-instance)
- MSIX: [Understanding how packaged desktop apps run on Windows](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-behind-the-scenes)
- Shell: [IExecuteCommand](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexecutecommand); [Simplifying context menu extensions with IExecuteCommand (Old New Thing)](https://devblogs.microsoft.com/oldnewthing/20100312-01/?p=14623); [ExecuteCommandVerb sample](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/winui/shell/appshellintegration/ExecuteCommandVerb/ExecuteCommandVerb.cpp); [How to launch an unelevated process… (IShellDispatch2 in explorer)](https://devblogs.microsoft.com/oldnewthing/20131118-00/?p=2643); [Please feel free to stop using DDE](https://devblogs.microsoft.com/oldnewthing/20070226-00/?p=27863); [CoAllowSetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/objbase/nf-objbase-coallowsetforegroundwindow); [AllowSetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-allowsetforegroundwindow); [Merged view of HKCR / per-user COM](https://learn.microsoft.com/en-us/windows/win32/sysinfo/merged-view-of-hkey-classes-root); UCPD: [kolbi.cz 2024](https://kolbi.cz/blog/2024/04/03/userchoice-protection-driver-ucpd-sys/), [2025](https://kolbi.cz/blog/2025/07/15/ucpd-sys-userchoice-protection-driver-part-2/)
- Preview handlers: [Preview Handlers and Shell Preview Host](https://learn.microsoft.com/en-us/windows/win32/shell/preview-handlers); [How to Register a Preview Handler](https://learn.microsoft.com/en-us/windows/win32/shell/how-to-register-a-preview-handler); [PowerToys previewpane](https://github.com/microsoft/PowerToys/tree/main/src/modules/previewpane)
- Residents/preloading: [Office Startup Boost tasks](https://learn.microsoft.com/en-us/troubleshoot/microsoft-365/admin/miscellaneous/new-startup-boost-tasks-windows-task-scheduler); [The Register 2025-05-01](https://www.theregister.com/2025/05/01/microsoft_will_preload_office_apps/); [Explorer preload (Windows Central)](https://www.windowscentral.com/microsoft/windows-11/windows-11-may-soon-preload-file-explorer-in-the-background-in-an-attempt-to-speed-it-up); [Explorer preload tested (Windows Latest)](https://www.windowslatest.com/2025/11/28/tested-windows-11s-faster-file-explorer-preloaded-is-still-slower-than-windows-10-and-uses-additional-ram/)
- ETW: [Windows Performance Recorder](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-recorder); [WPR start/stop](https://devblogs.microsoft.com/performance-diagnostics/wpr-start-and-stop-commands/); [Regions of Interest](https://needleinathreadstack.wordpress.com/2014/04/08/writing-a-wpr-regions-of-interest-file/); [launchlab](https://github.com/SP42K/launchlab)
