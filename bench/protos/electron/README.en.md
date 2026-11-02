[Русский](README.md) | **English**

# FastMD — Electron prototype

A real Markdown viewer on Electron (the Typora / Obsidian / MarkText class), measured by the shared harness
`bench/harness/bench.py` according to `bench/PROTOCOL.md`. The goal is to find the fastest **honest** launch configuration for
this stack and to show where the time goes.

## Summary in brief

* The best variant, `electron`: **~880–930 ms** to the first screen (median, small/medium/large); the window is visible after ~765 ms.
  For comparison: the calibration `baseline-win32` takes 70–90 ms, VS Code ~1.5 s, Notepad ~0.5 s.
* **~60 % of the time (~480–570 ms) is spent before the application's first instruction**: `CreateProcessW` for
  an unsigned 246 MB `FastMD.exe` (Defender scans the image on every launch). This cannot be fixed from inside
  the application.
* Everything after the process starts (Node + Chromium + window + renderer + first layout) is **~260–290 ms**, and
  tuning has squeezed out nearly all of it: what is left is ~100 ms of first layout/frame and ~120 ms of unavoidable Chromium initialization.
* The main effective lever is **turning off the GPU** (software compositing, no separate GPU process): −65…−140 ms
  and −80 MB commit. The other knobs give single-digit milliseconds or are within noise.
* Conclusion for choosing a stack: Electron hits a floor of **~0.85–0.9 s** on this machine. It will not open "like a bullet" (<200 ms)
  without a resident process.

## Stack and versions

| Component | Version |
|---|---|
| Electron | 44.4.3 (Chromium 152.0.7977.130, Node 24.21.0, V8 15.2.124.28) |
| Markdown | markdown-it 15.0.2 (CommonMark + GFM tables + strikethrough) + our own task lists plugin (~25 lines) |
| Syntax highlighting | highlight.js 11.12.0: in `electron` — core + 19 languages, registered lazily on first use; in `electron-default` — the full package (~190 languages) |
| DwmFlush from JS | koffi 3.3.0 (FFI, `dwmapi.dll!DwmFlush`), loaded lazily while the renderer does layout |
| Build | esbuild 0.28.2 (main / preload into one minified CJS file), npm 11.16, Node 24.18 |
| Styles | `bench/common/style.css` inline + the GitHub light palette for highlight.js (`src/hljs-github.css`) |

## How to build

```powershell
pwsh -File bench/protos/electron/build.ps1
```

* If `node_modules` is missing, it runs `npm ci` from `package-lock.json`, then `node node_modules/electron/install.js`
  (Electron ≥ 42 has no postinstall; the zip cache is in `bench/.tools/electron/cache`, not in `%LOCALAPPDATA%`).
* It recreates `out/`: `out/FastMD/` — the packaged application (a copy of the Electron dist, `electron.exe` → `FastMD.exe`,
  `resources/app/` = the esbuild bundle + page.html with embedded CSS + koffi; `default_app.asar` removed; of the locales
  only en-US and ru are kept), `out/app-default/` — an "ordinary" application folder for the default variant.
* Checked from scratch: `npm ci` (14 s) + `build.ps1` (5 s). npm 11 warns that the esbuild/koffi install scripts
  were not run (allow-scripts) — this does no harm: the binaries come from optional dependencies
  (`@esbuild/win32-x64`, `@koromix/koffi-win32-x64`).
* The Chromium profiles are created on first launch in `out/userdata-tuned/` and `out/userdata-default/` (outside `dist_dir`).

## Variants (`proto.json`)

| id | What it is |
|---|---|
| **`electron`** (best) | The packaged application `out/FastMD/FastMD.exe`; esbuild bundle; no application menu; `app.disableHardwareAcceleration()` + `--disable-software-rasterizer` (software compositing, no GL/WARP) + `--in-process-gpu` (no separate GPU process); window `show:false` → `loadFile()` (starts the renderer) → `show()`; while the renderer starts, main reads and parses only **the first ~4K characters** (up to a safe block boundary); the renderer fetches them via `ipcRenderer.invoke`, inserts them, double rAF → IPC → `DwmFlush()` → `t_content`. The rest is appended after the first frame in sections with `content-visibility:auto`. Files > 256 KB: only the "head" (256 KB) is read before the first frame, the tail after it (if the first chunk contains a possible reference link `[x][id]`/`[id]`, the whole file is read to find the definition). sandbox + contextIsolation are on. |
| `electron-gpu` | The same, but with Chromium's default graphics: hardware acceleration (ANGLE/D3D11) in a separate GPU process (`FASTMD_OPTS=gpu,gpuproc`). Shows the cost of the GPU process on the first frame. |
| `electron-default` | "A typical Electron application": `electron.exe <folder>` (via `default_app.asar`), `require()` from node_modules, default menu/GPU/webPreferences, the whole document is parsed (with the full highlight.js) **before** the window is created, the HTML is written to a temporary file → `loadFile()`, reported when the first screen of the progressively parsed page has reached the DOM, + double rAF (before the audit — after DOMContentLoaded, see "Audit"). |

