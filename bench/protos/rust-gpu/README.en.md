[Русский](README.md) | **English**

# rust-gpu — FastMD on Rust GPU UI (egui / iced)

A Markdown viewer prototype built on Rust "immediate-mode / GPU UI" frameworks:

* **egui 0.36 + egui_commonmark 0.25** — a custom host on winit 0.30 + egui-wgpu (wgpu 30, DX12), without eframe;
  stock eframe (wgpu / glow) is built only for comparison;
* **iced 0.14** with the built-in `markdown` widget (+ `highlighter` on syntect) and a custom `Viewer`
  (styles per the spec); tiny-skia (CPU) and wgpu (GPU) renderers.

All numbers come from the shared harness `bench/harness/bench.py` on the machine from the brief (Ryzen 9 7950X, RTX 5070 + AMD iGPU,
Defender on, MSI Afterburner/RTSS running). The machine was shared with other agents; the final tables were taken during
a quiet period (CPU load 15–30 %), but this is still a rough comparison — the final sequential measurement
is done by the orchestrator.

## TL;DR

| variant | what draws the first frame | medium: median / min, ms | verdict |
|---|---|---|---|
| **rust-gpu-egui-softfirst** | egui meshes are rasterized **on the CPU** (custom rasterizer) + GDI blit at the moment the window is shown; the GPU (wgpu/DX12, RTX 5070) takes over subsequent frames | **89 / 86** | best full-featured one; baseline-win32 + ~15 ms |
| rust-gpu-egui-softfirst-noime | same + `ImmDisableIME` (no IME on the UI thread) | 82 / 81 | another −5…10 ms, but no IME |
| rust-gpu-iced-tinyskia | iced, **CPU** renderer tiny-skia + softbuffer (wgpu not compiled in) | 129 / 124 | decent, but no virtualization: large.md finishes loading in ~10 s |
| rust-gpu-egui-wgpu | egui, **GPU** from the first frame: wgpu 30 / D3D12 on the RTX 5070 | 365 / 347 | honest "pure GPU": +270 ms for D3D12 |

baseline-win32 in the same run: 73 / 68 ms. iced with the GPU renderer (`out/iced-wgpu`, not included in proto.json):
~590 ms.

**Answer to the question "what actually draws the fast variants":** the earlier dev variants `rust-gpu-egui`
(110 ms) and `rust-gpu-iced` (150 ms) indeed **did not use the GPU for the first frame**: egui used its own CPU
rasterizer + GDI (the GPU was initialized only after `t_content`), iced used the pure CPU renderer tiny-skia.
This is now reflected in the names: `rust-gpu-egui-softfirst`, `rust-gpu-iced-tinyskia`. Both draw the full
styled document (screenshots below have been reviewed). An honest GPU variant of egui was added as
`rust-gpu-egui-wgpu`; `rust-gpu-egui-glow` (stock eframe + OpenGL, ~600 ms) and `rust-gpu-iced-wgpu`
(~590 ms) were removed from proto.json — their numbers are in the "Experiments" section.

**Main takeaway.** On Windows, "GPU UI" by itself is not suitable for instant opening: creating a
D3D12 device on the RTX 5070 costs ~270 ms (the NVIDIA driver loads ~100 MB of DLLs); on the AMD iGPU the device
is ready in ~135 ms, but the first present costs another ~200 ms; WARP executes the first frame in ~135 ms. egui becomes
fast when **the first frame is drawn without the GPU**: the egui meshes of the first screen are rasterized on the CPU in ~9 ms,
the window receives the frame at the moment it is shown, and the GPU is brought up only after the document is on screen (in normal
mode wgpu takes over rendering after ~0.3 s). After all the patches, egui's path **from `main()` to `t_content`
(~40 ms) is the same as bare Win32+GDI**; all of the remaining gap to the baseline is the launch of a 7.5 MB exe.

## Stack and versions

| component | version | notes |
|---|---|---|
| Rust | 1.98.1 stable, `x86_64-pc-windows-msvc` | cargo 1.98.1; MSVC 14.44 linker (rustc finds it on its own, no PATH needed) |
| egui / epaint / egui-winit / egui-wgpu / egui_extras | 0.36.2 | `default-features = false`; egui_extras: `file`, `image` (PNG) |
| egui_commonmark (+ _backend) | 0.25.0, **vendored and patched** (`egui/vendor/`) | typography per the spec, see "Patches" |
| pulldown-cmark | 0.13.4 (egui), 0.12.2 (iced) | GFM: tables, task lists, strikethrough, footnotes |
| wgpu / wgpu-core / wgpu-hal | 30.0.1 (egui), 27.0.x (iced) | DX12 + WGSL only; **wgpu-hal patched** (`vendor/wgpu-hal-30`, `vendor/wgpu-hal-27`) |
| winit | 0.30.13, **patched** (`vendor/winit-0.30`) | shared by egui and iced |
| eframe (+glow 0.17, glutin 0.32.3) | 0.36.2 | only `out/egui-eframe`, for experiments |
| iced / iced_widget / iced_graphics / iced_wgpu / iced_highlighter | 0.14.0 / 0.14.2 / 0.14.0 | features `markdown, highlighter, image, crisp, web-colors, thread-pool, advanced` |
| iced_tiny_skia / tiny-skia / softbuffer | 0.14.1 / 0.11.4 / 0.4.8 | iced CPU renderer |
| cosmic-text / swash / fontdb | 0.15.0 / 0.2.10 / 0.23.0 | text in iced (color emoji work) |
| syntect / two-face | 5.3.0 / 0.4.5 | code highlighting in iced |
| windows-sys | 0.61 | DwmFlush, GDI blit, IME, window hook, working set |

