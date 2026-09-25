# FastMD 1.3.0

*Release notes as published on 25.09.2026: <https://github.com/beanbo/FastMD/releases/tag/v1.3.0>. Release commands are
at the end of `docs/RELEASE-1.0.0.md` (substitute the version). По-русски — ниже.*

---

## Edit mode

FastMD can now edit a document without leaving the rendered view — no switch to a plain-text editor. The page keeps
its look; the caret moves over the text you see, and the Markdown underneath is changed for you.

- **In and out.** Double-click on text: the caret lands where you clicked. F2, the pencil button next to the gear,
  or «Edit here» in the context menu do the same. Esc or the ✕ at the right end of the toolbar leave edit mode.
- **The toolbar** slides down from the top: undo and redo; the paragraph style (text, headings 1–6); bold, italic,
  strikethrough, inline code, link; bulleted, numbered and task lists, quote; code block, table, formula, diagram,
  picture, horizontal rule. In a narrow window the rest goes under "…". A button that does not apply where the caret
  stands is greyed, and its tooltip says why.
- **Tables:** pick the size on a grid. With the caret in a table the same button inserts and deletes rows and
  columns, aligns a column or deletes the table. Tab moves to the next cell, Enter to the row below.
- **Formulas, diagrams, pictures, HTML blocks and front matter:** a click opens their source in a small panel under
  them (input methods work there), and the document redraws the result as you type. Ctrl+Enter keeps the change, Esc
  takes it back. Diagrams start from nine Mermaid templates (flowchart, sequence, class, state, ER, Gantt, pie, mind
  map, timeline).
- **Links:** Ctrl+K. With the caret in a link a small bubble shows its address, with Edit, Remove and Open.
  Ctrl+click opens a link while editing.
- **Pictures:** from a file dialog, by dropping image files on the page, or by pasting files copied in Explorer. The
  path is written relative to the document.
- **Keys:** Ctrl+B, Ctrl+I, Ctrl+Shift+X (strikethrough), Ctrl+` (inline code), Ctrl+K, Ctrl+1…6 (headings; the same
  key again returns to text), Ctrl+Shift+7/8/9 (numbered, bulleted, task list), Ctrl+Shift+Q (quote), Ctrl+Shift+K
  (code block), Ctrl+T (table), Ctrl+M / Ctrl+Shift+M (formula), Ctrl+Enter (a new paragraph after this block),
  Ctrl+Z / Ctrl+Y, Ctrl+S. Markdown typed at the start of a line still works: `# `, `- `, `1. `, `> `, ```` ``` ````.

**Saving.** Edits are saved to the file by themselves, a moment after you stop typing. Settings → «Автосохранение
правок» turns that off; then Ctrl+S or leaving edit mode saves. The file stays yours outside what you changed:

- its encoding (UTF-8 with or without a BOM, UTF-16, the ANSI code page), BOM and line ends are kept, and the file is
  rewritten in place from the first changed byte on, so links to the file and its permissions stay as they were;
- a file another program changed while you were editing is never overwritten silently: a strip under the toolbar
  offers to load the disk version or to overwrite it with yours;
- a read-only, busy or deleted file is reported in that strip, and you can save a copy elsewhere;
- a save cut short by a crash or power loss leaves a recovery copy, and edits that could not be saved yet are kept in
  a journal — FastMD offers both the next time the file is opened. They live in `%LOCALAPPDATA%\FastMD`;
- undo covers the whole session, across saves, and a second FastMD window cannot edit the same file at the same time.

Limits of this first version: input methods for Chinese, Japanese and Korean do not work in the page itself (they do
in the source panels); the Explorer preview pane stays read-only.

## Updates are offered where you see them

Up to 1.2.0 a new version was announced by a four-second note and a context-menu item of that one window, and
forgotten when the window closed; a check that failed waited a whole day. Now:

- a version once found is remembered: a dot on the gear button, and in Settings a new «Обновление» row with one button
  — check now, then update to the version found, then restart;
- after the installer finishes, FastMD offers to restart on the same document, at the same place;
- a check that failed is tried again an hour later, and a window left open for days checks again once a day;
- opening or reloading a document no longer waits for an update download.

## Also fixed

- **Copy as Markdown** dropped the first character of a document's first line, slipped after an emoji shortcode or
  an entity on the same line, and copied a stray line end in files with Windows line ends.
- A formula that appears twice in a document was drawn stretched the second time; both copies now have their real
  size.
- A display formula or a diagram that cannot be drawn shows its source instead of an empty box.
- Reloading a document waited for pictures and updates still downloading (4.2 s in a test); now it takes a few
  milliseconds.
- A rare crash when exporting to PDF, present in 1.2.0 too (2 exports in 30 under load): the page layouts were
  released only after the print job had been closed; now they go while it is still open.
- A picture redrawn on disk now updates in an open document even when the .md itself was rewritten unchanged.
- The settings window opens inside the screen's work area instead of partly beyond its edge.

## Numbers

