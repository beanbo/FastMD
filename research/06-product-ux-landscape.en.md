[Русский](06-product-ux-landscape.md) | **English**

# 06. Product and UX: what a fast, stylish Markdown reader must do, and how competitors handle it

- Date: 2026-09-19. Machine: Windows 11 Pro 26200, Ryzen 9 7950X, 64 GB, NVMe, Defender real-time ON, 2 GPUs.
  **The user's system theme is dark** (`AppsUseLightTheme=0`, transparency on, window animations on), checked in the registry.
- All of my own measurements are marked **[measured]**. Everything else is taken from sources; links are given in the text and in §12.
- Lab: `C:/Main/Projects/FastMD/research/lab/product-ux/`. How to reproduce: Appendix A.
- **About noise.** Everything was measured while ~11 agents were building prototypes in parallel. Absolute numbers are a guide
  (spread ±20–40 %). Orders of magnitude and comparisons within one series are reliable. For competitors I ran
  1 warm-up + 3 measurements. For Mica and animations, 10–12 interleaved measurements.

---

## 0. In short (TL;DR)

1. **All popular readers/editors open in 1–6 s** [measured, medium.md 53 KB, "visually complete"]:
   Typora 1.14.10 ≈ **1.1 s** to 50 % of the frame and 1.7 s to fully complete; MarkText 0.20-rc.4 ≈ **3.8 s**;
   Markdown Monster 4.5.3 ≈ **4.5 s** to 50 % of the frame; the PowerToys preview in the Explorer pane ≈ **0.6 / 1.4 s**.
   These are WebView2/Electron. The native newcomers are no faster: Inlyne (Rust + wgpu) ≈ **1.2 s**, Ferrite (Rust + egui) ≈ **1.3 s**,
   almost all of it spent on GPU initialization. The calibration Win32 "floor" on this machine: 0.09–0.12 s.
   **The "opens in ≤ 250 ms and looks good" niche on Windows is currently empty.**
2. **Large files are everyone's weak spot.** On large.md (3.7 MB) Typora shows "File too large to render
   in Typora". Ferrite didn't show a window within 30 s (29 s of CPU). Inlyne managed in 3.6 s with 940 MB of commit [measured].
   The first screen has to be rendered first, the rest later.
3. **Mermaid and formulas are rarely needed** [measured, 3,405 unique .md files from 4 corpora]: 0.3–1.8 % of documents.
   Math, 0–0.6 %. So a web engine is needed for ~2 % of opens. The other 98 % can be rendered natively,
   without paying for Chromium.
4. **But a "native" renderer has to do more than it seems** [measured, top-500 READMEs on GitHub]:
   - 76 % contain raw HTML (`<img>` 70 %, `<p align>` 45 %, `<br>` 40 %, `<picture>`+`<source media=prefers-color-scheme>` 20 %, `<details>` 19 %);
   - 85 % contain remote images, 69 % SVG (shields.io badges with `<text>`), and in 80 % of READMEs
     a remote image sits **within the first 30 lines**, i.e. on the first screen;
   - GitHub Alerts `> [!NOTE]` are in 18 % of READMEs and in 39–46 % of documentation (VS Code docs, Obsidian Help);
   - relative links to .md files, 60–86 %; `#…` anchors, 35 %.
   **Direct2D SVG doesn't draw `<text>`** (per Microsoft's documentation), so badges will require lunasvg or resvg.
5. **A hybrid is the right architecture.** A native first frame; a web engine lazily and only if there's `mermaid` or `$$`.
   The cost of the "rich mode" [measured, Chromium 153 = WebView2 153]:
   Mermaid 12 — 196 ms to execute 5.6 MB of JS and ~250 ms for the first diagram;
   KaTeX 0.18.7 — 11 ms + 19 ms for 50 formulas + ~100 ms for layout and fonts;
   Temml (MathML) — lighter still;
   **MathJax 4 — ~1 s for 50 formulas, don't use it**.
   Plus ~285 ms to create the WebView2 controller (report 02).
   A browser-free alternative: **mmdr** (Rust, native Mermaid) — 1.7 ms for a sequence diagram, 110 ms for a 12-node flowchart
   [measured], 23 diagram types, quality slightly below the reference.
6. **The DWM window-open animation eats ~200 ms of perceived speed** [measured, 60 Hz screen capture].
   With the animation the window is fully visible after 291 ms, without it after 85 ms.
   `DWMWA_TRANSITIONS_FORCEDISABLED` removes this difference. `t_content` in the harness **doesn't see** it: it happens after `DwmFlush` (§11).
7. **Mica and a dark title bar are free** [measured]: plain 113 / dark 108 / mica 108 / mica+dark 105 ms (medians, n=12).
   The difference is within noise. A stylish Windows 11 look doesn't hurt speed.
8. **The fonts for the style are already in the OS** [checked on the machine]: Segoe UI Variable (wght axis 300–700 and opsz 5–36),
   **Cascadia Code/Mono (part of the system)**, Segoe UI Emoji (color), Segoe Fluent Icons, Sitka VF (a serif option),
   Cambria Math (for MathML), CJK: Yu Gothic UI, Microsoft YaHei UI, Malgun Gothic. No need to ship fonts.
9. **The line length in the current style spec is too long** [measured, Segoe UI Variable metrics]:
   a column of 860 − 2×32 = 796 DIP at 15 px gives ~**119** Latin or ~**102** Cyrillic characters per line.
   The norm is 45–90. Product recommendation: body 16 DIP, text up to 680 DIP (like Bear, ~86 Cyrillic / 97 Latin),
   tables, code and images may extend wider.
10. **Must-have for v1** (§5): GFM, code highlighting, light/dark/auto theme, zoom, Ctrl+F, selection and copying,
    a "copy code" button, a table of contents, navigation by anchors and relative .md links (with "Back"),
    local and remote images with SVG, an HTML subset, GitHub Alerts, hiding front matter, emoji shortcodes,
    live reload, "Open in editor", file association, drag-and-drop, recent files via the Jump List.
    **Later:** Mermaid and formulas (lazy engine), printing and PDF, an Explorer preview pane handler
    (PowerToys takes 0.6–1.4 s there — a good chance to stand out), tabs.

---

## 1. Methodology and artifacts

| What | File(s) in `research/lab/product-ux/` | How |
|---|---|---|
| Competitor launch time | `measure_apps.py`, `results/apps-*.json`, `shots/*.png` | Launched through the `bench/harness/bench.py` primitives (Job Object, `t0` right before `CreateProcessW`, import only, no changes). Then a **visual timeline**: `PrintWindow(PW_RENDERFULLCONTENT)` of the window every ~20–30 ms. `window` is the first visible window. `p50` is 50 % of visual progress toward the final frame. `VC` is "visually complete": after this frame, all frames differ from the final one by < 0.3 % of pixels (a blinking cursor is allowed). Calibration on `baseline-win32`: VC = 94–125 ms, i.e. it matches its `t_content`. |
| Feature frequencies in real .md files | `feature_survey.py`, `html_tags.py`, `extra_stats.py`, `survey_*.json`, `html_tags.json` | 4 corpora, deduplicated by SHA-1, heuristics outside fenced code. Local: `C:/Main/Projects` without FastMD + `~/.cargo/registry/src`, 2,373 files. `corpus_github`: the READMEs of the 500 most-starred repositories, downloaded via `gh api`. `corpus_obsidian_help`: 176 files, PKM style. `corpus_vscode_docs`: 356 files, documentation. |
| Cost of Mermaid/KaTeX/Temml/MathJax | `webfeat/gen_pages.js`, `webfeat/run_pages.js`, `results/webfeat.json` | Headless Edge 153.0.4234.32 (the same Chromium as WebView2 Runtime 153) via puppeteer-core. 5 runs, a shared profile (V8 code cache, like the WebView2 UDF). Times are taken from `performance.now()` inside the page. |
| Native Mermaid | `apps/mmdr/`, `mmd/*.mmd`, `results/mmdr.json`, `shots/mmdr-*.png` | mmdr 0.3.1 (Rust). The `--timing` flag plus total process time. |
| Mica, dark mode, open animation | `mica/mica_test.c`, `mica/run_mica.py`, `mica/run_transition.py`, `mica/results/*.json` | C/Win32 modeled on the baseline and on PROTOCOL.md. Timing via `bench.run_once` (results in my folder). The animation was captured from the screen: desktop BitBlt ≈ 60 Hz. |
| Explorer .md preview handler | `prevhost/prevhost_test.cpp` | Hosts the registered `IPreviewHandler` for .md the way the preview pane does: `IInitializeWithFile/Stream` → `SetWindow` → `DoPreview`. On this machine that is PowerToys 0.100.2. Loaded in-proc so everything stays in the Job. |
| Glow (CLI) | `time_glow.py`, `results/glow.json` | Full render to ANSI, `stdin=DEVNULL` (§11). |