Release profile: `opt-level=3, lto=true, codegen-units=1, panic=abort, strip=true`; `.cargo/config.toml`:
static CRT (`+crt-static`) and `/DELAYLOAD` for dxgi/setupapi/ole32/oleaut32/shell32/shlwapi.

## Build

```
pwsh -File bench/protos/rust-gpu/build.ps1          # инкрементально; в свежем чекауте = сборка с нуля
pwsh -File bench/protos/rust-gpu/build.ps1 -Clean   # удалить target-*/out и собрать заново
```

`build.ps1` builds 4 binaries (`cargo build --release --locked`; crates from crates.io per Cargo.lock,
patched ones from `vendor/` and `egui/vendor/`):

| output | cargo | variants |
|---|---|---|
| `out/egui/fastmd-egui.exe` (7.5 MB) | `egui/`, default features, `target/` | rust-gpu-egui-softfirst, -softfirst-noime, -egui-wgpu |
| `out/egui-eframe/fastmd-egui-eframe.exe` (9.5 MB) | `egui/ --features eframe-shell`, `target-eframe/` | experiments only |
| `out/iced/fastmd-iced.exe` (7.8 MB) | `iced/ --no-default-features --features cpu`, `target-cpu/` | rust-gpu-iced-tinyskia |
| `out/iced-wgpu/fastmd-iced-wgpu.exe` (11.7 MB) | `iced/ --no-default-features --features gpu`, `target-gpu/` | experiments only |

Timing: rebuilding one binary with LTO after a code change takes 1 min 40 s … 2 min 40 s; after a winit change
the whole `build.ps1` takes ~7 min. A full build from scratch (≈360 crates for egui + ≈400 for iced) was not timed end to end
in this session; roughly 10–15 min. `build.ps1` in its final form was run several times, exit code 0.

## Variants (proto.json)

| id | exe + args | frame 0 → afterwards |
|---|---|---|
| `rust-gpu-egui-softfirst` | `fastmd-egui.exe --first soft --adapter high --paint-on-show 1` | CPU rasterization (soft.rs) + GDI at the moment the window is shown → wgpu DX12 (RTX 5070) after t_content |
| `rust-gpu-egui-softfirst-noime` | same + `--ime off` | same; `ImmDisableIME(0)` before the first window is created |
| `rust-gpu-egui-wgpu` | `fastmd-egui.exe --first gpu --adapter high` | wgpu DX12 on the RTX 5070 from the first frame |
| `rust-gpu-iced-tinyskia` | `fastmd-iced.exe --renderer tiny-skia` | iced tiny-skia (CPU) + softbuffer |

`fastmd-egui.exe` options (bold = the default in code):

| option | values | meaning |
|---|---|---|
| `--first` | **soft** \| gpu \| cpu | frame 0: CPU + GDI, then GPU / GPU right away / CPU only (never GPU) |
| `--paint-on-show` | **0** \| 1 | blit the prepared frame from a hook at the moment the window becomes visible (see below) |
| `--ime` | **on** \| off | `off` = `ImmDisableIME(0)` for the UI thread before the first window |
| `--adapter` | **all** \| high \| low \| warp \| primary | the patched wgpu-hal (`WGPU_DX12_ADAPTER`) returns exactly one DXGI adapter |
| `--power` | **high** \| low \| none | `PowerPreference` for `request_adapter` |
| `--backend` | **dx12** \| vulkan \| gl \| primary | vulkan/gl only with a `--features wgpu-all-backends` build |
| `--present` | **vsync** \| novsync | `AutoVsync` / `AutoNoVsync` |
| `--gpu-delay` | **1** \| 0 | with `--first soft`: the GPU starts after t_content (1) or in parallel (0) |
| `--fonts` | **mmap** \| read \| builtin | system fonts via mmap / via reading / built-in only (eframe build) |
| `--order` | **gpu-first** \| el-first | order: thread start → event loop, or the other way round |
| `--shell`, `--renderer` | **direct**/eframe, **wgpu**/glow | stock eframe — only in `fastmd-egui-eframe.exe` |

`fastmd-iced*.exe`: `--renderer tiny-skia|wgpu`, `--adapter all|high|low|warp`, `--present`,
`--confirm-frames N` (2), `--prefix-kb N` (8), `--plain-first 0|1` (first frame without highlighting),
`--wait-parse 0|1` (the first `view` waits for the parser instead of drawing an empty frame). Debugging: `FASTMD_LOG=<файл>` (egui:
CPU/GPU frames, GPU handover, hook trace), `FASTMD_SCROLL_TO=<pt>` (egui: scrolling for screenshots).

## How the fast path works (rust-gpu-egui-softfirst)

