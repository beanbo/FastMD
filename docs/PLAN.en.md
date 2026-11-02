[Русский](PLAN.md) | **English**

# FastMD — development plan

> Plan version: 21.09.2026, after v1.0.0 (stages 1–6 done, except code signing and the run on low-end hardware); edit mode (7.6) added on 25.09.2026.
> This plan replaces the former `ROADMAP.md`.
> Sources of priorities: [REPORT.md](../REPORT.en.md), [research/06 §5, §10](../research/06-product-ux-landscape.en.md) (how often features appear in real READMEs) and your requests.

---

## 0. Principles (apply to every task)

1. **Speed is the main goal.** A feature must not slow down start-up. The only things allowed on the critical path of the first frame are reading the file, parsing, laying out the first screen and drawing on the CPU.
   - Everything new starts after the first frame, in the background or on a user action.
   - Forbidden on the critical path: a GPU device, COM/WinRT, the network, WebView2, image decoding and loading "heavy" DLLs.
   - Budget: `t_content(FastMD) − t_content(baseline-win32)` ≤ 0 ms on medium and ≤ +15 ms on large, in one interleaved `bench.py` run. FastMD is currently ~15 ms *faster* than the baseline (control measurement of v0.2.0: `bench/results/guard-v0.2.0.json`).
2. **Every task is closed by three checks:**
   - a speed measurement — the command in §2;
   - a UI test — `python app/tests/ui_smoke.py`, with a check of its own added for a new feature;
   - a visual check with screenshots, usually in the light and dark themes.
3. **Secure by default.** A document is untrusted input.
   - Links open according to an allowlist of schemes.
   - Network access only by an explicit setting.
   - Parsers are fuzzed.
   - No scripts from the document are ever executed.
4. **Windows-only is a deliberate decision.** The engine is built on DirectWrite; portability is not a goal.

---

## 1. Where we are now: v1.0.0

| Area | Done |
|---|---|
| Speed | ~66 ms to the first screen on small, medium and large (3.7 MB); the empty-window baseline is ~80 ms in the same run. Everything new is off the critical path of the first frame |
| Rendering | CommonMark + GFM, GitHub Alerts, footnotes, front matter as a table, emoji, syntax highlighting for **55 languages**, an HTML subset, **SVG**, **images from the network**, **TeX formulas** and **Mermaid diagrams** |
| Reading | selection by mouse and **by keyboard**, copying **with formatting (HTML + RTF)** and **as Markdown**, **dragging out**, **printing and PDF**, search 2.0, table of contents, column width, font and size, themes, **high contrast** |
| Navigation | anchors, relative .md links with history, Tab through links, link and image menus, remembered reading position |
| Windows | .md association, Jump List, **Explorer preview pane**, **.md thumbnails**, **per-user installer**, **updates with consent**, **crash reports**, **screen reader support** |
| Infrastructure | CMake + Ninja, and cargo for the two Rust libraries; `build.ps1`, `publish.ps1`, `package.ps1`; the speed harness (`--cpus` to emulate a weak CPU); the UI test (136 checks), checks for the installer, preview pane, screen reader, formulas and diagrams, fuzzing with ASan |

What 1.0 lacks and why: **code signing** (needs a certificate) and **a run on low-end hardware** (needs another computer).

---

## 2. How we work (process)

| Step | Command / rule |
|---|---|
| Development build | `pwsh -File app/build.ps1` → `app/build/Release/FastMD.exe` |
| Speed test | `python bench/harness/bench.py run fastmd-app,baseline-win32 --doc small,medium,large --runs 12` (no other copies of this exe may be running before the measurement) |
| UI test | `python app/tests/ui_smoke.py` |
| Publishing (your working copy) | `pwsh -File app/publish.ps1` → `app/out/Release/FastMD.exe`; works even while documents are open |
| Commits | one per finished task, message in English, one line in Russian in `CHANGELOG.md` |
| Versions | SemVer: 0.x.y until the 1.0 release; the number lives in `project(FastMD VERSION …)` in `app/CMakeLists.txt` |
| Release | a `vX.Y.Z` tag and a GitHub Release with a zip (exe, README, licenses), notes from `CHANGELOG.md` |

Task size: **S** — up to half a day, **M** — 1–2 days, **L** — 3–5 days, **XL** — 1–2 weeks of an experienced developer's work. With me it is usually faster, but the checks take their own time.

---

## 3. Stages and tasks

### Stage 1 — v0.2 "Comfortable reading" — ✅ done in v0.2.0

The goal: make the reader pleasant to use every day. All tasks are closed by the three checks from §0: a speed measurement, a UI test against the definition of done, screenshots of the light and dark themes.

| # | Task | Size | Result |
|---|---|---|---|
| 1.1 | Saving the window position and size | — | v0.1.1 |
| 1.2 | Horizontal scrolling of wide code and tables | M | ✅ v0.2.0 |
| 1.3 | Table of contents (outline) | M | ✅ v0.2.0 |
| 1.4 | Per-file reading position memory | S | ✅ v0.2.0 |
| 1.5 | Search bar 2.0 | M | ✅ v0.2.0 |
| 1.6 | Start screen with recent documents + Jump List | S | ✅ v0.2.0 |
| 1.7 | Keyboard navigation through links and the link menu | S | ✅ v0.2.0 |
| 1.8 | Column width: presets | S | ✅ v0.2.0 |
| 1.9 | Settings window | M | ✅ v0.2.0 |
| 1.10 | "Open in editor": choosing the editor | S | ✅ v0.2.0 |