- **Start-up is unchanged.** Start-up guard from explorer's context, 12 interleaved runs per document against the
  untuned empty Win32 window, medians: medium 69.5 ms against 98.5, large (3.7 MB) 71.4 against 101.8, small 70.1
  against 100.2. The machine was busier than on the day of 1.2.0 (the empty window took 76 ms then); interleaved A/B
  runs of every stage against the one before it showed no change in the first frame or in scrolling. The source map
  edit mode needs is built only when you enter it.
- **Typing costs about a millisecond:** re-parsing and swapping in the document after a keystroke in a 53 KB
  document takes 0.97 ms (median; 1.2 ms p95); from the key to the new frame on screen 5.8 ms.
- **The exe** grew from 0.99 to 1.45 MB: about half a millisecond per start at the measured cost of exe size.
- **Tests:** 516 UI checks; 1.44 million checks of the window-free edit core on reference cases (Markdown before, a
  key or command, Markdown after — each also with Windows line ends, inside a quote and inside a list item) and on
  every caret position of the test documents; 200,000 fuzzed documents under AddressSanitizer, each edited by 50
  random operations and undone back to the original bytes. Before release the feature was reviewed from five sides
  (data safety, Markdown, usability, architecture, regressions): 67 findings, all resolved.

Everything else is as in [1.2.0](https://github.com/beanbo/FastMD/releases/tag/v1.2.0).

## Install

| How | What to do |
|---|---|
| Installer | download `FastMD-Setup.exe` and run it. No admin rights needed |
| Without installing | unpack `FastMD-1.3.0-win-x64.zip` anywhere and run `FastMD.exe` |

Each file has a `.sha256` beside it to check the download. The exe is not signed: SmartScreen warns on the first run
(More info → Run anyway). FastMD 1.0.0–1.2.0 find this update themselves once a day, if checking for updates is on in
the settings: a short note at the bottom of the window and «Update FastMD…» in the context menu of that window. If
you missed it, download the installer here — from 1.3.0 on, a found update stays on the gear and in the settings.

---

## По-русски

**Режим редактирования.** FastMD теперь правит документ, не уходя из отрисованного вида — никакого переключения на
голый текст. Страница выглядит как прежде, курсор ходит по видимому тексту, а Markdown под ним меняется сам.

- **Вход и выход.** Двойной щелчок по тексту ставит курсор туда, куда щёлкнули. То же — F2, кнопка-карандаш рядом с
  шестерёнкой или «Редактировать здесь» в контекстном меню. Выйти — Esc или ✕ справа на панели.
- **Панель инструментов** выезжает сверху: отменить и повторить; стиль абзаца (текст, заголовки 1–6); полужирный,
  курсив, зачёркнутый, код в строке, ссылка; маркированный, нумерованный список и список задач, цитата; блок кода,
  таблица, формула, диаграмма, картинка, горизонтальная линия. В узком окне остальное уходит под «…». Кнопка, которая
  неприменима там, где стоит курсор, серая, и подсказка говорит почему.
- **Таблицы:** размер выбирается на сетке. Когда курсор в таблице, та же кнопка вставляет и удаляет строки и
  столбцы, выравнивает столбец или удаляет таблицу. Tab — к следующей ячейке, Enter — на строку ниже.
- **Формулы, диаграммы, картинки, HTML-блоки и свойства документа (front matter):** щелчок открывает их исходник в
  панели под ними (там работают и методы ввода), а документ перерисовывает результат по мере набора. Ctrl+Enter
  оставляет правку, Esc отменяет. Для диаграмм — девять шаблонов Mermaid (блок-схема, последовательность, классы,
  состояния, ER, Гант, круговая, интеллект-карта, хронология).
- **Ссылки:** Ctrl+K. Когда курсор в ссылке, под ней появляется пузырёк с адресом и кнопками «Изменить», «Убрать»,
  «Открыть». Ctrl+щелчок открывает ссылку во время правки.
- **Картинки:** через диалог выбора файла, перетаскиванием файлов на страницу или вставкой файлов, скопированных в
  Проводнике. Путь пишется относительно документа.
- **Клавиши:** Ctrl+B, Ctrl+I, Ctrl+Shift+X (зачёркнутый), Ctrl+Ё (код в строке), Ctrl+K, Ctrl+1…6 (заголовки;
  повторное нажатие возвращает текст), Ctrl+Shift+7/8/9 (нумерованный, маркированный список, задачи), Ctrl+Shift+Q
  (цитата), Ctrl+Shift+K (блок кода), Ctrl+T (таблица), Ctrl+M / Ctrl+Shift+M (формула), Ctrl+Enter (новый абзац после
  блока), Ctrl+Z / Ctrl+Y, Ctrl+S. Markdown, набранный в начале строки, тоже работает: `# `, `- `, `1. `, `> `.

**Сохранение.** Правки сохраняются в файл сами, через мгновение после того, как вы перестали печатать. Настройки →
«Автосохранение правок» это выключает — тогда сохраняют Ctrl+S и выход из режима правки. Вне ваших правок файл
остаётся прежним:

- кодировка (UTF-8 с BOM и без, UTF-16, ANSI), BOM и переводы строк сохраняются, а файл переписывается на месте,
  начиная с первого изменённого байта, поэтому ссылки на файл и права доступа остаются прежними;
- файл, который другая программа изменила, пока вы правили, молча не перезаписывается: полоса под панелью предлагает
  загрузить версию с диска или перезаписать её своей;
- о файле только для чтения, занятом или удалённом говорит та же полоса, и копию можно сохранить в другое место;
- если сохранение оборвал сбой или отключение питания, остаётся копия для восстановления, а правки, которые пока не
  удалось сохранить, лежат в журнале — FastMD предложит и то и другое при следующем открытии файла. Всё это хранится
  в `%LOCALAPPDATA%\FastMD`;
- отмена (Ctrl+Z) работает на всю сессию, в том числе после сохранений, а второе окно FastMD не может править тот же
  файл одновременно.

Ограничения первой версии: методы ввода для китайского, японского и корейского не работают прямо на странице (в
панелях исходника — работают); панель просмотра в Проводнике по-прежнему только показывает.

**Обновления видно.** До 1.2.0 новая версия показывалась четырёхсекундной надписью и пунктом контекстного меню одного
окна и забывалась, как только окно закрывалось, а неудачная проверка ждала целые сутки. Теперь найденная версия
запоминается: точка на шестерёнке и в настройках новая строка «Обновление» с одной кнопкой — проверить сейчас, затем
обновить до найденной версии, затем перезапустить. После установки FastMD предлагает перезапуститься с тем же
документом на том же месте. Неудачная проверка повторяется через час, а окно, открытое сутками, проверяет раз в
сутки. Открытие и перезагрузка документа больше не ждут загрузки обновления.

**Исправлено попутно.** «Копировать как Markdown» терял первый символ первой строки документа, съезжал после
эмодзи-кода или HTML-сущности в той же строке и прихватывал лишний перевод строки в файлах с переводами строк Windows.
Формула, встреченная в документе второй раз, рисовалась растянутой — теперь у обеих копий настоящий размер. Формула
отдельной строкой или диаграмма, которую не удалось нарисовать, показывает свой исходник, а не пустой прямоугольник.
Перезагрузка документа ждала догружающиеся картинки и обновление (4,2 с в тесте) — теперь это миллисекунды.
Редкое падение при экспорте в PDF (было и в 1.2.0: 2 экспорта из 30 под нагрузкой) исправлено: вёрстка страниц
освобождалась уже после того, как задание печати закрыто, а теперь — пока оно ещё открыто. Картинка, перерисованная на диске, обновляется в открытом документе, даже если сам .md
перезаписали без изменений. Окно настроек открывается в пределах рабочей области экрана.

**Цифры.** Скорость запуска не изменилась: замер из контекста Проводника, по 12 чередующихся запусков на документ
против пустого Win32-окна, медианы — medium 69,5 мс против 98,5, large (3,7 МБ) 71,4 против 101,8, small 70,1 против
100,2 (машина была загружена сильнее, чем в день 1.2.0: пустое окно тогда открывалось за 76 мс; чередующиеся A/B-прогоны
каждого этапа против предыдущего разницы в первом кадре и прокрутке не показали). Карта «текст ↔ исходник» строится
только при входе в режим правки. Набор символа стоит около миллисекунды: разбор и подмена документа после нажатия в
документе на 53 КБ — 0,97 мс (медиана), от клавиши до нового кадра на экране — 5,8 мс. Exe вырос с 0,99 до 1,45 МБ —
около половины миллисекунды на запуск. Тесты: 516 проверок UI-теста; 1,44 млн проверок ядра правки на эталонных случаях
(Markdown до, клавиша или команда, Markdown после — каждый ещё с переводами строк Windows, в цитате и в пункте
списка) и во всех позициях курсора тестовых документов; 200 000 документов фаззинга под AddressSanitizer, каждый — 50
случайных правок и откат до исходных байтов. Перед выпуском функцию проверили с пяти сторон (сохранность данных,
Markdown, удобство, архитектура, регрессии): 67 замечаний, все разобраны.

Всё остальное — как в [1.2.0](https://github.com/beanbo/FastMD/releases/tag/v1.2.0).

| Способ | Что делать |
|---|---|
| Установщик | скачайте `FastMD-Setup.exe` и запустите. Права администратора не нужны |
| Без установки | распакуйте `FastMD-1.3.0-win-x64.zip` куда угодно и запустите `FastMD.exe` |

Рядом с каждым файлом лежит `.sha256`, чтобы проверить загруженное. Exe не подписан: SmartScreen предупредит при
первом запуске («Подробнее» → «Выполнить в любом случае»). FastMD 1.0.0–1.2.0 сами найдут это обновление раз в сутки,
если проверка обновлений включена в настройках: короткая надпись внизу окна и «Обновить FastMD…» в контекстном меню
этого окна. Если пропустили — скачайте установщик здесь; начиная с 1.3.0 найденное обновление остаётся на шестерёнке
и в настройках.