```
main thread  : main → read (≤1 мс) → split первых ≤64 КБ на блоки (pulldown-cmark) → mmap шрифтов
               → [старт layout-потока] → EventLoop::new → create_window(visible):
                   ├ CreateWindowEx (скрыто) → размер 1000×800 → ShowWindow
                   ├ ★ хук WH_CALLWNDPROC: первое сообщение видимому окну → SetDIBitsToDevice + GdiFlush
                   │   → DwmFlush → t_content          (в bench-режиме)
                   └ активация / IME / фокус (~20 мс) → create_window возвращается
               → (обычный режим) старт GPU-потока, фоновый прогрев раскладок клавиатуры
layout thread: egui::Context + set_fonts → egui-pass только по блокам 1-го экрана → tessellate
               → CPU-растеризация 1000×800 в DIB (≈9 мс)            — успевает до показа окна
bg thread    : (файлы > 96 КБ) парсинг остатка документа на блоки
GPU thread   : (после t_content) wgpu Instance → Adapter (RTX 5070) → Device → egui_wgpu::Renderer;
               главный поток создаёт surface, передаёт текстуры egui (атлас, картинки), дальше рисует GPU
```

* **Rasterizer** `egui/src/soft.rs` (~200 lines) replicates the egui-wgpu shader (`fs_main_gamma_framebuffer`):
  premultiplied colors in gamma space, bilinear atlas sampling, blend (ONE, ONE_MINUS_SRC_ALPHA).
  Visually the frame is indistinguishable from the GPU frame (`shots/egui-cpu-only-medium.png` vs
  `shots/rust-gpu-egui-wgpu-medium.png`).
* **Paint-on-show** (`shell_direct.rs`, module `early`): inside `create_window`, winit shows the window and then spends
  another ~20 ms on activation (WM_ACTIVATEAPP/NCACTIVATE/ACTIVATE, WM_IME_SETCONTEXT → IMM/TSF, focus).
  A thread hook `WH_CALLWNDPROC`, installed for the duration of `create_window`, catches the first message sent to the window once it
  is already visible and has its final client size (per the trace, `WM_WINDOWPOSCHANGING` inside ShowWindow), and
  blits the ready frame before that message is processed — the same thing a regular Win32 app's `WM_PAINT` would show.
  An external observer (`experiments/visprobe.py`) sees the window become visible ~1 ms before the blit;
  the harness's `observed` ≈ `t_window`. A screenshot with no settle delay (`shots/egui-paint-on-show-settle0-medium.png`,
  taken ~0 ms after `t_content` = 83 ms) shows the full document. If the size does not match, the normal path is used.
* **`t_content`** per PROTOCOL §3: the frame with the document has been drawn (GDI: `SetDIBitsToDevice` + `GdiFlush`; GPU:
  `queue.present` + `device.poll(wait)`), then `DwmFlush()`.
* **Virtualization**: each frame lays out only the top-level blocks that intersect the viewport; the rest
  are "measured later" in the background, ≤4 ms per frame (an honest scrollbar). Large files: the first
  ~64 KB are parsed synchronously (cut before an ATX heading outside fenced code), the rest in the background. This is the
  "first viewport first" technique allowed by PROTOCOL.
* **GPU handover** in normal mode (`FASTMD_LOG`, medium): 15 CPU frames of 9–14 ms each (background block
  measurement is running); ~320 ms after the first frame comes `gpu adopted`, after which GPU frames take 1.5–5 ms.
  No UI hangs > 50 ms on either medium or large (`experiments/responsive.py`).

## Spec support (PROTOCOL §5)

| element | egui (all egui variants) | iced |
|---|---|---|
| PMv2 DPI, 1000×800 DIP, window title | ✅ | ✅ |
| body Segoe UI Variable 15 px, line-height 1.6, `#1f2328` | ✅ `SegUIVar.ttf`, 24 px | ⚠️ `Segoe UI` (Variable cannot be selected via fontdb/cosmic-text), 1.6 ✅ |
| h1–h6 30/24/20/17/15/14, semibold, h1/h2 border, h6 `#59636e` | ✅ (semibold = `wght` axis 600) | ✅ (Segoe UI Semibold) |
| **bold** / *italic* / ~~strikethrough~~ | ✅ / ⚠️ synthetic slant / ✅ | ✅ / ✅ real italic / ✅ |
| `inline code` background `#eff1f3`, radius 4 | ✅ (padding = thin spaces) | ✅ |
| links `#0969da`, clickable | ✅ (including inside blockquotes — patch) | ✅ |
| lists: bullet, ordered, nested, task list | ✅ (task items without a bullet) | ✅ (GitHub-style checkboxes; all nested bullets are "•") |
| blockquote: 4 px bar `#d1d9e0`, text `#59636e` | ✅ | ✅ |
| horizontal rule | ✅ | ✅ |
| code block: Cascadia Mono 13.5, background `#f6f8fa`, radius 6, padding 16 | ✅, **with line wrapping** | ✅, no wrapping, horizontal scroll |
| syntax highlighting | ⚠️ simple built-in egui_extras (keywords/strings/comments) | ✅ syntect (base16 theme, muted colors) |
| GFM tables: 1 px grid, bold header, zebra, padding 6/13, alignment | ✅ custom `table.rs` widget (width fits content) | ✅ custom `Viewer::table` (full column width, shares by text length) |
| images (local PNG) | ✅ egui_extras (`file://`) | ✅ |
| Cyrillic / CJK / emoji | ✅ / ✅ (fallback `msyh.ttc`) / ⚠️ **monochrome** (egui does not support COLR) | ✅ / ✅ / ✅ color |
| scrolling | ✅ wheel/touchpad, floating scrollbar | ✅ |
| text quality | grayscale AA, egui rasterizer (no ClearType) | grayscale AA (swash) |
| large file (3.7 MB) | ✅ virtualization, no freezes | ⚠️ no virtualization: the rest is parsed in ~10 s (syntect), then a ~2.7 s UI freeze |