What turned out differently from the plan:
- **1.2:** the block's scrollbar appears both on hover and for a second after scrolling with the wheel, the touchpad or search. This is feedback on the position.
- **1.6:** the Jump List needed no code. Windows shows the "Recent" category by itself, since FastMD is registered for .md and calls `SHAddToRecentDocs`.
- **1.9:** the "images from the network" setting moved to task 2.3: without the download itself it would change nothing. The interface language (RU/EN) was done right away, so task 6.3 shrank to proofreading the texts.
- **1.10:** Sublime Text was added to the auto-detection.

**1.2 Horizontal scrolling.**
- A code block or table wider than the column gets its own X offset.
- Scrolling: Shift+wheel over the block and the touchpad (`WM_MOUSEHWHEEL`), a thin scrollbar at the bottom of the block on hover.
- Selection and search take the offset into account: when jumping to a match, the block scrolls itself into place.
- A "wrap lines in code" option — a menu item, off by default.
- *Done when:* a 200-character line can be read in full, and the UI test scrolls the block and finds text at the end of the line.

**1.3 Table of contents.**
- A panel on the left, opened with Ctrl+Shift+O or a button. Built from `Doc::headings` after the first frame.
- Highlights the current section while scrolling; a click jumps to the heading.
- In a narrow window the panel is shown over the text; in a wide one it shifts the column.
- The open/closed state is remembered.
- *Done when:* medium.md has 24 sections, a click on the fifth heading scrolls to it, and the current section is highlighted.

**1.4 Reading position.**
- For the last 200 files, `{путь, размер, время изменения, scrollY-якорь}` (path, size, modification time, scrollY anchor: block + offset) is stored. Location: `%LOCALAPPDATA%\FastMD\positions.bin`, read *after* the first frame.
- When the document is reopened, it scrolls smoothly to that place.
- If the file has changed, the position is looked up by the heading text of the nearest section.

**1.5 Search bar 2.0.**
- ↑ ↓ ✕ buttons and the mouse; "Match case" (Alt+C) and "Whole word" (Alt+W) toggles.
- The query is kept between documents.
- Matches show up as marks on the scrollbar.
- IME input (Chinese, Japanese): the input box lives on a separate thread — the document's UI thread starts without TSF for the sake of start-up speed (since 1.3.1, TSF for switching the keyboard layout is turned on in it after the first frame, without IME).

**1.6 Recent documents.** With no file in the arguments, a list of the last 10 documents with search by name is shown instead of an empty screen. Plus a Jump List on the taskbar (`SHAddToRecentDocs` is already called; what remains is the "Recent" category).

**1.7 Keyboard.**
- Tab / Shift+Tab through links with a visible focus; Enter opens.
- Right-click on a link: "Open", "Copy link address"; on an image: "Copy image", "Open image file".

**1.8 Column width.** Narrow (560), normal (680), wide (900) or full width. Through the menu and Ctrl+Alt+← / →.

**1.9 Settings window.**
- Options: theme, text font (Segoe UI Variable or the "book" Sitka with optical sizes), font size, column width, the editor for Ctrl+E, images from the network ("always / ask / never" — see 2.3), scroll animation, interface language.
- Implementation: a Win32 window, custom-drawn by the same engine, opened only on command.

**1.10 Editor.** An "Open in…" setting with auto-detection of VS Code, Void, Cursor, Notepad++, Notepad and the option to point at your own exe. By default, the system's "Edit" action.

---

### Stage 2 — v0.3 "A README from GitHub looks the way it does on GitHub"

The goal: rendering parity with GitHub on real READMEs. According to research 06: HTML appears in 76 % of READMEs, remote images in 85 %, SVG badges in 73 %.

| # | Task | Size | Status |
|---|---|---|---|
| 2.1 | md4c update: footnotes, alerts, regression suite | M | ✅ except the spec suite |
| 2.2 | HTML subset (mini-DOM) | L | ✅ except colspan |
| 2.3 | Remote images | L | ✅ without ETag revalidation |
| 2.4 | SVG (badges included) | M | ✅ |
| 2.5 | Syntax highlighting (our own registry instead of Lexilla) | M | ✅ 55 languages |
| 2.6 | Emoji shortcodes `:smile:` | S | ✅ |
| 2.7 | Front matter as a property table | S | ✅ without collapsing |
| 2.8 | Anchor on heading hover | S | ✅ |
| 2.9 | Formulas and Mermaid "as code" (temporary) | S | ✅ formulas; the mermaid label came with 2.5 |
| 2.10 | Visual regression on real READMEs | M | ✅ without comparison against GitHub |

**2.1 md4c master.** Done on 20.09.2026, except the spec suite.
- Pinned to master commit `7fc1815a` (17.09.2026). No patches of our own were needed: UTF-16 is supported by the library itself (`MD4C_USE_UTF16`).
- In the new version `MD_DIALECT_GITHUB` already includes `MD_FLAG_FOOTNOTES` and `MD_FLAG_ADMONITIONS`, so `> [!NOTE]` callouts now arrive as an `MD_BLOCK_ADMONITION` block rather than as a quote. Our former text-based parsing is kept as a fallback.
- Footnotes: a `[^1]` reference is drawn as `[1]` and leads to the definition; definitions are numbered and lead back with an arrow. Anchors of non-headings live in `Doc::anchors`.
- md4c sends a definition's content without a paragraph block, as in tight lists — the builder starts the leaf itself.
- *Remaining:* a run of CommonMark 0.31.2 (652 examples) and GFM specifically in the UTF-16 build; a check of autolinks ending with a period.

