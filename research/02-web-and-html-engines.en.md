[Русский](02-web-and-html-engines.md) | **English**

# 02. Web and HTML engines for FastMD

A study of the "web stack" for a Markdown reader that has to open "like a bullet": WebView2 and everything built on top of it
(Tauri 2 / wry, Neutralino, Wails), Electron / CEF, and also lightweight HTML/CSS engines: Sciter, litehtml, Ultralight,
RmlUi, Blitz, Servo.

- Date: 2026-09-19. Machine: Windows 11 Pro 26200, Ryzen 9 7950X, 64 GB, NVMe, Defender real-time ON.
- WebView2 Runtime **153.0.4234.32** (evergreen), WebView2 SDK **1.0.4191.47** (static loader).
- All our own measurements are marked **[measured]**. Everything else comes from sources, with links in the text and in §17.
- Lab: `C:/Main/Projects/FastMD/research/lab/web-engines/` (WebView2), `research/lab/blitz/`,
  `research/lab/servo/`, `research/lab/sciter/`. How to reproduce — in the Appendix.

> **An important caveat about noise.** The measurements were taken while ~11 other agents were building their prototypes in parallel.
> One and the same WebView2 configuration (warm UDF, medium.md) gave **~590 ms** under light load (00:39)
> and **700–1050 ms** under heavy load. That is why variants are compared by **round-robin interleaving**,
> with **paired differences** against the base variant of the same round (`multi.py` + `paired.py`).
> Absolute numbers are a guide only. The orchestrator will do the final sequential measurement on an idle machine.

---

## 0. Summary (TL;DR)

1. **WebView2 in "a new process per double click" mode will not be a bullet.** [measured] warm UDF, medium.md:
   `t_content` ≈ **590 ms** under light load (the Win32 floor on this machine ≈ 90 ms), **700–1000 ms** under load.
   Almost all of the time goes into bringing up the `msedgewebview2.exe` tree (browser → GPU + utility → renderer, ~285 ms)
   and the first frame of the new renderer (~180–240 ms). The document itself (transfer + HTML parsing of medium) takes ~50 ms.
   A document size of up to 53 KB has **no effect** on the time: blank, small and medium match within noise.
2. **`CreateCoreWebView2Environment` is almost free** [measured]: the callback arrives within 3–7 ms,
   **before the browser process has even been launched**. The whole cost is in `CreateCoreWebView2Controller`:
   ≈ 285 ms when quiet, 450–600 ms under load.
3. **A fresh and a reused user data folder on NVMe give the same time** [measured]. InPrivate speeds nothing up.
4. **A shared browser process** (another process already holds the same UDF with the same options) [measured]:
   `t_content` ≈ **400–545 ms** versus 730–1040 ms without it in the same time window, i.e. **≈ −45–50 %**.
   What remains is the launch of a new renderer and a short-lived "go-between" process.
   `SpareRendererForSitePerProcess` does not provide a pre-launched renderer.
   **A defect found in runtime 153:** every client that connects to an already running browser process leaves behind
   an extra **`msedgewebview2.exe --type=crashpad-handler`** that never exits on its own. 4 clients → 4 "orphans"
   (still alive 1.5 min after the browser process exited); 22 clients → 21 "orphans" up to 15 min old;
   other series had 8 and 13.
5. **Browser flags give almost nothing** [measured, paired differences]:
   `--in-process-gpu` −90 ms in set B (7 rounds, heavy load), but only **−11 ms** in the recheck G
   (10 rounds, lower load), i.e. the effect is unstable;
   `--disable-gpu` −144 / −57 ms (but that is software rasterization, and Microsoft is explicitly against it);
   the wry defaults, `--disable-background-networking`, `msWebView2CancelInitialNavigation`,
   `--edge-webview-no-dpi-workaround` and disabling Tracking Prevention are within noise.
6. **How the content is loaded does not matter; the host name does** [measured]. NavigateToString, `file://`,
   virtual host and WebResourceRequested are within ±60 ms of each other. But a host in the **`.local`** zone
   (`https://fastmd.local/`) or a **single-label name** (`https://demo/` from Microsoft's sample,
   `https://localmdimages/` in PowerToys) adds **+1.9 s** to every navigation, with both virtual host and
   WebResourceRequested; `--no-proxy-server` does not help. With `.example`, `.invalid`, `.localhost` there is no delay.
   The NavigateToString limit: 6.33 million characters → `0x80070057` (E_INVALIDARG).
7. **A large document** (large.md, 3.7 MB, 158 thousand DOM nodes) [measured]: Chromium spends ≈ **3.1 s** and
   ≈ **840 MB** private, Blitz ≈ 5.8 s. First Contentful Paint with streaming load ≈ 0.5 s.
   The document has to be delivered in chunks: the first screen right away, the rest later.
8. **KaTeX / highlight.js / Mermaid** work in WebView2 without modifications [measured, §13]:
   - highlight.js 11.12: 3–7 ms execution + 22–33 ms for 24 blocks;
   - KaTeX 0.18.7: ~50–80 ms for 80 formulas + fonts another ~110 ms later;
   - **Mermaid 12.0.0: 5.5 MB of JS, +300–700 ms** (execution 90–240 ms + rendering 2 diagrams 290–480 ms;
     the first load once took 5 s). Load it only if there is a `mermaid` block and after the first screen.
9. **Blitz (DioxusLabs, Rust: Stylo + Taffy + Parley + Vello)** is the most interesting "dark horse" [measured].
   The engine's own phases on medium ≈ 40 ms (styles + layout).
   The build with the CPU rasterizer: `t_content` ≈ **375 ms** under moderate load and ≈ 560 ms under heavy load
   (of which ~90–110 ms is process startup), working set 59 MB, a single 22 MB exe.
   The GPU build (vello_hybrid): ≈ 1.2–1.5 s, eaten up by GPU initialization.
   Layout quality on the corpus is good. Downsides: beta status and **no JavaScript** (KaTeX/Mermaid/hljs only via native equivalents).
10. **Servo nightly (2026-09-17)** [measured]: ≈ 0.8–1.2 s to content, WS ≈ 230 MB, distribution 276 MB.
    Visible layout bugs: `0.` numbering in `<ol>`, bold/italic/semibold with Segoe UI Variable are lost, emoji are monochrome.
    Not ready for FastMD.
11. **Sciter.JS 6.0.5.0** [measured, preliminary]: `sciter.dll` weighs **19.3 MB** (not "5 MB" as the marketing says).
    `scapp.exe` on medium: window after 1.7–2.5 s, the central column drifts off (horizontal scrolling); a tiny page — 0.47 s.
    A proper measurement is needed in the `litehtml-sciter` prototype.
12. **Electron** is slower than bare WebView2 by construction: the same Chromium plus Node, plus its own copy of Chromium
    (not shared with Edge via hardlink), plus executing the JS bundle. Typical complaints about MD apps: Typora 7–8 s,
    MarkText 3 s (warm) / 12 s (cold), Zettlr up to 40 s, Obsidian 4–20+ s.
    Electron's 2026 optimizations (Node snapshot, code cache) save **tens of ms**, not seconds.
13. **Conclusion:** for the "bullet" requirement (a target of ~150–250 ms to the first screen) the main renderer must be **native**
    (Direct2D/DirectWrite; consider litehtml as a layout engine for an HTML/CSS subset).
    A web engine fits only as (a) a **resident** mode with a pre-warmed WebView2
    or (b) a lazy "rich mode" for Mermaid/KaTeX. Details in §15.
14. **A note on the protocol/harness** [measured]: 6 times over several hundred launches the process "hung" **before the first line of `main`**
    for 0.56 / 1.9 / 2.3 / 6.3 / 15.1 / 27.1 s. This is an OS delay (loader, Defender, page-in under load), not the stack.
    Prototypes should set the `main` mark, and the harness report should show `main` and flag such outliers (§16).

---

## 1. Method

### 1.1 Tools (all in `research/lab/web-engines/`)

