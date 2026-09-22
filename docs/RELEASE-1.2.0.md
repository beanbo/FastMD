# FastMD 1.2.0

*Release notes as published on 23.09.2026: <https://github.com/beanbo/FastMD/releases/tag/v1.2.0>. Release
commands are at the end of `docs/RELEASE-1.0.0.md` (substitute the version). По-русски — ниже.*

---

## Task list boxes can be ticked

Until now a task box (`- [ ]`) was only a picture: clicking it did nothing. Now a click ticks it in the file itself
(`- [x]`), and a second click takes the tick back out, the way GitHub, Obsidian and Typora do it.

A reader must never damage what it shows, so the write is as small and as careful as it can be:

- exactly one character changes, the one between the brackets. It is overwritten in place, in the file's own encoding
  (UTF-8 with or without a BOM, UTF-16, the ANSI code page), so the line ends, the BOM, the rest of the text and the
  file itself stay as they were;
- FastMD writes only while the file on disk is, byte for byte, the document on screen. If another program has changed
  the file in the meantime, the document is reloaded and nothing is written;
- a read-only file, or one another program keeps open for writing, is left alone, and a note at the bottom of the
  window says why;
- the box works like a button: the pointer turns into a hand over it, an empty box lights up, and a press let go
  outside the box changes nothing.

This is the one time FastMD writes to a document. The Explorer preview pane still only shows it. Start-up speed is
unchanged.

Everything else is as in [1.1.1](https://github.com/beanbo/FastMD/releases/tag/v1.1.1).

## Install

| How | What to do |
|---|---|
| Installer | download `FastMD-Setup.exe` and run it. No admin rights needed |
| Without installing | unpack `FastMD-1.2.0-win-x64.zip` anywhere and run `FastMD.exe` |

Each file has a `.sha256` beside it to check the download. The exe is not signed: SmartScreen warns on the first run
(More info → Run anyway). FastMD 1.0.0, 1.1.0 and 1.1.1 find this update themselves — once a day, if checking for
updates is on in the settings — and install it when you agree.

---

## По-русски

**Флажки в списках задач нажимаются.** До сих пор квадратик `- [ ]` был просто рисунком: клик по нему ничего не
делал. Теперь клик ставит отметку прямо в файле (`- [x]`), повторный — снимает её, как на GitHub, в Obsidian и
Typora.

Просмотрщик не должен портить то, что показывает, поэтому запись — самая маленькая и осторожная из возможных:

- меняется ровно один символ — тот, что между скобками. Он переписывается на месте и в кодировке самого файла
  (UTF-8 с BOM и без, UTF-16, ANSI), поэтому переводы строк, BOM, остальной текст и сам файл остаются прежними;
- FastMD пишет, только если файл на диске байт в байт совпадает с документом на экране. Если другая программа
  успела его изменить, документ перечитывается, а отметка не ставится;
- файл только для чтения или занятый другой программой не трогается, и внизу окна появляется строка с причиной;
- флажок ведёт себя как кнопка: курсор над ним — рука, пустой квадратик подсвечивается, а нажатие, отпущенное вне
  флажка, ничего не меняет.

Это единственный случай, когда FastMD пишет в документ. Панель просмотра в Проводнике по-прежнему только
показывает. Скорость запуска не изменилась.

Всё остальное — как в [1.1.1](https://github.com/beanbo/FastMD/releases/tag/v1.1.1).

| Способ | Что делать |
|---|---|
| Установщик | скачайте `FastMD-Setup.exe` и запустите. Права администратора не нужны |
| Без установки | распакуйте `FastMD-1.2.0-win-x64.zip` куда угодно и запустите `FastMD.exe` |

Рядом с каждым файлом лежит `.sha256`, чтобы проверить загруженное. Exe не подписан: SmartScreen предупредит при
первом запуске («Подробнее» → «Выполнить в любом случае»). FastMD 1.0.0, 1.1.0 и 1.1.1 сами найдут это обновление —
раз в сутки, если проверка обновлений включена в настройках, — и поставят его, если согласиться.
