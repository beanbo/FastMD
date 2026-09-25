# FastMD — instant Markdown viewing for Windows

English · [Русский](README.ru.md)

**~66 ms** from launch to a rendered document (medium.md, 53 KB). That is faster than an empty, untuned Win32 window: in the 1.0.0 control run — 66.7 ms against 80.1 ms; in the run after the Mermaid work (21.09.2026) — 76–79 ms against the baseline's 95–102 ms. The absolute number depends on what the machine is busy with that day, which is why runs are always interleaved and it is the gap that carries the meaning.
For comparison: Void opens its window in 1.4 s, VS Code in 1.1 s, Typora in 1.7 s (the details are in [../REPORT.md](../REPORT.md)).

The stack: **C++20, Win32, DirectWrite, md4c**.
- The first frame is drawn on the CPU (DirectWrite → GDI DIB), with no GPU device.
- Only the first screen is laid out at first.
- Everything else is done on background threads after the first frame.
- One small exe (≈1.4 MB with edit mode), static CRT, no runtimes.

## Building

You need Visual Studio 2022 or newer (any edition, or the Build Tools) with the "Desktop development with C++" workload: MSVC, the Windows SDK, CMake and Ninja all come with it. `tools/msvc.cmd` finds the installation through vswhere.

```
pwsh -File app/build.ps1            # dev build → app/build/Release/FastMD.exe (tests, measurements)
pwsh -File app/build.ps1 -Config Debug
pwsh -File app/publish.ps1          # refresh the working copy app/out/Release/FastMD.exe (the .md association points at it)
```

The version is set in one place — `project(FastMD VERSION …)` in `CMakeLists.txt`. From it `version.h` is generated for the exe resource and the settings window.

## Usage

```
FastMD.exe file.md
```