Versions of the apps downloaded for measurement (portable, in `apps/`, nothing was installed):
Typora 1.14.10 (unpacked with innoextract 1.9, Electron 42.2.0 / Chromium 148),
MarkText 0.20.0-rc.4 (zip, Electron 42.1.0),
Markdown Monster 4.5.3 portable (needs .NET 10; runtime 10.0.12 unpacked locally and wired in via `DOTNET_ROOT`),
Inlyne 0.5.3, Ferrite 0.3.0 portable, Glow 3.0.0, mmdr 0.3.1.
Traces in `%APPDATA%`/`%LOCALAPPDATA%` (inlyne, marktext, Typora, glow, Markdown Monster)
and the keys `HKCU\Software\Typora` and `HKCU\Software\West Wind Technologies\Markdown Monster` were **deleted** after the measurements.
Obsidian (installed on the user's machine, 1.11.7) was **not launched**: it would have opened the personal vault and written its state.
Notepad, VS Code, Cursor and Void are measured by the `baselines-external` agent.

---

## 2. The competitive landscape

### 2.1 Summary table

"Startup" is either my measurement or complaints from sources. Competitor measurements: medium.md, warm launch, median of 3 runs under noise.

| Product | Stack | Startup | Look | Reading features | Takeaway for FastMD |
|---|---|---|---|---|---|
| **Typora 1.14.10** | Electron 42 (Chromium 148); native WebKit on macOS | [measured] window 0.79 s, p50 1.10 s, VC 1.68 s; first launch 2.5 s; commit 565 MB, 5 processes, 387 MB on disk. On large.md it **refuses** ("too large"). Complaints of 7–8 s (report 02) | The reference for "clean" WYSIWYG, CSS themes, different typefaces for headings and text | Outline in the sidebar, `[toc]`, Mermaid 11.x, MathJax 4, export to PDF/DOCX/…, focus/typewriter modes | Sets the quality bar. Speed and large files are its weak spot |
| **Obsidian 1.11.x** (reading view) | Electron | Forum: 4–5…20+ s (01.2026), ~14 s (04.2025), 46 s (02.2025, many plugins) | Pretty, customizable, callouts | Outline, backlinks, Mermaid, MathJax, callouts, properties (front matter), embeds, wikilinks | **Doesn't open files outside a vault** (a long-standing feature request). Not suitable for "double-click a .md" |
| **MarkText 0.20.0-rc.4** | Electron 42 (rewritten on electron-vite; the project came back to life in 2026) | [measured] window 1.45 s, **p50 = VC 3.8 s**; first launch 6.5 s; commit 441 MB. Complaint (#3862, 2024): 12 s cold / 3 s warm | Neat, follows the system dark theme | Muya WYSIWYG, Mermaid, KaTeX, TOC | Same class as Typora, but slower |
| **Zettlr 4.8.0** (2026-09-18) | Electron | Complaint #3391 (2022, v2.2.5): "40 seconds to start" | Academic | Citations, Pandoc, Zettelkasten | Not a reader |
| **iA Writer for Windows 2.x** | Stack not publicly disclosed | "Split the code into a small, fast-loading core and secondary components that load in the background" (iA blog, 12.2024–02.2025) | The typography reference (iA Quattro/Duo, a "dynamic" type engine, design tailored to Windows 11) | Full-width and fullscreen preview, focus modes | **Exactly our approach:** an instant core + everything else lazily |
| **Marked 3** (macOS, released 06.2026) | Native Cocoa + WebKit | — | Many styles, "Style Stealer" | Preview of a file from **any** editor, live reload, TOC, export to PDF/DOCX/EPUB/…, export profiles, tabs | The conceptual reference: "a viewer next to the editor" |
| **Bear 2 / Craft** (Apple) | Native | — | Bear Sans (a reworked Clarika), line length **680 px ≈ 75 characters**, line spacing and margins adjustable in em | Note-taking editors | A reference for typography and white space |
| **Markdown Monster 4.5.3** | WPF on .NET 10 + **2× WebView2** (editor and preview) | [measured] window 0.52 s (shell), **p50 4.5 s**, VC 5.6 s (+ spell-check underlines); commit 568 MB, 8 processes, CPU 12 s | Overloaded: panels, toolbars | Outline, preview, export, publishing to blogs | An anti-example for a reader: shows an empty shell right away, the content arrives seconds later |
| **VS Code 1.121 preview** | Electron, markdown-it | Measured by the `baselines-external` agent. The preview opens with a separate command (Ctrl+Shift+V) | GitHub-like | KaTeX built in since 1.58 (2021); Mermaid via an extension; outline; scroll sync | The de facto "reader" for developers, but heavy |
| **Notepad (Win 11) 11.2607** | A Store app on XAML/WinUI (formatting engine presumably RichEdit, no public details) | Measured by the `baselines-external` agent. The 06–07.2026 release notes say "Improved launch performance" twice | System look | Light formatting: headings, bold, italic, links, lists (11.2504, 05.2025), **tables** (11.2510, 11.2025), strikethrough, nested lists. No highlighted code, images, Mermaid | The fastest "built-in" option. FastMD has to win on quality and stay comparable on speed |
| **PowerToys Peek / Markdown Preview Handler 0.100.2** | WinUI 3 (Peek) and .NET + WebView2, Markdig | [measured] preview pane: host window 0.07 s, **p50 0.62 s, VC 1.4 s**; first display 5.8 s; 8 processes, commit 229 MB. Issue #46087 (03.2026): "preview is slow, ~400 ms even for .txt" | GitHub-like, system dark theme. **Blocks images by default** (a yellow banner) | Peek: Ctrl+Space. #45267 (02.2026) asks for a separate "Markdown Reader" with TOC, Mermaid, search, zoom, always-on-top | Direct confirmation of demand. A native FastMD preview handler would be 5–10× faster |
| **QuickLook 4.5** + MarkdownViewer | WPF (.NET Framework 4.6.2) + WebView2 1.0.4078; markdown-it (+anchor), highlight.js, Mermaid, MathJax, github-markdown.css | — (a resident process, Space in Explorer) | GitHub light/dark via `prefers-color-scheme` | A resizable TOC sidebar (the width is stored in localStorage) | This is what a "full" quick-look feature set looks like |
| **Markdown Viewer** (browser extension, simov) | Chrome/Edge/Firefox; markdown-it/marked/remark | Instant if the browser is already open | 30+ themes, width auto/full/wide/…/tiny | TOC, Mermaid (zoom, pan), MathJax, emoji, autoreload every 1 s for file://, raw/rendered | A good feature checklist. Downsides: you have to grant file:// access, no double-click |
| **GitHub** (the visual standard) | cmark-gfm + github-markdown-css v5.9.0 (02.2026) | — | 16 px, line-height 1.5, Primer light and dark palettes | Alerts (5 types, GA 12.2023), Mermaid (02.2022), MathJax (05.2022), footnotes, emoji, heading anchors and outline, `<picture>` / `#gh-dark-mode-only` | **This is what users expect.** Documents are written "for GitHub" |
| **Glow 3.0.0** / **frogmouth 0.9.1** | Go + Glamour / Python + Textual (TUI) | [measured] Glow: small 0.16 s, medium 0.27 s, **large 9.5 s** (renders the whole document, nothing incremental) | Terminal | Glow: pager, TUI file browser. frogmouth: TOC, bookmarks, history, `gh owner/repo` | The "browser" model (history, bookmarks) is useful. The TUI niche itself isn't ours |
| **Inlyne 0.5.3** | Rust + wgpu; comrak → HTML → its own layout ("just enough HTML to render 95 % of files") | [measured] window 0.18 s (empty), **content 1.23 s**; large 3.6 s / 940 MB | Dark theme by default, huge list indents, `\\?\C:\…` in the title bar | Live reload, themes, zoom | The "HTML subset" idea is right. wgpu doesn't give a fast startup on Windows |
| **Ferrite 0.3.0** | Rust + egui/eframe, comrak, syntect; **native Mermaid** | [measured] **1.34 s** to a window with content; large: no window within 30 s | Cramped, emoji monochrome and with artifacts (egui has no color fonts), "raw" mode by default | Outline, WYSIWYG, export to PDF/HTML, terminal… | egui doesn't do typography beautifully (cf. the rust-gpu report) |
| **MDHero** (2026) | Tauri + Rust (WebView2), < 8 MB | "Opens any .md in two seconds" (website) | Follows the Windows theme | KaTeX, Mermaid, TOC, live reload, file association, Marp slides | A competitor with the right feature set, but on WebView2, i.e. ~0.6–1 s at minimum (report 02) |

### 2.2 What the screenshots show [measured]

Files `shots/*.png`. The main observations on "stylishness":

- **Typora**: even GitHub-like layout, but headings and text in different typefaces. The menu takes a whole row,
  and the status bar has an "UNREGISTERED" badge.
- **MarkText**: minimal chrome, a narrow centered column (~640 px), dark theme from the system. Looks the best "out of the box".
- **Markdown Monster**: 3 panels, 2 toolbars. Visual noise for reading.
- **PowerToys Preview**: neat, but a yellow "Some images have been blocked…" banner at the top.
- **Inlyne**: good text typography, but gigantic list indents and a `\\?\…` path in the window title.
- **Ferrite**: no vertical rhythm (a heading sticks to the paragraph), text runs from the window edge, emoji are tofu-like outlines.

Conclusion: **good taste comes from GitHub-like layout + a narrow column + minimal chrome.**
None of these apps is faster than ~1 s.

---

## 3. My own competitor measurements [measured]

medium.md (53 KB, 1.4 thousand lines). Each series is 1 warm-up + 3 measurements, median. The machine was loaded with parallel builds.
"First launch" is the very first start after unpacking: Defender scans the new binaries.

| App | Stack | window, ms | p50, ms | **VC, ms** | First launch VC, ms | Job CPU, ms | Peak commit, MB | Processes | On disk |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline-win32 (floor) | C, GDI | 119 | 119 | **119** (min 94) | 175 | — | 3 | 1 | 0.16 MB |
| PowerToys Markdown Preview (pane host) | .NET + WebView2 | 71 | 618 | **1,391** | 5,827 | 4,094 | 229 | 8 | — |
| Typora 1.14.10 | Electron 42 | 785 | 1,099 | **1,682** | 2,538 | 3,594 | 565 | 5 | 387 MB |
| Inlyne 0.5.3 | Rust + wgpu | 181 | 1,230 | **1,230** | 2,127 | 1,391 | 277 | 1 | 20 MB |
| Ferrite 0.3.0 (rendered view) | Rust + egui | 1,342 | 1,342 | **1,342** | 1,894 | 1,734 | 259 | 1 | 29 MB |
| MarkText 0.20.0-rc.4 | Electron 42 | 1,451 | 3,827 | **3,827** | 6,528 | 6,766 | 441 | 5 | 460 MB |
| Markdown Monster 4.5.3 | WPF .NET 10 + 2×WebView2 | 525 | 4,460 | **5,644**¹ | 7,342 | 12,328 | 568 | 8 | 80 MB + .NET 10 |

¹ For MM, "visually complete" includes late spell-check underlines in the editor. For the preview, p50 is the fairer number.

**large.md (3.7 MB, 97 thousand lines)**, 1 warm-up + 1 measurement:

| App | Result |
|---|---|
| Typora | Window after 0.7–2.7 s, then the screen "**File too large to render in Typora**" with an "Open in…" button (`shots/typora-large.png`) |
| Inlyne | Window 0.15 s, content **3.6 s**, peak commit **940 MB** |
| Ferrite | **No window within 30 s**, 29 s of CPU on one core (both runs) |
| Glow (CLI) | Full render to ANSI **9.5 s** (small 0.16 s, medium 0.27 s) |

Conclusions:

1. **The gap between "the window appeared" and "the content is ready"** is the main UX defect of WebView2 apps.
   MM 0.5 → 4.5 s, PowerToys 0.07 → 0.6–1.4 s, MarkText 1.45 → 3.8 s. The harness in `external-title` mode
   records only the window appearing, so competitors need a visual timeline (§11).
2. **Native doesn't mean fast.** Inlyne and Ferrite spend ~1 s on GPU initialization (wgpu/glow) and laying out
   the whole document. The same shows up in the rust-gpu agent's preliminary runs in `bench/results`: egui ≈ 1.07 s.
3. **The first launch after an install or update** costs 1.5–2× more: Defender scans
   100–200 MB of Electron binaries. For FastMD at < 5 MB this penalty will be close to zero,
   and that matters for the first impression.
4. **Everyone takes a lot of memory**: 230–570 MB of commit for a 53 KB document. A reasonable budget for a reader is ≤ 50–80 MB.

---

## 4. What's actually inside real .md files [measured]

### 4.1 Corpora and sizes

| Corpus | Documents | p50 | p90 | p99 | max | > 100 KB | ≥ 5 headings | ≥ 15 headings |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Local (the user's projects + crate READMEs) | 2,373 | 3.7 KB | 19 KB | 116 KB | 523 KB | 1.4 % | 50 % | 15 % |
| Top-500 GitHub READMEs | 500 | 11.9 KB | 78 KB | 376 KB | 512 KB² | **9 %** | **91 %** | 47 % |
| Obsidian Help (PKM) | 176 | 2.9 KB | 8 KB | 17 KB | 33 KB | 0 % | 50 % | 11 % |
| VS Code docs (documentation) | 356 | 10.2 KB | 27 KB | 54 KB | 66 KB | 0 % | 88 % | 37 % |

² The GitHub API returns READMEs of no more than 512 KB, so the largest "awesome lists" are truncated.

Conclusions: a typical document is **4–12 KB**, and the first screen is 2–5 % of the file.
But 9 % of popular READMEs are over 100 KB (awesome lists, changelogs), and they need "first screen first" rendering.
A table of contents is needed in half of the local documents and in almost all READMEs.

### 4.2 Feature frequencies (share of documents, %)

| Feature | Local | GitHub-500 | Obsidian Help | VS Code docs | What it means |
|---|---:|---:|---:|---:|---|
| Tables | 27.8 | 41.0 | 18.8 | 29.8 | Must-have, done well |
| Fenced code | 36.8 | 64.6 | 25.0 | 54.8 | Highlighting needed. Top languages: bash/sh, js/ts, rust, python, c/cpp, toml, json, yaml, powershell |
| Task lists | 1.1 | 3.2 | 0.6 | 0 | Cheap, part of GFM |
| **Mermaid** | **0.42** | **1.6** | **1.7** | **0.28** | Rare |
| Math (`$$`, `$…\cmd…$`, `\begin{…}`) | 0.1 | 0.4–0.6 | 0.6 | 0.3 | Very rare |
| **Documents requiring a web engine** (Mermaid/diagrams or formulas) | **0.42** | **1.8** | **1.7** | **0.28** | The hybrid pays off |
| Front matter | 1.4 | 0.4 | **100** | **99.4** | Always present in documentation and PKM. Don't show it as "broken" text |
| GitHub Alerts `> [!NOTE]` etc. | 0.6 | **18.0** | **46.0**³ | **38.8** | Must-have |
| Footnotes | 0.3 | 0.4 | 1.1 | 0 | Cheap (md4c master) |
| Any images | 19.7 | **90.6** | 3.4 | 76.7 | Must-have |
| — local | 1.2 | 43.0 | 1.1 | **76.7** | Relative paths |
| — remote (http) | 19.3 | **85.2** | 2.3 | 0.3 | Asynchronously, with a cache |
| — SVG | 17.2 | **68.6** | 0 | 0.3 | SVG **with text** needed |
| Badges (shields.io etc.) | 18.2 | 73.0 | 0 | 0.3 | |
| Raw HTML | 11.8 | **68–76** | 4–8 | 27–40 | An HTML subset is mandatory (§4.3) |
| Wikilinks `[[…]]` | 0.2 | 1.4 | **98.3** | 0 | Only for an Obsidian vault |
| Emoji shortcodes `:rocket:` | 1.6 | 18.8 | 1.7 | 1.4 | gemoji table, cheap |
| Relative links to .md | 6.4 | **59.6** | 1.1 | **86.0** | Navigation inside the reader + "Back" |
| Anchors `(#…)` | 2.4 | 36.0 | — | 35.1 | Must-have |
| Cyrillic | 37.8 | 9.8 | 0.6 | 0 | A third of the user's files |
| CJK | 0.6 | **31.6** | 1.1 | 0 | Font fallback needed |
| Emoji characters | 7.8 | 44.4 | 1.7 | 4.8 | Color emoji are mandatory |

³ For Obsidian only the 5 GitHub types were counted. Obsidian callouts are broader (`info`, `example`, `danger`, `todo`, … — another 15 types in the corpus).

### 4.3 Which HTML subset the native renderer needs [measured, `html_tags.json`]

Top-500 READMEs, share of documents with the tag:
`img` 69.6 · `a` 59.8 · `p` 45.4 · `br` 40.4 · `div` 30.0 · `picture` 20.8 · `source` 20.4 · `details` 18.8 · `summary` 18.4 ·
`h1` 16.6 · `b` 15.6 · `strong` 14.2 · `table/tr/td` 13 · `sub` 9.6 · `h3` 8.6 · `em` 4.6 · `sup` 3.8 · `kbd` 3.6 · `hr` 3.6.
The `align=` attribute is in **58 %** of READMEs (centered logos and badges).
In **65 %**, raw HTML appears within the first 30 lines. In **80 %**, a remote image is within the first 30 lines, and in **61 %** it's an SVG.
`<source media="(prefers-color-scheme: dark)">` — 20 %, `#gh-dark-mode-only` — 3.2 %.
In VS Code docs: `div` 15.7 %, **`video` 14 %**, `details` 7 %.

**The minimal subset for v1:** `img` (src, width, height, alt, align), `a` (href, name/id),
`p`/`div`/`h1–h6` with `align="center|right"`, `br`, `hr`, `b/strong/i/em/code/kbd/sub/sup/s/del/ins/mark/u`,
`details`/`summary` (collapsing), `picture`/`source media=prefers-color-scheme` (choosing per theme),
simple `table/tr/td/th`; hide `<!-- -->` comments.
Show everything else (`video`, `iframe`, `style`, inline CSS) as a placeholder or a link.
Inlyne arrived at the same thing: "just enough HTML to render 95 % of markdown files".

---

## 5. Feature set: must-have for v1 / later / not needed

The "Startup" column: whether the feature affects the time to the first frame, and how to make it free.

| Feature | Class | Rationale (data) | Startup | Native? |
|---|---|---|---|---|
| GFM: tables, lists, task lists, strikethrough, autolinks | **v1** | Tables in 19–41 % of documents | Parser at MB/s, first screen only | Yes |
| Syntax highlighting | **v1** | Code in 25–65 %. The top 10 languages cover the bulk | Highlight only visible blocks, the rest lazily | Yes: lexers/tree-sitter/something syntect-like. No web needed |
| Light / dark / auto (follow the system) theme | **v1** | The user's system is in the **dark** theme. MarkText and PowerToys follow the system | Decide before the first frame (otherwise a white flash) | Yes |
| Zoom (Ctrl + wheel / ± / 0) | **v1** | In every competitor | No | Yes |
| Ctrl+F search (highlighting, F3, counter) | **v1** | #45267, every editor | The index is built after the first frame | Yes |
| Text selection and copying; a "copy code" button | **v1** | READMEs are commands to copy | No | Yes (hit-testing is a noticeable amount of work for a native renderer) |
| Table of contents (outline) in a sidebar or popup | **v1** | ≥ 5 headings in 50–91 % of documents | Hidden by default, built from the headings after the frame | Yes |
| Heading anchors, `#…` links | **v1** | 35–36 % of READMEs and docs | No | Yes (slugs per GitHub rules) |
| Relative links to .md + history (Alt+←/→) | **v1** | 60–86 % of READMEs and docs; the frogmouth model | No | Yes |
| External links → browser, a hover tooltip with the URL | **v1** | — | No | Yes (`ShellExecute`) |
| Local images (PNG/JPEG/GIF/WebP/BMP) | **v1** | 1–77 % | Decode asynchronously. Reserve space by width/height if given | Yes (WIC) |
| SVG with text | **v1** | 69 % of READMEs (badges) | Asynchronously | **Not Direct2D** (no `<text>`). lunasvg 3 (C++) or resvg (Rust) |
| Remote images | **v1** (privacy setting) | 85 % of READMEs, 80 % on the first screen | **Never block the first frame.** Disk cache. PowerToys blocks them by default | Yes (WinHTTP + cache) |
| HTML subset (§4.3) | **v1** | 76 % of READMEs | No | Yes (our own mini-DOM) or litehtml (report 02) |
| GitHub Alerts | **v1** | 18 % of READMEs, 39–46 % of docs | No | Yes (md4c `MD_FLAG_ADMONITIONS` in master) |
| Front matter: hide it or show it as a compact property table | **v1** | 99–100 % of docs/PKM | No | Yes |
| Emoji shortcodes `:smile:` | **v1** | 19 % of READMEs | No | Yes (gemoji table, ~1.9 thousand) |
| Color emoji, CJK and Cyrillic | **v1** | 44 % / 32 % / 38 % | No | Yes (DirectWrite color fonts + fallback) |
| Live reload when the file changes, keeping the scroll position | **v1** | Marked, Inlyne, MDHero, the browser extension. The "AI/editor writes, reader shows" scenario | Start the watcher after the frame | Yes (`ReadDirectoryChangesW`) |
| "Open in editor" (Ctrl+E) | **v1** | A reader isn't an editor: hand the file to VS Code/Void/Notepad | No | Yes |
| .md association / "Open with", drag-and-drop into the window | **v1** | The main scenario is a double click | No | Yes (HKCU ProgId + RegisteredApplications, the user confirms in Settings) |
| Recent files (Jump List) | **v1-lite** | — | `SHAddToRecentDocs` is free, after the frame | Yes |
| Multi-window (each double click is a window) | **v1** | Side-by-side comparison (#45267) | No | Yes |
| Footnotes | **v1** (cheap) | 0.3–1.1 % | No | Yes (`MD_FLAG_FOOTNOTES`) |
| Mermaid | **later (v1.x)** | 0.3–1.8 % | Only for documents with `mermaid`, after the first frame | Web (lazy WebView2) or mmdr (§6) |
| Formulas (KaTeX/Temml) | **later (v1.x)** | 0.1–0.6 % | Lazily | Web (KaTeX or Temml) or native MicroTeX (risky) |
| Printing / PDF export | **later** | Every editor has it, but for a reader it's a rare operation | Lazily | WebView2 `PrintToPdf` or D2D print |
| Explorer preview pane (IPreviewHandler) | **later (v2)**, high priority | PowerToys 0.6–1.4 s [measured], an issue about the slow preview | A separate DLL, in-proc in prevhost | Yes |
| Remembering the reading position per file | later | Convenience | No | Yes |
| Tabs | later / maybe never | Typora/MM/Ferrite. For a reader, windows + Snap are simpler | — | Yes |
| Wikilinks, embeds | later (Obsidian mode) | 98 % in a vault, ~0 % outside | No | Yes |
| `==highlight==`, sub/sup syntax | later | < 1 % | No | Yes (md4c flags) |
| User CSS themes | not needed in v1 | Requires a web engine | — | No |
| Editing, WYSIWYG | **not needed** | This is a reader; editing is left to an editor | — | — |
| Plugins, vault, backlinks, graph | not needed | Obsidian's territory | — | — |
| PlantUML/Graphviz/Vega, Marp slides | not needed | ~0 % | — | — |
| Spell checking, word counters | not needed | Editor features | — | — |

---

## 6. Which features require a web engine, and what it costs [measured]

### 6.1 Classification

| Feature | Web engine needed? | Comment |
|---|---|---|
| Everything in GFM, highlighting, images, SVG, alerts, TOC, search | **No** | Solved natively |
| The HTML subset from READMEs | **No** (our own mini-DOM or litehtml) | Full HTML/CSS — yes |
| **Mermaid** with full fidelity | **Yes** (mermaid.js) | mmdr is a native alternative with a fidelity risk |
| **Formulas** at the KaTeX/MathJax level | **Practically yes** | Natively: MicroTeX (C++, last push 2024-08, 661★). Windows has no system MathML renderer; MathML Core exists only in Chromium |
| User CSS, `<style>`, `<video>`, `<iframe>`, interactive HTML | Yes | Rare (video in 14 % of VS Code docs) |
| Printing with CSS paged media | Yes, or D2D print | |

### 6.2 The cost of the "rich mode" in Chromium 153 [measured]

Headless Edge 153.0.4234.32, 5 runs, medians. "After lib" is from the end of library execution to the frame with the result (2×rAF).

| Library | Size | Library execution | Work | Total after lib | Comment |
|---|---|---:|---|---:|---|
| **Mermaid 12.0.0** (`mermaid.min.js`) | **5.58 MB** | **196 ms** | 12-node flowchart **253 ms**, 5-participant sequence **35 ms** | 294 ms | The first diagram is expensive (chunks and the layout engine are lazy-loaded). Report 02 got +300–700 ms under load |
| **KaTeX 0.18.7** (HTML+CSS) | 273 KB JS + 25 KB CSS + 20 woff2 (254 KB) | 11 ms | 50 formulas `renderToString` **19 ms** | 114 ms (fonts 124) | Fast. Fonts load asynchronously, so layout shift is possible |
| KaTeX `output:'mathml'` | 273 KB | 19 ms | 8 ms | 78 ms | Rendered by Chromium's built-in MathML, no woff2 fonts |
| **Temml 0.13.5** (MathML) | **168 KB** | 28 ms | 9 ms | **71 ms** | The lightest. Good quality (`shots/web-temml.png`), Cambria Math font |
| MathJax 4.1.3 (`tex-svg.js`) | 1.85 MB | 112 ms | 50 formulas **955 ms** | **962 ms** | **Don't use it**: 10× slower than KaTeX |
| **mmdr 0.3.1** (native Rust, no browser) | exe 10 MB | — | flowchart: parse 3 + layout 107 + render 0.2 = **110 ms**; sequence **1.7 ms** | — | The whole process (with spawn) 68–180 ms. SVG with `<text>`, PNG via resvg. 23 diagram types, MIT. Quality close to mermaid, but edge routing is worse (`shots/mmdr-flow.png`) |

Add bringing up WebView2: `CreateCoreWebView2Controller` ≈ 285 ms when quiet and 450–600 ms under load
(report 02, [measured]). The result **for a document with Mermaid**: the diagram appears **~0.8–1.2 s** after
the native first frame. With mmdr, after 2–110 ms, with no browser.
Across the corpora only ~30 % of documents with Mermaid have a diagram within the first ~40 lines, i.e. on the first screen.
For the rest, the user will notice the delay only if they scroll down immediately.

### 6.3 A limitation of native graphics: SVG

Direct2D `ID2D1SvgDocument` supports only `circle, clipPath, defs, desc, ellipse, g, image(base64), line,
linearGradient, path, polygon, polyline, radialGradient, rect, stop, svg, title, use`.
**No `<text>`, `<style>`, `mask`, `filter`, `em`/`ex` units** (Microsoft Learn).
So shields.io badges (text in SVG), mermaid SVGs and logos with text would come out without text in Direct2D.
**lunasvg 3.x** is needed (C++, since 3.0 it handles text with system fonts, available in vcpkg) or **resvg** (Rust, the fidelity reference).
This decision doesn't depend on the choice of UI stack and is needed in v1.

---

## 7. The hybrid: an instant native first frame + a lazy web engine

### 7.1 Timeline (target)

```
t=0        double-click → CreateProcess (Defender ~20–35 мс; пол Win32 ≈ 90 мс до t_content)
~40 мс     main: mmap файла; быстрый скан «нужен ли rich-режим» (```mermaid, $$, \begin{) — микросекунды
~60 мс     окно создано: тема, Mica и dark caption выставлены ДО ShowWindow (бесплатно, §8.4),
           DWMWA_TRANSITIONS_FORCEDISABLED (–200 мс воспринимаемого времени, §8.5)
~150–250   ПЕРВЫЙ КАДР: вёрстка только видимого экрана. Картинки — плейсхолдеры с зарезервированным местом,
           mermaid/math — плейсхолдер «диаграмма…» (или исходник в стиле кода)
после кадра (idle): вёрстка остального → TOC → индекс поиска → подсветка видимых блоков →
           декодирование картинок (WIC) → HTTP-картинки из кэша/сети → watcher → Jump List
только если нужно (~2 % документов):
+0         создать WebView2 environment (дёшево) и controller (~285–600 мс) НЕВИДИМО
+~200      исполнить mermaid.min.js / katex.min.js
+~35–250   каждая диаграмма/формулы → SVG/HTML → подставить в документ
```

### 7.2 Three ways to "slot it in"

| Option | How | Pros | Cons / risks |
|---|---|---|---|
| **A. The web as a calculator** (recommended) | A hidden WebView2 receives the source → `mermaid.render()` → an **SVG string** via `postMessage` → native rendering through lunasvg/resvg straight into the document. For formulas: KaTeX/Temml → MathML/HTML → a **snapshot** (`CapturePreview` to PNG) | The whole document is native: one scroll, one search, one theme. No airspace problems | By default Mermaid puts `foreignObject` (HTML labels) into the SVG: `htmlLabels:false` is needed. `getBBox` in a hidden window needs checking (a hidden window gets no frames, report 02). Formula snapshots don't scale with zoom and have to be re-rendered |
| B. Replacing the whole document | When WebView2 is ready, render the whole document in it with the same `style.css` and swap out the native view, keeping the scroll position | Simple. Full fidelity (CSS, `<video>`, …) | A visible "jump" when metrics differ; +100–200 MB; search and selection switch between engines |
| C. No browser | mmdr (Rust, via a C ABI or a separate process) for Mermaid; MicroTeX or our own layout for formulas | Not a single Chromium process. Mermaid in 2–110 ms | mmdr's fidelity (0.3.x, "may not yet match mermaid-cli"). The MicroTeX project hasn't been updated since 2024 |

**Recommendation.** v1: Mermaid and formulas are shown as nicely formatted source with a
"diagram" / "formula" badge. That's honest and instant, and Mermaid source is readable on its own.
v1.x: option **A** (lazy WebView2 as a calculator) and, in parallel, evaluating mmdr as option **C** for Mermaid.
Keep mode **B** as a fallback, "Open in full web mode", for documents with heavy HTML.

### 7.3 Relation to the "resident" strategy

If the orchestrator picks the `-resident` variant (report 02: a shared browser process gives −45–50 % for WebView2),
one warmed-up WebView2 process can be kept for rich documents. For ordinary documents it's pointless:
the native path already fits into the first frame.

---

## 8. Visual design for Windows 11

### 8.1 Fonts (checked on the machine, `HKLM\…\Fonts` and `C:/Windows/Fonts`)

| Role | Font | Note |
|---|---|---|
| Text | **Segoe UI Variable** (`SegUIVar.ttf`, 1.85 MB): axes **wght 300–700**, **opsz 5–36**; named instances Small / Text / Display (15 in all) | With manual layout (DirectWrite), set opsz = point size, otherwise the optical correction is lost. XAML does this by itself |
| Headings | Segoe UI Variable **Display Semibold** (600) | Microsoft: Semibold for headings; Bold isn't part of the type ramp |
| Code | **Cascadia Mono** (a Windows 11 system font, `CascadiaMono.ttf`, variable), fallback Consolas | Cascadia **Code** with ligatures as an option. Italic isn't part of the system distribution |
| Emoji | Segoe UI Emoji (color, COLR) | Requires `D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT`. Ferrite/egui can't do this — visible in the screenshot |
| CJK / other scripts | Yu Gothic UI (ja), Microsoft YaHei UI (zh-Hans), Malgun Gothic (ko), Nirmala UI, Leelawadee UI | DirectWrite font fallback or `IDWriteFontFallback` |
| Serif "for reading" option | **Sitka** (VF with opsz: Small/Text/Subheading/Heading/Display/Banner), Georgia | A "Book mode" option |
| UI icons | Segoe Fluent Icons | For the TOC, search, menus — instead of our own PNGs |
| Formulas (MathML) | Cambria Math | Temml/KaTeX-mathml in WebView2 |

### 8.2 Type scale (a product proposal)

Anchors: the Windows 11 type ramp (Caption 12/16, Body 14/20, Body Large 18/24, Subtitle 20/28, Title 28/36, Title Large 40/52, Display 68/92)
and GitHub (16 px, line-height 1.5, h1 2em, h2 1.5em, code 85 %).
The bench spec (15 px) stays for comparability between prototypes. For the product, 16 is proposed:

| Element | DIP / line-height | Weight | Color light / dark |
|---|---|---|---|
| Body | **16 / 26** (1.6) | Text Regular 400, opsz auto | `#1f2328` / `#f0f6fc` |
| H1 | 32 / 40 + divider | Display Semibold | same as body |
| H2 | 24 / 32 + divider | Display Semibold | |
| H3 | 20 / 28 | Display Semibold | |
| H4 | 17 / 24 | Text Semibold | |
| H5 | 16 / 24 | Text Semibold | |
| H6 | 14 / 20 | Text Semibold | `#59636e` / `#9198a1` |
| Code (block and inline) | 14 / 21 (≈ 0.875 em) | Cascadia Mono | background `#f6f8fa` / `#151b23` |
| Captions, footnotes, status | 12 / 16 | Small | muted |
| Spacing between paragraphs | 0.75–1 em | | |

The GitHub palette (`github-markdown-css` v5.9.0, 02.2026) [checked in the CSS]:
light — background `#ffffff`, text `#1f2328`, muted `#59636e`, link `#0969da`, borders `#d1d9e0`, underlay `#f6f8fa`;
dark — background `#0d1117`, text `#f0f6fc`, muted `#9198a1`, link `#4493f8`, borders `#3d444d`, underlay `#151b23`.
Alert colors (light / dark): NOTE `#0969da`/`#4493f8`, TIP `#1a7f37`/`#3fb950`,
IMPORTANT `#8250df`/`#ab7df8`, WARNING `#9a6700`/`#d29922`, CAUTION `#d1242f`/`#f85149`.

### 8.3 Line length [measured]

Average character width of Segoe UI Variable (opsz = point size) on a typical paragraph from the corpus:

| Size | Latin, px/char | Cyrillic, px/char | Characters at 796 DIP (current spec) | At 680 DIP (Bear) | Width for 66 Cyrillic characters |
|---|---:|---:|---|---|---:|
| 15 px | 6.72 | 7.77 | **119 Lat / 102 Cyr** | 101 / 88 | 513 DIP |
| 16 px | 7.04 | 7.91 | 113 / 101 | **97 / 86** | 522 DIP |
| 17 px | 7.78 | 8.82 | 102 / 90 | 87 / 77 | 582 DIP |

The norm is 45–90 characters, ideally around 66 (Bringhurst, Butterick). Microsoft recommends 50–60 for UI. Bear: 680 px ≈ 75. iA: about 64.
Technical documents need width for tables and code.
**Proposal:** a text column **up to 680 DIP** at 16 px. Tables, code blocks and wide images
"break out" to ~960 DIP. Three presets: narrow 560 / normal 680 / wide 900 (+ "full width").

### 8.4 Materials and window chrome [measured]

- **Mica / Mica Alt, dark title bar: startup cost = 0** within noise.
  `t_content`, medians n=12: plain 112.9 · dark 107.6 · mica 108.2 · mica+dark 104.6 ms.
  `DwmSetWindowAttribute` takes < 0.5 ms (marks `window_created` → `dwm_attrs_set`).
- Microsoft's rules: Mica is only for the base layer of a long-lived window (title bar, sidebar).
  Content goes on a separate layer (`LayerFillColorDefault`). Mica samples the wallpaper once.
  In battery saver, with transparency off, or in an inactive window it falls back to a solid color.
  For a reader: **Mica in the title bar and the TOC sidebar, the document on a solid background** (readability).
- **No white flash:** the window background in the theme color (our own `WM_ERASEBKGND`/`WM_PAINT`),
  `DWMWA_USE_IMMERSIVE_DARK_MODE` before `ShowWindow` (otherwise a light title bar flashes),
  for WebView2, `DefaultBackgroundColor` or the `WEBVIEW2_DEFAULT_BACKGROUND_COLOR` variable.
- Minimal chrome: no toolbar by default. Window title = file name. TOC / search / theme / "⋯" buttons in the title bar (Fluent Icons).

### 8.5 Motion [measured]

Window opening as seen on screen (desktop BitBlt ≈ 60 Hz, medians n=10, `mica_test --mode=plain`):

| | window visible (flag) | 50 % on screen | 90 % on screen | Fully on screen |
|---|---:|---:|---:|---:|
| With the DWM animation (default) | 66 ms | 154 ms | 228 ms | **291 ms** |
| `DWMWA_TRANSITIONS_FORCEDISABLED` | 62 ms | 85 ms | 85 ms | **85 ms** |

The system open animation (scale + opacity) **adds ~200 ms** until the document is fully visible.
For a "bullet" it's worth turning off. Possibly as a setting ("instant open", on by default).
Inside the app: no animations on open. Smooth wheel scrolling ~100–150 ms.
Respect "Animation effects = off" (`SPI_GETCLIENTAREAANIMATION`).

---

## 9. Windows integration

| What | How | Note |
|---|---|---|
| .md / .markdown association | HKCU: ProgId + `Capabilities\FileAssociations` + `Software\RegisteredApplications`, no admin. The user makes the choice in Settings: `ms-settings:defaultapps?registeredAppUser=FastMD` (Win 11 22H2+ with the 2023-04 CU) | You can't become the default programmatically (protected UserChoice) — that's Microsoft's policy |
| "Open with", drag-and-drop | `OpenWithProgids`, `WM_DROPFILES`/`IDropTarget` | Cheap |
| Jump List / recent | `SHAddToRecentDocs` | After the first frame |
| Explorer preview pane | `IPreviewHandler` (an in-proc DLL; hosted in `prevhost.exe`) | PowerToys: 0.62 / 1.39 s [measured], blocks images. A native FastMD handler without WebView2 (an estimate, not a measurement): ~50–150 ms, in-proc, no new process. **A strong USP for v2** |
| A Peek-like "quick look" | Don't build our own. The preview handler covers the pane; PowerToys Peek uses its own previewers | |
| Single-instance? | **No**: each double click gets its own window (like Notepad with separate windows). A resident only as an option (§7.3) | Less complexity, Snap works |

---

## 10. Conclusions for FastMD

### 10.1 Recommendations

1. **Positioning:** "A Markdown reader that opens faster than Notepad and looks like GitHub in Windows 11".
   Competitors: 1–6 s (Electron/WebView2) or ~1.2–1.3 s (native GPU newcomers) [measured].
   FastMD's goal: **≤ 250 ms to the first screen** on medium, **≤ 400 ms** on large (first screen first).
2. **Architecture: native rendering + lazy enrichment.** The web engine is not on the critical path for any document.
   For the ~2 % of documents with Mermaid or formulas it is brought up after the first frame (option A from §7.2).
   For Mermaid, additionally evaluate the native mmdr.
3. **Scope of the v1 native renderer**, per the data in §4: GFM + alerts + footnotes + front matter + emoji shortcodes +
   the HTML subset (§4.3) + images (WIC) + **SVG with text (lunasvg/resvg, not D2D)** +
   asynchronous remote images with a cache + color emoji + CJK fallback + code highlighting.
   This is the mandatory part, not a "nice to have": without it, a README from GitHub looks broken.
4. **Parser:** md4c master (2026) already supports `MD_FLAG_FOOTNOTES`, `MD_FLAG_ADMONITIONS`, `MD_FLAG_LATEXMATHSPANS`,
   `MD_FLAG_WIKILINKS`, `MD_FLAG_HIGHLIGHT`. The latest tag is `release-0.5.3` — the commit needs to be pinned.
5. **Style:** Segoe UI Variable (opsz!), Cascadia Mono, the GitHub light/dark palettes, theme following the system
   (dark for the user), body 16 DIP, a column up to 680 DIP with breakout, Mica only in the title bar and sidebar,
   no white flash, **no open animation** (−200 ms of perceived time).
6. **v1 features (§5):** search, TOC, zoom, selection and copying, copying code, anchors, .md navigation with "Back",
   live reload keeping the scroll position, "Open in editor", association, drag-and-drop, Jump List.
   Initialize everything not needed for the first frame **after** it.
7. **The v2 trump card:** a native preview handler for the Explorer pane (PowerToys: 0.6–1.4 s, blocks images;
   #45267 and #46087 show demand).

### 10.2 Risks

| Risk | Why | What to do |
|---|---|---|
| The scope of "native HTML" and text selection | Our own mini-DOM, hit-testing, search and copying are the most labor-intensive part of the native path | Consider litehtml as the layout engine for the subset (report 02). Strictly limit the subset (§4.3) |
| SVG | D2D doesn't draw text | lunasvg/resvg from day one. Test on shields.io badges |
| Remote images on the first screen (80 % of READMEs) | Layout shift when the image arrives | Reserve space by width/height; a size cache; appear without animation |
| Mermaid fidelity without a browser | mmdr 0.3.x | Keep the mermaid.js path (option A) as the reference |
| A visual "jump" in mode B | Different metrics of the native and web renderers | Prefer option A |
| Large files | Every competitor fails (Typora refuses, Ferrite hangs) | First screen first, incremental layout, virtualization. Search index in the background |
| Turning off the DWM animation | May feel "un-Windows-like" | A setting, "instant" by default |
| Remote image privacy | The IP leaks to tracking pixels (which is why PowerToys blocks them) | A setting "load remote images: always / ask / never", "always" by default (like GitHub) |
| Default association | Can't be set programmatically | An in-app button that opens `ms-settings:defaultapps?registeredAppUser=…` |

---

## 11. Notes on the harness and protocol (for the orchestrator)

1. **`t_content` doesn't see the DWM open animation** [measured, §8.5]. `DwmFlush()` returns when the frame is composed,
   but on screen the window is still "growing" for ~200 ms. If one prototype sets `DWMWA_TRANSITIONS_FORCEDISABLED`
   and the others don't, `t_content` won't show it, while the perceived difference is ~200 ms.
   I propose: (a) agreeing that all prototypes leave the animation at its default (for comparability);
   (b) adding an optional "on screen" metric to the harness (desktop BitBlt within the window rectangle,
   as in `mica/run_transition.py`).
2. **The `external-title` mode measures only the window appearing**, not the content. Markdown Monster's window appears in 0.5 s,
   and the content in ~4.5 s [measured]. For third-party apps a visual timeline is worth using
   (`measure_apps.py`: PrintWindow frames → p50/VC). The script imports `bench.py` and requires no changes to the harness.
3. Grab rate: `PrintWindow(PW_RENDERFULLCONTENT)` for a 1000×800 window gives ~20–40 frames/s.
   For ±15 ms accuracy on fast prototypes, a screen BitBlt (60 Hz) is better, but then the window must not be covered.
4. A gotcha for CLI measurements: **Glow 3.0 reads stdin instead of the file argument if stdin isn't a TTY** (e.g. a pipe).
   Hence "2 bytes of output" in 0.17 s. `stdin=DEVNULL` is needed.

---

## 12. Sources

Products and status:
- Typora releases (1.14.10, Mermaid 11.x, MathJax v4): https://typora.io/releases/stable.html; "Typora uses Electron on Windows/Linux": https://support.typora.io/Launch-Arguments/ ; issue about large files: https://github.com/typora/typora-issues/issues/1285
- Obsidian: works only inside a vault — https://forum.obsidian.md/t/have-obsidian-be-the-handler-of-md-files-add-ability-to-use-obsidian-as-a-markdown-editor-on-files-outside-vault-file-association/314 ; startup time: https://forum.obsidian.md/t/obsidian-time-to-start-is-wildly-inconsistent/110681 (01.2026), https://forum.obsidian.md/t/startup-time-help/96844 (02.2025), https://forum.obsidian.md/t/lonnnngg-start-up-time/97958 ; Mermaid and MathJax: https://obsidian.md/help/advanced-syntax
- MarkText: revival and fork — https://github.com/marktext/marktext/issues/4098 , https://github.com/Tkaixiang/marktext ; 12 s / 3 s startup: https://github.com/marktext/marktext/issues/3862 (2024-09)
- Zettlr "40 seconds": https://github.com/Zettlr/Zettlr/issues/3391 (2022-04, v2.2.5)
- iA Writer for Windows 2.0 (core + background loading): https://ia.net/topics/ia-writer-for-windows-2-0 (2024-12), https://ia.net/topics/ia-writer-for-windows-2-0-released-into-the-wild (2025-02)
- Marked 3: https://markedapp.com/ , https://brettterpstra.com/2025/09/06/a-quick-look-at-marked-3/ , https://www.macsparky.com/blog/2026/06/marked-3-is-officially-out/
- Bear Sans and line length: https://blog.bear.app/2023/08/learn-about-our-new-custom-font-bear-sans/ , https://bear.app/faq/typography-options/ , https://blakecrosley.com/guides/design/bear
- Markdown Monster (WPF + WebView2, .NET 10): https://markdownmonster.west-wind.com/download , https://weblog.west-wind.com/posts/2021/Jul/22/Markdown-Monster-20-is-here , https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization
- VS Code math (KaTeX since 1.58): https://code.visualstudio.com/updates/v1_58
- Notepad Markdown: https://blogs.windows.com/windows-insider/2025/05/30/text-formatting-in-notepad-begin-rolling-out-to-windows-insiders/ ; tables: https://blogs.windows.com/windows-insider/2026/01/21/notepad-and-paint-updates-begin-rolling-out-to-windows-insiders/ , https://www.windowscentral.com/microsoft/windows-11/how-to-use-markdown-table-support-in-notepad-on-windows-11 ; release notes (launch performance, 06–07.2026): https://learn.microsoft.com/en-us/windows-insider/release-notes/apps/notepad
- PowerToys: the "Markdown Reader" request https://github.com/microsoft/PowerToys/issues/45267 (2026-02); "File Preview is Slow" https://github.com/microsoft/powertoys/issues/46087 (2026-03); Peek https://learn.microsoft.com/en-us/windows/powertoys/peek
- QuickLook MarkdownViewer (source code): https://github.com/QL-Win/QuickLook/tree/master/QuickLook.Plugin/QuickLook.Plugin.MarkdownViewer
- Markdown Viewer extension: https://github.com/simov/markdown-viewer
- GitHub: Alerts https://github.blog/changelog/2023-12-14-new-markdown-extension-alerts-provide-distinctive-styling-for-significant-content/ ; Mermaid https://github.blog/developer-skills/github/include-diagrams-markdown-files-mermaid/ ; math https://github.blog/changelog/2022-05-19-render-mathematical-expressions-in-markdown/ ; CSS https://github.com/sindresorhus/github-markdown-css (v5.9.0)
- Glow: https://github.com/charmbracelet/glow (v3.0.0, 2026-08) ; frogmouth: https://github.com/Textualize/frogmouth (v0.9.1)
- Inlyne: https://github.com/Inlyne-Project/inlyne (v0.5.3, 2026-08) ; Ferrite: https://github.com/OlaProeis/Ferrite (v0.3.0, 2026-05), https://news.ycombinator.com/item?id=46571980 ; MDHero: https://mdhero.app/windows/ , https://mdhero.app/blogs/best-free-markdown-viewers-2026/
- mmdr: https://github.com/1jehuang/mermaid-rs-renderer (v0.3.1, 2026-07) ; MicroTeX: https://github.com/NanoMichael/MicroTeX ; md4c: https://github.com/mity/md4c

Platform and design:
- Direct2D SVG support: https://learn.microsoft.com/en-us/windows/win32/direct2d/svg-support ; lunasvg: https://github.com/sammycage/lunasvg/releases
- Typography in Windows (Segoe UI Variable, type ramp, 50–60 characters): https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/typography
- Line length of 45–90 characters: https://practicaltypography.com/line-length.html
- Cascadia Code/Mono as Windows 11 system fonts: https://www.ctrl.blog/entry/cascadia-system-font/ , https://en.wikipedia.org/wiki/Cascadia_Code
- Mica: https://learn.microsoft.com/en-us/windows/apps/design/style/mica ; Win32: https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-mica-win32
- WebView2 DefaultBackgroundColor: https://learn.microsoft.com/en-us/dotnet/api/microsoft.web.webview2.core.corewebview2controlleroptions.defaultbackgroundcolor
- Default apps deep link: https://learn.microsoft.com/en-us/windows/apps/develop/launch/launch-default-apps-settings
- Companion report: `research/02-web-and-html-engines.md` (the cost of WebView2, Blitz, Sciter, complaints about Electron apps).

---

## Appendix A. How to reproduce

```powershell
cd C:/Main/Projects/FastMD/research/lab/product-ux
# 1) частоты функций
python feature_survey.py "C:/Main/Projects" "$env:USERPROFILE/.cargo/registry/src"   # FS_OUT=... для имени json
python fetch_github_readmes.py 5                        # топ-500 README (gh api, только чтение)
python fetch_repo_md.py obsidianmd/obsidian-help master en/ corpus_obsidian_help
python fetch_repo_md.py microsoft/vscode-docs main docs/ corpus_vscode_docs
python html_tags.py corpus_github corpus_vscode_docs corpus_obsidian_help
python extra_stats.py corpus_github
# 2) конкуренты (приложения лежат в apps/, портативно)
python measure_apps.py typora,marktext,mdmonster,inlyne,ferrite,pt-preview --doc medium --runs 3 --secs 9
python measure_apps.py typora,inlyne,ferrite --doc large --runs 1 --secs 30
python time_glow.py
# 3) цена Mermaid/KaTeX/Temml/MathJax (headless Edge)
cd webfeat; node gen_pages.js; node run_pages.js mermaid,katex,katex-mathml,temml,mathjax 5; cd ..
# 4) нативный Mermaid
apps/mmdr/mmdr.exe -i mmd/flow.mmd -o shots/mmdr-flow.png -e png --timing
# 5) Mica / dark / анимация открытия
../../../bench/tools/msvc.cmd cl /nologo /utf-8 /O2 /MT /DUNICODE /D_UNICODE mica/mica_test.c /Fe:mica/mica_test.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib dwmapi.lib
python mica/run_mica.py plain,dark,mica,mica-dark 12
python mica/run_transition.py 10
```

No apps were installed. Markdown Monster requires .NET 10: the runtime lives in `apps/dotnet10` and is wired in
via `DOTNET_ROOT` in `measure_apps.py`. After the measurements, delete the folders the apps created in `%APPDATA%`/`%LOCALAPPDATA%`
(the list comes from the `appdata_before.txt` snapshot) and the keys `HKCU\Software\Typora`, `HKCU\Software\West Wind Technologies`.