Screenshots (all reviewed): `shots/<variant>-medium.png`, `shots/<variant>-small.png`,
`shots/rust-gpu-egui-softfirst-large.png`, `shots/rust-gpu-iced-tinyskia-large.png`, egui scrolling —
`shots/egui-softfirst-medium-y780.png` (code, blockquote, table, image), `shots/egui-softfirst-medium-y2400.png`,
`shots/rust-gpu-iced-wgpu-*.png` (old iced GPU, before the table fixes).

## Quick numbers

`python bench/harness/bench.py run <4 варианта>,baseline-win32 --doc small,medium,large --runs 5
--out bench/protos/rust-gpu/results/quick-run-final.json` (`<4 варианта>` = the 4 variants) — 5 measurements after warm-up, CPU load 16–27 %.
Median / min / p90, ms from `CreateProcessW` to `t_content`:

| variant | small | medium | large | WS, MB | peak commit, MB | CPU, ms | dist, MB |
|---|---|---|---|---|---|---|---|
| baseline-win32 (floor) | 79 / 71 / 99 | 73 / 68 / 84 | 74 / 73 / 85 | 14 | 3 | 31 | 0.15 |
| **rust-gpu-egui-softfirst** | **86 / 85 / 98** | **89 / 86 / 93** | **91 / 90 / 96** | 23 (large 28) | 10 (large 41) | 63 | 7.5 |
| rust-gpu-egui-softfirst-noime | 88 / 81 / 96 | 82 / 81 / 87 | 93 / 87 / 94 | 23 | 10 (large 41) | 47–78 | 7.5 |
| rust-gpu-iced-tinyskia | 136 / 124 / 151 | 129 / 124 / 143 | 194 / 189 / 199 | 78 (large 111) | 63 (large 103) | 234 | 7.8 |
| rust-gpu-egui-wgpu | 352 / 352 / 374 | 365 / 347 / 371 | 365 / 353 / 369 | 89 | 382 | 313 | 7.5 |

Variance between sessions is large: under 40–100 % load from other builds the same variants gave 130–180 ms
(egui-softfirst before the latest patches) and 180–240 ms (iced) — `results/quick-run-1.json`, `quick-run-2.json`;
single outliers of up to 2–3 s at load peaks. Compare only within one run.

### Breakdown (medians of marks, medium, quick-run-final; ms from CreateProcessW)

**baseline-win32:** `CreateProcessW returned 21.6` → `main 32.0` → window visible `47.8` → `t_window 66.1` →
**`t_content 72.9`**.

**rust-gpu-egui-softfirst:** `CreateProcessW returned 37.8` → `main 48.9` → `read 49.1` → `parsed 49.7` →
`fonts 49.9` → ‖ layout thread: `layout_ui 59.2` (egui context + fonts + first-screen layout, ~9 ms) →
`layout_tess 59.3` → `layout_raster 67.7` (CPU rasterization ~8.5 ms) ‖ main thread: `event_loop 66.0`
(~17 ms: the first `CreateWindowEx` in the process, IMM, RTSS hooks) → window visible `78.0` → blit
`frame0_presented 79.0` → `DwmFlush` (~10 ms) → **`t_content 88.8`**.
From `main` to `t_content`: egui 39.9 ms, baseline 40.9 ms — **the in-process path is no slower than bare
Win32**; the whole difference (~16 ms) is before `main` (exe size).

**rust-gpu-egui-softfirst-noime:** `event_loop 61.1` (13.6 ms after main instead of 17) → window visible `74.5` →
**`t_content 82.3`**.

**rust-gpu-egui-wgpu:** the same up to `window_created 105`; GPU thread: `gpu_thread 51` → `gpu_instance 80`
(loading dxgi/d3d12) → `gpu_adapter 271` (~190 ms: NVIDIA D3D12 adapter, driver initialization) →
`gpu_device 337` (~66 ms) → `gpu_renderer 349` (egui pipelines) → `surface 354` → `frame0_presented 360` →
`poll(wait)` → **`t_content 365`**.

**rust-gpu-iced-tinyskia:** `main 50.1` → `boot 68.7` (iced runtime + event loop) → `font_system 82`
(cosmic-text scans all system fonts, separate thread) → empty first frame `view_empty 90.9` →
`parsed 103` (first 8 KB + syntect ~50 ms, separate thread) → `view_doc 112` → `frame1_msg 123.5` →
**`t_content 129.4`** (confirmation on the 2nd `window::frames()` message).

### Where the time goes (egui-softfirst, ~89 ms vs. a floor of ~73)

1. **Process launch: ~38 ms until `CreateProcessW` returns (baseline 22) + 11 ms until `main`.** The difference from
   the baseline, ~16 ms, is the 7.5 MB exe vs. 150 KB (Defender scans the image). Per `experiments/compare_size.py`:
   0.15 MB → 24 ms, 6 MB → 35, 7.5 MB → 38, 7.8 MB → 41, 11.7 MB → 48 ms ≈ **+2 ms per MB of exe**.
   But `opt-level="s"` (6.1 MB) brought no gain: −0.6 ms at launch, but layout/rasterization +8 ms, and those
   land on the critical path.
