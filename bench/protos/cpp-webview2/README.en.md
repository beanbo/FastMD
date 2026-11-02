[Русский](README.md) | **English**

# cpp-webview2 — C++ Win32 + WebView2 + md4c-html

A FastMD prototype on the "thin native host + system Chromium (WebView2)" stack. Markdown is parsed in C
(md4c → HTML), a page with the embedded `bench/common/style.css` is handed to WebView2, and `t_content`
is taken strictly per PROTOCOL §3: content in the DOM → `requestAnimationFrame` ×2 → `postMessage('painted')` →
`DwmFlush()` in the host → `t_content`.

**The one-line summary:** a cold start takes ≈ **0.5–0.6 s** to a rendered document on this machine
(a white window appears after ~0.07–0.1 s), and nearly all of that time is the WebView2 Runtime starting its processes, not
our code. The resident variant takes **~70–110 ms**. Here, a cold WebView2 never gets below 0.45 s with any
flags that can honestly be shipped.

## 1. Stack and versions

| Component | Version / details |
|---|---|
| Compiler | MSVC 14.44 (VS 2022 Community), Windows SDK 10.0.26100; `/O2 /GL /MT` + `/LTCG /OPT:REF,ICF`, static CRT |
| WebView2 SDK | NuGet `Microsoft.Web.WebView2` **1.0.4191.47**, **static loader** `WebView2LoaderStatic.lib` (no `WebView2Loader.dll` next to the exe) |
| WebView2 Runtime | Evergreen **153.0.4234.32** (system-wide, `C:\Program Files (x86)\Microsoft\EdgeWebView\Application`, ~860 MB, `msedge.dll` 332 MB) — not part of the distribution, preinstalled on Windows 11 |
| Markdown | md4c **0.5.3** (`release-0.5.3`), md4c-html, dialect `MD_DIALECT_GITHUB`; sources unmodified (`third_party/md4c`) |
| Syntax highlighting | our own table-driven tokenizer in C (`src/mdconv.c`), ~250 lines |
| Distribution (`out/`) | **0.52 MB**: `fastmd-wv2.exe` 443 KB + `fastmd-open.exe` 103 KB (the launcher for the resident variant) |

Files:

- `src/app.cpp` — the host: window, WebView2 environment/controller, navigation, bench protocol, resident mode;
- `src/mdconv.c/.h` — a wrapper over md4c-html: split points at top-level block boundaries (for "first screen first"), fenced-code highlighting, file reading;
- `src/launcher.c`, `src/ipc.h` — `fastmd-open.exe` and the `WM_COPYDATA` contract for `-resident`;
- `src/app.manifest` — PerMonitorV2 DPI, UTF-8 code page;
- `build.ps1`, `proto.json`; `results/*.json` — the raw runs behind this README; `shots/*.png` — screenshots.

## 2. Building

```
pwsh -File bench/protos/cpp-webview2/build.ps1
```

Non-interactive, ~10 s. If `third_party/` is missing, the script downloads the WebView2 NuGet package (nuget.org) and 6
md4c files (raw.githubusercontent.com, tag `release-0.5.3`) by itself — both URLs have been checked. It then recreates `out/`
and `obj/`, generates `obj/gen/style_css.h` from `bench/common/style.css` (the CSS is embedded into the exe, with comments
stripped) and compiles via `bench/tools/msvc.cmd`.

