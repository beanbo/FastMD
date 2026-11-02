[Русский](03-gpu-and-crossplatform-native-ui.md) | **English**

# 03. GPU / immediate-mode / cross-platform native UI toolkits and their text

*FastMD, research phase. Date: 2026-09-19. Author: the "GPU & cross-platform native UI" research agent.*
*Lab with the sources of every measurement: [`research/lab/gpu-ui/`](lab/gpu-ui/README.md).*

> **How to read this.** Everything marked **[measured]** is my own measurement on this machine (the raw JSON is in
> `research/lab/gpu-ui/**/results-*.json`). Everything else cites a source (with version and date).
> The measurements were taken while other agents' prototypes were being built in parallel, so the absolute numbers
> drift within ±2×. Only rows of **the same series** can be compared reliably (they were run interleaved, round-robin).
> The final comparative numbers are up to the orchestrator, on an idle machine.

---

## 0. TL;DR

1. **The main brake on GPU toolkits on this machine is not the toolkit itself but the first hardware GPU device in the process.**
   **[measured]** `D3D11CreateDevice` on an RTX 5070 (driver 32.0.16.1656) costs **200–250 ms**, D3D12 **~270 + 45 ms**,
   Vulkan instance+device **~280 ms**, WGL (OpenGL) **~330 ms**, D3D9Ex (WPF) **~210 ms**. For comparison:
   the whole Win32+GDI baseline up to content is ≈ 90 ms, and **WARP** (software D3D11) is created in **15–22 ms**.
   This is the fixed price of the NVIDIA user-mode driver; others see it independently too (RTX 3090: 160 ms HW vs 3.6 ms WARP,
   [nene-nib #24, 2026-09-16](https://github.com/hideyukiMORI/nene-nib/issues/24)).
2. **The iGPU does not help**: a device on the AMD iGPU is created in 42–55 ms, but `CreateSwapChainForHwnd` for a window on a monitor
   connected to the NVIDIA card loads the NVIDIA driver anyway (+210–240 ms). Creating the device in the background in parallel with the window
   saves only ~50–60 ms (the window is created faster than the driver loads).
3. **Direct2D + DirectWrite on WARP gives a first frame with text (ClearType, color emoji, CJK) in ~100–135 ms
   from `main`** versus 320–410 ms on a hardware device — **[measured]**; HW and WARP pixels match (except 161 pixels
   in a color emoji). Steady-state text scrolling on WARP is no slower than on HW (25–30 ms/frame in both cases on a
   deliberately "dumb" test without culling) — WARP is enough for a document.
4. **winit** (the windowing layer of egui, iced, Slint, Xilem, Floem, Freya, Blitz, Bevy) **adds 150–250 ms**: on the first
   `WM_SETFOCUS` it builds a full keyboard layout table (16 modifier states × 256 VKs × `ToUnicodeEx`).
   **[measured]** instrumented winit 0.30.13: `prepare_layout` = 158–202 ms; a C reproduction of 6800 calls to
   `MapVirtualKeyExW/ToUnicodeEx` = 159–201 ms. The logic is the same in winit 0.31-beta.3.
5. **wgpu adds its own tax on multi-adapter machines**: on `request_adapter` the DX12 backend creates a D3D12 device
   on **every** adapter (NVIDIA, AMD, WARP) → 350–530 ms, even with `force_fallback_adapter` (WARP). The egui-wgpu 0.36
   default is `PRIMARY | GL` + `HighPerformance` (it initializes Vulkan, DX12 and GL). **[measured]** a minimal
   winit+wgpu window with a single fill: **680–990 ms from `main`** to the first Present.
6. **System fonts**: stacks built on `fontdb`/cosmic-text (iced, Floem, glyphon) scan all 474 faces
   (449 MB in `C:\Windows\Fonts`) — **30–90 ms**, plus first shaping with fallback **25–75 ms**. Stacks on DirectWrite
   (fontique/Parley, Slint ≥1.16, gpui, Qt ≥6.8, Skia) — **1–3 ms**. egui scans nothing, but it cannot use system
   fonts at all either (CJK and color emoji only by hand / not supported).
7. **Text quality**: ClearType (subpixel AA) comes only from native DirectWrite/D2D, Qt Widgets, WPF, and from the
   GPU world from Zed/gpui since January 2026 (the crates.io version gpui 0.2.2 is grayscale). egui, iced, Slint, Makepad, Vello,
   Flutter are grayscale. At 100% scale the difference is visible to the naked eye (screenshots in §2.7).
8. **Whole toolkits measured [measured]**: Slint 1.18 (winit + software renderer) — **0.26–0.69 s** to the frame,
   Slint (winit + femtovg/OpenGL) — **0.73–1.48 s**, gpui 0.2.2 — **0.59–0.63 s**; minimal winit+wgpu — 0.74–1.05 s.
   None of them comes close to the 90 ms baseline, or even to the ~150 ms of "D2D on WARP".
9. **Conclusion for FastMD**: GPU / immediate-mode / cross-platform toolkits **are not fit to be the "bullet"** on machines
   with a discrete NVIDIA card without a deep rework of their initialization. The best path is **Win32 + DirectWrite + Direct2D, first
   frame on WARP (or D2D software), hardware device lazily or never**. If cross-platform support is ever needed,
   look at **Qt 6 Widgets** (CPU rasterization, no winit, DirectWrite + ClearType, md4c/GFM built in) — its
   prototype is being done by the `qt` agent; the other candidates structurally pay the "GPU tax" and/or the "winit tax".

---

## 1. Methodology and test bench

### 1.1 Machine (verified)

| Parameter | Value |
|---|---|
| OS | Windows 11 Pro 26200, not elevated |
| CPU/RAM | Ryzen 9 7950X (16C/32T), 64 GB |
| GPU | NVIDIA GeForce RTX 5070, driver **32.0.16.1656** (20.08.2026, UMD `nvwgf2umx.dll` 90.9 MB) + AMD Radeon iGPU, driver 32.0.21045.1000; adapter 0 (monitor) is NVIDIA |
| Injections into processes | **Windhawk** (`windhawk.dll` + mod DLLs in every process), **RTSS** (`RTSSHooks64.dll` shows up in processes with a D3D window), NVIDIA capture `nvspcap64.dll` in D3D processes; Vulkan implicit layers: RTSS, Steam overlay, Steam Fossilize |
| Keyboard layouts | ru-RU (0419) + en-US (0409) |
| Fonts | `C:\Windows\Fonts`: 645 files, 449 MB; DirectWrite: 218 families; fontdb: 474 faces |

### 1.2 How I measured

* **One scenario, one process** (DLLs and driver always "cold in the process", as on a double click), round-robin,
  5–7 runs each, median. The timer is `QueryPerformanceCounter` inside the process; `launch→main` is from
  `GetSystemTimePreciseAsFileTime` before `CreateProcess` in the Python driver to the first line of `main`.
* "First frame" is after `Present`/`EndDraw` **+ `DwmFlush()`**, as in [`bench/PROTOCOL.md`](../bench/PROTOCOL.md) §3.
  For Slint (the software renderer has no callback) the mark = the first `WindowEvent::RedrawRequested` → a zero-delay `slint::Timer`
  (fires on the next turn of the loop, i.e. after drawing) → `DwmFlush`. For gpui it is `Window::on_next_frame`
  (the protocol allows "the start of the next frame").
* Tools: MSVC 14.44 (`/O2 /utf-8`), Rust 1.98.1 (`--release`, thin LTO), Python 3.13 + Pillow for screenshots
  (`PrintWindow(PW_RENDERFULLCONTENT)`).
* Versions: wgpu **30.0.1**, winit **0.30.13**, fontdb **0.24.0**, cosmic-text **0.19.0**, fontique **0.11.1**,
  Slint **1.18.0**, gpui **0.2.2** (crates.io).

---

## 2. Hands-on measurements

### 2.1 Cost of initializing graphics APIs (no window) — [measured]

`gfxinit.exe <mode>`; "series A" at 00:38 (moderate load), "series B" at 01:14 (final, 7 runs).
`launch→main` for this C exe: 40–50 ms in all modes.

| Operation (in process, cold) | Series A, median, ms | Series B, median (min), ms |
|---|---:|---:|
| `CreateDXGIFactory` + enumerating 3 adapters | 13.7 | 16.3 (12.8) |
| `D3D11CreateDevice` **WARP** | 15.8 | 21.7 (15.4) |
| `D3D11CreateDevice` AMD iGPU (`MINIMUM_POWER`) | 41.7 (+13 factory) | 48.0 (+13) |
| `D3D11CreateDevice` **NVIDIA** (default adapter) | 236.1 | 243.4 (199.5) |
| …a second D3D11 device in the same process | 32.3 | 33.7 |
| …of which `LoadLibrary(nvwgf2umx.dll)` on its own | — | 42 (after that the device still takes 206) |
| `D3D12CreateDevice` + queue, AMD | 72.0 + 4.1 | 94.4 + 4.4 |
| `D3D12CreateDevice` + queue, NVIDIA | 268.9 + 71.6 | 266.7 + 45.7 |
| D3D9Ex: `Direct3DCreate9Ex` + `CreateDeviceEx(HAL)` (what WPF does) | — | 127.4 + 84.8 |
| Vulkan: `vkCreateInstance` (NVIDIA+AMD ICDs, RTSS layer) | 162.7 | 209.4 |
| Vulkan: `vkCreateDevice` (NVIDIA) | 133.9 | 65.9 |
| WGL: `ChoosePixelFormat` + `wglMakeCurrent` (NVIDIA ICD) | 191.2 + 96.1 | 233.1 + 93.0 |
| **DirectWrite**: shared factory + system collection + layout with fallback (Cyrillic+CJK+emoji) | 0.5 + 0.7 + 1.0 | 0.8 + 0.7 + 1.2 |

What gets loaded on `D3D11CreateDevice` on NVIDIA (**[measured]**, `EnumProcessModules` before/after, 38 → 66 modules):
`nvldumdx.dll` (the loader, checks signatures: `wintrust`, `crypt32`, `imagehlp`), `nvgpucomp64.dll` (compiler),
`NvMemMapStoragex.dll`, `nvwgf2umx.dll`, `nvspcap64.dll` (NVIDIA capture/overlay), `nvppex.dll`, `nvapi64.dll`.

Vulkan with loader filters (**[measured]**, `run_env.py`, under load): disabling the implicit layers
(`VK_LOADER_LAYERS_DISABLE=~implicit~`) **gave no gain**; AMD ICD only — instance 150 ms; NVIDIA only — 370–410 ms
(under load). So it is the NVIDIA driver itself that is expensive, not the RTSS/Steam layers.

### 2.2 Window + first frame — [measured]

From the first line of `main` to "frame presented + `DwmFlush`". Add ~45–50 ms for `launch→main`.

| Scenario | Series A, ms | Series B, median (min), ms | Where the time goes |
|---|---:|---:|---|
| **GDI**: `CreateWindow` + `ShowWindow` + `WM_PAINT` + `DwmFlush` | 50.6 | 75.4 (54.7) | window 20 + show 30–42 |
| D3D11 **WARP** swapchain (flip), fill | 79.3 | 99.3 (90.1) | device 17, swapchain 4.6, present 6.8 |
| D3D11 **HW NVIDIA** swapchain, fill | 305.4 | 322.6 (305.8) | device 231, present 22 |
| D3D11 on the **iGPU**, window on the NVIDIA monitor | 386.0 | 402.5 (325.4) | device 55, **swapchain 211** (cross-adapter) |
| D3D11 HW, device on a **background thread** in parallel with the window | 280.4 | 266.4 (252.8) | waiting for the thread another 174 ms after the window is shown |
| **D2D + DirectWrite text, WARP** swapchain | — | **106.0 (97.3)** | device 13.6, DrawTextLayout 18.5, present 6.1 |
| D2D + DirectWrite text, HW swapchain | — | 409.6 (315.6) | device 259, draw 34, present 22 |
| D2D `ID2D1HwndRenderTarget` by default (= HW) | — | 317.5 (281.9) | RT creation 217 |
| D2D `ID2D1HwndRenderTarget` `D2D1_RENDER_TARGET_TYPE_SOFTWARE` | — | 134.7 (106.9) | RT 19.5, draw+present 33 |

**HW vs WARP quality** (**[measured]**, `c-gfxinit/shot.py`): in D2D on a flip swapchain with `D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE`,
HW and WARP both give **ClearType** (11,638 vs 11,640 "colored" fringe pixels); the per-pixel difference is
**161 pixels**, only in the area of the color emoji 🚀🎉.

**Steady-state scrolling** (`d2dscroll_*`, 120 frames, `Present(0,0)`, 24 paragraphs / 2414 px drawn in full every
frame without culling — a deliberately worst case): WARP **25.9–30.3 ms/frame**, HW **24.6–30.6 ms/frame**. The time goes to the CPU
(DirectWrite glyph runs), not to rasterization — with normal culling of invisible blocks both variants fit into 60 fps.

### 2.3 wgpu 30.0.1 by backend — [measured]

Headless (no window), 5 runs, series B. "pipeline" is a single egui-like render pipeline (WGSL → backend).

| Backend / power preference | `Instance::new` | `request_adapter` | `request_device` | pipeline | Total from `main` | Adapter chosen |
|---|---:|---:|---:|---:|---:|---|
| DX12 / High | 21.8 | **378.5** | 99.0 | 7.2 | 510 | Dx12: RTX 5070 |
| DX12 / Low | 24.0 | **426.1** | 92.6 | 9.6 | 532 | Dx12: AMD |
| DX12 / `force_fallback_adapter` | 21–28 | **462–534** | 23–29 | 6.6–8.8 | 519–593 | Dx12: **WARP** |
| Vulkan / High | **217.3** | 12.2 | 68.6 | 0.9 | 299 | Vulkan: RTX 5070 |
| Vulkan / Low | **268.5** | 14.1 | 37.6 | 0.8 | 312 | Vulkan: AMD |
| GL (WGL) | **418.1** | 0.5 | 31.3 | 1.1 | 460 | Gl: RTX 5070 |
| PRIMARY (Vulkan+DX12) / None | 258.5 | 321.9 | 77.8 | 0.9 | 643 | Vulkan: RTX 5070 |
| all + `enumerate_adapters` | 457.4 | 280.6 + 356.1 | 80.3 | 1.4 | 1134 | (6 adapters) |

Why DX12 is so expensive even when WARP is chosen: in `dx12::Instance::enumerate_adapters`, `wgpu-hal` 30.0.1 calls
`Adapter::expose` for every DXGI adapter, and that creates a full `ID3D12Device` just for `CheckFeatureSupport`
(`wgpu-hal-30.0.1/src/dx12/instance.rs:166–175`, `adapter.rs:65–93`,
[docs.rs source](https://docs.rs/crate/wgpu-hal/30.0.1/source/src/dx12/adapter.rs)). On a machine with NVIDIA+AMD+WARP that is
~270 + ~90 + WARP ≈ 400 ms before anything is even chosen. FXC compilation of one simple pipeline on DX12 takes 7–10 ms versus ~1 ms on
Vulkan (DX12 uses FXC by default: `Dx12Compiler::Fxc`, "old, slow and unmaintained" — from the doc comment in
`wgpu-types-30.0.1/src/backend.rs`). A toolkit with 5–10 pipelines (iced) or dozens of compute shaders (Vello) pays a multiple of that.

With a window (winit 0.30.13 + wgpu 30, fill + one pipeline, 4 runs, series B), from `main` to the first Present+DwmFlush:

| Backend | Total | winit window | Instance | Adapter | Device | `surface.configure` | Present |
|---|---:|---:|---:|---:|---:|---:|---:|
| DX12 / High | **780** | 269 | 26 | 346 | 95 | 9 | 8.6 |
| Vulkan / High | **835** | 232 | 232 | 20 | 63 | **253** | 1.3 |
| GL (WGL) | **681** | 263 | 281 | 0.4 | 23 | 62.5 | 4.1 |
| DX12 / Low (AMD, monitor on NVIDIA) | **994** | 240 | 24 | 391 | 74 | **176** | 31 |

This is the **lower bound for any "winit + wgpu" toolkit** (egui/eframe, iced, Xilem/Masonry on Vello, Floem,
Dioxus Native/Blitz on Vello) on this machine — before loading fonts, shaping and laying out the document.

### 2.4 The winit tax — [measured]

`wgpu-probe --winonly` (only `EventLoop` + `create_window` + `DwmFlush`): `create_window` = **145–324 ms**
(median ≈ 245) versus ~50–75 ms on bare Win32 (§2.2). Disabling drag & drop (`with_drag_and_drop(false)`) has no effect.

An instrumented copy of winit 0.30.13 (`research/lab/gpu-ui/rust/vendor/winit`, `winit-instr/`):

```
winit-t            before_ShowWindow    31.29
winit-msg 0x0007 took   212.35 ms        <- WM_SETFOCUS (вложен в WM_ACTIVATE 0x0006)
winit-layout prepare_layout(hkl=0x4090409) took   202.21 ms
winit-t             after_ShowWindow   341.96
```

The cause: on `WM_SETFOCUS` winit synthesizes key presses (`KeyEventBuilder::synthesize_kbd_state`), and the first thing that does is
call `LAYOUT_CACHE.get_current_layout()` → `prepare_layout()`: for each of the 16 modifier states and
each of the 256 VKs — `MapVirtualKeyExW` + `ToUnicodeEx` (+ `vkey_to_non_char_key`)
([keyboard.rs](https://docs.rs/crate/winit/0.30.13/source/src/platform_impl/windows/keyboard.rs),
[keyboard_layout.rs](https://docs.rs/crate/winit/0.30.13/source/src/platform_impl/windows/keyboard_layout.rs)).
A C reproduction of the same loop (`gfxinit tounicode`, 6800 calls) takes **159–201 ms**, i.e. ~25–30 µs per
`ToUnicodeEx` call on this machine. The cache is per process and per HKL: the first ru↔en switch causes another such pause.
In winit **0.31.0-beta.3** (the `winit-win32` crate) `WM_SETFOCUS` handling is the same. I found no open issues about this in
rust-windowing/winit (searched with `gh search issues`); the closest is #4584 (a hang with the Punto Switcher hook on
layout switch, opened 2026-06-02, closed 2026-06-14).

Everything else winit does when creating a window is cheap (**[measured]** in C): `OleInitialize` 6.7 ms,
`CoCreateInstance(CLSID_TaskbarList)` + `HrInit` + `AddTab` 5.3 ms.

> The 25–30 µs per call may be inflated by the environment (Windhawk injects itself into every process). Structurally the problem
> remains: ~6800 system calls at startup for a table that is only needed at the first key press.
> In winit this is a ~10-line patch (build the table lazily / don't synthesize key presses if nothing is pressed).

### 2.5 System fonts — [measured]

`fontscan.exe <mode>`; series B (7 runs, machine less busy) and the range of medians from earlier series (under load).

| Operation | Series B, median | Earlier series | Who does it this way |
|---|---:|---:|---|
| `fontdb::Database::load_system_fonts()` (474 faces) | **32.4** | 66–89 | resvg, glyphon, cosmic-text, Floem, iced (via cosmic-text) |
| `cosmic_text::FontSystem::new()` (fontdb + locale) | **33.9** | 82–95 | iced 0.14 (cosmic-text 0.15), Floem, glyphon |
| cosmic-text: first `shape` of a "Latin+Cyrillic+CJK+emoji" line with fallback | **24.2** | 59–76 | same |
| `fontique::Collection::new(system_fonts: true)` (DirectWrite backend) | **1.2–1.4** | 2–12 | Parley (Xilem/Masonry, Blitz), Slint ≥1.16 |
| fontique: query for "Segoe UI Variable Text" + fallback for `Hani` | 0.7 | 1.1 | same |
| DirectWrite (C): system collection + `CreateTextLayout` with fallback | 0.7 + 1.2 | 0.7 + 1.0 | D2D/DWrite, gpui (Windows), Qt ≥6.8, Skia (Flutter, Freya) |

Conclusion: fontdb reads and parses the name tables of **all** font files (memory map), while DirectWrite hands over a ready-made
system cache (the FontCache service) — a 20–70× difference. On Windows fontique goes to DirectWrite
([`backend/dwrite.rs`](https://docs.rs/crate/fontique/0.11.1/source/src/backend/dwrite.rs)).

Color emoji: **[measured]** `seguiemj.ttf` on this machine is a **COLR v1** table (+ 3372 v0 base glyph records for
compatibility) and CPAL; no SVG/CBDT. A renderer without COLRv1 gets "flat" v0 emoji, and one without COLR at all gets monochrome/blank.
`msyh.ttc` (CJK) is 19 MB, `YuGothR.ttc` 13.5 MB: "just embed a CJK font" (as egui suggests) means tens of MB.

### 2.6 Whole toolkits — [measured]

`run_probe.py`, from `CreateProcess` to "frame + DwmFlush". Window 1000×800, a 30 px heading, a paragraph with
**bold**/*italic*/`code`, and the line «你好，世界。こんにちは 🚀🎉✅».

| Toolkit / mode | launch→frame, series B (min) | other series | main→frame | Breakdown |
|---|---:|---:|---:|---|
| **Slint 1.18**, `SLINT_BACKEND=winit-software` | **284 (228)** | 262–690 | 207 | component 18 ms, until the first `RedrawRequested` 178 (of which ~150–200 is winit/`WM_SETFOCUS`), render+present 10 |
| Slint 1.18, software, without the `accessibility` feature | 295 (261) | 262–378 | 226 | the effect of AccessKit is within noise (0–130 ms between series) |
| **Slint 1.18**, `winit-femtovg` (OpenGL/WGL) | **791 (729)** | 1095–1478 | 721 | + WGL/NVIDIA ≈ 300–450 ms + femtovg shaders |
| **gpui 0.2.2** (Win32 + D3D11 + DirectWrite) | **630 (591)** | 613 | 556 | until the `Application::run` callback **485 ms** (platform: OLE, 2× `D3D11CreateDevice`, DirectWrite, JumpList, VSync thread), window 55, frame 16 |
| minimal winit + wgpu (fill) | ≈ 740–1060 | — | 681–994 | see §2.3 |
| for comparison: D2D text on WARP (C) | ≈ 150–180 | — | 106 | §2.2 |

Binaries: `slint-probe.exe` 11.4–12.0 MB, `gpui-probe.exe` 10.3 MB (release, no UPX). Build under load:
Slint 1 min 30 s, gpui 2 min 43 s (≈ 1.2–1.7 GB of `target/`).

gpui 0.2.2 creates the D3D11 device **twice**: `get_adapter()` calls `D3D11CreateDevice` "to check support",
then creates the real one ([`directx_devices.rs`](https://docs.rs/crate/gpui/0.2.2/source/src/platform/windows/directx_devices.rs)).
The second one is cheaper on NVIDIA (~34 ms, §2.1), but it is still ~250–280 ms of device creation before the first line of UI.

### 2.7 Text quality — [measured], screenshots

`python shots.py` → [`lab/gpu-ui/shots/`](lab/gpu-ui/shots/). The same lines, 100% scale.

![Slint software / Slint femtovg / gpui 0.2.2](lab/gpu-ui/shots/compare_top.png)

![Zoomed ×3: Slint software, Slint femtovg, gpui 0.2.2, D2D ClearType](lab/gpu-ui/shots/compare_zoom.png)

| Renderer | Text AA | "Colored" pixels* | Default font | Emoji | CJK |
|---|---|---:|---|---|---|
| D2D + DirectWrite (WARP or HW) | **ClearType** (subpixel) | 11,640 | Segoe UI Variable (set explicitly) | color (COLR) | DWrite fallback |
| Slint 1.18 software (swash) | grayscale | 142 | **not Segoe UI** (Arial-like) | **monochrome** 🚀/✅ | yes (fontique) |
| Slint 1.18 femtovg (swash) | grayscale | 528 | same | color | yes |
| gpui 0.2.2 (DirectWrite, `DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE`) | grayscale | 409 | Segoe UI | color | yes |

\* pixels with max(R,G,B) − min(R,G,B) > 40 — for grayscale text these are only the emoji, for ClearType the glyph fringes.

A note on Slint: in my setup a `StyledText` paragraph in a `VerticalLayout` **did not wrap** to the width (it ran past the window edge) —
probably it needs an explicit width; the default font has to be set with `default-font-family`.

---

## 3. Toolkit survey

Versions and dates are from crates.io / GitHub as of 2026-09-19 (`crate-src/` holds the unpacked sources the defaults were checked against).

### 3.1 egui / eframe (+ egui_commonmark)

* **Version**: egui/eframe **0.36.2** (2026-09-08). eframe 0.36 depends on **wgpu 30** and **winit 0.30.13**; eframe's default
  renderer feature is **`wgpu`** (glow is optional): `default = ["accesskit","default_fonts",…,"wgpu",…]`.
* **GPU initialization**: egui-wgpu `WgpuSetup::without_display_handle()` → `backends = from_env().unwrap_or(PRIMARY | GL)`,
  `power_preference = HighPerformance` ([setup.rs](https://docs.rs/crate/egui-wgpu/0.36.2/source/src/setup.rs), lines ~245–255).
  Per §2.3 this is the worst option on a dual-GPU machine (≈ 0.65–1.1 s on wgpu alone). It can be narrowed with `WGPU_BACKEND=vulkan|dx12`
  or in code (`NativeOptions.wgpu_options`), or glow can be used instead (WGL ≈ 0.3–0.45 s).
* **Text**: since 0.34 (2026-03-26) rasterization is **skrifa + vello_cpu**, with hinting enabled (#7694); since 0.35 (2026-06-25)
  shaping is **harfrust** (kerning, ligatures, #8031), `subpixel_binning`, configurable hinting target
  ([epaint CHANGELOG](https://github.com/emilk/egui/blob/main/crates/epaint/CHANGELOG.md)). AA is grayscale.
* **Fonts**: embedded only (Ubuntu-Light 362 KB, Hack, monochrome NotoEmoji-Regular, emoji-icon-font). No system
  fonts ([#5233 "Automatically load system fonts when needed"](https://github.com/emilk/egui/issues/5233) is open),
  **no color emoji** ([#2551](https://github.com/emilk/egui/issues/2551), [#8559](https://github.com/emilk/egui/issues/8559) are open).
  CJK only if you load, say, `msyh.ttc` (19 MB) into `FontDefinitions` by hand.
* **Markdown**: [egui_commonmark 0.25.0](https://crates.io/crates/egui_commonmark) (2026-08-05, pulldown-cmark 0.13):
  tables, task lists, images, code highlighting (optional syntect). Immediate mode — the whole visible document is
  laid out again every frame (galleys are cached); the 3.7 MB `large.md` needs virtualization.
* **A11y/IME/selection**: AccessKit → UIA (an eframe default feature); IME via winit (preedit rendering since 0.35);
  text selection in `Label` (there is a selection optimization for large documents, #7917; how well selection works
  across egui_commonmark block boundaries I did not check).
* **For FastMD**: winit tax + wgpu tax + no system fonts/color emoji/ClearType → it will not be a "bullet"
  (the `rust-gpu` prototype by the egui/iced agent will give exact numbers).

### 3.2 iced (built-in `markdown`)

* **Version**: **0.14.0** (2025-12-07; no newer releases, main is active). wgpu **27**, cosmic-text **0.15**, pulldown-cmark 0.12.
* **GPU**: `iced_wgpu` — `Backends::from_env().unwrap_or(PRIMARY)`, `HighPerformance`; there is a **tiny-skia fallback**
  (`ICED_BACKEND=tiny-skia` — `iced_renderer/src/fallback.rs:278`), which is the only way to escape the GPU tax.
  0.14 has "Lazy `Compositor` initialization in `winit` shell" (#2722).
* **Text**: cosmic-text → fontdb scan of all fonts (§2.5: 30–95 ms + 25–75 ms for the first fallback), swash rasterization,
  grayscale; the `crisp` feature (snapping quads to pixels).
* **Markdown**: a built-in widget — incremental parsing (#2776), images (#2786), quotes (#3005), task lists (#3022)
  ([release 0.14.0](https://github.com/iced-rs/iced/releases/tag/0.14.0)). **Markdown text cannot be selected**
  ([discourse](https://discourse.iced.rs/t/markdown-widgets-text-should-be-selectable/1107)).
* **A11y**: none ([#552 "Implement accessibility support"](https://github.com/iced-rs/iced/issues/552) is open). IME since 0.14 (#2777).
* **For FastMD**: with tiny-skia an estimated ~0.35–0.5 s (winit + fontdb + CPU rendering), with wgpu ≥0.9 s. Unselectable
  text is a serious UX drawback for a reader.

### 3.3 Slint (rich text)

* **Version**: **1.18.0** (2026-09-16). License: GPL-3.0 / royalty-free 2.0 (free for desktop with attribution) / commercial.
* **Architecture on Windows**: **winit** backend (or Qt); renderers: **femtovg (OpenGL)** — the default, **software**
  (softbuffer → GDI), optionally Skia (OpenGL/Vulkan/D3D), femtovg-wgpu, vello. Fonts via **fontique** (DirectWrite),
  rasterization via swash (since 1.16) ([Slint 1.16](https://slint.dev/blog/slint-1.16-released)).
* **Rich text**: `StyledText` + `@markdown(...)` appeared in **1.16** (2026-04-16), runtime markdown parsing from Rust/C++/JS/Python
  in **1.17** (2026-06-24) ([1.17](https://slint.dev/blog/slint-1.17-released)). **Only an inline subset** is supported:
  italic/bold, strikethrough, inline code, links (`link-clicked`), lists, `<u>`, `<font color>`
  ([docs](https://docs.slint.dev/latest/docs/slint/reference/elements/styledtext/)). **Headings are an error**
  ("Markdown headings are not supported", test `slint-1.18.0/tests/styled_text.rs`); there are no code blocks or tables.
  The document would have to be assembled from elements by hand (our own block renderer on top of md4c/pulldown-cmark).
* **Text selection**: `Text`/`StyledText` cannot be selected (only `TextInput`/`TextEdit`, without rich text).
* **[measured]**: software 0.26–0.69 s, femtovg 0.73–1.48 s (§2.6); color emoji in the software renderer come out monochrome.
* **For FastMD**: the best of the winit toolkits on startup (software renderer with no GPU tax), but rich text is too poor,
  there is no ClearType, and the winit tax is there. Not a candidate for the "bullet".

### 3.4 gpui (Zed) + gpui-component / gpui-kit

* **Version**: crates.io **gpui 0.2.2** (2025-10-22); Zed itself is v1.20.2 (2026-09-17), and gpui in the monorepo has moved on.
  Windows backend: its own Win32 + **DirectX 11** + **DirectWrite** (Vulkan was dropped for compatibility reasons;
  Direct2D rasterization was replaced with DirectWrite for the sake of RenderDoc — [Zed blog, 2025-08-19](https://zed.dev/blog/windows-progress-report)).
  HLSL is compiled by `fxc.exe` at build time (`build.rs`, `GPUI_FXC_PATH`).
* **Text**: DirectWrite shaping + glyph rasterization into an atlas; in 0.2.2 — `DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE`
  ([direct_write.rs](https://docs.rs/crate/gpui/0.2.2/source/src/platform/windows/direct_write.rs)). Zed main has
  **subpixel (ClearType-like) rendering** since [PR #45423](https://github.com/zed-industries/zed/pull/45423)
  (merged 2026-01-06, `text_rendering_mode`), BGR layout in [#55174](https://github.com/zed-industries/zed/pull/55174)
  (2026-04-29). Software rendering "for systems without a GPU" is an open [PR #63936](https://github.com/zed-industries/zed/pull/63936).
* **Markdown**: [gpui-kit / gpui-component](https://github.com/longbridge/gpui-component) `TextView` — GFM, tables,
  code highlighting, HTML, images, **selection and copying** ([docs](https://gpui-kit.com/component/text-view/)),
  shaping cached across frames (PR #3115). Functionally the best markdown widget in the Rust GPU world.
* **A11y**: on Windows screen reader support is "missing completely" ([#41138](https://github.com/zed-industries/zed/issues/41138), open);
  AccessKit integration is under way in main (PR #61926, 2026-08-07).
* **[measured]**: 0.59–0.63 s to the frame, of which 485 ms is platform initialization before the first line of user code.
* **For FastMD**: text quality is close to native (DirectWrite), the markdown component is strong, but there is the D3D11 tax
  (×2 devices) and the API is unstable (crates.io lags behind the monorepo). Not a "bullet".

### 3.5 Makepad

* **Version**: makepad-widgets **1.0.0** (2025-05-13; [HN](https://news.ycombinator.com/item?id=43971829)), the repository is active
  (push 2026-09-18), no new releases on crates.io.
* **Windows**: its own Win32 + **D3D11 on `EnumAdapters(0)`** (here, NVIDIA) and **HLSL compiled at runtime**
  via `D3DCompile` for every draw shader generated from its own DSL
  ([d3d11.rs](https://docs.rs/crate/makepad-platform/1.0.0/source/src/os/windows/d3d11.rs), `D3d11Cx::new`, `compile_shader`).
  Total on this machine: the D3D11 tax (~230 ms) + runtime FXC (by my estimate tens to hundreds of ms for a set of widgets) — not verified.
* **Text**: its own stack (makepad-draw, SDF/rasterizer), **embedded fonts**, including the crates
  `makepad-fonts-chinese-*` and `makepad-fonts-emoji` — system fonts are not used; grayscale.
* **Markdown**: `Markdown` (pulldown-cmark), `Html` and `TextFlow` widgets.
* **A11y**: none ([#196 "On Accessibility"](https://github.com/makepad/makepad/issues/196), open).
* **For FastMD**: an interesting architecture, but D3D11 HW + runtime shaders + no system fonts/ClearType/UIA.

### 3.6 Xilem / Masonry (+ Vello, Parley)

* **Version**: xilem/masonry **0.4.0** (2025-10-29); main is active. In Q1 2026 Masonry moved to the **`imaging`** abstraction
  (it used to be tied tightly to Vello classic) — it can now run on **Vello CPU** without a GPU; Vello Hybrid is "roughly beta"
  ([Linebender Q1 2026, 2026-04-19](https://linebender.org/blog/tmil-25/)). Vello **0.10.0** (2026-08-14) on wgpu 29.
* **Text**: **Parley + fontique** (DirectWrite enumeration, §2.5: ~1–2 ms), skrifa/swash; grayscale.
* **A11y**: AccessKit built in; IME via `ui-events`.
* **Markdown**: no ready-made widget.
* **For FastMD**: an experimental framework; with Vello on the GPU — winit + wgpu + dozens of compute pipelines (DX12/FXC) —
  the slowest of all; with Vello CPU — only the winit tax. Premature for a product.

### 3.7 Floem

* **Version**: **0.2.0** (2024-11-15) on crates.io; the lapce/floem repository is active (push 2026-06-21).
* Stack: a winit fork (`floem-winit` 0.29.5), wgpu **22** (vger/vello renderers) or tiny-skia, **cosmic-text** (fontdb scan), taffy.
* A11y — not claimed; markdown — none.
* **For FastMD**: outdated dependencies + both taxes + fontdb. Not a candidate.

### 3.8 Freya

* **Version**: **0.5.0-rc.6** (2026-09-13); since 0.4 it has its own reactive core instead of Dioxus ([freyaui.dev/posts/0.4](https://freyaui.dev/posts/0.4)).
* Stack: **Skia** (skia-safe) via winit + **glutin (OpenGL)** / **ash (Vulkan)**; with the `gpu` feature disabled, softbuffer
  (CPU). Fonts via Skia FontMgr (DirectWrite on Windows): system fallback, color emoji (Skia handles COLRv1).
  AccessKit. Markdown — `freya-markdown` (`MarkdownViewer`, pulldown-cmark, highlighting via `freya-code-editor`).
* **For FastMD**: winit tax + WGL/Vulkan tax (≈0.3–0.45 s, §2.1) → an estimated ~0.7–1.0 s; with the CPU path ~0.35 s.
  Not verified in practice.

### 3.9 Dioxus Native / Blitz

* **Version**: dioxus-native **0.8.0-alpha.1**, blitz-dom **0.3.0-beta.2** (2026-08-24). Stylo (Firefox's CSS) + Taffy + Parley,
  rendering via `anyrender` (vello / vello_cpu / vello_hybrid / skia), winit **0.31-beta**. There is an HTML/markdown frontend
  (renders an HTML string without interactivity — [README](https://github.com/DioxusLabs/blitz)); the goal was beta by the end of 2025,
  production in 2026.
* Covered in detail in the `02-web-and-html-engines.md` report (along with the hands-on lab `research/lab/blitz`) — here
  just a remark: with GPU rendering it is the same "winit + wgpu" (§2.3, ≥0.7 s on this machine), with `vello_cpu` the winit tax.

### 3.10 Qt 6 (Widgets / QML)

* **Version**: Qt **6.11.2** documentation. Licenses: LGPLv3 / GPL / commercial. With **static** linking under LGPL
  the user must be able to relink (object files) — [Qt: obligations](https://www.qt.io/development/open-source-lgpl-obligations);
  dynamic linking is the standard path (Qt6Core/Gui/Widgets + `platforms/qwindows.dll`, ~20–30 MB).
* **Rendering**: **Qt Widgets rasterize on the CPU** (raster paint engine) → **no GPU device at all** (neither the GPU tax
  nor winit). Qt Quick uses QRhi, **D3D11** by default on Windows → GPU tax ~230 ms + the QML engine; text in Quick is
  distance-field by default (grayscale), `NativeRendering` optional.
* **Fonts/text**: since **6.8** the default font database is **DirectWrite**, "application start-up time has been improved"
  ([Qt 6.8 LTS, 2024-10-08](https://www.qt.io/blog/qt-6.8-released)); ClearType in Widgets on opaque widgets.
  Since **6.9** — **COLRv0/COLRv1** color fonts and emoji segmentation on Windows ([Emoji in Qt 6.9](https://www.qt.io/blog/emoji-in-qt-6.9)).
  System CJK fallback via DirectWrite.
* **Markdown**: `QTextDocument::setMarkdown(md, MarkdownDialectGitHub)` — CommonMark + tables, task lists,
  strikethrough, autolinks, front matter ([QTextDocument](https://doc.qt.io/qt-6/qtextdocument.html)); the parser is
  **md4c 0.5.3** ([attribution](https://doc.qt.io/qt-6/qtgui-attribution-md4c.html)). `QTextBrowser` gives selection,
  copying, links, images; styling is the limited CSS subset of QTextDocument (not "GitHub-like" one to one).
* **A11y/IME**: UIA, full IME.
* **For FastMD**: the only cross-platform toolkit on my list that **structurally pays neither the GPU tax nor the
  winit tax** and gives ClearType + md4c + UIA. The real startup number will come from the `qt` agent's prototype (`bench/protos/qt`);
  the risks are DLL weight, plugin loading, LGPL procedures, and a look that is "not quite GitHub" without our own block renderer.

### 3.11 Dear ImGui (+ imgui_md)

* **Version**: **1.92.9b** (2026-07-31). Since **1.92.0** (June 2025) — dynamic fonts: "Glyphs are loaded and rasterized
  dynamically. No need to specify ranges, prebake etc." when the backend supports `ImGuiBackendFlags_RendererHasTextures`
  ([release v1.92.0](https://github.com/ocornut/imgui/releases/tag/v1.92.0), [FONTS.md](https://github.com/ocornut/imgui/blob/master/docs/FONTS.md));
  color glyphs via FreeType (`ImGuiFreeTypeLoaderFlags_LoadColor`, plutosvg/lunasvg).
* The window/GPU are chosen by the application (`imgui_impl_win32` + dx11/dx12/gl/vulkan) → **WARP can be used**, landing at roughly
  "GDI + 30–50 ms" (an estimate based on §2.2). But: no real shaping (ligatures/complex scripts), no system
  fallback, grayscale, no UIA, minimal IME, no text selection (only a read-only `InputTextMultiline`).
* **Markdown**: [imgui_md](https://github.com/mekhontsev/imgui_md) (on md4c) — last commit 2022-05, 185★.
* **For FastMD**: fast, but a "stylish" reader with proper typography will not come out of it. Not a candidate.

### 3.12 Flutter desktop

* On Windows the engine draws via **ANGLE (GLES → D3D11)** ([Windows embedder, egl/manager](https://api.flutter.dev/windows-embedder/manager_8cc_source.html)).
  In 3.47 **Impeller** is the default (OpenGLES via ANGLE), and it is **noticeably slower at startup**:
  Flutter 3.47.1 stable on an Intel UHD 770: **Impeller 1232 ms** to the first frame (of which `FlutterViewController` is 1186 ms)
  versus **Skia 165 ms**; main channel: 618 vs 134 ms ([flutter#191860](https://github.com/flutter/flutter/issues/191860),
  see also [#191353](https://github.com/flutter/flutter/issues/191353)); the workaround is `ImpellerSwitch::Disabled` in the runner.
  On this machine D3D11/ANGLE on NVIDIA (~230 ms) is added to the Skia number → an estimated **~0.35–0.45 s** (Skia) / **0.8–1.4 s**
  (Impeller). Not verified in practice (the Flutter SDK is not installed).
* Text: Skia/Impeller, grayscale; system fallback and color emoji via Skia FontMgr (DirectWrite). A11y: UIA.
  Selection: `SelectionArea`.
* **Markdown**: `flutter_markdown` was declared **discontinued** (February 2025, [flutter#162966](https://github.com/flutter/flutter/issues/162966)),
  the successor is [flutter_markdown_plus](https://pub.dev/packages/flutter_markdown_plus) (Foresight Mobile); an alternative is `markdown_widget`.
* **For FastMD**: Dart VM + engine + ANGLE-D3D11 → not a "bullet"; plus ~20+ MB of runtime.

---

## 4. Summary tables

### 4.1 The stack on Windows

| Toolkit (version) | Windows | Default GPU path | Without a HW GPU? | Shaping / rasterization | System fonts | Markdown |
|---|---|---|---|---|---|---|
| **Win32 + D2D/DWrite** (reference) | Win32 | D3D11 (**WARP** possible) | **yes** (WARP / D2D software) | DirectWrite / DirectWrite | DirectWrite (~1 ms) | own renderer + md4c |
| egui/eframe 0.36.2 | winit 0.30 | wgpu 30 (PRIMARY\|GL, HighPerf) | no (glow = WGL) | harfrust / skrifa+vello_cpu | **no** (embedded) | egui_commonmark 0.25 |
| iced 0.14.0 | winit | wgpu 27 (PRIMARY, HighPerf) | **yes**: tiny-skia | cosmic-text (rustybuzz/harfrust) / swash | fontdb (30–95 ms) | built-in `markdown` |
| Slint 1.18.0 | winit (or Qt) | femtovg/OpenGL | **yes**: software | Slint text layout / swash | fontique→DWrite | `StyledText` (inline subset) |
| gpui 0.2.2 | own Win32 | D3D11 (×2 device) | no (PR #63936 open) | DirectWrite / DirectWrite | DirectWrite | gpui-kit `TextView` |
| Makepad 1.0.0 | own Win32 | D3D11 adapter 0 + runtime FXC | no | own / SDF | **no** (embedded, incl. CJK/emoji) | `Markdown`, `Html` |
| Xilem/Masonry 0.4 | winit | Vello (wgpu compute) | yes: Vello CPU (main, 2026) | Parley / swash, skrifa | fontique→DWrite | no |
| Floem 0.2.0 | winit 0.29 fork | wgpu 22 (vger/vello) | yes: tiny-skia | cosmic-text / swash | fontdb | no |
| Freya 0.5.0-rc.6 | winit | Skia/OpenGL (glutin) or Vulkan | yes: softbuffer | Skia (HarfBuzz) / Skia | Skia→DWrite | `freya-markdown` |
| Blitz 0.3-beta / Dioxus Native 0.8-alpha | winit 0.31-beta | Vello (wgpu) | yes: vello_cpu | Parley / vello | fontique→DWrite | HTML path (see report 02) |
| Qt 6.11 Widgets | own QPA | **no GPU** (raster) | **yes, by default** | HarfBuzz / DirectWrite | DirectWrite (since 6.8) | `QTextDocument::setMarkdown` (md4c 0.5.3) |
| Qt 6.11 Quick | own QPA | QRhi D3D11 | yes: software adaptation | HarfBuzz / distance field | DirectWrite | the same QTextDocument / own |
| Dear ImGui 1.92.9b | application | application (WARP possible) | yes | no shaping / stb_truetype or FreeType | no (by hand) | imgui_md (2022) |
| Flutter 3.47 | own embedder | ANGLE→D3D11, Impeller | (software fallback without a GPU) | Skia/Impeller (HarfBuzz) | Skia→DWrite | flutter_markdown_plus / markdown_widget |

### 4.2 Text quality and features

| Toolkit | ClearType / subpixel | Hinting | Color emoji (Segoe UI Emoji = COLRv1) | CJK fallback | Selection/copying | IME | UIA |
|---|---|---|---|---|---|---|---|
| Win32 + D2D/DWrite | **yes** [measured] | yes (DWrite) | yes, COLR via `ENABLE_COLOR_FONT` [measured] | yes [measured] | our own | TSF/IMM | our own (UIA provider) |
| egui 0.36 | no | yes (since 0.34) | **no** (#2551) | **no** (by hand) | yes (Label) | winit | AccessKit |
| iced 0.14 | no | — | swash (COLRv0?) — not verified | yes (cosmic-text) [measured: 4 fonts] | **no** in markdown | yes (0.14) | **no** |
| Slint 1.18 | no | — | femtovg: yes; software: **monochrome** [measured] | yes [measured] | **no** (Text) | winit | AccessKit |
| gpui 0.2.2 / Zed main | 0.2.2: no; main: **yes** (2026-01) | DWrite | yes [measured] | yes [measured] | yes (gpui-kit TextView) | yes | no (in progress) |
| Makepad 1.0 | no | — | embedded emoji font | embedded CJK | not verified | not verified ([#179](https://github.com/makepad/makepad/issues/179) about CJK input closed 2025-06) | no |
| Xilem/Vello | no | skrifa | not verified | yes | not verified | yes | AccessKit |
| Freya 0.5 | usually no (Skia GPU) | Skia | yes (Skia COLRv1) | yes | partly — not verified | winit | AccessKit |
| Qt Widgets 6.9+ | **yes** | DWrite | **yes, COLRv1** (6.9) | yes | yes (QTextBrowser) | yes | UIA |
| Dear ImGui 1.92 | no | FreeType opt. | FreeType+plutosvg | by hand | no | minimal | no |
| Flutter | no | — | yes (Skia) | yes | yes (`SelectionArea`) | yes | UIA |

### 4.3 Time to first frame on THIS machine

"Measured" — §2; "estimate" — the sum of measured components (process ~45–70 + window + GPU + fonts), without document layout.

| Variant | Time to frame | Status |
|---|---:|---|
| Baseline Win32 + GDI (orchestrator, no markdown) | ~90 ms | measured by the orchestrator |
| **Win32 + D2D/DWrite on WARP**, text with ClearType/emoji/CJK | **~150–180 ms** (main→frame 97–135) | **[measured]** |
| Win32 + D2D `HwndRenderTarget SOFTWARE` | ~155–200 ms (main→frame 107–150) | [measured] |
| Dear ImGui Win32 + DX11 **WARP** | ~150–200 ms | estimate |
| Win32 + D2D/DWrite on HW (NVIDIA) | ~360–460 ms | [measured] |
| Slint 1.18 winit + software | 0.26–0.69 s | [measured] |
| iced 0.14 + tiny-skia | ~0.35–0.5 s | estimate (winit + fontdb + CPU) |
| Qt 6 Widgets | waiting for the `qt` prototype (structurally no GPU/winit) | — |
| Flutter (Skia) | ~0.35–0.45 s | estimate based on flutter#191860 + D3D11 |
| gpui 0.2.2 | 0.59–0.63 s | [measured] |
| Freya (Skia/OpenGL) | ~0.7–1.0 s | estimate |
| minimal winit + wgpu | 0.74–1.06 s | [measured] |
| Slint 1.18 winit + femtovg | 0.73–1.48 s | [measured] |
| egui/eframe 0.36 by default (wgpu PRIMARY\|GL) | ~1.0–1.6 s | estimate (winit ~0.25 + wgpu 0.65–1.1 + configure) |
| iced 0.14 + wgpu | ~0.9–1.5 s | estimate |
| Xilem (Vello GPU), Blitz (Vello GPU) | ≥1 s | estimate (winit + wgpu + compute pipelines) |
| Flutter (Impeller, the 3.47 default) | ~0.8–1.4 s | estimate based on flutter#191860 |

---

## 5. Analysis along technical axes

### 5.1 D3D11 vs D3D12 vs Vulkan vs OpenGL/ANGLE on Windows

* On NVIDIA **every hardware API costs 200–350 ms** of cold driver startup in the process (§2.1). The cheapest are D3D11
  (~230 ms) and D3D9Ex (~210 ms); D3D12 + queue (~310 ms) and Vulkan (~280 ms, plus another ~250 ms for a Vulkan swapchain in a window
  via wgpu) are more expensive; OpenGL/WGL (~330 ms, of which `ChoosePixelFormat` is ~190–230) is the most expensive. ANGLE (Flutter,
  Chromium, Avalonia by default) = D3D11 + its own shader translation.
* **WARP** (software D3D11/D3D12) takes 15–22 ms, works with a swapchain and DirectComposition, and D2D on it draws the same
  ClearType. Its performance is enough for a text document (§2.2, scrolling).
* **Two GPUs** do not help: `EnumAdapterByGpuPreference(MINIMUM_POWER)` gives a cheap device (42–55 ms), but the very first
  swap chain into a window on the dGPU's monitor brings up the dGPU driver (+210–240 ms, §2.2). And wgpu on DX12
  initializes every adapter anyway (§2.3). On laptops with MSHybrid the picture is different (monitor on the iGPU) — not something I can check here.
* Parallelization (device in the background while the window is being created) saves only ~50–60 ms: the window is ready in ~60 ms, the driver in ~230.
  The honest "window now, content later" variant is credited by the protocol only via `t_content`.

### 5.2 System font lookup

See §2.5: fontdb/cosmic-text — 30–95 ms + 25–75 ms for the first fallback; fontique/DirectWrite/Skia — 1–3 ms. egui
saves time by giving up system fonts and pays for it with no CJK/color emoji.

### 5.3 Text quality next to DirectWrite

* Native D2D/DWrite (and Qt Widgets, WPF): ClearType + hinting + gamma/contrast from the system ClearType settings.
* GPU toolkits draw glyphs from a grayscale atlas (egui: vello_cpu + hinting; iced/Slint: swash; Makepad: SDF; Vello:
  its own rasterization). At 100% scale this looks softer/bolder (screenshots in §2.7); at 150–200% the difference
  almost disappears. In the GPU world only Zed main has subpixel AA (2026-01).
* Subpixel AA is incompatible with transparent/composited surfaces (Mica, DirectComposition with alpha) — one more
  reason to keep the document background opaque.

### 5.4 Color emoji and CJK

Segoe UI Emoji on Windows 11 is COLRv1 (+v0 fallback) **[measured]**. DirectWrite/D2D, Qt ≥6.9, Skia (Flutter, Freya)
draw COLRv1; swash stacks at best v0; egui only the monochrome NotoEmoji; Slint software is monochrome
**[measured]**. CJK: DirectWrite/fontique/Skia/Qt use the system fallback; egui/ImGui only by hand (+19 MB of font).

### 5.5 IME, selection, accessibility

For a reader IME is only needed in search. Selection and copying are mandatory for a reader: iced markdown and Slint
Text cannot do it, egui and gpui-kit can, Qt/Flutter/native can. UIA: AccessKit (egui, Slint, Xilem, Freya,
Blitz) provides a UIA provider; iced, Makepad, gpui (Windows), ImGui have no accessibility.

---

## 6. Conclusions for FastMD

### 6.1 Recommendations

1. **Do not build FastMD on GPU / immediate-mode / cross-platform Rust and Flutter toolkits** if the goal is a "bullet".
   On a typical gaming/work machine with a discrete NVIDIA card each of them pays **200–350 ms** for the driver,
   winit toolkits another **150–250 ms** for the keyboard layout table, and wgpu toolkits another **up to 0.5–0.8 s** for
   iterating over adapters/backends. This cannot be cured by settings "from outside" (except perhaps `ICED_BACKEND=tiny-skia`,
   `SLINT_BACKEND=winit-software`, `WGPU_BACKEND=…`), only by forking the toolkits.
2. **The reference "bullet" architecture (Windows-first)**: a Win32 window + **DirectWrite** (shaping, fallback, ClearType,
   COLRv1 emoji — all "for free", ~2 ms to initialize) + **Direct2D on top of D3D11 WARP** (or
   `D2D1_RENDER_TARGET_TYPE_SOFTWARE`) for the first frame. **[measured]** ~100–135 ms from `main` to a frame with text,
   i.e. baseline + ~40 ms. A hardware device only if it is ever needed (e.g. smooth animations/zoom): create it
   **after** the first frame on a background thread and switch over on the next frame (D2D needs device-lost logic anyway).
   This is a direct recommendation for the `cpp-d2d` prototype (and `cpp-richedit`, if it uses RichEdit's D2D mode).
3. If **macOS/Linux** is needed later, the only structurally fast candidate among those I looked at is **Qt 6 Widgets**
   (CPU rasterization, DirectWrite, ClearType, md4c/GFM, UIA). Wait for the numbers from the `qt` prototype; if they are good,
   keep the platform layer thin (our own markdown block renderer on top of `QPainter`/`QTextLayout`, not `QTextBrowser`
   with its limited CSS). The alternative is a shared C/C++/Rust core (md4c parser, block layout) + thin native
   renderers: DirectWrite/D2D on Windows, CoreText/CoreGraphics on macOS.
4. **For the other agents' prototypes** (so that the comparison is fair):
   * `rust-gpu` (egui, iced): show both the default and the best "knobs" — eframe with `WGPU_BACKEND=vulkan` (or glow),
     iced with `ICED_BACKEND=tiny-skia`; note in the README that ~150–250 ms is eaten by winit (§2.4).
   * `winui3`, `wpf`, `avalonia`: XAML/DComp, D3D9Ex and ANGLE/Skia also bring up the NVIDIA driver (~210–250 ms, §2.1);
     it is worth adding a `device/renderer created` mark to see its share.
   * `cpp-d2d`: compare `D3D_DRIVER_TYPE_WARP` vs `HARDWARE` for the first frame (§2.2 — a ~300 ms difference).
5. **Fonts**: use the system DirectWrite cache; don't pull in fontdb/cosmic-text; don't embed CJK fonts.

### 6.2 Risks and caveats

* **Machine specifics.** The NVIDIA 616.56 driver (32.0.16.1656), the NVIDIA capture hook `nvspcap64.dll`, `RTSSHooks64.dll`,
  Windhawk in every process and 30 µs per `ToUnicodeEx` may make the absolute numbers worse than on a "clean" machine. But the order of magnitude
  is confirmed independently (RTX 3090: HW 160 ms vs WARP 3.6 ms, [nene-nib #24](https://github.com/hideyukiMORI/nene-nib/issues/24)),
  and the ratios within one series are stable.
* **Noise**: all my series ran under load from other agents' builds (the same operation varied 1×–2× between series).
  Final numbers come only from the orchestrator's serial run.
* **Not verified in practice**: Makepad, Xilem, Floem, Freya, Flutter, Dear ImGui, Qt (the latter is being done by the `qt` agent),
  the cost of AccessKit (within noise), behavior on laptops with MSHybrid, on machines with only an iGPU (there the HW device should be
  cheaper — check on another machine before the final decision).
* **WARP risks**: on weak CPUs (2–4 cores) scrolling large documents on WARP may stutter; D2D on WARP produces
  the same picture, but GPU effects (blur, large rasters/images) will run on the CPU. That is acceptable for a reader,
  but it is worth having a switch to HW after startup.
* **The winit tax** may disappear in future winit versions (the patch is trivial), and gpui may get a software renderer
  (PR #63936) — in a year the picture for Rust toolkits may improve; as of today (2026-09-19) it has not.

### 6.3 Notes on the test bench (harness / protocol)

I found no bugs in `bench/harness/bench.py` and `PROTOCOL.md`. Two notes for the final run:
1. Windhawk is injected into every process (`windhawk.dll`, the mod `explorer-details-better-file-sizes_1.5_…dll`,
   `libunwind.whl`, `libc++.whl`), and `RTSSHooks64.dll` into D3D processes; this is part of the 90 ms "floor" and of the GPU numbers.
   It is worth recording in the final report (or, with the user's permission, doing one control run with
   Windhawk/RTSS disabled).
2. For GPU prototypes it is useful to ask for a "device created" mark; otherwise `t_content` does not show that 60–80% of the time is
   the driver, not the prototype's code.

---

## Sources

* wgpu-hal 30.0.1, DX12 enumerate/expose — <https://docs.rs/crate/wgpu-hal/30.0.1/source/src/dx12/adapter.rs>
* egui-wgpu 0.36.2 `setup.rs` (PRIMARY\|GL, HighPerformance) — <https://docs.rs/crate/egui-wgpu/0.36.2/source/src/setup.rs>
* winit 0.30.13 keyboard / keyboard_layout — <https://docs.rs/crate/winit/0.30.13/source/src/platform_impl/windows/keyboard_layout.rs>
* winit issue #4584 (Punto Switcher, 2026) — <https://github.com/rust-windowing/winit/issues/4584>
* fontique 0.11.1 DirectWrite backend — <https://docs.rs/crate/fontique/0.11.1/source/src/backend/dwrite.rs>
* egui/epaint CHANGELOG (0.34 skrifa+vello_cpu, 0.35 harfrust) — <https://github.com/emilk/egui/blob/main/crates/epaint/CHANGELOG.md>
* egui #2551 Color Emoji, #5233 system fonts, #8559 — <https://github.com/emilk/egui/issues/2551>, <https://github.com/emilk/egui/issues/5233>, <https://github.com/emilk/egui/issues/8559>
* egui_commonmark — <https://github.com/lampsitter/egui_commonmark>
* iced 0.14.0 release — <https://github.com/iced-rs/iced/releases/tag/0.14.0>; selectable markdown — <https://discourse.iced.rs/t/markdown-widgets-text-should-be-selectable/1107>; a11y #552 — <https://github.com/iced-rs/iced/issues/552>
* Slint 1.16 / 1.17 / StyledText / #9560 — <https://slint.dev/blog/slint-1.16-released>, <https://slint.dev/blog/slint-1.17-released>, <https://docs.slint.dev/latest/docs/slint/reference/elements/styledtext/>, <https://github.com/slint-ui/slint/issues/9560>
* Zed Windows (DX11, DirectWrite) — <https://zed.dev/blog/windows-progress-report>; subpixel PR #45423 — <https://github.com/zed-industries/zed/pull/45423>; BGR #55174 — <https://github.com/zed-industries/zed/pull/55174>; software render PR #63936 — <https://github.com/zed-industries/zed/pull/63936>; screen reader #41138 — <https://github.com/zed-industries/zed/issues/41138>; font rendering #39317 — <https://github.com/zed-industries/zed/issues/39317>
* gpui 0.2.2 `directx_devices.rs`, `direct_write.rs` — <https://docs.rs/crate/gpui/0.2.2/source/src/platform/windows/directx_devices.rs>
* gpui-kit / gpui-component TextView — <https://github.com/longbridge/gpui-component>, <https://gpui-kit.com/component/text-view/>
* Makepad 1.0 — <https://news.ycombinator.com/item?id=43971829>; `d3d11.rs` — <https://docs.rs/crate/makepad-platform/1.0.0/source/src/os/windows/d3d11.rs>; a11y #196 — <https://github.com/makepad/makepad/issues/196>
* Linebender Q1 2026 (imaging, Vello CPU/Hybrid) — <https://linebender.org/blog/tmil-25/>; Xilem releases — <https://github.com/linebender/xilem/releases>
* Freya 0.4 — <https://freyaui.dev/posts/0.4>; repo — <https://github.com/marc2332/freya>
* Blitz — <https://github.com/DioxusLabs/blitz>
* Qt 6.8 LTS (DirectWrite default) — <https://www.qt.io/blog/qt-6.8-released>; Qt 6.7 text — <https://www.qt.io/blog/text-improvements-in-qt-6.7>; Emoji in Qt 6.9 — <https://www.qt.io/blog/emoji-in-qt-6.9>; QTextDocument — <https://doc.qt.io/qt-6/qtextdocument.html>; md4c — <https://doc.qt.io/qt-6/qtgui-attribution-md4c.html>; LGPL obligations — <https://www.qt.io/development/open-source-lgpl-obligations>
* Dear ImGui 1.92.0 — <https://github.com/ocornut/imgui/releases/tag/v1.92.0>; FONTS.md — <https://github.com/ocornut/imgui/blob/master/docs/FONTS.md>; imgui_md — <https://github.com/mekhontsev/imgui_md>
* Flutter Impeller vs Skia startup on Windows — <https://github.com/flutter/flutter/issues/191860>, <https://github.com/flutter/flutter/issues/191353>; flutter_markdown discontinued — <https://github.com/flutter/flutter/issues/162966>; flutter_markdown_plus — <https://pub.dev/packages/flutter_markdown_plus>; embedder EGL/ANGLE — <https://api.flutter.dev/windows-embedder/manager_8cc_source.html>
* Independent confirmation of the D3D11 HW vs WARP cost on NVIDIA — <https://github.com/hideyukiMORI/nene-nib/issues/24>
* Vulkan loader env (`VK_LOADER_LAYERS_DISABLE`, `VK_LOADER_DRIVERS_SELECT`) — <https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md>