2. **Window: ~17 ms `EventLoop::new` + ~12 ms until the window is visible** — the critical path; layout and
   rasterization (~18 ms CPU) run in parallel and finish earlier.
3. **`DwmFlush` ~10 ms** — waiting for composition (part of the protocol, the same for everyone).

## Experiments and A/B (not in proto.json)

`experiments/compare_all.py` — interleaved runs via `bench.run_once`, medium, 4 measurements after warm-up,
load 16–26 %, final binaries:

| configuration | t_content median / min, ms | comment |
|---|---|---|
| egui `--first soft --paint-on-show 1` (= softfirst) | 93 / 87 | |
| egui `--first cpu` (no GPU at all) | 97 / 88 | in bench mode the same as soft |
| egui `--first gpu --adapter high` (RTX 5070) | 367 / 348 | adapter 269, device 335 |
| egui `--first gpu --adapter low` (AMD iGPU) | 412 / 406 | device at 187 ms, but the first present takes ~200 ms |
| egui `--first gpu --adapter warp` (software D3D12) | 248 / 244 | device 100 ms, first frame execution ~125 ms |
| egui `--first gpu --adapter all` (stock wgpu: all adapters) | 448 / 436 | wgpu creates a D3D12 device on **every** GPU |
| stock eframe + wgpu (DX12) | 520 / 504 | eframe hides the window until the first frame and picks the adapter itself |
| eframe + wgpu, device created in advance on a separate thread | 460 / 414 | |
| stock eframe + **glow** (OpenGL/WGL) | 604 / 564 | formerly `rust-gpu-egui-glow`; t_content at the start of the 2nd frame |
| iced tiny-skia | 155 / 136 | |
| iced wgpu `--adapter high` (formerly `rust-gpu-iced-wgpu`) | 592 / 581 | the compositor is created synchronously before the first `view` (`view_doc` 489) |
| iced wgpu `--adapter low` (AMD iGPU) | 644 / 611 | |
| iced wgpu `--adapter all` | 654 / 636 | |

A/B of the startup patches (`experiments/compare_ab.py`, medium):

| comparison | result (median / min, ms) | conditions |
|---|---|---|
| egui softfirst: before winit patches → + `GetSystemMenu`/first-focus patches | 170 / 118 → 139 / 127 | 8 measurements, load ~66 %, noisy |
| + `--ime off` | 139 → 115 / 100 | same |
| + winit size-first patch, without / with paint-on-show | 112 / 95 → **101 / 87** | 12 measurements, load ~14 % |
| `--ime off`, without / with paint-on-show | 96 / 88 → **90 / 80** | same (baseline 73 / 64) |
| iced: before winit patches → after | 190 / 162 → 169 / 156 | 8 measurements, ~66 % |
| iced `--wait-parse 1` (no empty frame) | 200 / 178 — **worse**: the first `view` runs before the window is created, the blocking delays the window | same |
| iced `--plain-first 1` (first frame without highlighting) | 177 / 147 | parsing 8 KB drops from ~70 to ~10 ms, but the critical path is the window + first render |
| iced `--prefix-kb 4` | 169 / 136 | within noise |
| egui `opt-level="s"` (6.1 MB) vs `3` (7.5 MB) | 94 / 82 vs 92 / 81 | `experiments/compare_size.py final` |

Conclusions on the knobs:
* **Adapter selection:** the "exactly one adapter" DXGI patch saves ~80–120 ms — stock wgpu opens a D3D12 device
  on every GPU (NVIDIA ~240 ms + AMD ~90 ms + WARP) just to enumerate them. `HighPerformance` → RTX 5070;
  `LowPower` → AMD iGPU (the device is faster, the total is worse because of the first present).
* **Deferred GPU (`--gpu-delay 1`)**: creating the NVIDIA D3D12 device under the loader lock loads ~100 MB of DLLs and
  slows down the parallel window creation by 5–10 ms, so in softfirst the GPU starts after `t_content`.
* **Fonts:** mmap of system TTFs (SegUIVar, CascadiaMono, seguiemj, msyh.ttc ~20 MB, seguisym) — < 1 ms;
  the real cost is rasterizing the encountered glyphs into the atlas on the first pass (inside the ~9 ms of `layout_ui`).
* **`ImmDisableIME`** — a trick that applies to any Win32 stack: it removes IMM initialization on the first
  `CreateWindowEx` and IMM/TSF activation in `ShowWindow` (5–20 ms depending on the run). The cost is no IME in
  the UI thread's windows (you cannot type a Chinese/Japanese search), so it is a separate variant, not the main one.
* **Static CRT + delay-loaded** DLLs: no loading of vcruntime/ucrt and dxgi/ole32/shell32 before the first frame.

## Dependency patches (all marked `FastMD patch` in the code)

* `vendor/wgpu-hal-30`, `vendor/wgpu-hal-27` (`src/auxil/dxgi/factory.rs`): `WGPU_DX12_ADAPTER=high|low|warp|primary`
  → `EnumAdapterByGpuPreference` returns a single adapter instead of enumerating all of them.
