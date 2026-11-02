[Русский](README.md) | **English**

# rust-webview — Rust + wry / Tauri → WebView2

A FastMD prototype in Rust on top of the system WebView2: `pulldown-cmark` turns Markdown into HTML,
the shared `bench/common/style.css` handles typography, and WebView2 (Chromium) draws the page.
There are four variants here: wry with a minimal Win32 window (the best one), wry with a tao window (the canonical combination),
a minimal Tauri 2 app (to assess Tauri's overhead), and the unsafe
`--single-process` (to find the ceiling of what can be squeezed out of WebView2 at all).

## TL;DR

| variant | small | medium | large | t_window | processes | dist |
|---|---|---|---|---|---|---|
| **rust-webview** (wry + raw Win32) | **558** | **563** | **598** | 93 | 7 | 2.5 MB |
| rust-webview-tao (wry + tao) | 579 | 571 | 603 | 231–260 | 7 | 2.5 MB |
| rust-webview-tauri (Tauri 2.11) | 705 | 731 | 804 | 606–625* | 7 | 5.6 MB |
| rust-webview-single (⚠ `--single-process`) | 462 | 462 | 487 | 92–103 | 3 | 2.5 MB |
| *baseline-win32 (empty GDI window, floor)* | ~90 | | | ~55 | 1 | |

Median `t_content` in ms, 5 runs after a warm-up run, `bench.py run … --runs 5`, file
`results/quick-3.json`. The machine was relatively quiet at the time, but other agents' builds were running in parallel,
so the accuracy is about ±30 ms. \*Tauri's window is already visible at ~75 ms (`observed_ms`), but
we honestly set `t_window` only after `build()`, see below.

**Conclusions:**

1. **WebView2 by itself costs ~450–500 ms, and there is no way around it.** Of the ~560 ms, the prototype's code
   accounts for ~60 ms: process start, window, controller request. Everything else is inside WebView2:
   the browser process spends ~150 ms on its own initialization alone before launching the GPU and network processes,
   the controller is ready at ~360 ms, then the renderer, DOM and first frame take another ~180 ms. Markdown parsing and
   highlighting run on a parallel thread and do not land on the critical path.
2. **Rust/wry adds nothing on top of WebView2**, as long as tao is not used naively. On the first
   `WM_SETFOCUS` tao builds a keyboard layout cache (~250–400 ms, measured separately). If the window
   is activated *during* controller creation (our "show in pump" technique), this delay
   hides behind the browser launch: tao ≈ raw (571 vs 563), but tao's `t_window` is ~250 ms.
3. **Tauri 2 adds ~+150–200 ms** to the best wry variant: the tao window is activated before the WebView,
   not in parallel with it, plus more init scripts and IPC via `invoke`.
4. **`--single-process` saves ~100 ms**, almost all of it on the first frame: no launching of GPU and
   renderer processes and no inter-process frame exchange. But this mode has no sandbox, is not officially
   supported, and is for measurements only.
5. **Lightning speed is not achievable on WebView2.** The WebView2 floor on this machine is ≈ 0.46 s (unsafe) or
   ≈ 0.55 s (safe), while a GDI window gets ≈ 0.09 s. If you need "flash-like" opening, you have to draw
   natively (DirectWrite/D2D) or keep a resident process with a warmed-up WebView2 (see "Ideas").

## Stack and versions

- Rust 1.98.1 (x86_64-pc-windows-msvc), release: `opt-level=3, lto="fat", codegen-units=1, panic="abort", strip=true`.
- **wry 0.57.0**, webview2-com 0.39.1, windows 0.62.2 / windows-sys 0.61.2, raw-window-handle 0.6.2.
- **tao 0.37.0**: only in the `rust-webview-tao` variant.
- **Tauri 2.11.5** (tauri-build 2.6.3, tauri-runtime-wry 2.11.4, with wry 0.55.1 and tao 0.35.3 inside, default features, no plugins).
- **pulldown-cmark 0.13.4**: GFM tables, strikethrough, task lists, footnotes, `ENABLE_GFM`.
- **syntect 5.3.0 + two-face 0.5.2** (syntaxes from bat 0.26), **Oniguruma** engine (onig 6.5.3, built automatically by the `cc` crate with MSVC).
- WebView2 Runtime 153.0.4234.32 (evergreen), Windows 11 26200.

## Build

```
pwsh -File build.ps1          # cargo build --release --workspace и раскладка по out/wry и out/tauri
pwsh -File build.ps1 -Clean   # полная пересборка с cargo clean (~5–8 мин: Tauri, onig, LTO)
```

Result: `out/wry/fastmd-wry.exe` (2.5 MB, a single file; WebView2Loader is linked statically) and
`out/tauri/fastmd-tauri.exe` (5.6 MB). `target/` (~2 GB) lives inside the prototype folder.
Internet access is needed only for the first build, for crates.io.

Structure:

```
Cargo.toml           workspace + release-профиль
mdcore/              общее ядро: протокол бенча (bench.rs), md→html + чанки + подсветка (render.rs),
                     состояние документа и custom protocol (serve.rs), шаблон страницы/JS (lib.rs)
  examples/hlbench.rs  замер подсветки по языкам
wry-app/             fastmd-wry.exe: main.rs, rawwin.rs (Win32-окно), showlater.rs (show-in-pump)
  examples/          taoprobe.rs, rawprobe.rs: пробы стоимости активации окна tao / Win32
tauri-app/           fastmd-tauri.exe: main.rs, build.rs, tauri.conf.json, icons/
results/             quick-3.* (итоговый быстрый прогон), ab-final-medium.* (A/B ручек);
                     quick-1/2 испорчены нехваткой памяти, см. «Грабли»
shots/               скриншоты всех вариантов (small/medium)
```

## How it works (critical path of the best variant)

```
t0 CreateProcess ─22ms─► main(≈38) ─► CreateCoreWebView2EnvironmentWithOptions (≈16 мс, loader)
                                   ─► поток рендера: read .md → pulldown-cmark → подсветка → первая страница (≈+15…35 мс)
                                   ─► скрытое Win32-окно (≈5 мс) → PostMessage(WM_SHOW_LATER)
                                   ─► wry::build(): create controller  ── браузер msedgewebview2 стартует (≈+8 мс)
                                        └ внутри wait_with_pump: WM_SHOW_LATER → ShowWindow+UpdateWindow → t_window (≈93)
                                   ─► контроллер готов (≈360) → Navigate("http://fastmd.localhost/")
                                   ─► WebResourceRequested → отдаём страницу из памяти (≈2 мс)
страница: CSS инлайн + первые ≥64 KB HTML + скрипт → DOM ready (≈+50) → rAF → rAF (≈+120)
         → chrome.webview.postMessage('painted …') → DwmFlush() → t_content (≈560)
после t_content: fetch('/__fastmd/rest') → остаток документа кусками по 512 KB (insertAdjacentHTML + setTimeout)
```

Process timeline (ms from the creation of our process, quiet machine, `medium`):

| event | ms |
|---|---|
| `main()` | 38–41 |
| env requested / window created, controller requested | 54–57 / 59–62 |
| msedgewebview2 (browser) created | 67–70 |
| crashpad-handler | 102–114 |
| **gpu-process + NetworkService** | **219–228** (≈150 ms of the browser's own initialization) |
| StorageService | 247–257 |
| renderer | 326–337 |
| controller ready (`webview_built`), navigation | 360–373 |
| DOM ready (`js_dom_ready`) | 410–430 |
| 1st rAF / 2nd rAF | 518–536 / 535–554 |
| `t_content` (after DwmFlush) | 547–565 |

## Variants (proto.json)

| id | arguments | what it is |
|---|---|---|
| `rust-webview` | `--id rust-webview` | **Best safe one.** wry 0.57 + minimal Win32 window, show-in-pump, env created first, custom protocol, first chunk 64 KB, highlighting on the render thread, `chrome.webview.postMessage`, persistent UDF `%LOCALAPPDATA%\FastMD\wry`, sandbox enabled. |
| `rust-webview-tao` | `--tao` | The same, but the window comes from tao 0.37, as in the wry examples. Also show-in-pump: the window is created invisible, `set_visible(true)` is called inside the pump via a subclass. |
| `rust-webview-tauri` | — | A minimal Tauri 2 app: `register_asynchronous_uri_scheme_protocol("fastmd")`, `WebviewWindowBuilder` in `setup`, IPC via `__TAURI_INTERNALS__.invoke('msg')`, the same page from mdcore. For identical screenshots a FluentOverlay scrollbar and the light theme are set. UDF `%LOCALAPPDATA%\FastMD\tauri`. |
| `rust-webview-single` | `--data wry-single --extra-args --single-process` | ⚠ **Unsafe, for measurements only.** Chromium `--single-process`: renderer and GPU inside the browser process, no sandbox. Separate UDF. |

`fastmd-wry.exe` flags for experiments: `--tao`, `--show-first` (show the window before the controller),
`--late-env` (wry creates the env itself), `--env-first` (diagnostics), `--nav html` (NavigateToString
instead of custom protocol), `--chunk N` (first chunk in bytes, 0 = the whole document), `--piece N`,
`--wry-ipc` (`window.ipc` from wry instead of `chrome.webview`), `--private` (InPrivate),
`--browser-args "…"` / `--extra-args "…"`, `--data <подпапка UDF>` (UDF subfolder), `--no-highlight`,
`--default-scrollbar`, `--tracking-prevention`, `--shared-udf`, `--eval <js>` (debugging and screenshots).
The variable `FASTMD_TRACE=<файл>` (file) logs events after `t_content` (for example `rest_done`).

## What is and isn't supported relative to the spec (PROTOCOL §5)

Supported:
- Headings h1–h6, paragraphs with wrapping, **bold**, *italic*, `inline code`, ~~strikethrough~~, links.
  External links open in the default browser (NavigationStarting → ShellExecute).
- Bullet, ordered, nested and task lists. The `contains-task-list` class that exists
  in style.css has no counterpart in pulldown-cmark, so we add it ourselves.
- Blockquotes, `hr`, GFM tables with alignment, footnotes.
- Fenced code: monospace Cascadia Mono, background, no wrapping, horizontal scroll. There is
  **syntax highlighting** (syntect + Oniguruma, GitHub-like colors, compact `<i class=k>`):
  rust, c, csharp, js/ts, json, python, powershell and about 200 more languages. An unknown language
  is output as plain text.
- Local images by relative paths: the custom protocol serves files next to the `.md`; escaping via `..` is forbidden.
- Unicode: Cyrillic, CJK, color emoji (Segoe UI Emoji) work via Chromium's font fallback.
- PMv2 DPI, 1000×800 DIP client area, the window is shown activated, title per the protocol,
  light theme, scrolling, Fluent overlay scrollbar, Esc closes the window, Ctrl+wheel zooms.
- Large files: the first chunk of at least 64 KB of HTML is cut at a top-level block boundary, the rest
  is loaded after the first frame. For `large.md` (3.7 MB) `t_content` is almost the same as for
  `small`. The full document (7 MB of HTML) finishes rendering ~3.1 s after start (`rest_done`),
  and the UI stays responsive the whole time.

Missing or limited:
- Heading anchors and a table of contents are not implemented: pulldown-cmark does not generate ids.
- No GFM autolinks for bare `www.`/`http://` (pulldown-cmark does not support them). No `> [!NOTE]` alerts.
- No search, dark theme, reload on file change, drag&drop.
- Raw HTML blocks from Markdown go into the page as is. A viewer needs CSP/sanitization, see "Ideas".
- `t_window` for tao and Tauri is conservative: it is set after `set_visible`/`build()` returns, even though
  the window is visible earlier (`observed_ms` ≈ 75–95 ms).

## Quick numbers (noisy)

`results/quick-3.json` (medians, ms from CreateProcess):

| variant / doc | content | window | main | controller ready | page served | DOM | rAF1 | rAF2 | ipc |
|---|---|---|---|---|---|---|---|---|---|
| rust-webview / small | 558 | 93 | 34 | 365 | 370 | 429 | 543 | 551 | 555 |
| rust-webview / medium | 563 | 94 | 41 | 367 | 371 | 419 | 536 | 553 | 554 |
| rust-webview / large | 598 | 105 | 37 | 391 | 394 | 447 | 576 | 593 | 594 |
| rust-webview-tao / medium | 571 | 250 | 40 | 391 | 392 | 437 | 545 | 563 | 565 |
| rust-webview-tauri / medium | 731 | 606 | 43 | 606 (window+webview) | 607 | 622 | 622 | 714 | 726 |
| rust-webview-single / medium | 462 | 101 | 36 | 374 | 385 | 396 | 439 | 452 | 453 |

Other figures from the same run: CPU of the whole job ≈1.6 s (7 processes) vs ≈1.05 s for single; peak commit
≈185 MB; working set of our process 26–34 MB; the first launch on an already existing profile is hardly
slower. On a **new** UDF the first launch creates the profile, which adds ~+0.3–1 s.

### Knob A/B (medium, 6 interleaved runs, quiet machine: `results/ab-final-medium.*`)

| configuration | median t_content | Δ vs best |
|---|---|---|
| **best** (raw + show-in-pump + custom protocol + highlighting) | **565** | — |
| `--show-first` (window shown before the controller is requested) | 614 | +49 |
| `--tao --show-first` (naive tao) | 737 | +173 |
| `--no-highlight` | 581 | ≈0 (highlighting is free) |
| `--nav html` (NavigateToString) | 600 | +36 (−20…−40 in other series: within noise) |
| `--extra-args --no-sandbox` | 573 | ≈0 |
| `--extra-args --single-process` | ≈462 (quick-3) | −100 |

Earlier series ran under heavy parallel load, with absolute values of 650–800 ms there, but
the sign of the effect is reliable:
- `--disable-gpu --disable-gpu-compositing`: worse (+135). `--use-angle=warp`: ≈0. `--in-process-gpu`: ≈0…+40.
  `--disable-gpu-sandbox`: ≈0…+50.
- `--enable-features=NetworkServiceInProcess2`: one process fewer, ≈0 in time.
  `--disable-crash-reporter --no-crashpad`: ≈0.
- A set of "background" flags (`--disable-background-networking --disable-component-update
  --disable-sync --no-first-run --disable-domain-reliability …`, `--disable-features=Translate,
  OptimizationHints,MediaRouter,AutofillServerCommunication,CalculateNativeWinOcclusion,
  RendererCodeIntegrity,SpareRendererForSitePerProcess`): ≈0.
- `--private` (InPrivate): ≈−50…0, noise. `--wry-ipc`: ≈0. `--late-env`: ≈0 (see pitfalls).
- First chunk 16 KB / 64 KB / the whole document: no difference for medium. For large the whole document
  would mean 7 MB of HTML before the first frame.
- DOM → first frame ≈130–150 ms regardless of content (ASCII, no CJK/emoji, small):
  this is compositor and GPU initialization, not layout and not fonts.

## Where the time goes

1. **The WebView2 browser process, ~300 ms from the request to a ready controller.** For about 150 ms it
   is busy with itself (loading msedge.dll, prefs, variations, profile) and only then launches the GPU,
   network and storage processes. The renderer appears only on navigation. No safe flag
   sped this up noticeably.
2. **The first frame, ~180 ms from responding to the page request.** About 50 ms until DOM ready (the renderer
   starts and parses the HTML), about 120 ms until the first rAF (compositor and GPU channel). In
   single-process ~70 ms of this remain.
3. **Our code, ~60 ms until the controller request**: CreateProcess ~22 ms, Defender included; until
   `main` ~15 ms; WebView2 loader ~16 ms; hidden window ~5 ms. Parsing and highlighting (medium ~20 ms,
   large ~50 ms for the first chunk and ~500 ms for the whole document) run in parallel and are never
   the bottleneck.
4. **IPC and DwmFlush, ~5–10 ms.**

## Ideas for further speedups

- **Resident mode (`-resident`)** — the only path to ~100–200 ms on WebView2. Keep the
  environment and a warmed-up hidden controller (the renderer already alive), and open a new file as
  Navigate and show. The technique is language-independent, cf. `cpp-webview2-resident` in the neighboring prototype.
  In Rust this is ~1–2 days: a named pipe or `WM_COPYDATA`, a tray icon, unloading on a timer.
- **Hybrid.** Draw the first screen natively right away (DirectWrite from the same AST) while
  WebView2 starts, then swap. Per the protocol this is allowed only if the native frame is honestly
  rendered according to the spec. In effect it is a second renderer, i.e. expensive.
- `ICoreWebView2CompositionController` (visual hosting) instead of HWND hosting: may remove
  synchronization with the browser's child window. Not tested.
- Warming the page cache and prefetching msedge.dll at logon: this is a system setting, not for the product.
- Using webview2-com directly instead of wry would gain almost nothing: wry does not hold up the critical path; with
  `with_environment` and show-in-pump everything needed is available.

## Effort estimate for a full product

A viewer MVP on this stack: 1.5–2 weeks for one developer. That includes the `.md` association and
single-instance, drag&drop, reload on file change (`notify`), search (WebView2 Find API
or JS), dark theme (`prefers-color-scheme` already works via CSS), anchors and table of contents, printing
and PDF (`PrintToPdf`), CSP and sanitization of raw HTML, settings, an installer (MSIX or Inno). Another
week for Mermaid and KaTeX (lazy JS bundles from the custom protocol) and polishing. Resident mode:
+1–2 days. Pure Rust ~1.5–2 KLOC. The WebView2 Runtime is already present in Windows 11.

## Pitfalls (what we stepped on)

- **tao 0.37: the first window activation costs 250–450 ms.** On `WM_SETFOCUS` it calls
  `gain_active_focus` → `LAYOUT_CACHE.get_current_layout()` → `prepare_layout`, which is thousands of
  `ToUnicodeEx` calls over all keys and modifiers. Probe `examples/taoprobe.rs`: focused 287–454 ms
  vs unfocused 80–87. A bare Win32 window (`examples/rawprobe.rs`) activates in 30–45 ms.
  The same applies to winit (tao is its fork) and Tauri.
- **`CreateCoreWebView2EnvironmentWithOptions` completes instantly** (`--env-first`: the callback
  arrives in the same millisecond), and the browser process is launched only when the
  **controller** is created. So an "early env" by itself gains nothing. The only gains come from an early
  HWND and showing the window **inside** the controller-creation pump (`showlater.rs`): −50 ms.
- `ShowWindow` may block during browser startup: the WebView2 child window lives in another
  process, and the input queues are attached. So a `t_window` set after it returns is conservative.
- `with_ipc_handler` in wry adds `AddScriptToExecuteOnDocumentCreated` with a wait for a
  round-trip before navigation. We attach `add_WebMessageReceived` ourselves and send via
  `chrome.webview.postMessage`: no init script, and the message is not lost because the
  handler is registered before the message loop starts.
- By default wry and WebView2 put the UDF next to the exe (`<exe>.WebView2`), i.e. they litter `dist`.
  We explicitly set `%LOCALAPPDATA%\FastMD\<вариант>` (variant). Different browser args require different UDFs if
  the instances live at the same time.
- NavigateToString: ~2 MB limit, origin `about:blank`. Relative images need
  `<base href="http://fastmd.localhost/">`, and with `<base>` `#anchors` navigate away from the page. So
  by default the page is served via the custom protocol from the same origin.
- Tauri 2: `WebviewUrl` is imported from `tauri::`, not from `tauri::webview::`. tauri-build requires
  `icons/icon.ico`. By default the scrollbar is classic and the theme is dark, so for identical
  screenshots you need to set FluentOverlay and Light. A custom scheme on Windows looks like
  `http://<scheme>.localhost/` and counts as "local" for IPC, so app commands work without capabilities.
- pulldown-cmark makes a first pass over the **entire** document (block structure) before the first
  event. For large this is ~14 ms. Chunking saves HTML generation and layout, but not this pass.
- syntect: with `regex-fancy` the first highlighting of each language costs 14–57 ms (regex compilation, ~160 ms
  for the first chunk of medium), and PowerShell in two-face is unavailable with fancy. With **Oniguruma** it is 1.6–4 ms per
  language. The stock `ClassedHTMLGenerator` nests a span for every scope, and the HTML doubles in size
  (the rest of large is 13.4 MB instead of 6.6). Our own compact output gives 7.0 MB.
- **Environment noise.** Someone else's process from the shared scratchpad (`qtw\sampler.exe`) twice ate 44–46 GB
  of RAM, leaving <2 GB of free memory. msedgewebview2 startup then consistently stalled for
  +2.1 s (the browser was created on time, the GPU process only after ~2.2 s), Tauri for +4 s, and one
  empty run produced `main` after 9.8 s. The runs `results/quick-1-INVALID-memory-pressure.*` and
  `quick-2-NOISY.*` are spoiled. The valid `quick-3` was taken after the memory was freed.

## Audit (independent review, 2026-09-19)

Reviewed by a separate auditor agent. Other agents' builds were running in parallel, so all numbers below
are noisy and suitable only for relative comparison.

### What was checked

| item | result |
|---|---|
| `t_content` per PROTOCOL §3 | ✅ `page_script()` (mdcore/src/lib.rs) sits at the end of `<body>`, after the `<article>` with the first chunk. Then rAF → rAF → `chrome.webview.postMessage` (`invoke` in Tauri) → `handle_message` → `bench::content_presented()`: `DwmFlush()` is called on the UI thread of the process that owns the window, and `t_content` is taken **right after** it. Nothing is marked earlier. |
| Resident processes, cache, pre-rendering | ✅ No resident processes: after the runs there is not a single `msedgewebview2` with UDF `FastMD\wry\|tauri`. The HTML is built anew on every launch on the render thread. Custom protocol responses carry `Cache-Control: no-store`. The persistent UDF is a regular WebView2 profile, the same as a real app would have. |
| Corpus data in the binary | ✅ None. `include_str!` pulls in only `bench/common/style.css`. The syntect/two-face syntaxes are standard dumps unrelated to the corpus. |
| Release build | ✅ Profile `opt-level=3, lto="fat", codegen-units=1, panic="abort", strip`. `cargo build --release --workspace` was up to date: `out/wry/fastmd-wry.exe` has the same sha1 as `target/release`. The Tauri exe is rebuilt on every `cargo build`; its sha1 differs, but the size is the same: `generate_context!` generates a new random invoke key each time. |
| `check` medium/large, all 4 variants | ✅ All `CHECK OK`, no warnings, `clean_exit=true`, `exit_code=0`. `observed_ms` (66–86 ms, Tauri 74–122) is always **earlier** than `t_window` and `t_content`; no signs of a mark set too early. |
| When the content actually appears on screen | ✅ For this check `audit/pixwatch.py` was written: a loop BitBlt-ing the top strip of the client area from the screen DC (what DWM actually composited), ~12 ms per capture frame. Runs of wry, tao, single, Tauri (17 in total): the frame with text appears **within one capture interval of `t_content`**. In 15 of 17 runs the last empty frame started before `t_content`. In 2 runs the empty frame started +2.7 ms (wry) and +15.6 ms (tao) after `t_content`, i.e. the mark can be ahead of the screen by 1–3 frames at 165 Hz. This is how double-rAF works for WebView2; it is the same for all web prototypes, and there is no systematic "early" bias. For Tauri the mark is even slightly late: the text is on screen ~10 ms before `t_content`, because `invoke` is slower. |
| First chunk and lazy loading of the rest | ✅ `audit/domcheck.py` counts the elements of the final DOM after the rest is loaded and compares them with the source. small, medium and large match **completely** (large: h2=1659, h3=3318, pre=1659, table=1659, img=34, checkbox=6636). There is no stray text at the top level, no broken images, all checkboxes are inside `ul.contains-task-list`. The first frame of `large` is a real styled document (`shots/audit-rust-webview-large.png`). |
| Screenshots | ✅ `shots/audit-*.png`. Below the window title, tao and Tauri match `rust-webview` pixel for pixel; single differs in 71 pixels (antialiasing). |

### What was fixed

1. **mdcore/src/render.rs, `TaskListClass`: a latent bug in the chunker's depth balance.** `Chunker`
   recognizes the `<ul class="contains-task-list">` replacement by pointer (`std::ptr::eq(*h, TASK_UL)`), while
   `TaskListClass` returned a separate string literal with the same text. This worked only because
   LLVM/rustc merged identical constants. Without the merge (debug build, different codegen) the depth
   would go to −1 after the first task list, and the document would be cut in the middle of a block: dangling `</h2>`,
   text outside elements. Now `TaskListClass` returns `TASK_UL` itself. After `pwsh -File build.ps1`
   all checks were rerun: `check` OK, `domcheck` for medium and large matches everything. This does not
   affect speed; the code path is the same.

Nothing else was changed, neither in the marking logic nor in `proto.json`.

### Not fixed, but important for the final comparison

- **`t_window` of `rust-webview-tauri` is an upper bound** (~510–960 ms), set after `build()`.
  The window is visible (`observed_ms`) already at ~75–120 ms, and the white strip appears on screen at ~190–210 ms,
  just like wry's (DWM open animation). The exact moment of Tauri's first background paint cannot be caught without
  intervening in tao, so I did not touch the mark. For the window, it is better to compare `observed_ms`
  across all variants. This does not affect `t_content`.
- **`t_window` of `rust-webview-tao` (~200–240 ms)** is most likely **honest**: the window is visible at ~80 ms,
  but the UI thread is blocked in tao's `WM_SETFOCUS` (layout cache), and the background is painted only after that.
- **`rust-webview-single` is not a product candidate.** Chromium `--single-process` without a sandbox,
  not supported by Microsoft. The variant complies with the protocol, but in the final table it should
  be shown only as the "WebView2 floor" with an explicit note, not on par with the safe stacks.
- `--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection` are wry's standard default arguments,
  i.e. the behavior of a regular wry app, not a special trick.

### Visual assessment (PROTOCOL §5)

Fonts, sizes, colors, the 860 DIP column with 32 padding fully match the spec, because
the shared `style.css` is used. Rendered correctly: h1–h6 (h1/h2 have a border, h6 is gray),
tables with alignment and zebra stripes, GitHub-style code highlighting (rust, csharp, python, js, json,
powershell, c), blockquotes, `hr`, nested lists, task checkboxes (disabled, as on GitHub),
Cyrillic, CJK, color emoji and local PNGs. Minor flaws: a nested `ol` gets
`margin-bottom:16px` from the shared `style.css`, which leaves an extra gap after it (the same for all
web prototypes). No autolinks, `[!NOTE]` alerts or heading anchors. **Visual quality 9/10,
GitHub fidelity 9/10.**

### Auditor's control run (noisy)

`bench.py run rust-webview,rust-webview-tao,rust-webview-tauri,rust-webview-single --doc small,medium --runs 5`,
file `results/audit-run.json` (median of 5 runs after a warm-up run, ms):

| variant | small | medium | win (med) | obs (med) | CPU job | processes |
|---|---|---|---|---|---|---|
| rust-webview | 567.5 | 553.0 | 91 | 72–75 | ~1.5 s | 7 |
| rust-webview-tao | 580.7 | 549.3 | 202–239 | 87–88 | ~1.6 s | 7 |
| rust-webview-tauri | 673.8 | 747.4* | 580–605 | 80–81 | ~1.6 s | 7 |
| rust-webview-single ⚠ | 452.8 | 451.4 | 88–90 | 72 | ~1.0 s | 3 |

\*One Tauri/medium outlier at 3.5 s due to machine noise (p90 2438). The numbers agree with the author's `quick-3`
within ±30 ms.

This run was made **before** the rebuild with the `TASK_UL` fix. After the rebuild, `check medium` gave
564, 575, 851 (noise) and 457 ms for wry, tao, Tauri and single, all OK.
The audit scripts live in `audit/`: `pixwatch.py <variant> <doc> [runs]` and `domcheck.py <doc>`. They
only import `bench/harness/bench.py` and do not change anything in it.
