# Edit mode — design v2

Status: design v2, 24.09.2026 (v1 + five adversarial reviews + orchestrator decisions). Scope: the FastMD document
window only. The Explorer preview pane and thumbnails stay read-only.

The reader asked for an edit mode that **keeps the rendered look**: no switch to a plain-text editor. A double-click
puts a blinking caret at that place, and a toolbar slides down from the top (table, formula, diagram, headings,
lists …). The toolbar has an ✕ at the right that leaves edit mode; Esc leaves it too.

This document is normative. An implementation agent builds a phase from this text plus the code; every rule is meant
to be concrete enough to write a golden test from. Where this text and the code maps disagree, the code wins and the
difference is reported in the phase report.

Background maps of the code this design hooks into (file:line level, read them before touching a subsystem):
`…/scratchpad/maps/{parse-srcmap,layout-view,window-input,loader-lifecycle,math-images,select-copy-uia-find-toc,chrome-settings-update,tests-build}.md`
(full path: `C:\Users\kalen\AppData\Local\Temp\claude\C--Main-Projects-FastMD\07638e7e-da37-4517-a1e5-7d59ca7923b5\scratchpad\maps\`).
The review reports and the decisions that produced v2 are in `…\scratchpad\review\`; Appendix B maps every finding to
the section that resolves it.

---

## 0. Notation and glossary

| Term | Meaning |
|---|---|
| `g.src` | the UTF-16 Markdown source being edited (the truth) |
| text position `t` | offset in `g.doc.text` (rendered text); always paired with a block index `b` and, in tables, a cell index `c` (`TextPos{t,b,c}`) |
| source offset `s` | offset in `g.src` |
| `‸` | the caret in an example; `⟦` / `⟧` are the selection anchor / focus |
| `⏎` | one line end in an example. Goldens write it `\n`; every golden also runs with CRLF (§14.1) |
| `E` | the line end the editor writes at a given point (§7.2): the current line's own ending, else `g.eol` |
| `⟨‸⟩` | a phantom row (§6.7): a caret row that exists only in the editor, not in `g.src` |
| splice | `g.src[a, a+len) := text`, the only way `g.src` changes (§7.1) |
| text atom | an indivisible piece of rendered text with its own source: entity, emoji shortcode, backslash escape, soft/hard break, `<br>`, NUL, footnote reference `[n]`, a tab in code indentation. The caret steps over it; one Backspace/Delete removes it |
| object atom | a picture-like thing whose source is edited in a popup: inline formula, inline image, HTML `<img>` (all U+FFFC in the text), block image, display formula, Mermaid diagram, HTML block, front matter, horizontal rule (HR has no popup). A click selects it; the first Backspace/Delete next to it selects it, the second deletes it |
| selected atom | an object atom drawn with a 1 px `P_ACCENT` outline (2 px outside its box); typing does nothing while one is selected (§2.10) |
| `ContPrefix(b)` | the prefix a new continuation line of block `b` needs (quote markers + list content indentation, §7.2) |
| `BlankPrefix(b)` | the prefix of a blank line inside the containers of `b` (quote markers only, no trailing space) |
| `u` | toolbar unit: 1 DIP at the current DPI, independent of document zoom (`u = 1 / g.cfg.zoom` in canvas units) |
| RU / EN | the two UI languages (`strings.cpp`); every UI string in this document is given in both |

---

## 1. Principles

1. **The Markdown source is the truth.** `g.src` is edited; the document on screen is always `ParseMarkdown(g.src)`
   (plus two view-only additions: the phantom row, §6.7, and raw-while-typing overrides, §6.9). Nothing regenerates
   Markdown from the model. Every change is a splice, and every splice is undoable.
2. **A reader must never damage what it shows** (tasks.cpp:1-5). Saving keeps the file's bytes outside the edited
   region byte for byte, keeps the encoding, BOM, line ends and file identity (in-place write), refuses to overwrite
   a file that changed behind our back, and refuses to enter edit mode on a file it cannot write back losslessly.
3. **Startup pays nothing.** No edit-mode code or data runs before the first frame: source maps are opt-in
   (`wantMap`, §4.3) and built only on entering edit mode; new DLLs (uxtheme) are delay-loaded; recovery lookups run
   after the first frame. The speed guard (PLAN §0) stays green and is the binding gate. Exe size: after Phase 1 the
   original +96 KB budget was already exceeded (1,171,968 bytes); moving edit mode into a delay-loaded DLL was rejected
   (static CRT in both modules makes STL objects unsafe to pass across, so it would need a copying C interface around
   `Doc`). Decision (orchestrator, after Phase 1): edit mode stays in the exe; every edit-only translation unit
   (`editcore`, `editops`, `editfile`, `edit`, `editbar`, `editpop`) is compiled with `/O1` (favour size, it is not
   on a hot path except `SrcOfText`/`TextOfSrc`, which are measured); `FastMD.exe` ≤ **1,450,000 bytes** at Phase 4
   (≈ +0.5–0.8 ms per launch by the measured 1.2–1.9 ms/MB exe tax), and the speed guard must stay within its limits.
4. **WYSIWYG, not a source view.** Markdown markers are never shown, the caret moves over rendered text, and typing
   Markdown syntax at a *visible* block start still works (`# `, `- `, `> ` … become formatting after the re-parse, as in
   Typora). Syntax that would appear at a line start the reader cannot see is escaped (§7.4).
5. **Object atoms** (formulas, diagrams, pictures, HTML blocks, front matter) have no editable rendered text; they are
   edited in a source popup (a real EDIT control, IME-capable) with the rendered result updating live.
6. **The core is window-free.** Mapping, operations and undo live in `editcore`/`editops` (pure functions over
   `const Doc&` + `const std::wstring&`), unit-tested by `fastmd-edit-tests` with golden cases (§14.1). `edit.cpp`
   is glue only.
7. **No modal UI from timers, ever.** The only modal dialogs are the leave-document prompts (close, navigate away)
   and the file dialogs the reader opened; every modal call is wrapped by the modal depth counter (§10.10), and tests
   pre-answer all of them (`FASTMD_TEST_ANSWER`, §13.6).

---

## 2. User-visible behaviour (the contract)

### 2.1 Entering edit mode

| Gesture | Where the caret lands |
|---|---|
| **Double-click** on document text (reading mode, `clickCount == 2`, `window.cpp:712`) | at the second press point (hit-tested text position, mapped to source after the entry re-parse) |
| Double-click on an object atom (formula, diagram, picture, HTML block, front matter) | the atom is selected and its source popup opens (§9) |
| **F2** (`CMD_EDIT_TOGGLE`) or the **pencil button** | at the caret / selection focus if `caretOn` or a selection is visible; else at the start of the first text block whose top is visible below the bar |
| Context menu «Редактировать здесь\tF2» / "Edit here\tF2" (`CMD_EDIT_HERE`) | at the right-click point (`ContextMenu` already hit-tests it, `window.cpp:332-336`) |

- A double-click on a **link** does not enter: its first click opens the link on button-up (unchanged). Task boxes,
  `<summary>` lines, heading anchors and the code copy button keep their single-click actions and never enter.
- **Triple-click stays a reading action** (UX-5): a third press within `GetDoubleClickTime()` of the double-click that
  entered, before any splice, cancels the entry (`EditExit(silent)`, bar slides back, no save) and runs
  `SelectBlockAt` in reading mode.
- **Pencil button** (reading mode only): glyph E70F, 30×30 at `[ViewW−78, 8] – [ViewW−48, 38]` (left of the gear,
  same look, hover and hit rules as the gear, `settings_ui.cpp:366-387`); hidden on the start screen, on a
  load-failed document, while find is open, in the first frame and in bench mode. Tooltip «Редактировать (F2 или
  двойной щелчок)» / "Edit (F2 or double-click)". `Q_EDIT_TOOL(CMD_EDIT_TOGGLE)` returns its centre in reading mode.
- The first entry ever (per profile, registry `EditHintShown`) shows the toast «Режим правки: Esc — выйти, F2 —
  войти снова» / "Edit mode: Esc — leave, F2 — enter again". The first *splice* of each process session shows, when
  autosave is on, «Правки сохраняются в файл автоматически · Ctrl+Z — отменить» / "Edits are saved to the file
  automatically · Ctrl+Z — undo".

**Refusals are never silent** (UX-22): each refused entry shows a 3 s toast, except in bench mode.

| Condition (checked in this order, §10.1) | Toast RU | Toast EN |
|---|---|---|
| start screen (`g.path.empty()`) | — (F2 ignored) | — |
| modal depth > 0 (§10.10) | — (ignored) | — |
| `g.loadFailed` | «Файл не загрузился — править нечего» | "The file did not load — nothing to edit" |
| `g.fullPending` | «Документ ещё загружается…»; the request is remembered and entry happens automatically when `OnFullDoc` arrives within 2 s | "The document is still loading…" |
| `DataDir()` empty | «Нет папки данных FastMD — правка отключена» | "No FastMD data folder — editing is off" |
| file cannot be read (open fails, short read, size changed, D5) | «Не удалось прочитать файл: <причина>» | "Could not read the file: <reason>" |
| NUL bytes outside BOM-detected UTF-16 (D14) | «Файл похож на двоичный — правка отключена» | "The file looks binary — editing is off" |
| BOM FE FF (UTF-16 BE) | «UTF-16 BE не поддерживается для правки» | "UTF-16 BE files cannot be edited" |
| UTF-16 LE of odd byte length | «Файл UTF-16 повреждён (нечётная длина)» | "The UTF-16 file is damaged (odd length)" |
| stateful code page, or `Encode(decoded) != bytes` (D1) | «Кодировку файла нельзя сохранить без потерь — правка отключена» | "This file's encoding cannot be saved without loss — editing is off" |
| another FastMD window edits the same file (D18) | reading strip «Файл уже редактируется в другом окне FastMD» with [Перейти к нему] [Только чтение] (§2.5) | "This file is already being edited in another FastMD window" [Go to it] [Read only] |
| bench mode | silent | silent |

A **read-only** file (attribute, ACL, Controlled Folder Access — detected by a write probe, D17) *enters* edit mode,
shows the READONLY strip at once (§2.5) and never autosaves.

### 2.2 Leaving edit mode

**Esc chain in edit mode** (never reaches `WM_CLOSE`):
1. source popup open → cancel it (restores the text it opened with, §9.2);
2. popover or link bubble open → close it;
3. find bar open → close it;
4. outline drawer (overlay) open → close it;
5. an object atom is selected → deselect it, caret after the atom;
6. otherwise → **leave edit mode**.

The ✕ button, F2 (with no atom selected), `CMD_EDIT_EXIT` and the edit context menu item «Закончить
редактирование\tEsc» leave directly (an open popup is committed — kept — first).

**Leaving requires that nothing is unsaved** (D6, UX-15): leaving commits the popup, forces a pending re-parse
(§5.8), and flushes (§10.3). If the flush fails, edit mode stays and the **leave strip** appears: «Правки не
сохранены: <причина>. [Повторить] [Сохранить как…] [Отменить правки]». «Отменить правки» reverts `g.src` to the disk
text as one undoable step (kind DISCARD) and then leaves.

After leaving: the caret stays drawn, collapsed, at the edit caret's text position (`caretOn = true`); an Esc that
arrives within 1 s of the Esc that left edit mode never posts `WM_CLOSE` (UX-6); the undo history is kept until the
document is closed or reloaded, so re-entering can still undo.

### 2.3 The toolbar

**Geometry.** A band from `DocLeft()` to `ViewW()`, top 0, height **44 u**, sized by DPI only (UX-11). It **covers**
the page (UX-4, §12.1). Fill `P_OVERLAY_BG`, 1 px `P_OVERLAY_BORDER` rule at the bottom. Buttons 30×30 u, r6, 2 u
apart, vertically centred; group dividers 1 px `P_BORDER`, 18 u tall, 6 u margin each side; left padding 8 u; the ✕
right edge at `ViewW − 14 u` (where the gear is). Icons from `g.typo.uiIcon` at 14–15 u (Quote 16 u); Fluent-only
code points fall back to MDL2 as listed.

**Buttons, left to right** (`CMD` = the id a click runs, §13.1; the tooltip shows «name (shortcut)»):

| # | Group | Glyph | CMD | Shortcut | Tooltip RU | Tooltip EN | Collapse level |
|---|---|---|---|---|---|---|---|
| 0 | nav | E8FD | `CMD_TOC` (117) | Ctrl+Shift+O | Оглавление | Outline | 6 (hidden when `TocAvailable()` is false) |
| 1 | history | E7A7 | `CMD_UNDO` | Ctrl+Z | Отменить | Undo | never |
| 2 | history | E7A6 | `CMD_REDO` | Ctrl+Y | Повторить | Redo | never |
| 3 | style | text + E70D chevron | `CMD_BLOCK_MENU` | — | Стиль абзаца | Paragraph style | label → icon E8E9 at level 4 |
| 4 | inline | E8DD | `CMD_FMT_BOLD` | Ctrl+B | Полужирный | Bold | never |
| 5 | inline | E8DB | `CMD_FMT_ITALIC` | Ctrl+I | Курсив | Italic | never |
| 6 | inline | EDE0 | `CMD_FMT_STRIKE` | Ctrl+Shift+X | Зачёркнутый | Strikethrough | 2 |
| 7 | inline | F54C (MDL2: E943) | `CMD_FMT_CODE` | Ctrl+` | Код в строке | Inline code | 2 |
| 8 | inline | E71B | `CMD_LINK` | Ctrl+K | Ссылка | Link | 6 |
| 9 | lists | E292 | `CMD_LIST_BULLET` | Ctrl+Shift+8 | Маркированный список | Bulleted list | 3 |
| 10 | lists | "1." drawn in `g.typo.ui` semibold | `CMD_LIST_NUMBER` | Ctrl+Shift+7 | Нумерованный список | Numbered list | 3 |
| 11 | lists | E9D5 | `CMD_LIST_TASK` | Ctrl+Shift+9 | Список задач | Task list | 3 |
| 12 | lists | E9B2 | `CMD_QUOTE` | Ctrl+Shift+Q | Цитата | Quote | 3 |
| 13 | insert | E943 | `CMD_CODEBLOCK` | Ctrl+Shift+K | Блок кода | Code block | 5 |
| 14 | insert | F232 (MDL2: E80A) | `CMD_TABLE_MENU` | Ctrl+T (inserts 3×3) | Таблица | Table | 5 |
| 15 | insert | E94B | `CMD_FORMULA_MENU` | Ctrl+M / Ctrl+Shift+M | Формула | Formula | 5 |
| 16 | insert | EF90 | `CMD_DIAGRAM_MENU` | — | Диаграмма | Diagram | 5 |
| 17 | insert | EB9F | `CMD_INS_IMAGE` | — | Изображение | Image | 5 |
| 18 | insert | E738 | `CMD_INS_HR` | — | Горизонтальная линия | Horizontal rule | 5 |
| — | flexible space | | | | | | |
| 19 | status | text, or icon at level ≥ 1 | `CMD_SAVE` | Ctrl+S | the full status text (§2.6) | same | text → icon at level 1 |
| 20 | more | E712 | `CMD_EDIT_MORE` | — | Ещё | More | shown only when something is collapsed |
| 21 | close | E711 | `CMD_EDIT_EXIT` | Esc | Закончить редактирование | Finish editing | never |

- Shortcut labels are built with `GetKeyNameTextW(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16)` for OEM keys, so
  `VK_OEM_3` shows «Ctrl+Ё» on the Russian layout (UX-12); they are rebuilt on `WM_INPUTLANGCHANGE`.
- **Fixed widths** (no reflow while the caret moves): the style button is as wide as the widest of its labels
  («Текст», «Заголовок 1…6», «Код», «Таблица», «Сноска», «—»; EN "Text", "Heading 1…6", "Code", "Table",
  "Footnote", "—") + 22 u chevron
  + 16 u padding. The status slot is as wide as the widest of «Сохранено», «Не сохранено», «Сохранение…» (EN "Saved",
  "Not saved", "Saving…") + 16 u; error texts are elided with "…" and shown whole in the tooltip.
- **Collapse** (UX-11). The collapse level is the smallest level `L ∈ 0…6` at which everything fits; it is recomputed
  only on resize, DPI, zoom or language change. Level 1: the status becomes an icon (E73E saved, E895 saving, a 6 u
  `P_ACCENT` dot when dirty, E7BA in `P_ALERT_CAUTION` on an error). Level 2: Strike and Inline code move into "…".
  Level 3: the lists group moves into "…". Level 4: the style label becomes the icon E8E9. Level 5: the insert group
  moves into "…". Level 6: Link and the outline toggle move into "…". At the 360 px minimum window the bar shows
  Undo, Redo, style icon, Bold, Italic, status icon, "…", ✕.
- **Looks.** Idle glyph `P_MUTED`; hover `P_HOVER` fill + glyph `P_TEXT`; **active** (the format at the caret, or a
  pending format, is on) `P_CURRENT` fill + 1 px `P_ACCENT` stroke + glyph `P_ACCENT` (find.cpp:480); disabled glyph
  `P_BORDER`. **High contrast** (`PaletteIsHighContrast()`, UX-23): idle glyph `P_OVERLAY_TEXT`, disabled `P_MUTED`,
  hover = 1 px `P_OVERLAY_TEXT` stroke, active = `P_ACCENT` fill + `P_ONACCENT` glyph.
- **Enabled state** follows the context matrix (§8.1). A disabled button's tooltip is «<имя> — недоступно в
  таблице» / «… в блоке кода» / «… для выбранного объекта» (EN "<name> — not available in a table / in a code block /
  for the selected object").
- **Tooltip:** a pill (`P_OVERLAY_*`, h 26 u, r8) **under the hovered button**, clamped to the window, shown
  instantly; the bottom-left `g.tip` pill stays empty for bar parts.
- **Slide:** 150 ms, cubic ease-out, paced by the message-loop animation branch with QPC (not a timer); instant when
  `!g.cfg.smoothScroll` or `SPI_GETCLIENTAREAANIMATION` is off. Scroll compensation during the slide: §12.1.
- While editing, the gear and the pencil are hidden (the ✕ takes that corner); the floating outline button is
  replaced by bar button 0; the find bar moves below the bar (and below a strip, if one is shown).

### 2.4 Popovers

Drawn on the canvas (not `TrackPopupMenu`, which stays light in the dark theme): panel `P_OVERLAY_BG`, 1 px
`P_OVERLAY_BORDER`, r8, rows 28 u with `P_HOVER` hover and `P_CURRENT` for the current value, label left in
`g.typo.ui`, shortcut right in `P_MUTED`. Anchored under their button (left-aligned, clamped). Esc, a click outside,
the wheel, resize, zoom, theme change or deactivation closes a popover. Keyboard: ↑/↓ move the hot row, Enter runs
it. `Q_EDIT_TOOL(id | row << 16)` returns a row centre.

| Popover (CMD that opens it) | Rows (RU / EN) | Row action |
|---|---|---|
| Style (`CMD_BLOCK_MENU`) | Обычный текст / Normal text; Заголовок 1…6 (Ctrl+1…6) / Heading 1…6 — heading rows drawn semibold | `CMD_BLOCK_P`, `CMD_BLOCK_H1…H6` |
| Table, caret not in a table (`CMD_TABLE_MENU`) | an 8-column × 10-row grid of 18 u cells, gap 3 u; hovered range `P_CURRENT` + `P_ACCENT` stroke; label «3 × 4» (columns × rows, header row included) | `CMD_INS_TABLE` with arg `rows << 4 \| cols` |
| Table, caret in a table (`CMD_TABLE_MENU`) | Вставить строку выше / Insert row above; … ниже / below; Вставить столбец слева / Insert column left; … справа / right; Удалить строку / Delete row; Удалить столбец / Delete column; По левому краю / Align left; По центру / Center; По правому краю / Align right; Удалить таблицу / Delete table | `CMD_TABLE_*` |
| Formula (`CMD_FORMULA_MENU`) | В строке (Ctrl+M) / Inline (Ctrl+M); Отдельной строкой (Ctrl+Shift+M) / On its own line | `CMD_INS_FORMULA`, `CMD_INS_FORMULA_BLOCK` |
| Diagram (`CMD_DIAGRAM_MENU`) | Блок-схема / Flowchart; Диаграмма последовательности / Sequence diagram; Диаграмма классов / Class diagram; Диаграмма состояний / State diagram; ER-диаграмма / ER diagram; Диаграмма Ганта / Gantt chart; Круговая диаграмма / Pie chart; Интеллект-карта / Mind map; Хронология / Timeline | `CMD_INS_DIAGRAM` with arg = row index 0…8 (§8.8) |
| More (`CMD_EDIT_MORE`) | the collapsed buttons, in bar order, icon + name + shortcut | that button's CMD |
| Link (`CMD_LINK`) | URL field (a real EDIT, §9), [Готово] / [Done], [Убрать ссылку] / [Remove link]; for a reference link a notice line (§8.3) | §8.3 |
| Code language (`CMD_CODE_LANG`) | info-string field (EDIT), [Готово] / [Done] | §8.7 |

### 2.5 Strips and banners

A strip is a 36 u band directly under the bar (reading-mode strips: at the top of the document area), full bar width,
`P_OVERLAY_BG` with a 3 u accent bar at the left in the colour given, text in `g.typo.ui`, buttons in the settings
"Button" style (`settings_ui.cpp:126-147`) at the right. It covers the document (no inset); the reveal margin
accounts for it (§12.1). At most one strip is shown; priority top to bottom:

| Strip (`Q_EDIT_STRIP`) | Accent | Text RU | Text EN | Buttons (CMD) |
|---|---|---|---|---|
| 1 CONFLICT | `P_ALERT_WARNING` | «Файл изменён другой программой (на диске: <size>, у вас: <size>).» | "The file was changed by another program (on disk: <size>, yours: <size>)." | [Загрузить версию с диска (мои правки отменятся)] / [Load the disk version (discard mine)] `CMD_CONFLICT_LOAD`; [Перезаписать файл моими правками] / [Overwrite the file with my edits] `CMD_CONFLICT_KEEP` |
| 2 ENCODING | `P_ALERT_WARNING` | «Символ «<c>» нельзя сохранить в кодировке <имя>.» | "The character “<c>” cannot be saved in the <name> encoding." | [Сохранять в UTF-8] / [Save as UTF-8] `CMD_ENC_UTF8`; [Убрать символ] / [Remove the character] `CMD_ENC_REMOVE_CHAR` |
| 3 LEAVE | `P_ALERT_CAUTION` | «Правки не сохранены: <причина>.» | "Edits are not saved: <reason>." | [Повторить] / [Retry] `CMD_SAVE_RETRY`; [Сохранить как…] / [Save as…] `CMD_SAVE_AS`; [Отменить правки] / [Discard edits] `CMD_DISCARD_EDITS` |
| 4 READONLY | `P_ALERT_NOTE` | «Файл только для чтения — правки нельзя сохранить в него.» | "The file is read-only — edits cannot be saved to it." | [Сохранить как…] `CMD_SAVE_AS` |
| 5 MISSING | `P_ALERT_CAUTION` | «Файл удалён или переименован.» | "The file was deleted or renamed." | [Сохранить как…] `CMD_SAVE_AS` |
| 6 RECOVERY (reading mode, §10.5) | `P_ALERT_WARNING` | «Прошлое сохранение прервалось» or «Есть несохранённые правки от <время>» | "The last save was interrupted" / "There are unsaved edits from <time>" | [Открыть копию] / [Open the copy] `CMD_RECOVERY_OPEN`; [Восстановить] / [Restore] `CMD_RECOVERY_RESTORE`; [Удалить] / [Delete] `CMD_RECOVERY_DELETE` |
| 7 OTHER WINDOW (reading mode, D18) | `P_ALERT_NOTE` | «Файл уже редактируется в другом окне FastMD.» | "This file is already being edited in another FastMD window." | [Перейти к нему] / [Go to it] `CMD_OTHER_WINDOW`; [Только чтение] / [Read only] `CMD_STRIP_CLOSE` |

A strip closes itself when its condition ends. Transient errors (busy, network) never get a strip, only the status
slot (§2.6), and at most one toast per failure kind per session (D8).

### 2.6 Status slot and title

| Save state (`Q_EDIT_SAVE_STATE`, §13.2) | Status text RU / EN | Icon (level ≥ 1) |
|---|---|---|
| SAVED | Сохранено / Saved | E73E |
| PENDING (dirty, autosave armed) | nothing new is shown for the first 300 ms, then Не сохранено / Not saved | dot |
| SAVING | Сохранение… / Saving… — only if the save has run 300 ms (UX-24) | E895 |
| BUSY (transient, retrying) | Не сохранено: файл занят / Not saved: the file is busy | E7BA |
| DENIED, READONLY | Не сохранено: нет доступа / Not saved: access denied | E7BA |
| MISSING | Не сохранено: файла нет / Not saved: the file is gone | E7BA |
| CONFLICT | Не сохранено: конфликт / Not saved: conflict | E7BA |
| UNENCODABLE | Не сохранено: кодировка / Not saved: encoding | E7BA |
| UNKNOWN (disk unreadable, D5) | Не сохранено: файл недоступен / Not saved: file unavailable | E7BA |
| FAILED (other) | Не сохранено: ошибка записи / Not saved: write error; with a recovery path in the tooltip when one was kept (D2) | E7BA |
| OFF (autosave off, dirty) | Не сохранено (Ctrl+S) / Not saved (Ctrl+S) | dot |

`dirty := g.src != g.disk.text` (compared, not flagged: undoing back to the saved text clears it, UX-5). The window
title is `name.md* — FastMD` while dirty (the `*` directly after the name, so `startswith("name.md")` checks keep
passing); `UpdateTitle()` runs only when `dirty` flips or edit mode is entered or left.

### 2.7 Keyboard in edit mode

| Key | Action | CMD (for tests) |
|---|---|---|
| characters (`WM_CHAR` ≥ 0x20 and ≠ 0x7F; a high surrogate waits in `pendingHigh` for its low half) | type at the caret, replacing the selection (§7.3, §7.9) | — |
| Backspace / Delete | delete one cluster or text atom before / after the caret; object atom: the 1st press selects it, the 2nd deletes it; at block edges §7.7; with a selection: delete it | — |
| Ctrl+Backspace / Ctrl+Delete | delete to the previous / next word boundary inside the block or cell | — |
| Enter / Shift+Enter | §7.6 | — |
| Ctrl+Enter | new paragraph after the current block (phantom, §6.7) | `CMD_NEW_PARAGRAPH` |
| Tab / Shift+Tab | §7.8 | — |
| ← / → | by cluster or text atom; an inline object atom is stepped over; a block atom is selected on arrival | — |
| Ctrl+← / Ctrl+→ | by word (Windows rules, `MoveWord`) | — |
| ↑ / ↓ | by visual line keeping `wantX`; inter-block gaps are skipped; at the document edge next to a non-text block a phantom is created (§6.8) | — |
| Ctrl+↑ / Ctrl+↓ | to the start of the previous / next block | — |
| Home / End | visual line start / end (End lands after trailing blanks of a source line, §6.5) | — |
| Ctrl+Home / Ctrl+End | document start / end | — |
| PgUp / PgDn | by `ViewH() − 56 − EditRevealTop()`, moving the caret | — |
| Shift + any move | extends the selection | — |
| Ctrl+A | select all | `CMD_SELECT_ALL` |
| Ctrl+C / Ctrl+Insert | copy: rich formats as today + the private "FastMD Markdown" format (§7.11) | `CMD_COPY` |
| Ctrl+Shift+C | copy as Markdown | `CMD_COPY_MD` |
| Ctrl+X / Shift+Delete | cut (Phase 2b; unbound before) | `CMD_CUT` |
| Ctrl+V / Shift+Insert | paste (Phase 2b; unbound before) | `CMD_PASTE` |
| Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z | undo / redo / redo | `CMD_UNDO`, `CMD_REDO` |
| Ctrl+S | save now | `CMD_SAVE` |
| Ctrl+B, Ctrl+I, Ctrl+Shift+X, Ctrl+` (`VK_OEM_3`) | bold, italic, strikethrough, inline code | `CMD_FMT_*` |
| Ctrl+K | link popover | `CMD_LINK` |
| Ctrl+1 … Ctrl+6 | heading 1…6; pressing the active level again returns to normal text | `CMD_BLOCK_H1…H6` |
| Ctrl+Shift+7 / 8 / 9 | numbered / bulleted / task list | `CMD_LIST_NUMBER/BULLET/TASK` |
| Ctrl+Shift+Q | quote | `CMD_QUOTE` |
| Ctrl+Shift+K | code block | `CMD_CODEBLOCK` |
| Ctrl+T | table 3×3 (header + 2 rows) | `CMD_INS_TABLE` (arg 0) |
| Ctrl+M / Ctrl+Shift+M | inline formula / formula block | `CMD_INS_FORMULA`, `CMD_INS_FORMULA_BLOCK` |
| Enter or F2 on a selected object atom | open its source popup (HR: Enter creates a phantom after it) | `CMD_ATOM_EDIT` |
| F2 (nothing selected) | leave edit mode | `CMD_EDIT_TOGGLE` |
| Esc | the chain of §2.2 | — |
| Ctrl+E | flush; on success leave edit mode and open the external editor; on failure stay and show the leave strip (D19) | `CMD_EDIT` (104) |
| F5 / Ctrl+R | flush; on success leave edit mode and reload (the disk now holds the edits); on failure stay (R14) | `CMD_RELOAD` |
| Alt+← / Alt+→, mouse X buttons, Ctrl+O, Ctrl+W, a click on a link to another .md | leave the document through the leave-document rules (§10.8) — never "suspended" (T20) | — |
| Space | types a space (never pages) | — |
| Ctrl+F, F3, Ctrl+P, Ctrl+Shift+P, Ctrl+± / Ctrl+0, Ctrl+, , Ctrl+Shift+O, Ctrl+Alt+← / → | as in reading mode | — |
| Ctrl+Alt+<anything else> | never an edit chord: AltGr text reaches `WM_CHAR` (the `ctrl && alt` bail-out of `KeyCommand`) | — |

Every chord has a CMD id; functional UI tests use `cmd()` and only `test_edit_bindings` sends real chords (T5).

### 2.8 Mouse in edit mode

- Click in text: collapsed caret (no drag candidates for links/images); Shift+click extends; press-drag selects with
  autoscroll; a press inside the selection that moves starts the existing drag-out (copy only, modal, §10.10).