The experimental knobs remain in the code (`FASTMD_OPTS`: `gpu, gpuproc, swrast, nosandbox, single, netinproc,
noocclusion, menu, nohl, nochunk, chunk8k, worker, showfirst, datashell, mempart`; `scrollend` is for debugging: scroll to the end after
the remainder arrives; `FASTMD_SWITCHES` — arbitrary Chromium switches); the results are in the "Knob experiments" section.

### Measurement honesty

* `t_content` is taken in the main process (it owns the window) only after the renderer has inserted the HTML into the DOM and
  **two requestAnimationFrame** callbacks have passed, then IPC `painted` → `DwmFlush()` (koffi) → mark. We never mark earlier.
* The first screen is the real rendered document (headings, lists, code, tables, styled), not raw text and not a
  placeholder; 4096 characters of prose are guaranteed to be more than one 1000×800 screen (≈ 37 lines). Sending the rest after
  the first frame is the legitimate "first viewport first" technique (PROTOCOL §3).
* No resident processes, no caches of rendered HTML, no embedded corpus. Chromium's own disk caches
  (the `userdata-*` profile) are the standard behavior of any Electron application.
* The result is written atomically (tmp + rename), then `app.exit(0)`; the harness sees a clean exit.

## What is supported (per spec §5)

| Element | `electron` | Note |
|---|---|---|
| h1–h6 with sizes, border under h1/h2 | yes | from `style.css` |
| Paragraphs with word wrap | yes | |
| **bold** / *italic* / `code` / ~~strike~~ | yes | |
| Links (color #0969da) | yes | click → external browser (`shell.openExternal`), navigation inside the window is blocked |
| Bulleted, numbered, nested lists | yes | |
| Task lists | yes | our own plugin: disabled checkboxes, `ul.contains-task-list` |
| Blockquotes, horizontal rules | yes | |
| Code blocks | yes | Cascadia Mono, background, **no wrapping** (horizontal scrolling of `pre`) |
| Syntax highlighting | yes | highlight.js, 19 languages (rust, csharp, cpp, c, python, js, ts, go, java, json, yaml, bash, powershell, sql, css, xml/html, diff, ini/toml, markdown) + aliases |
| GFM tables (alignment, zebra) | yes | |
| Images (local PNG/JPEG/GIF) | yes | paths relative to the .md folder and absolute `C:\...`; the size is read from the file header and reserved on the first frame (no layout "jump"); decoding is asynchronous — image pixels may appear 1–2 frames after `t_content` |
| Unicode: Cyrillic, CJK, color emoji | yes | system DirectWrite fallback |
| Reference links `[text][id]` | yes | definitions are collected across the whole file before the first chunk is rendered |
| Scrolling | yes | |
| DPI | yes | Chromium per-monitor v2 |

**Not supported:** GFM autolink literals (`www.x.com` without `<>`; `linkify:false`), footnotes, math, Mermaid,
GitHub alerts (`> [!NOTE]`), heading anchors/table of contents, raw HTML (escaped — a safe default), dark
theme, find in page, file watching, drag-and-drop, single-instance. In Electron all of this is added with
markdown-it plugins in hours, not days.

Screenshots: `results/shots/electron-medium.png`, `electron-small.png`, `electron-large.png`,
`electron-gpu-medium.png`, `electron-default-medium.png` (the default one has the File/Edit/View/Window menu and a dark window
title bar following the system theme). Images and reference links check: `results/_exp/img-test.png`. Proof that the document
is sent in full, down to the last line (medium — 2 chunks, large — the "head" + 46 tail chunks):
`results/shots/electron-medium-end.png`, `electron-large-end.png` (the variant with `FASTMD_OPTS=scrollend`; the last
section 1659.3 and the final `---` rule are in place). Equivalence of the "head+tail" HTML and the single-pass split:
`node tools_headcheck.js` (IDENTICAL for all three documents).

## Quick numbers

`python bench/harness/bench.py run electron,electron-gpu,electron-default --doc small,medium,large --runs 5`
(interleaved, 5 measurements + 1 warm-up, the machine was **not idle** — other agents were running in parallel; the final
sequential measurement is done by the orchestrator). File: `results/quick-final2.json`.

| variant | doc | median, ms | min | t_window | CPU, ms | commit, MB | WS, MB | processes | dist, MB |
|---|---|---|---|---|---|---|---|---|---|
| **electron** | small | **879** | 867 | 764 | 641 | 89 | 99 | 3 | 323.5 |
| **electron** | medium | **912** | 902 | 766 | 656 | 96 | 109 | 3 | 323.5 |
| **electron** | large | **932** | 853 | 768 | 594 | 94 | 103 | 3 | 323.5 |
| electron-gpu | small | 1017 | 953 | 784 | 984 | 172 | 97 | 4 | 323.5 |
| electron-gpu | medium | 1038 | 951 | 786 | 1031 | 175 | 97 | 4 | 323.5 |
| electron-gpu | large | 998 | 970 | 765 | 891 | 175 | 100 | 4 | 323.5 |
| electron-default | small | 1034 | 980 | 895 | 1000 | 184 | 113 | 4 | 367.6 |
| electron-default | medium | 1118 | 1072 | 928 | 1219 | 207 | 120 | 4 | 367.6 |
| electron-default | large | 3488\* | 3413 | 1409 | 4953 | 1010 | 288 | 4 | 367.6 |

\* The `electron-default` rows were taken **before the audit**, with a late mark (it waited for DOMContentLoaded): on large the document was
on screen after ~1.7 s already. After the fix: large ≈ 1.63–1.65 s (check), small/medium unchanged within noise.
Details and new numbers are in the "Audit" section.

**The first launch after a build/install** (a new 246 MB exe, cold cache) takes 1.2–3.4 s (observed:
`electron-default` 3372 ms, `electron` 1229–1905 ms right after `build.ps1`). After that, the numbers above.

### Breakdown by marks (`electron`, medium, medians, ms from `t0` = just before `CreateProcessW`)

| mark | ms | Δ | what happens |
|---|---|---|---|
| process created (`launch_overhead`) | 568 | 568 | `CreateProcessW` + Job assignment + `ResumeThread`. A bare `CreateProcessW` of this exe takes 484 ms (see below) |
| `main` — first line of main.js | 632 | 64 | Electron/Node/V8 init, `resources.pak`, `icudtl.dat`, V8 snapshot, reading the 225 KB bundle |
| `ready` | 684 | 52 | Chromium browser-process init (BrowserMainParts, in-process viz thread, network-service start) |
| `win_ctor` | 729 | 45 | `new BrowserWindow` (HWND + WebContents) |
| `load_called` | 742 | 13 | `loadFile()` → the renderer process starts |
| `win_show_called` = `t_window` | 766 | 24 | `show()`; the harness sees the window at 746 ms |
| `file_read`, `parsed` | 772, 787 | 6 + 15 | reading + markdown-it on the first 4270 characters (in parallel with the renderer start) |
| `r_preload` | 806 | 64 from `load_called` | renderer process + Blink + preload |
| `r_inserted` | 810 | 4 | HTML of the first chunk in the DOM |
| `r_raf2` | 907 | **97** | the first style/layout/paint/raster/compositor frame |
| `t_content` | 912 | 5 | IPC `painted` → `DwmFlush()` |

## Where the time goes

1. **`CreateProcessW` ≈ 480–570 ms — more than half.** `tools_createproc.py` measures a bare
   `CreateProcessW(CREATE_SUSPENDED)` + an immediate `TerminateProcess` (not a single application instruction
   runs), 5 repetitions, medians:

   | exe | size | signed | CreateProcessW |
   |---|---|---|---|
   | `out/FastMD/FastMD.exe` (Electron 44) | 246 MB | no | **484 ms** |
   | `node_modules/electron/dist/electron.exe` | 246 MB | no | 535 ms |
   | Void.exe (VS Code fork) | 191 MB | no | 436 ms |
   | Cursor.exe (2.5.26) | 211 MB | yes (Anysphere) | 271 ms |
   | Code.exe (VS Code 1.113) | 203 MB | yes (Microsoft) | 252 ms |
   | `baseline-win32/out/baseline.exe` | 0.15 MB | no | 24 ms |

   The cost grows with the image size and is roughly half for signed files — this is Defender's on-access scanning
   (real-time protection is on; exclusions cannot be added without admin rights). Repeated launches are not cached.
   Child processes (renderer, network service, GPU) of the same exe start cheaply (~50–65 ms): the image is already
   mapped while the browser process is alive. In Electron all of Chromium is statically linked into one exe, so
   it cannot be made smaller without a custom Electron build.
2. **Node + Chromium initialization ≈ 115 ms** (`launch→main` 64 + `main→ready` 52) — a fixed runtime
   cost; our code before `ready` takes 16 ms (require electron, switches, `nativeTheme`).
3. **Window + renderer ≈ 125 ms** (`ready→r_inserted`): the window constructor 45 ms, the renderer process start 64 ms.
   Markdown parsing (15–20 ms) is fully hidden inside this window.
4. **First layout and frame ≈ 90–100 ms** (`r_inserted→r_raf2`). In the Chromium trace (`results/traces/tuned_v2*`, an
   earlier build of the same variant; tracing itself slows things down) this is `LocalFrameView::performLayout` ~118 ms, mostly text shaping and font loading. An experiment
   (`results/exp13-fonts.json`, documents from `results/_exp/`): the same medium without CJK+emoji — 75 ms, pure
   ASCII — 68 ms. So **~25–30 ms is font fallback** (finding and loading the system CJK and emoji fonts;
   in the renderer sandbox these are synchronous IPCs to the font proxy in the browser process, `FontDataService::MatchFamilyName` is visible in the trace), and the remaining ~65–70 ms is cold initialization of Segoe UI Variable / Cascadia Mono,
   layout and rasterization.
5. **GPU process = +100 ms on the first frame** (`electron-gpu`: `r_inserted→r_raf2` 180–200 ms versus 90–100),
   plus +300 ms CPU and +80 MB commit. A text document does not need GPU rasterization.
6. **The default variant** loses on full parsing before the window (medium +145 ms, large +600 ms), on full
   layout of the whole document (large: ~1 s between DOMContentLoaded and the frame) and on `default_app` (+20 ms).
   A large document (3.7 MB) takes it 3.5 s and 1 GB commit, the tuned one 0.93 s and 94 MB. *(Audit: the 3.5 s is
   an artifact of the late mark; Chromium shows the first screen without waiting for parsing to finish — really ≈ 1.6–1.7 s,
   commit by `t_content` ≈ 420 MB; full layout of the whole document (~1 s) happens after the first screen.)*

## Knob experiments (medium unless stated otherwise; medians, noise ±50 ms)

| Knob | Effect | Decision |
|---|---|---|
| Software compositing (`disableHardwareAcceleration`) | −50…−130 ms (exp1, exp2, exp4, final) | **adopted** |
| `--in-process-gpu` (no separate GPU process) | −64 ms (exp2), a separate GPU process +98 ms (exp3) | **adopted** |
| `--disable-software-rasterizer` (no WARP/SwiftShader GL) | time 0 (exp12: min 912 vs 924), commit 154 → 97 MB | **adopted** |
| Packaged application without `default_app` + esbuild bundle | `launch→main` 64 vs 86 ms (−20) | **adopted** |
| First chunk of 4K characters vs the whole document (`nochunk`) | −105 ms on medium (exp1); large: ~1 s of layout without it | **adopted** |
| First chunk of 8K (`chunk8k`) | +~50–90 ms (exp2) | no |
| Reading only the "head" of files > 256 KB | large ≈ medium (932 vs 912); before, large was 40–80 ms slower | **adopted** |
| `loadFile()` before `show()` | `showfirst` +100 ms (exp2) | **adopted** (order loadFile → show) |
| Page shell as a `data:` URL (`datashell`) | +245 ms (exp2) | no |
| No menu (`Menu.setApplicationMenu(null)`) | 0 (exp4) | adopted (cleaner UI) |
| `nativeTheme.themeSource='light'` (light title bar) | 0 (exp9) | adopted |
| sandbox off (`nosandbox`) | 0…−15 ms (exp1, exp2, exp4) — within noise | no (security matters more) |
| Parser in `worker_threads` (`worker`) | small +39, medium +60, large −98 ms (exp10) | no (with the "head" there is no gain on large) |
| No highlighting (`nohl`) | ±50 ms, the sign flips between runs (exp5, 6, 11) | highlighting kept |
| `NetworkServiceInProcess2`, a separate `partition`, `CalculateNativeWinOcclusion` off | within noise (exp3, exp4) | no |
| `--disable-direct-composition` | 0 (exp12) | no |
| `--single-process` | crashes (0x80000003) | impossible |
| vsync / frame-rate limit | 165 Hz monitor: two frames = 12 ms, nothing to gain | not tried |

Raw experiment files: `results/exp1.json` … `exp13-fonts.json`, `quick-final*.json`;
analysis: `python tools_delta.py <run.json>` and `python tools_marks.py <run.json>`.

## Ideas for further speedups

1. **Sign the exe with an OV/EV certificate.** By the measurements, signed files of the same size are created roughly twice
   as fast (Code.exe 203 MB — 252 ms versus 484 ms for our 246 MB). Potentially −200 ms, but **not verified**
   on our exe (it needs a real certificate).
2. **Shrink the exe:** a custom Electron build without PDF, printing, spellchecker, WebGPU/Dawn, media — possibly
   −30…40 % of the size → −100…150 ms of scanning. The price is building Chromium (hours, ~100 GB).
3. **Resident mode (`electron-resident`):** a hidden process keeps Chromium and the exe image in memory, and a new window
   opens in ~150–250 ms (window 45 + renderer ~60 + layout ~100). Under the protocol it is allowed only as
   `-resident`; not done here.
4. The first chunk by **estimated height** rather than by 4K characters (for medium the first screen is ~1100 characters):
   −10…20 ms of layout.
5. Font fallback (~25–30 ms) cannot be removed honestly — CJK/emoji are on the first screen; at most one could warm up
   the DirectWrite fallback in the browser process in parallel with the renderer start (not tried).
6. A V8 snapshot for the main bundle: the bundle is 225 KB, the gain ≤ 10 ms.

The ceiling for Electron on this machine: ~480 ms (exe scan) + ~115 ms (Node+Chromium) + ~110 ms (window+renderer) +
~70 ms (layout) ≈ **0.78 s** unsigned; ~0.55–0.6 s signed — still 7–8 times slower than the Win32 floor.

## Effort estimate for a full product

Electron is the cheapest option to develop: the whole markdown-it ecosystem (footnotes, anchors/table of contents, KaTeX,
Mermaid, GitHub alerts), Chromium's find in page, printing/PDF, themes in CSS.

* An MVP viewer (this prototype + .md association, single-instance, file watching, dark theme): **1–2 weeks**.
* A polished product (table of contents, search, settings, math/mermaid, an electron-builder/NSIS installer,
  auto-update, **code signing**): **4–6 weeks** for one developer.
* The price: a cold start of ≥ 0.85–0.9 s, a distribution of ~320 MB unpacked (~100 MB installer), ~95 MB commit per window.
  "Like a bullet" speed is not achievable with this stack.

## Pitfalls

* **Electron ≥ 42 does not download the binary in postinstall** — an explicit `node node_modules/electron/install.js` is needed
  (and `electron_config_cache`, so the cache does not end up in the user profile).
* npm 11 (`allow-scripts`) does not run the esbuild/koffi install scripts — it works thanks to prebuilt optional deps.
* DwmFlush from Electron is only possible via a native module/FFI (koffi, ~10 ms to load): load it lazily while layout
  is running, otherwise it is on the critical path.
* `app.disableHardwareAcceleration()` and Chromium switches — strictly before `ready`.
* `--single-process` crashes in Electron 44; a `data:` page shell is slower than `loadFile()`.
* With the sandbox on, preload cannot `require` arbitrary modules — hence preload is a separate small file and parsing happens in main.
* `worker_threads` costs ~35 ms at main startup — a loss for small documents.
* The first launch after install/build takes 1.2–3.4 s (Defender scans the new 246 MB file, cold cache).
* The Chromium profile (~1.7 MB, `Local State`, `Preferences`, caches) is created on first launch; by default in
  `%APPDATA%\<name>`, here in `out/userdata-*` (the "write only into your own folder" rule).
* Images of unknown size cause a layout "jump" after `t_content` — which is why the size is read from the header.
* `content-visibility:auto` with a constant `contain-intrinsic-size` (3000 px) makes the scrollbar wrong until
  the sections are laid out: End / `scrollTo(end)` on medium landed in the middle of the document. Now the height of each section
  is estimated from the source (lines × 24 px × 1.4) — scrolling to the end lands exactly.
* `dist` contains `dxcompiler.dll`, `dxil.dll`, `vk_swiftshader.dll`,
  `vulkan-1.dll`, `d3dcompiler_47.dll` (~38 MB) and `LICENSES.chromium.html` (20 MB), unneeded for software rendering — in the tuned mode they are not
  loaded (checked against the module list) and do not affect launch time; they are kept because the dist is shared with `electron-gpu`.

## Notes on the harness

No bugs found. One note: `launch_overhead_ms` (≈ `CreateProcessW`) for large unsigned exes is
~60 % of the result; when comparing stacks it is worth showing as a separate column, otherwise "slow Electron" looks
like "slow code", even though the code has nothing to do with it.

## Files

| Path | Purpose |
|---|---|
| `src/main.js` | main process of the tuned variant (switches, window, first chunk, IPC, sending the rest) |
| `src/preload.js` | preload (sandbox): fetches the first chunk, inserts it, double rAF → `painted`, receives `append` |
| `src/md.js` | markdown-it + task lists + highlight.js (lazy languages) + image paths/sizes + splitting into chunks |
| `src/worker.js` | parser in worker_threads (the `worker` experiment) |
| `src/bench.js` | protocol: FILETIME ticks, marks, DwmFlush via koffi, atomic result write |
| `src/page.html`, `src/hljs-github.css` | page shell (CSP, `style.css` inline), highlighting colors |
| `src/default/` | the "ordinary" application for `electron-default` |
| `build.ps1`, `package.json`, `package-lock.json`, `proto.json` | build and variant descriptions |
| `tools_createproc.py` | cost of a bare `CreateProcessW` for any exe |
| `tools_delta.py`, `tools_marks.py` | analysis of harness results by marks |
| `tools_trace.py`, `tools_trace_view.py`, `tools_perfetto.py` | Chromium startup trace and viewing it |
| `tools_parse_timing.js`, `tools_headcheck.js`, `tools_mkdocs.py` | parse timing; a check that "head+tail" mode yields byte-for-byte the same HTML; documents for the font experiment |
| `results/` | raw runs, screenshots (`shots/`), trace (`traces/`), experiments (`_exp/`) |
| `tools_audit_present.py`, `tools_audit_chunkcheck.js`, `results/audit/` | audit (see below) |

## Audit (independent check, 2026-09-19)

The auditor checked the `electron` prototype and `baselines-external` for the honesty of the `t_content` mark (PROTOCOL §3),
whether the build is a release build, and render quality. During the check the machine was **loaded** by other agents — the numbers are rough.

### What was checked and confirmed

* **Marks.** `t_content` is taken in the main process (the window owner): the renderer received the first chunk → `innerHTML` →
  rAF → rAF → IPC `painted` → `DwmFlush()` (koffi) → mark. This is exactly the §3 procedure for web engines. The mark is never set
  earlier, at "navigation started / text parsed".
* **No cheating.** No FastMD/electron processes remain after the runs. There is no cache of the rendered document: in
  `out/userdata-*` there are only Chromium's housekeeping files (DIPS, Preferences, empty GPUCache/Dawn*); `Code Cache` is empty (the
  page has no scripts, CSP `default-src 'none'`). The corpus is not embedded in the build (`page.html` is CSS only, the bundle is
  code only). `userdata-default/fastmd-doc.html` is the default variant's temporary HTML, overwritten on every
  launch before `loadFile` (not a cache). First viewport first + sending the rest is an allowed technique and is described in the README.