**2.2 HTML subset.** Raw HTML from md4c is parsed into a small DOM and drawn by the same engine. Supported:
- `<img src width height align alt>`, including inside `<a>` (linked badges);
- `<p align=center>` and `<div align=center>` — centering logos and badges;
- `<br>`, `<hr>`;
- `<details><summary>` — a collapsible block, collapsed by default and expanded with a click;
- `<picture><source media="(prefers-color-scheme: dark)">` — an image matching the theme;
- `<sub>`, `<sup>`, `<kbd>` (a key in a frame), `<b>/<strong>`, `<i>/<em>`, `<code>`, `<a href>`, `<a name/id>` (anchors);
- simple `<table>/<tr>/<td>/<th>` without colspan (colspan later).

Other tags are output as text with the tags stripped. Scripts, styles and iframes are never executed.
*Done when:* 20 popular READMEs look as they do on GitHub in a screenshot comparison (2.10).

**2.3 Remote images.**
- Download through WinHTTP in the background, strictly after the first frame. Space is reserved from `width/height` in the HTML or from the file header, so the layout does not jump.
- An on-disk cache `%LOCALAPPDATA%\FastMD\cache` (LRU, 200 MB) that honors ETag/Last-Modified.
- Limits: timeouts, a maximum size, https only (http by a setting).
- A privacy setting "always / ask / never": tracking pixels reveal the IP. The default is "always", as on GitHub.
- Animated GIFs are shown as their first frame; animation later, and optional.

**2.4 SVG.** The lunasvg library (C++, MIT) rasterizes SVG into a DIB, with text support (shields.io badges). External references inside SVG are forbidden. Rasters are cached by size and DPI.

**2.5 Syntax highlighting.** Done on 21.09.2026 — but not with Lexilla.
- *Decision changed:* Lexilla is ~4 MB of source, its own document interface and a style table per lexer. Our highlighter is already built as a language registry: a table row = the comment and string syntax plus a keyword list. Adding a language costs one row, so 38 new languages (55 entries in total, 163 name aliases) came without a new dependency and without growing the exe by megabytes.
- The language label in the corner of a code block is done; it also covers the "diagram" label for ```mermaid (task 2.9).
- If a real grammar is ever needed (for example, for highlighting in export), Lexilla can be added as a separate DLL following the `fastmd-svg.dll` pattern.
- **Markup and YAML get their own parsers** (25.09.2026, shipped in 1.3.1). The generic rules read the text between HTML tags
  and YAML values as code: a capitalized word became a "type", a year a "number". Now `html|xml|svg|xaml|htm` and `yaml|yml`
  are parsed separately (`LexMarkup`, `LexYaml` in `highlight.cpp`), with GitHub's colors, and a new
  `P_TAG` (entity-tag) for the tag name. For languages where bare words are data (shell, bat, Dockerfile, Makefile, CMake, INI/TOML,
  conf/.env, TeX), the `words` flag in the registry row turns off "types" and space-separated calls.
  *Left alone:* TeX commands (`\section`) are not highlighted — `\` does not start a word; in an nginx line `;` is taken as
  the start of a comment.

**2.6 Emoji shortcodes.** The gemoji table (~1.9 thousand names) is embedded compressed: `:rocket:` → 🚀. Works outside code.

**2.7 Front matter.** A compact "key: value" table. Complex YAML (lists, nesting) is shown as now, as a YAML block. The block collapses.

**2.8 Heading anchors.** On hover, a 🔗 icon appears to the left of a heading. A click copies a `файл.md#slug` link and scrolls to the heading.