| File | What it does |
|---|---|
| `wv2probe.cpp` → `out/wv2probe.exe` | C++ / Win32 + WebView2 (static loader, 268 KB). Sets marks at every step: `main`, `env_call`, `env_ready`, `window_created`, `host_first_paint`, `controller_call`, `controller_ready`, `navigate_called`, `dom_content_loaded`, `nav_completed`, `msg_received`, `t_content`. Content counts as shown per the PROTOCOL.md §3 rule: in the page `requestAnimationFrame` ×2 → `postMessage` → the host calls `DwmFlush()` → `t_content`. After that it captures the process tree via `ICoreWebView2Environment8::GetProcessInfos`: type, creation time (`GetProcessTimes`), WS and private bytes. Options: `--udf`, `--args` (AdditionalBrowserArguments), `--load string\|file\|vhost\|resreq`, `--host`, `--inprivate`, `--notrack`, `--late`, `--hidden`, `--hold`. |
| `drive.py` | A series of launches of one scenario. `t0` = `GetSystemTimePreciseAsFileTime` right before `CreateProcess`; between launches it waits for the browser process to exit. The `--udf shared` mode brings up a hidden "holder" of the same UDF. |
| `multi.py` + `paired.py` | Round-robin interleaving of scenarios (forward/reverse order) and paired differences against the base of the same round. |
| `gen_pages.mjs` | markdown-it 15.0.2 + task-lists renders the corpus to HTML with `bench/common/style.css` inlined. Pages: `p-*` (pre-rendered HTML), `cm-*` (markdown-it in the page), `hl-*` (highlight.js 11.12.0), `kx-ext` (KaTeX 0.18.7, 80 formulas), `mm-ext` (Mermaid 12.0.0, 2 diagrams), `blank`. |
| `title_watch.py` | For engines without a channel to the host (Servo, Sciter): after rAF×2 the page sets `document.title`, and the script polls the window title every ~1 ms and takes a screenshot. |
| `proto_run.py` | A mini runner following the `FASTMD_BENCH_OUT` contract for builds outside `bench/protos` (Blitz rdme). |

The medium page: 95 thousand characters of HTML, 2,344 DOM nodes. Large: 7.1 MB of HTML, 158,863 nodes.

### 1.2 Limitations

- CPU load from other agents (see the caveat above).
- A truly cold start after a reboot was not measured: no rights, and other agents must not be disturbed.
  "The first launch of a series" sometimes caught outliers, but their cause is delays before `main` (§2.12).
- **Edge is running** on this machine (14 `msedge.exe` processes), and `msedge.dll` (332 MB) of Edge, EdgeCore, Copilot and
  WebView2 of the same version is **one and the same file via hardlink** (`fsutil hardlink list`).
  The pages of WebView2's main DLL are most likely already in memory. This is the favorable case: on a machine without
  a running Edge, a cold start of WebView2 will be slower.

---

## 2. WebView2

### 2.1 Process model