* **Build.** `pwsh -File build.ps1` from scratch — OK: Electron 44.4.3 (release binary), JS — esbuild `--minify`.
* **Chunking is correct.** The new `tools_audit_chunkcheck.js` compares "first chunk + the rest" with rendering **the whole
  document as one chunk** (the stock `tools_headcheck.js` only compared two ways of splitting with each other):
  small / medium / large — **IDENTICAL byte for byte** (6,954 / 105,402 / 7,205,499 B). The first chunk's image:
  `file:///…/corpus/img/diagram1.png`, width/height 640×240 from the PNG header.
* **`bench.py check`** for `electron`, `electron-gpu`, `electron-default` on medium and large — CHECK OK with no
  warnings; `observed_ms` is 15–30 ms **earlier** than `window_ms` (the harness sees the window while still inside `show()`).

### Independently: is the document really on screen at `t_content`? (`tools_audit_present.py`)

The tool launches a variant the way the harness does (Job Object, but `FASTMD_BENCH_EXIT=0`) and, from the moment the window appears, continuously
grabs the client area: `print` — `PrintWindow(PW_RENDERFULLCONTENT)` (the window's own DWM surface),
`screen` — `BitBlt` from the screen (what DWM actually composited). A "frame with the document" is a white background + text. The step is ~13–25
ms. A control on `baseline-win32` showed that the `screen` method is distorted for the first ~180 ms by the window-open animation
(blending with what is under the window), so for a window shown shortly before the content the primary method is `print`.

| variant | doc | method | `t_content`, ms | last frame **without** the document (grab from–to) | first frame **with** the document | conclusion |
|---|---|---|---|---|---|---|
| electron | medium | print | 819.4 | 796.1–810.5 | 815.2–858.5 | mark accurate |
| electron | medium | print | 873.7 | 851.9–866.1 | 870.7–913.8 | mark accurate |
| electron | medium | screen | 894.5 | 867.5–878.4 | 884.9–897.9 | mark accurate |
| electron | medium | screen | 1231.9 | 1177.0–1204.2 | 1212.5–1239.5 | mark accurate |
| electron | large | print | 940.8 | 908.4–928.7 | 934.4–1006.6 | mark accurate |
| electron-gpu | medium | screen | 1211.7 | 1258.2–1271.4 | 1276.7–1290.9 | **early by ~45–80** |
| electron-gpu | medium | screen | 1041.6 | 1102.6–1115.0 | 1121.3–1135.3 | **early by ~60–95** |
| electron-gpu | medium | print | 1111.3 | 1187.7–1208.7 | 1214.9–1228.0 | **early by ~75–115** |
| electron-default *(before the fix)* | large | print | 3745.4 | 1708.8–1726.3 | 1731.2–1745.5 | **late by ~2.0 s** |
| electron-default *(after)* | large | print | 1572.7 | 1608.8–1622.0 | 1627.4–1640.4 | early by ~35–70 |
| electron-default *(after)* | large | print | 1623.5 | 1659.7–1678.6 | 1685.5–1702.1 | early by ~35–80 |
| electron-default *(after)* | medium | print | 1104.8 | 1128.8–1153.5 | 1157.9–1176.7 | early by ~25–70 |

Frames: `results/audit/present/*-before.png` / `*-first_doc.png` (`print` method only). The first frame with the document is
a fully styled first screen (h1/h2 with a rule, CJK, emoji, nested lists, checkboxes), not a placeholder.

**Conclusions.**
1. **`electron` (software compositing, the main variant): the mark is honest** — the document appears on screen within
   the grab resolution (≤ ~10 ms) of `t_content`.
2. **`electron-default` on large: the mark was ~2 s late.** Chromium parses and paints a large HTML
   progressively: the styled first screen is on screen after ~1.7 s, but the variant waited for `DOMContentLoaded` (~3.4 s) —
   that is not "the first screen of the document" per §3. **Fixed** (see below).
3. **GPU compositing (`electron-gpu`, `electron-default`): the §3 procedure (double rAF + `DwmFlush` in the
   browser process) fires ~40–90 ms BEFORE the frame actually reaches the screen** (under load).
   The frame is drawn and presented by a separate GPU process (DirectComposition) asynchronously; the second rAF only means the commit
   of the previous frame, and `DwmFlush` in another process does not wait for it. With in-process software compositing this
   effect does not occur. **Not fixed in the prototype**: this is a property of the protocol's procedure itself, and it most likely
   overstates the speed of all GPU-composited web prototypes equally (default WebView2, Tauri/wry); fixing it
   here alone would make the comparison asymmetric. Recommendation to the orchestrator: run `tools_audit_present.py` (the
   `print` method) on the other web variants, or refine the protocol (for example, wait for presentation feedback via
   `PerformanceObserver` paint/element timing). For `electron-gpu`/`electron-default` the real time ≈ the harness
   number + 40–90 ms; the conclusion "a GPU process costs more than software compositing" only gets stronger from this.
4. **`t_window` for all Electron variants** = the `show` event; until the renderer's first frame the window's client area is
   the system backdrop (a pale background, not the white `backgroundColor`), i.e. there is no separate "background painted" frame.
   `window_ms` here means "the window frame is visible", not "the window painted something". It does not affect `t_content`.

### What the auditor changed

1. `src/default/preload.js` — the mark of the `electron-default` variant: instead of `DOMContentLoaded` + double rAF, it now checks
   on every frame whether the already parsed part of `<article>` has reached the bottom of the window (or parsing has finished);
   that frame paints the first screen, the next rAF → `painted`. This is the same §3 procedure ("content in the DOM → double
   rAF"), applied to the first screen. New mark: `r_viewport`.
2. `src/default/main.js` — koffi (for `DwmFlush`) is loaded on `did-navigate`, while the renderer is parsing, rather than synchronously
   after `painted`: ~15 ms of harness instrumentation is no longer charged to the application (as was already
   done in the tuned variant). `r_raf2 → t_content`: ~20 → ~6 ms.
3. `proto.json` (`electron-default` notes), the variant description and the footnotes in this README; rebuilt with `build.ps1`.
4. New tools: `tools_audit_present.py`, `tools_audit_chunkcheck.js`.

The tuned `electron` and `electron-gpu` were not changed.

Effect of the fix (check): `electron-default` large **3333–3488 → 1629–1647 ms**, peak commit ~1 GB → ~420 MB
(exit right after the first screen); medium 1057–1118 → 1031–1060 ms (within noise).

### Sanity run after the audit

`bench.py run electron,electron-gpu,electron-default,ext-vscode,ext-void,ext-cursor --doc small,medium --runs 5`
(5 + warm-up, interleaved; the machine is loaded, sd up to 300 ms for some variants). File:
`results/audit/sanity-run.json`.

| variant | small, median / min | medium, median / min | CPU, ms (medium) | commit, MB | processes |
|---|---|---|---|---|---|
| **electron** | **909 / 873** | **885 / 873** | 625 | 94 | 3 |
| electron-gpu | 1054 / 967 | 1081 / 980 | 953 | 175 | 4 |
| electron-default | 1003 / 961 | 1128 / 1032 | 1172 | 205 | 4 |
| ext-vscode | 1437 / 1422 | 1431 / 1337 | 2000 | 332 | 5 |
| ext-void | 1872 / 1809 | 1804 / 1601 | 2094 | 352 | 4 |
| ext-cursor | 3826 / 3474 | 3671 / 3515 | 4891 | 635 | 8 |

`ext-notepad` was checked with a single `check` on medium (507 ms, OK; the user's Notepad was not open); it was not included in the `run` with small
— every new file becomes a tab in the user's Notepad session.

### External baselines (`baselines-external`)

All four work (`check` medium: VS Code 1341, Void 1497, Cursor 3444, Notepad 507 ms). The builder's caveats
are confirmed by the screenshots: Cursor shows a Sign Up / Log In screen, Void the "Welcome to Void" onboarding (the document is not visible,
the number = "a window with the file name"); VS Code is the editor, not the preview. For comparison with the prototypes this is a **lower bound**
on the time of third-party programs (in favor of the baselines, not of FastMD). `ext-notepad` in the final run — **medium
only**, and only if the user's Notepad is closed.

### Visual assessment

The auditor's screenshots: `results/audit/shots/electron-{small,medium}.png`, `electron-gpu-medium.png`,
`electron-default-{small,medium}.png`; scroll to the end and images — the builder's `results/shots/*-end.png`,
`results/_exp/img-test.png`.

* The §5 style is followed exactly (the shared `style.css` inline): Segoe UI Variable, 15 px / 1.6, an 860 column, h1/h2 with
  a `#d1d9e0` rule, h6 gray, code in Cascadia Mono on `#f6f8fa` with a radius, inline code with a background, links `#0969da`,
  a blockquote with a bar, a table with borders, a bold header, zebra striping and alignment, disabled task-list checkboxes without
  bullets, nested lists (disc → circle, numbering), color emoji, CJK, Cyrillic, PNG images. Syntax
  highlighting in the GitHub palette. The window title and client-area size (1000×800) follow the protocol.
* Minor points: the window icon is the stock Electron icon; `electron-default` has a dark title bar and menu (system theme) —
  this is "a typical application", acceptable. No GFM autolink literals, footnotes, alerts, anchors (listed above).
* **Score: visual quality 9/10, GitHub fidelity 9/10.**

### Audit summary

The comparison is honest: the main `electron` variant is measured correctly and complies with §3; `electron-default` has been fixed
(it was unfair to itself on large); for the GPU variants the mark is 40–90 ms optimistic because of the protocol's procedure
(a common problem of the web prototypes, reported to the orchestrator). Valid for the final sequential measurement:
`electron`, `electron-gpu`, `electron-default`, `ext-vscode`, `ext-void`, `ext-cursor`, `ext-notepad` (medium only).