* `vendor/winit-0.30`:
  1. `window.rs::on_create` — **the window size is applied before showing** (upstream shows the window at
     CW_USEDEFAULT size, 2564×984 on this machine, and only then shrinks it: a flash of a huge window + an extra resize);
  2. `window_state.rs::apply_diff` — don't touch the system menu (`GetSystemMenu` makes a copy of the menu, ~5 ms)
     if the "close" button does not change;
  3. `keyboard.rs` — on the first `WM_SETFOCUS` of a new window, don't poll `GetAsyncKeyState` 256 times
     (don't synthesize presses of keys still held down in Explorer);
  4. `synthesize_kbd_state` / `get_agnostic_mods` — don't build the keyboard layout cache (thousands of
     `ToUnicodeEx` calls, ~100–150 ms per layout) in `ShowWindow`; `prewarm_keyboard_layouts()` builds it in the background
     after the first frame.
* `egui/vendor/egui_commonmark*`: GitHub typography (h1–h6 sizes, `wght` 600, line-height 24, inline code
  at 85 % with thin spaces), code block padding 16 / radius 6 without a border, square checkboxes, task items without
  a bullet, blockquote 4 px + 15 px, no extra blank lines around blocks, link color inside blockquotes.
* iced is not patched; `markdown::Row::cells` is private, so the custom table reads it via a pointer cast
  (with a compile-time size check) — a prototype hack; in the product it would be an iced patch.

## Ideas for further speedups

1. **Exe size** (~2 ms/MB at launch): move wgpu/naga (not needed before the first frame) into a separate delay-loaded DLL
   or a second process; drop `image`/`egui_extras` (decode PNG lazily, WIC); without
   this ~15 ms to the baseline remain.
2. **A custom Win32 host instead of winit** (like the baseline): `EventLoop::new` (~17 ms) + `create_window` do a lot of
   unnecessary work; paint-on-show already removes most of it, but event loop creation remains.
3. **Less work on the layout thread** — currently it is not on the critical path (it finishes ~10 ms before the window is shown),
   but on weak machines it will be: a baked atlas is not allowed (forbidden cache), but one can skip loading `msyh.ttc`
   until CJK is encountered, and add an ASCII fast path.
4. **iced**: a CPU-first frame like egui's requires forking `iced_winit` (the compositor is created synchronously);
   list virtualization and background highlighting are mandatory for large files.

## Effort estimate for a full product

* **egui (softfirst architecture):** 6–10 person-weeks to a quality viewer: in-document search,
  selection/copying across virtualized blocks, anchors/table of contents, color emoji (a custom COLR rasterizer
  or bitmap emoji), better text rendering (egui has nothing ClearType-like — only replacing the rasterizer),
  dark theme, DnD, reload on file change, moving between monitors with different DPI (re-layout and
  a new CPU frame), rendering tests. Plus maintaining three forks (winit, wgpu-hal, egui_commonmark).
* **iced:** 5–8 weeks feature-wise (color emoji, highlighting and real italic are already there), but it needs its own
  virtualization and a background parser that doesn't block the UI; startup time ≥125 ms (CPU) cannot be brought down without
  forking iced; iced's GPU renderer is not suitable for "opening like a bullet".

## Pitfalls

* **D3D12 on NVIDIA is expensive**: ~270 ms until the device is ready on the RTX 5070; AMD iGPU — device ~135 ms,
  but the first present ~200 ms; WARP — first frame ~125–135 ms. No GPU path fits into 200 ms.
* **By default wgpu creates a device on every adapter** while enumerating — +80…120 ms without the patch.
* **eframe** hides the window until the first frame and enumerates adapters itself — hence the custom host.
* **winit**: keyboard layout cache in `ShowWindow` (~100–150 ms), showing the window at the default size followed by
  a resize, copying the system menu, 256 × `GetAsyncKeyState` — all patched; `EventLoop::new`
  still takes ~15 ms.
* **egui** cannot do color emoji — Segoe UI Emoji is drawn as outlines; no ClearType; italic is synthetic.
* **iced**: `markdown::parse` immediately highlights all code blocks with syntect (regexes are compiled on the first
  use of a language, ~15–20 ms per language) — hence the cut of the first 8 KB; no virtualization — `large.md`
  is fully available only after ~10–13 s, with a ~2.7 s UI freeze; `Segoe UI Variable` cannot be selected via fontdb;
  private fields of `markdown::Row`.
* **iced wgpu**: the compositor is created synchronously before the first `view` — the whole D3D12 cost is on the critical path.
* **Defender** scans the exe on every launch: ~+2 ms per MB; the first launch after a rebuild takes 300–600 ms.
* **RTSS (MSI Afterburner)** on this machine injects `RTSSHooks64.dll` into all processes with windows and is visible in
  the profile of the first `CreateWindowEx` — the same for all prototypes.
* No issues with the harness; `bench.py shot` writes to `bench/results/shots/` by default, here
  `--out` into the prototype directory was used everywhere.

## Files

* `proto.json`, `build.ps1`, `README.md`
* `common/bench.rs` — the protocol (marks, DwmFlush, atomic result write)
* `egui/src/`: `main.rs` (arguments, fonts, IME, GPU init), `shell_direct.rs` (custom winit host, thread pipeline,
  paint-on-show), `soft.rs` (CPU rasterizer + GDI blit), `view.rs` (virtualized document),
  `blocks.rs` (splitting into blocks, prefix cut), `table.rs` (GFM table), `shell_eframe.rs` (stock eframe)