**2.9 Formulas and Mermaid as code.**
- The md4c flag `MD_FLAG_LATEXMATHSPANS` is turned on: `$…$` and `$$…$$` are shown in a monospace font, like code.
- ```mermaid blocks get a "diagram" label.
- Real rendering comes in stage 4.

**2.10 Visual regression.** Done on 21.09.2026: `app/tests/readme_check.py` + the list `app/tests/readmes.txt` (15 repositories: vscode, react, rust, cpython, node, tailwind, svelte, deno, ollama, md4c, lunasvg, bat, fzf, neovim, obsidian).
- Downloads the READMEs, opens each in a real window, saves a snapshot and compares it with the one accepted earlier; prints the share of "non-empty" pixels and the number of differing pixels.
- *Decision changed:* there is no comparison against GitHub's reference rendering — that needs a browser and a manual check of every image, while comparing against our own accepted look already gives the regression its value. Differences caused by network images (that had not arrived in time) are visible in the report and are reviewed by eye.
- This corpus is where the last gap of the stage turned up: Markdown badges `[![alt](badge)](link)` were shown as alt text; now they are inline images.

---

### Stage 3 — v0.4 "Working with text"

| # | Task | Size | Status |
|---|---|---|---|
| 3.1 | Copying with formatting (HTML + RTF) | M | ✅ |
| 3.2 | Copying as Markdown (source) | M | ✅ |
| 3.3 | Keyboard selection | S | ✅ |
| 3.4 | Printing and PDF export | L | ✅ without a preview window |
| 3.5 | Dragging out (text, links, images) | S | ✅ |

**3.1 Copying with formatting.** Besides text, `CF_HTML` and RTF are put on the clipboard: pasting into Word or Outlook keeps headings, bold text, links, tables and code. Done on 21.09.2026 in `src/copy.cpp`: one pass over the selected blocks builds both entries at once.

**3.2 Copying as Markdown.** Done on 21.09.2026.
- A "text position → source position" map (`Doc::srcMap`). Built from md4c's pointers: `ptr − src` works for all of the text, verified on 145 thousand calls.
- A "Copy as Markdown" menu item and Ctrl+Shift+C. The selection is extended to line boundaries; line breaks are CRLF.

**3.3 Keyboard selection.** Done on 21.09.2026. Shift+arrows, Shift+Home/End, Ctrl+Shift+arrows (by word), Ctrl+Shift+Home/End and Shift+PageUp/PageDown — with a visible caret that appears only during keyboard selection and keeps its column when moving up and down. The caret is drawn together with the document, so scrolling carries it along with the text and partial frame redraws keep working.

**3.4 Printing and PDF export.** Done on 21.09.2026 (`src/print.cpp`, `src/canvas_print.cpp`).
- Layout by the same engine at the printer's DPI: for the duration of printing the window lends its geometry to the paper and gets it back along with the reading position.
- Pages are cut at block boundaries; a block taller than a page is cut between lines of text or table rows (`BlockSplitY`).
- Header and footer: the file name at the top, "N of M" at the bottom. The palette is always light.
- Text goes out as GDI glyphs (`ExtTextOut` + `ETO_GLYPH_INDEX`) with a font made from the same DirectWrite face, so in the PDF it stays text; color emoji are drawn as COLR layers. Check: the UI test prints a document to PDF and reads it back through pypdf.
- Choosing the printer, paper and page range is the system dialog. PDF goes through "Microsoft Print to PDF" with the file name set in advance (there is no second dialog).
- *Remaining:* there is no preview window of our own (the Windows system dialog has none either) — PDF export serves as the preview; right-to-left text is printed left to right.

**3.5 Dragging out.** Done on 21.09.2026 (`src/drag.cpp`). Selected text (text + CF_HTML + RTF), a link (text + `UniformResourceLocatorW`) and an image (`CF_DIB` + `CF_HDROP` for a file on disk) are dragged into other applications through `DoDragDrop`; COM is brought up on the first drag. The drag threshold is the system one (`SM_CXDRAG`); a press inside the selection clears it only if the mouse has not moved. An automated test checks the set of formats (`Q_DRAG`); the drag loop itself belongs to the system and is checked by hand.

---

### Stage 4 — v0.5 "Formulas and diagrams"

| # | Task | Size | Status |
|---|---|---|---|
| 4.1 | Formulas: RaTeX | L | ✅ |
| 4.2 | Mermaid: our own Rust renderer (instead of mmdr + WebView2) | L | ✅ 30 of 31 kinds |
| 4.3 | Compatibility checks | M | ✅ with our own suite |

**4.1 RaTeX.** Done on 21.09.2026.
- The RaTeX crates (`ratex-parser`, `ratex-layout`, `ratex-svg` with `embed-fonts`) are built into `fastmd-tex.dll` — with a small C ABI of our own in `app/rust/fastmd-tex`. The SVG comes out already as glyph outlines, with no references to the KaTeX fonts, so it is drawn by our own SVG engine (lunasvg).
- Measurements on this machine: the first formula 1.07 ms, then 0.09–0.36 ms. Parsing runs on a background thread after the first frame; in the first frame space for the formula is reserved from an estimate.
- An inline formula sits on the baseline (`Image::ascent` → `IDWriteInlineObject`); a formula in its own paragraph becomes a block. The color comes from the theme; when the theme changes, the document is reloaded.
- Whatever the engine cannot parse is shown as source — as on GitHub.

**4.2 Mermaid.** Done on 21.09.2026, but not as planned; flowcharts were moved to our own layout in 1.1.0.
- Instead of `mmdr` + a WebView2 fallback, `mermaid-rs-renderer` (pure Rust, MIT) was taken: it handles 23 kinds of diagrams and outputs SVG. The WebView2 fallback was not needed and was not built — there is no browser in the package at all.
- A ```` ```mermaid ```` block becomes an image block; until the diagram is drawn (and if it never is), its source is visible in its place. The diagram background is transparent, with a palette of its own in the dark theme.
- **Our own flowchart layout engine (1.1.0).** The library places `graph` / `flowchart` nodes once and then pushes them apart until nothing overlaps. On a real architecture diagram — nested subgraphs, arrows leading into a subgraph — this produced frames lying across one another, nodes outside their own subgraph and a canvas several times larger than needed. Now `graph` / `flowchart` is laid out by `app/rust/fastmd-mermaid/src/flow/`: the classic layered layout (break the cycles → ranks → order within a rank → coordinates), with each subgraph laid out on its own and reaching its parent as a single box — the same way Mermaid itself does it. The other 22 kinds of diagrams remain with the library.
- A diagram with 32 nodes and 5 subgraphs: before, 4246×5308 in 623 ms (and over the rasterization limit of 4096 — the diagram was squashed as well); now 2404×1718 in 2.8 ms. A simple diagram: 0.06 ms against 17 ms.
- A label's width is measured with the very font file that `fastmd-svg.dll` later draws it with (Segoe UI): measuring with one font and drawing with another is what makes text spill over the frame.
- A large diagram does not fit in the column and must not shrink: the diagram takes the full width between the margins (`g.availW`), and if even that is not enough, it is drawn at its natural size and the block scrolls sideways (`ImageScrollsWide`, the same bar as for a wide table). On paper and in the Explorer pane there is no scrolling — there the diagram shrinks to fit the width (`g.fitWide`).

**4.3 Compatibility.** Done on 21.09.2026: `python app/tests/math_check.py` runs 108 formulas (the whole set of constructs that KaTeX documents) and 31 diagrams, assembles the contact sheets `math-formulas.png` / `math-diagrams.png` and the report `math-report.md`.
- Result: **108 of 108 formulas**, **30 of 31 diagrams**. A `sequenceDiagram` with activation (`A->>+B`) is not drawn — a limitation of the library. The slowest diagram takes 2.8 ms (30.7 ms before our own layout engine).
- The comparison is against our own accepted result: there is no KaTeX or mermaid-cli on the machine (the same deviation as in 2.10).

