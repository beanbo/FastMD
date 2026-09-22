# FastMD 1.1.1

*Release notes as published on 23.09.2026: <https://github.com/beanbo/FastMD/releases/tag/v1.1.1>. Release
commands are at the end of `docs/RELEASE-1.0.0.md` (substitute the version). По-русски — ниже.*

---

## Fixed: a crash when opening a document with a picture

The window showed its first screen, the picture's place stayed empty, the window stopped responding for a couple of
seconds, and then the program closed without a word. Documents without pictures were never affected; how often it
happened depended on the timing of the machine.

The cause was COM. Only the background threads that decode and scale pictures initialised it — the window thread held
no apartment, to keep start-up fast. So when the picture thread finished, its `CoUninitialize` was the last one in the
process and tore COM down for the whole process, while the window thread was still inside `SHAddToRecentDocs`; the
shell call then carried on with state that had just been freed. The window thread now takes a COM apartment right
after the first frame, before any background work starts, and keeps it. Start-up is untouched: the first frame is on
screen before this happens.

With the picture thread's teardown deliberately forced into the shell call, the old build crashed in 6 launches out
of 99 and the new one in none of 99, with the picture drawn and the window responsive every time.

Everything else is as in [1.1.0](https://github.com/beanbo/FastMD/releases/tag/v1.1.0).

## Install

| How | What to do |
|---|---|
| Installer | download `FastMD-Setup.exe` and run it. No admin rights needed |
| Without installing | unpack `FastMD-1.1.1-win-x64.zip` anywhere and run `FastMD.exe` |

Each file has a `.sha256` beside it to check the download. The exe is not signed: SmartScreen warns on the first run
(More info → Run anyway). FastMD 1.0.0 and 1.1.0 find this update themselves — once a day, if checking for updates is
on in the settings — and install it when you agree.

---

## По-русски

**Исправлено: падение при открытии документа с картинкой.** Окно успевало показать первый экран, место картинки
оставалось пустым, окно на пару секунд переставало отвечать, и программа молча закрывалась. Документы без картинок
не падали никогда; как часто это случалось, зависело от того, как быстро работает машина.

Причина — COM. Его инициализировали только фоновые потоки, которые декодируют и масштабируют картинки, а поток окна
апартамента не держал — ради скорости запуска. Поэтому `CoUninitialize` потока картинок оказывался последним в
процессе и сносил COM целиком, пока поток окна был ещё внутри `SHAddToRecentDocs`, — и шелловый вызов продолжал
работать с уже освобождённым. Теперь поток окна берёт апартамент сразу после первого кадра, до запуска фоновой
работы, и больше его не отпускает. Запуск это не замедляет: первый кадр к тому моменту уже на экране.

Если нарочно загнать `CoUninitialize` потока картинок внутрь шеллового вызова, старая сборка падала в 6 запусках из
99, новая — ни в одном из 99, и каждый раз картинка нарисована, а окно отвечает.

Всё остальное — как в [1.1.0](https://github.com/beanbo/FastMD/releases/tag/v1.1.0).

| Способ | Что делать |
|---|---|
| Установщик | скачайте `FastMD-Setup.exe` и запустите. Права администратора не нужны |
| Без установки | распакуйте `FastMD-1.1.1-win-x64.zip` куда угодно и запустите `FastMD.exe` |

Рядом с каждым файлом лежит `.sha256`, чтобы проверить загруженное. Exe не подписан: SmartScreen предупредит при
первом запуске («Подробнее» → «Выполнить в любом случае»). FastMD 1.0.0 и 1.1.0 сами найдут это обновление — раз в
сутки, если проверка обновлений включена в настройках, — и поставят его, если согласиться.