* `iced/src/main.rs` — the iced app, custom `markdown::Viewer` (headings, lists, blockquotes, code, table)
* `vendor/`, `egui/vendor/` — patched crates
* `experiments/`: `compare_all.py`, `compare_ab.py`, `compare_size.py`, `exp.py` (shared runner),
  `shot.py` (screenshot of an arbitrary configuration), `marks.py` (breakdown by marks), `responsive.py`
  (UI hangs in normal mode), `visprobe.py` (external observer of window visibility), `winit-probe/`
  (sampling profiler of window creation), copies of old binaries for A/B, logs
* `results/` — JSON of quick runs; `shots/` — screenshots

## Audit (independent review, 2026-09-19)

Auditor of the `rust-gpu` prototype: checked the honesty of the marks per PROTOCOL §3, the builds, that everything works, and
the visual quality. **The code, `build.ps1` and `proto.json` were not changed**: no protocol violations were found.
Only audit tools and artifacts were added: `experiments/audit_pixprobe.py`, `experiments/audit-notrans/`,
`shots/audit-*.png`, `results/audit-run.json`.

### What was checked

| item | result |
|---|---|
| Release builds | `cargo build --release --locked` in `egui/` and `iced/` (target-cpu) — a no-op in 0.3 s (artifacts up to date); SHA-256 of `out/egui/fastmd-egui.exe` and `out/iced/fastmd-iced.exe` match `target*/release/`. Profile: `opt-level=3`, `lto=true`, `codegen-units=1`, `panic=abort`, `strip`, `+crt-static` — as stated |
| Caches / resident processes / embedded corpus | none: in `egui/src`, `iced/src`, `common/` there are no `include_bytes!/include_str!`, no writes to temp/AppData, no mentions of the corpus; fonts are mmapped from system TTFs on every launch; in softfirst the GPU is initialized only after `t_content` (in bench mode it is not touched at all — this is honestly reflected in the name `softfirst`) |
| `t_content` egui softfirst | the document frame, rasterized on the CPU (`soft.rs`), is output via `SetDIBitsToDevice` + `GdiFlush` → `DwmFlush` → mark. With `--paint-on-show 1` this is done from the `WH_CALLWNDPROC` hook on the first message to the already visible window (per `notes`, `WM_WINDOWPOSCHANGING` 0x46) with a client-area size check; the frame is the full document, not a placeholder. A screenshot with no settle delay (`shots/audit-rust-gpu-egui-softfirst-medium-settle0.png`) differs from the GPU frame only in antialiasing: 1.2 % of pixels, of which 287 px differ by > 32 levels. |
| `t_content` egui wgpu | `queue.present` → `device.poll(wait)` → `DwmFlush`; pixel probe (below): the document appears on screen in the first sample after `t_content` (+2.4…2.5 ms); in the previous sample (−7…12 ms) the window is still empty — the mark is accurate |
| `t_content` iced tiny-skia | the 2nd `window::frames()` message after `view` received the document (+ `DwmFlush`). Frames are counted only after `view` with the document, so the mark cannot fire too early; `confirm_frames=2` is conservative (one frame later than necessary; per the marks `frame1_msg → t_content` ≈ 6 ms including `DwmFlush`) — left as is |
| First screen of large files | egui — the first ~64 KB synchronously, the rest in the background; iced — the first 8 KB. Allowed by PROTOCOL §3 ("first viewport first"), described in the README |

### `bench.py check` (machine shared with other agents)

