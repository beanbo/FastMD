# Внешние эталоны (baselines-external)

Сторонние программы, которыми пользователь сейчас открывает `.md`, измеренные тем же харнессом в режиме
`external-title`: `t_content` = момент, когда харнесс (опрос ~1 мс) увидел **видимое окно верхнего уровня, в
заголовке которого есть имя файла** (окна прототипов FastMD исключаются по `FastMD (`). Протокол изнутри эти
программы не выполняют, поэтому это «время до окна с именем файла», а не строгое «документ на экране».
После замера харнесс шлёт окну `WM_CLOSE`.

## Варианты

| id | Программа | Версия | Что показывает |
|---|---|---|---|
| `ext-notepad` | `C:\Windows\System32\notepad.exe` → Store-приложение Notepad | 11.2607.14.0 | сырой текст, режим «Синтаксис Markdown» |
| `ext-vscode` | VS Code (`%LOCALAPPDATA%\Programs\Microsoft VS Code\Code.exe`) | 1.113.0, подписан Microsoft | текстовый редактор с подсветкой Markdown (не preview) |
| `ext-cursor` | Cursor (`%LOCALAPPDATA%\Programs\cursor\Cursor.exe`) | 2.5.26, подписан Anysphere | экран Sign Up / Log In поверх workbench (см. ниже) |
| `ext-void` | Void (`C:\Program Files\Void\Void.exe`) — текущая ассоциация `.md` у пользователя | 1.99.30039, не подписан | экран «Welcome to Void» поверх workbench (см. ниже) |

VS Code, Cursor и Void запускаются с **изолированным профилем**: `--new-window --user-data-dir
{proto_dir}/profiles/<name>/data --extensions-dir {proto_dir}/profiles/<name>/ext --disable-workspace-trust`.
Другой `--user-data-dir` = другой экземпляр (свой IPC-канал), поэтому уже запущенные у пользователя VS Code /
Cursor / Void не затрагиваются, их настройки и расширения не читаются.

## Сборка

```powershell
pwsh -File bench/protos/baselines-external/build.ps1            # проверить exe, пересоздать пустые профили
pwsh -File bench/protos/baselines-external/build.ps1 -KeepProfiles
```

Компилировать нечего. Скрипт печатает found/MISSING для каждого exe и пересоздаёт `profiles/` (первый запуск
после этого — «первый запуск программы»).

## Результаты

`bench.py run ext-vscode,ext-cursor,ext-void --doc small,medium,large --runs 5` и
`bench.py run ext-notepad --doc medium --runs 5` (5 замеров + 1 прогрев; машина не простаивала). Файлы:
`results/quick-vscode-family.json`, `results/quick-notepad.json`.

| вариант | doc | медиана, мс | min | из них CreateProcessW*, мс | CPU, мс | commit, МБ | процессов |
|---|---|---|---|---|---|---|---|
| ext-notepad | medium | **512** | 492 | 28 (стаб System32) | 672 | 92 | 2 |
| ext-vscode | small | **1437** | 1431 | 260 | 2203 | 334 | 5 |
| ext-vscode | medium | **1517** | 1483 | 263 | 2141 | 334 | 5 |
| ext-vscode | large | **1471** | 1386 | 253 | 2063 | 334 | 5 |
| ext-void | small | 1888 | 1753 | 444 | 2547 | 353 | 4 |
| ext-void | medium | 1759 | 1611 | 431 | 2078 | 353 | 4 |
| ext-void | large | 1785 | 1693 | 439 | 2219 | 351 | 4 |
| ext-cursor | small | 3759 | 3558 | 278 | 4672 | 638 | 8 |
| ext-cursor | medium | 4132 | 3865 | 277 | 5188 | 636 | 8 |
| ext-cursor | large | 3878 | 3612 | 268 | 4969 | 638 | 8 |

\* `launch_overhead_ms` харнесса. Отдельный замер голого `CreateProcessW` (`../electron/tools_createproc.py`):
Code.exe 252 мс, Cursor.exe 271 мс, Void.exe 436 мс — это скан Defender больших exe (Void не подписан — дороже).

**Первый запуск на свежем профиле** (сразу после `build.ps1`): VS Code 2555 мс, Void 10 642 мс, Cursor 23 500 мс;
второй — 1687 / 2445 / 5853 мс. Notepad при первом запуске в сессии — 1270 мс (скриншот-прогон — 875 мс).

Для сравнения (прототипы, те же условия): `electron` 880–930 мс, `baseline-win32` 70–90 мс.

## Скриншоты (`results/shots/`)

* `ext-notepad-medium.png` — сырой Markdown моноширинным шрифтом, тёмная тема. **Вкладки пользовательской сессии
  Notepad размыты** (`tools_mask_tabs.py`): Notepad восстанавливает при каждом запуске все вкладки прошлой сессии
  пользователя.
* `ext-vscode-medium.png` — редактор с подсветкой синтаксиса Markdown, тёмная тема, миникарта.
* `ext-cursor-medium.png` — только экран Sign Up / Log In: текст документа **не виден**.
* `ext-void-medium.png` — только онбординг «Welcome to Void»: текст документа **не виден**.

## Оговорки (важно при сравнении)

1. **Cursor и Void на пустом профиле показывают онбординг/логин поверх workbench.** Окно с именем файла в
   заголовке появляется, но сам текст не отрисован; цифры — «время до workbench-окна с файлом». Пропустить
   онбординг флагами нельзя: у Void флаг `isOnboardingComplete` хранится в зашифрованном (safeStorage/DPAPI)
   состоянии `void.settingsServiceStorageII`, у Cursor нужен логин. Копировать реальный профиль пользователя
   (токены, настройки) в проект — неприемлемо. У пользователя с настоящим профилем и расширениями время почти
   наверняка **больше** (активация расширений).
2. **VS Code-семейство открывает текстовый редактор, а не Markdown preview** (у CLI нет ключа «открыть preview»).
   Рендер preview добавил бы ещё запуск webview.
3. **Заголовок ≠ пиксели.** VS Code выставляет заголовок `medium.md - Visual Studio Code`, когда открыт редактор;
   текст рисуется в пределах нескольких кадров — погрешность порядка десятков мс.
4. **Notepad — чужое состояние.** Время включает восстановление всех вкладок сессии пользователя (у него их ~20), так
   что цифра зависит от его сессии. Замерялся только `medium`: каждый новый открытый файл остаётся вкладкой в
   сессии пользователя.
5. Шумно: параллельно работали другие агенты.

## Безопасность прогонов

* Перед каждым запуском Notepad проверялось `Get-Process Notepad` — Notepad у пользователя открыт не был
  (харнесс закрывает окно, в котором найден файл, через `WM_CLOSE`; при открытом Notepad пользователя файл
  открылся бы вкладкой в его окне, и `WM_CLOSE` закрыл бы его окно). **Правило: если Notepad открыт — пропускать
  `ext-notepad`.**
* **Побочный эффект:** в сессию Notepad пользователя добавилась вкладка `medium.md` (Notepad сохраняет сессию при
  закрытии и восстанавливает при следующем запуске). Её можно просто закрыть.
* Процессы VS Code / Cursor / Void после замеров не остаются (Job Object харнесса).

## Файлы

| Путь | Назначение |
|---|---|
| `proto.json` | 4 варианта `external-title` |
| `build.ps1` | проверка exe + пересоздание изолированных профилей |
| `profiles/<vscode,cursor,void>/` | изолированные `--user-data-dir` / `--extensions-dir` |
| `tools_mask_tabs.py` | размытие вкладок сессии пользователя на скриншоте Notepad |
| `results/` | прогоны и скриншоты |