- **With no file** it opens a start screen with the recent documents. Type to find one by name, Enter opens it.
- **The `.md` association.** Pick the "Open .md files with FastMD…" context menu item, or run `FastMD.exe --register`. The program registers itself in HKCU, no administrator rights are needed, and Settings → Default apps → FastMD opens. Windows does not let a program assign itself, so the confirmation is yours to give. To undo the registration: `FastMD.exe --unregister`. Once registered, recent documents also appear in the taskbar Jump List.
- **Reading position.** The place is remembered for the last 200 documents. A document opened again glides back to it. If the file has changed, the place is found by the nearest heading.
- **Settings** (the gear icon at the top right, Ctrl+, or the context menu) apply at once and reach the other open windows:
  - theme, text font (Segoe UI, or the Sitka book font with optical sizes), text size;
  - column width, line wrapping in code, smooth scrolling;
  - the editor for Ctrl+E (VS Code, Cursor, Void, Notepad++, Sublime Text and Notepad are found automatically, or point it at your own exe);
  - interface language (Russian or English);
  - autosave of edits (on by default; see [Edit mode](#edit-mode)).

  Settings and the window position live in `HKCU\Software\FastMD`, reading positions and the recent list in `%LOCALAPPDATA%\FastMD\positions.bin`.
- `publish.ps1` can be run with documents open: the running exe is renamed (`FastMD.old-*.exe`, removed on the next publish), and new windows start the new version.

### Keys and mouse

| Action | Keys |
|---|---|
| Scrolling | wheel, ↑ ↓, PgUp / PgDn, Space / Shift+Space, Home / End |
| Wide code and tables sideways | Shift+wheel, touchpad, wheel tilt, or the slider at the bottom of the block |
| Selection | mouse; a triple click selects a paragraph or a cell (a double click starts editing there, see [Edit mode](#edit-mode)); Shift+click extends the selection |
| Selection from the keyboard | Shift+arrows (by character), Ctrl+Shift+arrows (by word), Shift+Home / End (to the end of the line), Ctrl+Shift+Home / End (to the ends of the document), Shift+PgUp / PgDn |
| Copy / select all | Ctrl+C / Ctrl+A |
| Copy as Markdown | Ctrl+Shift+C — the clipboard gets the source as the author wrote it |
| Drag out | drag the selected text, a link or an image into another application |
| Print / PDF | Ctrl+P — the system print dialog; Ctrl+Shift+P — save a PDF |
| Copy a code block | the button in the block's top right corner (it appears on hover) |
| Search | Ctrl+F; Enter / F3 — next, Shift+Enter / Shift+F3 — previous; Alt+C — case sensitive, Alt+W — whole word; Esc — close |
| Table of contents | Ctrl+Shift+O or the button at the top left |
| Links from the keyboard | Tab / Shift+Tab — next and previous, Enter — open |
| A heading's address | hover over a heading: a link icon appears to its left, and a click copies `file.md#anchor` |
| Links with the mouse | click: `#anchor` scrolls to the heading, `.md` opens right here, `http(s)` and `mailto` go to the browser, anything else after a confirmation; right click — "Open", "Copy link address" |
| Images | right click — "Copy image", "Open image file" |
| Task lists | a click on a box ticks it in the file (`- [ ]` ↔ `- [x]`); a press let go outside the box does nothing |
| Column width | Ctrl+Alt+← / → (narrow, normal, wide, full width) |
| Zoom | Ctrl+wheel, Ctrl + / Ctrl −, Ctrl+0 |
| Back / forward | Alt+← / Alt+→, Backspace (not in edit mode, where it deletes), the side mouse buttons |
| Open / refresh | Ctrl+O, dragging a file into the window / F5 or Ctrl+R (a change on disk refreshes by itself; in edit mode the edits are saved first) |
| Edit | F2, a double click on the text, the pencil icon at the top right, or "Edit here" in the menu — see [Edit mode](#edit-mode) |
| Open in an external editor | Ctrl+E (in edit mode the edits are saved first, then edit mode is left) |
| Settings | Ctrl+, or the gear icon at the top right |
| Close | Esc (when there is no selection, no search and no focused link; in edit mode Esc leaves edit mode instead, and a second Esc right after it does not close the window), Ctrl+W |
| Menu | right click: "Edit here", theme, zoom, column width, code wrapping, "Show in folder", settings, the association |

### Edit mode

The page keeps its look: the caret moves over the rendered text, and every change is written into the Markdown source (only the changed part of the file, in its own encoding and line ends). What is saved when, the recovery of an interrupted save and the limits are described in the [project README](../README.md#editing).

| Action | Keys |
|---|---|
| Enter / leave | F2, a double click on the text (the caret lands there), the pencil icon, "Edit here" in the menu / Esc, F2, the ✕ at the right end of the toolbar. Esc first closes what is open: a source editor, a popover, the search, the outline, a selected object |
| Move | arrows; Ctrl+← / → by word; Ctrl+↑ / ↓ by block; Home / End, Ctrl+Home / End; PgUp / PgDn. With Shift they select; Ctrl+A selects everything; a double click selects a word, a triple click a paragraph or a cell |
| Type | letters go in at the caret. Markdown at the start of a line becomes formatting: `# `, `- `, `1. `, `> `; ```` ``` ```` + Enter opens a code block, `$$` + Enter a formula block, `---` + Enter a rule |
| Delete | Backspace / Delete; Ctrl+Backspace / Ctrl+Delete by word. Next to a formula, picture, diagram or rule the first press selects it, the second deletes it; at the start of a heading, list item or quote Backspace takes the heading, marker or quote away |
| New lines | Enter: a new paragraph or list item (in a table: the cell below, a new row after the last one); Shift+Enter: a line break; Ctrl+Enter: a new paragraph after the block (after a table, code block or picture too) |
| Tab | in a list: nest the item / Shift+Tab: lift it out; in a table: the next / previous cell; in code: indent |
| Format | Ctrl+B bold, Ctrl+I italic, Ctrl+Shift+X strikethrough, Ctrl+` (Ctrl+Ё on the Russian layout) inline code, Ctrl+K a link; with no selection the format applies to what is typed next |
| Blocks | Ctrl+1…6 heading 1–6 (the same again: back to text); Ctrl+Shift+8 / 7 / 9 bulleted, numbered, task list; Ctrl+Shift+Q quote; Ctrl+Shift+K code block |
| Insert | Ctrl+T a 3×3 table (or any size from the toolbar's grid); Ctrl+M a formula in the line, Ctrl+Shift+M a formula block; the toolbar also inserts diagrams (nine templates), pictures (or drop a picture file into the window) and a horizontal rule |
| Objects | a click on a formula, diagram, picture, HTML block or front matter opens its source under it, and the page redraws it as you type; Ctrl+Enter keeps the change, Esc takes it back. Enter or F2 opens a selected object's source |
| Links | Ctrl+click opens a link; with the caret in a link a bubble offers "Edit", "Remove" and "Open" |
| Undo / save | Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z); Ctrl+S saves now (with autosave on this is rarely needed) |
| Clipboard | Ctrl+C / Ctrl+X / Ctrl+V; Ctrl+Shift+C copies as Markdown. A paste from FastMD keeps its formatting, from elsewhere it goes in as plain text |
| Menu | right click: undo, cut, copy, paste, the link and table actions, "Edit source…" on an object, save, "Save as…", "Finish editing" |
| Search, outline, zoom | Ctrl+F, Ctrl+Shift+O, Ctrl+wheel work as in reading mode; the toolbar's first button opens the outline |

## What is supported

- **Markup:**
  - CommonMark + GFM: tables with alignment, task lists, strikethrough, autolinks;
  - footnotes: `[^1]` in the text links to the definition, the definitions are gathered at the bottom of the document and link back;
  - emoji shortcodes `:rocket:` (the gemoji base, 1913 names);
  - formulas `$…$` and `$$…$$` — genuine typesetting (RaTeX, KaTeX-compatible): fractions, roots, sums, matrices, cases;
  - Mermaid diagrams: a ```` ```mermaid ```` block is drawn as a diagram;
  - headings with anchors by GitHub's rules;
  - nested lists and quotes;
  - GitHub Alerts `> [!NOTE]` / `[!TIP]` / `[!IMPORTANT]` / `[!WARNING]` / `[!CAUTION]`;
  - YAML front matter: a plain `key: value` is shown as a property table, a nested one as a YAML block;
  - a subset of HTML: alignment, images inline (rows of badges) and as blocks, `<details>`, `<picture>` following the theme, tables, `<kbd>`, `<sub>`/`<sup>`, links and anchors, headings.
- **Code:** syntax highlighting for 55 languages (163 spellings of the name: c, cpp, cs, js, ts, rust, python, go, java, kotlin, swift, php, ruby, perl, lua, r, haskell, elixir, erlang, clojure, julia, nim, zig, dart, scala, solidity, objective-c, fortran, pascal, ocaml, f#, vb, asm, sql, css, html, yaml, toml, dockerfile, makefile, cmake, terraform, nix, graphql, protobuf, glsl, nginx, bash, powershell, batch, vim, tex, matlab, prolog, ada and their synonyms). The language name is written in the corner of the block. Wide code blocks and tables reach past the text column, and whatever still does not fit scrolls sideways.
- **Images and fonts:**
  - SVG (badges, diagrams): drawn by the lunasvg library out of `fastmd-svg.dll`, which is loaded only when an SVG is met. The picture is redrawn at the size it needs rather than stretched;
  - images from the network: downloaded after the first frame (https only, except localhost), put into the `%LOCALAPPDATA%\FastMD\cache` cache (200 MB, evicted by age) and appearing as they become ready. In the settings — "Pictures from the web: Always / Ask / Never";
  - local images (PNG/JPEG/GIF/BMP/WebP through WIC, with room reserved for them in advance). An image whose size on screen does not match the file is downscaled with a filter: a copy at the right size is prepared in the background and redone when the zoom or the column width changes;
  - color emoji, CJK, Cyrillic.
- **Appearance:**
  - GitHub light and dark themes, following the system or set by hand, with no white flash at startup;
  - a dark title bar and Mica in the window title.
- **Large files:** a 3.7 MB document shows its first screen in the same ~66 ms. For it only the beginning of the file is parsed, and the rest is read in the background.

## Known limitations

- HTML: a subset is supported (alignment, images inline and as blocks, `<details>`, `<picture>`, tables, `<kbd>`, `<sub>`/`<sup>`, links, headings). Unknown tags are dropped and their text kept; `colspan`/`rowspan` and CSS are not supported.
- Animated GIFs are shown as their first frame.
- There is no print preview window of our own.
- A `sequenceDiagram` with activation (`A->>+B`) is not drawn yet — its source is shown instead.
- Edit mode: CJK input methods, the emoji panel and dictation work only in the source editors of objects (the document window runs without IME, for its startup speed); the Explorer preview pane and thumbnails only show.

The detailed plan of every remaining step is in [../docs/PLAN.md](../docs/PLAN.md), the history of changes in [../CHANGELOG.md](../CHANGELOG.md).

## How it is put together

| Module | What it does |
|---|---|
| `src/window.cpp` | the window, input routing, hotkeys, commands, the context menu, applying settings, the window position, test queries, `wWinMain` |
| `src/view.cpp` | columns and their presets, moving wide blocks out and scrolling them sideways, virtualized layout, drawing (on a scroll — only the new strip), hit-testing, selection, link focus |
| `src/find.cpp` | search (case, whole words), the search panel, the marks on the scrollbar, an input box with IME on a thread of its own |
| `src/toc.cpp` | the table of contents: beside the text or over it |
| `src/home.cpp` | the start screen with the recent documents |
| `src/uia.cpp` | a UI Automation provider over the document: Narrator and NVDA see the window as a Document with a Text pattern, move by characters, words, lines and paragraphs and jump between headings. Answered straight from the model, so there is no second copy of the document; `uiautomationcore.dll` is delay-loaded and only a reader who actually runs one pays for it |
| `src/loader.cpp` | loading a document (a starting thread in parallel with creating the window; opening, reloading, history), measuring heights in the background, WIC and screen-size copies of images, watching the file, reading positions |
| `src/store.cpp` | settings in the registry, `positions.bin` (positions and the recent list, shared by all windows), finding editors |
| `src/shell.cpp` | links, the clipboard (text, images), the editor, Explorer, dialogs, the .md association |
| `src/copy.cpp` | copying the selection: HTML and RTF with formatting, the Markdown source through the `Doc::srcMap` map |
| `src/drag.cpp` | dragging out: a data object with text, a link or an image (COM only for the duration of the drag) |
| `src/tasks.cpp` | ticking a task box in the file: one character between the brackets, overwritten in place in the file's own encoding, and only while the file on disk decodes to exactly the text on screen; the file watcher does not reload for this write |
| `src/edit.cpp` | edit mode's glue: entering and leaving, input, the model swap after every change, autosave and its retries, conflicts with other programs, the journal of unsaved edits, the questions before leaving a document |
| `src/editcore.cpp` | edit mode's core, window-free: where a caret may stand, a position in the text ↔ an offset in the source, typing, Enter, Backspace and Delete, selections, the clipboard, undo |
| `src/editops.cpp` | the formatting commands and inserts: bold and the other inline formats, headings, lists, quotes, code blocks, tables, formulas, diagrams, pictures, rules |
| `src/editfile.cpp` | edit mode's file half: the encoding checks, the byte-exact write of only the changed part, the recovery file around it |
| `src/editbar.cpp` | the toolbar, its popovers, the source editors' panels, the strips that say what is wrong with the file, the pencil button |
| `src/editpop.cpp` | the source editors of formulas, diagrams, pictures, HTML and front matter (real EDIT controls on the input thread, with IME) and the worker that redraws the object as you type |
| `src/print.cpp` | printing and PDF export: layout for paper, splitting into pages, headers and footers |
| `src/canvas_print.cpp` | a canvas over the printer DC: glyphs go to GDI, so text stays text in the PDF |
| `src/settings_ui.cpp` | the settings window, drawn by the same engine, and the gear icon that opens it |
| `src/strings.cpp` | the interface strings in Russian and English |
| `src/parse.cpp` | md4c (master, at a pinned commit) → a flat block model; footnotes linked both ways, alerts, front matter, heading slugs, emoji shortcodes |
| `src/html.cpp` | parsing the tags and entities of raw HTML (the mini-DOM lives in `parse.cpp`) |
| `src/highlight.cpp` | highlighting inside fenced code blocks: a single-pass highlighter with no grammar (comments, strings, numbers, keywords, capitalized types, calls, `$vars`) in GitHub's light colours |
| `src/net.cpp` | downloading images from the network through WinHTTP, and the disk cache |
| `src/svg.cpp` | the lazy load of `fastmd-svg.dll` and the calls into it |
| `src/formulas.cpp` | the lazy load of `fastmd-tex.dll` and `fastmd-mermaid.dll`: formulas and diagrams → SVG |
| `src/update.cpp` | the once-a-day update check, downloading the installer and verifying its SHA-256 |
| `src/crash.cpp` | a minidump on a crash, and an offer to open the folder on the next launch |
| `preview/preview.cpp` | the preview pane and thumbnails in Explorer: the same engine in a COM DLL |
| `setup/setup.cpp` | the installer: it carries the program inside itself and installs it for one user |
| `rust/fastmd-tex` | formula typesetting (RaTeX), a separate library in Rust |
| `rust/fastmd-mermaid` | Mermaid diagrams, a separate library in Rust |
| `src/svg_dll.cpp` | the library itself: lunasvg + plutovg, a separate binary so that the exe stays small |
| `src/layout.cpp` | typography (Segoe UI Variable Text/Display or Sitka Small…Banner by optical size, Cascadia Mono) and block layout |
| `src/canvas_gdi.cpp` | the CPU renderer: DirectWrite → DIB (a buffer taller than the window, so that scrolling does not copy pixels), an SDF rasterizer for shapes, layered COLR emoji, per-pixel clipping of text |
| `src/theme.cpp` | the GitHub light/dark palettes |
| `src/util.cpp` | the bench protocol hooks, tracing (`FASTMD_TRACE=1` → `%TEMP%\fastmd-trace.txt`), files |

The document model is a single UTF-16 text buffer. Every block and every table cell owns a range in that buffer. A position in the text is therefore a single number, and selection and search work the same way through blocks and cells.

Everything added since stage 1 stands off the critical path of the first frame. The search box, reading `positions.bin`, the settings window and finding editors are all created after the first frame or on demand. The icon buttons (table of contents, settings) appear from the second frame on: the icon font is loaded after the first one. The SVG, formula and diagram libraries are mapped on a worker thread, and only for a document that actually needs them; `uiautomationcore.dll` only when a screen reader is running. The exception is the start screen with no document: its list is read as part of the launch.

## Tests

- **Speed** is checked by the same harness that was used to choose the stack. The `fastmd-app` variant is registered in `bench/protos/fastmd-app`:
  ```
  python bench/harness/bench.py run fastmd-app,baseline-win32 --doc small,medium,large --runs 12
  ```
  The rule: FastMD is no slower than `baseline-win32` on medium, and no more than +15 ms behind on large.
- **The UI test** starts real windows and checks about 500 things (including a local http server for images from the network and SVG): selection, copying, search, links, zoom, the theme, history, live reload, the window position, the settings icon, screen-size copies of images, the partial scroll frame matching a full redraw, footnotes and callouts, every stage 1 task against its readiness criterion from the plan, and edit mode — typing in four encodings, the toolbar's commands, the source editors, saving, conflicts, recovery. State is read through `WM_APP_QUERY`. The test works in a profile of its own (`FASTMD_REGKEY`, `FASTMD_DATA`) and does not touch your settings. Screenshots are saved to `app/tests/out/`; `FASTMD_ONLY=name,name` runs a part of it.
  ```
  python -X utf8 app/tests/ui_smoke.py
  ```
- **Edit mode's core** has tests of its own, with no window: golden cases (Markdown before, a key or a command, Markdown after — each also with CRLF, in a quote and in a list), a walk over every caret stop of the test documents, the undo stack and the byte-exact save. The fuzzer (`tests/fuzz/run.ps1`) also drives random edits through the core.
  ```
  pwsh -File app/tests/edit/run.ps1
  ```
- **Edit mode's pictures:** `app/tests/edit_shots.py` photographs the toolbar, every popover, source editor and strip, at 100 % and 150 %, light and dark, into `app/tests/out/edit-final/`, for a look before a release.
- **Real READMEs.** A corpus of 15 popular repositories (the list is in `app/tests/readmes.txt`) is downloaded, opened one by one and compared against an accepted picture: it shows when an edit has unexpectedly changed how real documents look. `--update` accepts the current look as the reference.
  ```
  python app/tests/readme_check.py
  ```