---

### Stage 5 — v0.6 "Windows integration and delivery"

| # | Task | Size | Status |
|---|---|---|---|
| 5.1 | Moving the engine into a static library | M | ✅ differently: shared sources + stubs |
| 5.2 | Explorer preview pane (preview handler) | L | ✅ 6–20 ms |
| 5.3 | .md thumbnails in Explorer (optional) | M | ✅ |
| 5.4 | Installer without administrator rights | M | ✅ |
| 5.5 | Code signing | S (+ obtaining the certificate) | ⛔ needs a certificate |
| 5.6 | Auto-update | M | ✅ |
| 5.7 | winget | S | ✅ manifest ready, submission is up to the owner |

**5.1 Engine as a library.** Done on 21.09.2026, but not as intended: there is no separate `fastmd-core.lib`.
The engine's only consumer is the preview pane DLL, and it needs a different set of modules than the window: no search, no
table of contents, no settings, no background threads, no network. So the DLL is built from the same engine sources
(`view.cpp`, `layout.cpp`, `canvas_gdi.cpp`, `parse.cpp`, `html.cpp`, `highlight.cpp`, `theme.cpp`, `svg.cpp`,
`formulas.cpp`), and everything else is covered by stubs in `preview/stubs.cpp` — twenty functions, one line each.
The separation is achieved (the engine lives in two different binaries and knows nothing about the window), and no extra library appeared.

**5.2 Preview handler.** Done on 21.09.2026 (`app/preview/preview.cpp`).
- An in-proc COM server (`IPreviewHandler`, `IInitializeWithFile`, `IObjectWithSite`, `IOleWindow`,
  `IPreviewHandlerVisuals`); Explorer hosts it in `prevhost.exe`.
- **Measured: 6–20 ms** from `DoPreview()` to a rendered document (700×900) — against 0.6–1.4 s for PowerToys. A document with a formula costs ≈40 ms the first time in the process: the typesetting library is loaded.
- Read-only and offline: network images are not downloaded, settings are not written, the document is not changed. The wheel and
  arrow keys scroll the pane.
- Images from disk, SVG, formulas and diagrams are prepared by the pane synchronously when the document loads: it has no background threads, and no first frame to protect either.
- Per-user registration: `FastMD.exe --register-preview` (the installer calls it), removal with `--unregister-preview`.
  No `regsvr32` and no administrator.

**5.3 Thumbnails.** Done on 21.09.2026: an `IThumbnailProvider` in the same DLL, with its own CLSID. A thumbnail is a page: the document
is laid out at the usual column width and drawn into an image `cx` high and `cx/1.414` wide, always in the light
palette. Measured: **1.5–2.7 ms** per 256 px thumbnail.

Checking 5.2 and 5.3: `python app/tests/preview_check.py` registers the handlers, creates them through COM from the registry,
shows a document in a real window, captures a picture, takes a thumbnail and analyzes it pixel by pixel, and at the end removes the
registration and makes sure not a single key is left in the registry.

**5.4 Installer.** Done on 21.09.2026: our own tiny C++ installer (`app/setup/setup.cpp`); Inno Setup was not needed.
- Per-user into `%LOCALAPPDATA%\Programs\FastMD`, no admin. We do not use MSIX: AppData virtualization slows down file I/O.
- Carries the exe, three libraries and the licenses inside itself as RCDATA resources (the build puts them into `setup_payload.rc`).
- A Start menu shortcut, an entry in "Settings → Apps" (`DisplayName`, version, size, `QuietUninstallString`), the .md association by a checkbox in the dialog.
- Switches: `/S` silent, `/noassoc` without the association, `/uninstall` (+`/S`). When uninstalling itself, the installer copies itself to `%TEMP%` so it can remove the whole folder, and takes the association away only if it pointed at the copy being removed.
- Check: `python app/tests/setup_check.py` installs, inspects the files, the shortcut and the "Apps" entry, runs the installed copy on a document and uninstalls again — twelve checks.

**5.5 Signing.** Azure Trusted Signing (~$10/month) or an OV certificate. Without a signature SmartScreen warns on the first run; a signature may also speed up Defender's check at launch.

**5.6 Auto-update.** Done on 21.09.2026 (`app/src/update.cpp`).
- One HTTPS request to GitHub Releases once a day, after the first frame, in the background. Nothing about the reader is sent: no identifiers, no telemetry. Turned off in the settings ("Check for updates"); does not run at all in benchmark mode.
- When a newer version is found, "Version X is available" pops up and an "Update FastMD…" menu item appears. From there on, only with consent: a dialog with the version numbers, the installer download and **a SHA-256 check against the file from the same release**. There is no signature yet (task 5.5); the hash is what can be checked today.
- The update is installed by the ordinary installer with `/S`: it renames the running exe, so open windows do not get in the way.
- Check: the UI test brings up its own server on loopback, serves a made-up release and verifies that there is exactly one request, the installer is downloaded, the hash matches, and with the setting off there is no request at all.
- **Fixed on 24.09.2026 (shipped in 1.3.0):** an installed 1.1.0 never offered 1.2.0. The version found
  lived only in a pop-up line for 4 seconds and in the menu of one window, and the "checked today" mark was written
  before the request — a failed request postponed the next one by a day. Now the version found is remembered in the registry
  (`UpdateVersion`, `UpdateUrl`, `UpdateShaUrl`) and is visible in every window: a dot on the gear, and in the settings an
  "Update" row with a single button (check now, update to X, restart). A failed check is retried after an
  hour, and a window left open for days checks once a day; after a silent install FastMD offers to restart at the same
  place in the same document. The update threads no longer hold up opening a document. Installed 1.1.0 and 1.2.0
  check the old way: the new version will reliably reach them only through one manual install.