- Double-click selects a word; triple-click selects the block's text (a cell's text in a table).
- Click on an object atom: selects it and opens its popup (UX-2). Click on an HR: selects it.
- Click on link text: places the caret. **Ctrl+click** opens the link (Ctrl read from `MK_CONTROL` in the message's
  wParam, T5); a local .md link goes through the leave-document rules. Hovering a link shows the pill «Ctrl+щелчок —
  открыть ссылку» / "Ctrl+click — open link" (UX-19).
- Click on a task box: toggles it through a splice (undoable, kind TASK; §8.10).
- Click below the last block (more than 8 DIP under its bottom): caret in a phantom after the last block (UX-3).
- Right-click outside the selection moves the caret there (collapsed) before the menu opens (UX-18).
- Files dropped in edit mode (`WM_DROPFILES`): image files (png, jpg, jpeg, gif, bmp, svg, webp, ico, tif, tiff) are
  inserted at `DragQueryPoint` → `HitTestDoc` as `![stem](dest)` (§8.8); any other file is opened through the
  leave-document rules (UX-14).
- Wheel scrolls; the caret stays where it is; popovers close; a popup whose anchor leaves the screen closes (keeping
  its text) and gives focus back to the document (§9.2).
- Cursor: I-beam over the whole document column; arrow over object atoms; hand over bar, popover and strip buttons,
  and over links only while Ctrl is held.

### 2.9 Context menus

Reading mode: a new first item «Редактировать здесь\tF2» / "Edit here\tF2" (`CMD_EDIT_HERE`); the old
`S_MENU_EDIT` becomes «Открыть во внешнем редакторе\tCtrl+E» / "Open in external editor\tCtrl+E" (UX-24).

Edit mode (replaces the reading menu): Отменить\tCtrl+Z · Повторить\tCtrl+Y · — · Вырезать\tCtrl+X ·
Копировать\tCtrl+C · Копировать как Markdown\tCtrl+Shift+C · Вставить\tCtrl+V · — · on a link: Изменить
ссылку…\tCtrl+K, Убрать ссылку, Открыть ссылку\tCtrl+щелчок · on an object atom: Изменить источник…\tEnter · in a
table: the ten table actions of §2.4 · — · Выделить всё\tCtrl+A · Найти…\tCtrl+F · — · Сохранить\tCtrl+S ·
Сохранить как… · — · Закончить редактирование\tEsc. EN: Undo, Redo, Cut, Copy, Copy as Markdown, Paste, Edit link…,
Remove link, Open link\tCtrl+click, Edit source…, Select all, Find…, Save, Save as…, Finish editing. Items that do not
apply are greyed. A keyboard-invoked menu opens at the caret (`CaretPoint` → `ClientToScreen`).

### 2.10 Object atoms and popups (user view)

- Clicking an atom, Enter or F2 on a selected atom, or a double-click on it in reading mode opens its **source
  popup**: a panel under the atom (above it when there is no room) with a title, a monospace EDIT (IME works there),
  a hint and an error line; the document re-renders the atom live as the source changes (§9).
- Titles: «Формула» / "Formula", «Формула (отдельной строкой)» / "Formula (own line)", «Диаграмма Mermaid» /
  "Mermaid diagram", «HTML», «Свойства (YAML)» / "Properties (YAML)", «Изображение» / "Image" (two fields: «Подпись»
  / "Alt text" and «Путь» / "Path", plus [Выбрать файл…] / [Choose file…]).
- Hint: «Ctrl+Enter — готово · Esc — отменить» / "Ctrl+Enter — done · Esc — cancel". **Esc restores the text the
  popup opened with** (the step nets to nothing); Ctrl+Enter or a click outside keeps the text. Ctrl+Z / Ctrl+Y inside
  the popup use the popup's own history. After closing, the caret stands after the atom.
- While an atom is selected and no popup is open, typed characters do nothing and the pill shows «Enter — изменить ·
  Delete — удалить · ←/→ — выйти» / "Enter — edit · Delete — delete · ←/→ — leave". Typing never goes into the
  paragraph next to a selected atom.
- A render that fails keeps the last good picture with a 1 px `P_ALERT_CAUTION` outline and shows the error in the
  popup; in reading mode a failed display formula or diagram shows its source in `P_MUTED` instead of a blank box.

### 2.11 Link bubble (Phase 3b, optional if time is short)

While the collapsed caret is inside a link's text, a bubble is drawn under the link (below its last line, clamped):
the destination elided to 48 characters in `P_MUTED`, then [Изменить] / [Edit] (`CMD_LINK`), [Убрать] / [Remove]
(`CMD_LINK_REMOVE`), [Открыть] / [Open] (`CMD_LINK_OPEN`, 126, applied to the caret's link). It hides while typing
(for 1 s after the last keystroke) and when the caret leaves the link.

### 2.12 Saving (user view)

- **Autosave is on by default** (Obsidian-style): 800 ms after the last edit for sources under 1 M characters, 2 s up
  to 8 M, 5 s idle above; at least 5 s between saves on remote or cloud-backed files (D8). Also on Ctrl+S, on leaving
  edit mode, before leaving the document, before Ctrl+E, and at close and session end (§10.9).
- Settings row, **appended as the last row** (`ROW_AUTOSAVE = 12`, after `ROW_ASSOC`; hit ids 1200 = off, 1201 = on,
  so no existing hit id moves): label «Автосохранение правок» / "Autosave edits", segments «Выкл» / "Off", «Вкл» /
  "On". Stored as registry `Autosave` (DWORD, default 1) in `Config::autosave`, synced to other windows by
  `BroadcastSettings` / `OnSettingsBroadcast`. Off = saving happens on Ctrl+S and on leaving edit mode (with the leave
  strip on failure); the status slot shows OFF while dirty; the journal still protects the edits (§10.6).
- Undo history lives for the whole document session (across saves, across leaving and re-entering edit mode).

### 2.13 Input methods

The document window's UI thread runs with `ImmDisableIME(0)` for its whole life (a startup speed measure; cannot be
undone). Latin, Cyrillic, dead keys and AltGr arrive through `WM_CHAR` and work. CJK composition, the emoji panel and
dictation do not reach the document; they do work in the source popups, which are EDIT controls on the input thread.
This is an accepted limit of v1 and is stated in the README (RU + EN).

---

## 3. Architecture and module split

### 3.1 New files

| File | Responsibility | Window-free | Linked into |
|---|---|---|---|
| `src/editcore.h` | core types and API (§3.2); includes `doc.h` only | yes | FastMD, fastmd-edit-tests, fastmd-fuzz |
| `src/editcore.cpp` | map queries (`CaretStop`, `SrcOfText`, `TextOfSrc`), `MapSelfCheck`, `DiffBlocks`, prefixes and line ends, typing, Backspace/Delete, Enter/Tab, selection delete/replace, delimiter emission, block-syntax escaping, `UndoStack` | yes | same |
| `src/editops.cpp` | formatting commands, link, block style, lists, quote, code block, inserts, table operations, paste/copy transforms, task toggle, atom source replacement | yes | same |
| `src/editfile.h/.cpp` | byte-exact encode/decode with a resolved code page, `SaveSource` on a snapshot, recovery file, journal, write probe, file identity, mutex names, fault hooks | no `g`, no HWND (Win32 file API only) | FastMD, fastmd-edit-tests |
| `src/edit.cpp` | glue: `EditSession`, enter/exit, input routing, `EditReparse`, timers, save orchestration and save states, conflicts, strips, commands, queries, test hooks, big-document deferral | no | FastMD |
| `src/editbar.cpp` | toolbar, pencil, popovers, strips, link bubble, popup frames, tooltips: geometry, drawing, hit-testing, slide, collapse; UIA button fragments | no | FastMD |
| `src/editpop.cpp` | source popups on the find input thread (EDITs, shared buffer, popup history, theming) and the preview worker | no | FastMD |
| `tests/edit/edit_tests.cpp`, `tests/edit/cases/*.txt`, `tests/edit/run.ps1` | golden cases, property sweeps, undo/chord/encoding unit tests | — | fastmd-edit-tests |
| `tests/preview_direct.py` | preview-DLL pixel regression without the registry (§14.4) | — | — |

Nothing new is compiled into `fastmd_preview`.

### 3.2 Core API (`editcore.h`, normative shape)

```cpp
enum EditKind : uint8_t { EK_TYPE, EK_DEL_BACK, EK_DEL_FWD, EK_STRUCT, EK_FORMAT, EK_PASTE, EK_CUT, EK_POPUP,
                          EK_TASK, EK_ADOPT, EK_DISCARD, EK_OTHER };
struct Splice { uint32_t at; std::wstring removed, inserted; };        // g.src[at, at+removed.size()) := inserted
struct TextPos { uint32_t t; int32_t block; int32_t cell = -1; };
enum PhantomKind : uint8_t { PH_NONE, PH_AFTER, PH_BEFORE, PH_BREAK };
struct Phantom {
    PhantomKind kind = PH_NONE;
    uint32_t anchorSrc = 0;            // PH_AFTER: anchor's outerEnd; PH_BEFORE: anchor line + container prefix; PH_BREAK: content end
    int32_t anchorBlock = -1;          // resolved after every re-parse from anchorSrc
    std::wstring prefix, blankPrefix;  // ContPrefix / BlankPrefix the materialised text gets
    uint8_t depth = 0;                 // container depth (Enter/Backspace in an empty phantom pop one level)
    uint8_t style = 0;                 // 0 paragraph, 1..6 heading, 7 bullet, 8 numbered, 9 task, 10 quote (§8.1)
};
enum Fmt : uint16_t { FMT_BOLD = 1, FMT_ITALIC = 2, FMT_STRIKE = 4, FMT_CODE = 8, FMT_LINK = 16 };
struct EditState {
    uint32_t focus = 0, anchor = 0;    // source offsets; anchor == focus → collapsed (THE caret: survives re-parses)
    int8_t lineAff = 0;                // visual affinity at a soft-wrap boundary: -1 end of the upper line, +1 lower start
    Phantom phantom;
    uint16_t pendOn = 0, pendOff = 0;  // pending format for the next typed text (§7.3)
    int32_t atom = -1;                 // selected object atom id (§6.2) or -1
    uint32_t burstBeg = UINT32_MAX;    // deferred typing burst (§5.8)
    float wantX = -1;                  // column for vertical moves
};
using ClusterFn = uint32_t (*)(const std::wstring& text, uint32_t pos, int dir, void* ctx);
struct EditCtx {
    const Doc& doc; const std::wstring& src;
    const wchar_t* eol;                // g.eol: L"\n", L"\r\n" or L"\r"
    ClusterFn clusters; void* clusterCtx;  // app: IDWriteTextLayout::GetClusterMetrics; tests: a grapheme-lite table
    uint64_t nowMs;
};
struct EditResult {
    std::vector<Splice> splices;       // applied in order
    EditState after;
    EditKind kind = EK_OTHER;
    struct Expect { uint32_t sBeg, sEnd; uint16_t fmt; bool present; };
    std::vector<Expect> verify;        // post-reparse checks of emitted delimiters (§7.5); empty = none
    std::string refused;               // non-empty: nothing applied, the glue shows the toast named here
};
// mapping (§6)
bool     CaretStop(const Doc&, const TextPos&);
enum MapMode { MAP_CARET, MAP_OUTER_START, MAP_OUTER_END, MAP_INNER_START };
uint32_t SrcOfText(const Doc&, const std::wstring& src, const TextPos&, MapMode);
TextPos  TextOfSrc(const Doc&, const std::wstring& src, uint32_t s, int dir, uint16_t* trailCols);
bool     MapSelfCheck(const Doc&, const std::wstring& src, std::string* why);   // §4.5, pure: links into fastmd-fuzz
struct BlockDiff { uint32_t p, q; };
BlockDiff DiffBlocks(const Doc& oldD, const Doc& newD);                     // §5.5 step 5
std::wstring ContPrefix(const Doc&, const std::wstring& src, int block);
std::wstring BlankPrefix(const Doc&, const std::wstring& src, int block);
const wchar_t* LineEol(const std::wstring& src, uint32_t s, const wchar_t* fallback);
// operations (§7, §8): pure; the glue applies the splices, re-parses and verifies
EditResult OpType(const EditCtx&, const EditState&, std::wstring_view text);
EditResult OpBackspace(const EditCtx&, const EditState&, bool word);
EditResult OpDelete(const EditCtx&, const EditState&, bool word);
EditResult OpEnter(const EditCtx&, const EditState&, int variant /* 0 Enter, 1 Shift+Enter, 2 Ctrl+Enter */);
EditResult OpTab(const EditCtx&, const EditState&, bool shift);
EditResult OpDeleteSelection(const EditCtx&, const EditState&);
EditResult OpReplaceSelection(const EditCtx&, const EditState&, std::wstring_view text);
EditResult OpPaste(const EditCtx&, const EditState&, std::wstring_view text, bool privateFormat);
std::wstring BalancedSlice(const EditCtx&, const EditState&);             // §7.11
EditResult OpToggleInline(const EditCtx&, const EditState&, uint16_t fmt);
EditResult OpLink(const EditCtx&, const EditState&, std::wstring_view url, bool confirmRefDef);
EditResult OpLinkRemove(const EditCtx&, const EditState&);
EditResult OpBlockStyle(const EditCtx&, const EditState&, int level /* 0 paragraph, 1..6 */);
EditResult OpList(const EditCtx&, const EditState&, int kind /* 7 bullet, 8 numbered, 9 task */);
EditResult OpQuote(const EditCtx&, const EditState&);
EditResult OpCodeBlock(const EditCtx&, const EditState&);
EditResult OpCodeLang(const EditCtx&, const EditState&, std::wstring_view info);
EditResult OpInsertTable(const EditCtx&, const EditState&, int rows, int cols);
EditResult OpInsertFormula(const EditCtx&, const EditState&, bool block);
EditResult OpInsertDiagram(const EditCtx&, const EditState&, int tmpl);
EditResult OpInsertImage(const EditCtx&, const EditState&, std::wstring_view dest, std::wstring_view alt);
EditResult OpInsertHr(const EditCtx&, const EditState&);
EditResult OpTable(const EditCtx&, const EditState&, int op /* CMD_TABLE_* − CMD_TABLE_ROW_ABOVE */);
EditResult OpTaskToggle(const EditCtx&, const EditState&, int task);
EditResult OpAtomSource(const EditCtx&, const EditState&, int atom, int field, std::wstring_view text);
// undo (§11)
struct EditStep { std::vector<Splice> splices; EditState before, after; EditKind kind; uint64_t t0, t1; };
class UndoStack { public: void Push(EditStep, uint64_t nowMs); void BreakCoalescing(); const EditStep* PeekUndo() const;
                  const EditStep* PeekRedo() const; void DidUndo(); void DidRedo(); void Clear();
                  size_t Depth() const; size_t RedoDepth() const; };
bool ApplySplices(std::wstring& src, const std::vector<Splice>&, bool inverse, std::string* why);  // verifies first
// keyboard (§12.5)
unsigned EditChord(unsigned vk, bool ctrl, bool shift, bool alt);  // → CMD id or 0; ctrl && alt → always 0
```

Every `Op*` is deterministic in its inputs (time only through `EditCtx::nowMs`), never touches `g`, never allocates
Win32 objects, and returns `refused` instead of doing something unsafe.

### 3.3 Changed files

| File | Change |
|---|---|
| `third_party/md4c/md4c.{h,c}` | the patch of §4.1 (the first local change; documented in a comment block at the top of `md4c.h` and at `parse.cpp:3`) |
| `src/doc.h`, `src/parse.cpp` | §4.2–4.4: map structures, `ParseOptions`, Builder rules; `srcMap` gets an entry after each emoji substitution; `Image` pixels become shared (§5.2) |
| `src/loader.cpp` | `Spawn` kinds, `JoinDocReaders`, render table + long-lived image worker, decoupled scaler, measure index lists, `loadGen`, `LoadSource` keeps the encoding and byte baseline, `Q_RELOADS`, `OnFileChanged` rules (§10.7), watcher reports "unavailable" and restarts with backoff, recovery lookup after the first frame, `OpenDocument`/`ReloadDocument` go through the leave-document rules and `EditExit` |
| `src/util.cpp` | `ReadFileUtf16(…, DiskBytes* out = nullptr)`; `DecodeText` resolves `CP_ACP` to `GetACP()` (or `FASTMD_ACP`) and stores the number |
| `src/view.cpp` | `EditInset`, phantom row in `RecomputeY`/`HitTestDoc`/`CaretGeom`, caret stops per block/cell, `KeyMoveCaret`, caret drawing/blink/clip, `RevealCaret` (minimal scroll), one `ScrollTrackTop()`, `FrameKey`, `ScrollFrame` repair, atom outline, failed-formula source text |
| `src/layout.cpp`, `src/canvas_gdi.cpp` | shared pixels, `sc` tagged with the pixel serial it was made from |
| `src/window.cpp` | input routing, `Command(id, arg)`, queries, close/session-end, `PrepareToClose`, timers, messages, modal wrappers, the 1 s Esc guard, pencil, the `FastMD.EditOwner` registered message |
| `src/find.cpp` | bar position under bar + strip, `FindRefresh()` (no scroll, no `userMoved`), `WM_CHAR` routing (§12.6), popup EDITs hosted on the input thread |
| `src/toc.cpp` | `TocAvailable` frozen while editing, rebuild skipped when (block, level, text) is unchanged, no re-centre, floating button hidden while editing |
| `src/uia.cpp` | read-only attribute, stamped and clamped ranges, `Select`/`ScrollIntoView` routed through the editor, events, toolbar button fragments |
| `src/copy.cpp` | `SelectionMarkdown` on segments in edit mode, fixes of `copy.cpp:196` and `:203`, the private clipboard format |
| `src/tasks.cpp` | `ToggleTask` rebased on splice + `SaveSource` + `EditReparse` (T2) |
| `src/settings_ui.cpp`, `src/store.cpp` | `ROW_AUTOSAVE`; `Autosave`, `EditHintShown` values; gear hidden while editing |
| `src/strings.{h,cpp}` | the `S_ED_*` strings (every RU/EN string of this document), appended in one group |
| `src/crash.cpp` | no indirectly referenced memory in dumps taken while editing (D22) |
| `src/print.cpp`, `src/drag.cpp`, `src/shell.cpp` | modal depth around `StartDocW`, `DoDragDrop`, dialogs; inset 0 and caret hidden while printing; Ctrl+E rule |
| `src/app.h` | ids (§13), fields (§3.5), declarations |
| `CMakeLists.txt` | new sources; `fastmd-edit-tests` (option `FASTMD_TESTS`, ON); fuzz target gains `editcore.cpp`/`editops.cpp`; option `FASTMD_ASAN_APP` builds FastMD itself under ASan; `uxtheme.dll` in `/DELAYLOAD` |
| `preview/stubs.cpp`, `preview/preview.cpp` | §3.4 |
| `tests/ui_smoke.py`, `tests/uia_check.ps1`, `tests/fuzz/fuzz.cpp`, `tests/fuzz/run.ps1` | §14 |

### 3.4 The preview DLL

`view.cpp` and `layout.cpp` are shared with `fastmd_preview` (CMakeLists.txt:175-180). To keep the pane unchanged:
- `EditInset()`, `ScrollTrackTop()` and the phantom/caret code in `view.cpp` read only plain `App` fields
  (`editing`, `barT`, `phantomBlock`, …) that are zero in the DLL, so they need no stubs.
- `view.cpp` calls exactly three new external functions; `preview/stubs.cpp` gets
  `void DrawEditChrome() {}`, `int EditChromeRects(float (*)[4], int) { return 0; }` and `void EditCaretMoved() {}`.
- `Spawn(fn, arg, prio, stack, kind)` gains the `kind` parameter; the stub signature follows.
- `preview.cpp` fills the new shared pixel fields when it decodes a picture synchronously.
- `tests/preview_direct.py` (§14.4) proves the pane renders exactly as with the 1.2.0 DLL at every phase gate.

### 3.5 New `App` fields (`app.h`)

```cpp
// ---- edit mode: the fields view.cpp reads; the rest lives in edit.cpp's EditSession
bool editing = false;
float barT = 0;               // eased slide progress 0..1 (0 when not editing)
float barComp = 0;            // scroll added so far to compensate the bar (§12.1)
int32_t phantomBlock = -1;    // the phantom row sits after (or, with phantomBefore, before) this block
bool phantomBefore = false;
float phantomH = 0, phantomX = 0;
bool caretVisible = true;     // blink phase && window active, or FASTMD_CARET_STEADY
uint32_t editSerial = 0;      // bumped by every edit swap
int32_t selAtomBlock = -1, selAtomImage = -1;  // the selected object atom (view.cpp draws its outline)
int editModal = 0;            // modal depth (§10.10)
float stripH = 0;             // height of the visible strip under the bar (0 = none)
std::atomic<uint32_t> loadGen{0};  // bumped by LoadSource; cancels queued picture jobs (§5.2)
uint32_t reloads = 0, saves = 0;   // Q_RELOADS, Q_SAVES
std::wstring eol = L"\n";     // §7.2
DiskState disk;               // §10.2: the text, bytes, encoding and identity believed to be on disk
std::unordered_map<std::wstring, RenderEntry> renders;  // §5.2, UI thread only
```
`Config` gains `bool autosave = true;`.

### 3.6 Threads

| Thread | Reads | Writes | Stops on | In `g.workers` |
|---|---|---|---|---|
| UI | everything | everything | — | — |
| measure (`WK_MEASURE`) | `g.doc` (joined before every swap) | posts heights | `gen` | yes |
| full parse (`WK_FULLPARSE`) | `g.src` | posts a Doc | `docGen`; edit mode is refused while it runs | yes |
| picture worker (one, long-lived) | copies in its jobs | posts results (`WM_APP_IMAGES`) | job `loadGen` ≠ current, `closing` | no (detached) |
| scaler (`WK_SCALE`) | the shared pixels it holds a reference to | posts `ScaledImage` | `closing` | yes (`JoinWorkers` waits, `JoinDocReaders` does not) |
| preview worker (one, long-lived) | its latest job | posts `WM_APP_PREVIEW` | stale sequence, `closing` | no (detached) |
| save worker (sources ≥ 1 M chars only) | its snapshot | the file; posts `WM_APP_SAVED` | always completes | no; flush points wait for it without pumping |
| input thread (find box + popups) | its EDITs | the popup buffer (SRW lock); posts | process end | no |
| watcher | file stamp | posts `WM_APP_FILECHANGED` | `watchStop` | no |
| updater threads | — | posts | — | no (already detached) |
| UI Automation calls | arrive on the UI thread wherever it pumps (no `UseComThreading`) | — | — | — |

---

## 4. Parser extensions (Phase 1a)

### 4.1 The md4c patch

The only local change to md4c. It adds one pointer to `MD_PARSER` (after `syntax`, so a zero-initialised parser is
unchanged) and a few call sites. With `fastmd == NULL` md4c behaves exactly as today.

```c
/* ---- FastMD patch 1: source extents for the editor (all offsets relative to the text passed to md_parse()) ---- */
typedef struct MD_FASTMD_HOOKS {
    void (*leaf_extent)(MD_BLOCKTYPE type, MD_OFFSET beg, MD_OFFSET end, unsigned flags, void* userdata);
    void (*verbatim_line)(MD_OFFSET beg, MD_OFFSET end, unsigned indent, void* userdata);
    void (*break_extent)(MD_TEXTTYPE type, MD_OFFSET beg, MD_OFFSET end, void* userdata);
    void (*span_extent)(MD_SPANTYPE type, int enter, MD_OFFSET beg, MD_OFFSET end, void* userdata);
    void (*cell_extent)(MD_OFFSET beg, MD_OFFSET end, int missing, void* userdata);
    void (*footnote_extent)(MD_OFFSET def_beg, MD_OFFSET content_beg, MD_OFFSET content_end, void* userdata);
} MD_FASTMD_HOOKS;
#define MD_FASTMD_SETEXT  0x0008   /* leaf_extent flags: = MD_BLOCK_SETEXT_HEADER */
#define MD_FASTMD_FENCED  0x0100   /* leaf_extent flags: fenced (not indented) code block */
/* in MD_PARSER, after `syntax`: */
const MD_FASTMD_HOOKS* fastmd;     /* NULL = off */
```