The WebView2 profile (user data folder) is `%LOCALAPPDATA%\FastMD\WebView2-<variant id>`, not next to the exe
(by default WebView2 writes `<exe>.WebView2\` into the program folder, which both litters `out/` and is impossible
in Program Files). The `WebView2-cpp-webview2-x-*` folders there are leftovers from experiments and can be deleted by hand.

## 3. How the cold start works (variant `cpp-webview2`)

1. `wWinMain`: `fmb_init` (mark `main`) → `CoInitializeEx` (STA) → `CreateCoreWebView2EnvironmentWithOptions` **right away**.
   On runtime 153 the completion callback arrives synchronously inside that call, and it in turn calls
   `CreateCoreWebView2ControllerWithOptions` — that is what launches `msedgewebview2.exe`. The controller is created on
   a message-only "parking" window (`--early=1`); the real window does not exist yet.
2. In parallel, a worker thread reads the file and converts it md4c → HTML (medium: <1 ms, large 3.7 MB: ~20–35 ms),
   recording the offsets of top-level block boundaries.
3. A white 1000×800 DIP window is created and **shown immediately** (`t_window` ≈ 70–100 ms from CreateProcess).
4. `ctrl_ready`: the controller is moved into the real window (`put_ParentWindow` + toggling `IsVisible`, otherwise
   rAF never fires — see "Pitfalls"), the settings are configured, and `NavigateToString` is called with a page that
   holds only **the first ~24 KB of HTML** (whole top-level blocks; that is comfortably more than the first screen).
5. The page: a CSP with a nonce, `<base href="https://dXXXXXXXX.doc.fastmd.example/">` + `SetVirtualHostNameToFolderMapping`
   onto the document's folder (local images), `style.css` inline. After `DOMContentLoaded` → rAF → rAF →
   `chrome.webview.postMessage('painted')` → `DwmFlush()` in the host → `t_content`.
6. After the first frame the rest of the document is appended by `PostWebMessageAsString('A'+html)` messages in
   256 KB chunks (`insertAdjacentHTML`). **This is the legitimate "first screen first" technique (PROTOCOL §3)**: the first screen is
   the real rendered document, and the rest loads over the next ~100–300 ms (large: 28 chunks).
7. Links in the document open in the default browser (`NavigationStarting`/`NewWindowRequested` are cancelled).

If the first page is larger than 1.5 MB (for example `--chunk=0` on large.md), `NavigateToString` will not do (2 MB limit,
`E_INVALIDARG`), and the host automatically switches to `Navigate("https://page.fastmd.example/doc.html")` +
`WebResourceRequested`, which serves the page from memory.

## 4. Variants (`proto.json`)

| id | exe / args | What it is |
|---|---|---|
| **`cpp-webview2`** (best honest) | `out/fastmd-wv2.exe --variant=cpp-webview2` | Everything from §3 + the default browser arguments (see below), EnvironmentOptions: `ExclusiveUserDataFolderAccess=FALSE`, `EnableTrackingPrevention=FALSE`, `AreBrowserExtensionsEnabled=FALSE`; ControllerOptions: white `DefaultBackgroundColor`; settings: no DevTools/status bar/autofill/swipe/SmartScreen check |
| `cpp-webview2-default` | `--untuned --chunk=0` | The reference "as in the Microsoft sample": no options objects, no browser args, GPU process enabled, the whole document as one page (large → via `WebResourceRequested`). The profile is also in `%LOCALAPPDATA%` |
| `cpp-webview2-resident` | `out/fastmd-open.exe` + resident `fastmd-wv2.exe --resident` | **Resident** (PROTOCOL §6), see §4.2 |

Browser arguments of the `cpp-webview2` variant (`kBaseArgs + kLeanArgs` in `app.cpp`; `--gpu=1` turns off the second part):

```
--disable-features=msSmartScreenProtection,msWebOOUI,msPdfOOUI,msEdgeTranslate
--disable-background-networking --disable-component-update --no-first-run --disable-default-apps
--disable-sync --disable-extensions
--disable-gpu --in-process-gpu --enable-features=NetworkServiceInProcess2,NetworkServiceInProcess
```

This is the default configuration for a normal launch of `fastmd-wv2.exe file.md` (i.e. what a double click gives you).

### 4.1 Knobs tried and why this one was chosen

All the knobs remain in `app.cpp` as command-line arguments. Measured on medium.md, `t_content`, median,
interleaved order; other agents were loading the machine at the same time (CPU 45–70 %), so look at the
per-phase differences, not at the absolute values. Raw data: `results/knobs-medium-*.json`.

| Configuration | Median, ms | Processes | CPU per launch, ms | Key phase findings |
|---|---|---|---|---|
| `--single-process` (+ base args) | 547 / 563 | 3 | 1100 | renderer inside the browser process: nav→DCL 34 ms instead of ~165, but env→ctrl +30 ms, first frame +55 ms |
| **lean: `--disable-gpu --in-process-gpu` + NetworkServiceInProcess (chosen)** | **566 / 584 / 535** | **5** | **1130** | DCL→frame 3–4 ms |
| `--disable-gpu` | 579 / 579 | 7 | 1430 | DCL→frame 4 ms |
| `--nav=script` (ExecuteScript + `document.write` into the initial about:blank, no navigation) | 600 | 7 | 1500 | no gain: the renderer still starts only on the first script/navigation |
| `--no-sandbox` | 630 | 7 | 1590 | no gain |
| tuned with a GPU process (`--gpu=1`) | 612 / 632 / 620 | 7 | 1520–1610 | DCL→frame 19–32 ms (waiting for the GPU process) |
| `--in-process-gpu` without `--disable-gpu` | 666 | 6 | 1625 | on par with default, no gain |
| default (untuned) | 665 / 644 | 7 | 1750–1800 | DCL→frame 17–46 ms |

In addition (`results/knobs-medium-3.json`, `-4.json`, with lean as the base):

| Knob | Effect |
|---|---|
| `--early=0` (controller directly on the real hidden window, no parking) | 535 vs 535 — the same; `--early=1` kept |
| `--envlast` (show the window first, then start WebView2) | **+37 ms** — creating/showing the window before the env start lands on the critical path |
| `--udf=fresh` (a new profile every time) | env→ctrl +40 ms (profile creation) |
| `--inprivate` | within noise |
| `--nav=resource` (`WebResourceRequested` instead of `NavigateToString`) | within noise (+5–10 ms) |
| `--chunk=0` (the whole of medium as one page) | +20–30 ms on medium (DCL 126 vs 102), on large — **2.7 s versus 0.54 s** |

Why not `--single-process`: it is a "debugging only" Chromium flag, not officially supported in WebView2,
it disables the sandbox and renderer isolation, and it is known to crash; the gain is ~20 ms (within noise). Unacceptable for a product —
it is absent from the final variants. `--no-sandbox` gives nothing and weakens security.

The price of the chosen lean mode: compositing and rasterization are done in software (Skia on the CPU). For a text viewer
the picture is the same (screenshots of medium lean and of the resident GPU variant differ in 77 pixels out of 794 thousand), but scrolling
a large document on 4K costs more CPU. `--in-process-gpu` and the network service inside the browser process are not officially
documented for WebView2 — they work on runtime 153 (dozens of launches without failures), but that is a risk when the runtime
updates. The conservative alternative is `--disable-gpu` alone (~10 ms difference, but 7 processes and +25 % CPU).

### 4.2 The resident variant (`cpp-webview2-resident`) and PROTOCOL §6 compliance

- `proto.json` → `"resident": {"exe": "out/fastmd-wv2.exe", "args": ["--resident", ...]}`; the harness launches it
  once, outside the measurement. The resident creates the message-only window `FastMDWebView2Resident`, an environment, a **hidden**
  controller, and loads an "empty" shell page (CSS + script). The GPU process is enabled in the resident (`--gpu=1`
  is the default for `--resident`) — its startup time is not measured, and scrolling is better.
- What is measured is `fastmd-open.exe` (103 KB, only kernel32/user32/shell32): it takes the absolute path, `FASTMD_BENCH_OUT`
  and `FASTMD_BENCH_EXIT`, sends `WM_COPYDATA` to the resident (`AllowSetForegroundWindow` for activation) and exits.
  If there is no resident, it is an ordinary cold launch of `fastmd-wv2.exe`.
- The resident: converts the file, `PostWebMessageAsString('D'+base+first-screen html)`, shows the window
  (`"<file> — FastMD (cpp-webview2-resident)"`), the page → rAF ×2 → `painted` → `DwmFlush` → the **resident** writes
  the result file (mark `launcher_main` — the launcher's start time). Then, with `FASTMD_BENCH_EXIT != 0`, the document window
  is closed from the user's point of view (hidden, controller `IsVisible=FALSE`, DOM cleared), while the process stays alive.
  Closing the window with the close button in normal mode does the same.
- The id ends in `-resident` — this is the only variant with a pre-started process.

## 5. Spec support (PROTOCOL §5)

Screenshots (`bench.py shot`, 1000×800 DIP): `shots/cpp-webview2-medium.png`, `shots/cpp-webview2-small.png`,
`shots/cpp-webview2-resident-medium.png`, and deeper into medium — `shots/cpp-webview2-medium-scroll780.png`,
`shots/cpp-webview2-medium-scroll1560.png`, as well as `shots/cpp-webview2-large-scroll300000.png` — section 208 of
large.md, i.e. content appended in chunks after the first frame (taken with `FASTMD_WV2_SCROLL=<px>`, a dev variable).

Supported (checked by eye on the screenshots):

- headings h1–h6 with the sizes from the spec, a rule under h1/h2, h6 in gray; paragraphs with word wrap;
- **bold** / *italic* / `inline code` / ~~strikethrough~~ / links `#0969da` (open in the default browser);
- bulleted, numbered and nested lists, **task lists** (disabled checkboxes, as on GitHub; md4c does not set
  the `contains-task-list` class, so `.task-list-item{list-style-type:none}` was added to `kExtraCss`);
- blockquotes (4 px bar), horizontal rules, GFM tables with column alignment and zebra striping;
- fenced code: `Cascadia Mono`, background `#f6f8fa`, radius 6, **no line wrapping, with horizontal scrolling**;
  **syntax highlighting** (built in, in C, during conversion): C/C++, Rust, C#/Java/Kotlin, JS/TS, Python,
  PowerShell, JSON, Bash, Go — keywords, strings, comments, numbers, functions, types; colors as in GitHub light;
- **local images** (PNG from `img/` next to the document) via `SetVirtualHostNameToFolderMapping`;
- Unicode: Cyrillic, CJK, color emoji (Segoe UI Emoji via Chromium's DirectWrite fallback);
- PerMonitorV2 DPI (manifest + `WM_DPICHANGED`), a 1000×800 DIP client area, light theme, scrolling,
  Ctrl+wheel zoom (built into WebView2), text selection and copying.

Not supported / simplified:

- footnotes, front matter, math (KaTeX/MathJax), Mermaid — md4c cannot do them, and we deliberately load no JS libraries;
- heading anchors (md4c does not generate `id`), table of contents;
- find in page (no UI of our own; the built-in WebView2 Ctrl+F was not checked; the SDK has a Find API — a couple of days' work), printing, dark theme, live reload on
  file change, drag&drop, opening relative links to other `.md` files inside the viewer;
- highlighting is heuristic (not a full lexer), with no multi-line Rust/C# raw strings.

## 6. Quick measurements (`bench.py run ... --runs 5`)

**Noisy:** other agents were running in parallel (CPU load 45–70 %), CreateProcess here takes 20–40 ms because of
Defender, and one run hit a 2.1 s stall before `main` (OS/Defender, not our code). The final sequential
measurement is done by the orchestrator. `results/quick-final.json` (5 runs + 1 warm-up, medians):

| Variant | small | medium | large | t_window | CPU, ms (small/med/large) | peak commit job, MB | Host WS, MB | Processes |
|---|---|---|---|---|---|---|---|---|
| **cpp-webview2** | **488** | **629** | **535** | 70–100 | 1170 / 1270 / 1000 | 144–157 | 20–26 | 5 |
| cpp-webview2-default | 665 | 558 | **2700** | 70–85 | 1625 / 1780 / 4640 | 179 / 191 / **900** | 20–34 | 7 |
| cpp-webview2-resident | **70** | **79** | **108** | 53–88 | — (only the launcher is in the job) | — | 22–28 | 1 (+resident) |

A separate "best versus reference" run, 10 runs, interleaved (`results/head2head.json`):
small **542 vs 603**, medium **604 vs 644** ms; CPU 1050–1125 vs 1620–1750 ms; 5 vs 7 processes.
In `quick-final` the default's medium happened to come out lower — that is noise (sd 170–330 ms); on every sample of ≥10 runs
lean is faster by 40–60 ms, and the phases (below) show where it comes from.

The "second window" scenario (one viewer with the same profile is already open → the new process attaches to its
browser process; this is not a resident but standard WebView2 behavior; `python bench/protos/cpp-webview2/tools/warm.py 6 medium`,
the first viewer lives in its own Job Object and is killed at the end): **~335 ms**, env_call→ctrl_ready 125 ms instead of ~330.

Helper scripts: `tools/knobs.py` (compares knobs without editing `proto.json`, reuses `bench.py`),
`tools/warm.py` ("second window"), `tools/phases.py` (phase medians from the JSON of a `bench.py run`).

### Breakdown by marks (medians, ms from CreateProcess; lean, medium)

| Phase | ms | What happens |
|---|---|---|
| CreateProcess → `main` | 20–40 | loading the exe (443 KB, static CRT), Defender |
| `main` → `env_call` | 14 | almost all of it is `CoInitializeEx` (13 ms, combase/RPC); WebView2 does not work without STA COM |
| `env_call` → `ctrl_ready` | **300–390** | **WebView2 Runtime startup**: `msedgewebview2.exe` + `msedge.dll` (332 MB), profile, crashpad, (GPU/network — inside the browser in lean), WebContents creation. Meanwhile our process creates and shows the window and converts the document |
| `ctrl_ready` → `nav` | 3–6 | settings, handlers, `NavigateToString` |
| `nav` → `content_loading` | 62–75 | renderer process startup + navigation commit |
| `content_loading` → `DOMContentLoaded` | 70–100 | Blink/V8 initialization in the new renderer, parsing the HTML/CSS of the first page |
| `DOMContentLoaded` → `painted` (rAF×2) | **3–4** (17–46 with a GPU process) | first frame |
| `painted` → `t_content` | 6–9 | `DwmFlush` |

Resident (medium): the launcher's CreateProcess → its `main` ~40 ms (Defender), `WM_COPYDATA` <1 ms, conversion+post
1 ms (large: 35 ms — md4c parses the whole 3.7 MB file before sending the first screen), post → `painted` 21–32 ms, `DwmFlush` 8 ms.
That is the steady state; the **first** document after the resident starts (`bench.py check`, run #0 in `run`)
takes 165–280 ms — the renderer is still cold (JIT, font/style caches); in `run` it is discarded as the warm-up.

## 7. Where the time goes (conclusion)

~85 % of the cold start is the WebView2 Runtime: ~60 % on starting the browser process and ~30 % on starting the renderer. Everything
our code does (reading, md4c, highlighting, the window) is either <20 ms or hidden in parallel behind the browser start.
No flag noticeably shortens the browser-process start (it is within noise for all variants); flags
really affect only the tail: the GPU process (−15…−40 ms on the first frame) and the number of processes/CPU.
The practical floor of this stack's cold start on this machine is **~0.45–0.5 s**; "like a bullet" (≤100 ms)
is reachable only with a resident or with a window already open (~0.33 s).

## 8. Ideas for further speedups

1. **A resident as a product option** (tray/autostart; ~20–30 MB host WS + ~150–180 MB commit of WebView2 processes
   permanently in memory) — 70–110 ms.
2. **Reuse an already open window/process** (single-instance: a second double click hands the path to the first
   process via `WM_COPYDATA` and opens a new tab/window in the same WebView2 environment) — no resident,
   but the second and later files open in ~70–100 ms (like the resident), not 330 ms.
3. For large in the resident: have md4c parse only a prefix of the file (up to a safe boundary outside a fenced block) for the first
   screen, the rest on a worker thread (−30 ms on 3.7 MB; the risk is reference links defined further down the text).
4. Resident: after startup, "warm up" the renderer with a hidden sample document (headings, code, a table, emoji — fonts,
   styles, JIT), so that the **first** document also opens in ~80 ms rather than 165–280 ms.
5. `CoInitializeEx` (13 ms) is the only noticeable piece of our code on the critical path; try
   `COINIT_DISABLE_OLE1DDE` and delay-loading unneeded DLLs (`/DELAYLOAD:shell32.dll` etc.) — ~5–10 ms.
6. A fixed-version runtime next to the application will not speed anything up (same `msedge.dll`), but it gives control over
   the `--in-process-gpu` risks; it costs +~250 MB of distribution.
7. Excluding the runtime folder from Defender scanning (admin) — impossible in a product, but it explains part of the spread.

## 9. Effort estimate for a full product on this stack

| Stage | Estimate (1 C++ developer) |
|---|---|
| MVP: `.md` association, single-instance + windows, search (Find API), zoom, print/PDF (`PrintToPdf`), live reload on file change, dark theme following the system, drag&drop, links to `.md` inside, icon/signing, installer (MSIX or Inno) with a WebView2 Runtime check | **3–4 weeks** |
| 1.0: table of contents/anchors, footnotes (our own post-processor or cmark-gfm), settings, optional tray resident, KaTeX/Mermaid lazily (only if the document has them), automated render tests | another **4–6 weeks** |

Strengths of the stack: typography and Unicode quality "for free" (Chromium + DirectWrite), CSS styling
is trivial, a small distribution (0.5 MB), accessibility and text selection out of the box. The weakness: the cold start
is bounded by the runtime (~0.5 s), and 5–7 processes / ~150 MB commit per document.

## 10. Pitfalls

- **`NavigateToString` > 2 MB** → `E_INVALIDARG`, and the window stays white forever. Fix: keep the first page small (the first
  screen), or `Navigate(https://...)` + `WebResourceRequested` (done automatically above 1.5 MB).
- **Virtual host name**: `.local` or a name without a dot adds ~1.9 s to every navigation (name resolution).
  We use `*.fastmd.example` (`.example`/`.invalid`/`.localhost` all work).
- **A controller created under an invisible (message-only) parent** does not paint after `put_ParentWindow`, and rAF does not
  fire, until you toggle `put_IsVisible(FALSE)` → `put_IsVisible(TRUE)` + `NotifyParentWindowPositionChanged`.
- On runtime 153 the completion callback of `CreateCoreWebView2EnvironmentWithOptions` is called synchronously; the browser process
  starts in `CreateCoreWebView2Controller` — so the controller should be created as early as possible (a parking window
  or a hidden real window — the same; but showing the window before the env start costs +37 ms).
- **The default user data folder** is `<exe>.WebView2` next to the exe: it does not work in Program Files and bloats dist.
- Instances with the same UDF but **different browser args/options** cannot share a browser process (per the WebView2
  documentation, creating the environment fails in that case; I did not check this myself) — hence each variant has its own UDF.
- Runtime 153 **leaves a crashpad-handler process for every client** that attaches to a shared
  browser process (an observation by the web-engines researcher; in the "second window" measurement the client's job has 3 processes).
  For a single-instance product this is one more argument for opening new documents in the already running process.
- `--single-process` "works" but is unsupported and unsafe; `--no-sandbox` is useless.
- `ExecuteScript` into the initial `about:blank` does not save the renderer start (it is created lazily).
- md4c-html does not emit `class="contains-task-list"`/heading anchors; no footnotes.
- Under Defender every new process costs 20–40 ms, and WebView2 starts 5–7 processes — this is the main source of
  measurement spread; occasionally an exe launch stalls for seconds (a 2.1 s outlier before `main` is visible).

## 11. Debug variables

- `FASTMD_WV2_LOG=<file>` — a trace of WebView2 HRESULTs/events (off by default, zero cost).
- `FASTMD_WV2_SCROLL=<px>` — scroll the document after the first frame (for screenshots past the first screen via `bench.py shot`).
- The command-line knobs are listed in the header of `src/app.cpp` (`--gpu`, `--nav`, `--chunk`, `--slice`, `--hl`, `--cv`,
  `--udf`, `--early`, `--envlast`, `--inprivate`, `--fluent`, `--tp`, `--bargs`, `--untuned`).

## Audit (independent auditor, 2026-09-19)

**Verdict: honest.** No violations of PROTOCOL §3/§6 were found; the code and `build.ps1` were not changed. All three variants
(`cpp-webview2`, `cpp-webview2-default`, `cpp-webview2-resident`) are fit for the final sequential measurement.
During the audit the machine was heavily loaded by other agents (their windows sometimes covered the test window), so
the absolute numbers below are noisy.

### What was checked

| Item | How it was checked | Result |
|---|---|---|
| Release build | `pwsh -File build.ps1` from scratch | `/O2 /Oi /GL /MT` + `/LTCG /OPT:REF /OPT:ICF`, static CRT; the sizes matched byte for byte (443,392 / 103,424) |
| Where `t_content` is taken | `src/app.cpp`: `kScript`, `OnPainted`; `bench/common/fastmd_bench.h` | DOMContentLoaded → rAF → rAF → `postMessage('painted')` → host `DwmFlush()` → `fmb_now()` — exactly the §3 procedure for web engines. `DwmFlush` is called in the process that owns the window |
| Nothing from the corpus in the build | `build.ps1`, `obj/gen/style_css.h` | only `bench/common/style.css` is embedded in the exe |
| No cache or pre-rendering | sources; `processes` in the job | the file is read and converted by md4c on every launch. The job has 5 (lean) and 7 (default) processes, i.e. the browser process is new every time and no foreign one is reused: attaching to a live one would give 1–3 processes and env→ctrl ≈125 ms instead of 300–500. The profile UDF is persistent (`%LOCALAPPDATA%\FastMD\WebView2-<id>`), as for any WebView2 application; the document goes through `NavigateToString` and does not end up in the HTTP cache |
| "First screen first" | `bench.py shot ... --doc large` with `FASTMD_WV2_SCROLL=999999999` | a legitimate §3 technique, described in the README. The rest of large.md (6.9 MB of HTML, 28 chunks) really is appended: 300 ms after the first frame the bottom of the document is the last section, 1659 (`shots/audit-large-bottom-at300ms.png`). NB: in bench mode the process exits right after the first frame, so CPU and commit on large do not include laying out the remainder. The same holds for all prototypes using this technique |
| Resident (§6) | `launcher.c`, `ResidentOpen`; `run` | what is measured is `fastmd-open.exe` (1 process in the job, CPU ~16 ms). For every request the resident reads and converts the file itself, writes the result, hides the window and clears the DOM |
| `bench.py check` × medium/large | 6 launches | all OK. `observed_ms` ≤ `window_ms` (e.g. 111.9 versus 133.8 ms), i.e. `t_window` is not early. With WebView2, `observed_ms` sees only the window, not the document, so I checked the content mark by pixels (below) |

### Independent check of `t_content` by screen pixels (`tools/pixcheck.py`)

The script launches the exe the way the harness does (Job Object, the same environment variables) and in a loop grabs from the screen (BitBlt
of the screen DC, i.e. what DWM composited) a 900×200 band of the client area: ~6–12 ms per grab. In the table,
"last grab without text" is the start of that grab and "first grab with text" is its end; everything is in ms relative to
`t_content`. Only runs not covered by other windows were counted.

| Variant | Clean runs | Last grab without text | First grab with text | Conclusion |
|---|---|---|---|---|
| `cpp-webview2` (lean, no GPU process) | 10 | −24…+1.4 | +1…+20 | the mark is **accurate** (within one grab/frame) |
| `cpp-webview2-default` (GPU process) | 5 | +10…+18 | +32…+45 | text is on screen **~15–35 ms later** than `t_content` |
| `cpp-webview2 --gpu=1` (tuned + GPU process) | 3 | +1…+19 | +20…+32 | the same, ~10–30 ms |
| `cpp-webview2-resident`, steady state (the window is not hidden, small/medium documents alternate, `--keep`) | 7 | −24…−11 | −5…+1.8 | the mark is **accurate** |

Conclusion on the GPU process: on a cold start with a separate GPU process, rAF×2 + `DwmFlush` fire before
the GPU process hands its first frame to DWM. The prototype still follows the PROTOCOL §3 procedure, so I did not
change it: the other web prototypes use the same procedure, and changing it here alone would make the comparison unfair.
For the orchestrator: for all web stacks with a GPU process (Electron-gpu, wry/Tauri, winui3-webview2, etc.) `t_content` is
probably optimistic in the same way, by 10–35 ms. The lean variant (software compositing) and the resident in the steady
state do not have this effect.

### Visual defects found (they do not affect measurement honesty)

1. **A dark flash in lean mode (`cpp-webview2`).** Between `ctrl_ready` and the first frame (≈440–590 ms from the start)
   the WebView2 area is filled with `#131313` for 55–80 ms. It is visible in all clean runs; a grab of the band is
   `shots/audit-dark-flash-band.png`. The user sees the sequence "white window → dark rectangle →
   document". The cause is `--disable-gpu` (software compositing): with `--gpu=1` and in `default` there is no flash, or it is ≤10 ms,
   and `--early=0` does not help. The flash does not affect `t_content`, because the mark is taken after it. For a product this
   needs fixing: for example, with a white placeholder child window over WebView2 until `painted`, or by dropping
   `--disable-gpu` (≈ +20–40 ms on the mark and another +10–30 ms to real pixels).
2. **The resident shows the previous document.** When the window is shown again, DWM briefly displays the last frame
   of the previous document: the DOM clearing (`'C'`) runs on an already hidden controller and is not painted. In a trace with
   small/medium alternating, the pixel signature of the old document is visible before the new one. This does not affect the mark, because
   `t_content` is taken after the new document's frame. For a product: clear or hide the content before hiding the window,
   or show the window only after `painted`.
3. **A second resident silently exits.** `--resident` exits on its own when a resident is already running (`FindWindowExW` on
   the global class `FastMDWebView2Resident`), and the launcher hands the document to the other resident. During the audit
   another agent's parallel `bench.py run all` held its own instance, and my requests went to it. This does not interfere with the final
   sequential measurement, but `-resident` variants must not be run by two harnesses at the same time.

### Auditor's changes

- `proto.json`: only the `notes` of `cpp-webview2-default` were clarified. The host-side speedups (controller on the parking
  window before the window is created, md4c on a worker thread) are enabled in it, so it is "WebView2 with standard settings", not
  the bare Microsoft sample. The arguments and the exe were not changed.
- `tools/pixcheck.py` — a pixel check of `t_content`, applicable to any prototype with `mode: self`.
- `shots/audit-*.png` — audit screenshots; `results/audit-run.json` — the control run; `results/pix/` — pixcheck
  working files.

### Control run (`bench.py run ... --doc small,medium --runs 5`, machine under load)

| Variant | small, median (min) | medium, median (min) | t_window | CPU, ms | commit, MB | Processes |
|---|---|---|---|---|---|---|
| `cpp-webview2` | 758 (572) | 735 (567) | 93–123 | 1390–1420 | 144–149 | 5 |
| `cpp-webview2-default` | 774 (515) | 768 (687) | 115–120 | 1920–2000 | 178–190 | 7 |
| `cpp-webview2-resident` | 93 (72.5) | 94 (80) | 65–69 | 16 (launcher) | 1.7 | 1 |

The phases (medians) show where the increase over the developer's numbers comes from: env_call→ctrl_ready 438–505 ms versus
300–390 for the developer, i.e. under load the browser process starts more slowly. In one resident run the
launcher took 2.2 s to reach `main` (OS/Defender; the outlier shows up in `p90`).

### Visual assessment

The medium/small screenshots (`shots/audit-*.png`) are typographically on par with GitHub: this is Chromium plus the shared
`style.css`. Headings h1–h6 with a rule under h1/h2, Cyrillic, CJK, color emoji, inline code, strikethrough,
blue links, nested and numbered lists, task lists (disabled checkboxes, as on GitHub), a blockquote with a bar,
a table with column alignment and zebra striping, code highlighting, a local PNG — all correct. Downsides: a classic
scrollbar with arrows (not a Fluent overlay), heuristic highlighting, a dark flash before the first frame in
lean mode. Score: **visuals 9/10, completeness 9/10**.
