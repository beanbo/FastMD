[Русский](CHANGELOG.md) | **English**

# FastMD changelog

Format: [version] — date — what changed. Plans are in [docs/PLAN.md](docs/PLAN.en.md).

## [1.3.1] — 03.10.2026

[Release on GitHub](https://github.com/beanbo/FastMD/releases/tag/v1.3.1): zip, installer and hashes. The keyboard
layout in edit mode, HTML, YAML and shell highlighting as on GitHub, and the MIT license in place of GPL-3.0.

- **Fixed: the keyboard layout did not switch in edit mode.** The shortcut from the Windows language settings (Ctrl+Shift or
  Alt+Shift) did nothing over the page, and text kept being typed in the previous language; only Win+Space worked. The cause
  was a startup-speed trick: the window's UI thread starts without IME (`ImmDisableIME(0)` saves 12–35 ms before the first frame),
  and that shortcut reaches the thread only through the Windows text services (TSF). Now the thread turns TSF on itself, on a
  timer right after the first frame: ≈5 ms that startup never sees — the first frame still does without IME.
  Verified with real key presses: in 1.3.0, three presses of the "F" key with Ctrl+Shift between them typed «ааа», now they type
  «аfа»; Win+Space gives «аfа» in both versions. CJK input methods, the emoji panel and dictation still do not work in the
  page itself (they do work in the search box and in the source editors).
- **MIT license in place of GPL-3.0.** FastMD's code can be taken into any project, closed and commercial ones included:
  it is enough to keep the copyright notice and the license text. Releases up to and including 1.3.0 came out under
  GPL-3.0. The third-party libraries (md4c, lunasvg, plutovg, RaTeX, mermaid-rs-renderer and the Rust dependencies) were already
  under MIT, Apache-2.0 and other permissive licenses, so nothing else had to change.
- **Fixed: HTML highlighting colored text as code.** In a ```` ```html ```` block (say, one with a Telegram message
  signature) every capitalized word between tags became an orange "type", and years and numbers turned blue: HTML had
  no parsing of its own, and the text went through the generic rules for code. Now HTML, XML, SVG and XAML have their own small
  parser, and the colors are as on GitHub: the tag name (a new color — green in the dark theme, blue in the light one), attributes,
  values, comments, CDATA and entities such as `&amp;`, while the text between tags stays plain. The content of
  `<script>` and `<style>` is highlighted as JavaScript (JSON for `application/ld+json` and import maps) and CSS.
- **YAML as on GitHub.** Keys take the tag color; values are strings, except numbers, `true`/`false`, `yes`/`no`, `null`
  and `~` (constants); comments, anchors and aliases (`&base`, `*defaults`), tags (`!!str`), `[…]` and `{…}` lists with
  keys inside, `|` and `>` blocks on one line. Previously capitalized words in values also became "types",
  and a `#` inside a URL became a comment. Front matter that does not fold into a table is drawn the same way.
- **Shell and config files.** In bash/sh/zsh, bat/cmd, Dockerfile, Makefile, CMake, INI/TOML, nginx/apache/.conf/.env and TeX
  a capitalized word is no longer taken for a type, and a word followed by a space and a parenthesis is no longer taken for a
  function call. Apache directives (`RewriteEngine`, `Directory`…) are highlighted as keywords: their list used to fail to
  match because of case.
- How it was verified. All 1,698 code blocks from the repository's documents (C, C++, C#, JS, Rust, Python, PowerShell, JSON)
  are highlighted exactly as before the change. The new parsers went through 500 thousand random inputs under AddressSanitizer,
  the parser fuzzer through 20,000 documents with HTML and YAML plus another 4,000 with edits, and the edit-mode tests ran 1,437,943 checks
  without errors. `FastMD.exe` grew by 8.5 KB.

## [1.3.0] — 25.09.2026

[Release on GitHub](https://github.com/beanbo/FastMD/releases/tag/v1.3.0): zip, installer and hashes. Edit mode —
the document is edited right on the rendered page — and a repaired auto-update.

### Edit mode

- **Editing where you read.** A double click on the text (or F2, the pencil at the top right, "Edit here" in the
  menu) puts a caret at that spot, and a toolbar slides down from the top; Esc, F2 or ✕ on the toolbar return to
  reading. This is not a switch to a text editor: the page keeps its look, the caret moves over the rendered text,
  and every edit is written into the Markdown source, after which the document is parsed again. Markdown typed at the start of
  a line (`# `, `- `, `> `, ```` ``` ````, `---`) becomes formatting.
- **The toolbar:** undo and redo, the paragraph style (text, headings 1–6), bold, italic, strikethrough, inline
  code, link, three kinds of lists, quote, code block, table (a size grid and actions on rows, columns and
  alignment), formula, diagram (nine templates), image (file picker or drag and drop), horizontal rule
  and the save status. In a narrow window the buttons go under "…". The commands have keys (Ctrl+B / I / K, Ctrl+1…6,
  Ctrl+Shift+7 / 8 / 9, Ctrl+T, Ctrl+M…), and the tooltip of a grayed-out button explains why it is unavailable.
- **Formulas, diagrams, images, HTML blocks and front matter** are edited as source in a small editor under the object
  (IME works there), and the object is redrawn as you type. A broken formula stays as the last good picture
  in a red frame, and the editor says it has an error.
- **Saving breaks nothing.** Edits are saved by themselves 0.8 s after the last one (this can be turned off in settings:
  "Autosave edits") and always when you leave edit mode, the document or the program. Only the changed part of the file
  is rewritten, in place: the encoding (UTF-8 with or without a BOM, UTF-16 LE, ANSI), the BOM, the line ends and the other
  bytes stay as they were, and every write is checked beforehand by decoding it back. A character the file's encoding
  does not have is not written: a strip at the top offers UTF-8 or removing the character. A file that cannot be written
  back exactly is not opened in edit mode.
- **Changes from elsewhere and failures.** If another program changed the file and nothing is unsaved, the new version is taken in
  (Ctrl+Z brings back the previous one); if something is unsaved, a strip asks whose version stays, with no modal dialogs. A read-only
  file can be edited and saved under another name. Before each write the bytes being replaced are copied to
  `%LOCALAPPDATA%\FastMD\recovery`; while saving is impossible, edits go to a journal in the same place after 3 s; an interrupted
  save or the journal is offered the next time the file is opened. One file is edited in one window.
- **Undo** works while the document is open — across saves and across leaving and re-entering edit mode.
- Limitations: CJK input methods, the emoji panel and dictation work only in the source editors (the document window runs
  without IME for startup speed); the Explorer preview pane and thumbnails are still read-only.
- Startup did not get slower: the text ↔ source map is built only on entering edit mode, the editing code does not
  run before the first frame, and startup and scrolling speed were checked at every stage with interleaved runs. The exe
  grew from 0.99 to 1.44 MB.
- How it was verified. The editing core, without a window: 1.4 million checks on golden cases (Markdown before, a key or command,
  Markdown after — each also with CRLF, in a quote and in a list) and on a sweep over every caret position; fuzzing with edits under ASan.
  The UI test grew to ~500 checks: typing in four encodings, every toolbar command, the source editors, write failures,
  conflicts, recovery, two windows, Windows session end. Screenshots of every new element at 100 and 150 %, in the
  light and dark themes (`app/tests/edit_shots.py`). Before the release the feature was reviewed from five angles (data
  safety, Markdown, usability, architecture, regressions): 67 findings, all addressed.

### Updates

- **Fixed: the installed version did not offer to update.** 1.1.0 never showed 1.2.0: the version it found
  lived only in a pop-up line for 4 seconds and in the menu of one window, and the "checked today" mark was written before
  the request — a failed request put the next check off by a day. Now the version found is remembered and visible in
  every window: a dot on the gear, an "Update" row in settings with a single button — check now, update to
  X, restart. A failed check is retried after an hour, a window left open for days checks once a day, and after a
  silent install FastMD offers to restart at the same place in the document. Opening a document no longer waits for
  the update threads. Installed 1.1.0 and 1.2.0 still check the old way and may not show the new version: it is
  safer to install it once by hand.

### Other

- Fixed a rare crash during PDF export (it was in 1.2.0 too): the page layout is now freed while the print job
  is still open, rather than after it is closed.
- An image redrawn on disk is updated in the open document even if the .md itself was rewritten without changes.
- The settings window opens within the screen's work area rather than partly beyond its edge.

## [1.2.0] — 23.09.2026

[Release on GitHub](https://github.com/beanbo/FastMD/releases/tag/v1.2.0): zip, installer and hashes. Task list
checkboxes can be clicked — the tick goes right into the file.

- **Task list checkboxes can be clicked.** Until now the `- [ ]` box was just a drawing: clicking it did
  nothing. Now a click puts the tick into the file itself (`- [x]`), and a second click removes it, as on GitHub, in Obsidian and
  Typora. Exactly one character between the brackets changes in the file — in place and in the file's encoding (UTF-8 with or without a BOM,
  UTF-16, ANSI); the line ends, the BOM, the rest of the text and the file itself stay as they were. FastMD writes only if the file
  on disk matches what is on screen byte for byte: if another program has changed it in the meantime, the document
  is re-read and the tick is not set. A read-only file, or one locked by another program, is not touched — a pop-up line
  says so. FastMD's own write does not re-read the document; images and layout stay in place. The checkbox
  behaves like a button: the cursor over it is a hand, an empty box is highlighted, and a press released outside the checkbox
  changes nothing. This is the only case where FastMD writes to a document; the Explorer preview pane is still
  read-only.
- How it was verified. The UI test (+22 checks, 164 in total) clicks checkboxes in documents in four encodings and compares the file
  with the expected one byte by byte; separately it covers a press released outside the checkbox, a read-only file, a file locked by another
  program, and a file changed on disk. Startup speed did not change (interleaved run, medians): medium
  60.0 ms against 60.7 for the build before the change and 67.8 for the empty window, large 59.9 against 63.4 and 71.9.

## [1.1.1] — 23.09.2026

[Release on GitHub](https://github.com/beanbo/FastMD/releases/tag/v1.1.1): zip, installer and hashes. One fix
after the 1.1.0 release — a crash when opening a document with an image.

- **Fixed: a crash when opening a document with an image.** The window managed to show the first screen, the image's place
  stayed empty, the interface froze for a couple of seconds, and the process silently closed (`0xC0000005`, `0xC000041D`,
  here `0xC0000409`). Documents without images never crashed. The cause was COM: it was initialized only by the
  background threads that decode and scale images through WIC, and not by the window thread, for startup speed.
  So the image thread's `CoUninitialize` turned out to be the last one in the process and tore COM down entirely — right under
  `SHAddToRecentDocs`, which the window thread was running at that moment, and the shell call kept working with already
  freed memory (on the machine where the crash was analyzed in a debugger, with the `urlmon` critical section). Now the window thread
  takes an apartment (STA) for itself right after the first frame, before background work starts, and never lets it go.
  The first frame is not affected; the call itself costs ≈2 ms, and `SHAddToRecentDocs` inside an apartment runs more than twice
  as fast: 11 ms against 28 (medians over a hundred launches).
- How it was verified. On this machine the race does not hit the dangerous spot by itself: the image thread finishes at the 3rd–5th
  millisecond of the shell call, while the dangerous moment is around the 9th (before the fix, 0 crashes in 35 launches, including
  on the README itself). So the check used a diagnostic build — it did not go into the repository — that
  holds back the image thread's `CoUninitialize` and releases it inside `SHAddToRecentDocs` after a set delay of
  0–30 ms: before the fix, 6 crashes in 99 launches (all fail-fast `0xC0000409`, no minidump), after it 0 in 99, and in
  each the image is drawn and the window responds. All launches were from the Explorer context, outside MSIX virtualization
  (research/05, §11.1). The build with the fix passes the UI test and the Narrator check.

## [1.1.0] — 21.09.2026

[Release on GitHub](https://github.com/beanbo/FastMD/releases/tag/v1.1.0): zip, installer and hashes. Mermaid diagrams
and fixes in the Explorer preview pane — everything that turned up after the 1.0.0 release.

### Mermaid diagrams are drawn in a new way

- **Our own diagram layout engine.** `graph` / `flowchart` (the most common kind of diagram) is no longer laid out by a third-party
  library: its nested subgraphs overlapped each other, nodes ended up outside their frame, and the canvas swelled
  several times over. Now it is the classic layered layout, as in dot and dagre, and as Mermaid itself does it: each
  subgraph is laid out separately and goes to its parent as one box (`app/rust/fastmd-mermaid/src/flow/`).
  The other 22 kinds of diagrams are still left to the library.
- **A real architecture diagram** (32 nodes, 5 subgraphs, nesting, arrows into a subgraph) took 4246×5308 —
  beyond the rasterization limit, so it got squashed as well. It became 2404×1718: a 5.5 times smaller canvas, the labels
  are readable, the frames do not cross.
- **Hundreds of times faster:** the same diagram — 623 ms against 2.8 ms, a simple one — 17 ms against 0.06 ms.
- Label width is measured with the very font file that `fastmd-svg.dll` then draws with (Segoe UI), so text
  no longer sticks out of its frame or gets cut off.
- The order of nodes in a row follows the source, not the alphabet.
- **Lines no longer go up and down.** Two arrows between the same pair of nodes were spread apart at the middle of the
  segment between their *centers* — but a subgraph's center lies deep inside its frame, and an arrow leaving the frame first
  went up to that middle and only then went down. Now only the arrows that, after being clipped to the frames, really would
  lie on top of each other are spread apart, and they are spread along the visible segment.
- An arrow leaving a subgraph looks for an exit point near its node and, if that point is taken by a neighbor, moves
  toward the target rather than into the nearest corner of the frame.

### A diagram at full width, scrolling sideways

- A diagram takes the full width of the window between the margins, not the text column plus an allowance: the more is visible at once, the
  less there is to scroll.
- A diagram wider than the window no longer shrinks into illegibility: it is drawn at its own size, and the block scrolls
  sideways — Shift+wheel, the touchpad's horizontal wheel, dragging the scrollbar, as with a wide table or a code block.
  On paper and in the Explorer pane there is no scrolling, so there the diagram still shrinks to fit the width.
- An image larger than 4096 pixels on a side is scaled down as a whole, not each side separately (it used to be squashed).

### The Explorer preview pane

- **The preview pane had no images, formulas or diagrams.** The libraries `fastmd-svg.dll`, `fastmd-tex.dll` and `fastmd-mermaid.dll` were looked for next to the process, but the pane's host is Explorer's `prevhost.exe`, not FastMD. Now they are looked for next to the library that calls them, so they are found both in the pane and in the window.
- **The pane prepared nothing in the background** — it has no background threads, and images and formulas stayed empty frames. Now it draws them right when the document loads: 6 ms for a document without formulas, 40 ms for the first document with a formula (the typesetting library is loaded), then single milliseconds again.
- Both checks were added to `app/tests/preview_check.py`.

## [1.0.0] — 21.09.2026 — "The product"

[Release on GitHub](https://github.com/beanbo/FastMD/releases/tag/v1.0.0): zip, installer and hashes.

Stages 2–6 of the plan. Speed did not change: 66.7 ms to the first screen against 80.1 ms for an empty Win32 window in the same
run. Everything new — formulas, diagrams, printing, the preview pane, updates — lives off the critical path of the first
frame and is loaded only when it is needed.

Not done for 1.0 and honestly left out of the release: **code signing** (it needs a certificate — SmartScreen still
warns on the first run) and **a run on weak hardware** (it needs a laptop with an iGPU and 4 cores; on this machine
a process limited to four cores was measured, see docs/PLAN.md, 6.6).

- **Fixed: a document with an unclosed HTML tag no longer hangs the program.** A block like `<p align="center">` with a line `<i>текст<` at the end made the parser spin on that `<` forever — the window never appeared at all. Fuzzing (`app/tests/fuzz`) found it, the minimal case is 33 characters; now such a `<` is shown as plain text, the way a browser does it. The case was added to the UI tests and to the fuzzer seeds.
- **Screen reader support.** The window now presents itself to the system as a document with the UI Automation Text pattern: Narrator and NVDA read it in full, move by character, word, line and paragraph, jump by heading (the level is exposed as a style attribute), follow the selection and highlight what has been read. The accessibility library is loaded only when a screen reader is actually running, so startup speed is not affected.
- **A preview pane in Explorer.** Select a .md file in Explorer, and the document appears in the preview pane just as it does in the program itself: the same parsing, the same layout, the same highlighting. Measured on this machine: **10–19 ms** to a drawn document (PowerToys takes 0.6–1.4 s). The pane only reads: it does not go to the network and does not touch settings. The wheel and the arrow keys scroll it.
- **.md thumbnails in Explorer.** In place of a file icon, a page of the document: headings, tables, code. A thumbnail is drawn in 1.5–2.7 ms and always on the light palette, like real paper.
- **Updates.** Once a day after the first frame FastMD asks GitHub, with one request, whether there is a newer version, and if there is, shows "Version X is available" and an "Update FastMD…" menu item. Nothing further happens without consent: the program shows the version numbers, downloads the installer and **verifies its SHA-256 against the file from the same release** — and only then runs it. Nothing about the user is sent. The check can be turned off in settings.
- **Installer.** `FastMD-Setup.exe` installs the program for one user into `%LOCALAPPDATA%\Programs\FastMD`: no administrator rights, no MSI or MSIX, with a Start menu shortcut and an entry in Settings → Apps, from which it can be uninstalled. The installer carries the program inside itself (the exe, three libraries and the licenses) and supports `/S` (silent), `/noassoc` (leave the .md association alone) and `/uninstall`. Uninstalling takes the .md association away only if it pointed at the copy being removed.
- **Crash report.** If the program crashes, it writes a minidump to `%LOCALAPPDATA%\FastMD\crashes` — and nothing more: no network, no automatic sending. On the next launch FastMD offers once to open the folder with the report. The ten most recent files are kept. The handler is installed with a single call and no library at all, so it does not affect startup speed.
- **High contrast.** If a high-contrast theme is on in Windows, the palette is taken from the system colors and syntax highlighting is turned off: in this mode contrast matters, not looks. A change of mode is picked up on the fly.
- **Formulas.** `$E = mc^2$` and `$$…$$` are now genuinely typeset: fractions, roots, sums with limits, matrices, systems of equations, Greek letters, arrows and accents — as in KaTeX. Typesetting is done by RaTeX (Rust) in the `fastmd-tex.dll` library, which is loaded only for a document with formulas. An inline formula sits on the text baseline, a formula in its own paragraph forms a separate block. The color follows the theme. A formula the engine did not understand is shown as source, the way the author wrote it. Tested on a set of 108 formulas (`python app/tests/math_check.py`).
- **Mermaid diagrams.** A ```mermaid``` block is drawn as a diagram rather than as code: flowcharts, sequences, classes, states, ER, Gantt, pie charts, mindmap, timeline and a dozen more kinds — 29 of the 30 tested. It is drawn by `fastmd-mermaid.dll` (Rust, no browser or WebView2); the diagram's background is the page background, and the dark theme has its own palette. The first diagram takes ≈20 ms in the background after the first frame, the following ones fractions of a millisecond.
- **Printing and PDF export.** Ctrl+P opens the system print dialog, Ctrl+Shift+P (or the "Export to PDF…" menu item) saves a PDF. The document is laid out again for the paper width by the same engine that draws the window; pages are cut at block boundaries, and a block longer than a page is cut at lines of text or table rows. The file name is at the top of each page, the page number at the bottom. Text goes to the printer as text, not as a picture: fonts are embedded in the job, the text in a PDF can be searched and copied, and emoji stay in color. The dark theme is not carried over to paper.
- **Dragging out.** Selected text (with formatting — HTML and RTF), a link (as an address, like from a browser) and an image (as a picture, and if it is on disk, as the file itself) can be dragged into another application. COM is brought up only at the moment of the first drag and never gets onto the startup path.
- **Copying with formatting.** Besides plain text, the clipboard now receives HTML (`CF_HTML`) and RTF: pasting into Word, Outlook or an email keeps headings, bold and italic text, strikethrough, links, tables, lists and code. Programs that do not understand formatting get the same text as before.
- **Copy as Markdown** — Ctrl+Shift+C or the menu item. The clipboard receives the source exactly as the author wrote it, not markup rebuilt from scratch: the parser remembers which place in the file each piece of text came from, and the selection is expanded to line boundaries.
- **Keyboard selection.** Shift+arrows select by character, Ctrl+Shift+arrows by word, Shift+Home/End to the edge of the line, Ctrl+Shift+Home/End to the start and end of the document, Shift+PageUp/PageDown by a screen. The caret is visible while the selection is being made from the keyboard and disappears on the mouse and on Esc; when moving up and down it keeps the column it started from, as in text editors.
- **Badges in Markdown.** A line like `[![build](badge.svg)](ссылка)` now shows the image rather than the alt text: a row of badges at the top of a README looks as it does on GitHub.
- **Code highlighting: 55 languages instead of 17.** Added php, ruby, perl, lua, r, haskell, elixir, erlang, clojure, julia, nim, zig, solidity, objective-c, fortran, pascal, ocaml, f#, vb, asm, batch, awk, tex, matlab, prolog, crystal, d, glsl, dockerfile, makefile, cmake, terraform, nix, graphql, protobuf, nginx, vim, ada and others — 163 spellings of language names in total. The corner of a code block now shows the language name (including "mermaid" for diagrams).
- **SVG.** Vector images and badges are now drawn rather than replaced with a placeholder: this is done by lunasvg and plutovg inside a separate library, `fastmd-svg.dll`. It sits next to the exe and is loaded only when a document contains SVG, so the exe size and startup time do not change. An image of any size is redrawn from the vector, so it stays sharp rather than stretched.
- **Images from the network.** Links to images on the internet no longer stay placeholders: the files are downloaded in the background after the first frame and appear as they become ready. What has been downloaded is kept in the cache `%LOCALAPPDATA%\FastMD\cache` (up to 200 MB, the excess is evicted by age), so the second opening of a document no longer goes to the network. Https only (the exception is localhost, for local previews), a 10-second timeout, a limit of 16 MB per image, no cookies and no authentication. Settings gained an "Images from the network" row: always, ask, or never.
- **A subset of HTML.** READMEs with HTML now look as they do on GitHub:
  - `<p align="center">` centers logos, badges and lines, and nested tags inherit the alignment;
  - inline images: a row of badges stays on one line, and an icon can be put right into a sentence; `<img width=…>` is shown at the given size;
  - `<details>` collapses: the content is hidden until the summary is clicked, and `open` expands it right away;
  - `<picture>` picks the image for the theme (`prefers-color-scheme`), and the document is re-read when the theme changes;
  - HTML tables `<table>/<tr>/<td>/<th>` are drawn as ordinary tables;
  - `<kbd>` (a key in a frame), `<sub>`, `<sup>`, `<b>`, `<i>`, `<code>`, `<del>`, `<a href>`, `<br>`, `<hr>`, `<a name>` anchors and `<h1>`–`<h6>` headings (they go into the table of contents) work.
  - The content of `<script>`, `<style>` and `<iframe>` is not shown and is never executed.
- **Front matter is shown as a table.** Simple `key: value` properties at the start of the file became a table with highlighted keys. Nested YAML is shown as a code block, as before.
- **A heading's address on click.** Hovering over a heading shows a link icon on the left: a click copies an address of the form `файл.md#якорь` and scrolls to the heading.
- **Emoji shortcodes.** `:rocket:` turns into 🚀: 1913 names from the gemoji database, as on GitHub. Inside code, shortcodes stay as typed.
- **Formulas.** `$E = mc^2$` and `$$…$$` no longer fall apart: they are shown in a monospace font, like code. Prices like `$100` are still not taken for formulas. Real formula typesetting is in stage 4.
- **Footnotes.** `[^1]` in the text became a link to the definition, and the definitions are gathered at the bottom of the document into a numbered list, where an arrow returns to the place of the reference. Along with this the md4c parser was updated (master branch, pinned commit `7fc1815a`): it now understands both footnotes and GitHub `> [!NOTE]` alerts by itself. Our previous alert parsing remains as a fallback.

- **A settings icon** at the top right of the window, to the left of the scrollbar: a click opens the settings window. It is on the start screen too. While the search bar is open, it takes this corner.
- **Scrolling redraws only what changed.** The canvas keeps a buffer taller than the window, so shifting the picture copies not a single pixel: only the strip that has scrolled into the window is drawn anew, plus what stays in place (the scrollbar, icons, the table of contents). A full-screen frame at 3440×1440 takes 1.3 ms instead of 4.4 (the worst 5 % of frames 2.3 ms instead of 5.5), with large images 1.3 ms instead of 3.0. Scrolling now moves in whole screen pixels so that lines do not jitter.
- **Images became sharper and faster.** If an image on screen does not match the size of the source file, a copy of exactly the needed size, downscaled with a proper filter, is prepared in the background. Previously scaling was done "by nearest pixel" right during drawing: small text in screenshots fell apart, and a full-screen scroll frame cost 3.1 ms instead of the current 2.7. The copy is redone when the zoom, the column width or the window size changes.
- **Full-screen scrolling is faster.** A 3440×1440 frame is drawn in 4.2 ms instead of 4.6 (the worst 5 % of frames in 5.0 ms instead of 5.8), with large images in 3.5 ms instead of 4.4. On a 165 Hz monitor there are 6 ms per frame. The causes were found in the machine code:
  - the background fill compiled into a "one pixel per step" loop, because the loop bounds were object fields;
  - images were scaled with a 64-bit division for every pixel.

## [0.2.0] — 19.09.2026 — "Comfortable reading"

The first public release. Startup did not get slower: about 66 ms to the first screen on small, medium and large, against about 81 ms for the empty-window reference in the same run.

- **Wide code and tables scroll sideways:** Shift+wheel, touchpad or wheel tilt, a thin slider at the bottom of the block. Selection and search take the offset into account: a match at the end of a long line scrolls into view by itself. The "Wrap lines in code" menu item turns on wrapping instead of scrolling.
- **Table of contents** (Ctrl+Shift+O or the button at the top left):
  - highlights the current section and jumps to a heading on click;
  - in a wide window it sits next to the text, in a narrow one it opens over it;
  - an open table of contents is remembered.
- **Reading position memory.** A document opens where it was closed and glides smoothly to that place. If the file has changed, the place is found by the nearest heading.
- **Search 2.0:**
  - ↑ ↓ ✕ buttons, "Match case" (Alt+C) and "Whole word" (Alt+W) toggles;
  - matches are marked on the scrollbar;
  - the query is kept for subsequent windows;
  - IME input (Chinese, Japanese, Korean). The search box runs on a separate thread, so startup still does without TSF.
- **Start screen.** With no file, recent documents are shown with search by name. Recent documents are also in the Jump List on the taskbar if FastMD is set as the program for .md.
- **Keyboard and links:**
  - Tab / Shift+Tab move through links with a visible focus frame, Enter opens;
  - a right click on a link gives "Open", "Copy link address";
  - on an image, "Copy image", "Open image file".
- **Column width:** narrow, normal, wide or full width — via the menu and Ctrl+Alt+← / →.
- **Settings window** (Ctrl+, or the menu):
  - theme, text font (Segoe UI or the book font Sitka with optical sizes), text size, column width;
  - line wrapping in code, smooth scrolling;
  - the editor for Ctrl+E: auto-detection of VS Code, Cursor, Void, Notepad++, Sublime Text, Notepad, or an exe of your own;
  - interface language: Russian or English;
  - changes apply immediately and reach the other open windows.
- **Fixed:**
  - zooming or changing the window or column width while images were loading or a large file was being parsed stopped them until F5;
  - a GIF whose frame is smaller than the size declared in its header could be drawn with a read beyond the buffer;
  - truncated text got no ellipsis.
- **Development:**
  - the source is open on GitHub under the GPL-3.0 license;
  - the version is set in one place (`project(... VERSION)` in CMake → the exe resource and the settings window);
  - the build finds any edition of Visual Studio 2022+ or the Build Tools through vswhere;
  - the UI test grew to 59 checks and runs in a separate profile (`FASTMD_REGKEY`, `FASTMD_DATA`), without touching your settings;
  - `window.cpp` was split into modules.

## [0.1.1] — 19.09.2026

- **The window opens where it was closed.** Position, size and maximized state are remembered. The window is created in the right place straight away, with no extra moves, and startup speed is not affected. Several documents opened one after another are cascaded. If a monitor has been disconnected, the window opens on the primary one.
- The position is saved on sign-out and on restart too.
- Development: the dev build (`app/build`) is separated from the working copy (`app/out/Release`, `publish.ps1`). Open documents no longer get in the way of a rebuild, and speed measurements stay honest.
- Docs: the detailed roadmap `docs/PLAN.md` replaced `ROADMAP.md`.

## [0.1.0] — 19.09.2026

The first version, about 64 ms to the first screen.

- **Engine:** C++20 + Win32 + DirectWrite (first frame on the CPU, no GPU) + md4c; only the first screen is laid out at first, large files are parsed starting from a prefix.
- **Rendering:**
  - CommonMark + GFM, GitHub Alerts, YAML front matter;
  - highlighting for 17 languages, color emoji, CJK;
  - local images, wide blocks extending beyond the column.
- **Reading:**
  - selection, copying, a copy-code button;
  - Ctrl+F search;
  - zoom, themes (system, light, dark), Mica.
- **Navigation:** GitHub anchors, relative .md links with history, external links by allowlist.
- **Environment:**
  - Ctrl+O, drag-and-drop, live reload;
  - "Open in editor", "Show in folder";
  - the .md association in HKCU.
