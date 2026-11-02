[Русский](REPORT.md) | **English**

# FastMD: choosing the technology stack

> The final report of the research phase, 19.09.2026.
>
> - **Theory:** 6 reports in [`research/`](research/).
> - **Practice:** 12 prototypes and 47 variants in [`bench/protos/`](bench/protos/). All of them were measured by one harness under one protocol ([`bench/PROTOCOL.md`](bench/PROTOCOL.md)).
> - **Checks:** every prototype had an independent audit of how honest its measurement and its rendering were. The report itself went through an adversarial review by three critics: all 47 × 3 medians were recomputed from the raw data, and the interpretations were corrected according to their comments.

---

## 0. In short

**Recommendation: C++ + Win32 + DirectWrite + md4c with our own document rendering engine.**
- The first frame is computed on the CPU, with no GPU device.
- Only the first screen is rendered first; the rest is rendered afterwards.

This path gives the largest speed margin and the best look. The price is the largest amount of manual work: text selection, search, copying, accessibility and an HTML subset are written from scratch (§6–7). If that price turns out to be unacceptable, the fallback paths are defined in advance (§7, §9).

| What is opened | Time to rendered document, medium.md (53 KB), median |
|---|---|
| **The FastMD prototype `cpp-d2d`** | **59 ms** |
| `cpp-d2d` in resident mode (the process is already running) | **28 ms** |
| An empty Win32 window (a reference with no speed-up techniques) | 78 ms |
| Windows 11 Notepad (syntax mode, no formatting; restores the session's tabs) | 473 ms |
| WinUI 3, the best measured configuration (C#, NativeAOT) | 324 ms |
| WebView2 / wry / Tauri | 447–630 ms |
| Electron (the Typora, Obsidian, MarkText class) | 753–903 ms |
| VS Code (the editor window only, not the preview) | 1,138 ms |
| **Void — your current app for .md** | **1,444 ms**. This is only to the window: on a clean profile the document is hidden behind onboarding, and with your profile and extensions it is probably longer |
| Cursor | 2,351 ms |

About the 59 ms: this is the level of a "tuned floor". An empty window with the same techniques (IME initialization disabled for the thread, showing without activation) gives ≈58–64 ms ([05 §5](research/05-windows-startup-engineering.en.md)). The 78 ms reference does not use these techniques. In other words, Markdown barely gets onto the critical path.

**The main points:**

- **The time goes into platform initialization, not into Markdown.** md4c parses 3.7 MB in 10.6 ms. The rest is the GPU driver, browser processes, runtimes and the first activation of the window.
- **On the GFM core, "stylish" is achievable almost everywhere.** Native rendering on DirectWrite matches the Chromium reference in layout (9/10, §4). A caveat: the corpus contained no raw HTML, SVG badges or remote images, and these are present in 68–85 % of READMEs on GitHub. Web engines draw them for free; a native engine has yet to implement them.
- **Among the stacks faster than 150 ms, the choice is speed margin versus amount of work:**

  | Stack | medium.md |
  |---|---|
  | cpp-d2d | 59 ms |
  | RichEdit | 69 ms |
  | litehtml | 103 ms |
  | Blitz | 112 ms |
  | Avalonia | 143 ms |
  | Qt | 149 ms |

  All of them fit the product goal of "≤250 ms" from [06](research/06-product-ux-landscape.en.md). `cpp-d2d` was chosen because the main goal is a "bullet": ≈86 ms from a double click against ≈170–180 ms for Qt and Avalonia. The price is the highest effort (§7).
- **WinUI 3 opens 5.5 times slower.**
  - About 135 ms of the 324 (≈40 %) it spends waiting for a D3D11 device for the XAML compositor on NVIDIA. The device itself costs ≈200 ms, but it is created in parallel with the window.
  - Another ≈80 ms goes into fixed XAML initialization.
  - No untested "knob" will bring it below ≈260–290 ms (§5).
- **A web engine is needed only as a lazy "rich mode"** for Mermaid and full KaTeX. WebView2 has a floor of ≈0.45 s for every new process, and neither the document nor the flags move it much.

---

## 1. How it was measured

**The machine:**
- Windows 11 Pro 26200, Ryzen 9 7950X (16 cores), 64 GB, NVMe;
- RTX 5070 + AMD iGPU, a 165 Hz monitor;
- Defender RTP on;
- Windhawk and RTSS are injected into every process: the RTSS hook costs ≈10 ms on the first window of each process, the reference included.

**The `t_content` metric.** It counts from the call to `CreateProcessW` to the moment the first screen of the rendered and styled document reached the screen. The application records that moment itself: it calls `DwmFlush` and writes down the precise time. For web engines the mark is taken after a double `requestAnimationFrame` plus `DwmFlush` in the host.

**How the mark was checked.** The harness ([`bench/harness/bench.py`](bench/harness/bench.py)) starts the application in a Job Object, so it accounts for the WebView2/Electron child processes as well, and it independently watches for the window to appear. The auditors also captured the screen with pixel probes:

| Stack | When the document is actually on screen relative to the mark |
|---|---|
| GDI/DIB: `cpp-d2d`, RichEdit, Qt, WPF, wry/Tauri | in the same composition frame |
| XAML and WebView2 without a GPU process | 1–4 frames later (≈6–35 ms) |
| Web variants with a separate GPU process | 15–95 ms later |
| egui-softfirst | the mark is taken inside `ShowWindow`, ≈10 ms earlier than stacks that mark in `WM_PAINT` |
| Avalonia, iced | the mark is conservative by 6–50 ms |

Almost all of these errors favor the slow stacks, so they do not change the conclusion.

**The corpus.** Cyrillic, CJK, color emoji, GFM tables, task lists, nested lists, code in 7 languages, local images.

| Document | Size | Lines |
|---|---|---|
| small.md | 3.4 KB | ~90 |
| medium.md | 53 KB | 1.4 thousand |
| large.md | 3.7 MB | 97 thousand |

**The final run:**
- 45 variants × 3 documents × 13 launches in interleaved order; 2 resident variants in separate back-to-back runs;
- the first launch is not included in the median;
- the run went through the explorer.exe context so that the AppData/HKCU virtualization of the Claude Desktop MSIX package would not interfere. Compared with a run from the session: native stacks are 2–2.5 ms slower, heavy ones 5–10 %;
- raw data: [`bench/results/final/`](bench/results/final/).

**Caveats:**

1. **All measurements were taken on a warm file cache.** The exe, DLLs, fonts and WebView2/Electron profiles are already in memory. A true cold start (after a reboot, the first launch after an install or update) was not measured. It hits stacks with large binaries harder: WinUI (235 files), Electron, the WebView2 Runtime.
2. **Third-party applications (`ext-*`) do not report content.** For them only the appearance of a window with the file name in its title was measured, so the real time is even longer.
3. **Speed-up techniques were applied unevenly.**
   - Disabling IME is present in cpp-d2d, RichEdit, Qt, litehtml, Sciter, WPF and egui-noime.
   - It is absent in WinUI, Avalonia, Blitz, iced, egui-softfirst and the web stacks.
   - That is ≈10–15 ms in favor of the former, more on this machine: `TextInputHost.exe` had accumulated 139 thousand seconds of CPU according to Get-Process as of 19.09 and was not restarted before the run.
   - **Differences of up to ≈15 ms between these groups are not meaningful.**
4. **`t_content` does not include the Windows 11 window-open animation.** For the fast stacks the document is fully visible ≈40–80 ms after `t_content`. For stacks that show the window early (WinUI, WebView2, Electron), the animation has time to finish before the content appears. `cpp-d2d` turns the animation off (`DWMWA_TRANSITIONS_FORCEDISABLED`), but that is one call available to any stack with an HWND. It is not an advantage of the stack but a **mandatory item whatever the choice**.
5. **The tail of the distribution.** 14 of 1,833 launches (3 of 39 for the reference) stalled for 2.1–2.7 s inside `CreateProcessW` itself, roughly once per round of the run. The median and p90 hide this. It does not depend on the stack; it can be avoided only by not creating a process (§6, resident).
6. **The numbers apply to a machine with a discrete NVIDIA GPU and a 16-core CPU.**
   - On a laptop with only an iGPU the GPU device penalty is smaller: here the AMD iGPU costs ≈55–85 ms against ≈205 ms for NVIDIA.
   - CPU rasterization on a weak CPU will be slower.
   - On a 60 Hz monitor `DwmFlush` waits up to 16.7 ms.

   The orders of magnitude do not change because of this, but before the final decision the measurement is worth repeating on a weak laptop (§9).

---

## 2. Results: the best variant of each stack

Median `t_content` in ms over 12 launches (the first one excluded).

| Stack, best honest configuration | small | **medium** | large | Distribution size | Visuals (audit) |
|---|---|---|---|---|---|
| **C++ Win32 + DirectWrite → GDI DIB + md4c** (`cpp-d2d`) | 60 | **59** | 61 | 0.4 MB | **9** |
| C++ RichEdit windowless + md4c→RTF (`cpp-richedit-wl-mt`) | 70 | **69** | 72 | 0.26 MB | 7 |
| C++ RichEdit, a regular control (`cpp-richedit`) | 80 | **84** | 86 | 0.25 MB | 7 |
| egui, first frame on the CPU, patched winit (`rust-gpu-egui-softfirst`)¹ | 78 | **77** | 83 | 7.5 MB | 7 |
| *Reference: an empty Win32 window with no speed-up techniques* | 77 | *78* | 78 | 0.15 MB | — |
| litehtml + our own D2D/DWrite container, CPU (`litehtml-sciter-litehtml`) | 90 | **103** | 102 | 1.8 MB | 9 |
| Blitz: Rust, real CSS via Stylo, CPU raster (`blitz`) | 111 | **112** | 115 | 8 MB | 8 |
| iced 0.14, tiny-skia CPU (`rust-gpu-iced-tinyskia`) | 120 | **129** | 183 | 7.8 MB | 6.5 |
| Avalonia 12 NativeAOT, Skia CPU (`avalonia`) | 132 | **143** | 143 | 31 MB | 8 |
| Qt 6.12 Widgets, static (`qt`) / official DLLs (`qt-dll`) | 141 / 150 | **149 / 154** | 151 / 160 | 20 / 27 MB | 8.5 |
| WPF .NET 8 + R2R + a native launcher (`wpf`) | 295 | **287** | 295 | 1.75 MB + .NET | 8 |
| Sciter.JS 6 | 319 | **320** | 326 | 19 MB | 7 |
| **WinUI 3, C# NativeAOT** (`winui3`) | 302 | **324** | 330 | 58 MB (8 MB without the bundled WinAppSDK) | 8 |
| C++ + WebView2, tuned (`cpp-webview2`) | 429 | **447** | 445 | 0.5 MB | 9 |
| Rust wry + WebView2 | 468 | **473** | 474 | 2.5 MB | 9 |
| WinUI 3 + WebView2 | 479 | **497** | 495 | 58 MB | 9 |
| Tauri 2 | 591 | **630** | 668 | 5.6 MB | 9 |
| Electron 44, tuned | 727 | **753** | 744 | 324 MB | 9 |
| **Resident modes**² | | | | | |
| `cpp-d2d-resident` | 29 | **28** | 29 | 0.4 MB | 9 |
| `cpp-webview2-resident` | 65 | **65** | 84 | 0.5 MB | 9 |

¹ Possible only with patches to winit and wgpu and our own CPU raster; the mark is ≈10 ms earlier than for stacks that take it in `WM_PAINT` (§1).
² The process is already running; a launcher from the same exe passes the path via `WM_COPYDATA`. This is not DelegateExecute, see §6. The first document after the WebView2 resident starts opens in 165–280 ms; it was not included in the median.

**A large file.** If the whole document is rendered before the first frame, the times on large.md are:

| Variant | Whole document at once | First screen first |
|---|---|---|
| RichEdit, windowed | 998 ms | 86 ms |
| Qt | 721 ms | 151 ms |
| Blitz | 2,478 ms | 115 ms |
| WebView2: untuned, with a GPU process / tuned | 2,105 ms | 445 ms |

**Conclusion: "first screen first" is mandatory for any stack.**

The full table of all 47 variants is in Appendix A.

---

## 3. Where the milliseconds go: the laws of this machine

### 3.1 A hardware GPU device: ≈150–250 ms, and with shader pipelines up to +620 ms

On NVIDIA, `D3D11CreateDevice` pulls in ≈195 MB of user-mode driver and a signature check ([05 §4](research/05-windows-startup-engineering.en.md)). The same code without and with the GPU:

| Prototype | Without GPU | With GPU | Difference | Note |
|---|---|---|---|---|
| cpp-d2d | 59 | 261 | +202 | |
| litehtml | 103 | 259 | +156 | |
| Avalonia | 143 | 344 | +201 | |
| egui | 77 | 324 | +247 | |
| Blitz | 112 | 735 | +623 | plus vello_hybrid's D3D12 pipelines |

In `cpp-d2d-gpu` the device is created on a worker thread from about the 26th to the 206th ms, i.e. ≈180 ms. The UI thread waits for it from the 52nd ms.

Who pays this tax:
- **Always:** WinUI 3 (the XAML compositor always takes the hardware adapter), WPF (D3D9), Avalonia with ANGLE, wgpu, `ID2D1HwndRenderTarget` by default.
- **Chromium, WebView2, Electron — only in the default configuration.** A separate GPU process starts in parallel and adds 18–55 ms: `cpp-webview2-default` 465 against 447 ms, `electron-gpu` 808 against 753 ms. The best web variants run without a GPU process. Their slowness is the ≈0.45 s floor (§3.3).

**For our own engine: the first frame is drawn on the CPU.** DirectWrite → GDI DIB, D2D on WARP, or Skia CPU.

### 3.2 The EXE size tax (most likely Defender): ≈1.2–1.9 ms per MB, on every launch

The time goes into synchronous kernel processing inside `CreateProcessW`. For Blitz it came to up to ~2.2 ms/MB ([05 §2.2](research/05-windows-startup-engineering.en.md)).
- **Electron:** **461 ms** pass before the first line of code (`launch_overhead`, a 235 MB exe).
- **VS Code and Cursor:** ≈250 ms per process.
- **.NET single-file, 64 MB:** +130 ms.

DLLs do not pay this tax. **Rule: a small exe, the heavy parts in DLLs.**

### 3.3 The web engine floor: ≈0.45 s per new process

The breakdown of `cpp-webview2`:

| Interval, ms | Duration | What happens |
|---|---|---|
| 0 → 42 | 42 | the exe has started, environment creation is called |
| 42 → 290 | ≈240 | the browser process comes up, the controller is ready |
| 290 → 432 | ≈140 | the renderer process, navigation, DOM |
| 432 → 441 | ≈10 | the first frame |

Chromium flags (no GPU process, networking inside the browser process) give 20–60 ms and cut CPU by a third, but they do not move the floor. Tauri adds ≈150 ms over bare wry: the tao window is activated sequentially and builds a keyboard layout cache. Details are in [02 §15](research/02-web-and-html-engines.en.md) and the prototype READMEs.

### 3.4 The first activation of the window

- **TSF/IME on the first `ShowWindow`** costs 12–35 ms. It is cured by calling `ImmDisableIME(0)` for the document's UI thread only, as in the prototype. `ImmDisableIME(-1)` turns IME off for the whole process for good, and CJK input in search would become impossible. The alternative is showing without activation and activating after the first frame (not measured from Explorer). **Correction (03.10.2026, FastMD 1.3.1):** under `ImmDisableIME(0)`, switching the keyboard layout with the shortcut from the settings (Ctrl+Shift or Alt+Shift; Win+Space works) does not work in the thread either — it reaches the thread only through TSF. It is cured by our own `ITfThreadMgr::Activate` after the first frame (≈5 ms, startup does not see it); IME is still absent from the thread.
- **The DWM open animation** adds ≈40–80 ms after the rendered frame. It is cured by `DWMWA_TRANSITIONS_FORCEDISABLED`.
- **The keyboard layout cache in winit and tao** (Rust toolkits, Tauri) costs 150–350 ms in the first `ShowWindow`. It is cured only by patching winit.

### 3.5 Runtimes are not the main problem; the frameworks on top of them are

- **.NET itself:** NativeAOT adds ≈4 ms to the native minimum, the CLR with JIT ≈15 ms ([05 §7](research/05-windows-startup-engineering.en.md)).
- **WinUI 3:** from `main` (57 ms) to `activate` (137 ms) XAML is initializing; then up to `loaded` (270 ms) it waits for the device and the compositor.
- **WPF:** the render thread and D3D9. The prototype got down to 287 ms only with a native launcher, D3D9 preloading and without the Aero2 theme; the "textbook" variant takes 487 ms.

### 3.6 What does not matter or was not measured

- **The parser does not matter:** md4c parses 3.7 MB in 10.6 ms, pulldown-cmark in 16.8 ms ([04](research/04-markdown-pipeline.en.md)).
- **Static or dynamic CRT, manifest:** ±2 ms.
- **Code signing was not measured.** According to [05 §2.2](research/05-windows-startup-engineering.en.md), a known small exe is created in 4–6 ms against 18–24 ms for a new unsigned one. Signing and reputation may give up to ~15 ms.
- **MSIX was not measured directly** (no Developer Mode). Indirectly: AppData virtualization slows file I/O down by 1.3–2.4 times. No speed gain is expected.

### 3.7 The "bullet" budget (`cpp-d2d`, medium.md)

| Mark, ms | What is ready |
|---|---|
| 17 | `CreateProcessW` has returned |
| 29 | `main` |
| 30 | the file is read and parsed by md4c |
| 31 | DirectWrite is ready |
| 40 | layout of the first screen |
| 41 | the window is created |
| 43 | the frame is drawn in memory |
| 51 | the window is shown |
| 52 | the frame is copied to the window (BitBlt) |
| **59** | **`DwmFlush`: the frame is on screen** |

Our own document pipeline (reading, md4c, DirectWrite, layout and rendering of the first screen) takes ≈14 ms and runs in parallel with window creation. **The critical path after `main` is OS calls:**
- `CreateWindowEx` with injected hooks: ≈11 ms, of which RTSS is ≈10;
- `ShowWindow`: ≈8–12 ms;
- waiting for DWM composition: ≈7 ms.

There is almost no slack left inside the application.

---

## 4. Visual quality

Screenshots of medium.md are in [`bench/results/final/shots/`](bench/results/final/shots/). This is how the stacks ranked in the audit:

- **9/10.** The native `cpp-d2d` matches the Chromium reference in layout: headings at the same Y coordinates, tables, color emoji, CJK. Its task checkboxes are neater than Chromium's. litehtml, WebView2, Electron and wry are at the same level — they share a GitHub-style `style.css`.
- **8–8.5/10.** Qt, WinUI 3, Avalonia, WPF, Blitz.
  - WinUI 3, Avalonia and WPF have no rounded corners on inline code, and selection works only within a single block.
  - In WPF, emoji are monochrome.
  - Blitz has no code highlighting.
- **7/10.** RichEdit: monochrome emoji, links always underlined, no rounded corners. egui: grayscale antialiasing without ClearType, monochrome emoji, synthetic italics.
- **6.5/10.** iced: punctuation after a bold fragment or inline code inherits its style.

Conclusion: **on the GFM core, choosing by speed costs nothing in looks.** Raw HTML, SVG badges and remote images were not in the corpus and were not checked. A native engine has yet to implement them (§6).

A note on typography. The benchmark spec (an 860 DIP column, 15 DIP text) gives ≈119 characters per line. For the product, [06 §10.1](research/06-product-ux-landscape.en.md) recommends 16 DIP text and a column of up to 680 DIP, with tables, code and images wider than the column.

---

## 5. WinUI 3 in particular

For the C# path the main "knobs" were tested:
- NativeAOT;
- self-contained WinAppSDK 1.8;
- a custom `Main`;
- the document is built in code, without `XamlControlsResources`;
- first screen first;
- parsing the start of the document separately from the rest.

| Variant | medium.md |
|---|---|
| Best (`winui3`) | **324 ms** |
| JIT + R2R | 384 ms |
| With Fluent resources and Mica | 449 ms |
| With WebView2 inside | 497 ms |

**Why it will not get faster:**

1. **The compositor waits ≈135 ms for the GPU device** on the critical path, between `activate` (137 ms) and `loaded` (270 ms). The device itself costs ≈200 ms on NVIDIA, but it is partly created in parallel. There is no public way to switch XAML to WARP: the strings `ForceWarp` and `SoftwareRendering` are present in `Microsoft.UI.Xaml.dll`, but there is no API.
2. **Before `Activate`, WinUI spends 137 ms even before waiting for the GPU.** That is twice the entire `cpp-d2d` path.
3. **Mica in WinUI costs +125…240 ms**, because `SystemBackdrop` creates a GPU device synchronously. In plain Win32 the same Mica via `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)` is almost free ([05 §5](research/05-windows-startup-engineering.en.md)).
4. **A 58 MB distribution** in the self-contained build. Relying on an installed Windows App Runtime 1.8 gives 8 MB at the same speed.

**Not tested:**

| Knob | Expected effect |
|---|---|
| C++/WinRT instead of C# | ≈−30 ms before `main` |
| `ImmDisableIME` or activation after the frame | ≈−10…20 ms |
| A per-app GPU preference for the iGPU | unlikely to help: on this machine the cross-adapter chain on the iGPU itself costs ≈190 ms (`cpp-d2d --igpu`) |
| Resident mode | the prototype author's estimate is 100–150 ms |

**Even if everything works, a cold start is ≈260–290 ms. The conclusion does not change.**

**Effort.** By the prototype authors' estimates, an MVP is comparable to `cpp-d2d` (6–8 weeks). But WinUI is closer to a full product: 12–14 weeks against "6–8 weeks + 3–4 months" for DirectWrite. The difference is mostly accessibility (UIA) and ready-made controls. RichTextBlock gives selection across paragraphs, but it cannot hold tables and code blocks without InlineUIContainer and cannot virtualize. We pay this difference for a fivefold win at startup.

**Verdict:** do not use WinUI 3 for rendering the document. It can be used only for the "rich shell" (settings, dialogs), loaded after the first frame through XAML Islands, as in Windows 11 Notepad. But native Win32 with Mica via DWM already looks like Windows 11, so this is not needed in v1.

---

## 6. The recommended FastMD architecture

```
fastmd.exe  (C++20, Win32, /SUBSYSTEM:WINDOWS, static CRT, /GS + CFG в релизе, < 2 МБ; редкое — delay-load DLL)
 ├─ старт: ImmDisableIME(0) только для UI-потока документа (поиск с IME — отдельный поток или лениво);
 │         DWMWA_TRANSITIONS_FORCEDISABLED; Mica/тёмный заголовок через DWM; без COM/WinRT на критическом пути
 ├─ парсинг: md4c в UTF-16 (свой форк, закреплённый коммит master 0.6.0-dev ради сносок и Alerts;
 │           до перехода проверить UTF-16-сборку и регрессию автоссылок; в CI — CommonMark + GFM-spec)
 ├─ модель: блоки + inline-прогоны; у блока свой IDWriteTextLayout; LRU-кэш вёрсток (все вёрстки large.md = +1,5 ГБ)
 ├─ отрисовка: ОДИН программный бэкенд — DirectWrite → GDI DIB (как в прототипе). Замер прокрутки прототипа:
 │            GDI 2,1 мс/кадр, WARP 2,7, аппаратный D2D 11,1 (1000×800); при 3300×1300 — 6,4 / 3,9 / 9,0.
 │            Второй бэкенд (D2D WARP/HW) — только если замер на слабом ноутбуке/4K покажет нехватку
 ├─ вёрстка: только первый экран до первого кадра; остальное — в фоне, высоты сначала оценочные
 ├─ подсветка: Lexilla (лексеры Scintilla): 0,1–0,3 мс инициализации, 22–36 МБ/с — синхронно
 ├─ эмодзи: послойный COLR через TranslateColorGlyphRun, как в прототипе (59 мс уже с цветными эмодзи на экране)
 ├─ картинки: WIC — размер синхронно (0,1 мс), декодирование асинхронно; удалённые — асинхронно, с дисковым кэшем
 │            и настройкой приватности; SVG с текстом — lunasvg или resvg
 ├─ HTML-подмножество (06 §4.3: img/align/width, p/div align, br, details/summary, picture/prefers-color-scheme,
 │   sub/sup/kbd, простые table) — одно из двух: (а) свой мини-DOM внутри движка; (б) litehtml как движок ВСЕГО
 │   документа (прототип: 103 мс, 9/10). Не «litehtml только для HTML-блоков»: в README HTML чаще inline или
 │   оборачивает Markdown, а два движка в одном документе — это раздельные выделение и поиск
 ├─ math/mermaid: v1 — как оформленный код; v1.x — RaTeX (≈0,07 мс/формула) и mmdr (≈40–90 мс) в фоне;
 │                запасной путь — WebView2 → SVG-кэш (Mermaid с htmlLabels:false; окно cloaked, а не скрытое —
 │                иначе не срабатывают rAF/getBBox; virtual host только в зоне .example/.invalid;
 │                один экземпляр WebView2 на процесс)
 └─ резидент + DelegateExecute — только если замер на слабом ноутбуке даст холодный двойной клик > 150 мс
```

RaTeX, mmdr and resvg are written in Rust. A C++ product will need cargo and FFI for them (or lunasvg plus WebView2). This is an argument for considering Rust for the whole project (§7).

**The startup budget:**
- ≈60 ms to the first screen when a new process is launched on a warm cache;
- ≈86 ms from a double click, because Explorer adds ≈27 ms;
- a resident with DelegateExecute would cut this to ≈35–40 ms. This is an estimate based on the trivial viewer from [05 §8.2](research/05-windows-startup-engineering.en.md); the 28 ms in §2 were measured through `--forward`, not through DelegateExecute. It is below the threshold of perception, and [06 §7.3](research/06-product-ux-landscape.en.md) considers a resident unnecessary for the native path.

**v1 features** (per [06 §10.1](research/06-product-ux-landscape.en.md)):
- **rendering:** GFM, alerts, footnotes, front matter, emoji shortcodes, the HTML subset;
- **images:** local, remote, and SVG with text;
- **reading:** search, table of contents, zoom, selection and copying, code copying;
- **navigation:** anchors, following .md links with "Back";
- **environment:** live reload that keeps the scroll position, a theme that follows the system (yours is dark), "Open in editor", the .md association, drag-and-drop, Jump List.

**Deliberately outside v1:** RTL/bidi documents, hyphenation, printing and PDF.

**v2:** a native preview handler for the Explorer pane. The PowerToys preview opens in 0.6–1.4 s; we can be 5–10 times faster (an estimate of ≈50–150 ms, not measured).

**Effort** (the prototype authors' estimates, not calibrated):
- an MVP based on `cpp-d2d` ≈6–8 weeks: selection and copying, search, zoom, the dark theme, live reload, anchors and the table of contents, horizontal scrolling of code and tables, the association, an installer;
- footnotes, the HTML subset, SVG, remote images, UIA and printing are not included. The same author estimated them at another ≈3–4 months;
- **realistically:** v1 per the list above takes 3 months or more for one experienced C++/Win32 developer, a product with accessibility ≈5–6 months. To be refined after the spike (§9).

---

## 7. Decision matrix: what to choose under which priorities

MVP and product figures are estimates from the prototype READMEs.

| Stack | medium, ms | Look | What comes for free | What we write ourselves | MVP → product | macOS/Linux |
|---|---|---|---|---|---|---|
| **cpp-d2d** (recommended) | **59** | 9 | — | selection, search, copying, UIA, HTML, printing | 6–8 wk → +3–4 mo | no |
| RichEdit, windowless / windowed | 69 / 84 | 7 | for the windowed one: selection, RTF copying, search, zoom, printing, IME, UIA | the look: emoji, rounded corners, links | 3–5 wk | no |
| litehtml (whole document) | 103 | 9 | HTML/CSS layout, theming via CSS | selection, search, UIA; layout is superlinear (3.7 MB in one piece > 30 s) → cut into chunks | ≈2–2.5 mo | partly |
| Blitz (Rust) | 112 | 8 | real CSS (Stylo), AccessKit | search, virtualization, a winit patch; beta | 5–7 wk + maintenance | yes |
| Avalonia 12 NativeAOT, Skia CPU | 143 | 8 | controls, .NET | selection across blocks; 31 MB | 4–6 wk | yes |
| Qt 6 Widgets | 149 static / 154 DLL | 8.5 | selection, search, IME, UIA, printing, HTML blocks | styling by hand (cut-down CSS) | 3–5 wk → 2–3 mo | yes |
| WinUI 3 (C# AOT) | 324 | 8 | UIA, controls | document layout from blocks | 6–8 wk → 12–14 wk | no |
| WebView2 (C++) | 447 (resident 65) | 9 | all of HTML/CSS/JS, Mermaid, KaTeX | the shell | 3–4 wk → +4–6 wk | no |

**How to read it:**
- **The main thing is the "bullet":** `cpp-d2d`.
- **The main thing is shipping v1 quickly:** RichEdit, which looks worse, or Qt, which is about 90 ms slower but gives selection, search, IME and UIA out of the box.
- **macOS/Linux needed within the next 1–2 years:** decide before work starts (§8). A DirectWrite engine is not portable; on macOS it would have to be rewritten on CoreText. Options:
  - Qt: official DLLs, 154 ms. The 149 ms static build under the LGPL requires handing out object files or buying a commercial license;
  - a shared core (model, block layout, selection, search) with a thin shaping and raster layer per platform ([03 §6.1](research/03-gpu-and-crossplatform-native-ui.en.md)).
- **Rust is preferred** (memory safety when parsing untrusted files; comrak, RaTeX, mmdr and resvg are all Rust): Rust + windows-rs + DirectWrite with the same architecture. By construction the speed is the same (§3.7), but **there is no prototype; not measured**. If ready-made CSS/HTML is needed, Blitz. pulldown-cmark does not support GFM autolinks, so take comrak.
- **Exact Chromium layout needed for all documents:** WebView2, resident only. 65 ms in steady state, 165–280 ms for the first document after the resident starts. The resident holds ≈150–260 MB in the background.

**Do not take:**
- **Electron:** 0.75–0.9 s, 324 MB.
- **Tauri:** 630 ms.
- **WPF:** 287 ms, and even that only with heroic hacks.
- **Sciter:** closed source, 320 ms, layout defects.
- **GPU-first toolkits** (egui and iced on wgpu, Blitz-GPU): 324–735 ms.
- **egui/iced in the fast configuration:** relies on patches to winit and wgpu, visuals 6.5–7.

---

## 8. Risks and open questions

1. **The effort of our own engine.**
   - The hardest parts: selection across blocks and table cells, search in a virtualized document, copying as text/HTML, UIA `TextPattern`, the HTML subset.
   - Mitigation: a spike in order of decreasing risk (§9), building UIA into the architecture from the start, and a fallback to RichEdit/litehtml/Qt by a criterion set in advance.
2. **Untrusted input.** A .md from the internet is parsed without a sandbox by C/C++ code: md4c, WIC, lunasvg, the HTML subset. Links go to `ShellExecute`. What is needed:
   - an allowlist of schemes: `http`, `https`, `mailto` and relative .md; `file:`, UNC and third-party protocols only with confirmation;
   - fuzzing of the parser, HTML and SVG;
   - `/GS` and CFG in release (the prototype was built with `/GS-`);
   - an "always / ask / never" setting for remote images, because tracking pixels reveal the IP.
3. **Decide on cross-platform support before work starts.** If it is "Windows only", record that as a deliberate decision. Otherwise see §7.
4. **Scrolling on weak CPUs and in 4K was not measured.** On this machine the CPU path is faster than the hardware one. Run `--scroll-test` on a 2–4-core laptop before choosing the backend.
5. **Everything was measured on one machine with a discrete NVIDIA GPU** (§1, caveat 6). Before the final decision, run the harness on a laptop with only an iGPU: `pwsh -File build.ps1` and `bench.py run` are portable.
6. **IME.** The 59 ms were obtained with IME disabled for the UI thread; without that, ≈+10 ms. The search box with CJK input lives on a separate thread. Showing without activation was not measured in the final run and was not checked from Explorer.
7. **Resident mode, if it is needed.** Questions arise:
   - handing over the foreground (`CoAllowSetForegroundWindow`);
   - updating the exe while the resident is alive;
   - working set trimming after idle time (not measured);
   - EcoQoS throttling of a background process;
   - the fallback cloaked window is tied to the virtual desktop and the monitor/DPI where it was created;
   - one process for all windows is a single point of failure ([06 §9](research/06-product-ux-landscape.en.md) recommends a process per window);
   - autostart and "yet another background process" in the user's eyes.
8. **Delivery.**
   - Installation without MSIX, into HKCU: ProgID, RegisteredApplications, and with a resident also a COM LocalServer32.
   - Code signing, otherwise SmartScreen warns on the first run.
   - Our own auto-update.
   - Do not take MSIX: AppData virtualization, activation not measured, and an arbitrary DelegateExecute probably cannot be registered through the manifest.
9. **Peculiarities of your machine that affect any application:**
   - Windhawk: ≈5 ms of CPU per process;
   - RTSS: ≈10 ms on the first window;
   - `TextInputHost.exe` had accumulated 139 thousand seconds of CPU and is probably malfunctioning. Restarting it, or signing out and back in, will most likely make the first window activation cheaper for programs without IME disabled. The final run did not check this.

---

## 9. Next step

1. **Make the final decision after the spike**, by criteria set in advance.
2. **A 2–3-week spike based on `bench/protos/cpp-d2d`**, in order of decreasing risk:
   - selection across blocks and table cells, copying as text/HTML, search with highlighting in a virtualized document;
   - the HTML subset from [06 §4.3](research/06-product-ux-landscape.en.md), SVG badges (lunasvg) and remote images on 20–30 real READMEs with a screenshot comparison against GitHub;
   - a minimal UIA `TextPattern` checked with Narrator;
   - a search box with IME on a separate thread;
   - the harness and `--scroll-test` in 4K on a laptop with only an iGPU and a weak CPU;
   - Mica and the dark theme are almost free — not a priority.
3. **Fallback criterion:** if selection or the HTML subset exceed the estimate by more than 1.5 times, switch to litehtml (whole document) or Qt. If a fast v1 is needed at any cost in looks, switch to RichEdit.
4. **A speed regression test.** A nightly run on a dedicated machine with an interactive session, in one interleaved run with the reference. Criteria:
   - `t_content(fastmd) − t_content(baseline-win32)` ≤ 0 ms on medium and ≤ +15 ms on large;
   - monitoring of the `main → layout_first_viewport` phase.

---

## Appendix A. All 47 variants (final run, median in ms)

| # | variant | stack | small | medium | large | p90 med | window med | CPU ms | commit MB | dist. MB | visuals |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `cpp-d2d-resident` | C++ D2D/DWrite, resident process (hand-off) | 29 | **28** | 29 | 29.3 | 21.2 | 15.6³ | 1.8³ | 0.4 | 9 |
| 2 | `cpp-d2d` | C++ Win32 + DirectWrite → GDI DIB + md4c | 60 | **59** | 61 | 61.1 | 49.3 | 31.2 | 7.0 | 0.4 | 9 |
| 3 | `cpp-webview2-resident` | WebView2, resident process (hand-off) | 65 | **65** | 84 | 69.0 | 54.8 | 15.6³ | 1.7³ | 0.52 | 9 |
| 4 | `cpp-richedit-wl-mt` | C++ RichEdit windowless (GDI) + md4c→RTF | 70 | **69** | 72 | 72.5 | 55.8 | 46.9 | 4.5 | 0.26 | 7 |
| 5 | `rust-gpu-egui-softfirst-noime` | egui softfirst + no IME | 76 | **76** | 77 | 82.0 | 68.2 | 46.9 | 10.1 | 7.54 | 7 |
| 6 | `rust-gpu-egui-softfirst` | egui (first frame on the CPU, patched winit) | 78 | **77** | 83 | 94.5 | 70.1 | 62.5 | 10.1 | 7.54 | 7 |
| 7 | `baseline-win32` | Reference: an empty Win32+GDI window | 77 | **78** | 78 | 92.5 | 72.3 | 31.2 | 3.2 | 0.15 | — |
| 8 | `cpp-richedit-d2d` | RichEdit windowless via D2D/DirectWrite | 79 | **83** | 86 | 92.7 | 69.9 | 46.9 | 61.2 | 0.26 | 7 |
| 9 | `cpp-richedit` | RichEdit (regular control) + md4c→RTF | 80 | **84** | 86 | 85.6 | 78.3 | 62.5 | 4.3 | 0.25 | 7 |
| 10 | `cpp-d2d-warp` | C++ + Direct2D on WARP (CPU) | 75 | **91** | 92 | 93.2 | 49.8 | 93.8 | 75.8 | 0.4 | 9 |
| 11 | `cpp-richedit-full` | RichEdit windowed, whole document at once | 90 | **103** | 998 | 104.3 | 97.4 | 62.5 | 5.1 | 0.25 | 7 |
| 12 | `litehtml-sciter-litehtml` | litehtml + our own D2D/DWrite container | 90 | **103** | 102 | 104.0 | 65.5 | 101.6 | 87.0 | 1.79 | 9 |
| 13 | `blitz` | Blitz (Stylo+Taffy+Parley) CPU | 111 | **112** | 115 | 120.3 | 108.4 | 101.6 | 27.7 | 8.0 | 8 |
| 14 | `blitz-full` | Blitz, whole document at once | 108 | **112** | 2478 | 115.6 | 106.4 | 125.0 | 27.6 | 8.0 | 8 |
| 15 | `rust-gpu-iced-tinyskia` | iced 0.14 (tiny-skia CPU) | 120 | **129** | 183 | 132.6 | 123.4 | 242.2 | 64.5 | 7.78 | 6.5 |
| 16 | `avalonia` | Avalonia 12 NativeAOT (DLL+launcher), Skia CPU | 132 | **143** | 143 | 149.7 | 123.5 | 109.4 | 42.3 | 31.26 | 8 |
| 17 | `qt` | Qt 6.12 Widgets (static, in a DLL) | 141 | **149** | 151 | 161.3 | 141.8 | 109.4 | 13.9 | 20.12 | 8.5 |
| 18 | `qt-dll` | Qt 6.12 (official DLLs) | 150 | **154** | 160 | 159.1 | 148.2 | 109.4 | 68.5 | 26.8 | 8.5 |
| 19 | `qt-full` | Qt, whole document at once | 137 | **155** | 721 | 160.0 | 144.8 | 125.0 | 15.0 | 20.12 | 8.5 |
| 20 | `avalonia-exe` | Avalonia 12 NativeAOT exe, Skia CPU | 149 | **163** | 164 | 168.0 | 143.8 | 101.6 | 42.4 | 31.17 | 8 |
| 21 | `litehtml-sciter-litehtml-gpu` | litehtml + D2D HwndRenderTarget (GPU) | 241 | **259** | 260 | 278.7 | 254.5 | 226.6 | 84.8 | 1.79 | 9 |
| 22 | `cpp-d2d-gpu` | C++ + Direct2D on hardware D3D11 | 232 | **261** | 258 | 272.8 | 49.8 | 250.0 | 133.9 | 0.4 | 9 |
| 23 | `wpf` | WPF .NET 8 R2R + a native launcher | 295 | **287** | 295 | 295.7 | 278.3 | 429.7 | 109.8 | 1.75 | 8 |
| 24 | `wpf-hw` | WPF, GPU from the first frame | 291 | **292** | 311 | 313.6 | 282.2 | 484.4 | 135.6 | 1.75 | 8 |
| 25 | `litehtml-sciter-sciter` | Sciter.JS 6 | 319 | **320** | 326 | 326.1 | 305.3 | 234.4 | 137.4 | 18.75 | 7 |
| 26 | `winui3` | WinUI 3 NativeAOT, XAML | 302 | **324** | 330 | 329.2 | 122.0 | 375.0 | 163.6 | 58.08 | 8 |
| 27 | `rust-gpu-egui-wgpu` | egui on wgpu DX12 | 323 | **324** | 324 | 332.3 | 96.6 | 273.4 | 382.1 | 7.54 | 7 |
| 28 | `wpf-apphost` | WPF, the stock apphost | 336 | **333** | 333 | 344.9 | 322.5 | 406.2 | 56.2 | 1.75 | 8 |
| 29 | `avalonia-gpu` | Avalonia 12 NativeAOT, ANGLE/D3D11 | 335 | **344** | 326 | 356.1 | 288.3 | 312.5 | 189.9 | 31.26 | 8 |
| 30 | `winui3-r2r` | WinUI 3 JIT+ReadyToRun | 362 | **384** | 388 | 400.1 | 206.1 | 460.9 | 170.0 | 100.77 | 8 |
| 31 | `cpp-webview2` | C++ + WebView2 (tuned) | 429 | **447** | 445 | 454.8 | 81.6 | 789.0 | 150.6 | 0.52 | 9 |
| 32 | `winui3-fluent` | WinUI 3 + Fluent resources + Mica | 426 | **449** | 449 | 476.6 | 330.3 | 429.7 | 172.1 | 59.37 | 8 |
| 33 | `cpp-webview2-default` | C++ + WebView2 (untuned, GPU process, whole document) | 445 | **465** | 2105 | 473.8 | 81.3 | 1195.3 | 181.7 | 0.52 | 9 |
| 34 | `rust-webview` | Rust wry + WebView2 | 468 | **473** | 474 | 484.4 | 87.9 | 1234.3 | 183.9 | 2.46 | 9 |
| 35 | `rust-webview-tao` | Rust wry + tao + WebView2 | 470 | **473** | 483 | 506.0 | 199.7 | 1265.6 | 182.5 | 2.46 | 9 |
| 36 | `ext-notepad` | Notepad (md syntax; to the window; restores tabs) | 464 | **473** | 714 | 497.0 | 473.4 | 609.4 | 92.2 | — | — |
| 37 | `wpf-plain` | WPF "textbook" | 489 | **487** | 491 | 509.9 | 476.5 | 562.5 | 81.2 | 0.67 | 8 |
| 38 | `winui3-webview2` | WinUI 3 + WebView2 | 479 | **497** | 495 | 519.9 | 131.2 | 1070.3 | 280.1 | 58.08 | 9 |
| 39 | `avalonia-stock` | Avalonia 12 by default (JIT, Fluent, ANGLE) | 570 | **570** | 574 | 587.7 | 497.5 | 578.1 | 186.4 | 37.47 | 8 |
| 40 | `rust-webview-tauri` | Tauri 2 | 591 | **630** | 668 | 663.7 | 511.8 | 1367.2 | 200.4 | 5.64 | 9 |
| 41 | `blitz-gpu` | Blitz on the GPU (vello_hybrid/wgpu) | 742 | **735** | 749 | 750.3 | 727.9 | 656.2 | 236.1 | 10.51 | 8 |
| 42 | `electron` | Electron 44 (tuned, packaged) | 727 | **753** | 744 | 757.4 | 627.6 | 523.4 | 93.8 | 323.54 | 9 |
| 43 | `electron-gpu` | Electron 44 with a GPU process | 805 | **808** | 809 | 823.2 | 622.1 | 687.5 | 174.4 | 323.54 | 9 |
| 44 | `electron-default` | Electron "typical app" | 853 | **903** | 1391 | 932.4 | 750.4 | 945.3 | 205.9 | 367.59 | 9 |
| 45 | `ext-vscode` | VS Code 1.113 (editor; to the window with the file name) | 1158 | **1138** | 1149 | 1183.2 | 1138.4 | 1593.8 | 332.2 | 1264 | — |
| 46 | `ext-void` | Void (your .md association; clean profile, onboarding on top) | 1442 | **1444** | 1441 | 1471.9 | 1444.2 | 1726.5 | 350.8 | 449 | — |
| 47 | `ext-cursor` | Cursor 2.5 (login screen on top) | 2338 | **2351** | 2362 | 2409.2 | 2351.2 | 3070.4 | 638.8 | 782 | — |

How to read the columns:
- **"window"** is the median of the self-reported `t_window`, the first painted window. For `ext-*` it is the moment the window appears as observed by the harness.
- **CPU** is the total CPU time of all processes in the Job Object by the moment of `t_content`, with a granularity of 15.6 ms.
- **commit** is the peak committed memory of the Job Object.
- ³ For `*-resident`, CPU and commit refer to the launcher only. The `cpp-d2d` resident holds another ≈20–30 MB, the WebView2 resident ≈150–260 MB.
- **Failures in the final run: 0** of 1,833 launches. 14 of them stalled for 2.1–2.7 s inside `CreateProcessW` (§1, caveat 5).

## Appendix B. Where things are

**Theory:**

| Report | Topic |
|---|---|
| [01](research/01-native-ui-frameworks.en.md) | native UI frameworks |
| [02](research/02-web-and-html-engines.en.md) | web and HTML engines |
| [03](research/03-gpu-and-crossplatform-native-ui.en.md) | GPU and cross-platform toolkits |
| [04](research/04-markdown-pipeline.en.md) | the Markdown pipeline: parsers, highlighting, math, Mermaid |
| [05](research/05-windows-startup-engineering.en.md) | Windows startup engineering |
| [06](research/06-product-ux-landscape.en.md) | product and UX, competitors |

The researchers' lab experiments (`research/lab/`) and the prototype builds were deleted on 19.09.2026 to free up ~43 GB; all numbers and conclusions are preserved in the reports and the prototype READMEs. The utility for launching from the Explorer context is kept in `bench/tools/explorer-launch/`.

**Practice:**
- `bench/protos/<id>/`: sources, `build.ps1`, `proto.json`, a `README.md` with a timing breakdown and an "Audit" section.
- The harness and protocol: `bench/harness/`, `bench/PROTOCOL.md`.

**Reproducing the final run:** `bench/results/final/run_final.cmd`, run from an ordinary terminal or through Explorer. The summary table: `python bench/harness/final_table.py bench/results/final/main.json bench/results/final/res-*.json`.