| # | Where (md4c.c) | What fires | Reported values and edge cases |
|---|---|---|---|
| P1 | `md_process_leaf_block` (5525), after the `switch` that fills `det` and before the `if(!is_in_tight_list ‖ …) MD_ENTER_BLOCK` (so it also fires for a P that a tight list swallows, F14) | `leaf_extent` for every leaf with `n_lines > 0`: P, H, CODE, HTML, TABLE, HR | `beg = lines[0].beg`, `end = lines[n−1].end` (`MD_VERBATIMLINE` stride for CODE and HTML, `MD_LINE` otherwise); `flags = (block->flags & MD_BLOCK_SETEXT_HEADER) \| (CODE && block->data ? MD_FASTMD_FENCED : 0)`. Fenced code: `lines[0]` is the opening fence line (`beg` at the fence character). Setext: the underline is **not** in `lines` (5939-5946); the Builder finds it (F14c). ATX: `end` excludes the closing sequence and trailing blanks. TABLE: header line to last row (F14d). The table-density check at the top of the function runs first, so a suppressed table reports as P |
| P2 | `md_process_verbatim_block_contents` (5429), first statement of the loop body | `verbatim_line(line->beg, line->end, line->indent)` for every content line of a code block (the fence line is skipped by `md_process_code_block_contents`) and every HTML-block line | `beg` is after *all* leading whitespace of the line (container prefix and code indentation); `indent` is the code indentation in columns that md4c re-emits as spaces. Empty lines are reported too (`beg == end`) — they produce no text chunk (F3) |
| P3 | `md_process_inlines` (4858), immediately before each `MD_TEXT(break_type, "\n")` and before the code-span/formula `MD_TEXT(text_type, " ")` that joins two lines | `break_extent(type, beg, end)` | soft/hard break: `beg = line->end − 1` when the break is a backslash hard break (`enforce_hardbreak`), else `line->end` (so trailing blanks, incl. a two-space break, are inside); `end = (line+1)->beg` (after the next line's container prefix and indentation). Code span / formula: `beg = off` (the EOL position after md4c emitted trailing blanks as text), `end = (line+1)->beg`. Raw inline HTML across lines: reported with `MD_TEXT_HTML`, ignored by the Builder |
| P4 | `md_process_inlines`, next to every `MD_ENTER_SPAN` / `MD_LEAVE_SPAN` | `span_extent(type, enter, beg, end)` | EM: `[off, off+1)` and STRONG: `[off, off+2)`, fired inside the `*`/`_` loops with the current `off` (a run of three reports EM then STRONG on enter, STRONG then EM on leave); CODE, DEL, LATEXMATH(_DISPLAY): `[mark->beg, mark->end)` (a code opener includes the stripped space); A/IMG: enter `[opener->beg, opener->end)` (`[` or `![`), leave `[closer->beg, closer->end)` — the closer covers `](dest "title")`, `][label]` or `]`; FOOTNOTE_REF: `[opener->beg, closer->end)` on both enter and leave; autolinks (`<…>`, permissive URL/WWW/e-mail): enter `[opener->beg, opener->end)`, leave `[closer->beg, closer->end)`, fired only under the same `VALIDPERMISSIVEAUTOLINK` condition as the span (permissive delimiters are zero-width, F23). Other span types (not enabled by FastMD's flags) report their mark extents the same way |
| P5 | `md_process_table_cell` (5245) gains `int missing`; `md_process_table_row` passes 1 in the padding loop (5314-5315) | `cell_extent(beg, end, missing)` after trimming, before `MD_ENTER_BLOCK` | an empty present cell reports `beg == end ==` the offset of its closing pipe (trimming ran to it); a padding cell reports `(0, 0, 1)` (F14, F20) |
| P6 | `MD_FOOTNOTE_DEF` gains `OFF def_beg`, set to `lines[0].beg` in `md_is_footnote_definition` (2046); `md_process_footnote_def` (7217) fires before `MD_ENTER_BLOCK` | `footnote_extent(def_beg, content_lines[0].beg, content_lines[n−1].end)` | `def_beg` is the `[` of `[^label]:`; a definition with no content line reports `(def_beg, def_beg, def_beg)` (Phase 1 notes). Unreferenced definitions are not processed, so they get no extent (they are invisible lines, §7.9). Definitions are one paragraph (2082-2105) (F5) |
| P7 | `md_is_container_mark` (6573), both list branches (6594-6601, 6613-6624) | — | `p_container->task_mark_off = beg;` (the first character of the marker). The task path (7012) overwrites it for task items, so `MD_BLOCK_LI_DETAIL::task_mark_offset` means "offset of the task mark" for task items and "offset of the list marker" otherwise (comment updated in `md4c.h`). The Builder derives a task item's marker by scanning back from `task_mark_offset − 1` (the `[`) over blanks to the marker (F14a) |
| P8 | `md_consume_link_reference_definitions` (5896-5911), the injected HR | — | the injected HR keeps its line: the line `lines[n]` is saved before `n++`, the HR is pushed with `sizeof(MD_BLOCK) + sizeof(MD_LINE)`, the current block is moved by that size, `n_lines = 1` and the line is stored after the block, so P1 reports it (F14e). Ordinary HR and ATX blocks already carry their single line (7142-7148) |

The patch is mechanical; §4.5's invariants run under the fuzzer on every input (§14.2). Two upstream bugs are fixed
with it, in every parse (marked "FastMD fix" in `md4c.c`): a NUL in code or raw HTML reached the text twice (as
U+FFFD and raw, `md_text_with_null_replacement`), and a code span whose closer starts a line (`` foo⏎`` ``) was
followed by a soft break inside its own closer (the closer now moves the line on, as the link closer does), which
also rendered `` ``⏎foo⏎``bar `` as "foo bar" instead of CommonMark's "foobar".

### 4.2 Doc additions (`doc.h`)

```cpp
enum SegKind : uint8_t { SEG_PLAIN = 0,  // tLen == sLen, char for char (text, code, table cells)
                         SEG_TEXTATOM,   // entity, emoji, escape, soft/hard break, <br>, NUL, footnote ref, code-indent tab
                         SEG_OBJATOM,    // inline image, inline formula, HTML <img> (one U+FFFC)
                         SEG_SYNTH };    // synthesized text with no source (the footnote " ↩"): never a caret stop
struct SrcSeg { uint32_t t, tLen, s, sLen; uint8_t kind; uint8_t flags; };  // flags: SEGF_SPLITTAB (§4.4)
enum BlockSrcFlags : uint16_t {
    BS_RAW = 1,        // HTML block / front matter: an object atom edited in a popup
    BS_SETEXT = 2, BS_ATX = 4, BS_FENCED = 8, BS_UNCLOSED = 16,  // heading and fence shapes
    BS_SYNTH = 32,     // alert title, footnote-section HR: no source, not a caret stop
    BS_FOOTNOTE = 64,  // footnote definition text (rendered far from its source)
    BS_EMPTYITEM = 128,// synthesized leaf of an empty list item
    BS_OBJECT = 256,   // BK_IMAGE block or HR: an object atom
    BS_NOCONTENT = 512,// fenced block with no content line at all
    BS_RAWTEXT = 1024, // raw-while-typing leaf (§6.9)
    BS_FRONT = 2048, BS_HTML = 4096 };
struct BlockSrc {
    uint32_t beg = 0, end = 0;   // content extent in g.src (after prefixes and markers; ATX: before the closing sequence)
    uint32_t line = 0;           // start of the block's first source line (container prefix included)
    uint32_t lineEnd = 0;        // end of the block's last content line before its EOL, trailing blanks included (F1)
    uint32_t outerEnd = 0;       // end of the last line that belongs to the block: closing fence / setext underline;
                                 // = lineEnd otherwise; unclosed fence = last content line end (F14f)
    uint32_t segOff = 0, segCount = 0, spanOff = 0, spanCount = 0;  // this block's slices of segs / spans
    int32_t container = -1;      // innermost container (Doc::containers), -1 = top level
    int32_t rawId = -1;          // BS_RAW: index of the first block made from the same HTML block / front matter
    uint32_t aux = UINT32_MAX;   // BS_FENCED: end of the opening fence line; BS_FOOTNOTE: def_beg
    uint16_t flags = 0;
};
enum ContainerKind : uint8_t { CT_QUOTE, CT_ALERT, CT_ITEM, CT_FOOTNOTE };
struct ContainerSrc {
    ContainerKind kind; int32_t parent;       // parent container or -1
    uint32_t firstBlock, lastBlock;
    uint32_t markOff = UINT32_MAX;            // CT_ITEM: marker's first char; CT_FOOTNOTE: def_beg
    uint8_t markLen = 0;                      // "-" 1, "12." 3
    wchar_t bullet = 0, delim = 0;            // '-' '+' '*' / '.' ')'
    uint32_t number = 0;                      // ordered: the number as written
    uint16_t contentCol = 0;                  // CT_ITEM: absolute column of the content (tab stops of 4, F16)
    uint32_t taskOff = UINT32_MAX;            // task mark offset (the char between [ ])
    bool tight = true;
};
enum SpanSrcType : uint8_t { /* MD_SPANTYPE values for EM, STRONG, A, IMG, CODE, DEL, LATEXMATH(_DISPLAY), FOOTNOTE_REF */
    ST_HTML_B = 0x80, ST_HTML_I, ST_HTML_CODE, ST_HTML_S, ST_HTML_KBD, ST_HTML_SUP, ST_HTML_SUB, ST_HTML_A };
enum SpanFlags : uint8_t { SF_AUTOLINK = 1, SF_REF = 2, SF_UNCLOSED = 4, SF_ENTERABLE = 8, SF_UNDERSCORE = 16 };
struct SpanSrc { int32_t block; uint32_t tBeg, tEnd; uint32_t openBeg, openEnd, closeBeg, closeEnd;
                 uint8_t type, flags; };
struct CellSrc { uint32_t beg, end; bool missing; };
struct RowSrc { uint32_t lineStart, contentStart, lineEnd; std::vector<uint32_t> pipes; };  // pipes: offsets of '|'
struct TableSrc { std::vector<RowSrc> rows; };  // rows[1] is the delimiter row

// in Doc (filled only when ParseOptions::wantMap):
std::vector<SrcSeg> segs;            // text order; each block's segments are contiguous (BlockSrc::segOff)
std::vector<BlockSrc> blockSrc;      // parallel to blocks
std::vector<uint32_t> blockOrder;    // block indices sorted by blockSrc.line (footnote definitions in source order)
std::vector<SpanSrc> spans;          // per block, in opener order (BlockSrc::spanOff)
std::vector<CellSrc> cellSrc;        // parallel to cells
std::vector<TableSrc> tableSrc;      // parallel to tables
std::vector<ContainerSrc> containers;
bool hasMap = false;
```
`Image` gains `uint32_t outerBeg, outerEnd` (with delimiters or fences), `srcBeg, srcEnd` (TeX between the dollars;
Mermaid content lines; for pictures the destination), `altBeg, altEnd` (pictures), all copied by the copy
constructor and assignment. Picture pixels move to shared, immutable storage (§5.2).

### 4.3 Opt-in maps and parse options

```cpp
struct ParseOptions {
    bool wantMap = false;                                         // build §4.2's vectors and install the md4c hooks
    const std::vector<uint32_t>* masks = nullptr;                 // offsets parsed as U+E000, restored in the text (§6.9)
    const std::vector<std::pair<uint32_t, uint32_t>>* raw = nullptr;  // source ranges built as raw-text leaves (§6.9)
};
bool ParseMarkdown(Doc& d, const wchar_t* src, size_t n, const ParseOptions* opt = nullptr);
```
- Startup, the preview pane, `FullParseThread` and every reading-mode parse pass no options: no hooks, no map
  vectors, identical cost to today (R19, T12). `srcMap` keeps being built there (for reading-mode "copy as
  Markdown"), with one fix: `AppendWithEmoji` adds an entry after each substitution, so positions after an emoji map
  exactly.
- `EditEnter` re-parses with `wantMap`; every edit-mode parse wants maps; `srcMap` is not built with maps.
- With masks, `ParseMarkdown` parses a private copy of the source in which each masked offset holds U+E000; the
  Builder writes the original character into `d.text`. Offsets are identical, so segments need no correction.

### 4.4 Builder rules (`parse.cpp`, map mode)

Every md4c offset gets `+ mdBase`. `LineStart(s)` / `LineEnd(s)` are the start of the line containing `s` and the
position of its EOL. The Builder keeps a one-slot **extent stash** filled by `leaf_extent` / `footnote_extent`,
consumed by the next `Emit`, and **cleared after every `Emit`** (F14b).

**Segments** (in text order, per block, per cell):

| Text the Builder appends | Segment |
|---|---|
| a `NORMAL`/`CODE`/`LATEXMATH` chunk that points into the source | `SEG_PLAIN {t, n, s, n}` |
| a chunk in which `AppendWithEmoji` substituted shortcodes | split into PLAIN / `SEG_TEXTATOM`(emoji: `tLen` = emoji code units, `sLen` = shortcode with both colons) / PLAIN |
| a one-character chunk that is a backslash escape (preceded by an odd run of `\`) | `SEG_TEXTATOM {t, 1, s−1, 2}` |
| an `ENTITY` chunk | `SEG_TEXTATOM {t, decodedLen, s, n}` |
| `SOFTBR` `" "`, `BR` `"\n"`, code-span/formula line join `" "` | `SEG_TEXTATOM` over the last `break_extent` (one atom per static text, never merged) |
| `NULLCHAR` → U+FFFD | `SEG_TEXTATOM {t, 1, g, 1}` where `g` = the end of the previous segment's source (the NUL itself) |
| verbatim line content (`verbatim_line` beg..end) | `SEG_PLAIN` |
| verbatim indentation (`indent` columns re-emitted as spaces) | the source whitespace run `W` before `beg` (back to the last `>` or the line start) holds container indentation followed by the code indentation; the code indentation starts at column `col(beg) − indent` (tab stops of 4 from the line start). Each space of it → `SEG_PLAIN` 1:1; each tab → `SEG_TEXTATOM {sLen 1, tLen = its width}`; a tab that straddles the boundary → `SEG_TEXTATOM` with `SEGF_SPLITTAB` (an edit touching it first rewrites that one tab as spaces, §7.1) (F3) |
| verbatim `"\n"` between lines k and k+1 | `SEG_TEXTATOM` over `[end_k, start of line k+1's code indentation)` = EOL + container prefix + container whitespace |
| trailing `"\n"` of a code block | map mode: `EndLeaf` trims exactly one trailing `"\n"` (and its atom), so empty last lines keep caret positions (F4); without maps all are trimmed as today |
| inline image / inline formula U+FFFC | `SEG_OBJATOM` over the span's outer extent (`openBeg` of `![`/`$` .. `closeEnd`) |
| HTML `<img>` U+FFFC; HTML `<br>` `"\n"` | `SEG_OBJATOM` / `SEG_TEXTATOM` over their `MD_TEXT_HTML` chunk (its pointer points into the source) |
| footnote reference `"[n]"` | `SEG_TEXTATOM` over `[openBeg, closeEnd)` |
| the footnote back arrow `" ↩"` | `SEG_SYNTH {t, 2, content end, 0}` |
| alert titles, front-matter text, HTML-block text, picture-only paragraph alt text, display formula and Mermaid placeholder text | no segments (`BS_SYNTH` / `BS_RAW` / `BS_OBJECT` blocks) |

**Blocks** (`blockSrc`):

| Block | `beg..end` | `line` | `lineEnd` | `outerEnd` | flags |
|---|---|---|---|---|---|
| paragraph (incl. tight-list implicit leaf) | leaf extent | `LineStart(beg)` | `LineEnd(end)` | `lineEnd` | — |
| ATX heading | leaf extent (after `#…# `, before the closing sequence) | `LineStart` | `LineEnd` | `lineEnd` | `BS_ATX` |
| setext heading | leaf extent | `LineStart(beg)` | `LineEnd(end)` | end of the underline line: the Builder scans the next line (EOL, container prefix, `=+` or `-+`, blanks) | `BS_SETEXT` |
| fenced code | position of text offset 0 (the first content line's code indentation) .. end of the last kept content line; no content line: `beg = end = LineEnd(fence line)` + `BS_NOCONTENT` | `LineStart(fence)` | last content line end (fence line end if none) | closing fence line end if a closing fence follows (scan: container prefix, ≥ n fence chars, blanks), else `lineEnd` + `BS_UNCLOSED` | `BS_FENCED`; `aux` = end of the opening fence line |
| indented code | position of text offset 0 .. end of last line | `LineStart` | `LineEnd` | `lineEnd` | — |
| HTML block | leaf extent | `LineStart` | `LineEnd` | `lineEnd` | `BS_RAW\|BS_HTML`; every block `EmitHtmlBlock` makes from it shares this record and `rawId` |
| table | leaf extent | `LineStart` | `LineEnd` | `lineEnd` | — (+ `tableSrc`, `cellSrc`) |
| HR (incl. injected) | leaf extent | `LineStart` | `LineEnd` | `lineEnd` | `BS_OBJECT` |
| picture-only paragraph, display formula, Mermaid (BK_IMAGE) | the P / code extent | as above | as above | as above | `BS_OBJECT` (+ `BS_FENCED` for Mermaid); segments of the paragraph erased |
| front matter (table or YAML) | first body line start .. last body line end | 0 | last body line end | closing `---`/`...` line end | `BS_RAW\|BS_FRONT` |
| alert title (md4c admonition or `TryAlert`), footnote-section HR | — | — | — | — | `BS_SYNTH` |
| footnote definition text | `footnote_extent` content | `LineStart(def_beg)` | `LineEnd(content end)` | `lineEnd` | `BS_FOOTNOTE`, `aux = def_beg` |
| footnote definition with no text (`[^2]:`; a synthesized leaf that shows its number and `↩`, in reading mode too) | `beg = end` = after `]:` and the blanks, clamped to the line end | `LineStart(def_beg)` | `LineEnd` | `lineEnd` | `BS_FOOTNOTE\|BS_EMPTYITEM`, `aux = def_beg` |
| empty list item (synthesized leaf) | `beg = end` = marker end + following blanks (task: after `[ ]` + one blank), clamped to the line end | `LineStart(markOff)` | `LineEnd` | `lineEnd` | `BS_EMPTYITEM`; an item whose marker line holds more than blanks but no leaf (a footnote or link reference definition took it, `- [^1]: x`) stays `BS_SYNTH`: the line is the definition's |

**Spans.** A stack per block: `span_extent(enter)` pushes `{type, tBeg = text.size(), openBeg, openEnd}`,
`span_extent(leave)` pops and records `closeBeg/closeEnd`, `tEnd`. Flags: `SF_ENTERABLE` for EM, STRONG, DEL,
`ST_HTML_B/I/S/SUP/SUB`; `SF_AUTOLINK` for an A whose opener is `<` or zero-width; `SF_REF` for an A/IMG whose closer
does not start with `](`; `SF_UNDERSCORE` when the run is `_`. **Inline HTML pseudo-spans** (F25): each inline tag
chunk of `b strong i em cite var code tt samp del s strike kbd sup sub a` that points into the source opens or closes
a pseudo-span of its class (innermost open of the class); an opener still open at the block end gets `SF_UNCLOSED`
with `closeBeg = closeEnd =` the block's content end. A tag split across lines (md4c sends it one line at a time)
is put together and read as one tag in both modes: it adds what the tag adds (a `<br>` across lines is one atom over
its whole source, line end and prefix included), its attributes are never text, and it makes no pseudo-span.

**Other rules.**
- `TryAlert` (one-line `> [!NOTE] text`): segments inside the replaced text range are dropped, later ones shifted by
  `shift` (like the runs); the content block's `beg` moves past the tag and its whitespace (F24).
- Picture-only paragraph → `BK_IMAGE`: the paragraph's segments are erased.
- `Image` ranges: formulas from the LATEXMATH span extent; pictures from the IMG span (`altBeg = openEnd`,
  `altEnd = closeBeg`, destination parsed from the closer text; reference pictures have no destination range);
  Mermaid from the code block's content lines; HTML `<img>` from its chunk.
- Tables: `cellSrc` from `cell_extent`; `RowSrc` per row line: `lineStart = LineStart(first present cell)`,
  `contentStart` after the container prefix, `lineEnd = LineEnd(...)`; `pipes` = the leading pipe (a `|` found
  scanning back from the first cell over blanks), each separator (the first `|` at or after a cell's `end`) and the
  trailing pipe. The delimiter row (never reported by md4c) is split by the Builder on `|` (F20).
- Containers: pushed on QUOTE/ADMONITION/LI/FOOTNOTE_DEF enter, popped on leave. `CT_ITEM.contentCol` = column of
  the marker + marker length + the blanks after it (1–4; five or more, or none before the EOL, count as 1).
- Raw leaves (§6.9): when a leaf's extent intersects a `raw` range, the Builder ignores md4c's text and span callbacks
  for that leaf and appends the source lines itself (container prefix stripped, lines joined by `"\n"`, one `F_CODE`
  run), with PLAIN segments per line and TEXTATOM line joins; flag `BS_RAWTEXT`.

### 4.5 Invariants (`MapSelfCheck`, pure, in `editcore.cpp`)

1. `segs` sorted by `t`, non-overlapping; each segment lies inside its block's text range (and inside its cell).
2. `SEG_PLAIN`: `src.compare(s, len, text, t, len) == 0`.
3. `SEG_TEXTATOM`/`SEG_OBJATOM`: `sLen ≥ 1`; an escape atom starts with `\`; a NUL atom is `\0`. Every segment
   with a source (plain ones too) lies inside the block's `[line, outerEnd]`, and a table cell's inside that cell's
   `[beg, end]`.
4. Coverage: every text position of a block that is not `BS_SYNTH`, `BS_RAW` or `BS_OBJECT` is covered by exactly
   one segment (`SEG_SYNTH` counts as covering).
5. Inside a block, segments are non-decreasing in `s`.
6. `line ≤ beg ≤ end ≤ lineEnd ≤ outerEnd` for every block with a record, `line` at a line start and `lineEnd`,
   `outerEnd` at line ends; blocks of the normal flow (not `BS_FOOTNOTE`, not `BS_SYNTH`) have non-decreasing
   `line`; `blockOrder` is sorted by `line`, and neighbours in it claim disjoint `[line, outerEnd]` (the blocks of one
   HTML block, which share its record, excepted).
7. Spans: `openBeg ≤ openEnd ≤ closeBeg ≤ closeEnd` (unless `SF_UNCLOSED`); spans of a block nest without partial
   overlap in both text and source; `tBeg ≤ tEnd` inside the block's text; no segment reaches into a delimiter,
   except the atom that stands for the whole span (a picture, a formula, a footnote reference).
8. Cells: a present cell lies inside its row's `[contentStart, lineEnd]`; pipes are `|` characters.
9. Items: `g.src[markOff, markOff + markLen)` is a list marker (`-`, `+`, `*`, or digits and `.`/`)`) followed by a
   blank or the line end; no two items share a marker; the marker lies before its first block's `beg`; `taskOff` (if
   any) is ` `, `x` or `X` between `[` and `]`.
10. Every `Image` with ranges has `outerBeg ≤ srcBeg ≤ srcEnd ≤ outerEnd` inside its block's lines.

The self-check says the map is well formed; the round trips of §14.1's sweep (`tests/edit/map_sweep.h`) say it is
usable, and the fuzzer runs both (§14.2).

`MapSelfCheck` is compiled into Release: `Q_MAP_SELFCHECK` runs it on demand, and with `FASTMD_EDIT_SELFCHECK=1`
after every `EditReparse` (failures counted, T22). The fuzzer runs it on every input (§14.2).

---

## 5. Re-parse, swap and workers (Phases 1b–1c)

### 5.1 Worker kinds and joins
- `enum WorkerKind { WK_OTHER, WK_MEASURE, WK_SCALE, WK_FULLPARSE };` `Spawn(fn, arg, prio, stack, kind)` first closes
  and removes handles that are already signalled (R23); `g.workers` holds `{HANDLE, kind}`.
- `JoinWorkers()` (open, reload) keeps its meaning (bump `gen`, `docGen`, wait for all, `jobsPending = 0`, drop a
  pending full parse) and additionally bumps `loadGen` and clears `g.renders`. The updater, the picture worker and
  the preview worker are detached and never waited for (a reload no longer waits for a download).
- `JoinDocReaders()` (every edit swap): `gen++; docGen++;` **then** waits for `WK_MEASURE` handles only (they stop
  within one block), removes and closes them, `jobsPending = 0`. It asserts `!g.fullPending`. It never waits for the
  scaler, the picture worker, the preview worker or the updater (R3). Bumping `docGen` is harmless: nothing that can
  run in edit mode checks it any more. "Within one block" holds because a measure thread reads no picture header that
  can block: a path on a network drive or a cloud file not on this disk is left at size unknown (placeholder), and the
  picture worker's decode supplies the size (Phase 1 notes).

### 5.2 Render table and the picture worker (R4, R5, R6, R15, R20)

- **Keys.** `m<kind>:<source>` for formulas and diagrams (UTF-16 source from `Image::alt`), `u:<url>` for remote
  pictures, `p:<path>` for local ones. Render context `ctx = hash(fontPx, g_pal[P_TEXT], dark)` for formulas and
  diagrams, 0 for pictures. The table key is `key + '\x1f' + ctx`.
- **Entries** (UI thread only): `struct RenderEntry { uint8_t state /* RS_PENDING, RS_OK, RS_FAILED */;
  std::shared_ptr<const Pixels> pix; int w, h; float ascent; std::shared_ptr<const Scaled> sc; std::wstring
  cachePath; FILETIME failTime; uint64_t failSize; std::string error; uint64_t lastUse; }`, with
  `struct Pixels { std::vector<uint32_t> px; int pxW, pxH; std::vector<uint8_t> svg; uint32_t serial; }` and
  `struct Scaled { std::vector<uint32_t> px; int w, h; uint32_t serial; }`. `serial` is a global counter bumped for
  every new pixel set (`pxSerial`, R4).
- **Image entries** carry `std::shared_ptr<const Pixels> pix; std::shared_ptr<const Scaled> sc; uint8_t state;
  bool renderFailed; std::wstring pxFor;` (the key the pixels were rendered from, R15) and plain `wantW/wantH`.
  Pixels are immutable once published, so the copy constructor copies the pointers (no more "copies lose pixels");
  `canon` is gone (every entry points at the shared pixels). `DrawImage` uses `sc` only when
  `sc->serial == pix->serial` and the size matches the box (R4).
- **`StartImages()`** (after a load and after every `EditReparse`): for each entry, a hit in the table copies state,
  pixels, sizes, `sc`, `cachePath` (remote pictures keep their cache path, R20) and `pxFor = key`; a miss creates an
  `RS_PENDING` entry and enqueues a job, except: a remote picture in the caret's block while editing is held in
  `deferredRemote` until the caret leaves that block or `TIMER_EDIT_IDLE` fires (1 s without typing, R5); a remote
  picture that is not allowed (setting "ask"/"never") becomes `RS_FAILED` without a fetch. Keys already in the table
  are never enqueued again, whatever their state (no job storms; failed formulas are not re-rendered per keystroke).
- **The worker**: one long-lived detached thread (`CreateThread` + `CloseHandle`, started on first use), a queue under
  an SRW lock with an auto-reset event. A job is a value: `{key, ctx, kind, math (UTF-8), path, url, attrW, attrH,
  fontPx, rgb, dark, loadGen}`. It skips jobs whose `loadGen` differs from `g.loadGen` and exits on `closing`. Per-edit
  `docGen` bumps do not affect it (R6). Results are posted in batches (every 50 ms or 8 results) as `WM_APP_IMAGES`,
  lParam = `std::vector<RenderResult>*` (deleted by the receiver; deleted by the worker if the post fails).
- **Applying** (`OnImagesLoaded(batch)`): a result whose `loadGen` is stale is dropped; a formula/diagram result whose
  `ctx` differs from the current context is dropped and, if still referenced, enqueued again (theme switch while
  rendering, R6). Otherwise the table entry becomes `RS_OK` (new `pxSerial`) or `RS_FAILED` (error text, file stamp),
  every `g.doc.images` entry with that key and context is updated, and — once per batch — only the blocks that
  reference a changed entry (a `BK_IMAGE` with that `aux`, or a block with an `F_IMAGE` run on it) lose their cached
  layout and get `H` from `ImageDisplayHeight`; `RecomputeY` under the anchor, `pixelSerial++`, `Invalidate()`.
- **Failures.** A failed local picture keeps its file stamp and is retried only on an explicit reload,
  `CMD_LOAD_REMOTE`, or when a stamp check on window activation shows the file changed. The activation check runs on
  the UI thread, so it runs at most every 2 s, never inside a modal loop, and never for a path on a network drive
  (those are retried at the next load). A failed formula or diagram is never retried for the same key and context.
- **Memory.** When the pixel bytes of entries not referenced by `g.doc` exceed 64 MB, the least recently used are
  evicted.

### 5.3 The scaler (R3, R4)
`ScheduleImageScaling` picks entries with `state == RS_OK` whose wanted size differs from their `sc`; the job holds
`{key, ctx, pix (shared_ptr), wantW, wantH}`, so replacing pixels on the UI thread can never free what the scaler
reads. `ScaledImage` becomes `{key, ctx, pxSerial, w, h, px}`; `OnScaledImages` applies it to every entry with that key
whose `pix->serial == pxSerial` and stores it in the table entry, drops stale ones, and **always** sets
`scalingImages = false`. The scaler never checks `docGen` and always posts (an empty list if it stopped early), except
while closing — so the flag can no longer stick (the leak at `loader.cpp:469`).

### 5.4 Measuring (R10)
`MeasureJob` gains `std::vector<uint32_t> idx` (empty = the `from..to` range, as today). In edit mode
`StartMeasure()` measures only blocks with `known == 0`, nearest to the viewport first, in jobs of at most 64 blocks
on one thread; it runs from `TIMER_EDIT_IDLE` (1 s after the last edit) and from `Relayout`. `EditExit` runs the
ordinary full `StartMeasure()`. Reading mode after a swap (a tick) measures only the unknown blocks too, but with more
than 512 of them (a big document still being measured) splits them over `StartMeasure`'s threads in one go.

### 5.5 `EditReparse` — the swap (UI thread, synchronous)

No message is pumped between the first splice of an operation and the end of step 16 (R13): no `EnsureInput`, no
`SendMessage` to another thread, no modal call. `EditReparse` never runs while `g.editModal > 0` (§10.10).

1. Assert `!g.fullPending` and `g.editModal == 0`.
2. `JoinDocReaders()` (§5.1).
3. `Doc nd; nd.baseDir = g.doc.baseDir; ParseMarkdown(nd, g.src.data(), g.src.size(), &opt)` with `opt.wantMap =
   true` and the masks and raw ranges of §6.9 — always the whole source.
4. **Carry pictures** from the render table (§5.2) into `nd.images`. An entry with no table hit whose outer range
   intersects the edited range (the formula or diagram being typed, or bound to the open popup) adopts the old
   pixels of the previously bound image: `pix`, `w`, `h`, `ascent`, `pxFor = old key`, state `RS_OK`,
   `renderFailed = old.renderFailed`; the render for the new key is requested (preview worker when a popup is bound,
   else the picture worker). Such an adoption is never stored in the table under the new key, so a later exact match
   cannot show a/b for `\frac{a}{` (R15). An entry with no table hit whose source the old model shows with pixels
   (the render context changed since, e.g. the text size, or the entry was evicted) adopts that picture the same way,
   in reading mode too, until its own render arrives. `detailsOpen` is carried when the `<details>` group count is
   unchanged.
5. `DiffBlocks(g.doc, nd)` → common prefix `p` and suffix `q` (`p + q ≤ min(nOld, nNew)`) under layout equality:
   equal kind, heading, marker, listLevel, muted, lang, alertTitle, align, indent, details, text, run count and every
   run's `(start − textOff, len, flags, color, link target text, picture key)`, table shape and every cell's text and
   runs, code language name, and picture key + size for `BK_IMAGE`. `gap` and `number` may differ.
6. **Anchor capture** (old geometry, R7). `top = EditRevealTop()` (§12.1); `a = FirstVisible(scrollY + top)`.
   - mode A: `a < p` — nothing above the change moves; `scrollY` stays.
   - mode B: block `p` is visible — remember `pTop = Y[p] − scrollY`.
   - mode C: otherwise — take the first visible unchanged block after the edit, old index `u = max(a, nOld − q)`
     (a suffix block): `a' = u + (nNew − nOld)`, `off = scrollY − Y[u]`. If no suffix block is visible, keep
     `scrollY` (mode A).
7. **New per-block vectors** of size `nNew`: prefix blocks `i < p` and suffix blocks `nNew − q ≤ i` take `cache`,
   `H`, `known`, `hx` from their old index; middle blocks get `cache = nullptr`, `H = BlockHeightEstimate`,
   `known = 0` (hidden blocks `H = 0`, `known = 1`), `hx = 0`. A carried layout of a block with `F_IMAGE` runs is
   deleted (its `H` kept) when an image index before it changed; old middle layouts are deleted; `cachedCount` is
   recounted. When the middle is exactly one table in both documents with the same shape, the new table layout reuses
   the old per-cell `IDWriteTextLayout`s of cells whose text and runs are unchanged (R16).
8. **Install**: `g.doc = std::move(nd)` (the address `&g.doc` is unchanged, so carried `InlineImage` objects stay
   valid); install the vectors; `UpdateColumns()`. `TocAvailable()` is frozen while editing (§12.7), so the columns
   change only on resize; if `textW`/`wideW` changed anyway, fall back to `Relayout()` semantics and go to step 11.
9. `RecomputeY()`; resolve the caret (§6.4); `EnsureLayout(caretBlock)` on `g.doc` — **`LayoutBlock` is never called
   on a Doc other than `g.doc`** (R1); `RecomputeY()` again if its height changed. `Render → EnsureVisible` lays out
   the other visible middle blocks.
10. **Anchor apply**: A — keep; B — `scrollY = Y[p] − pTop`; C — `scrollY = Y[a'] + off`; clamp to
    `[0, MaxScroll()]`; `targetY = scrollY` (a glide in flight stops).
11. Bookkeeping: `jobsPending = 0`, `docSerial++`, `editSerial++`; `lowerText` spliced — the old middle text range is
    replaced by `ToLower` of the new middle text (R16); `FindRefresh()` if find is open (§12.6); reset exactly
    `hoverLink, hoverCode, hoverHBlock, dragHBlock, hbarFlash, hoverHeading, hoverTask, downTask, focusLink, ctxLink,
    ctxImage, tocHover = -1`, `restoreBlock = -1`, `userMoved = true` (never `ClearLayoutCache`, `docH = 0`,
    `scalingImages = false` or `CancelPendingRestore`, R20); `hxSerial++` when an `hx` moved; re-anchor the phantom
    (§6.7).
12. Caret: `TextOfSrc` → `g.selAnchor/selFocus` with normalisation (§6.4); `RevealCaret()` (minimal, §12.1).
13. `StartImages()` for new keys.
14. `UiaDocumentChanged()` and `UiaSelectionChanged()`, debounced 100 ms (`TIMER_EDIT_UI`).
15. With `FASTMD_EDIT_SELFCHECK=1`: `MapSelfCheck`; a failure is counted (`Q_MAP_SELFCHECK` lp 1) and logged.
16. Stats ring (§5.8): parse, carry + diff, install + layout, in µs; `Invalidate()`.

`EditEnter` runs the same function with no text change (the first map parse; the diff keeps every unchanged layout).

### 5.6 Invalidation list

| State | After an edit swap |
|---|---|
| `docSerial` | bumped (FrameKey, TOC). Tests prove "no reload" with `Q_RELOADS`, not `Q_DOC_SERIAL` |
| `editSerial` | bumped (FrameKey, UIA range stamps) |
| `gen`, `docGen` | bumped by `JoinDocReaders` |
| `pixelSerial` | unchanged by the swap; bumped by picture results, preview results, task ticks, failure outlines |
| `hxSerial` | bumped when `hx` is remapped |
| `cache`, `H`, `Y`, `known`, `cachedCount` | carried for prefix and suffix, estimated for the middle (§5.5 step 7) |
| `hx` | remapped by block identity (prefix same index, suffix shifted, middle 0) |
| `lowerText` | spliced (never compared by size, find.cpp:300) |
| `matches`, `curMatch` | `FindRefresh()` without scrolling |
| `toc` | `TocSync` rebuilds only if the (block, level, text) list changed; no re-centre while editing (§12.7) |
| index-keyed UI state, `g_focusS/E` | reset by the list in §5.5 step 11 (`focusLink = -1` clears the focus ring) |
| selection | derived from the source caret |
| pictures | carried through the render table |
| `detailsOpen` | carried by group index when the count is unchanged |
| `fileTime`, `fileSize` | changed only by saves and adoptions (§10) |
| UIA ranges held by clients | stamped with `editSerial`, clamped (§12.8) |
| `numLayouts` | nothing (typography only) |

### 5.7 Big documents (R9, R14, D11, T11)
- Threshold `kEditDeferChars = 262144` characters (`FASTMD_EDIT_DEBOUNCE_CHARS=<n>` overrides; 0 defers every
  eligible keystroke — tests use it on small files).
- **Deferred-eligible** operations only: `OpType` of ordinary characters (§7.3) with a collapsed caret, no phantom,
  no pending format, no selected atom, not in a table cell, not at a break atom, no raw-while-typing; and a
  non-word `OpBackspace` whose cluster lies inside `[burstBeg, focus)` (text typed in this burst). The splice is
  applied to `g.src` at once, `focus` advances arithmetically, and the first deferred keystroke arms
  `TIMER_EDIT_REPARSE` for 150 ms (later keystrokes do not re-arm it), so the view catches up at most every 150 ms plus
  one parse. The stale document is used for nothing.
- **Everything else first calls `EditSync()`** (= run the pending `EditReparse` now): any other key, a mouse press, a
  command, paste, a popup, UIA calls, `FindRefresh`, the context menu, `TIMER_EDIT_IDLE`, queries that need text
  geometry (`Q_CARET`, `Q_SEL_*`, `Q_EDIT_PHANTOM`, `Q_MAP_SELFCHECK`), leaving edit mode and leaving the document.
  Saving needs only `g.src` and does not sync.
- While deferred, typed text becomes visible at the next tick (documented limit for sources above 256 K characters).
- `fullPending` never coexists with edit mode: entry is refused while it is set (§2.1), `OpenDocument` and
  `ReloadDocument` leave edit mode after the flush, and every splice asserts `!g.fullPending`.

### 5.8 Cost budget and statistics (R16, R24, T12)
The budget is measured from `WM_CHAR` to the end of `Present`, Release build, this machine:

| Case | Target |
|---|---|
| typing in a paragraph of `medium.md` (53 KB) | median < 3 ms, p95 < 8 ms |
| typing in a cell of a 200 × 5 table | median < 12 ms (unchanged cell layouts reused) |
| typing in a 700-line code block | median < 10 ms |
| `large.md`, deferred keystroke | < 1 ms; the 150 ms tick (full parse with maps) reported, target < 100 ms |
| autosave of `medium.md` | < 5 ms on the UI thread; sources ≥ 1 M characters save on the save worker (§10.3) |

An always-on ring in `edit.cpp` keeps the last 128 `EditReparse` samples split into parse / carry+diff /
install+layout; `Q_EDIT_STATS` returns them (§13.2). The TOC skips its rebuild when nothing changed and keeps item
layouts; there is no special caret-only frame (a blink repaints a full frame, ≈2–4 ms at 2 Hz).

---

## 6. Caret, selection and the text↔source map (Phase 2a–2b)

### 6.1 State
`EditState` (§3.2) in **source space** is the truth; it survives re-parses. `g.selAnchor/g.selFocus` (text space)
are derived from it after every re-parse and drive drawing, copying and UIA as today. The glue caches the resolved
`TextPos` of the caret, `trailCols` (§6.5) and the phantom geometry. Keyboard and mouse compute a new *text* position,
which is mapped to source with `SrcOfText(…, MAP_CARET)`.

### 6.2 Caret stops, text atoms, object atoms
`CaretStop(doc, pos)` is true when the block is not hidden (folded `<details>`) and not `BS_SYNTH`, and:
- **text blocks** (paragraphs, headings, list-item blocks, code, footnote text, empty items, empty headings, raw
  leaves): every `t` in `[textOff, textOff + textLen]` that is not strictly inside a `SEG_TEXTATOM`, `SEG_OBJATOM` or
  grapheme cluster, and not after the start of a `SEG_SYNTH`;
- **table cells**: every `t` in each cell's range, empty and missing cells included (the cell index disambiguates);
- **block atoms** (`BS_OBJECT`, `BS_RAW`): the block is one stop; arriving there selects the atom;
- **the phantom row** (§6.7).

Clusters come from `ClusterFn`: the app uses `IDWriteTextLayout::GetClusterMetrics` of the caret block's layout; the
tests use a table-driven approximation (surrogate pairs, combining marks U+0300–036F, U+1AB0–1AFF, U+20D0–20FF,
U+FE20–FE2F, variation selectors, ZWJ sequences, regional-indicator pairs). ←/→ step over a text atom or an inline
object atom in one step (never selecting it). Backspace after an inline object atom / Delete before it selects it
(atom id = image index, the caret is hidden and the atom outlined); the next Backspace or Delete deletes its outer
range. Block atoms are selected on arrival by any caret move; ← / → / ↑ / ↓ from a selected block atom go to the
neighbouring stop. Atom ids: inline = image index; block = `0x40000000 | block index` (for `BS_RAW` the `rawId`).

### 6.3 Text → source: `SrcOfText(doc, src, pos, mode)`

`MAP_CARET` (where typed text goes), for a collapsed caret at `pos = {t, b, c}`:
1. `t` strictly inside a `SEG_PLAIN` → `s + (t − seg.t)`.
2. `t` inside an atom or a cluster cannot happen (stops exclude it); callers snap first.
3. Otherwise `t` is a boundary. Let **L** be the segment ending at `t` in the same block/cell, **R** the one starting
   there, and the **format context** the spans (`spans` of the block; in a table only those opening in the cell's
   own source, since a cell with no text shares its offset with the next one) covering the character `t − 1`.
   - **L exists**: start at `L.s + L.sLen`. Walk the closers that follow in the source (spans of the context that end
     at `t`, innermost first). If the context contains a span that is *not enterable* (A, IMG, CODE, LATEXMATH,
     FOOTNOTE_REF, autolinks, `ST_HTML_CODE/KBD/A`), advance past the closer of the **outermost** such span (and hence
     past every span nested in it); enterable spans outside it stay open. Result: typing continues the formatting of
     the character before the caret, but never inside a link, code or formula from its right edge (F12, Word rule).
   - **No L** (block or cell start): start at `R.s`. If an opener of a non-enterable span sits at `t`, the result is
     the `openBeg` of the **outermost** such opener (outside it, inside any enterable span that opens before it);
     otherwise `R.s` (inside every enterable opener at `t`).
   - **Neither** (empty block or cell): the block's or cell's insertion point (§6.3.1).
4. A soft/hard break atom as L carries the flags of its run; it is treated like any other L.

| Before (boundary shown by ‸) | Typing `x` gives |
|---|---|
| `**bold‸** tail` | `**boldx** tail` |
| `plain ‸**bold**` | `plain x**bold**` |
| `‸**Bold** text` (block start) | `**xBold** text` |
| `see ‸[Docs](u)` | `see x[Docs](u)` |
| `[Docs‸](u) more` | `[Docs](u)x more` |
| `‸[Docs](u) are` (block start) | `x[Docs](u) are` |
| ``run ‸`make` `` | ``run x`make` `` |
| `` `make‸` now`` | `` `make`x now`` |
| `**[x‸](u)** y` | `**[x](u)x** y` |
| `[**x‸**](u)` (block end) | `[**x**](u)x` |
| `the $E$‸ is` | `the $E$x is` → fails verification (the formula stops being one) → `the $E$ x is` (§7.3) |

**6.3.1 Insertion points of empty blocks and cells.** Empty paragraph cannot exist (phantom instead). Empty ATX
heading `##`: `beg` (after the `#`s); a space is inserted first when the character before `beg` is not a blank
(`## x`). With a closing sequence (`## ##`, `lineEnd > end`) `beg` sits before it, and the inserted text gets a
trailing space when the character at `beg` is not a blank (`## x ##`, not `## x##`, which is the heading "x##").
Empty footnote definition `[^2]:`: `beg` of its record (after `]:` and the blanks); a space first when none follows
the colon.
Empty list item `-` / `1.` / `- [ ]`: `beg` of the `BS_EMPTYITEM` record; a space is inserted first when none follows
the marker. Empty fenced block with an empty content line: the start of that line after its container prefix;
`BS_NOCONTENT` (```` ```⏎``` ````): insert `E + ContPrefix` at `beg` (the fence line's EOL) first (F4). Empty table
cell: right after the single blank that follows its opening pipe (right after the pipe if no blank follows); when the
character there is `|`, the inserted text gets a trailing space. Missing cell: §7.10.

**Other modes.** `MAP_OUTER_START(t)` = the source position before every opener of spans with `tBeg == t`;
`MAP_OUTER_END(t)` = after every closer of spans with `tEnd == t`; `MAP_INNER_START(t)` = after every opener at `t`.
Selection deletion uses the outer modes and then re-balances (§7.9).

### 6.4 Source → text: `TextOfSrc(doc, src, s, dir, &trailCols)` and normalisation
1. Find the block whose `[line, outerEnd]` contains `s` by binary search over `blockOrder` (footnote definitions are
   found in source order; do not assume text order).
2. Inside the block, binary-search its segments by `s`. Inside a `SEG_PLAIN` → exact. Inside an atom, a delimiter,
   a marker or a prefix gap → the nearest text boundary in direction `dir` (after an insertion: the boundary after
   the inserted text). In the trailing blanks of a line → that line's last text position with `trailCols` (§6.5).
3. No block contains `s` (a blank-line gap, an invisible reference definition): if the phantom's `anchorSrc` equals
   `s` → the phantom; else the nearest block boundary in direction `dir`. Never an implicit phantom.

**Normalisation** (F7): after every re-parse `focus := SrcOfText(TextOfSrc(focus, dir), MAP_CARET)` (and the anchor
likewise), except while the caret is in a phantom, stands in trailing blanks (§6.5), has a pending format, or a
deferred burst is active. So after typing a space before a soft break, the next character goes where the caret is
drawn.

### 6.5 Whitespace the renderer drops (F1, UX-1)
- **Trailing blanks.** The spaces and tabs between a source line's last segment and its EOL are invisible (md4c trims
  them). The caret may stand anywhere in that run: `TextOfSrc` returns the line's last text position with `trailCols`
  = the width in columns of `g.src[segEnd, focus)`, and the caret is drawn that many space-advances (measured in the
  run's font) further right, clamped to the block box. End and → reach the end of the run; ←, Backspace and Delete act
  on those source characters one at a time. In a cell the run ends at the separator pipe.
- **Leading blanks.** A Space or Tab typed at the first content position of a block, a cell or a phantom is dropped
  (outside code blocks): it would be stripped, or four of them would make indented code.
- **Tab in a paragraph or heading** does nothing, except in a paragraph whose previous block in source order belongs
  to a list item: Tab indents the paragraph into that item (§7.8).
- Test: typing `a` + Space at the end of a paragraph moves `Q_CARET` x right, and the file gets the space.

### 6.6 Soft and hard breaks (F7)
- A soft break is a text atom drawn as a space; a hard break (`\`+EOL, two spaces+EOL, `<br>`) is a text atom drawn as
  a line break. The caret stands at either edge, never inside.
- Whitespace typed at the **left** edge of a soft break moves the caret across it and inserts nothing (the break
  already renders as that space); at the **right** edge it is dropped (§6.5). Typing therefore never leaves trailing
  blanks before a line end, and never two (no accidental hard break).
- A `\` typed at the left edge of a soft or hard break is written `\\`.
- Enter, Shift+Enter and list Enter at either edge of a break atom **replace** the atom's source (§7.6).
- Backspace at the right edge / Delete at the left edge of a break atom removes its whole source (lines join with
  nothing between them).
- Shift+Enter at the end of a block creates a `PH_BREAK` phantom (§6.7): nothing is written until text follows.

### 6.7 Phantoms (view-level rows, R2)

Markdown cannot store an empty paragraph, so the editor shows one without touching the source. `g.doc` is never
changed for a phantom.
- **Kinds.** `PH_AFTER` (a new paragraph after the anchor block), `PH_BEFORE` (before it), `PH_BREAK` (a pending hard
  line break at the anchor's content end). At most one exists.
- **Anchors** (F17): `PH_AFTER` at the anchor's `outerEnd` (after a closing fence, setext underline, ATX closing
  sequence); `PH_BEFORE` at the anchor's `line` + its container prefix (so before a heading's `#`; list items never
  get a `PH_BEFORE` phantom — Enter at an item's start writes an empty item, §7.6); `PH_BREAK` at the anchor's
  content end. After every re-parse the anchor block is found again from
  `anchorSrc` (adjusted by the splices of the operation, like the caret).
- **View.** `RecomputeY` adds `phantomH` (one body line + the paragraph gap, 16) after (before) `phantomBlock`;
  `HitTestDoc` maps that band to the phantom; `CaretGeom` puts the caret at the content left edge of the phantom's
  container depth (`phantomX`). A styled phantom draws its decoration only: a bullet, number `1.`, an empty task box, a
  quote bar, or the placeholder «Заголовок 2» / "Heading 2" in `P_MUTED` at the heading size.
- **Created by:** Enter at the end of a paragraph/heading/quote block or at the start of a non-empty non-item block
  (§7.6); Enter in an emptied list item (§7.6); Ctrl+Enter anywhere; Down/→ at the last stop when the last block is
  a code block, table or object atom; Up/← at the first stop when the first block is one; a click below the last
  block; Enter on a selected HR (UX-3, F18).
- **Lifetime** (UX-7): it survives while the caret is in the phantom **or in its anchor block**; it is dropped when the
  caret enters any other block, on leaving edit mode, and on undo/redo (unless the step recorded it). Up from the start
  of the anchor block enters a `PH_BEFORE` phantom; Down from the anchor's last line enters a `PH_AFTER` phantom.
- **Keys in an empty phantom.** Enter: with `depth > 0` the phantom moves out one container level (from a quote to
  after the whole quote; from a list continuation to the parent level, with blank lines on both sides, F18, UX-8); at
  depth 0 with a style, the style is cleared; otherwise nothing. Backspace: with `depth > 0` it moves out one level;
  at depth 0 it drops the phantom and the caret goes to the end of the previous block (for `PH_BEFORE`: stays at the
  anchor's start). Delete drops it and the caret goes to the start of the next block. Tab in a phantom after a list
  item gives it that item's continuation prefix. Commands: §8.1.
- **Materialisation** (the first typed character, paste or insert): one splice, one undo step:

| Kind | Splice at `anchorSrc` | Example |
|---|---|---|
| `PH_AFTER` | `E + blankPrefix + E + prefix + stylePrefix + text`, plus `E + blankPrefix` after it when the next source line is not blank | `abc‸⏎⏎def` Enter, type `x` → `abc⏎⏎x‸⏎⏎def` |
| `PH_BEFORE` | `stylePrefix + text + E + blankPrefix + E + prefix` | `# Title` Enter at its start, ↑, type `x` → `x‸⏎⏎# Title` |
| `PH_BREAK` | `\` + `E + prefix + text` (ATX heading: `<br>` + text) | `abc` Shift+Enter, type `x` → `abc\⏎x‸` |

`stylePrefix` is `#…# ` for a heading style, `- ` / `1. ` / `- [ ] ` for list styles, `> ` for a quote style.
Quote example: `> abc‸` Enter, type `x` → `> abc⏎>⏎> x‸`; a second Enter in an empty phantom inside the quote pops
out: `> abc‸` Enter, Enter, type `x` → `> abc⏎⏎x‸`.

### 6.8 Navigation (`KeyMoveCaret` in view.cpp)
Reuses the static `MoveChar`/`MoveWord`/`MoveLine`/`LineEdge`/`SnapPos` (view.cpp:1285-1362) with these changes:
- stops per §6.2 (block and cell aware); `MoveChar` stops at cell edges and steps into the next cell;
- `MoveLine` skips inter-block gaps by probing at `Y[i] + 1` / `Y[i−1] + H[i−1] − 1`; keeps `wantX`;
- Home/End in a table cell go to the cell's start/end; Ctrl+↑/↓ go to block starts;
- document edges create phantoms next to non-text first/last blocks (§6.7);
- PgUp/PgDn move by `ViewH() − 56 − EditRevealTop()`.
After every move: `RevealCaret()` (minimal), `EditCaretMoved()` → blink restart, pending format cleared, undo
coalescing broken, phantom lifetime checked, link bubble updated, `UiaSelectionChanged()` debounced.

### 6.9 Raw-while-typing (F22)
Typing can turn the caret's own block into an object atom mid-word. The editor keeps such a line editable until the
caret leaves it:
- **HTML block start** (`<b>` alone on a line, `<div>`, `<!--`, …): when a typing splice made the caret's block
  `BS_HTML` (it was a text block), the first `<` of that line is **masked** (§4.3) while the caret stays on the line:
  md4c sees plain text and the line renders literally.
- **`<!--`** stays masked while no `-->` follows it in the source (so the rest of the document does not vanish); the
  masked text is drawn in `P_ALERT_CAUTION` as a warning.
- **Picture-only paragraph** (`![logo](logo.png)`) and **display formula** (`$$x$$`): when typing turned the caret's
  block into `BK_IMAGE`, its source range is passed as a **raw** range (§4.4): the block is shown as its raw source in
  code style, with PLAIN segments, until the caret leaves it.
- Detection costs a second parse only on the keystroke that changes the block kind. `Q_EDIT_RAW` reports whether an
  override is active.

---

## 7. Editing operations (Phases 2a–2b)

Every operation is a pure `Op*` in `editcore`/`editops`: splices + the state after. The glue applies the splices,
runs `EditReparse`, checks `verify` expectations (§7.5), pushes one undo step (§11), arms autosave, updates the title,
and shows the first-edit toast. After every splice the whitespace normalisation (§7.5 step 1), empty-span removal, the
merge rule and block-syntax escaping (§7.4) run as part of the same step.

### 7.1 The splice primitive (D12)
- `ApplySplice(at, len, text)` asserts `!g.fullPending`. It **refuses** (logs, toast «Правка отклонена» / "Edit
  refused", nothing changes) when `at` or `at + len` would split a surrogate pair or a CRLF.
- Lone surrogates in inserted text become U+FFFD; text that came from the file is never touched.
- A splice touching a `SEGF_SPLITTAB` atom first rewrites that tab as spaces of its full width (same undo step).
- Every splice is recorded `{at, removed, inserted}` in the current step. Inserted line ends are the caller's job
  (§7.2); no operation inserts a bare `\n` into a CRLF line.

### 7.2 Line ends and container prefixes (D15)
- **`g.eol`** at edit entry: count CRLF, lone LF and lone CR in the disk text; the most frequent wins; ties prefer CRLF,
  then LF. A file with no line end uses LF (documented choice).
- **`E` at a point** = `LineEol(src, s)`: the ending of the source line that contains `s`; the last line (no ending)
  uses `g.eol`. Structural operations never add or remove the file's final line end.
- **`ContPrefix(b)`**: compose the container chain of `b` outer → inner: a quote → `>` plus one blank if `b`'s own
  first line has a blank after that `>` (else none); a list item → blanks up to the item's `contentCol` (columns from
  the line start, each `>` and its blank counting); a footnote → nothing. **`BlankPrefix(b)`** = `ContPrefix(b)` with
  trailing blanks removed.

| Block | `ContPrefix` | `BlankPrefix` |
|---|---|---|
| top level | `` | `` |
| in `> quote` | `> ` | `>` |
| item `- a` | two spaces | `` |
| item `12. a` | four spaces | `` |
| item in a quote `> - a` | `>   ` | `>` |
| nested item `- a⏎  - b` (the `b` block) | four spaces | `` |
| quote in an item `- a⏎  > q` (the `q` block) | `  > ` | `  >` |

- **Marker line prefix** of an item = `ContPrefix` of its parent chain; the **marker text** keeps the item's own
  spacing (1–4 blanks) and kind (`-`/`+`/`*`, `N.`/`N)`, task box).

### 7.3 Typing (`OpType`)
1. A selected atom: nothing (hint pill, §2.10). A phantom: materialise it (§6.7). A selection: replace (§7.9).
2. The insertion point: `SrcOfText(caret, MAP_CARET)`, or `focus` itself when the caret stands in trailing blanks.
3. Context transforms:
   - Space/Tab at the first content position of a block, cell or phantom (outside code) → dropped (§6.5);
   - whitespace at a soft-break edge → §6.6;
   - `\` directly before a line end (outside code) → `\\`;
   - `|` in a table cell → `\|` (also inside a code span: GFM needs the escape there too);
   - the first character of an empty ATX heading or empty list item → a blank is inserted before it if none follows
     the marker;
   - **pending format** (`pendOn`/`pendOff` set by a toggle with no selection, §8.2): the first **non-whitespace**
     character materialises it through §7.5 (`pendOn = BOLD`, type `x` → `**x‸**`); whitespace typed while pending is
     inserted plain and the pending format stays (F11); a character for which §7.5's flanking check fails is inserted
     plain and the format stays pending (`foo‸`, Ctrl+B, `(x` → `foo(**x‸**`, never `foo**(**`);
   - **sticky end of a span**: whitespace typed at the end of an enterable span (the caret inside, before its closer)
     is placed *after* the closer (`**bold** ‸`) and marks the span sticky; the next non-whitespace character extends
     that span over the whitespace and itself (`**bold x‸**`) through the merge rule of §7.5 (Word keeps typing bold
     after a space; Markdown cannot hold `**bold **`).
4. Splice `[s, s) := text`.
5. **Verification** (ordinary characters only — the Markdown-significant characters ``\ ` * _ ~ $ [ ] ( ) ! < > # | =
   + - : &`` are inserted verbatim, Principle 4): after the re-parse the rendered text must equal the old text with the
   character at `t`, and the old neighbours must keep their format flags. On failure the splice is replaced (not
   recorded twice) by the first candidate that verifies: the other side of the adjacent delimiters (after the closers
   / before the openers at `s`); then, next to a `$` delimiter, the character with a separating blank. If none
   verifies, the original splice stays (typing is never blocked). Examples: `**API‸**s` + `.` → `**API**.‸s`;
   `the $E$‸ is` + `x` → `the $E$ x‸ is`. In deferred mode (§5.7) verification is skipped.
6. Undo coalescing (§11), autosave, title, first-edit toast; §7.4 escaping (the trigger typed at a visible block start
   is exempt).

### 7.4 Escaping accidental block syntax (F8)
After every splice that is **not** (a) typing at a block's visible text start or in a phantom, (b) a paste, or (c) a
command whose purpose is to write that syntax (block style, list, quote, code block, inserts), every source line whose
content start changed (the lines the splice created, and the line after a removed line end) and that belongs to a
text block (not code, HTML, front matter, raw leaf, table row) is checked at its content start:

| Trigger at the content start | Escaped as |
|---|---|
| `#`…`######` followed by a blank or the line end | `\#` |
| `-`, `+`, `*` followed by a blank or the line end | `\-`, `\+`, `\*` |
| digits + `.` or `)` followed by a blank or the line end | `1\.`, `1\)` |
| `>` | `\>` |
| three or more `` ` `` or `~` | `` \` ``, `\~` |
| a setext underline (`=`+ or `-`+ alone) directly under a paragraph line | `\=`, `\-` |
| an HR pattern (`***`, `---`, `___` with blanks) | `\*`, `\-`, `\_` |
| `<` that starts an HTML-block pattern | `\<` |
| `[label]:` / `[^label]:` shape | `\[` |
| four or more columns of indentation | the indentation is removed |

The re-parse verifies that the escaped line renders the same characters. Examples:
- `The value⏎is high`, caret before `is`, type `- ` → `The value⏎\- is high` (still one paragraph).
- `I like C‸# and F#.` Enter → `I like C⏎⏎\# and F#.`
- `# 1. Introduction`, Backspace at its text start → `1\. Introduction` (a paragraph, not a list).
- `⟦I use C⟧# daily` Delete → `\# daily`.
- `see ‸- this` Shift+Enter → `see\⏎\- this`.

### 7.5 The delimiter emission routine (F9, F10)
All delimiters the editor writes (toggles, balancing, splits, pending formats, link commands) go through
`EmitSpan(type, sA, sB)` over a content range:
1. **Whitespace outside** (F10): `sA` moves forward over whitespace (Unicode Zs, tab, line end), `sB` back; the same
   normalisation runs after every splice for every EM/STRONG/DEL/LATEXMATH/HTML-format span whose content now starts
   or ends with whitespace (`**bold x**` Backspace after `x` → `**bold** ` is written as `**bold**` + ` `).
2. **Merge** (F9-1): a span of the same type whose closer ends at `sA` (or, when sticky, is separated from `sA` only by
   whitespace) loses its closer and the closer is written at `sB`; a span whose opener starts at `sB` loses its opener
   and it is written at `sA`. Never two runs of the same type side by side (`**a****b**` is written `**ab**`).
3. **Delimiter choice**: `**`, `*`, `~~`, `$`; code: a backtick run longer than any inside, padded with a blank when
   the content starts or ends with `` ` ``. `_` is never written; a surviving `_` span that becomes intraword has both
   of its delimiters rewritten to `*` (F9-2).
4. **Flanking** (F9-3) against the real neighbours: an opener must be left-flanking (next char not whitespace; if it is
   punctuation, the previous char is whitespace, punctuation or the line start), a closer right-flanking (mirror).
   `$`: an alphanumeric outside neighbour gets a separating blank (F9-4). A failing edge is extended outward to the
   word boundary if that crosses no span edge and no block edge; if it still fails the operation is refused with the
   toast «Здесь это форматирование не получится» / "This formatting cannot be applied here".
5. **Verification** (F9-5): `EditResult::verify` lists, for every emitted span, the source range that must render with
   the flag and the neighbours that must keep theirs. After the re-parse the glue checks it; on failure the step is
   reverted (not recorded) and the refusal toast is shown.

Examples: `don⟦'t⟧` Ctrl+B → `**don't**` (the edge moved to the word start); `**bold1**‸` + `**bold2**` joined by
Backspace → `**bold1bold2**`; `_ab⟦cd⟧ef_` Ctrl+I (remove italic from `cd`) → `*ab*cd*ef*`; `cost‸` Ctrl+M →
`cost $x$`.

### 7.6 Enter, Shift+Enter, Ctrl+Enter

**Split rule** for text blocks (F2): at the split point, whitespace adjacent to it on both sides is deleted; every span
open at the split point is closed before the break and reopened after it with the same delimiters (innermost first; a
link duplicates its closer: `[link](u)` / `[text](u)`; a code span reuses its backtick run); then
`E + BlankPrefix + E + ContPrefix` (+ marker for list items) is inserted; §7.4 checks the new line; the caret goes to
the second half's text start. At a break atom the atom's source is replaced instead of inserting beside it (F7).

**Enter**

| Container | Before | After |
|---|---|---|
| paragraph | `ab‸cd` | `ab⏎⏎‸cd` |
| paragraph, inside bold | `**bold ‸text**` | `**bold**⏎⏎**‸text**` |
| paragraph, inside a link | `[link ‸text](https://x)` | `[link](https://x)⏎⏎[‸text](https://x)` |
| paragraph, inside code | `` `a ‸b` `` | `` `a`⏎⏎`‸b` `` |
| paragraph end / start | `abcd‸` / `‸abcd` | source unchanged: `PH_AFTER` phantom / `PH_BEFORE` phantom with the caret staying at `‸abcd` (UX-7) |
| at a soft break (either edge) | `one‸⏎two` | `one⏎⏎‸two` |
| quote | `> ab‸cd` | `> ab⏎>⏎> ‸cd` |
| nested quote | `> > ab‸cd` | `> > ab⏎> >⏎> > ‸cd` |
| ATX heading | `## Ti‸tle ##` | `## Ti ##⏎⏎‸tle` (closing sequence stays with the heading; the second half is a paragraph) |
| ATX heading end / start | `## Title‸` / `## ‸Title` | `PH_AFTER` / `PH_BEFORE` phantom |
| setext heading | `Ti‸tle⏎=====` | `# Ti⏎⏎‸tle` (converted to ATX first) |
| multi-line setext | `Line one⏎Li‸ne two⏎---` | `## Line one Li⏎⏎‸ne two` (lines joined: soft break → blank, hard break → `<br>`) |
| tight bullet item | `- ab‸cd⏎- x` | `- ab⏎- ‸cd⏎- x` |
| loose bullet item | `- ab‸cd⏎⏎- x` | `- ab⏎⏎- ‸cd⏎⏎- x` |
| ordered item | `1. ab‸cd` / `9) ab‸cd` | `1. ab⏎2. ‸cd` / `9) ab⏎10) ‸cd` (later numbers untouched: md4c numbers from the start) |
| task item | `- [x] ab‸cd` / `1. [ ] ab‸cd` | `- [x] ab⏎- [ ] ‸cd` / `1. [ ] ab⏎2. [ ] ‸cd` (own marker kind; the new box unticked, F16) |
| item end | `- abcd‸` | `- abcd⏎- ‸` (an empty item is representable, so it is written) |
| item start (non-empty) | `- ‸abcd` | `- ⏎- ‸abcd` (an empty item before; the caret stays with its text) |
| item with children | `- ab‸cd⏎  - child` | `- ab⏎- ‸cd⏎  - child` |
| empty last item | `- a⏎- ‸` | `- a` and a `PH_AFTER` phantom at the list's own level |
| empty middle item | `- a⏎- ‸⏎- b` | `- a⏎- b` and a phantom between; typing `x` → `- a⏎⏎x‸⏎⏎- b` (outer prefix, blank lines on both sides, F18) |
| empty nested item | `- a⏎  - ‸` | `- a⏎- ‸` (outdented one level) |
| second paragraph of a loose item | `- a⏎⏎  ab‸cd` | `- a⏎⏎  ab⏎⏎  ‸cd` |
| item in a quote | `> - ab‸cd` | `> - ab⏎> - ‸cd` |
| alert body | `> [!NOTE]⏎> ab‸cd` | `> [!NOTE]⏎> ab⏎>⏎> ‸cd` |
| fenced code | ```` ```⏎co‸de⏎``` ```` | ```` ```⏎co⏎‸de⏎``` ```` |
| fenced code, indented line | ```` ```⏎    return 1‸⏎``` ```` | ```` ```⏎    return 1⏎    ‸⏎``` ```` (the line's code indentation is repeated) |
| fenced code in a list (F4) | ```` - x⏎  ```⏎  co‸de⏎  ``` ```` | ```` - x⏎  ```⏎  co⏎  ‸de⏎  ``` ```` (indentation counted after the container prefix) |
| code, end of the last line | ```` ```⏎foo‸⏎``` ```` | ```` ```⏎foo⏎‸⏎``` ```` (the empty last line stays visible, F4) |
| indented code | `    co‸de` | `    co⏎    ‸de` |
| table cell, not the last row | `\| a‸ \| b \|` | caret to the same column of the next row; source unchanged |
| table cell, last row | `\| x‸ \| y \|` | a row `\|  \|  \|` (container prefix first) is appended; caret in its cell of that column |
| table, empty last row | caret in `\|   \|   \|` | the row is deleted; `PH_AFTER` phantom after the table (UX-3) |
| footnote definition | `[^a]: No‸te` | as Shift+Enter: `[^a]: No\⏎‸te` (definitions are one paragraph, F5) |
| selected object atom | — | opens its popup; a selected HR: `PH_AFTER` phantom |
| empty phantom | — | §6.7 |

**Shift+Enter**

| Container | Before | After |
|---|---|---|
| paragraph | `ab‸cd` | `ab\⏎‸cd` |
| quote | `> ab‸cd` | `> ab\⏎> ‸cd` |
| list item | `- ab‸cd` | `- ab\⏎  ‸cd` |
| at a soft break | `one‸⏎two` | `one\⏎‸two` (the soft break becomes a hard break) |
| block end | `abcd‸` | `PH_BREAK` phantom; typing `x` → `abcd\⏎x‸` |
| ATX heading | `# Ti‸tle` | `# Ti<br>‸tle` (UX-20) |
| setext heading | `Ti‸tle⏎===` | `# Ti<br>‸tle` |
| table cell | `\| a‸b \|` | `\| a<br>‸b \|` |
| code block | as Enter | |
| footnote definition | `[^a]: No‸te` | `[^a]: No\⏎‸te` |

**Ctrl+Enter** (`CMD_NEW_PARAGRAPH`): a `PH_AFTER` phantom after the current block at the block's own container level
(in a list item: a continuation paragraph of the item; after a table, code block or atom: after it). Enter in that
empty phantom pops a level (F18, UX-3).

### 7.7 Backspace and Delete (F19)
Inside a block: delete one cluster, text atom or trailing-blank character (Ctrl: to the word boundary); an inline
object atom is selected by the first press and deleted (its outer range) by the second. A span emptied by the deletion
loses its delimiters (`****`, `[]()`, ``` `` ```); §7.5 normalisation and merge run (`*a* ‸*b*` Backspace →
`*ab*`). A deletion that would leave a setext heading without content first turns it into an empty ATX heading of
the same level (`T‸⏎---` Backspace → `##‸`, never `⏎---`, which is an HR) (F17).

**Backspace at the start of block B** (first matching row wins; P = the previous block in source order):

| B | P | Action | Example |
|---|---|---|---|
| any | none | nothing | |
| phantom | — | §6.7 | |
| ATX heading | any | the `#…# ` prefix and the closing sequence are removed → paragraph; §7.4 | `# ‸1. Intro` → `‸1\. Intro` |
| setext heading | any | the underline line is removed → paragraph | `‸Title⏎===` → `‸Title` |
| empty ATX heading | any | its line (and one separating blank line) is deleted; caret to P's end | `abc⏎⏎#‸` → `abc‸` |
| first block of a list item, tight list, P in the same item tree | text | the marker becomes a hard break inside P's item (UX-21) | `- молоко⏎- ‸хлеб` → `- молоко\⏎  ‸хлеб` |
| same, loose list | text | the marker becomes P's item continuation indent (F16) | `- a⏎⏎- ‸b` → `- a⏎⏎  ‸b` |
| first block of the first item of a top-level list | outside the list | marker removed → paragraph; a blank line is inserted before the next item if any; §7.4 (F16) | `1. ‸a⏎2. b` → `‸a⏎⏎2. b` |
| first item of a nested list | the parent item's block | hard break into the parent item (tight) / continuation paragraph (loose) | `- p⏎  - ‸a` → `- p\⏎  ‸a` |
| empty list item | any | its line and line end are deleted; caret to P's end | `- a⏎- ‸` → `- a‸` |
| first content block of an alert | the alert title | the alert becomes a plain quote: the tag line (or the tag and its blanks on a one-line alert) is removed | `> [!NOTE]⏎> ‸Text` → `> ‸Text` |
| first block of a quote | any | one `>` level (and one following blank) is stripped from **every** line of B (F15) | `> ‸line one⏎> line two` → `‸line one⏎line two` |
| footnote definition | any | nothing (F5) | |
| code block, empty | any | the whole fenced block is deleted; caret to P's end | |
| code block, non-empty | any | caret to P's last stop | |
| table cell | — | caret to the end of the previous cell (first cell: to P's last stop); nothing deleted | |
| text block | text block | **join** (F6): delete `[P.end, B.beg)` when that gap holds only blanks, line ends and container prefixes; joining into an ATX heading turns B's soft breaks into blanks and hard breaks into `<br>` (F15); invisible lines in the gap (reference/footnote definitions, comments, hidden HTML) are moved after the joined block | `abc⏎⏎‸def` → `abc‸def`; `# Head⏎⏎‸l1⏎l2` → `# Head‸l1 l2`; `See [x].⏎⏎[x]: u⏎⏎‸Next` → `See [x].‸Next⏎⏎[x]: u` |
| text block | code block or table | caret to P's last stop; nothing deleted | |
| text block | object atom (HR, picture, formula/diagram block, HTML block, front matter) | the first press selects P, the second deletes it (its lines and one separating blank line) | |

**Delete at the end of block B** (N = the next block in source order):

| B | N | Action | Example |
|---|---|---|---|
| any | none | nothing | |
| phantom | — | dropped; caret to N's start | |
| B or N is a footnote definition | — | nothing | |
| text block | text block | join N into B under the same whitespace-only rule; N's prefix, `#` marks, list marker and quote markers go with the gap; into an ATX B N's breaks become blanks / `<br>`; invisible lines moved after the result | `abc‸⏎⏎# Head` → `abc‸Head`; `- a‸⏎- b` → `- a‸b` |
| text block | code block or table | caret to N's first stop | |
| text block | object atom | the first press selects N, the second deletes it | |
| table cell | — | caret to the start of the next cell | |
| code block, end of last line | any | caret to N's first stop | |

Joins happen only between text blocks, in source order.

### 7.8 Tab and Shift+Tab

| Context | Tab | Shift+Tab |
|---|---|---|
| list item (caret or selection in items) | nest under the previous sibling: every line of the item and its children gains `sibling.contentCol − item.markerCol` columns of blanks; no previous sibling → nothing; an item that becomes the first of a nested ordered list is renumbered `1.` (F16). `1. a⏎2. ‸b` → `1. a⏎   1. ‸b` | outdent: every line loses `item.markerCol − parent.markerCol` columns; the following siblings become children of the outdented item (indented to its new content column) and are renumbered from `1.` (F16). `1. a⏎   1. x⏎   2. ‸y⏎   3. z⏎2. b` → `1. a⏎   1. x⏎2. ‸y⏎   1. z⏎2. b`. At the top level the item becomes a paragraph (as Backspace on a first item) |
| paragraph whose previous block (source order) is in a list item | the paragraph joins that item as a loose paragraph: its lines gain the item's content indent (UX-1) | nothing |
| other paragraph, heading | nothing | nothing |
| code block | 4 blanks at the caret (a `\t` when the block's indentation uses tabs); a multi-line selection indents every selected line after its container prefix | removes up to 4 leading blanks or one tab from the current / selected lines, after the container prefix |
| table cell | next cell with its content selected (UX-24); in the last cell: a new row, caret in its first cell | previous cell, content selected |
| phantom after a list item | the phantom takes the item's continuation prefix | nothing |

Indentation is counted in columns with tab stops of 4 from the line start; inserted indentation is blanks (F16).

### 7.9 Deleting and replacing a selection (F6, F13, F20)
`OpDeleteSelection`, with `A < B` the text ends in blocks `bA`, `bB`:
1. **One block (one cell).** `sA = MAP_OUTER_START(A)`, `sB = MAP_OUTER_END(B)`; delete `[sA, sB)`; re-balance: a span
   whose opener is inside and closer outside gets its opener re-emitted at `sA` (a link: `[`); a span whose closer is
   inside and opener outside gets its closer re-emitted at `sA` (a link: its whole closer text); spans inside go,
   spans around stay. Then empty spans are removed, §7.5 steps 1–2 run, whitespace left at the block's content start
   is removed, and §7.4 runs. `x ⟦**bold**⟧ y` → `x ‸ y`; `**⟦foo ⟧bar**` → `‸**bar**`; `a *b⟦c* d⟧e` → `a *b*‸e`;
   `a <b>b⟦old</b> c⟧` → `a <b>b</b>‸` (HTML pseudo-span, F25).
2. **Several blocks** — "covers whole blocks" means *the range crosses the block's end* (F13): `bA` loses the text from
   `sA` to its content end (balanced; its prefix stays); blocks strictly between lose their whole lines, except
   invisible lines inside the range (reference and footnote definitions, comments, hidden HTML), which are kept (F6);
   `bB` loses the text from its content start to `sB` (balanced) and its rest is joined into `bA` by the join rule of
   §7.7. A `bB` (or `bA`) that is a table has its covered cells cleared (no pipe removed; a fully covered table is
   deleted); a code, HTML or front-matter block is clamped to its covered content lines and its fence line goes only
   when the whole block is covered (F6).
3. **One table, several cells**: each covered cell is cleared separately; no pipe is removed (F20).
4. The caret collapses at `sA`'s new position.

`OpReplaceSelection(text)` (typing or paste over a selection, F13): the enterable spans that cover the first selected
character become a pending format, the selection is deleted as above, and `text` is inserted with that format through
§7.5 (merging with the surviving pieces). The first block's prefix always stays. `**⟦bold⟧** text` + `strong` →
`**strong‸** text`; `# ⟦Title⟧` + `New` → `# New‸`.

### 7.10 Tables (F20, UX-10)
- A typed `|` in a cell is written `\|`; pasted line ends become `<br>` and pasted `|` becomes `\|`.
- **Missing cell** (a short row): its insertion point is the end of the row line. Typing first completes the row: a
  missing trailing pipe is appended (` |`), then for every missing cell up to the target ` <text or nothing> |`.
  `| a | b |⏎|---|---|⏎| x |`, type `y` in the second cell of row 3 → `| x | y |`.
- A deletion across cells clears each cell separately (§7.9).
- New rows are written as the table's container prefix + `|` + `  |` per column.
- Empty header cells show «Столбец N» / "Column N" in `P_MUTED` in edit mode only; nothing is written (UX-24).
- Enter, Tab, Shift+Tab: §7.6, §7.8. Table commands: §8.9.

### 7.11 Copy, cut and paste (F21, UX-10, UX-14, T25)
- **Copy** (Ctrl+C, both modes): the existing rich formats (`CF_UNICODETEXT` rendered text, HTML, RTF) plus, in edit
  mode, the private format `RegisterClipboardFormatW(L"FastMD Markdown")`: the **balanced slice** — the selection's
  source with the delimiters of cut spans re-emitted (`**br⟦own** fox⟧` → `**own** fox`); fully covered blocks with
  their markers and container prefixes relative to the selection's common container; partial first and last blocks as
  text. Encoding: UTF-16LE, NUL-terminated, line ends `g.eol`.
- **Copy as Markdown** (Ctrl+Shift+C): reading mode — whole source lines as today through `srcMap`, fixed so a
  selection before the first chunk no longer copies the whole file (`copy.cpp:196`) and the file's first character is
  no longer dropped (`copy.cpp:203`); edit mode — the balanced slice.
- **Cut** (Phase 2b): copy, then `OpDeleteSelection`; one step, kind CUT.
- **Paste** (Phase 2b; the private format and files in 3b), sources in this order:
  1. the private format;
  2. `CF_HDROP` with picture files → `![stem](dest)` per file, blank-separated (§8.8); other files → toast «Вставить
     можно только картинки» / "Only pictures can be pasted";
  3. `CF_UNICODETEXT`, taken as Markdown source;
  4. only a bitmap (`CF_DIB`) → toast «Вставка картинок из буфера пока не поддерживается — перетащите файл сюда» /
     "Pasting pictures from the clipboard is not supported yet — drop the file here".
- **Transformation** (`OpPaste`): line ends normalised to `E` (D15); over a selection → replace (§7.9); lines 2…n get
  the target block's `ContPrefix` (blank lines `BlankPrefix`); in a code block the pasted indentation is kept after
  the prefix; in a table cell line ends → `<br>`, `|` → `\|`; in an ATX heading line ends → blanks; lone surrogates →
  U+FFFD; no §7.4 escaping (pasted source is intentional). One step, kind PASTE.
  Examples: `one⏎⏎two` into `> quo‸ted` → `> quoone⏎>⏎> two‸ted`; into `| a‸ |` → `| aone<br><br>two‸ |`; into
  `# Ti‸tle` → `# Tione  two‸tle`.

---

## 8. Formatting commands (Phase 3a, `editops.cpp`)

Each command is one undo step (kind FORMAT, or STRUCT for inserts), applied back to front, then re-parsed and verified.

### 8.1 Context matrix (UX-9)
✓ = enabled, — = disabled (button greyed, tooltip «… — недоступно …»), "after" = the block is inserted after the
current block (§8.8), "style" = sets the phantom's style (§6.7).

| Caret in | B/I/S/Code | Link | Style (P/H1–6) | Lists | Quote | Code block | Table | Formula inline | Formula block, Diagram, HR | Image | Table ops | Style button shows |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| paragraph, heading, item block, quote block | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ (wraps the block) | after | ✓ at the caret | after | ✓ inline at the caret | — | Текст / Заголовок n |
| code block | — | — | — | — | — | ✓ (unwraps) | after | — | after | after | — | Код / Code |
| table cell | ✓ | ✓ | — | — | — | after the table | after the table | ✓ | after the table | ✓ inline | ✓ | Таблица / Table (greyed) |
| footnote definition text | ✓ | ✓ | — | — | — | — | — | ✓ | — | ✓ inline | — | Сноска / Footnote (greyed) |
| selected object atom | — | — | — | — | — | after | after | — | after | after | — | — |
| phantom | ✓ (pending format) | ✓ | style | style | style | ✓ (becomes an empty fence) | ✓ (becomes the table) | ✓ | ✓ (becomes the block) | ✓ | — | the phantom's style |
| raw leaf (§6.9) | — | — | — | — | — | — | — | — | — | — | — | — |

A selection over several blocks applies inline toggles per piece (§8.2), style/list/quote to every text block it
touches (§8.4–8.6), code block to all of them; inserts go after the last touched block. `Q_EDIT_ACTIVE` reports the
active bits, the context and the style (§13.2); buttons draw the "active" look from the same bits.

### 8.2 Inline toggles: bold, italic, strikethrough, inline code (F11)
**With a selection:**
1. Split it into pieces: per block and cell; inside a block at every edge of another enterable span and of a link's
   text. A piece that touches an atomic span (code span, formula, autolink, picture, footnote reference) grows to that
   whole span: emphasis delimiters are never placed inside code, math, autolinks or link destinations.
2. If every non-blank selected character already has the format → **remove**, else **add**.
3. Add: `EmitSpan` per piece (§7.5; adjacent pieces merge). Remove: the covering span is split around the piece
   (close before, reopen after; §7.5 rules): `**ab⟦cd⟧ef**` → `**ab**cd**ef**`; at a span edge only that delimiter
   moves: `**⟦ab⟧cd**` → `ab**cd**`.
4. Inline code, add: each piece's source is replaced by its **rendered** text (delimiters stripped, escapes and
   entities decoded, soft breaks as blanks; inline object atoms split the piece and keep their source) in a backtick
   run (§7.5): `a **b** \* &amp; c` → `` `a b * & c` ``. Remove: the unwrapped text is written with Markdown-significant
   characters escaped (`\*`, `\_`, `` \` ``, `\[`, `\]`, `\<`, `\&`, `\$`, `\|` in cells, and §7.4 at line starts).

Examples: `**bold** text` select `ld te`, Ctrl+I → `**bo*ld*** *te*xt` (pieces split at the bold edge);
`` see `code` more `` select `de mo`, Ctrl+B → ``see **`code` mo**re``.

**Without a selection:**
- caret not in such a span → `pendOn |= fmt`; the next non-blank character materialises it (`**x‸**`), blanks typed
  before it stay plain;
- caret inside such a span, before its end → `pendOff |= fmt`; the next non-blank character splits the span around the
  typed text: `**ab‸cd**` Ctrl+B, `x` → `**ab**x‸**cd**`;
- caret at the end of such a span (inside its closer) → the caret moves past the closer: typing continues plain;
- a second toggle while pending cancels the pending bit; any caret move clears pending bits.

### 8.3 Link (Ctrl+K, F23)
The link popover (§2.4) has a URL field; Enter or [Готово] commits (`OpLink`), [Убрать ссылку] runs `OpLinkRemove`.
- **Selection in one block, no link in it** → `[` + text + `](` + dest + `)`: the text is the selection's source with
  `[`/`]` escaped; the field is pre-filled from the clipboard when it holds an http(s)/mailto URL.
- **Selection containing (part of) links** → the inner links are stripped (their text kept) and one link wraps the
  whole selection.
- **No selection, caret not in a link** → `[url](url)` at the caret.
- **Caret in an inline link** → the field shows its destination; committing rewrites only the destination.
- **Caret in a reference link** (`[t][ref]`, `[t][]`, `[t]`) → the field shows the definition's URL and a notice «Адрес
  задан определением [ref] — изменение затронет все ссылки на него» / "Defined by [ref] — changing it changes every
  link that uses it"; the first Enter confirms the notice, the second commits the change to the definition line.
- **Destination syntax**: `<dest>` when the URL contains blanks, `<`/`>` (percent-encoded inside), or unbalanced
  parentheses; otherwise bare.
- **Remove** (`CMD_LINK_REMOVE`): an inline or reference link loses its delimiters (`[`, `](…)` / `][ref]`), the text
  stays and the definition stays; an autolink is neutralised by escaping so it cannot re-trigger: `<http://x>` →
  `http\://x`, `www.x.com` → `www\.x.com`, `a@b.c` → `a\@b.c`. Editing an autolink's URL turns it into `[text](url)`.
- Typing at either edge of a link stays outside it (§6.3); the bubble (§2.11) offers edit/remove/open.

### 8.4 Block style: paragraph, H1–H6 (F15, F17, F24)
Per touched text block; Ctrl+n on a heading of level n returns it to a paragraph.
- **Paragraph → heading n**: the block's lines become one line: first-line prefix (container prefix and a list marker
  stay) + `#`×n + blank + the content with soft breaks as one blank and hard breaks as `<br>`.
  `line one⏎line two` + Ctrl+2 → `## line one line two`.
- **ATX m → ATX n**: only the `#` run changes; the closing sequence stays.
- **Setext → ATX n**: content lines joined as above, the underline removed.
- **Heading → paragraph**: the ATX prefix and closing sequence (or the setext underline) are removed; §7.4 escapes
  (`# 1. Intro` → `1\. Intro`).
- **One-line alert** (`> [!NOTE] Remember this`): the tag is first split onto its own line —
  `> [!NOTE]⏎> ## Remember this` (F24).
- In a phantom: sets its style. In a list item: `- ## x` is valid and allowed.

### 8.5 Lists: bulleted, numbered, task (F15, F16)
- **On** (text blocks not in a list of that kind): the first line of each block gets the marker after its container
  prefix — `- `, `1. `, `2. `, … in order, or `- [ ] `; continuation lines get the content indent (2, 3 or 2 columns);
  blank lines between the converted blocks are removed (a tight list); a block directly after an existing list of the
  same kind joins it. `Buy milk⏎and eggs` → `- Buy milk⏎  and eggs` (one item); `a⏎⏎b` → `- a⏎- b`.
- **Off** (every touched block is an item of that kind): markers removed; continuation lines and children lose the
  content indent; the resulting paragraphs are separated by blank lines; §7.4. `- a⏎- b` → `a⏎⏎b`.
- **Change kind**: with a selection only the selected items change (which may split the list); with a caret the whole
  list level at the caret changes. Bullet → numbered numbers from 1; continuation lines and children are re-indented
  by the change of marker width (columns). Task ↔ plain inserts / removes `[ ] ` after the marker (a `[x]` is kept when
  switching between bulleted and numbered tasks).
- In a phantom: sets its style.

### 8.6 Quote (F15)
- **On**: every line of the touched blocks **and the blank lines between them** get `> ` (blank lines `>`) after their
  container prefix: `a⏎⏎b` → `> a⏎>⏎> b` (one quote).
- **Off** (all touched blocks are in a quote): one `>` level (and one following blank) is stripped from every line of
  the blocks and the blank lines between them; if they sit in the middle of the quote, the quote splits (a blank line
  is kept on each side). An alert loses its tag line together with the quote level.
- In a phantom: sets its style.

### 8.7 Code block
- **Text blocks** (the selection's, or the caret's non-empty block) → replaced by one fence: container prefix +
  a backtick fence longer than any backtick run inside (≥ 3) + `E`; each source line of the blocks as rendered text
  (inline atoms keep their source); a blank line between blocks; the closing fence.
- **Empty phantom** → an empty fence pair with the caret on its empty content line.
- **Caret in a code block** → unwrapped: fences removed, each non-empty code line becomes a paragraph (blank lines
  between), escaped as in §8.2.
- **Language** (`CMD_CODE_LANG` popover): rewrites the info string of the opening fence; an indented block becomes a
  fenced one with that info string.

### 8.8 Inserts
**After the current block**: the splice goes at the block's `outerEnd`: `E + BlankPrefix + E` + the new block's lines
(each with `ContPrefix`), then `E + BlankPrefix` when the next source line is not blank — blank lines on both sides.
For a table cell the current block is the table; for a list item's block the new block stays inside the item. In a
phantom the phantom **becomes** the new block.

| Command | Written | Caret afterwards |
|---|---|---|
| `CMD_INS_TABLE` (arg `rows << 4 \| cols`, 0 = 3 × 3, rows include the header) | `\|  \|  \|  \|`, `\| --- \| --- \| --- \|`, then `rows − 1` × `\|  \|  \|  \|` | first header cell |
| `CMD_INS_FORMULA` | selection in one block → `$` + its rendered text + `$`; else `$x$` at the caret (separating blanks per §7.5) | the formula popup opens with `x` selected |
| `CMD_INS_FORMULA_BLOCK` | after the block: `$$`, `x`, `$$` on three lines | popup, `x` selected |
| `CMD_INS_DIAGRAM` (arg 0–8) | after the block: ```` ```mermaid ````, the template lines, ```` ``` ```` | the diagram popup opens |
| `CMD_INS_IMAGE` | file dialog (`GetOpenFileNameW`, or `FASTMD_OPEN_FILE`); `![stem](dest)` inline at the caret, or its own paragraph in a phantom / after a non-text block | after the picture |
| `CMD_INS_HR` | after the block: `---` | a `PH_AFTER` phantom after the HR (UX-3) |

**Picture destinations** (UX-14): relative to the document folder with `/` when on the same volume; another volume →
absolute with `/`; the `<…>` form whenever the path contains blanks, `(`, `)`, `<` or `>`; `%` written as `%25`.
`C:\docs\a.md` + `C:\docs\img\Снимок экрана.png` → `![Снимок экрана](<img/Снимок экрана.png>)`;
`D:\pics\x.png` → `![x](<D:/pics/x.png>)` (`AddImage` accepts drive paths, parse.cpp:445). Drag-and-drop and
`CF_HDROP` paste use the same rules (§2.8, §7.11).

**Diagram templates** (language-neutral, one line each here; written with 4-space indentation):

| # | Template |
|---|---|
| 0 | `flowchart TD` · `A[Start] --> B{Choice}` · `B -->\|Yes\| C[Do it]` · `B -->\|No\| D[End]` |
| 1 | `sequenceDiagram` · `Alice->>Bob: Hello` · `Bob-->>Alice: Hi` |
| 2 | `classDiagram` · `class Animal {` · `+name` · `+speak()` · `}` · `Animal <\|-- Cat` |
| 3 | `stateDiagram-v2` · `[*] --> Idle` · `Idle --> Running : start` · `Running --> Idle : stop` · `Running --> [*]` |
| 4 | `erDiagram` · `CUSTOMER \|\|--o{ ORDER : places` · `ORDER \|\|--\|{ ITEM : contains` |
| 5 | `gantt` · `title Plan` · `dateFormat YYYY-MM-DD` · `section Work` · `Task A :a1, 2026-01-01, 7d` · `Task B :after a1, 5d` |
| 6 | `pie title Share` · `"A" : 60` · `"B" : 40` |
| 7 | `mindmap` · `root((Idea))` · `Branch A` · `Branch B` (nesting by 2 extra blanks) |
| 8 | `timeline` · `title History` · `2025 : Start` · `2026 : Growth` |

`test_edit_popups` renders all nine and requires none to fail.

### 8.9 Table operations (`CMD_TABLE_*`, via `tableSrc`)

| Op | Splice |
|---|---|
| row above / below | a new row line (container prefix + `\|` + `  \|` per column + `E`) before / after the caret's row; on the header row "above" is disabled and "below" goes after the delimiter row |
| column left / right of column c | in every row line, `  \|` (delimiter row ` --- \|`) right after the pipe that precedes cell c (left) / follows cell c (right); a row without a leading pipe gets `  \|` at its content start for c = 0; a row without a trailing pipe after its last cell gets ` \|  \|` appended; rows too short to have cell c are left alone |
| delete row | the row line with its line end; the header row cannot be deleted (a header-only table is valid) |
| delete column c | cell c's area and one adjacent pipe (the following one; the preceding one for the last cell) in every row; a one-column table is deleted instead |
| align left / centre / right | delimiter cell c becomes `:---`, `:---:`, `---:` (at least three dashes, width kept when possible) |
| delete table | all its lines and one separating blank line; the caret goes to the previous block's end (or a phantom) |

The caret stays in the same cell, or the nearest one.

### 8.10 Task boxes
In edit mode a click on a task box runs `OpTaskToggle`: a splice of the mark (`[ ]` ↔ `[x]` at `taskOff`), kind TASK,
undoable; nothing is written by `tasks.cpp`. In reading mode `ToggleTask` is rebased on the same path (splice +
`SaveSource` + `EditReparse`, §10.2) and pushes a TASK step when a history exists (D4, T2). Reading mode cannot be
dirty (leaving edit mode needs a save or a discard); should it ever be, `ToggleTask` refuses with a toast instead of
reloading (D6).

---

## 9. Source popups and the preview worker (Phase 3b, `editpop.cpp`)

### 9.1 Kinds and hosting

| Kind | Opened on | Field(s) | Bound source range | Title RU / EN |
|---|---|---|---|---|
| FORMULA | inline formula | TeX | between `$` and `$` | Формула / Formula |
| FORMULA_BLOCK | display formula block | TeX | between `$$` and `$$` | Формула (отдельной строкой) / Formula (own line) |
| DIAGRAM | Mermaid block | Mermaid | the content lines | Диаграмма Mermaid / Mermaid diagram |
| HTML | HTML block, inline `<img>` | HTML | the block's lines / the tag | HTML |
| FRONT | front matter | YAML | the body lines | Свойства (YAML) / Properties (YAML) |
| IMAGE | picture (inline or block) | alt; path + [Выбрать файл…] | alt range; destination (a reference picture shows the definition's URL read-only with the note «Адрес задан определением [ref]») | Изображение / Image |
| LINK | link popover | URL | §8.3 | — |
| CODELANG | code-language popover | info string | the opening fence's info string | — |

- The find input thread (find.cpp:178-213) creates the popup windows on demand: a multi-line EDIT
  (`ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL`, monospace 13 u) for sources, single-line EDITs for
  the image path, link URL and code language. In the dark theme the EDIT gets `SetWindowTheme(L"DarkMode_Explorer")`
  (uxtheme, delay-loaded) and `WM_CTLCOLOREDIT` colours `P_BG`/`P_TEXT` (UX-13). Tab inserts 2 blanks (TeX, YAML) or 4
  (Mermaid, HTML); in the image popup Tab moves between the fields.
- UI thread → input thread, posted only: `IM_POPUP_OPEN {kind, rect, font, colours, text, selection, fields}`,
  `IM_POPUP_MOVE {rect, show}`, `IM_POPUP_CLOSE`. Input thread → UI thread: on every `EN_CHANGE` it writes
  `{seq, field, text}` into the **popup buffer** (an SRW-locked struct) and posts `WM_APP_EDITINPUT(EI_TEXT, seq)`;
  keys go as `EI_KEY` (Esc, Ctrl+Enter, Enter in single-line fields), focus as `EI_FOCUS`.
- **Popup history** (UX-13): the EDIT subclass keeps snapshots `{text, selection}` — a new one when more than 400 ms
  passed or a word ended, else the top one is replaced; Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z move through it (`EM_UNDO` is
  never used).
- **Frame** (canvas): `P_OVERLAY_BG`, 1 px `P_OVERLAY_BORDER`, r8; title (semibold), the EDIT area, the hint line,
  the error line (`P_ALERT_CAUTION`), image-popup buttons. Placement under the atom (above when there is no room),
  width `clamp(textW, 420 u, availW)`, height `min(lines, 16) × line height` + chrome; it scrolls with the document.
  While the frame moves (scroll, slide) the EDIT is hidden (`IM_POPUP_MOVE show = 0`) and the canvas paints a snapshot
  of its text; `TIMER_EDIT_UI` shows it again 120 ms after the last move (R18).
- `EnsureInput()` runs when a popup opens, before any splice — never between a splice and the end of `EditReparse`
  (R13).

### 9.2 Session semantics (UX-13, R11, D20)
- **Open** (click, Enter/F2 on a selected atom, double-click in reading mode, an insert command): `EditSync()`; bind
  `{kind, atom, range, openText, prefix}` where `openText` is the bound source with container prefixes stripped per
  line and `prefix` is the `ContPrefix` for multi-line content.
- **Changes**: 120 ms after the last `EI_TEXT` (`TIMER_EDIT_POPUP`) the glue runs `OpAtomSource`: line ends normalised
  to `E`, lines 2…n prefixed; inline formulas turn line ends into blanks; a Mermaid text with a line that would close
  the fence gets a longer fence (both fences rewritten); a front-matter text with a `---` or `...` line is not applied
  (error line «Строка --- закончит блок свойств» / "A --- line would end the properties block"). The splice replaces
  the bound range, and the range end moves with it. One POPUP undo step per session.
- **Esc** / `CMD_POPUP_CANCEL`: the bound range is restored to `openText`'s source and the session's step is removed
  (the change nets to nothing); the popup closes.
- **Ctrl+Enter** / `CMD_POPUP_DONE` / a click outside: the latest text is applied at once; the popup closes; the caret
  stands after the atom (deselected). An inline formula left empty is removed with its `$…$` and one surplus blank.
- **Anchor off-screen** after a scroll: close, keeping the text, and give focus back to the document.
- **Commit points** (R11, D20): `EditPopupCommit()` drains queued `WM_APP_EDITINPUT` messages
  (`PeekMessage(PM_REMOVE)`) and handles them in order (a queued Esc still cancels), then reads the popup buffer under
  its lock and applies it synchronously. It runs first in every save, `EditExit`, Ctrl+E, `CanLeaveDocument`,
  `PrepareToClose`, `WM_QUERYENDSESSION`, `WM_ENDSESSION`, and before every command that is not a popup key.
- Tests: `Q_EDIT_POPUP` (hwnd of field `lp`), `Q_EDIT_POPUP_STATE` (0 none, 1 ok, 2 error, 3 render pending); the
  helper `popup_set(text)` uses `EM_SETSEL(0, −1)` + `EM_REPLACESEL`, which raises `EN_CHANGE` (T23).

### 9.3 When the input thread is unavailable
If `EnsureInput()` fails (`g_inputFailed`), opening an atom turns its block (for an inline atom, its paragraph) into
a raw leaf (§6.9) edited in place, without IME, until the caret leaves it; the link and code-language popovers do the
same for their paragraph / fence. A toast says so once: «Окно ввода недоступно — источник правится прямо в тексте» /
"The input box is unavailable — the source is edited in place" (R11).

### 9.4 Preview worker
- One long-lived detached thread (started on first use), a "latest job" slot, an event and sequence numbers. Job:
  `{seq, key, ctx, kind, source (UTF-8), fontPx, rgb, dark, scale}` → `TexSvg` / `MermaidSvg` → `SvgMeasure`
  (diagrams) → `SvgRender` at **device pixels** (longer side ≤ 4096) → `WM_APP_PREVIEW` with a heap
  `PreviewResult{seq, key, ctx, ok, error, pix, w, h, ascent}` (deleted by the worker if the post fails).
- On the UI thread a result older than the binding's latest sequence is dropped. Success: the table entry for
  key + ctx becomes `RS_OK` (new `pxSerial`), every image with that key gets the pixels (`pxFor = key`,
  `renderFailed = false`, `sc = nullptr`), only its block is re-laid out, `pixelSerial++`. Failure: the entry becomes
  `RS_FAILED` with the error; the shown image keeps its last good pixels with `renderFailed = true` (a 1 px
  `P_ALERT_CAUTION` outline), the popup shows the error line, `Q_MATH lp 3` counts such entries.
- Warm-up once per process, lazily, on the worker (R22): TeX (`TexSvg("x")`) when edit mode is entered in a document
  with formulas or when the formula popover or popup opens; a non-flowchart diagram (`sequenceDiagram⏎A->>B: x`) plus
  one `SvgMeasure` when the diagram popover or popup opens. Documents that never touch them load no DLL.
- Error texts: with the optional exports `fastmd_tex_svg2` / `fastmd_mermaid_svg2` (an error out-buffer; looked up
  with `GetProcAddress`, older DLLs keep working) the popup shows the message; otherwise «Ошибка в формуле» /
  "Formula error", «Ошибка в диаграмме» / "Diagram error". The exports are optional in Phase 3b.
- Reading mode: a failed display formula or diagram draws its source in `P_MUTED` in its box instead of a blank
  placeholder (view.cpp:667-669).

---

## 10. Saving, conflicts, recovery (Phases 1c and 2a, `editfile.cpp` + `edit.cpp`)

### 10.1 Encoding model and the entry checks (D1, D13, D14, D15)
- **Reading** (`ReadDisk`): `CreateFileW(GENERIC_READ, SHARE_READ|WRITE|DELETE, OPEN_EXISTING)`,
  `GetFileInformationByHandle` (volume serial, file index, size, mtime, attributes), reads in 1 MB chunks. `got !=
  size`, or a size that changed during the read, is **UNKNOWN** — never "empty", "missing" or "changed" (D5).
- **Decoding**: BOM EF BB BF → UTF-8; FF FE → UTF-16 LE; **FE FF → refused**; otherwise strict UTF-8, else the ANSI
  code page resolved to a number: `FASTMD_ACP` (tests only) or `GetACP()` — never the constant `CP_ACP`. Pure ASCII
  therefore decodes (and saves) as UTF-8 without BOM (README). When the baseline is re-derived from new bytes that are
  pure ASCII (§10.3 step 2, §10.7), the previous code page is kept (policy, D13).
- **Encoding**: `WideCharToMultiByte` with flags 0 and `lpUsedDefaultChar = nullptr` for 65001, 54936, 42 and
  50220–50229/57002–57011/65000; `WC_NO_BEST_FIT_CHARS` + `usedDefault` for the others. A character is encodable iff
  `decode(encode(c)) == c` (checked at splice time for ANSI files, D7).
- **Entry checks** (in this order; any failure refuses edit mode with the toast of §2.1): readable; no NUL byte unless
  the file is BOM-detected UTF-16; UTF-16 of even length; not a stateful code page (50220–50229, 57002–57011, 65000:
  their bytes cannot be rewritten locally); the decoded text equals `g.src` (else `ReloadDocument()` and all checks
  once more); **byte-exact round trip**: `header + Encode(text) == bytes`. A lossy decode (mojibake, a stray byte)
  therefore refuses edit mode and is never "converted" (D1).
- **Baseline** `g.disk = {text, cp, header bytes, byte length, FNV-1a-64 of the bytes, volume serial, file index,
  mtime, size, attributes, remote, cloud}` and the line-end statistics (§7.2).

### 10.2 The baseline rule (D23)
`g.disk` is set only from bytes just read and verified, or just written: at edit entry, after every successful save,
after every adoption (§10.7), and by `ToggleTask`. It is never set from a load-failed buffer. `ToggleTask` (reading
mode) is rebased on the same path: splice → `SaveSource` → `EditReparse`, with its TR_* toasts; a real external change
still reloads (T2). `dirty := g.src != g.disk.text` (compared, not flagged).

### 10.3 `SaveSource(snapshot)` — the write (D2, D7, D12, D21, R24)
Pure of UI: no dialog, no pumping, the handle is closed before any UI (D7). Callers run `EditPopupCommit()` first and
do nothing when not dirty (equal bytes are never written, UX-5).
1. `CreateFileW(GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING)`; error class per §10.4.
2. Read all bytes (1 MB chunks); a short read → UNKNOWN. Same length and hash as the baseline → unchanged. Otherwise
   decode → `now`; `now != g.disk.text` → **CONFLICT**, nothing written. Same text but different bytes (re-encoded
   outside) → take code page, header and EOL from these bytes, require byte exactness (else FAILED "encoding
   changed"), adopt them as the baseline and continue (D13).
3. **Splice-local encoding** (D1): `p`, `s` = common prefix / suffix of `g.disk.text` and the snapshot (moved so that
   neither splits a surrogate pair or a CRLF); `pb`, `sb` = their byte lengths (UTF-16: ×2; else `WideCharToMultiByte`
   of the prefix / suffix); `middle = Encode(text[p, len − s))`. An unencodable character → **UNENCODABLE** with the
   first offending character, nothing written. `out = bytes[0, pb) + middle + bytes[len − sb, len)`. **Proof**:
   `decode(header + out) == text` in full for sources under 1 M characters and always under `FASTMD_EDIT_SELFCHECK`;
   above that, the changed window ± 8 bytes is decoded and compared, and an ANSI file whose edit removes, adds or
   borders a byte ≥ 0x80 is checked whole for turning into valid UTF-8 (refused: the next read would take it for
   UTF-8). Reason codes on failure: UNENCODABLE, LONE_SURROGATE,
   BOM_LOOKALIKE (text starting with U+FEFF in a BOM-less UTF-8 file, or ANSI bytes that would start with EF BB BF / FF
   FE — or FF FE right after the UTF-8 mark an ANSI file can have — ENCODING strip with its own message),
   ENCODER_ERROR (an API failure: status FAILED, no dialog) (D12).
4. **Recovery file** (§10.5): header + the old bytes `[pb, pe)` — `pe = oldLen`, or the end of the replaced bytes when
   the length stays (a tick keeps one byte) — written under a new name, `FlushFileBuffers`, closed. Failure → FAILED,
   the target untouched. While saves go unflushed (step 7), the recovery file of the last flushed version stays and
   stands for it: a save inside its range writes none, one outside it writes a wider one pieced together from it and
   the unchanged bytes around (the old one is deleted once the new one is safe); the file's result block records what
   the target holds after each save. It is deleted at the next flushed save or flush point.
5. **Growing file**: `SetFilePointerEx(newLen)` + `SetEndOfFile` before any content byte changes; failure → FAILED
   "disk full", content untouched, recovery file deleted.
6. Write `middle + suffix` from `pb` in 1 MB chunks; `SetFilePointerEx(newLen)` + `SetEndOfFile`.
7. `FlushFileBuffers(target)`: always on remote volumes (`GetFileInformationByHandleEx(FileRemoteProtocolInfo)`
   succeeds or `GetDriveTypeW == DRIVE_REMOTE`) and cloud-backed files (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, pinned,
   unpinned); on local volumes at leave, close, session end and Ctrl+S, and on every save when the volume's measured
   flush cost (median of its first three flushes this session) is below 5 ms — the number goes into the phase report.
8. `GetFileInformationByHandle` (the new stamp) **before** `CloseHandle`; a failed flush or close → FAILED, still dirty.
9. A failure in steps 5–8 after bytes changed: the old bytes are written back at `pb` and the old length restored; if
   that fails too the recovery file is kept, its path shown in the status tooltip, and the RECOVERY strip shown at
   once; no save writes the file while a leftover of it waits on the strip.
10. Success: recovery file deleted if the save was flushed (else kept, step 4); `g.disk` = the snapshot text with
    `out`'s hash and length and the new stamp;
    `g.fileTime/g.fileSize` from the handle (the watcher stays quiet); `g.saves++`; the journal deleted; `dirty`
    recomputed against the current `g.src`.

**Threads and cadence.** Sources under 1 M characters save synchronously on the UI thread. Larger ones autosave on
the save worker from a snapshot (a copy of `g.src` and `g.disk`); one save at a time; `WM_APP_SAVED` delivers the
result; flush points wait for an in-flight save (no pumping) and then save synchronously. `TIMER_EDIT_SAVE` fires
800 ms after the last edit (< 1 M characters), 2 s (< 8 M), 5 s above; remote or cloud files keep at least 5 s between
saves; `FASTMD_AUTOSAVE_MS` overrides; autosave off arms no timer.

### 10.4 Error classes, retry and read-only (D8, D17)

| Class (state) | Source | Behaviour |
|---|---|---|
| TRANSIENT (BUSY) | sharing/lock violation, `ERROR_CLOUD_FILE_*`, `ERROR_NETNAME_DELETED`, `ERROR_BAD_NETPATH`, `ERROR_UNEXP_NET_ERR`, `ERROR_SEM_TIMEOUT`, `ERROR_PATH_NOT_FOUND` on a mapped or network drive | `TIMER_EDIT_RETRY` at 0.5, 1, 2, 4, 8 s, then every 15 s while dirty; one toast per class per session «Файл занят — сохраню, как только он освободится» / "The file is busy — it will be saved as soon as it is free" |
| UNKNOWN | short read, size changed during the read | same backoff; never adopted, never overwritten |
| DENIED / READONLY | `ERROR_ACCESS_DENIED`, `ERROR_WRITE_PROTECT`, a failed write probe | READONLY strip; autosave stops (no Controlled Folder Access notification per save); re-probed on watcher events and `WM_ACTIVATE`; writable again → PENDING and a save |
| MISSING | file not found while its folder exists | MISSING strip; re-checked on every watcher event, retry tick and `WM_ACTIVATE`; a reappearing file goes through §10.7 |
| CONFLICT | disk text ≠ baseline | CONFLICT strip, autosave paused, journal armed |
| UNENCODABLE / BOM_LOOKALIKE | encoder | ENCODING strip, autosave paused |
| FAILED | anything else | status + tooltip; backoff as TRANSIENT, at most every 15 s |

**Write probe** at entry (D17): `CreateFileW(GENERIC_WRITE, FILE_SHARE_READ|WRITE|DELETE, OPEN_EXISTING)` and an
immediate close. Denied → edit mode with READONLY (this triggers Controlled Folder Access once, before any typing).

**Encoding strip actions**: [Сохранять в UTF-8] converts once — the header becomes EF BB BF and the whole text is
written as UTF-8 (a full encode, not splice-local), `g.disk.cp = CP_UTF8`; [Убрать символ] deletes the offending
characters as one undo step. Autosave stays paused until one is chosen; the modal UTF-8 question of v1 is gone
(UX-16, D7).

### 10.5 Recovery file and the restore banner (D2, D3, D22)
- Name `DataDir()\recovery\<volSerial:8 hex>-<fileIndex:16 hex>-<pid>-<n>.rec`, created with `CREATE_NEW` (never
  over another one: a kept file may be the only copy of its bytes); header `{magic "FMDREC2", original path, pre-save
  size, mtime, code page, header bytes, pb, pe, a hash of the old bytes [0, pb), of [pe, size) and of the saved ones
  (8 bytes a step: a tick at the top of a big file hashes the rest of it), FNV-1a-64 of all the old bytes (the
  baseline's), writer pid and process start time, a hash of all that}`, a result block `{length, FNV-1a-64, their hash}` of
  what the target holds after the save that wrote (or last used) it, then the old bytes `[pb, pe)`. It exists only
  **around a save** (written and flushed before the target changes, deleted right after a successful flushed save —
  or, between flushes, at the next flushed save or flush point, §10.3 step 4), so a leftover means "a save was
  interrupted" — or a crash between flushes.
- A source with `FILE_ATTRIBUTE_ENCRYPTED` gets an encrypted recovery file; files older than 14 days are purged at edit
  entry (D22). BitLocker To Go / VeraCrypt cannot be detected (README).
- After the first frame of an open (never on the startup path), `recovery\<vol>-<index>-*` is listed for the opened
  file's identity, keeping files whose header names this path (case-insensitive: FAT gives a new file a deleted one's
  index) and whose header and saved bytes check out, and skipping those of another FastMD process that still runs (pid
  and start time; this process's own are listed). Each is classified against the file's bytes and stamp: it holds
  the result block's bytes (the save went through) or the old bytes (it never wrote) → deleted without a word; its
  bytes around `[pb, pe)` are the old version's and the file was not written later than the recovery file (+2 s for
  FAT) → **torn**; anything else → **changed**. A leftover shows the RECOVERY strip «Прошлое сохранение прервалось»
  (changed: «…, а файл с тех пор изменён», without [Восстановить]): [Открыть копию] rebuilds the previous file
  (current bytes `[0, pb)` + the saved ones, + the current ones after `pe` for a save in place) as
  `%TEMP%\FastMD\<name> (восстановлено).md` and opens it in a new window; [Восстановить] (torn only, checked again
  under an exclusive handle) first writes the bytes it replaces to a recovery file of its own, then writes the saved
  bytes back at `pb`, restores the old length, flushes, deletes its own and reloads; [Удалить] deletes the recovery
  file.
- «Перезаписать файл моими правками» keeps the overwritten external version as `<id>.theirs` until the document is
  closed.
- Edit mode is refused when `DataDir()` is empty (§2.1).

### 10.6 Journal of unsaved edits (D10)
- While dirty **and** the real save is off or failing (autosave off, CONFLICT, ENCODING, READONLY, MISSING, BUSY,
  UNKNOWN, FAILED), `TIMER_EDIT_JOURNAL` writes `recovery\<id>.unsaved` 3 s after the last edit: `{magic "FMDJRN1",
  path, time, FNV-64 of g.disk.text, code page}` + `g.src` as UTF-16, via a temp name, flushed, `MoveFileExW(
  REPLACE_EXISTING)`; local only; encrypted like recovery files.
- Deleted on a successful save and on a discard.
- On the next open of that file identity: RECOVERY strip «Есть несохранённые правки от <время>»: [Открыть копию]
  (the journal text as a temp .md in a new window), [Восстановить] (only when the journal's disk hash equals the
  current file text: enters edit mode and replaces `g.src` with the journal text as one ADOPT step), [Удалить].
- The crash handler stays allocation-free (crash.cpp:24) and writes no journal; the 3 s journal covers crashes.

### 10.7 External changes (D4, D5, D13, D16, UX-17)
The watcher posts `WM_APP_FILECHANGED` (wParam 0 = changed, 1 = unavailable) when the stamp changes **or becomes
unavailable**; when its handle fails it restarts itself with backoff (1, 2, 4 … 30 s); its baseline is the load stamp.
`OnFileChanged` runs after the 120 ms debounce (re-armed while `g.editModal > 0`).

**Reading mode:** stamp unchanged → nothing. Else read: a failed or short read → retry with backoff 250 ms → 4 s (no
reload into the error document); text equal to the baseline → adopt the stamp only (no reload, the history survives,
D6); otherwise `ReloadDocument()` (`Q_RELOADS++`, history cleared). The error document (a load that failed) keeps the
file's stamp as the watcher's baseline, and is reloaded only once the file can be read: a locked file is read again
with the same backoff (from the open on), a denied one waits for F5.

The watcher has a stop event of its own, so one that outlives `StopWatcher` (stuck on a share that went away) ends
when its call returns instead of running on for the next document.

**Edit mode:**
1. stamp unchanged → nothing;
2. unavailable: file gone and folder present → MISSING (editing continues); folder or share unavailable → UNKNOWN;
3. read failed / short / size changed → UNKNOWN: re-armed with backoff 250 ms → 4 s, status «файл недоступен»; never
   adopted, never offered for overwrite (D5);
4. text equal to the baseline → adopt the stamp, and — if the bytes differ — the new code page, header and EOL after
   the byte-exact check (D13);
5. not dirty → **adopt** as one ADOPT undo step (the minimal differing middle as its splice, so Ctrl+Z visibly undoes
   the external change), new baseline, `g.eol` recomputed, `EditReparse` (caret by source offset, clamped), toast
   «Файл изменён другой программой — загружена новая версия» / "The file was changed by another program — the new
   version is loaded" (UX-17, D4);
6. dirty → **CONFLICT** strip with both sizes; autosave paused; journal armed. [Загрузить версию с диска] replaces
   `g.src` with the disk text as one ADOPT step (Ctrl+Z brings the edits back); [Перезаписать файл моими правками]
   keeps the disk bytes as `.theirs`, takes them as the baseline (explicit consent) and saves at once;
7. a file that reappears after MISSING goes through 4–6.

**Save As** (`CMD_SAVE_AS`, D16): `GetSaveFileNameW` (`OFN_OVERWRITEPROMPT`; `FASTMD_SAVE_AS` in tests; modal scope).
When the folder differs and the document has relative links or pictures, MessageBox «Относительные ссылки и
картинки (N) перестанут работать в новой папке. Сохранить всё равно?» / "N relative links and pictures will stop
working in the new folder. Save anyway?" (Да/Нет; test answer kind `rellinks`). The full text is written in the
current encoding (UTF-8 without BOM after a conversion) to a temp file in the target folder, flushed, and moved over
the target (`MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`; a new file, so the in-place rule does not apply). Then
`g.path`, `baseDir` (followed by `EditReparse`: relative pictures resolve anew), the watcher, the mutex, recovery
names, the title, `SHAddToRecentDocs` and the reading-position entry move to the new path. Following a same-folder
rename by file ID is out of scope.

### 10.8 Leaving edit mode, leaving the document (D6, D9, D19, UX-15, T1, T20)
- **Leaving edit mode** never shows a modal: §2.2 (the leave strip).
- **`CanLeaveDocument()`** runs before `OpenDocument`, `ReloadDocument`, `NavigateBack/Forward` (before the history
  pops), close (`PrepareToClose`), `UpdateRestart` (`PrepareToClose`), a dropped non-picture file, Ctrl+O and a link
  to another .md: `EditPopupCommit`, `EditSync`, a synchronous flush (waiting for a save in flight). Success →
  `EditExit(silent)` and proceed. Failure → one MessageBox (modal scope, owner `g.hwnd`), worded to fit
  Да / Нет / Отмена (UX-24):

| Reason | Text RU (EN in `strings.cpp`) | Да | Нет | Отмена |
|---|---|---|---|---|
| MISSING, DENIED, READONLY | «Правки в «<имя>» не сохранены: <причина>.⏎⏎Сохранить их в другой файл?» | Save As, then proceed | discard, proceed | stay |
| BUSY, UNKNOWN, FAILED | «Правки в «<имя>» не сохранены: <причина>.⏎⏎Попробовать сохранить ещё раз?» | retry once; a failure asks the Save As question | discard, proceed | stay |
| CONFLICT | «Файл «<имя>» изменён другой программой, а ваши правки не сохранены.⏎⏎Перезаписать файл вашими правками?» | overwrite, proceed | asks «Сохранить ваши правки в другой файл?» (Да Save As / Нет discard / Отмена stay) | stay |
| UNENCODABLE | «Символ «<c>» нельзя сохранить в кодировке <имя>.⏎⏎Сохранить файл в UTF-8?» | convert, save, proceed | asks the Save As question | stay |

  Every prompt reads `FASTMD_TEST_ANSWER` first (kinds `leave`, `leave2`) and records `Q_LAST_PROMPT` (T1).
- **Ctrl+E** (D19): flush; failure → leave strip, nothing launched; success → `EditExit`, then `OpenInEditor`.
- **F5 / Ctrl+R / `CMD_RELOAD`**: flush; success → `EditExit` + `ReloadDocument` (the disk holds the edits);
  failure → leave strip, stay (R14).
- **Alt+←/→ and the X buttons** (T20) go through `CanLeaveDocument` and are never suspended; `WM_XBUTTONUP` gets the
  ready guard.
- **Theme switch** on a themed document while editing re-parses `g.src` (`EditReparse`; formula contexts change and
  re-render) instead of reloading from disk.

### 10.9 Close and session end
- `WM_CLOSE`: `if (!PrepareToClose()) return 0;` before `g.closing = true`. `PrepareToClose` = inside a modal →
  remember the close and return false (re-posted when the modal ends); else `EditPopupCommit` and, when editing,
  `CanLeaveDocument` (Отмена → false). `SaveAll` then saves the reading position after the edits (positions.bin gets
  the new stamp).
- `WM_QUERYENDSESSION` (new): when dirty — `EditPopupCommit`, the journal written synchronously,
  `ShutdownBlockReasonCreate(«FastMD сохраняет правки» / "FastMD is saving edits")`, the real save on the save worker
  waited for at most 3 s, `ShutdownBlockReasonDestroy`; always returns TRUE, shows no UI; the same inside a modal
  (the system sends this message, it cannot be re-posted).
- `WM_ENDSESSION(TRUE)`: `EditPopupCommit`; if still dirty and no save in flight, one synchronous save without UI;
  then `SaveAll`.

### 10.10 Modal depth (R12)
- `struct ModalScope` increments `g.editModal` around every modal call: MessageBoxW, TrackPopupMenu, DoDragDrop,
  GetOpenFileNameW, GetSaveFileNameW, the print dialog and `StartDocW`.
- While it is above zero: `TIMER_RELOAD`, `TIMER_EDIT_SAVE`, `TIMER_EDIT_RETRY`, `TIMER_EDIT_REPARSE`,
  `TIMER_EDIT_POPUP` and `TIMER_EDIT_IDLE` re-arm for 250 ms instead of acting; `WM_APP_EDITINPUT`, `WM_APP_PREVIEW`,
  `WM_APP_IMAGES`, `WM_APP_SAVED` and `WM_APP_FILECHANGED` go to a deferred queue (payload kept); `WM_CLOSE` sets
  `closePending`. When the depth returns to zero, `WM_APP_REPLAY` is posted: the queue replays in order, then a pending
  close is re-posted. `EditReparse` therefore never runs inside a menu, a dialog or a print job.
- No UI is ever shown while the file handle is open: questions come before a save starts, and the save restarts from
  step 1 afterwards.
- Printing: `StartDocW` inside a modal scope; paper has no edit inset, no caret, no popups, no phantom row.
- Phase 1 has the scope and its first users: `ModalScope` wraps every modal call listed above; while it is open
  `TIMER_RELOAD` re-arms for 250 ms, `EditSplice` refuses (the test hooks arrive as sent messages inside such loops),
  window activation looks at no file, and `WM_APP_IMAGES` batches wait in the queue that `WM_APP_REPLAY` replays. The
  other timers, messages and `closePending` join with 2a.

### 10.11 Two windows on one file (D18)
At entry, `CreateMutexW(L"Local\\FastMD.edit.<volSerial>-<fileIndex>")`; `ERROR_ALREADY_EXISTS` refuses entry with the
OTHER WINDOW strip. [Перейти к нему] calls `AllowSetForegroundWindow(ASFW_ANY)`, enumerates top-level windows of
class `FastMD.Document` and sends each (`SendMessageTimeoutW`, 200 ms) the registered message `FastMD.EditOwner` with
the file identity; the owner answers 1 and brings itself to the foreground. The mutex is released by `EditExit`.
Recovery and journal names carry the pid.

### 10.12 Privacy (D22)
While editing or dirty, `crash.cpp` writes dumps without `MiniDumpWithIndirectlyReferencedMemory |
MiniDumpScanMemory` (a global atomic flag); recovery and journal files are encrypted for encrypted sources and purged
after 14 days; the README says what is stored where.

---

## 11. Undo and redo

- **Steps**: `EditStep{splices, before, after, kind, t0, t1}` (§3.2). Undo applies the inverse splices in reverse
  order; redo re-applies them.
- **Coalescing** (T19): a new step merges into the top one only if all hold: the same kind, one of TYPE, DEL_BACK,
  DEL_FWD; the top step's `after.focus` equals the new step's `before.focus` and nothing broke coalescing in between
  (every caret move not caused by the typing itself, click, command, popup or focus change calls `BreakCoalescing`);
  less than 1.5 s since the top step's `t1`; and, for TYPE, the new character is not the first non-blank character
  after a blank (a step breaks **before** it). Saves never break a step. Typing «Новый мир» makes two steps, «Новый »
  and «мир»; one Undo leaves «Новый ».
- Everything else is one step: structural keys, commands, paste, cut, a popup session (created on its first change,
  removed when cancelled), task toggles, ADOPT, DISCARD.
- A new step clears the redo stack. Limits: 1000 steps or 32 MB of step text (oldest dropped).
- **Verification** (D4, UX-17): undo applies nothing unless every splice's inserted text is at its offset
  (`g.src.compare(at, inserted.size(), inserted) == 0`); redo checks `removed`. A mismatch clears both stacks and shows
  «История правок сброшена: текст изменился» / "Undo history reset: the text changed".
- Undo/redo runs one `EditReparse`; caret, selection and phantom come from `before` (undo) or `after` (redo); pending
  formats are cleared; the popup, if open, is closed first (kept).
- **Lifetime**: the document session — across saves and across leaving and re-entering edit mode; cleared by open,
  reload and navigation. Reading-mode task ticks push TASK steps when a history exists.
- `Q_UNDO_DEPTH` lp 0 = undo depth, lp 1 = redo depth. Unit tests inject time through `Push(step, nowMs)`.

---

## 12. View, chrome and input integration (Phases 2a–2c)

### 12.1 Top inset and scroll compensation (UX-4)
- `EditInset() = 44 u × barT` while editing and `!g.fitWide`, else 0 (printing and the preview pane: 0).
  `RecomputeY` starts at `kPadTop + EditInset()`.
- **The bar covers the page; `scrollY` absorbs the inset.** At the start of a slide in, `maxComp = min(44 u,
  scrollY)`; at the start of a slide out, `maxComp = g.barComp`. Each animation step: `newComp = min(EditInset(),
  maxComp)`, `scrollY += newComp − g.barComp`, `g.barComp = newComp`, clamped. A document scrolled at least 44 u does
  not move at all; one scrolled `y < 44 u` moves down by `44 u − y` (at the very top, by the whole bar height). Leaving
  reverses it.
- After the slide, `RevealCaret()` scrolls minimally if the caret ended under the bar.
- `EditRevealTop() = EditInset() + g.stripH + (findOpen ? 52 : 0) + 8`; bottom margin 48. `RevealCaret()` scrolls by
  the overflow only (never re-centres) and passes the caret's block and cell (R8); paging, `ScrollToBlock`,
  `TocCurrent` and the `KeySelect` start probe use `EditRevealTop()`.
- One `ScrollTrackTop() = 2 + EditInset() + g.stripH`, called by view.cpp:713, window.cpp:506-509 and find.cpp:563.
- The find bar's y is `12 + EditInset() + g.stripH`; `FindRelayoutInput()` runs at the start and end of the slide and
  whenever a strip appears or goes (the EDIT is hidden during the slide, R18).

### 12.2 The caret (R8, R17, UX-23, T17)
- Width `max(2, SPI_GETCARETWIDTH × dpi / 96)` device px, `P_TEXT`, the height of the line box; in trailing blanks
  moved right by `trailCols` space advances; in a phantom at `phantomX`.
- Drawn by `DrawDocumentBand` only when the caret's block is cached and `[Y, Y + H]` meets the band; the paint path
  never lays out; clipped to the block box ∩ `[DocLeft, ViewW − kPadX]` (R17); `TrimCache` keeps the caret's block.
- Blink: `TIMER_CARET` = `GetCaretBlinkTime()` (`INFINITE` = steady); the visible phase restarts on every key, click
  and move; hidden while the window is inactive (`WM_ACTIVATE`, `WM_SETFOCUS`/`WM_KILLFOCUS`, the find box taking
  focus). `FASTMD_CARET_STEADY=1` = always visible and never blinking, whatever the activation (T17).
  `Q_CARET` returns the position in the off phase too.
- A hidden system caret (`CreateCaret`/`SetCaretPos`) follows it for Magnifier and screen readers.
- A selected object atom: a 1 px `P_ACCENT` outline 2 px outside its box, no caret.

### 12.3 Frame key, full frames and the scroll fast path (R8, R18)
- `FrameKey` gains: editing, `barT` (quantised to 1/256), bar hot and pressed parts, collapse level, active and
  disabled masks, style id, save state, popover kind + hot row, strip kind + hot button, bubble state, popup rect +
  state + text-snapshot serial, bar tooltip part, pencil hot, the **caret** (focus and anchor source offsets, block,
  cell, `trailCols`, `lineAff`, phantom kind and block, visible — replacing `caretOn ? selFocus + 1 : 0`), the selected
  atom, window active, `editSerial`. Failure outlines bump `pixelSerial`.
- `overText` is also true while a popover, a popup frame, the link bubble or a bar tooltip is visible (full frames).
  The bar, the pencil and a strip are repaired by `ScrollFrame` like the gear: `EditChromeRects()` →
  `RedrawDocRect(l, min(t, t − d), r, max(b, b − d))`, then `DrawChrome` — edit-mode scrolling stays on the fast path.

### 12.4 Hit priority and cursor
Mouse down and hover, first hit wins: popup frame (its EDIT is a child window) → popover → strip buttons → link bubble
→ toolbar → find bar → outline (docked panel, drawer, toggle) → pencil and gear (reading) → scrollbars → document
(selected atom, object atoms, text, phantom row, the area below the last block). `WM_MOUSELEAVE` resets every hover
part. Cursor: §2.8.

### 12.5 Keyboard routing (T5, T15, UX-12)
- `OnKeyDown`: start screen → `HomeKey`; **editing → `EditKey(vk, ctrl, shift, alt)`** first (chords via `EditChord`,
  navigation, editing keys, the Esc chain); otherwise as today. Edit logic never lives in `KeyCommand`, which the find
  box shares.
- `EditChord` returns 0 whenever `ctrl && alt` (AltGr text must reach `WM_CHAR`, UX-12, T21).
- `WM_KEYDOWN` owns Back, Tab, Enter, Esc and Delete; `EditChar` drops `c < 0x20` and 0x7F, so a posted `VK_BACK`
  (keydown + `WM_CHAR 0x08`) acts once.
- `WM_CHAR` routing (T15): `if (g.path.empty()) HomeChar(c); else if (FindInputFocused()) FindTypeChar(c); else if
  (g.editing) EditChar(c); else if (g.findOpen) FindTypeChar(c);`.
- Ctrl+click reads `MK_CONTROL` from the mouse message's wParam (T5).
- With `FASTMD_TEST_HOOKS`, `WM_APP_TESTKEY(vk, KM_CTRL | KM_SHIFT | KM_ALT)` calls `OnKeyDown` with explicit modifiers.

### 12.6 Find
`FindRefresh()` = `FindUpdate` without the final `FindStep(0)` and without `userMoved`: the current match is the first
match at or after the previous one, shifted by the edit's text delta; the view does not scroll. Typing goes to the
document whenever the document has focus, even with the bar open (§12.5). The bar sits under the bar and strip.

### 12.7 Outline (R21)
While editing, `TocAvailable()` returns the value frozen at entry (the panel neither docks nor undocks); `TocSync`
rebuilds only when the (block, level, text) list changed, keeps item layouts and does not re-centre; the floating
button is replaced by bar button 0. `EditExit` unfreezes, calls `UpdateColumns()` and, if the columns changed,
`Relayout()` (which starts the full measure). A resize while editing measures only unknown blocks (§5.4).

### 12.8 UI Automation (R13, UX-23, T13)
- `UIA_IsReadOnlyAttributeId` = `!g.editing`; `UiaDocumentChanged()` on entering and leaving.
- Every `Range` records `editSerial` at creation; every method clamps its offsets to the text size; a stale range works
  on the clamped offsets and never throws (`FindText` at uia.cpp:137 included).
- In edit mode `Range::Select` goes through the editor (`EditSync`, `SrcOfText` for both ends, `EditState`,
  `RevealCaret`, `UiaSelectionChanged`); `ScrollIntoView` uses the minimal reveal.
- No lock: calls arrive on the UI thread, and no message is pumped between a splice and the end of `EditReparse`.
- **Toolbar fragments** (Phase 2c): every visible bar button, popover row and strip button is an
  `IRawElementProviderFragment` child of the document: ControlType Button, Name = its tooltip name, AcceleratorKey =
  its shortcut, IsEnabled per §8.1, BoundingRectangle from the geometry, Invoke → `Command(id, arg)`; `Navigate` from
  the root reaches them; `ElementProviderFromPoint` returns the button under the point; structure-changed events when
  the bar appears or goes.
- `UiaSelectionChanged()` on caret moves (debounced 100 ms); `OnFullDoc` also raises `UiaDocumentChanged()`.

### 12.9 High contrast (UX-23)
Bar colours per §2.3; the caret width per §12.2; strips and popovers use `P_OVERLAY_TEXT` text with a 1 px
`P_OVERLAY_TEXT` border; hover and active states never rely on fills alone.

---

## 13. Automation (ids frozen now, T6)

Ids are append-only and never renumbered; this list is complete for v1 and is added to `app.h` and mirrored in the
`CMD`/`Q` dicts of `ui_smoke.py` (which also gains the missing `"UPDATE": 140`) in Phase 1a, even where the behaviour
lands later (an id whose feature is not built yet does nothing / returns −1).

### 13.1 Commands (`enum Cmd`, after `CMD_UPDATE = 140`)

| Id | Name | Action (`arg` = `HIWORD(wParam)`, 0 = default) |
|---|---|---|
| 141 | `CMD_EDIT_TOGGLE` | reading: enter (F2 rule); editing: selected atom → open its popup, else leave |
| 142 | `CMD_EDIT_HERE` | enter at the context-menu point |
| 143 | `CMD_EDIT_EXIT` | leave (✕) |
| 144 | `CMD_UNDO` | undo |
| 145 | `CMD_REDO` | redo |
| 146 | `CMD_CUT` | cut |
| 147 | `CMD_PASTE` | paste |
| 148 | `CMD_SAVE` | save now |
| 149 | `CMD_SAVE_AS` | Save As |
| 150 | `CMD_FMT_BOLD` | toggle bold |
| 151 | `CMD_FMT_ITALIC` | toggle italic |
| 152 | `CMD_FMT_STRIKE` | toggle strikethrough |
| 153 | `CMD_FMT_CODE` | toggle inline code |
| 154 | `CMD_LINK` | open the link popover |
| 155 | `CMD_LINK_REMOVE` | remove the link at the caret |
| 156 | `CMD_BLOCK_P` | normal text |
| 157–162 | `CMD_BLOCK_H1` … `CMD_BLOCK_H6` | heading n (the same level again → normal text) |
| 163 | `CMD_LIST_BULLET` | bulleted list |
| 164 | `CMD_LIST_NUMBER` | numbered list |
| 165 | `CMD_LIST_TASK` | task list |
| 166 | `CMD_QUOTE` | quote |
| 167 | `CMD_CODEBLOCK` | code block |
| 168 | `CMD_CODE_LANG` | open the code-language popover |
| 169 | `CMD_INS_TABLE` | insert a table; `arg = rows << 4 \| cols` (0 = 3 × 3) |
| 170 | `CMD_INS_FORMULA` | inline formula |
| 171 | `CMD_INS_FORMULA_BLOCK` | formula block |
| 172 | `CMD_INS_DIAGRAM` | diagram; `arg` = template 0–8 |
| 173 | `CMD_INS_IMAGE` | picture from a file |
| 174 | `CMD_INS_HR` | horizontal rule |
| 175 | `CMD_NEW_PARAGRAPH` | Ctrl+Enter |
| 176–185 | `CMD_TABLE_ROW_ABOVE`, `_ROW_BELOW`, `_COL_LEFT`, `_COL_RIGHT`, `_DEL_ROW`, `_DEL_COL`, `_ALIGN_L`, `_ALIGN_C`, `_ALIGN_R`, `_DEL` | table operations (§8.9), in this order |
| 186 | `CMD_BLOCK_MENU` | open the style popover |
| 187 | `CMD_TABLE_MENU` | open the table popover |
| 188 | `CMD_FORMULA_MENU` | open the formula popover |
| 189 | `CMD_DIAGRAM_MENU` | open the diagram popover |
| 190 | `CMD_EDIT_MORE` | open the "…" popover |
| 191 | `CMD_ATOM_EDIT` | open the selected atom's popup |
| 192 | `CMD_POPUP_DONE` | Ctrl+Enter in a popup |
| 193 | `CMD_POPUP_CANCEL` | Esc in a popup |
| 194 | `CMD_CONFLICT_LOAD` | conflict: load the disk version |
| 195 | `CMD_CONFLICT_KEEP` | conflict: overwrite with my edits |
| 196 | `CMD_SAVE_RETRY` | leave strip: retry |
| 197 | `CMD_DISCARD_EDITS` | leave strip: discard |
| 198 | `CMD_ENC_UTF8` | encoding strip: save as UTF-8 |
| 199 | `CMD_ENC_REMOVE_CHAR` | encoding strip: remove the character(s) |
| 200 | `CMD_RECOVERY_OPEN` | recovery strip: open the copy |
| 201 | `CMD_RECOVERY_RESTORE` | recovery strip: restore |
| 202 | `CMD_RECOVERY_DELETE` | recovery strip: delete |
| 203 | `CMD_OTHER_WINDOW` | other-window strip: go to it |
| 204 | `CMD_STRIP_CLOSE` | close a dismissible strip ([Только чтение]) |

`WM_COMMAND` dispatches `Command(LOWORD(wp), HIWORD(wp))`; menus pass 0. Every toolbar button, popover row and strip
button runs its id through `Command`, so tests and UIA drive exactly what a click does.

### 13.2 Queries (`enum Query`, after `Q_DOC_SERIAL = 41`)

| Id | Name | Returns |
|---|---|---|
| 42 | `Q_EDITING` | 1 editing, 0 not |
| 43 | `Q_EDIT_DIRTY` | 1 when `g.src != g.disk.text` |
| 44 | `Q_EDIT_CARET_SRC` | caret (focus) source offset, −1 when not editing |
| 45 | `Q_EDIT_ANCHOR_SRC` | selection anchor source offset (two queries instead of a 16-bit packing, T4) |
| 46 | `Q_EDIT_TOOL` | `lp = id \| row << 16` → centre `MAKELONG(x, y)` in client px of a bar button, the pencil (`CMD_EDIT_TOGGLE` in reading mode), a popover row, a strip, bubble or popup button; −1 hidden |
| 47 | `Q_EDIT_BAR` | slide progress 0–100 |
| 48 | `Q_UNDO_DEPTH` | lp 0 undo depth, lp 1 redo depth |
| 49 | `Q_RELOADS` | number of `LoadSource` runs after startup (proves "no reload") |
| 50 | `Q_SAVES` | successful saves |
| 51 | `Q_EDIT_POPUP` | lp = field → hwnd of the popup EDIT, 0 none |
| 52 | `Q_MAP_SELFCHECK` | lp 0: run `MapSelfCheck` now (in reading mode on a temporary map parse) → 1 ok, 0 broken; lp 1: failures counted under `FASTMD_EDIT_SELFCHECK` |
| 53 | `Q_SRC_HASH` | FNV-1a-32 of the UTF-16 units: lp 0 `g.src`, lp 1 `g.disk.text` |
| 54 | `Q_SRC_LEN` | lp 0 `g.src` length, lp 1 disk text length |
| 55 | `Q_EDIT_BUSY` | bits: 1 deferred re-parse, 2 autosave armed, 4 save in flight, 8 measure jobs, 16 preview job, 32 picture jobs, 64 bar sliding, 128 scroll glide, 256 popup change pending, 512 retry armed, 1024 journal armed, 2048 modal queue non-empty |
| 56 | `Q_EDIT_PHANTOM` | lp 0 anchor block or −1; lp 1 kind (`PhantomKind`); lp 2 style |
| 57 | `Q_BLOCK_COUNT` | `g.doc.blocks.size()` |
| 58 | `Q_EDIT_SAVE_STATE` | 0 SAVED, 1 PENDING, 2 SAVING, 3 BUSY, 4 DENIED, 5 READONLY, 6 MISSING, 7 CONFLICT, 8 UNENCODABLE, 9 FAILED, 10 UNKNOWN, 11 OFF |
| 59 | `Q_EDIT_CONFLICT` | 1 while in conflict |
| 60 | `Q_EDIT_ENC` | `cp \| headerBytes << 24` |
| 61 | `Q_EDIT_EOL` | `g.eol`: 0 LF, 1 CRLF, 2 CR |
| 62 | `Q_EDIT_ACTIVE` | bits 0–4 `FMT_*` at the caret (pending included); 8 bullet item, 9 numbered, 10 task, 11 quote, 12 table cell, 13 code block, 14 atom selected, 15 phantom, 16 footnote, 17 link; bits 24–31 style: 0 text, 1–6 heading, 7 code, 8 table, 9 atom, 10 footnote, 11 raw leaf |
| 63 | `Q_LAST_PROMPT` | lp 0 kind of the last prompt (0 none, 1 LEAVE, 2 LEAVE2, 3 RELLINKS, 4 SAVEAS dialog, 5 OPENIMG dialog); lp 1 prompt count |
| 64 | `Q_RELAYOUT_ALL` | **changes state**: `ClearLayoutCache`, `UpdateColumns`, `InitGeometry`, `RecomputeY`, keeping `scrollY`, then a full repaint (T7) |
| 65 | `Q_FRAME_STATS` | lp 0 partial (scrolled) frames since the last lp 0 query, lp 1 full frames since the last lp 1 query |
| 66 | `Q_RENDERS` | formula/diagram renders + picture decodes started since start (both workers) |
| 67 | `Q_EDIT_STATS` | µs over the last 128 `EditReparse`: lp 0 median, 1 p95, 2 max, 3 count, 4 parse median, 5 carry+diff median, 6 install+layout median |
| 68 | `Q_EDIT_CARET_VISIBLE` | 1 when the caret is drawn in this frame |
| 69 | `Q_EDIT_CARET_PHASE` | blink toggles since entry |
| 70 | `Q_EDIT_POPUP_STATE` | 0 none, 1 ok, 2 error, 3 render pending |
| 71 | `Q_EDIT_ATOM` | selected atom id or −1 |
| 72 | `Q_EDIT_STRIP` | 0 none, 1 CONFLICT, 2 ENCODING, 3 LEAVE, 4 READONLY, 5 MISSING, 6 RECOVERY, 7 OTHER WINDOW |
| 73 | `Q_EDIT_RAW` | 1 while a raw-while-typing override is active |
| 74 | `Q_EDIT_BUBBLE` | 1 while the link bubble is shown |
| 75 | `Q_EDIT_COLLAPSE` | toolbar collapse level 0–6 |

Existing queries: `Q_MATH` gains lp 3 = entries shown with stale or failed pixels (T23); `Q_CARET` keeps returning the
caret in the blink's off phase; `Q_DOC_SERIAL` now changes on every model replacement (loads and edits) — "no
reload" is proved with `Q_RELOADS` (T2, changed in the same commit as `test_tasks`).

### 13.3 Timers (`TIMER_UPDATE = 5` is taken)
`TIMER_CARET = 6`, `TIMER_EDIT_SAVE = 7` (autosave), `TIMER_EDIT_RETRY = 8` (save backoff), `TIMER_EDIT_IDLE = 9`
(1 s after the last edit: measure unknown blocks, deferred remote pictures), `TIMER_EDIT_REPARSE = 10` (big-document
tick), `TIMER_EDIT_JOURNAL = 11`, `TIMER_EDIT_POPUP = 12` (popup text → splice), `TIMER_EDIT_UI = 13` (debounced UIA
events, popup EDIT re-show). The bar slide uses the message-loop animation branch, not a timer.

### 13.4 Messages
`WM_APP_PREVIEW = WM_APP + 9` (lParam `PreviewResult*`), `WM_APP_EDITINPUT = WM_APP + 10` (wParam `EI_TEXT` = 1 with
lParam = seq, `EI_KEY` = 2 with lParam = `vk | mods << 16`, `EI_FOCUS` = 3), `WM_APP_SAVED = WM_APP + 11` (lParam
`SaveResult*`), `WM_APP_TESTKEY = WM_APP + 12` (wParam vk, lParam `KM_CTRL = 1 | KM_SHIFT = 2 | KM_ALT = 4`; only with
`FASTMD_TEST_HOOKS`), `WM_APP_REPLAY = WM_APP + 13` (the modal queue). `WM_APP_QUERY` stays `WM_APP + 64`.
`WM_APP_FILECHANGED` gains wParam 1 = unavailable; `WM_APP_IMAGES` carries a result batch; `WM_APP_SCALED` carries keyed
results. Registered message `FastMD.EditOwner` (D18). `WM_COPYDATA` carries test splices.

### 13.5 Environment hooks (read once at startup; zero cost when unset)

| Variable | Effect |
|---|---|
| `FASTMD_TEST_ANSWER` | `leave:yes\|no\|cancel,leave2:…,rellinks:yes\|no[,auto-dismiss:<ms>]` — pre-answers every prompt (a missing kind answers cancel); `auto-dismiss` shows the real box and posts IDCANCEL after `<ms>`, so modal re-entrancy is exercised (T1) |
| `FASTMD_SAVE_AS` | the path the Save As dialog returns (empty = cancelled) |
| `FASTMD_OPEN_FILE` | the path(s) the picture dialog returns (`\|`-separated) |
| `FASTMD_TEST_HOOKS=1` | `WM_COPYDATA` dwData 1 = splice (`"at\tlen\ttext"`, UTF-16) through the splice primitive + `EditReparse` (reading mode too, no save); dwData 2 = `SaveSource` now; dwData 3 = a modal loop of its own for `"<ms>"` (§10.10 tests); enables `WM_APP_TESTKEY` (T2, T5) |
| `FASTMD_TEST_SLOW` | `images:<ms>,scale:<ms>,preview:<ms>,fullparse:<ms>,save:<ms>` sleeps per job (T11, T13) |
| `FASTMD_TEST_FAIL_WRITE` | `<kind>[:<n>][,always][,norollback]`, kinds `partial` (fail after n bytes), `busy`, `denied`, `missing`, `short_read`, `flush`, `close`, `recovery`, `diskfull`; once by default (T25, D24) |
| `FASTMD_AUTOSAVE_MS` | autosave delay override (T8) |
| `FASTMD_CARET_STEADY=1` | caret always visible, never blinking, regardless of activation (T17) |
| `FASTMD_EDIT_SELFCHECK=1` | `MapSelfCheck` after every `EditReparse`, full proof decode on every save (T22) |
| `FASTMD_EDIT_DEBOUNCE_CHARS` | big-document threshold (0 = defer every eligible keystroke) (T11) |
| `FASTMD_ACP` | the ANSI code page to assume instead of `GetACP()` (tests only, D1, T18) |

### 13.6 `ui_smoke.py` support
- Dicts mirror §13.1–13.2; `VK` gains back, delete, return, tab, f2, `oem_3`.
- Helpers: `wait_for(pred, timeout)`, `dbl_click(hwnd, x, y)`, `settle(hwnd)` (`Q_TARGETY == Q_SCROLLY`,
  `Q_EDIT_BUSY == 0`, `Q_EDIT_BAR ∈ {0, 100}`), `src_hash(text)` (FNV-1a-32 over UTF-16LE units), `edit_tool(id, row)`,
  `set_clipboard(text=None, fmt=None, files=None)` (plain, the private format, `CF_HDROP`), `popup_set(text)`,
  `testkey(vk, mods)`, `copydata(kind, payload)`; `type_text` posts UTF-16 code units (an emoji becomes two `WM_CHAR`s,
  T18).
- Edit tests run with `FASTMD_CARET_STEADY=1`, `FASTMD_EDIT_SELFCHECK=1`, `FASTMD_TEST_ANSWER=leave:cancel` (an
  unexpected prompt fails the test instead of hanging it), each on its own file (never `features.md`); the close helper
  asserts `Q_MAP_SELFCHECK` lp 1 == 0.

---

## 14. Test plan

### 14.1 `fastmd-edit-tests` (window-free, T3)
- **Target**: `option(FASTMD_TESTS "Build the edit-mode unit tests" ON)`; console exe from `tests/edit/edit_tests.cpp`,
  `src/editcore.cpp`, `src/editops.cpp`, `src/editfile.cpp`, `src/parse.cpp`, `html.cpp`, `highlight.cpp`, `util.cpp`,
  `theme.cpp`, `md4c.c` with `MD4C_USE_UTF16 FASTMD_NO_APP`; it defines `TexAvailable`/`MermaidAvailable` from a global
  switch. Exit code = number of failures; total runtime under 2 s. `tests/edit/run.ps1` builds and runs it.
- **Golden format** (`tests/edit/cases/*.txt`):
  ```
  === <name> (§ref)
  src:     <source; ‸ caret, ⟦ ⟧ selection; \n, \t, \\ escapes>
  tex:     on | off | both              (optional; default both, on when the case contains '$')
  flags:   noquote nolist nocrlf        (optional: skip automatic variants)
  do:      <op> [; <op> …]
  want:    <expected source with ‸ or ⟦ ⟧>
  phantom: none | after <b> | before <b> | break <b> [style <n>]      (optional)
  render:  <rendered text with <b> <i> <s> <code> <a> markers>        (optional)
  state:   atom=<id> pending=<fmt> refused=<id>                       (optional)
  ```
  Ops: `type "…"`, `Enter`, `S-Enter`, `C-Enter`, `BS`, `C-BS`, `Del`, `C-Del`, `Tab`, `S-Tab`, arrows/Home/End with
  `S-`/`C-` prefixes, `sel(a,b)`, `click(t,b[,c])`, `Bold`, `Italic`, `Strike`, `Code`, `Link("u")`, `Unlink`, `P`,
  `H1`…`H6`, `Bullet`, `Number`, `Task`, `Quote`, `Fence`, `Lang("js")`, `Table(r,c)`, `Table.<Op>`, `Formula`,
  `FormulaBlock`, `Diagram(n)`, `Image("dest","alt")`, `Hr`, `Paste("…")`, `PastePrivate("…")`, `Cut`,
  `TaskToggle(n)`, `AtomSource(n,field,"…")`, `Undo`, `Redo`, `adopt "…"` (an external change), `tick(ms)` (time for
  the undo tests).
- **Automatic variants** of every case: LF and CRLF (every `\n` of `src`/`want` becomes `\r\n`); TeX on/off; wrapped
  in a quote (every line prefixed `> `, blank lines `>`); wrapped in a list item (`- ` then `  `). Flags opt out.
- **Automatic checks after every op**: `MapSelfCheck` on both documents; the caret is a stop, not inside an atom, not in
  `BS_SYNTH`; the inverse splices give the old source byte for byte and redo gives the new one; no splice splits a
  surrogate pair or a CRLF.
- **Mandatory coverage**: every example row of §6.3, §6.5–6.7, §7.3–7.11 and §8.2–8.9; `map_*` goldens with exact
  `blockSrc`, `cellSrc`, `spans`, containers and image ranges for every construct of §4.4 (footnotes out of order,
  one-line and md4c alerts, picture-only paragraphs, reference definitions before a paragraph, front matter as table
  and as YAML, HTML blocks, setext, unclosed fences in a quote, the injected HR, empty items, tabs in code indentation,
  CRLF); `DiffBlocks` p/q for typing, a new heading and an inline picture inserted before a badge (index shift);
  `EditChord` for every vk with Ctrl+Alt (must be 0) and every §2.7 chord; `UndoStack` coalescing with injected time
  («Новый мир» → 2 steps); `editfile` on temp files: the byte-exact entry check (a CP1251 file under `FASTMD_ACP=65001`
  and a stray byte under 932 are refused), splice-local output equals the full encode for UTF-8, UTF-8+BOM+CRLF,
  UTF-16 and 1251, lone-surrogate and BOM-look-alike reasons, stateful code pages refused, every
  `FASTMD_TEST_FAIL_WRITE` kind leaves the original bytes (or the recovery file with `norollback`).
- **Property sweeps** over `bench/corpus/*.md` and `app/tests/*.md` (strided on large.md): at every caret stop
  `OpType("x")` renders the old text with `x` at `t` and unchanged neighbour flags (or a verified fallback); at every
  `t` inside a PLAIN segment `OpBackspace` removes exactly one cluster; `TextOfSrc(SrcOfText(t)) == t` at every stop.

### 14.2 Fuzzing (T14)
`fuzz.cpp`: TeX availability toggled per input (`g_texOn = rng() & 1`); a third of the inputs get CRLF; every input
parsed with maps and checked by `MapSelfCheck` and by the round trips of the corpus sweep (`tests/edit/map_sweep.h`,
strided to a few thousand stops and offsets per input) — a failure writes `out\fuzz\map-<n>.md` and returns 3
(`run.ps1` reads the exit code). NUL is in the mutation alphabet. `--edit` mode (from 2b): 50 random operations at random caret stops, `MapSelfCheck` after each, and
"undo all == the original bytes" at the end. 200 000 inputs under ASan at every gate.

### 14.3 UI tests (`ui_smoke.py`, each on its own file)

| Phase | Test | Asserts |
|---|---|---|
| 1a | `test_map_selfcheck` | `Q_MAP_SELFCHECK == 1` on features, medium, footnotes, html, math and large.md (after the full parse) |
| 1a | `test_copy_md_exact` | COPY_MD after an emoji, after an entity and on the first line gives the exact source lines; a selection in the front-matter table no longer copies the whole file |
| 1b | existing image tests | green: math, remote, svg, wide_diagram, image_scaling |
| 1b | `test_image_race` (`SLOW=images:300`) | theme switch, reload and zoom during renders: alive, `Q_MATH(1) == 3`, `Q_RENDERS` ≤ 2 × distinct keys × contexts |
| 1b | `test_reload_during_update` | fake `/latest` delays 5 s; a reload returns within 0.5 s |
| 1c | `test_tasks` (rebased, T2) | byte for byte in utf8, bom-crlf, utf16, ansi; own writes leave `Q_RELOADS` unchanged; `Q_EDIT_ENC` right |
| 1c | `test_task_swap` | a tick next to a formula leaves `Q_RENDERS` unchanged; a mid-document tick leaves `Q_SCROLLY` unchanged and pixels equal to those after `Q_RELAYOUT_ALL` |
| 1c | `test_splice_hook` (`TEST_HOOKS`) | 20 scripted splices: `Q_SRC_HASH` matches, self-check ok, pixels equal after `Q_RELAYOUT_ALL`, `Q_RELOADS` unchanged |
| 1c | `test_save_fault` | `FAIL_WRITE=partial:10` via the save hook: original bytes, state FAILED, dirty, no recovery file left; a later save succeeds. `partial:10,norollback`: torn file, recovery kept; reopening shows the RECOVERY strip; `CMD_RECOVERY_RESTORE` gives the original bytes |
| 1c | `test_reading_touch` | reading mode: `os.utime` with equal content → `Q_RELOADS` unchanged; changed content → reload |
| 2a | `test_edit_enter_leave` | entry by double-click (`Q_EDIT_CARET_SRC` = click point), F2, pencil, `CMD_EDIT_HERE`; `Q_EDIT_BAR == 100`; ✕ via `Q_EDIT_TOOL`; Esc chain (find first); three Esc presses leave the process alive; refusals with toasts: start screen, load-failed, UTF-16 BE, NUL bytes, 1251 under `FASTMD_ACP=65001`; triple-click cancels entry (`Q_SAVES == 0`, paragraph selected) |
| 2a | `test_edit_fullpending` (`SLOW=fullparse:1500`, large.md) | a double-click during the full parse shows the toast, then entry happens by itself when it arrives (< 2 s) |
| 2a | `test_edit_typing_{utf8,bomcrlf,utf16,ansi}` | `Q_SRC_HASH` right after typing; after `settle` only the edited bytes differ; `Q_EDIT_ENC`/`Q_EDIT_EOL`; title `*` only while dirty; one save per burst; `Q_RELOADS` unchanged; ANSI text made of ACP round-trip characters with a byte ≥ 0x80 invalid in UTF-8, else skipped with a printed reason (T18) |
| 2a | `test_edit_keys_once` | posted VK_BACK/VK_DELETE act once; Ctrl+Backspace inserts no 0x7F; Tab in a paragraph does nothing; Space types a space |
| 2a | `test_edit_surrogates` | a lone high surrogate: file unchanged after the autosave delay; the low half: the emoji is in the file |
| 2a | `test_edit_undo` | «Новый мир» → `Q_UNDO_DEPTH == 2`, one undo leaves «Новый »; history survives saves and leave + re-enter; a reload clears it; an adopted external change is undone first, then our typing |
| 2a | `test_edit_guards` | `CMD_RELOAD` saves, leaves, reloads (`Q_RELOADS + 1`, edits in the file); Ctrl+click (MK_CONTROL) on a link to other.md saves and navigates; posted VK_BACK does not navigate; theme switch on math.md: still editing, `Q_RELOADS` unchanged; a task click is an undoable splice; a reading-mode tick then F2 + typing raises no conflict; Ctrl+E with a dummy editor saves, leaves and launches |
| 2a | `test_edit_conflict` (`AUTOSAVE_MS=60000`, T8) | dirty → external write → `Q_EDIT_CONFLICT == 1`; `CMD_SAVE` leaves the bytes; `CMD_CONFLICT_KEEP` writes ours and keeps `.theirs`; `CMD_CONFLICT_LOAD` → `Q_SRC_HASH` = disk, `CMD_UNDO` brings ours back; same-size change + `os.utime(old)` → `CMD_SAVE` reports CONFLICT; the not-dirty adopt path keeps `Q_RELOADS` and adds one undo step |
| 2a | `test_edit_busy_retry` | a share-none handle held 2 s after typing → BUSY, then saved after release, one toast |
| 2a | `test_edit_readonly_missing` | read-only → READONLY strip at entry; `FASTMD_SAVE_AS` moves the document to the new path; renaming the file while editing → MISSING strip, renaming back → saved; closing with `leave:no` returns 0 and leaves the file unchanged |
| 2a | `test_edit_encoding_strip` (`FASTMD_ACP=1251`) | typing ✓ in a 1251 file → UNENCODABLE, ENCODING strip, no dialog; `CMD_ENC_UTF8` → EF BB BF + UTF-8; `CMD_ENC_REMOVE_CHAR` in a second run → bytes unchanged, not dirty |
| 2a | `test_edit_close_session` | WM_CLOSE 100 ms after typing → saved, rc 0; sent `WM_QUERYENDSESSION` + `WM_ENDSESSION(1)` → saved, journal gone; `recovery\` empty after a clean leave |
| 2a | `test_edit_recovery` | autosave off (`Autosave = 0`), typing, 3.5 s → `.unsaved` exists; the process is killed; reopening shows «Есть несохранённые правки»; `CMD_RECOVERY_RESTORE` → editing with the journal text |
| 2a | `test_edit_two_windows` | a second process on the same file cannot enter (`Q_EDIT_STRIP == 7`) |
| 2a | `test_edit_frames` | a wheel scroll in edit mode gives `Q_FRAME_STATS` partial > 0 and the partial shot equals `Q_FULL_REDRAW`; after typing, a heading appearing/vanishing, a picture before a badge, a list renumber, a table row and a formula edit the shot equals the shot after `Q_RELAYOUT_ALL` (T7); toolbar light + dark; a 400 px window shows "…" (`Q_EDIT_COLLAPSE ≥ 3`) |
| 2a | `test_edit_blink` (no STEADY) | `Q_EDIT_CARET_PHASE` changes within 2 × `GetCaretBlinkTime()`; posted WM_KILLFOCUS → `Q_EDIT_CARET_VISIBLE == 0`, WM_SETFOCUS → 1 |
| 2a | `test_edit_perf` | 200 characters into medium.md: `Q_EDIT_STATS` median < 3000 µs, p95 < 8000 µs; 50 into large.md (deferred); numbers printed |
| 2a | `test_edit_debounced` (`DEBOUNCE_CHARS=0`) | "ab" after an emoji, End, "c" → exact file text; `Q_EDIT_BUSY` bit 1 set right after typing and clear within 300 ms |
| 2a | `test_settings_autosave` | hit ids 1200/1201 switch the setting; with autosave off nothing is saved until `CMD_SAVE` or leaving |
| 2a | `test_basics` (updated, T24) | double-click → `Q_EDITING == 1` → Esc → 0; file hash unchanged, `Q_SAVES == 0` (the word-selection check moves to `test_edit_selection`) |
| 2b | edit-tests goldens | §6.5–§7.11 |
| 2b | `test_edit_structure` | Enter at a paragraph end → `Q_EDIT_PHANTOM ≥ 0`, file unchanged; typing materialises it; Enter in a list; Backspace at a heading start; Tab in a list; Up/Down stop on an empty item and empty cells; Down at the end of a document ending with a table → phantom; a click below the last block → phantom; in a mixed-EOL file Enter writes the line's own ending |
| 2b | `test_edit_selection` | double-click selects a word, triple-click the paragraph; typing over a bold selection stays bold; deleting across two paragraphs keeps a reference definition between them |
| 2b | `test_edit_paste_plain` | `set_clipboard` + `CMD_PASTE` into a quote re-prefixes; into a cell `<br>` and `\|`; `CMD_CUT` |
| 2b | `test_edit_table_typing` | typed `\|` → `\|` escaped; typing into a missing cell completes the row; Enter moves down; Tab selects the next cell; Enter on an empty last row → phantom |
| 2b | `test_edit_raw_typing` | typing `<!--` in a new paragraph: `Q_EDIT_RAW == 1`, later blocks still rendered; typing `![x](img/diagram0.png)` stays raw until the caret leaves, then becomes a picture |
| 2c | `test_edit_find` (T15) | with the find box focused posted characters reach the box and `Q_SRC_HASH` is unchanged; after a click into the document they edit it and `Q_MATCHES` refreshes with `Q_SCROLLY` unchanged |
| 2c | `test_edit_outline` | `Q_TOC_DOCKED` unchanged after deleting the last heading; `Q_TOC_CURRENT` stable while typing; after leaving the panel follows the headings |
| 2c | `uia_check.ps1` | §14.4 |
| 3a | edit-tests goldens | §8 |
| 3a | `test_edit_commands` | every format/block/list/quote/code-block CMD: `Q_EDIT_ACTIVE`, then the bytes; disabled commands in a cell change nothing; Ctrl+2 twice returns to text |
| 3a | `test_edit_table` | `CMD_INS_TABLE` with an arg; grid popover rows via `Q_EDIT_TOOL`; every `CMD_TABLE_*` on the bytes |
| 3a | `test_edit_hr` | `CMD_INS_HR` → HR + phantom after it |
| 3b | `test_edit_popups` | a click on a formula opens a popup (`Q_EDIT_POPUP`); `popup_set` → `Q_RENDERS + 1`, state 1; broken TeX → state 2, `Q_MATH lp 3 == 1`; Esc restores the source and leaves `Q_UNDO_DEPTH` unchanged; Ctrl+Enter keeps; an emptied inline formula disappears with its `$…$`; diagram templates 0–8 render (`Q_MATH lp 2 == 0`); the image popup edits alt and path; a wheel scroll that hides the anchor closes the popup and keeps the text; WM_CLOSE 50 ms after `popup_set` saves the popup text (D20) |
| 3b | `test_edit_link` | the link popover builds `[sel](url)`; `<…>` for a URL with blanks; the reference-link notice; `CMD_LINK_REMOVE`; autolink removal escapes |
| 3b | `test_edit_image` | `FASTMD_OPEN_FILE` → `![name](<img/…>)`; a dropped png is inserted at the drop point; a `CF_HDROP` paste inserts; a bitmap-only paste shows the toast and changes nothing |
| 3b | `test_edit_paste_private` | copy then paste inside FastMD keeps balanced markup |
| 3b | `test_edit_bindings` (T5) | isolated: real posted chords (Ctrl+Z/Y/B/I/K/S/1, Ctrl+Shift+7/8/9/Q/K, Ctrl+T/M, Ctrl+`, Ctrl+Enter); the file is reset and the app relaunched between retries |
| 3b | `test_edit_race_images` (T13) | math.md + a remote picture served after 2 s, `SLOW=images:300,preview:300`: 30 characters, a theme switch, 30 undos while jobs are pending → alive, `Q_MATH(1) == 3`, `Q_RENDERS` bounded, self-check failures 0 |
| 3b | `test_edit_bubble` (optional) | the bubble shows with the caret in a link; its buttons work |
| 4 | final gates | `--zoom=150` light/dark shots of bar, popovers, strips, popups; speed guard of record; size budget; `preview_direct` |

### 14.4 Other checks
- **`uia_check.ps1`** (T13): `IsReadOnly` false while editing; the document range contains typed text; a range captured
  before a deletion does not throw in `GetText`, `FindText`, `Select`; a `GetText` loop in a background job while
  ui_smoke types does not crash the app; (2c) bar buttons are found by name and `Invoke` runs them.
- **`tests/preview_direct.py`** (T16): `ctypes` loads `fastmd-preview.dll`, calls `DllGetClassObject(CLSID_FastMdThumb)`,
  creates the `IThumbnailProvider`, initialises it through **`IInitializeWithFile`** (what the DLL implements,
  preview.cpp:368) with features.md and math.md, calls `GetThumbnail(1024)` and compares the pixels with a baseline made
  from the 1.2.0 DLL at the start of Phase 1a (`app/tests/out/preview-baseline/`). No registry writes;
  `preview_check.py` is never run on this machine.
- **ASan app build**: `FASTMD_ASAN_APP=ON` builds FastMD under ASan; once per phase the edit UI tests run against it.

---

## 15. Phases

### 15.1 The common gate (every phase, T10)
1. Full build of all targets (`app/build.ps1`): FastMD, `fastmd_preview` (only its link catches missing stubs),
   `fastmd-edit-tests`, the setup; no new warnings in new files.
2. `tests/edit/run.ps1` green.
3. Fuzz: 200 000 inputs under ASan (`FASTMD_FUZZ=ON FASTMD_ASAN=ON`), with `--edit` from 2b.
4. The full `python -X utf8 app/tests/ui_smoke.py` green, with at most one `FASTMD_ONLY=key_selection` rerun.
5. `uia_check.ps1` green.
6. `preview_direct.py` equal to the baseline.
7. Speed guard in the explorer context (`bench/tools/explorer-launch`): the gap to baseline-win32 ≤ 0 ms on medium and
   ≤ +15 ms on large (1.2.0: −16.4 / −16.5 ms); interleaved A/B parse time of medium and large-prefix without maps
   unchanged within noise.
8. Exe size recorded; `FastMD.exe` ≤ 1,450,000 bytes at Phase 4 (§1 principle 3), edit-only TUs built with `/O1`.
9. Light and dark screenshots of every new visual (from 2a); 100 % and 150 % in Phase 4.
10. The edit UI tests once against the ASan app build.
11. The phase report records §5.8's numbers, the local flush cost, the size and any code-vs-design differences.

### 15.2 Phases (T10, Appendix C of the testability review)

| Phase | Content | Acceptance |
|---|---|---|
| **1a Parser** | md4c patch; `ParseOptions`; Builder maps, containers, tables, spans, image ranges; `MapSelfCheck`; `editcore` skeleton (types, `CaretStop`, `SrcOfText`, `TextOfSrc`, `DiffBlocks`); `fastmd-edit-tests` with the `map_*` goldens; fuzz invariants; all ids of §13 declared; `Q_MAP_SELFCHECK`; `srcMap` emoji fix and the two `copy.cpp` fixes; preview baseline captured. **No behaviour change.** | map goldens for every construct of §4.4; fuzz green; `test_map_selfcheck`; `test_copy_md_exact`; parse cost without maps unchanged |
| **1b Workers** | `Spawn` kinds, `JoinDocReaders`, render table, long-lived picture worker, keyed and decoupled scaler, measure index lists, `loadGen`, `FASTMD_TEST_SLOW`, `Q_RENDERS` | existing image tests; `test_image_race`; `test_reload_during_update` |
| **1c Swap and save** | `EditReparse` (§5.5), `DiskState` and encoding (§10.1), `SaveSource` with recovery (§10.3, §10.5), `ToggleTask` rebased (T2) with `test_tasks` on `Q_RELOADS`, `UndoStack` (unit-tested), test hooks (`TEST_HOOKS`, `TEST_FAIL_WRITE`, `ACP`), `Q_SRC_HASH`, `Q_RELAYOUT_ALL`, `Q_RELOADS`, `Q_SAVES`, `Q_EDIT_ENC/EOL`, watcher "unavailable" + backoff, reading-mode content compare, the recovery strip | `test_tasks`, `test_task_swap`, `test_splice_hook`, `test_save_fault`, `test_reading_touch`; **review gate** |
| **2a A minimal editor that cannot lose data** | first the queries and env switches (T1, T4, T6, T17); then **all guards before the first UI splice**: entry checks, mutex, write probe, `CanLeaveDocument`/`PrepareToClose`/session end, conflicts and adoption, modal depth, journal, task routing in edit mode, theme re-parse; then the bar shell (slide, inset and compensation, ✕, undo/redo, status, strips, pencil, collapse, settings row), caret, blink, stops, navigation, typing, in-block Backspace/Delete, undo/redo, autosave. Object blocks are whole-block stops; typing next to them shows the hint (T9); popups not yet; cut/paste unbound (T9) | the 2a rows of §14.3; **review gate** |
| **2b Structure** | goldens first, then: phantoms, Enter/Shift+Enter/Ctrl+Enter, the Backspace/Delete matrix, Tab, selection delete/replace with balancing, §7.4 escaping, §7.5 routine, raw-while-typing, table typing rules, cut and plain-text paste (§7.11) | the 2b rows |
| **2c Integration** | UIA (read-only, stamped ranges, select routing, toolbar fragments), find routing and `FindRefresh`, outline freezing, `uia_check` additions, the Ctrl+click hover tip | the 2c rows |
| **3a Commands** | §8.1 matrix, inline toggles, block style, lists, quote, code block, table insert + operations + popovers (grid, actions, style, more), HR, formula/diagram source inserts (the popup opens in 3b) | the 3a rows |
| **3b Popups** | input-thread popups (formula, diagram, HTML, front matter, image), link and code-language popovers, the preview worker with warm-up and error display, atom selection and opening, the picture dialog, drag-and-drop and `CF_HDROP`, the private clipboard format; optional: link bubble, `*_svg2` exports | the 3b rows |
| **4 Review and polish** | adversarial multi-lens review and fixes, screenshots (light/dark, 100/150 %), speed guard of record, `preview_direct`, README RU+EN (edit mode, keys, autosave, IME limit, recovery and journal storage, ASCII → UTF-8, encryption note), PLAN item, CHANGELOG, the key tables of `app/README.md` (`:50`, `:66`, `:68`, `:70`), release-notes draft | final gates |

Rule for every phase: guards, flush-on-leave and the close/session-end flush land in the same commit as (or before) the
first path that mutates `g.src` from the UI (T9).

### 15.3 Review gates
After 1c (the swap and save code — the data-loss surface) and after 2a (the first UI mutation), an adversarial review
with the lenses mapping, architecture, data safety, UX and testability; its findings are fixed before the next phase
starts. Phase 4 repeats it over the whole feature.

---

## 16. Out of scope for v1
- CJK IME, the emoji panel and dictation in the document itself (they work in popups).
- Drag-and-drop of text inside the document (dragging out keeps working as a copy).
- Pasting pictures from the clipboard (bitmaps): a toast explains it.
- Converting `CF_HTML` to Markdown on paste.
- Spell checking; find and replace; multiple carets.
- Collaborative editing and three-way merging of external changes.
- Editing inside `<details>` summaries and inline HTML structure beyond typing text between tags (HTML blocks are
  edited as source).
- Following a same-folder rename by file ID (D16, optional part).
- Rewriting the written numbers of ordered lists (md4c numbers from the start anyway).
- Edit mode in the Explorer preview pane or thumbnails.
- Stateful code pages (ISO-2022, ISCII, UTF-7): edit mode is refused.
- Detecting BitLocker To Go / VeraCrypt volumes for recovery encryption (README note only).

---

## Appendix A. Strings

Every RU/EN string in this document (tooltips §2.3, popovers §2.4, strips §2.5, status §2.6, menus §2.9, popup texts
§2.10 and §9, toasts §2.1, §7, §10, prompts §10.8, placeholders §6.7 and §7.10, the settings row §2.12) is appended to
`strings.h`/`strings.cpp` as one `S_ED_*` group, in the order they appear here, in both tables at the same position
(the `static_assert` checks only the count). Strings with `<…>` placeholders are format strings. Shortcut labels are
never stored: they are built at run time (§2.3).

---

## Appendix B. Review resolution

Every finding of the five v1 reviews, where v2 resolves it. "Accepted" = the reviewer's fix (or the simpler of the
offered alternatives) is normative; "adapted" = resolved differently, with the reason.

| Id | Resolved in | Note |
|---|---|---|
| F1 | §4.2, §6.4, §6.5 | accepted: `lineEnd` in `BlockSrc`, containment on `[line, outerEnd]`, trailing blanks map to the line end with `trailCols`; cells bounded by the separator pipe |
| F2 | §7.6 | accepted: every split closes and reopens open spans; links duplicate their closer; code reuses its run |
| F3 | §4.1 (P2, P3), §4.4 | accepted: verbatim indentation per character, one atom per static, statics at edges; exact extents come from two md4c callbacks (`verbatim_line`, `break_extent`) instead of gap arithmetic; `↩` is `SEG_SYNTH`; `<img>`/`<br>` use their HTML chunk |
| F4 | §4.4, §6.3.1, §7.6 | accepted: map mode trims one trailing `\n`; empty fence insertion point; indentation counted after the container prefix |
| F5 | §4.1 (P6), §4.4, §7.6, §7.7 | accepted: `footnote_extent`, `BS_FOOTNOTE`, no joins/splits across definitions, joins in source order |
| F6 | §7.7, §7.9 | accepted: whitespace-only joins, invisible lines kept or moved, cuts clamped to code/HTML/front-matter content |
| F7 | §6.4, §6.6, §7.6 | accepted: normalisation, whitespace at break edges, `\\`, Enter replaces the break atom, virtual trailing break |
| F8 | §7.4 | accepted |
| F9 | §7.5 | accepted: one emission routine (merge, `*` intraword, flanking, `$` spacing, verification); a failing edge extends to the word boundary, then refuses |
| F10 | §7.5 (step 1), §7.3 | accepted; plus "sticky" continuation so typing a blank at the end of bold keeps typing bold |
| F11 | §8.2 | accepted: split at enterable-span and link-text edges, grow over atomic spans, rendered text for code, pending on the first non-blank |
| F12 | §6.3 | accepted, generalised: the insertion inherits the previous character's format but never enters a non-enterable span from its edge |
| F13 | §7.9 | accepted |
| F14 | §4.1 (P1, P5, P7, P8), §4.4 | accepted: (a) marker offset through `task_mark_off` + scanning back from `[`; (b) stash cleared per `Emit`, empty-item extents; (c) setext underline scanned into `outerEnd`; (d) TABLE extent; (e) injected HR keeps its line; (f) unclosed fence ends at its last line |
| F15 | §7.7, §8.4, §8.5, §8.6 | accepted: per-block prefix operations |
| F16 | §7.6, §7.7, §7.8 | accepted |
| F17 | §6.7, §7.6, §7.7, §8.4 | accepted |
| F18 | §6.7, §6.8, §7.6 | adapted: Enter on a selected image or HTML/front-matter atom opens its popup (DECISIONS); the phantom next to atoms comes from Ctrl+Enter, arrows at the document edges, a click below the last block, and Enter on a selected HR |
| F19 | §7.7 | accepted: complete matrix, joins only between text blocks in source order |
| F20 | §4.1 (P5), §4.4, §7.10, §8.9 | accepted |
| F21 | §7.11 | accepted |
| F22 | §6.9, §4.3 | accepted: masks for HTML-block starts, raw leaves for picture/formula paragraphs, `<!--` masked while unterminated and drawn as a warning |
| F23 | §8.3 | accepted; the reference-definition question is an in-popover notice confirmed by a second Enter (no modal) |
| F24 | §4.4, §7.7, §8.4 | accepted |
| F25 | §4.4, §6.3, §7.9 | accepted: HTML pseudo-spans with partner-safe deletion |
| R1 | §5.5 (steps 7–9) | accepted |
| R2 | §6.7 | accepted: the phantom is a view-level row; `g.doc` never changes for it |
| R3 | §5.1, §5.3 | accepted: bump before waiting; the scaler is decoupled (the reviewer's "better" option), so edits never wait for it and its flag cannot stick |
| R4 | §5.2, §5.3 | accepted: immutable shared pixels + `pxSerial`; `sc` tagged with the serial |
| R5 | §5.2 | accepted |
| R6 | §5.2 | accepted |
| R7 | §5.5 (steps 6, 10) | accepted |
| R8 | §6.8, §12.2, §12.3 | accepted |
| R9 | §5.7 | adapted: DECISIONS' deferral of pure typing with a forced synchronous re-parse before anything else, instead of section re-parsing or a worker parse |
| R10 | §5.4 | accepted |
| R11 | §9.2, §9.3 | accepted; plus the in-place fallback when the input thread cannot start |
| R12 | §10.9, §10.10 | accepted; adapted for `WM_QUERYENDSESSION`, which the system sends and cannot be re-posted: journal first, return TRUE |
| R13 | §5.5, §12.8 | accepted |
| R14 | §5.7, §10.8 | accepted |
| R15 | §5.2, §5.5 (step 4), §9.4 | accepted |
| R16 | §5.5, §5.8, §12.7 | accepted: scoped budget, lowerText splice, cell-layout reuse, TOC skip; no caret-only frame |
| R17 | §12.2 | accepted |
| R18 | §9.1, §12.1, §12.3 | accepted |
| R19 | §3.4, §4.3, §4.5 | accepted: maps opt-in (DECISIONS), stubs listed, pure self-check in the fuzz target |
| R20 | §5.2, §5.5 (step 11) | accepted; `canon` is replaced by shared pixels |
| R21 | §12.7 | accepted |
| R22 | §9.4 | accepted |
| R23 | §5.1 | accepted |
| R24 | §5.8, §10.3 | accepted: save worker from 1 M characters, cadence by size, measured |
| UX-1 | §6.5, §7.8 | accepted |
| UX-2 | §2.10, §6.2, §9 | accepted |
| UX-3 | §6.7, §6.8, §7.6, §8.8 | accepted |
| UX-4 | §12.1 | accepted; the compensation is written as a per-step `maxComp` formula |
| UX-5 | §2.1, §2.6, §2.12 | accepted: triple-click stays reading, dirty compared, toast, settings row (autosave on by default per DECISIONS) |
| UX-6 | §2.2 | accepted |
| UX-7 | §6.7, §7.6 | accepted |
| UX-8 | §6.7 | accepted |
| UX-9 | §8.1 | accepted; clarified: heading/list/quote are disabled in a table, block inserts go after the table |
| UX-10 | §7.10, §7.11 | accepted |
| UX-11 | §2.3 | accepted |
| UX-12 | §2.3, §2.7, §12.5 | adapted: DECISIONS chose Ctrl+1…6 and no Ctrl+Alt+digit, so the `ToUnicodeEx` test is unnecessary; all other key choices accepted |
| UX-13 | §2.10, §9.1, §9.2 | accepted |
| UX-14 | §2.8, §7.11, §8.8 | accepted |
| UX-15 | §2.2, §2.5, §10.6, §10.8 | accepted, except "F5 = flush only": F5 flushes, leaves edit mode and reloads (R14, the test plan); nothing is lost because the reload only follows a successful flush |
| UX-16 | §2.5, §10.4 | accepted, stronger: no modal encoding question anywhere (also not on Ctrl+S) |
| UX-17 | §10.7, §11 | accepted: adoption is an undo step, every undo is verified |
| UX-18 | §2.8, §2.9 | accepted |
| UX-19 | §2.8, §2.11 | accepted (bubble in 3b, optional per DECISIONS) |
| UX-20 | §7.6 | accepted |
| UX-21 | §7.7 | accepted for tight lists; loose lists keep a continuation paragraph (F16) |
| UX-22 | §2.1 | accepted |
| UX-23 | §2.3, §12.2, §12.8, §12.9 | accepted: UIA button fragments (the fuller option) plus shortcuts |
| UX-24 | §2.3–§2.6, §7.10, §10.8 | accepted; prompts worded for Да/Нет/Отмена, placeholders drawn only on screen |
| D1 | §10.1, §10.3 | accepted; the "equals the full encode" assertion is a full proof decode under 1 M characters and under `FASTMD_EDIT_SELFCHECK`, a window proof above (cost); stateful code pages refused |
| D2 | §10.3 | accepted |
| D3 | §10.5 | accepted |
| D4 | §8.10, §10.7, §11 | accepted |
| D5 | §10.1, §10.4, §10.7 | accepted, including both sizes in the conflict strip |
| D6 | §2.2, §10.7, §10.8 | accepted |
| D7 | §7.3, §10.4, §10.10 | accepted |
| D8 | §10.3, §10.4 | accepted |
| D9 | §10.8 | adapted: reason-specific Да/Нет/Отмена MessageBoxes, two-step where four choices exist (DECISIONS: MessageBox wording) |
| D10 | §10.6, §10.9 | accepted |
| D11 | §5.7 | accepted (forced synchronous re-parse) |
| D12 | §7.1, §10.3 | accepted |
| D13 | §10.1, §10.3, §10.7 | accepted |
| D14 | §10.1 | accepted (refusals) |
| D15 | §7.2, §7.11 | accepted; a file without line ends uses LF (the documented choice) |
| D16 | §10.7, §16 | accepted; following a same-folder rename by file ID is out of scope |
| D17 | §10.4 | accepted |
| D18 | §10.11 | accepted |
| D19 | §10.8 | accepted |
| D20 | §9.2 | accepted |
| D21 | §10.3 | accepted |
| D22 | §10.5, §10.12 | accepted; only EFS encryption is detectable (README note for other encrypted volumes) |
| D23 | §10.2 | accepted |
| D24 | §13.5, §14.3 | adapted: the fault kinds live in `FASTMD_TEST_FAIL_WRITE` (the DECISIONS hook list) instead of a new `FASTMD_FAULT`; every listed test case is planned |
| T1 | §10.8, §13.5 | accepted |
| T2 | §10.2, §14.3, §15.2 | accepted |
| T3 | §3.1, §3.2, §14.1 | accepted |
| T4 | §13.2, §13.6 | accepted; the selection is two queries |
| T5 | §2.7, §12.5, §13.4 | accepted |
| T6 | §13 | accepted; adapted: the slide is paced by the message loop, so no `TIMER_EDIT_ANIM` (id 8 is the retry timer); `CMD_POPUP_CLOSE` is `CMD_POPUP_CANCEL` because Esc cancels (UX-13) |
| T7 | §13.2, §14.3 | accepted |
| T8 | §14.3 | accepted |
| T9 | §15.2 | accepted: task routing in 1c/2a, the 2a rule for object blocks, guards first, cut/paste unbound until 2b |
| T10 | §15.1, §15.3 | accepted |
| T11 | §5.7, §13.5, §14.3 | accepted |
| T12 | §5.8, §13.2, §15.1 | accepted (maps opt-in) |
| T13 | §13.5, §14.3, §14.4 | accepted |
| T14 | §14.2 | accepted |
| T15 | §12.5 | accepted |
| T16 | §3.4, §14.4 | adapted: `IInitializeWithFile` (what the thumbnail provider implements), not `IInitializeWithStream` |
| T17 | §2.3, §12.2, §13.2 | accepted |
| T18 | §13.6, §14.3 | accepted |
| T19 | §11 | accepted |
| T20 | §2.7, §10.8 | accepted |
| T21 | §12.5, §14.1 | adapted: no Ctrl+Alt chords exist (DECISIONS), so `IsAltGrText` is replaced by a unit test that `EditChord` never claims Ctrl+Alt |
| T22 | §4.5, §13.5 | accepted |
| T23 | §9.2, §13.2 | accepted |
| T24 | §14.3 | accepted |
| T25 | §7.11, §13.5, §13.6, §14.3 | accepted; the recovery-file lifecycle follows D3 (kept only when the rollback fails, tested with `norollback`) |

### Phase 1 notes (the review gate after 1c)

Four lenses (mapping, threads, data safety, regressions) reviewed 1a–1c. What changed in the design with the fixes;
the per-finding verdicts and evidence are in the phase report.

| Finding | Resolved in | Note |
|---|---|---|
| Code span closer at a line start: a break atom inside the closer | §4.1 | md4c fix (the closer moves the line on); renders `` ``⏎foo⏎``bar `` as CommonMark does |
| Footnote definition inside a list item: two blocks on one line | §4.4, §4.5 #6 | the item stays `BS_SYNTH` (typing at its bullet would break the definition); neighbours in `blockOrder` must be disjoint |
| Empty referenced footnote definition: a second `↩` on the one before | §4.1 P6, §4.4, §6.3.1 | its own synthesized leaf with number, anchor and arrow (reading mode too), an insertion point after `]:`; `InSegs`/`LastStop` go to the start of a run of synthesized segments |
| NUL in code or raw HTML twice in the text | §4.1 | md4c fix; NUL in the fuzz alphabet |
| Inline tag across lines: a pseudo-span, its attributes editable text | §4.4 | the pieces are read as one tag in both modes; no pseudo-span |
| `## ##`: typing at `beg` breaks the heading | §6.3.1 | the op adds a trailing blank (`## x ##`); `InsertionPoint` stays `beg` |
| MapSelfCheck passed wrong maps | §4.5 | plain segments in the block's lines and in their cell, no segment in a delimiter, disjoint `blockOrder` neighbours, whole-line records, item markers whole, unique and before their content |
| The fuzzer never checked round trips | §14.2 | the sweep's oracle (`map_sweep.h`) on every input |
| SYNTH left neighbour in `SrcOfText` | §6.3 | counts as absent when a right one exists (as the 1a report said) |
| Kept recovery file hidden (own pid) and overwritten by the next save | §10.3 step 9, §10.5 | unique names with `CREATE_NEW`; the strip at once; no save while a leftover waits; pid + process start time |
| Restore over a file that changed since, or another file on FAT | §10.5 | FMDREC2: path, hashes around the range, write time; torn / changed / done / untouched; Restore rechecks and backs up |
| Recovery file deleted after an unflushed save | §10.3 steps 4, 10 | one recovery file stands for the last flushed version until the next flush |
| Leftover never verified | §10.5 | header and tail hashes; the result block tells a finished save |
| ANSI above 1 M characters could turn into UTF-8; FF FE after an ANSI file's UTF-8 mark | §10.3 step 3 | whole-file UTF-8 check when a high byte is involved; BOM check after any header |
| A tick holds the whole tail in the recovery file | §10.3 step 4 | a save that keeps the length holds only `[pb, pe)` |
| Unreadable file at open: a reload loop | §10.7 | the failed load keeps the stamp; locked → backoff; denied → F5 |
| Swap after a text-size change blanks every formula | §5.5 step 4 | the old picture is adopted on a table miss, in reading mode too |
| Swap waits for a measure thread blocked on a network picture | §5.1 | measure threads read no header from a share or a cloud placeholder |
| Activation stats failed pictures on the UI thread | §5.2 | at most every 2 s, no network paths, never inside a modal loop |
| `editModal` never raised | §10.10 | `ModalScope` and its Phase 1 users |
| UIA range past the end after a swap | §5.6 | every range method clamps first |
| Watcher outliving `StopWatcher` runs on forever | §10.7 | a stop event per watcher |
| A tick during a big document's measuring: 4× slower | §5.4 | reading mode splits many unknown heights over the threads |
| Exe size over budget | §15.1 item 8 | deferred: the preview DLL compiles the map out (`FASTMD_PREVIEW_DLL`); moving edit mode into a delay-loaded DLL is a decision for before 2a |