According to the documentation ([Process model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/process-model)):
each user data folder (UDF) gets **one process group** — one browser process, renderers, GPU and
utility processes (network service, storage, etc.). All processes are tied to the browser process, and that one to a single UDF.
Several `CoreWebView2Environment`s with the same UDF **and identical options** (including
`AdditionalBrowserArguments`, `Language`) share one group, even from different apps.
With different options, WebView creation **fails**
([Performance best practices](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance),
updated 2026-09-02: "Multiple apps can share a browser process by using the identical user data folder and
CoreWebView2EnvironmentOptions").

[measured] The tree for medium.md (WebView2 153, base from set B, medians of 7 rounds):

| Process | Appears after (from t0), quiet / under load | Working set | Private |
|---|---|---|---|
| host (`wv2probe.exe`) | 0.7 ms (created) / `main` 35 / 70–110 ms | 21 MB | 4 MB |
| browser (`msedgewebview2.exe`) | 84 / 130–220 ms | 113 MB | 44 MB |
| GPU | 227 / 330–500 ms | 57 MB | 59 MB |
| utility ×2 (`--type=utility`; per the documentation these are services such as network/storage, `GetProcessInfos` does not tell them apart) | 229–255 / 400–540 ms | 27 + 18 MB | 11 + 8 MB |
| renderer | 336 / 470–670 ms | 73 MB | 37 MB |
| crashpad-handler | not visible in `GetProcessInfos` | — | — |
| **Total** | 5 processes + host | **≈ 320 MB** (WS is summed together with shared pages) | **≈ 165–185 MB** |

### 2.2 Startup anatomy [measured]

A run in a quiet window (smoke, 00:39, persistent UDF, NavigateToString medium):

| Mark | ms from t0 | Comment |
|---|---|---|
| process created | 0.7 | `GetProcessTimes` |
| `main` | 34.5 | as for baseline-win32 (35–40 ms): the cost of CreateProcess + Defender |
| `env_call` → `env_ready` | 49.1 → 52.5 | **3.4 ms**. There is no browser process yet |
| window created / host's first WM_PAINT | 57.5 / 74.8 | the native window is visible almost immediately |
| `controller_call` | 74.9 | |
| browser process created | 83.7 | +9 ms after controller_call |
| GPU / utility created | 226.9 / 229.3 | **~140 ms of browser process initialization** |
| renderer created | 336.3 | |
| `controller_ready` | **360.2** | 285 ms for the controller |
| `navigate_called` (NavigateToString, 95 thousand characters) | 362.9 | reading the file + UTF-16: 1.2 ms |
| the page's `performance.timeOrigin` | 370.1 | |
| `DOMContentLoaded` (host event) | 442.6 | in the page `responseEnd` = +54 ms, DCL = +57 ms |
| `NavigationCompleted` | 538.3 | |
| rAF×2 in the page → message to the host | 591.6 | rAF×2 = +219 ms from the start of navigation |
| **`t_content`** (after DwmFlush) | **598.4** | median of 3 runs: **591.5** |

Conclusions:

- **Three parts:** ~40 ms of host process startup (as for any exe); ~285 ms to bring up browser/GPU/renderer;
  ~230 ms for navigation and the first frame of the new renderer. The native window (creation + first WM_PAINT) takes ~25 ms.
- **The renderer's first frame costs ~180–240 ms at any document size.** For `blank.html` (12 nodes) and
  medium (2,344 nodes) the "navigation start → second rAF" interval is the same: 243 ms in both cases (set C).
  This is the cost of the first compositor frame (GPU channel, raster, vsync), not of layout.
  With `--in-process-gpu` and `--disable-gpu` it is shorter (≈ 190–220 ms).
- Even NavigateToString of 3 KB delivers `responseEnd` ~50 ms after the start of navigation: the data goes over IPC.

### 2.3 Environment versus Controller

The common belief that "the environment is slow to create" is **wrong** on runtime 153.
`CreateCoreWebView2EnvironmentWithOptions` returns within 3–7 ms, and the callback arrives before the
browser process is created [measured]. The `msedgewebview2.exe` process is launched only in `CreateCoreWebView2Controller`,
and all the waiting is there. This matches the old issues:
[#1910](https://github.com/MicrosoftEdge/WebView2Feedback/issues/1910) (a delay "between calling
CreateCoreWebView2Controller and its handler", ~2 s on a Surface Go 2) and
[#1540](https://github.com/MicrosoftEdge/WebView2Feedback/issues/1540).

In practice:
- Call `CreateCoreWebView2EnvironmentWithOptions` **as the first line**, before creating the window. The savings are small
  (set B: the `--late` variant +15 ms at the median, within noise), but free.
- The controller is created on an already **visible** window. A hidden window has no frames: rAF does not fire, and `t_content` never comes.
  I got caught by this myself with the "holder" hanging — see §2.5.

### 2.4 User data folder, InPrivate, Tracking Prevention [measured, set B, 7 rounds, paired differences against the base]

| Variant | t_content, median | Δ vs base (median) | Better than base in rounds |
|---|---|---|---|
| base (persistent UDF in the lab folder) | 965 | 0 | — |
| **a fresh UDF for every launch** | 847 | +71 | 2/7 |
| InPrivate (`ControllerOptions.IsInPrivateModeEnabled`) | 941 | +154 | 1/7 |
| `EnableTrackingPrevention = FALSE` | 917 | +33 | 3/7 |
| env after the window is shown (`--late`) | 904 | +15 | 3/7 |

Creating the UDF for the first time (dozens of files and `EBWebView\...` folders) is **not noticeable** on NVMe. Microsoft's recommendation
to keep the UDF on a fast local disk in `%LOCALAPPDATA%` still stands
([Performance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance)).
A reader does not need InPrivate, and it speeds nothing up.

### 2.5 Shared browser process, pre-warming, residency

[measured] The "holder" scenario: a hidden owner process with the same UDF and the same options is already running (set matrix2, 7 launches):

| Scenario | env_ready | controller_ready | t_content | What is created after t0 |
|---|---|---|---|---|
| no holder (same time window) | 100 | 620–730 | **870–1040** | browser + GPU + 2 utility + renderer |
| **with holder** | 53 | 203–304 | **437–520 (median 460)** | only a new renderer (≈ 180–200 ms) |
| with holder + `SpareRendererForSitePerProcess` | see §2.5.1 | | | |

- Savings ≈ 50 % (~450 ms under load). But even with the holder the controller costs ~150–250 ms: a
  **short-lived** `msedgewebview2.exe` is launched, finds the running browser (process singleton), forwards the
  request and exits, and then the browser brings up a new renderer.
- **Defect:** after every such connection a **hanging `crashpad-handler`** is left behind. 8 launches → 8 processes
  with `--type=crashpad-handler --user-data-dir=<our UDF>`; they kept living after the holder was closed (I killed them myself).
  For a "reader shares the browser process with a resident" scheme this is a process leak that will have to be worked around
  (e.g. the resident hosts the windows itself) or reported to WebView2Feedback.
- A pre-warming trap: a **hidden** window (and a WebView created on an invisible window) has no frames,
  so `requestAnimationFrame` is not called. Rick Strahl describes the same for Markdown Monster: "The WebView2 control has a
  feature that doesn't fully initialize if it's not UI visible"
  ([West Wind, 2022](https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization)).
  Microsoft recommends creating an invisible WebView2 in advance
  ([Performance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance)),
  but readiness must be awaited via `NavigationCompleted`/DOM, not via frames.
- A **fully resident** variant (a process with a pre-warmed controller and renderer that only shows the window and
  swaps in the content) is a separate prototype, `cpp-webview2-resident`, by another agent. Based on my data, one should
  expect it to reach the level of "show the window + `ExecuteScript`/`PostWebMessage` + one frame": ~100–200 ms. This is the only path
  where WebView2 approaches the "bullet". The price is a constant background ~170–260 MB private (§2.9) and autostart.

#### 2.5.1 Spare renderer and a repeated series [measured, set E, 7 consecutive launches, one time window]

| Scenario | controller_ready | t_content (median) | Renderer created after t0 | Tree private |
|---|---|---|---|---|
| no holder | 686 | **929** | 518–717 ms (the whole set of processes) | 186 MB |
| with holder | 306 | **544** | 198–315 ms (renderer only) | 255 MB (2 clients) |
| with holder + `--enable-features=SpareRendererForSitePerProcess` | 271 | **503** | 169–315 ms | 254 MB |

The `SpareRendererForSitePerProcess` flag (from the official list) **does not hand a pre-launched renderer to a new
client**: in every launch the renderer was created after t0. The 544 → 503 ms difference is within noise.

**The crashpad-handler leak, a separate check** (`crashpad_check.py`): holder up → 1 crashpad-handler. After
each of the 4 clients: 2, 3, 4, 5. Holder killed, browser process exited → **4 `crashpad-handler`s remained**, and they
were still alive after 30 s and after ~1.5 min. matrix1 had 13 such "orphans", matrix2 had 8. In sets E and Q, 22 connections
left **21** processes behind, the oldest was **14.8 min** old when I killed them. They never exit on their own.
(The cleanup after set E at first showed 0, but that was a bug in my filter: a relative `--udf-dir` put the UDF in
`out/udf/…`.) Each process weighs ~1–2 MB, but every file open adds a new one.

### 2.6 AdditionalBrowserArguments [measured, set B, 7 rounds, paired differences]

| Flags | Median | Δ vs base | Better than base | Processes | Private MB | Comment |
|---|---|---|---|---|---|---|
| base | 965 | 0 | — | 5 | 167 | |
| `--in-process-gpu` | **765** | **−90** | 6/7 | 4 | 217 | GPU thread inside the browser; no separate process. A Chromium flag, not documented in the WebView2 list |
| `--disable-gpu` | 807 | **−144** | 6/7 | 5 | 124 | Software rasterization. Microsoft: "Don't disable the use of the GPU … except when you're troubleshooting" |
| `--no-sandbox` | 817 | −70 | 5/7 | 5 | 225 | For experiments only: removes the renderer sandbox |
| combo (in-process-gpu + wry defaults + CancelInitialNavigation + no-bg-network) | 818 | −54 | 5/7 | 4 | 219 | |
| `--enable-features=msWebView2CancelInitialNavigation` | 855 | +22 | 3/7 | 5 | 172 | Per the documentation "to improve startup performance"; no visible effect |
| `--disable-background-networking --disable-component-update --no-first-run --disable-sync --disable-extensions` | 885 | +9 | 3/7 | 5 | 173 | |
| wry defaults `--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection` | 933 | −26 | 5/7 | 5 | 172 | |
| `--edge-webview-no-dpi-workaround` | 919 | +42 | 3/7 | 5 | 181 | |

**Recheck (set G, 10 rounds, lower load, base 794 ms):**

| Flags | Median | Δ vs base (paired) | Better than base | Private MB | nav start → rAF×2 |
|---|---|---|---|---|---|
| base | 794 | 0 | — | 186 | 220 |
| `--in-process-gpu` | 749 | **−11** | 6/10 | 218 | 204 |
| `--disable-gpu` | 755 | **−57** | 6/10 | 125 | 182 |

Bottom line: **even the best flags give 0–90 ms depending on conditions**. `--in-process-gpu` removes a process, but adds
~+30 MB private to the browser process. The gain from `--disable-gpu` is a cheaper first frame without the GPU channel, but
long scrolling and animations will go to software. Microsoft states outright that flags
should not be used in production ("might be removed or altered at any time",
[WebView2 browser flags](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/webview-features-flags),
updated 2026-08-31). In issue [#5321](https://github.com/MicrosoftEdge/WebView2Feedback/issues/5321)
(July 2025) a developer went through the performance flags, got "largely unchanged", and never got an answer from Microsoft.
My data confirm this: **flags do not cure WebView2 startup**.

### 2.7 Content loading methods [measured, set C, 5 rounds]

| Method | Median t_content | Δ vs NavigateToString | Note |
|---|---|---|---|
| NavigateToString | 757 | 0 | origin `null`, relative images **do not load**, 2 MB limit |
| `file:///...html` | 718 | −12 | the simplest; relative resources work; resources are resolved in the WebView2 processes |
| virtual host `https://fastmd.example/` | 678 | −55 | HTTPS origin, relative resources, resolved in the WebView2 processes |
| virtual host `https://fastmd.localhost/` | 733 | −24 | the same (Tauri uses `tauri.localhost`) |
| WebResourceRequested `https://fastmd.req/` | 766 | +11 | response from the host's memory; every request goes through the host's UI thread |
| **virtual host `https://fastmd.local/`** | **2,639** | **+1,864** | **mDNS/LLMNR resolution of the `.local` zone** |
| virtual host `.local` + `--no-proxy-server` | 2,662 | +1,883 | the proxy (WPAD) is not the cause |
| **WebResourceRequested `https://fastmd.local/`** | **2,772** | **+1,955** | the delay sits in name resolution, before interception |

- This looks like [WebView2Feedback #2381](https://github.com/MicrosoftEdge/WebView2Feedback/issues/2381) ("2 seconds delay …
  when the domain is non-existent"). The suggestion there is to put the name into `hosts`. **It is simpler to pick the right name.**
  [measured, set F, 4 rounds, virtual host, medium]:

  | Host name | t_content (median) | nav start → rAF×2 |
  |---|---|---|
  | `fastmd.invalid` | 724 | 171–300 |
  | `fastmd.example` | 862–907 | 231–247 |
  | `tauri.localhost` | 627–827 | 207–270 |
  | **`demo`** (as in the sample from Microsoft's documentation) | **2,690** | **2,130–2,160** |
  | **`localmdimages`** (as in PowerToys) | **2,688** | **2,126–2,175** |
  | **`fastmd.local`** (set C) | **2,639** | **2,135** |

  Single-label names (without a dot) and the `.local` zone go to system resolution (LLMNR/mDNS/NetBIOS) and wait for its timeout of
  ~2 s **on every navigation**. Names from the reserved zones `.example`, `.invalid`, `.localhost` work without delay.
  Consequence for PowerToys: the images of its Markdown preview (`https://localmdimages/...`) should arrive about 2 s
  later than the text (I did not check this myself). The sample from Microsoft's documentation (`https://demo/index.html`) is slow too.
- **The NavigateToString limit:** 6,330,415 characters → `0x80070057` [measured]. Per the documentation the limit is 2 MB, the error is
  "Value does not fall within the expected range"
  ([Using local content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content)).
  PowerToys measures **UTF-8 bytes** and above 1,500,000 switches to a temporary file
  ([MarkdownPreviewHandlerControl.cs](https://github.com/microsoft/PowerToys/blob/main/src/modules/previewpane/MarkdownPreviewHandler/MarkdownPreviewHandlerControl.cs),
  fix [PR #47391](https://github.com/microsoft/PowerToys/pull/47391): previously it checked `.Length` in UTF-16 and failed on CJK).
  Rick Strahl: a temporary file + `file://` works "slightly faster"
  ([2024](https://weblog.west-wind.com/posts/2024/Jul/22/Work-around-the-WebView2-NavigateToString-2mb-Size-Limit)).
- Microsoft's documentation: WebResourceRequested is "slower than other approaches" because every request waits for the host's UI thread.
  For a single HTML page the difference is not visible (+11 ms); with dozens of images and scripts it will be.
- **Recommendation for FastMD:** deliver the document via NavigateToString while the UTF-8 size is < 1.5 MB, otherwise via a temporary file
  or a virtual host. Images and assets — via `SetVirtualHostNameToFolderMapping` to a host in the `.example` zone
  (the document's folder + the app's asset folder).

### 2.8 Document size [measured, set C]

| Page | DOM nodes | t_content | nav start → rAF×2 | Private MB (tree) |
|---|---|---|---|---|
| blank | 12 | 854 | 243 | 153 |
| small (3.4 KB md) | 152 | 864 | 279 | 172 |
| medium (53 KB md) | 2,344 | 757 | 243 | 182 |
| medium, markdown-it **in the page** | 2,251 | 939 | 295 | 186 |
| **large** (3.7 MB md), `file://` | 158,863 | **3,109** | 2,418 | **838** |
| large, markdown-it in the page | 152,230 | 3,688 | 3,112 | 1,030 |
| large via NavigateToString | — | **error 0x80070057** | — | — |

- Up to ~50 KB of Markdown, size has no effect at all: the time is determined by engine startup.
- markdown-it in the page for medium: 8 ms to execute the library (120 KB), 30 ms for parsing and rendering,
  4 ms for `innerHTML`, a total of +50 ms to rAF×2. For large: 520–590 ms parsing + 190 ms `innerHTML` + 2.1 s layout.
  It pays to parse Markdown in the native host (md4c/cmark-gfm) and deliver ready HTML.
- **Streaming rendering works:** for `file-large` First Contentful Paint = **516 ms** from the start of navigation, although
  the whole document is ready only by 2.1–2.8 s. Hence the technique: first deliver the HTML of **the first ~50–100 KB** (the first screen),
  then append the rest (`PostWebMessage` in portions / `insertAdjacentHTML` / `content-visibility: auto`).
  PROTOCOL.md explicitly allows this.

### 2.9 Memory [measured]

- medium.md: the whole tree ≈ **165–185 MB private**, ≈ 320 MB WS (the WS sum is inflated by shared pages).
  Host 4 MB private. `--in-process-gpu`: ~217 MB private (GPU in the browser process).
- large.md: ≈ **840 MB private** (ready HTML), ≈ 1 GB (markdown-it in the page).
- Two windows / two clients on one UDF: ≈ 250–260 MB private for two documents (the shared set, 6 processes).
- PowerToys Peek, according to reports, holds 100–200 MB when idle
  ([WindowsForum, 2026](https://windowsforum.com/windows-news.4/windows-11-quick-look-quicklook-vs-powertoys-peek-spacebar-preview.402847)).

### 2.10 Evergreen and Fixed Version

- Evergreen **ships with Windows 11**, and on Windows 10 "the vast majority" of devices already have it
  ([Distribution](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution), updated 2026-09-14).
  For FastMD this means **0 MB** added to the distribution, plus the static `WebView2LoaderStatic.lib` (~100–200 KB in the exe).
- "On eligible systems, binaries for Microsoft Edge and the Evergreen WebView2 Runtime are hard-linked together when they
  are on the same version. This linking provides benefits for disk footprint, memory, and performance" (same source).
  [measured] Here `msedge.dll` 331.9 MB is one file under 4 paths (Edge, EdgeCore, Copilot, EdgeWebView).
- Fixed Version: "over 250 MB" added to the distribution, with no automatic security updates. There will be no hardlink with Edge, and so
  no "Edge is already in memory" benefit either: a cold start will be slower. Microsoft recommends evergreen for
  performance too ("Using a fixed version risks missing out on recent optimizations",
  [Performance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance)).
- The evergreen risk: the version changes under your feet (here 152.0.4191.66 and 153.0.4234.32 sit side by side).
  Tests on the Beta/Dev channels and feature detection of new APIs are needed.

### 2.11 Known WebView2 users for Markdown

- **PowerToys (File Explorer preview + Peek):** Markdig → HTML → WebView2. UDF
  `%USERPROFILE%\AppData\LocalLow\Microsoft\PowerToys\MarkdownPreview-Temp`, the `--block-new-web-contents` flag,
  **scripts disabled** (`IsScriptEnabled=false`), NavigateToString < 1.5 MB, otherwise a temp file; images via
  the virtual host `localmdimages`
  ([source code](https://github.com/microsoft/PowerToys/blob/main/src/modules/previewpane/MarkdownPreviewHandler/MarkdownPreviewHandlerControl.cs)).
  Peek runs as a PowerToys background process (residency). There are complaints about a ~400 ms preview delay
  ([#46087](https://github.com/microsoft/powertoys/issues/46087), 2026-03) and a proposal for a separate "Markdown Reader"
  on WinUI 3 + WebView2 + Mermaid ([#45267](https://github.com/microsoft/PowerToys/issues/45267), 2026-02).
- **Markdown Monster** (WPF + several WebView2s): caches and reuses one `CoreWebView2Environment`
  ([2023](https://weblog.west-wind.com/posts/2023/Oct/31/Caching-your-WebView-Environment-to-manage-multiple-WebView2-Controls)),
  waits for the document to load
  ([2025](https://weblog.west-wind.com/posts/2025/May/06/WebView2-Waiting-for-Document-Loaded)),
  fights with invisible initialization ([2022](https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization)),
  keeps temporary render files in `%appdata%\Markdown Monster\temp` (judging by the post about the 2 MB limit,
  the preview is loaded via a file).

### 2.12 Delays before `main` (important for all prototypes) [measured]

Over several hundred `wv2probe` launches, six times the process was created on time (`GetProcessTimes`: 0.7–4 ms from t0), but the first line
of `wWinMain` ran after **27.1 s, 15.1 s, 6.3 s, 2.3 s, 1.9 s and 0.56 s** (normally after 22–50 ms). There is nothing of our own in the exe before `main` (static CRT
and system DLLs), and WebView2 has nothing to do with it. This is the OS: the image loader, Defender scanning, page-in with
the disk and CPU clogged by parallel builds. Without the `main` mark such an outlier would look like
"WebView2 sometimes takes 27 seconds to start". See the harness recommendations in §16.

---

## 3. Tauri 2 and wry

- Versions: **tauri 2.11.5** (2026-07-01, stable), **tauri 3.0.0-alpha.1** (2026-09-15), **wry 0.57.0**,
  **webview2-com 0.39.1** (crates.io). In Tauri 3 alpha the runtime is chosen at build time: `tauri-runtime-wry`
  (the system WebView) **or `tauri-runtime-cef`** (its own Chromium)
  ([release tauri-runtime-cef v3.0.0-alpha.1](https://github.com/tauri-apps/tauri/releases/tag/tauri-runtime-cef-v3.0.0-alpha.1)).
- **Overhead on top of bare WebView2 on Windows.** wry calls the same
  `CreateCoreWebView2EnvironmentWithOptions` / `CreateCoreWebView2Controller` via webview2-com, so the floor is the same
  (§2.2). On top of that come:
  1. a window via tao/winit and Rust runtime initialization (single-digit ms);
  2. IPC scripts via `AddScriptToExecuteOnDocumentCreated` (Tauri);
  3. **the app's assets go through a custom protocol**. On Windows this is `http://tauri.localhost/...`, implemented
     via **WebResourceRequested** (commit
     ["custom protocol on Windows now uses the http scheme"](https://github.com/tauri-apps/tauri/commit/4cb51a2d56cfcae0749062c79ede5236bd8c02c2)).
     [measured] For a single page, WebResourceRequested versus NavigateToString gives +11 ms, i.e. noise. The `.localhost` zone
     does not hit the `.local` trap (vhost-localhost −24 ms). For large assets there are complaints about throughput
     ([#4197](https://github.com/tauri-apps/tauri/issues/4197));
  4. **wry by default** passes `--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection`, and if you override the
     arguments, the defaults are lost ([tauri #6416](https://github.com/tauri-apps/tauri/issues/6416)).
     [measured] −26 ms, within noise.
- **Expectation:** Tauri/wry ≈ bare WebView2 + 10–50 ms. The exact numbers will come from the `rust-webview` prototype on the common harness.
- Known startup problems: a "white flash" before the first frame (workaround: `visible:false` + showing once ready),
  and on some machines a startup of more than 20 s before `setup` with no cause found
  ([#13727](https://github.com/tauri-apps/tauri/issues/13727), status "upstream"). It resembles my delays before `main` (§2.12).
- Published "Tauri versus Electron" comparisons (N=1 on a MacBook, "difference was negligible" for startup —
  [Hopp, 2025-04](https://www.gethopp.app/blog/tauri-vs-electron); the marketing "Electron 12 s versus Tauri 1.8 s")
  are useless for our question.

## 4. Electron (and why MD apps built on it are slow)

- Version: **electron 44.4.3** (npm, 2026-09-18).
- Startup mechanics: the same Chromium (browser → GPU/utility → renderer) as WebView2, **plus** Node.js in the main process,
  **plus** its own copy of Chromium (~150–250 MB of binaries, **no hardlink with Edge**, i.e. a cold page-in from disk),
  **plus** loading and executing the app's JS bundle.
- Why MD apps in particular take seconds to start:
  - **JS loading:** "the biggest bottleneck in app launch is obviously the process to load JavaScript … 1–2 secs in my app"
    (Inkdrop, [devas.life, 2020](https://www.devas.life/how-to-make-your-electron-app-launch-1000ms-faster-32ce1e0bb52c)).
    In the same post a V8 snapshot gave ≈ 1 s (4 → 3 s on macOS), and Atom got ~500 ms. The official guide
    ([Performance](https://www.electronjs.org/docs/latest/tutorial/performance)): `require()` is expensive, you should bundle
    and defer module loading.
  - **"Workspace" initialization:** plugins, vault indexing, restoring tabs.
    - Obsidian: 4–5 s and up to 20+ s with plugins ([forum](https://forum.obsidian.md/t/obsidian-time-to-start-is-wildly-inconsistent/110681)).
    - Typora 0.9.53: 7–8 s on a double click ([#1670](https://github.com/typora/typora-issues/issues/1670), 2018).
    - MarkText: 12 s cold / 3 s warm ([#3862](https://github.com/marktext/marktext/issues/3862), 2024, M1).
    - Zettlr 2.2.5: 40 s on Windows 10 ([#3391](https://github.com/Zettlr/Zettlr/issues/3391), 2022).
  - **An editor instead of a reader:** CodeMirror/ProseMirror engines, highlighting, spellcheck — all of this loads before the first screen.
- Electron's progress in 2026: Node startup snapshot + build-time V8 code cache
  ([PR #51831](https://github.com/electron/electron/pull/51831), 2026-06, Electron 42;
  [PR #53136](https://github.com/electron/electron/pull/53136), 2026-08).
  The authors' numbers (Linux): utilityProcess.fork 86 → 40 ms, renderer with Node 91 → 65 ms, main process "within
  run-to-run noise". These are **tens of milliseconds**; the Chromium floor remains.
- For FastMD: Electron is the worst of the web options in startup and size (~250 MB, 244 MiB at Hopp).
  It makes sense only as an external baseline (the `electron` agent has one).

## 5. Neutralinojs and Wails

- **Neutralinojs 6.9.0** (2026-07-24). A C++ binary of ~2 MB + the system WebView (WebView2 on Windows). Architecture:
  **a local HTTP static server + a WebSocket server for IPC**, the window loads `http://localhost:<port>/`
  ([docs](https://neutralino.js.org/docs/), [DeepWiki](https://deepwiki.com/neutralinojs/neutralinojs)).
  At startup, bringing up the server, an HTTP request and a WebSocket handshake are added on top of WebView2.
  **It cannot be faster than bare WebView2.**
- **Wails**: v2.12.0 (stable, 2026-03-26) and v3.0.0-beta.23 (2026-09-16). Go + WebView2 (v3 has a pure Go loader
  instead of `WebView2Loader.dll`); assets via the built-in asset server. The "~15 MB, sub-0.5 s startup" claim comes from Wails itself,
  not independently confirmed ([digitalapplied, 2026](https://www.digitalapplied.com/blog/desktop-apps-web-stack-tauri-electron-deno-wails-2026)).
  The floor is the same as for WebView2.
- For FastMD both are "WebView2 with an extra layer". There is no point: if WebView2, then directly (C++) or via wry.

## 6. Sciter (Sciter.JS)

- Version **6.0.5.0** (SDK commit of 2026-09-05, [gitlab sciter-js-sdk](https://gitlab.com/sciter-engine/sciter-js-sdk)).
  [measured] `bin/windows/x64/sciter.dll` = **19.3 MB**, `scapp.exe` = 19.3 MB (the engine is statically inside), unsigned
  and without a version resource. The marketing "5+ MB" ([sciter.com](https://sciter.com/)) is outdated.
- A single-process engine: its own HTML/CSS (+ its own `flow`/`flex` layout; `display:flex/grid` are supported
  "for popular cases", not fully — [Sciter docs](https://docs.sciter.com/docs/CSS/flows-and-flexes/),
  [flexbox vs flow](https://sciter.com/flexbox-browser-versus-flow-flex-sciter/)), a QuickJS-based JS engine,
  Direct2D/Skia rendering.
- License ([prices](https://sciter.com/prices/)): the binary is free "as it is published". Source code: Indie $310
  (≤3 employees, Windows only), Indie+ $620, Business $1,620–2,720. The requirement to mention "this code contains Sciter
  engine" in About is lifted only at Enterprise++.
- [measured, preliminary]:
  - a tiny page: a window with content ≈ **0.47 s** under load (without `<meta charset>` Sciter reads the file as cp1251);
  - medium (ready HTML with `style.css`): the window appeared after **1.7–2.5 s** (5 launches), the content in the screenshot
    is not drawn yet ~0.7 s after the window appears, and is drawn after 6 s;
  - the typography is good, color emoji are there, but **the `max-width:860px; margin:0 auto` column drifts to the right**
    (horizontal scrolling). CSS compatibility is not browser-grade;
  - with a `file:///` URL the page title never appeared.

  The method here is crude: the window title and a screenshot. An honest measurement should be done by the `litehtml-sciter` prototype.
- Fidelity: highlight.js (pure JS) should work in QuickJS. KaTeX (CSS layout of formulas) and Mermaid (SVG + `getBBox` +
  lots of DOM API) are high risk.

## 7. litehtml

- **v0.10** (2026-06-13), last commit 2026-08-31, BSD-3 license (+ gumbo, Apache-2.0)
  ([GitHub](https://github.com/litehtml/litehtml)).
- The essence: it parses HTML/CSS and **lays out** the blocks. Fonts, text, images and drawing are provided by the application via
  `document_container`. For FastMD this means: DirectWrite/Direct2D for text and drawing, our own font cache,
  our own highlighting. In essence, "a native renderer with a ready-made CSS layout".
- Support: "most HTML tags and CSS properties" (CSS2 + part of CSS3: tables, float, positioning, selectors;
  according to [DeepWiki](https://deepwiki.com/litehtml/litehtml) there is flex layout too, it is not mentioned in the README, needs checking).
  The author: "not fully compatible with HTML/CSS standards",
  "not recommended as a full-featured HTML engine". No JS, so KaTeX/Mermaid/hljs only natively or pre-rendered to SVG.
- Who uses it: **Qt Creator** — the default help viewer is based on litehtml (`qlitehtml`,
  [doc.qt.io](https://doc.qt.io/qtcreator/creator-how-to-get-help.html),
  [attribution](https://doc.qt.io/qtcreator/qtassistant-attribution-litehtml.html)), the litebrowser demo (Win/Linux/Haiku).
- Pros for FastMD: one process, instant startup (limited only by our own code), a "GitHub-like" CSS theme carries over
  almost one to one, hundreds of KB of code.
  Risks: CSS gaps (grid, `:not()`, `max-content`, some units), no incremental layout for 100k+
  nodes, text (selection, shaping, bidi) is entirely on us. The `litehtml-sciter` prototype will show how it works in practice.

## 8. Ultralight

- **1.4** ([2025-04-21](https://ultralig.ht/blog/ultralight-1-4-out-now)), core **WebKit 615.1.18** (≈ Safari 16.4),
  single process, GPU or CPU rendering, JavaScriptCore.
- License ([pricing](https://ultralig.ht/pricing)): Free — "Limited performance, Limited feature-set, Indie devs only
  (< $100K)", PC only. Pro — **$3,000/year per application** (< $10M revenue). Source code only in Enterprise.
- Downloading the SDK now **requires signing in to an account** ([download](https://ultralig.ht/download/)), so I
  practically did not test it. Claimed: "near-instant cold starts", "~1/10 RAM of Chromium"; no numbers.
- For FastMD: closed source, a lagging WebKit, "limited performance" in the free version. **Not recommended.**

## 9. RmlUi

- **RmlUi 6.3** (2026-08-22, [GitHub](https://github.com/mikke89/RmlUi)). "Based around XHTML1 and CSS2 while borrowing
  features from HTML5 and CSS3": flexbox, transforms, animations, media queries, border-radius, data binding, decorators.
  **It does not aim for CSS/HTML compatibility**; documents are written "for RmlUi". No JS (there is a Lua plugin).
- Aimed at game UIs (rendering via OpenGL/Vulkan/DX backends). For typography of long texts (selection,
  copying, CJK/bidi, GFM tables) a lot would have to be added. **Not suitable for FastMD.**

## 10. Blitz (DioxusLabs) [measured]

- Repository [DioxusLabs/blitz](https://github.com/DioxusLabs/blitz), commit `e7bf7bc` (2026-09-18), packages
  `0.3.0-beta.2`. README: "Blitz is currently in a **beta** state. It can already render many popular no-JS websites".
  Components: **Stylo** (Firefox's CSS engine), html5ever, **Taffy** (flex/grid/block), **Parley** (text), AccessKit,
  rendering via `anyrender` (Vello GPU, **vello_hybrid**, **vello_cpu**, Skia). Support: "flexbox, grid, table, block,
  inline, absolute/fixed", complex selectors, media queries, CSS variables, float (feature).
- The repository contains a ready-made **Markdown viewer `rdme`** (`apps/readme`: comrak 0.52 + github-markdown.css).
  I built it with a patch adding PROTOCOL marks (`research/lab/blitz/src/apps/readme/src/fmb.rs`: `main`, `parsed`,
  `dom_built`, `t_content` = the first `RedrawRequested` + DwmFlush). The `cargo build --release -p rdme` build took
  5 min 36 s with `-j 8` under load.

| Build | exe | t_content medium (median of 5) | `main` | parse (comrak) | DOM build | Window visible | WS |
|---|---|---|---|---|---|---|---|
| default (`hybrid` = vello_hybrid on wgpu) | 28.4 MB | **1,577 ms** (1,491–2,306) | 91–180 | ~23–60 ms | ~7–13 ms | ~125 ms | 205 MB |
| `cpu-softbuffer` (vello_cpu) | 22.5 MB | **564 ms** (477–658) | 97–141 | ~30–45 ms | ~8–13 ms | ~145 ms | **59 MB** |
| `cpu-softbuffer`, **large.md** | | **5,780–6,300 ms** | 160–208 | ~430–450 ms | ~760–860 ms | | |

- Engine phases from the `log-phase-times` logs for medium: **Resolve 40 ms** (style 11, construct 8 + 10, layout 8.8 ms),
  vello_cpu raster 7 ms, softbuffer present 8 ms. The engine itself is fast. The "DOM built → first frame" gap (~400 ms in the
  CPU build, ~1.35 s in the GPU build) is window creation, rasterizer/GPU initialization (wgpu adapter/device,
  shaders) and probably enumeration of system fonts (fontique). **This is a place for optimization, not the engine's limit.**
- Large: the first Resolve takes 3.5 s (style 1.9 s, layout 0.85 s, construct 0.4 s). Layout is not limited to the visible area;
  the whole document is laid out.
- Fidelity (screenshot `research/lab/blitz/rdme-medium.png`): color emoji, CJK, Cyrillic, bold/italic, nested
  lists (`a.`/`b.` per the GitHub CSS), tables, code. The theme follows the system one (dark). It looks like GitHub.
- Downsides: **no JavaScript** (KaTeX/Mermaid/hljs only natively: highlighting via syntect/tree-sitter, formulas
  via MathML or a custom renderer), beta, the API changes, Windows specifics (IME, text selection, printing) are untested,
  cold start of the GPU backend.
- Verdict: **the main candidate among "lightweight HTML engines"** if real CSS is needed without Chromium. A separate
  `rust-blitz` prototype on the common harness and an analysis of the 400 ms to the first frame make sense.

## 11. Servo [measured]

- Status: `servo` 0.1.0 was first published on crates.io on **2026-04-13** ([blog](https://servo.org/blog/2026/04/13/servo-0.1.0-release/)),
  then 0.5.0 (2026-08-17). There is a plan for LTS releases ([Phoronix](https://www.phoronix.com/news/Servo-Embed-Crates-LTS)).
  The embedding API is evolving, "many WebView related APIs are still missing" ([PR #44984, C API](https://github.com/servo/servo/pull/44984)).
- Nightly **2026-09-17** (`servo-x86_64-windows-msvc.zip` 103 MB, **276 MB** unpacked: lots of GStreamer DLLs).
  The `download.servo.org` link from the Servo Book currently returns 404 (GitHub Pages "Site not found"); I took it from
  [servo/servo-nightly-builds](https://github.com/servo/servo-nightly-builds).
- [measured] `servoshell.exe file:///…/t-medium.html` (ready HTML + style.css, signaled via `document.title`):
  window ≈ 350–640 ms, **content ≈ 1.2 s** (1,210–1,496 ms, 3 launches under load; the first one 3.1 s), WS ≈ 230 MB.
- Fidelity (screenshot `research/lab/servo/servo-medium.png`): **`<ol>` numbering starts at `0.`**, **no bold/italic and
  no semibold headings** (apparently the weights of the Segoe UI Variable variable font are not applied), emoji are monochrome.
  CJK and Cyrillic are fine.
- Verdict: **not** for FastMD right now (startup, size, layout bugs). Keep an eye on the LTS and the C API.

## 12. CEF

- Its own Chromium in the application's process. Binary distribution: `libcef.dll` 100+ MB, a minimal set of ~117 MB and more
  ([CEF forum](https://magpcss.org/ceforum/viewtopic.php?f=6&t=15213)). The version follows Chromium
  (CEF 143.0.13 / Chromium 143 — December 2025).
- By construction, startup is no better than WebView2: the same multi-process model, **but without sharing binaries with Edge**, i.e.
  a cold page-in of its own ~200+ MB. Upside: the version is pinned and does not update under your feet. In 2026 CEF became an option
  in Tauri 3 (`tauri-runtime-cef`).
- For FastMD: +200 MB to the distribution for the sake of a pinned version. **Not needed.**

## 13. Fidelity: KaTeX, highlight.js, Mermaid in WebView2 [measured, set D]

All three libraries work in WebView2 **without modifications**: it is ordinary Chromium 153. The only question is the cost. Time inside
the page (`performance.now()`) does not depend on process startup, so it is more reliable than `t_content` under load.
Scripts were included as an external file via WebResourceRequested (`https://fastmd.req/lib/...`) unless stated otherwise.

| Library (version, size) | Script execution | Work | Total to the first frame | Comment |
|---|---|---|---|---|
| **highlight.js 11.12.0** (`highlight.min.js` 129 KB, common set) | **3–7 ms** | **22–33 ms** for medium's 24 blocks (synchronous `highlightElement`) | ~+30–40 ms | `hljs.highlightAll()` with `readyState=loading` defers the work to `DOMContentLoaded`, i.e. before the first frame. For the first screen, native highlighting or highlighting only the visible blocks is better |
| **KaTeX 0.18.7** (`katex.min.js` 273 KB + CSS 25 KB + woff2) | 4–15 ms (35 ms the first time) | 35–62 ms for 80 formulas | +50–80 ms, **the fonts arrive another ~110 ms later** (`document.fonts.ready`) | If shown before the fonts load, there will be a relayout. The fonts can be embedded (data: URI) or preloaded |
| **Mermaid 12.0.0** (`mermaid.min.js` **5.5 MB**) | 87–240 ms (warm), **the first load in one series — 4.96 s** | 290–480 ms for 2 diagrams | **+300–700 ms** | Load **only** if there is a `mermaid` block and **after** the first screen. The first load of 5.5 MB most likely hit Defender scanning the file when the host read it |
| Mermaid via `file://` | 200–280 ms | 280–420 ms | | executing a `file://` script is 2 times slower than via WebResourceRequested (apparently no streaming compilation / cache) |
| Mermaid + `--enable-features=msWebView2CodeCache` | 87–120 ms from the 3rd launch (97–123 without the flag) | | ≈ −10–15 % on execution | The bytecode cache flag for resources from vhost/WebResourceRequested (from the official list) gives little |
| markdown-it 15.0.2 in the page (120 KB) | 8 ms | 30 ms on medium, 520–590 ms on large | +50 ms (medium) | better to parse natively |
| highlight.js **inlined** in the HTML (NavigateToString) | interval 118–182 ms | | | the interval includes the parser yielding and the first frame, not comparable; inline scripts are not cached and are compiled on the main thread |

Conclusion for FastMD: full GitHub compatibility (highlighting + formulas + diagrams) is available in WebView2 "out of the box".
highlight.js and KaTeX are almost free (tens of ms), Mermaid is expensive (hundreds of ms + 5.5 MB).
**If the main renderer is native**, there are two paths:
(a) highlighting natively (tree-sitter/syntect/a custom lexer), formulas via MathML or a micro-TeX, Mermaid via a background
WebView2 into SVG with a cache keyed by the block hash;
(b) a "rich mode" entirely on WebView2 on demand.

---

## 14. Summary table

**A control rerun in one time window** [measured, ~01:25, moderate load, 5 launches, medium]:

| Variant | t_content (median) | `main` | Comment |
|---|---|---|---|
| WebView2, warm UDF, NavigateToString | 731 | 50 | controller_ready 481, browser/GPU/renderer created at 123/294/443 ms |
| WebView2 + holder (shared browser process) | **396** | 22 | controller_ready 204 |
| Blitz rdme, CPU raster (vello_cpu) | **375** | 92 | parsing 21 ms, DOM 7 ms, window + rasterizer → frame 248 ms |
| Blitz rdme, GPU (vello_hybrid) | 1,226 | 103 | ~1.1 s for GPU renderer initialization |
| Servo nightly (servoshell) | 815 | — | window 305 ms |

Separately: `main` for the 22-megabyte `rdme.exe` is consistently ~90 ms, for the 268-kilobyte `wv2probe.exe` 22–50 ms.
Exe size affects the time to `main` (image loading and Defender scanning).

| Engine | Version (September 2026) | t_content medium, **[measured]** on this machine | Memory | Added to distribution | CSS / layout | JS: hljs / KaTeX / Mermaid | License | Main risk |
|---|---|---|---|---|---|---|---|---|
| Win32 GDI (floor, baseline) | — | ~90 ms (quiet) | ~5 MB | ~0.1 MB | — | — | — | — |
| **WebView2** (C++, NavigateToString) | runtime 153, SDK 1.0.4191.47 | **~590** quiet / 700–1000 under load | 165–185 MB priv (5 processes) | 0 (evergreen) + ~0.2 MB loader | the reference (Chromium) | yes / yes / yes | free | ~0.5 s floor; depends on the OS and Edge; crashpad leak with a shared UDF |
| WebView2 + shared browser process | 〃 | **~400–545** (730–930 without the holder in the same window) | +resident ~170 MB | 〃 | 〃 | 〃 | 〃 | resident process, crashpad leak |
| WebView2 fully resident | 〃 | see the `cpp-webview2-resident` prototype | constantly ~170–260 MB | 〃 | 〃 | 〃 | 〃 | autostart, "honesty" of the measurement |
| Tauri 2 / wry | 2.11.5 / 0.57 | ≈ WebView2 + 10–50 ms (estimate) | ≈ WebView2 | 3–10 MB | 〃 | 〃 | MIT/Apache | as for WebView2 |
| Neutralino / Wails | 6.9.0 / 2.12, 3.0-β23 | ≥ WebView2 (architecturally) | ≈ WebView2 | 2–15 MB | 〃 | 〃 | MIT | an extra layer (HTTP/WS) |
| Electron | 44.4.3 | not measured (the `electron` agent); by construction ≥ WebView2 | 200–450 MB | ~250 MB | 〃 | 〃 | MIT | size, its own Chromium without hardlink |
| CEF | 14x | ≥ WebView2 (estimate) | ≈ Electron | 120–250 MB | 〃 | 〃 | BSD | size |
| **Blitz** (rdme, CPU raster) | 0.3.0-beta.2 | **375–560** (moderate/heavy load; engine 40 ms) | **59 MB WS** | **22 MB exe** | Stylo: flex/grid/table/float, very close to GitHub | **no JS** | MIT/Apache | beta; 400 ms init before the frame; large 5.8 s |
| Blitz (GPU, vello_hybrid) | 〃 | 1,200–1,500 | 205 MB WS | 28 MB | 〃 | no | 〃 | GPU initialization |
| Servo (servoshell) | nightly 2026-09-17 / crate 0.5.0 | 800–1,200 | ~230 MB WS | 276 MB | noticeable bugs (ol, weights) | yes (SpiderMonkey) | MPL-2.0 | fidelity, size |
| Sciter.JS | 6.0.5.0 | preliminary 1.7–2.5 s (medium), 0.47 s (tiny) | ~95–120 MB WS | 19.3 MB | its own model (flow/flex), the column "drifted" | QuickJS: hljs probably, KaTeX/Mermaid questionable | binary free, source code from $310 | CSS compatibility, closed source |
| litehtml | 0.10 | not measured (the `litehtml-sciter` agent); limited by our own code | single-digit MB | ~1 MB | CSS2 + part of CSS3 | no | BSD-3 | CSS gaps, all text handling is on us |
| Ultralight | 1.4 (WebKit 615) | not measured (SDK behind a login) | "1/10 of Chromium" (claimed) | ~30–40 MB (estimate) | WebKit from 2023 | JSC: yes | free < $100K with "limited performance"; Pro $3k/year | license, closed source |
| RmlUi | 6.3 | not measured | small | ~1–2 MB | a subset, "for RmlUi" | no (Lua) | MIT | not for documents |

---

## 15. Conclusions for FastMD

### 15.1 The main points

1. **Do not build FastMD on "a new WebView2 process per double click".** On this machine, in the favorable
   case (Edge running, DLLs in memory, NVMe), the floor is ≈ 0.5–0.6 s to content. Flags barely help, document size
   does not matter, so there is nothing to optimize: the time is eaten by browser/GPU/renderer. Electron, CEF, Tauri, Neutralino and Wails
   live on the same or an even heavier Chromium and **will not be faster than this floor**.
2. **The main path to the "bullet" is a native renderer** (Direct2D/DirectWrite + md4c; results from the `cpp-d2d`,
   `winui3`, `rust-gpu` agents). From web engines, take **the CSS model and the theme**:
   - option A: **litehtml** as a layout engine with a DirectWrite container. GitHub CSS almost as is, one process,
     startup limited only by our own code;
   - option B: **Blitz** (Rust) — real CSS via Stylo, a single 22 MB exe, 59 MB of memory, good layout quality.
     Currently 0.37–0.56 s, but the engine itself works for 40 ms; the rest is process startup and window/rasterizer initialization, which can be dissected.
     Beta, no JS.
3. **Keep WebView2 as a "rich mode" or a resident:**
   - **lazy rich mode:** the native renderer draws the first screen, and WebView2 is brought up in the background only if the
     document contains `mermaid`/`math` or the user asked for "like on GitHub". Mermaid/KaTeX can be rendered in a
     background WebView2 into SVG and cached;
   - **resident variant** (`-resident` per PROTOCOL §6): a process holds a pre-warmed controller and renderer, the launcher
     passes the path. This is the only way to get Chromium layout in ~100–200 ms. The price: +170–260 MB in the background,
     autostart, behavior after a runtime update (`NewBrowserVersionAvailable` → restart).

### 15.2 If WebView2 after all — a checklist (all confirmed by the measurements above)

1. `CreateCoreWebView2EnvironmentWithOptions` — the first line of `WinMain`, create the window in parallel,
   the controller on a visible window. `put_DefaultBackgroundColor` set to the theme color (no white flash).
2. A persistent UDF in `%LOCALAPPDATA%\FastMD\WebView2`, **the same options** in all instances (otherwise the shared
   browser process will not connect and creation will fail). Do not enable InPrivate.
3. Document: parse Markdown natively, deliver ready HTML. NavigateToString when UTF-8 < 1.5 MB, otherwise a temp file
   (`file://`) or a vhost. For large documents deliver **the first screen right away** and append the rest
   (`PostWebMessage` in portions, `content-visibility: auto`).
4. Images and assets: `SetVirtualHostNameToFolderMapping` to a **`*.example`** or `*.invalid` host
   (never `.local` and never a name without a dot: +1.9 s per navigation).
5. Flags: none gives a stable gain (`--in-process-gpu` −90 ms in one series and −11 ms in another).
   Better not to use them; Microsoft is against flags in production. Definitely do not use `--disable-gpu` and `--no-sandbox`.
6. Load Mermaid (5.5 MB) and KaTeX **lazily**, only when the corresponding blocks are present. highlight.js — either lazily,
   or highlight natively (tree-sitter/syntect) at HTML generation time. Details in §13.
7. A shared browser process between instances (the second document opens faster) — yes, but taking into account the
   `crashpad-handler` leak (§2.5): it is better for the first instance to open new windows itself (single-instance + `WM_COPYDATA`).
8. Evergreen, not Fixed: 0 MB, hardlink with Edge, security updates. Test on the Beta/Dev channels.

### 15.3 Risks

| Risk | Where | What to do |
|---|---|---|
| A WebView2 startup floor of ~0.5 s and **instability** (Defender, memory, Edge not running, runtime update) | all Chromium stacks | a native first screen; a resident |
| A `crashpad-handler` leak when connecting to a shared browser process | WebView2 153 | a single-instance host; an issue in WebView2Feedback |
| Delays of seconds before `main` under load | the OS, any stack | the `main` mark is mandatory; a signed exe and Defender exclusions are not a guaranteed cure |
| `.local` and names without a dot → system resolution ~2 s | WebView2, Tauri (with a custom host) | the `.example` / `.invalid` / `.localhost` zone |
| Large documents: 3–6 s and 0.8–1 GB | Chromium, Blitz | portions, virtualization, `content-visibility` |
| No JS → no Mermaid/KaTeX | litehtml, Blitz, RmlUi, D2D | native equivalents or a background WebView2 → SVG cache |
| Licenses and closed source | Sciter (paid source code), Ultralight ($3k/year, limited free) | prefer BSD/MIT stacks |
| Immaturity | Blitz (beta), Servo (fidelity) | a prototype + pinning the version/commit |

---

## 16. Notes on the harness and the protocol

1. **Outliers before `main`.** Delays of 0.56–27 s between `CreateProcess` and the first line of `main` (§2.12) under load are not
   a stack problem. Proposal: in `bench.py report` show `main` as a separate column and flag runs where
   `main` > 200 ms (or more than median×3). In the final run on an idle machine such runs should be repeated, not averaged.
2. **Hidden windows and rAF.** If a `-resident` variant keeps WebView2 in a hidden window, rAF does not fire there. The resident
   must mark `t_content` after the window is shown (as in the protocol), otherwise it will hang.
3. **Order and load.** Under load the spread of a single configuration is hundreds of ms. For comparisons the
   `--order interleaved` mode (it exists) and output of paired differences per round are useful.
4. **The browser process tail — checked, the harness is fine.** [measured] After a clean host exit the WebView2
   browser process lives for another **150–290 ms**, once **2.5 s** (4 probes). `bench.py` does `TerminateJobObject` +
   `wait_job_empty`, i.e. kills the browser in the middle of a regular shutdown. I checked whether a "dirty" exit slows down
   the next launch (`kill_vs_graceful.py`, 7 pairs, interleaved): median **612 ms** after a regular exit and
   **618 ms** after a kill, no difference.

---

## 17. Sources

WebView2 (Microsoft Learn): [Performance best practices](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance) (updated 2026-09-02) ·
[Process model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/process-model) ·
[Browser flags](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/webview-features-flags) (2026-08-31) ·
[Using local content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content) ·
[Distribution](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution) (2026-09-14) ·
[Evergreen vs fixed](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/evergreen-vs-fixed-version).
WebView2Feedback: [#1910](https://github.com/MicrosoftEdge/WebView2Feedback/issues/1910) · [#1540](https://github.com/MicrosoftEdge/WebView2Feedback/issues/1540) ·
[#2381](https://github.com/MicrosoftEdge/WebView2Feedback/issues/2381) · [#3398](https://github.com/MicrosoftEdge/WebView2Feedback/issues/3398) ·
[#5321](https://github.com/MicrosoftEdge/WebView2Feedback/issues/5321) · [#2323](https://github.com/MicrosoftEdge/WebView2Feedback/issues/2323).
Rick Strahl: [2 MB limit](https://weblog.west-wind.com/posts/2024/Jul/22/Work-around-the-WebView2-NavigateToString-2mb-Size-Limit) ·
[Visibility](https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization) ·
[Environment caching](https://weblog.west-wind.com/posts/2023/Oct/31/Caching-your-WebView-Environment-to-manage-multiple-WebView2-Controls) ·
[Waiting for document](https://weblog.west-wind.com/posts/2025/May/06/WebView2-Waiting-for-Document-Loaded).
PowerToys: [MarkdownPreviewHandlerControl.cs](https://github.com/microsoft/PowerToys/blob/main/src/modules/previewpane/MarkdownPreviewHandler/MarkdownPreviewHandlerControl.cs) ·
[PR #47391](https://github.com/microsoft/PowerToys/pull/47391) · [#45267](https://github.com/microsoft/PowerToys/issues/45267) · [#46087](https://github.com/microsoft/powertoys/issues/46087).
Tauri/wry: [tauri #6416](https://github.com/tauri-apps/tauri/issues/6416) · [#13727](https://github.com/tauri-apps/tauri/issues/13727) ·
[#4197](https://github.com/tauri-apps/tauri/issues/4197) · [http scheme commit](https://github.com/tauri-apps/tauri/commit/4cb51a2d56cfcae0749062c79ede5236bd8c02c2) ·
[tauri-runtime-cef v3.0.0-alpha.1](https://github.com/tauri-apps/tauri/releases/tag/tauri-runtime-cef-v3.0.0-alpha.1) ·
[Hopp: Tauri vs Electron](https://www.gethopp.app/blog/tauri-vs-electron).
Electron: [Performance](https://www.electronjs.org/docs/latest/tutorial/performance) · [PR #51831](https://github.com/electron/electron/pull/51831) ·
[PR #53136](https://github.com/electron/electron/pull/53136) · [Inkdrop/devas.life](https://www.devas.life/how-to-make-your-electron-app-launch-1000ms-faster-32ce1e0bb52c) ·
[Typora #1670](https://github.com/typora/typora-issues/issues/1670) · [MarkText #3862](https://github.com/marktext/marktext/issues/3862) ·
[Zettlr #3391](https://github.com/Zettlr/Zettlr/issues/3391) · [Obsidian forum](https://forum.obsidian.md/t/obsidian-time-to-start-is-wildly-inconsistent/110681).
Neutralino/Wails: [neutralino.js.org](https://neutralino.js.org/docs/) · [DeepWiki](https://deepwiki.com/neutralinojs/neutralinojs) · [Wails v3](https://v3.wails.io/whats-new/).
Sciter: [sciter.com](https://sciter.com/) · [prices](https://sciter.com/prices/) · [flows & flexes](https://docs.sciter.com/docs/CSS/flows-and-flexes/) · [SDK](https://gitlab.com/sciter-engine/sciter-js-sdk).
litehtml: [GitHub](https://github.com/litehtml/litehtml) · [Qt Creator help](https://doc.qt.io/qtcreator/creator-how-to-get-help.html).
Ultralight: [1.4](https://ultralig.ht/blog/ultralight-1-4-out-now) · [pricing](https://ultralig.ht/pricing).
RmlUi: [GitHub](https://github.com/mikke89/RmlUi).
Blitz: [GitHub](https://github.com/DioxusLabs/blitz) · [Web Engines Hackfest 2024 slides](https://webengineshackfest.org/2024/slides/blitz_a_truly_modular_hackable_web_renderer_by_nico_burns.pdf).
Servo: [0.1.0 release](https://servo.org/blog/2026/04/13/servo-0.1.0-release/) · [Phoronix LTS](https://www.phoronix.com/news/Servo-Embed-Crates-LTS) ·
[nightly builds](https://github.com/servo/servo-nightly-builds) · [C API PR #44984](https://github.com/servo/servo/pull/44984) · [Servo Book](https://book.servo.org/trying/getting-servoshell.html).
CEF: [CEF forum: footprint](https://magpcss.org/ceforum/viewtopic.php?f=6&t=15213) · [Wikipedia](https://en.wikipedia.org/wiki/Chromium_Embedded_Framework).

---

## Appendix: how to reproduce

```powershell
cd C:/Main/Projects/FastMD/research/lab/web-engines
npm install                      # markdown-it, highlight.js, katex, mermaid (уже установлены)
node gen_pages.mjs               # pages/*.html
# сборка пробника (SDK распакован в wv2sdk/)
../../../bench/tools/msvc.cmd cl /nologo /utf-8 /O2 /MT /EHsc /std:c++20 /DUNICODE /D_UNICODE /I wv2sdk/build/native/include wv2probe.cpp /Fo:out/ /Fe:out/wv2probe.exe /link /SUBSYSTEM:WINDOWS wv2sdk/build/native/x64/WebView2LoaderStatic.lib
python drive.py warm --page pages/p-medium.html --runs 7                 # один сценарий
python drive.py shared --page pages/p-medium.html --udf shared --runs 7  # с «держателем»
python multi.py B-flags sets/B-flags.json --rounds 7                     # чередование
$env:PYTHONIOENCODING='utf-8'; python paired.py results/B-flags.json base
# Blitz
cd ../blitz/src; $env:CARGO_TARGET_DIR='../target-cpu'
cargo build --release -p rdme --no-default-features --features "cpu-softbuffer,comrak,floats,scrollbars,log-phase-times"
cd ../../web-engines; python proto_run.py --runs 5 -- ../blitz/target-cpu/release/rdme.exe ../../../../../bench/corpus/medium.md
# Servo / Sciter
python title_watch.py --runs 3 -- ../servo/servo/servo/servoshell.exe file:///C:/Main/Projects/FastMD/research/lab/web-engines/pages/t-medium.html
```

Raw results: `research/lab/web-engines/results/*.json|*.log`:
- matrix1/matrix2 — early sequential series (in matrix1 some scenarios were spoiled by the hung holder,
  described in §2.5; in matrix2 the scenarios with `--flags`/`--args=` without spaces did not start because of argparse and
  were remeasured in sets B/C);
- B-flags, C-load, D-libs, F-hosts, G-gpu — interleaved sets;
- E-*, Q-* — series with the holder and the control rerun.

Additional scripts: `crashpad_check.py` (the crashpad-handler leak), `kill_vs_graceful.py` (the effect of killing the
browser process on the next launch). Screenshots: `research/lab/servo/servo-medium.png`,
`research/lab/blitz/rdme-medium.png`, `research/lab/sciter/sciter-medium-settle6.png`, `sciter-tiny.png`.
Disk: `research/lab/blitz/target*` take up ~3 GB (cargo intermediate files); the exes are in `target*/release/rdme.exe`,
the folders can be deleted if no rebuild is needed.