**5.7 winget.** The manifest is ready: `packaging/winget/` (three files + instructions on how to fill in the version and hash and check with `winget validate`). Submitting the pull request to `microsoft/winget-pkgs` is up to the project owner: it is a publication in their name into a shared catalog.

---

### Stage 6 — v1.0 "Product"

| # | Task | Size | Status |
|---|---|---|---|
| 6.1 | Accessibility (UI Automation) | L | ✅ |
| 6.2 | High contrast and system colors | S | ✅ |
| 6.3 | RU / EN localization | S | ✅ |
| 6.4 | Crash handling | S | ✅ except the parse timeout |
| 6.5 | Fuzzing and security | M | ✅ with our own harness |
| 6.6 | Performance on low-end hardware | M | ⚠️ partial: needs another computer |
| 6.7 | Documentation and website | M | ✅ without a website |
| 6.8 | Release 1.0 | S | ✅ 21.09.2026 |

**6.1 Accessibility.** Done on 21.09.2026 (`app/src/uia.cpp`).
- A UI Automation provider: the window is a `Document` with a `TextPattern`. Ranges are computed directly from the model (flat text + block table); there is no second copy of the document.
- Movement by character, word, line (as DirectWrite wrapped them), paragraph and the whole document; selection, scrolling to a range, text rectangles on screen, search within a range.
- The heading level is exposed as the `UIA_StyleIdAttributeId` attribute (`StyleId_Heading1…6`) — Narrator and NVDA use it to jump between headings.
- `uiautomationcore.dll` is delay-loaded, and the first `WM_GETOBJECT` for the root arrives only when a screen reader is actually running: those who do not use one pay nothing.
- Check: `powershell -ExecutionPolicy Bypass -File app/tests/uia_check.ps1` — 13 checks by a real UI Automation client (.NET). This version 1.0 client cannot query the `StyleId` attribute, so the test checks traversal by paragraph, which the jump by heading is built on.

**6.2 High contrast.** Done on 21.09.2026: with a high-contrast theme on, the palette is built from `GetSysColor` (`theme.cpp`), syntax highlighting is turned off, and a mode change is picked up via `WM_SETTINGCHANGE` / `WM_SYSCOLORCHANGE`.

**6.3 Localization.** Done: all interface strings in `strings.cpp` are in Russian and English, the language follows the system with a switch in the settings; the English texts were proofread on 21.09.2026. System dialogs (printing, file picker, "Default apps") are translated by Windows itself.

**6.4 Crashes.** Done on 21.09.2026, except the parse timeout (`app/src/crash.cpp`).
- The exception handler writes a minidump to `%LOCALAPPDATA%\FastMD\crashes`; it is installed with one call, and dbghelp.dll is loaded only at the moment of the crash.
- On the next launch (after the first frame), a one-time offer to open the folder. The last ten reports are kept. Nothing is sent anywhere.
- **Not every crash leaves a dump** — found out on 22.09.2026 with the COM crash (CHANGELOG, 1.1.1). An exception
  inside window message handling (`WM_PAINT` and the rest are a user-mode callback from the kernel) terminates the process
  with code `0xC000041D` (`STATUS_FATAL_USER_CALLBACK_EXCEPTION`) without ever reaching `SetUnhandledExceptionFilter`: that is how
  most crashes ended on the machine where it was being investigated. Fail-fast (`0xC0000409`: CFG, heap corruption,
  `__fastfail`) calls no handlers at all — that is how it crashed here, 6 times out of 6 with no dump. What ends up in `crashes` is
  mostly worker-thread crashes, so the number of files there understates the crash rate.
  *Possible:* `AddVectoredExceptionHandler` sees exceptions in callbacks too — but also every handled one, so it needs a
  filter; fail-fast is caught only by WER LocalDumps.
- *Remaining:* a parse timeout on pathological files. Partial display already exists: large files are parsed by prefix, the rest in the background.

**6.5 Fuzzing and security.** Done on 21.09.2026.
- libFuzzer requires clang-cl, which the machine does not have, so the harness is our own: `app/tests/fuzz/fuzz.cpp` takes the
  corpus documents as seeds, mutates them (character replacement, cutting, duplication, splicing two seeds, long runs of
  special characters) and feeds them to the Markdown parser and the HTML tag parser. Every input is written to disk before parsing,
  so a crash can always be reproduced: `fastmd-fuzz.exe --file out\fuzz\last.md`.
- Build with `/fsanitize=address`: `pwsh -File app/tests/fuzz/run.ps1`. Runs on 21.09.2026: **20,000 inputs in 93 s** and **200,000 in 944 s**, both with ASan and without a single crash.
- The review of link and path handling was done back in 1.7/2.3: schemes by allowlist, `file:` and UNC only with
  confirmation, network https only (except loopback), no cookies and no authentication.
- **What turned up right away:** a parser hang on an HTML block with an unclosed tag (`<p align=center>` + a line
  ending in `<`). The tag walk stayed on that character and never moved on — the window did not appear at all.
  The minimal case is 33 characters, kept in `app/tests/fuzz/seed-broken-html.md`; fixed, and a check was added to the
  UI tests (`test_broken_html`).

**6.6 Low-end hardware.** Partially done on 21.09.2026: there is no real run on a low-end laptop — it needs another
computer. On this machine we measured what can be measured here.