| variant | medium: content / window / observed, ms | large: content / window / observed, ms |
|---|---|---|
| rust-gpu-egui-softfirst | 253.9 / 246.3 / 245.5 (cold start after other agents' builds, launch 202 ms; subsequent launches 80–100) | 86.2 / 80.1 / 79.3 |
| rust-gpu-egui-softfirst-noime | 82.2 / 71.5 / 70.8 | 81.2 / 76.6 / 75.9 |
| rust-gpu-egui-wgpu | 370.1 / 100.8 / 82.0 | 359.4 / 102.2 / 85.3 |
| rust-gpu-iced-tinyskia | 291.0 / 286.6 / 251.8 (cold start, launch 201 ms) | 195.7 / 190.6 / 102.9 |

In all cases `observed_ms` ≤ `t_window` ≤ `t_content` — the harness never sees the window later than the mark; `check`
issued no warnings, exit code 0.

### Independent screen probe (`experiments/audit_pixprobe.py`)

The harness checks only window visibility. The probe reads the **screen** (BitBlt from the screen DC = the frame
composited by DWM) over the top 240 px of the client area every 6–12 ms and compares it with the final frame.
`reveal` = the first sample in which ≥ 50 % of pixels match the final frame (DWM has shown the window). medium, medians of 4–5
runs, ms from `CreateProcessW`:

| configuration | t_content | window visible (probe) | reveal on screen | reveal − t_content | reveal − visibility |
|---|---|---|---|---|---|
| baseline-win32 | 82 | 51 | 144 | +63 | 95 |
| egui softfirst, `--paint-on-show 1` (proto.json) | 100 | 84 | 176 | +75 | 95 |
| egui softfirst, `--paint-on-show 0` | 112 | 79 | 176 | +63 | 97 |
| egui softfirst-noime, `--paint-on-show 1` | 97 | 80 | 176 | +79 | 98 |
| egui softfirst-noime, `--paint-on-show 0` | 95 | 78 | 171 | +75 | 97 |
| iced tiny-skia | 141 | 100 | 198 | +55 | 97 |
| egui wgpu | 365 | 81 | 171 (empty white window), document at t_content + 2.5 | — | 91 |
| baseline copy (`audit-notrans`) | 80 | 49 | 143 | +63 | 95 |
| same + `DWMWA_TRANSITIONS_FORCEDISABLED` | 79 | 50 | 76 | **≈ 0** (−8…+0.3) | 27 |

(For egui softfirst, one of the 5 runs was a ~2.9 s outlier due to load; the medians do not depend on it.
Probe resolution is one sample, 6–12 ms.)

System settings: `SPI_GETCLIENTAREAANIMATION = 1`, window animation enabled.

Conclusions:

1. **The Windows 11 window-open animation hides any window for ~95 ms after it becomes visible**,
   regardless of when the app drew its frame (if it managed to do so earlier). For all stacks `t_content`
   comes before the actual appearance on screen. The marks are still honest: the frame with the document is already in DWM.
2. **Paint-on-show complies with the protocol**: it is a real document frame, `GdiFlush` + `DwmFlush`. But
   with the animation enabled its gain (−10…12 ms of `t_content` vs `--paint-on-show 0`) **is not visible
   on screen**: the reveal is the same (176 / 176 ms). The variant is kept in `proto.json`. The trick really works if
   the transition is disabled (see item 3), and it applies to any Win32 stack. But when comparing stacks keep in mind that
   for egui-softfirst `t_content` is taken at an earlier point of `ShowWindow` (before window activation) than for stacks
   that draw in `WM_PAINT` — that is ~10 ms.
3. **A finding for the whole project**: `DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, TRUE)` before
   `ShowWindow` removes this delay: the window appears on screen immediately with its content (reveal − t_content ≈ 0
   instead of +63 ms). For "opening like a bullet" this is **~60–65 ms of real visible latency** almost for free for any
   Win32 stack (including winit: it is enough to call it before the window is shown). The harness does not see this because
   `t_content` comes before the reveal. The user-visible time for fast stacks ≈
   max(`observed_ms` + ~95, `t_content`).
4. The on-screen time for egui-softfirst (~176 ms) is determined by the moment of `ShowWindow` (~80 ms), not by
   rasterization. The gap to the baseline (reveal 144) is ~30 ms before the window is shown: the 7.5 MB exe plus
   winit's `EventLoop::new` / `create_window`.

### Visual assessment (screenshots `shots/audit-*`: medium, small, large, medium scrolled to y=700/1300)

* **egui (softfirst / noime / wgpu draw identically)** — visually **7/10**, spec compliance
  **7.5/10**. Everything required is there and looks like GitHub: h1–h6 (h1/h2 with a rule, h6 gray), Segoe UI
  Variable, line height 1.6, links `#0969da`, inline code with a background, nested bullet and ordered
  lists, task list, blockquote with a bar, hr, code block (background, radius, padding, highlighting), GFM table
  (grid, bold header, zebra, alignment, width fits content), PNG, Cyrillic and CJK. Downsides:
  **monochrome emoji** (✅ ❌ 🚀 — outlines), synthetic italic, grayscale antialiasing without ClearType (text
  slightly thinner and softer than with DirectWrite), egui-style checkboxes (gray, not GitHub's), garish simple
  code highlighting, an extra copy icon in code blocks.
* **iced tiny-skia** — visually **6.5/10**: color emoji, real italic, GitHub-like checkboxes,
  muted syntect highlighting. But there is a **rendering defect**: a punctuation mark after a styled
  fragment inherits its style — "**bold,**" (the comma is bold), a comma after `inline code` ends up inside
  the monospace chip. The span model in `iced_widget::markdown` is correct, so the cause is probably
  cosmic-text itemization. Also: inline code sticks to the preceding word, kana in a CJK line are
  thinner than the ideographs (a fallback font with a different weight), Segoe UI instead of Segoe UI Variable, tables
  are stretched to the full width, all nested markers are "•".

### Control run (`results/audit-run.json`, `--runs 5`, shared machine)

| variant | small: median / min | medium: median / min | observed medium | CPU, ms | WS, MB |
|---|---|---|---|---|---|
| baseline-win32 | 73.7 / 68.3 | 74.2 / 68.0 | 48.4 | 47 | 14 |
| rust-gpu-egui-softfirst | 85.5 / 84.5 | 84.7 / 83.0 | 77.7 | 47 | 23 |
| rust-gpu-egui-softfirst-noime | 86.8 / 80.8 | 82.6 / 81.1 | 74.6 | 47 | 23 |
| rust-gpu-iced-tinyskia | 118.5 / 117.1 | 127.3 / 123.2 | 89.7 | 250 | 78 |
| rust-gpu-egui-wgpu | 351.9 / 346.0 | 352.1 / 346.3 | 81.9 | 313 | 89 |

Matches the author's numbers (85 vs 89, 83 vs 82, 127 vs 129, 352 vs 365 ms): the claims are
confirmed. All 4 variants are fit for the final sequential measurement.