| What was measured | How | Result |
|---|---|---|
| Start-up on four cores | `bench.py … --cpus 4` (a new switch: the process is started already pinned to N cores) | medium 65.4 ms, large 67.5 ms — against 63.5 and 67.1 ms on all 16. Start-up is not bound by parallelism: it is determined by creating the process and the window |
| Full-screen scrolling | `python app/tests/weak_check.py` (`--scroll-test`), a 3440×1400 window | frame 1.33 ms (median), 6.13 ms (95th percentile) with one core kept busy in the background |
| Memory on a large file | same, large.md 3.7 MB | working set 51 MB, peak 53 MB, commit 42 MB |

As a result, a second rendering backend (D2D WARP/HW) and the resident mode were not needed: the numbers do not call for them.
*Remaining:* a run on a laptop with an iGPU, 4 cores and a 4K / 150 % screen.

**6.7 Documentation.** Done on 21.09.2026: the project README was rewritten for the features of stages 2–6, with a privacy section added (what goes to the network and when — exactly two cases) and a table of third-party libraries with their licenses; `app/README.md` — keys and how the code is organized; `docs/PLAN.md` — this plan. At the same time: **the English README became the main one**, the Russian one moved to `README.ru.md` and `app/README.ru.md`, and each pair is linked by a language switcher on the first line; the root README gained a section on why the first launch after an install or update can take longer (the antivirus scans an unfamiliar exe inside `CreateProcessW`) and how to add the program's folder to the Defender exclusions. *Remaining:* there is no separate website and no GIF demo.

**6.8 Release 1.0.** Released on 21.09.2026: <https://github.com/beanbo/FastMD/releases/tag/v1.0.0>.
- In the release: `FastMD-1.0.0-win-x64.zip`, `FastMD-Setup.exe` and both `.sha256` files; the notes are `docs/RELEASE-1.0.0.md`.
- Final run before the release: the UI test (136 checks), the installer, the preview pane and thumbnails, the screen reader, 108 formulas and 30 diagrams, fuzzing with 200,000 inputs under ASan, a speed measurement (66.7 ms against 79.2 ms for the empty window).
- **1.1.0** was released the same day (our own Mermaid flowchart layout engine, full-width diagrams with sideways scrolling, a preview pane fix — see the CHANGELOG): <https://github.com/beanbo/FastMD/releases/tag/v1.1.0>.
- **1.1.1** was released on 23.09.2026 — a single fix: a crash when opening a document with an image (a worker thread's last `CoUninitialize` tore COM down under `SHAddToRecentDocs`; see the CHANGELOG): <https://github.com/beanbo/FastMD/releases/tag/v1.1.1>. For the first time the notes were in two languages, English first, as in the README.
- **1.2.0** was released on 23.09.2026 — task list boxes can be ticked: a click sets or clears the mark right in the file, changing the one character between the brackets in the file's own encoding, and only if the file on disk matches what is shown (`app/src/tasks.cpp`; see the CHANGELOG). The first and only place where FastMD writes to a document; the preview pane still only shows it: <https://github.com/beanbo/FastMD/releases/tag/v1.2.0>.
- **1.3.0** was released on 25.09.2026 — edit mode (7.6): a document is edited right on the rendered page, with a toolbar, source editors for formulas and diagrams, and saving that does not damage the file; and the fixed auto-update (5.6; see the CHANGELOG): <https://github.com/beanbo/FastMD/releases/tag/v1.3.0>.
- **1.3.1** was released on 03.10.2026 — keyboard layout switching in edit mode (Ctrl+Shift and Alt+Shift over the page), GitHub-style highlighting of HTML, YAML and shell (2.5) and the MIT license: <https://github.com/beanbo/FastMD/releases/tag/v1.3.1>.
- **MIT license** since 03.10.2026 (shipped in 1.3.1): the owner switched from GPL-3.0 to MIT so that the code can be taken into any project; the third-party libraries and Rust dependencies are under permissive licenses anyway. Releases up to and including 1.3.0 came out under GPL-3.0.
- *Left to the owner:* submit the winget manifest to `microsoft/winget-pkgs` (`packaging/winget/README.md`) and sign the code once there is a certificate.

---

### Stage 7 — after 1.0 (on request and by measurement)

| # | Task | When to do it |
|---|---|---|
| 7.1 | Resident mode + `DelegateExecute`: a double click without creating a process (≈86 → 35–40 ms) | if a cold double click on low-end hardware takes > 150 ms |
| 7.2 | Obsidian mode: `[[wikilinks]]`, note embedding, `==выделение==` (highlight) | if you will be reading Obsidian vaults |
| 7.3 | Custom themes (JSON palettes) and fonts | if wanted |
| 7.4 | Comparing two versions of a file (diff view) | if wanted |
| 7.5 | Cross-platform support | **not planned** (decision §0.4) |
| 7.6 | Edit mode: editing right on the rendered page | ✅ v1.3.0 |

**7.6 Edit mode.** Done on 25.09.2026, shipped in v1.3.0 (branch `claude/edit-mode`; design — [docs/EDIT-MODE.md](EDIT-MODE.md)).
- What was asked for was editing that **keeps the rendered look** rather than switching to a text editor: a double click puts
  the caret at that spot, a toolbar slides down from the top (paragraph style, formatting, lists, quote, code, table, formula,
  diagram, image, rule), the ✕ on the right and Esc leave. It can also be entered with F2, the pencil and "Edit here".
- The source of truth is the Markdown source: every edit is a replacement of a piece of the source, after which the document is parsed again, and
  what is shown is always equal to `ParseMarkdown(исходник)` (the source). The caret lives in the source, and the "text ↔ source" map is built
  by a small modification of md4c and only on entering edit mode: start-up and reading pay nothing for it.
- Formulas, diagrams, images, HTML blocks and front matter are edited as source in a pop-up editor (a real EDIT
  control on the input thread, with IME); the object on the page is redrawn as you type.
- Saving: autosave by default (0.8 s after an edit); only the changed part of the file is rewritten, in
  place, in its encoding, with its BOM and line ends; every write is verified by decoding it back. A recovery
  file around every write, a journal of unsaved edits, conflicts with other programs shown in a strip, with no
  modal windows; one file is edited by one window. The undo history lives as long as the document is open.
- Limitation: CJK input methods, the emoji panel and dictation do not work in the page itself (the window starts without IME for the sake of start-up
  speed) — only in the source editors. The Explorer preview pane still only shows the document.
- **Fixed on 03.10.2026 (shipped in 1.3.1):** the keyboard layout over the page did not switch with the shortcut from the settings
  (Ctrl+Shift or Alt+Shift; Win+Space worked) — it reaches the thread only through TSF, and `ImmDisableIME(0)` kept
  it out. Now the UI thread turns on its own TSF on a timer right
  after the first frame (`StartTextServices`, ≈5 ms); there is still no IME in the page.
- How it was done: a design of ~2,700 lines after five adversarial reviews, nine gated phases, a review of the whole feature
  at the end (67 findings, all addressed). Checks: 1.4 million checks of the core on golden cases and on a sweep of every caret
  position, fuzzing with edits, ~500 UI test checks, snapshots of all new elements at 100 and 150 %, in the light and dark
  themes (`app/tests/edit_shots.py`). Start-up and scrolling speed did not change (interleaved runs); the exe is
  1.44 MB.
- *Remaining after the first version:* the round trip of text cut and pasted back into the same place (three rare forms),
  a fast path for ticking a box in reading mode, laying out a code block in chunks and reusing the layout of table cells
  (typing in a 700-line code block takes ≈20 ms), a detailed formula error, the order in which the toolbar collapses in a narrow window;
  and in reading as well — the line height from which an inline image is moved to a line of its own (an extra ≈240 px above the text).

---

## 4. Cross-cutting tasks (alongside the stages)

| Task | When | Size |
|---|---|---|
| Engine unit tests (parser, slugs, selection, copying, search): a small test exe without frameworks | stage 1 | M |
| CI on GitHub Actions (windows-latest): Release/Debug build, unit tests, md4c spec tests, ASan; the UI test if the runner provides an interactive desktop | stages 1–2 | M |
| Nightly speed measurement on the work machine: a Task Scheduler task, launched through the Explorer context (`bench/tools/explorer-launch`), a history of results and a chart | stage 2 | S |
| ~~`CHANGELOG.md` and the version number from a single place (CMake → rc)~~ — done in v0.2.0 | — | — |
| ~~Splitting up `window.cpp`~~ — done in v0.2.0: find, toc, home, store, shell, settings_ui, strings | — | — |

---

## 5. Checklist for every release

- [ ] Speed measurement: FastMD ≤ the baseline on medium and ≤ +15 ms on large, the result saved in `bench/results/`.
- [ ] UI test: all checks green.
- [ ] Visual regression on the README corpus (from stage 2 on).
- [ ] Edit mode snapshots reviewed: `python -X utf8 app/tests/edit_shots.py` — the toolbar, pop-up menus, source editors and strips at 100 and 150 %, in the light and dark themes (`app/tests/out/edit-final/`).
- [ ] Manual check: double click from Explorer, a large file, the dark theme, 150 % DPI, a second monitor.
- [ ] `CHANGELOG.md` updated, version number bumped, commit, tag.
- [ ] `publish.ps1` for the working copy; a GitHub Release with a zip (since v0.2.0), and after stage 5 the installer.

---

## 6. Order and estimate

| Stage | Version | Essence | Estimate |
|---|---|---|---|
| 1 | v0.2 | ✅ comfortable reading: scrolling of wide blocks, table of contents, positions, search 2.0, recent documents, settings | done |
| 2 | v0.3 | GitHub parity: HTML, remote images, SVG, footnotes, Lexilla, visual regression | ~3 weeks |
| 3 | v0.4 | copying with formatting and as Markdown, keyboard selection, printing and PDF | ~1.5 weeks |
| 4 | v0.5 | formulas (RaTeX) and Mermaid (mmdr + a WebView2 fallback) | ~2 weeks |
| 5 | v0.6 | engine library, preview handler, installer, signing, auto-update, winget | ~2.5 weeks |
| 6 | v1.0 | accessibility, high contrast, localization, crashes, fuzzing, low-end hardware, documentation | ~2.5 weeks |

In total, about 11–12 weeks of an experienced developer's work to 1.0 after stage 1; faster with me.
Stages 3 and 4 can swap places if formulas and diagrams matter more than copying.
Next up is stage 2: HTML and network images break the look of most READMEs from GitHub.

---

## 7. Risks

| Risk | What we do |
|---|---|
| The HTML subset keeps growing | A strict list of tags (2.2). Everything else is text. The criterion is a corpus of 30 READMEs |
| The network (remote images) slows down start-up | Only after the first frame, strict timeouts, an on-disk cache, space reserved in advance |
| Rust dependencies (RaTeX, mmdr) complicate the build | Prebuilt static libs in the repository or a cargo build in CI. The fallback for diagrams is WebView2 |
| The UIA provider is laborious | A separate stage (6.1); we start with a minimal `TextPattern` |
| Antivirus false positives on an unsigned exe | Signing (5.5), builds only from CI |
| Speed regressions pile up bit by bit | The §0 budget, a nightly measurement with a chart, the rule "no commit may be slower" |
