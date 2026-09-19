# 05 — Инженерия старта процесса в Windows: куда уходят миллисекунды и как их легально срезать

> Исследование для FastMD (быстрый ридер .md). Дата: 2026-09-19. Машина: Windows 11 Pro 25H2 (26200.8037), Ryzen 9 7950X, 64 ГБ, NVMe, NVIDIA RTX 5070 (драйвер 32.0.16.1656, монитор 3440×1440@165 Гц, 96 DPI) + AMD iGPU (без монитора), Defender RTP включён, сессия без админа.
> Всё, что помечено **[замер]**, измерено мной на этой машине лабораторными программами из `research/lab/startup/` (исходники, сырые JSONL и скрипты воспроизведения там же). Остальное — источники со ссылками.
> **Оговорка о шуме:** основная серия шла, пока 11 других агентов собирали прототипы (загрузка CPU 80–92 %); в таблицах median (min–p90), и min — лучшая оценка «собственной» стоимости. Часть групп повторена на «тихой» машине (загрузка 13–20 %) — они помечены **[тихо]**. Финальная серийная прогонка оркестратора на простаивающей машине — авторитетна; мои числа для относительных выводов.

---

## 0. TL;DR

**Самое важное, что показали эксперименты (все — [замер]):**

| # | Находка | Цена на этой машине | Что делать FastMD |
|---|---|---|---|
| 1 | `D3D11CreateDevice` на NVIDIA (UMD ≈195 МБ DLL: `nvwgf2umx.dll` 87 МБ + `nvgpucomp64.dll` 107 МБ) | **≈205 мс [тихо]** (до 280 под нагрузкой) на процесс + первый `Present` ≈18 мс; флаги не помогают. Первый кадр D2D: NVIDIA 314 мс vs WARP 100 мс [тихо] | Первый кадр — без аппаратного D3D: WARP (≈15 мс), DirectWrite→DIB→GDI или GDI. HW-устройство — лениво/в фоне или никогда |
| 2 | Defender синхронно «обрабатывает» **каждое** создание процесса в ядре, в потоке-родителе; стоимость ∝ размеру EXE | **≈1,2–1,9 мс/МБ на каждый запуск** [тихо] (150 МБ → ≈300 мс, Electron 235 МБ → ≈465 мс, VS Code 193 МБ → ≈245 мс); DLL любого размера — ≈1 мс | EXE маленький (сотни КБ – единицы МБ), тяжёлое — в DLL. Не single-file .NET (64 МБ → +130 мс) |
| 3 | Первое окно: `ShowWindow` активирует окно → инициализация TSF (TextInputFramework, CoreMessaging, CoreUIComponents) внутри `WM_ACTIVATE` | **≈12–15 мс [тихо]**, 25–35 мс под нагрузкой (+ редкие зависания до 2 с) | `ImmDisableIME(-1)` до создания окна или показ `SW_SHOWNOACTIVATE` → активация после первого кадра |
| 4 | Анимация открытия окна DWM Windows 11 | окно «полностью видно» на **40–60 мс позже** t_content | `DWMWA_TRANSITIONS_FORCEDISABLED` = TRUE |
| 5 | Цветные эмодзи (Segoe UI Emoji — COLRv1) в Direct2D | 5 разных эмодзи (как первый экран medium.md): **+22 мс (WARP) / +35 мс (NVIDIA) [тихо]**, до 90 мс под нагрузкой; повторный кадр ~1–3 мс | viewport-first; свой кэш битмапов эмодзи; при нужде — монохром в первом кадре |
| 6 | Передача «открой файл» **уже запущенному** процессу через `DelegateExecute`/`IExecuteCommand` (COM local server) | Explorer→окно показано: **32–57 мс** против **98 мс** у классического нового процесса (тривиальный GDI-вьюер) | Опциональный резидентный режим (выигрыш растёт с тяжестью инициализации движка) |
| 7 | Новый (только что собранный/установленный) EXE | +20–40 мс (4 КБ) … +150 мс (150 МБ) до создания объекта процесса при **первом** запуске | Учитывать «первый запуск после установки»; не пересобирать перед замерами |
| 8 | COM/WinRT-апартамент: `CoInitializeEx(STA)` / `RoInitialize` | 10–20 мс каждый | Не звать на критическом пути, если движок не требует |

**Пол (floor) процесса на этой машине [замер, тихо]:** объект процесса создаётся через ≈0,7 мс после `CreateProcessW`, но сам вызов возвращается через ≈21 мс (синхронная обработка Defender для «неизвестного» EXE; у известных мелких EXE — 4–8 мс), первая строка `main` пустого EXE — через ≈31 мс (min 28). «Win32+GDI окно с текстом» — ≈77 мс, из них ≈12–15 мс TSF; с `ImmDisableIME`+показом без активации — ≈58 мс; анимация DWM добавляет ещё 40–60 мс «до полностью видимого окна» (t_content её не видит).

**Рекомендация по архитектуре (детали — §12):** маленький нативный EXE (C/C++ или Rust, статический CRT), DirectWrite-вёрстка только первого экрана, первый кадр через WARP-D2D или DWrite→DIB, `ImmDisableIME`/no-activate-first, отключённая анимация DWM, никакого D3D-HW/COM/WinRT на критическом пути; опционально — резидентный режим с `DelegateExecute` (без нового процесса вообще).

---

## 1. Методика и инструменты

### 1.1 Лаборатория `research/lab/startup/`

| Файл | Назначение |
|---|---|
| `src/labmark.h` | метки ребёнка в именованную разделяемую память `Local\FastMDLabShm` (только kernel32, работает и в `/NODEFAULTLIB`), событие «готово» |
| `src/launch.cpp` → `bin/launch.exe` | лаунчер: `t0 = GetSystemTimePreciseAsFileTime()` непосредственно перед запуском; режимы `cp` (CreateProcessW), `cpjob` (как `bench.py`: SUSPENDED+Job+Resume), `shell` (ShellExecuteExW), `explorer` (ShellExecute **внутри explorer.exe** через `IShellWindows→IShellDispatch2`, т.е. путь двойного клика), `susp` (CreateProcessW(SUSPENDED)+TerminateProcess: ребёнок не исполняет ни одной инструкции). Опции `--msonly` (политика `BLOCK_NON_MICROSOFT_BINARIES` — отсекает инъекцию Windhawk), `--self-msonly`, `--fresh` (копия EXE с новым хэшем), `--console`. Пишет: время возврата вызова, время создания объекта процесса (`GetProcessTimes`), метки ребёнка, CPU ребёнка (`QueryProcessCycleTime`), CPU родителя внутри `CreateProcessW` (циклы + kernel/user) |
| `src/trivial.c` | пустые EXE: GUI/console, `/MT`, `/MD`, без CRT, с/без манифеста, с импортом user32, с балластом 5/50/150 МБ (случайные данные и нули в RCDATA) |
| `src/probe.cpp` → `bin/probe.exe` | «конструктор» шагов: статически импортирует **только kernel32**, всё остальное `/DELAYLOAD` → время шага включает загрузку нужных DLL. Шаги: `load:`/`map:` DLL, COM/WinRT, окно (с трассировкой первых сообщений `WndProc`), D3D11 hw/warp/iGPU/dGPU, D2D, DWrite, swap chain hwnd/DComp, отрисовка, Present, DwmFlush, GDI-текст, DWrite→DIB, D2D DC RT, пре-варм эмодзи… |
| `src/dup.cpp` → `bin/dup.exe` | «фотонный» замер через DXGI Desktop Duplication: каждый скомпонованный кадр рабочего стола с QPC-временем показа, сетка 24×24 точек в окне → когда пиксели окна реально появились и когда анимация закончилась |
| `src/srv.cpp`, `src/handoff.c` | вьюер-заглушка в ролях: новый процесс; COM local server `IExecuteCommand` (DelegateExecute); резидент (+предсозданное/«плащ»-окно); крошечный лаунчер-передатчик `WM_COPYDATA` |
| `src/bstest.c` | Windows App SDK 1.8 bootstrapper (`MddBootstrapInitialize2`) |
| `dotnet/` | один и тот же `Program.cs` в 8 вариантах публикации .NET 8 + рантайм .NET 9 |
| `lab.py`, `handoff.py`, `reg.ps1`, `build.ps1` | сборка, чередующиеся раунды, отчёты; регистрация тестовых ассоциаций `.fmdtest*` (HKCU) |

Воспроизведение: `pwsh -File research/lab/startup/build.ps1`, затем `python research/lab/startup/lab.py run <proc|fresh|dll|gfx|win|frame|susp|map|dotnet|dotnet2> [--rounds N] [--tag _x]`, `python research/lab/startup/handoff.py` (нужна регистрация `reg.ps1 install` **через explorer**, см. §11.1). Сырые данные — `results/*.jsonl`, сводки — `results/*.txt`.

### 1.2 Три особенности именно этой машины, влияющие на ВСЕ замеры стенда

1. **Windhawk 1.7.3** внедряется в каждый процесс (движок `windhawk.dll` + мод `explorer-details-better-file-sizes` с `Include=*`, тянущий `libc++.whl`, `SHELL32`, `windows.storage`, `OLEAUT32`, `combase`…). Механизм: перехват `CreateProcessInternalW` в родителе и APC-инъекция в приостановленного ребёнка ([m417z, 2022](https://m417z.com/Implementing-Global-Injection-and-Hooking-in-Windows/), [wiki Windhawk](https://github.com/ramensoftware/windhawk/wiki/Injection-targets-and-critical-system-processes)). **[замер]** +≈5 мс CPU на каждый процесс; до `main` +≈5 мс под нагрузкой и ≈0 в тишине (работа идёт в APC до точки входа, но параллельно с загрузчиком); для процессов, которым shell32/COM не нужны, это чистая добавка. В лаборатории я отсекал его политикой «только Microsoft-подписанные образы» (`--msonly`); обычные пользователи с Windhawk/подобными инструментами будут платить эти мс в любом приложении.
2. **Defender RTP** (движок 1.1.26080.3, `EnableFileHashComputation=False`, SAC выключен, VBS работает без HVCI) — см. §2.2: стоимость создания процесса зависит от размера EXE.
3. **Claude Desktop — MSIX-пакет** (`Claude_2.2553.1.0_x64__pzs8sxrjxfjjc`), и всё, что запускается из этой сессии (включая `bench.py` и все прототипы!), получает **виртуализацию AppData и HKCU** этого пакета — см. §11.1. Это повлияло на эксперимент с COM (§8) и влияет на веб-прототипы.

---

## 2. Анатомия `CreateProcessW` → `main`

### 2.1 Хронология пустого процесса [замер]

`t0` → **≈0,7–1 мс**: объект процесса создан (время из `GetProcessTimes`) → **≈21 мс [тихо]** (18–30 под нагрузкой): `CreateProcessW` вернул управление → **≈31 мс [тихо]** (32–40): первая инструкция `main` у EXE без CRT (ntdll+kernel32+kernelbase) → далее то, что делает само приложение.

Ключевой эксперимент — разделить «до объекта процесса» и «после». Режим `susp` создаёт процесс приостановленным и сразу убивает (ни одной инструкции ребёнка), а лаунчер меряет CPU **собственного потока** внутри вызова:

| EXE (CreateProcessW SUSPENDED, лаунчер вне Windhawk) | вызов, мс | CPU потока-родителя внутри вызова | из них kernel |
|---|---|---|---|
| наш `t_gui_nocrt.exe`, 4 КБ, неподписан, «никому не известен» | 20,7 | 19,3 | 17,3 |
| `t_gui_mt_pad150.exe`, 150 МБ, неподписан | 367 | 365 | 356 |
| `MRT.exe`, 211 МБ, **подписан Microsoft** | 285 | 282 | ≈300 |
| `msedge.exe`, 5 МБ, подписан Microsoft | 4,6 | 3,8 | ≈5 |

→ Почти всё время `CreateProcessW` — это **CPU в режиме ядра в потоке вызывающего**, пропорциональный размеру EXE, т.е. синхронный callback создания процесса, а не ожидание I/O или сервиса.

**Доказательство, что это Defender [замер]:** ETW-провайдеры `Microsoft-Antimalware-Engine`/`-RTP` доступны группе «Пользователи журналов производительности» без админа. На каждое создание процесса движок пишет событие 73 `SyncStart`, и интервал `SyncStart → Termination` (лаунчер убивает процесс сразу после возврата) совпадает с измеренной длительностью `CreateProcessW` (≈330 мс для pad150, ≈280 мс для MRT); далее — Behavior Monitoring `IsKnownFriendly()`; при первом запуске — `USN Cache | MISS` и `NRIOnForProcess`. Выдержка: `results/defender_events.txt`. Провайдер `Microsoft-Windows-CodeIntegrity` без админа недоступен, поэтому ядерный стек (WdFilter vs CI) я не видел — формулировка «Defender-путь создания процесса» это наиболее вероятная, но не на 100 % доказанная атрибуция.

### 2.2 Размер EXE — «налог» на каждый запуск [замер]

`susp`, 10 раундов, лаунчер вне Windhawk; median (min–p90), мс:

| EXE | размер | подпись | под нагрузкой | **[тихо]** |
|---|---|---|---|---|
| python.exe | 0,1 МБ | PSF | 3,8 (3,6–22) | — |
| git-bash.exe | 0,1 МБ | **нет** | 5,0 (4,8–49) | — |
| cmd.exe | — | MS (каталог) | 4,3 (3,6–14) | — |
| msedge.exe | 5 МБ | MS | 4,9–7,3 | 7,8 (6,7–23) |
| dotnet.exe | 0,2 МБ | MS | 6,2 (5,7–10) | — |
| **наш** t_gui_nocrt | 4 КБ | нет, новый | 18,7–23,8 | 22,3 (19,4–25,9) |
| наш pad5 (случайные данные) | 5 МБ | нет | 30,1 (26,8–42,8) | 30,3 (28,8–38,3) |
| наш pad50 | 50 МБ | нет | 119 (113–129) | 113 (111–127) |
| node.exe | 88 МБ | OpenJS | 127 (min 8,2!) | 116 (112–162) |
| WPF single-file (копия прототипа wpf) | 147 МБ | нет | 316 (298–373) | 299 (291–500) |
| наш pad150 **случайные** | 150 МБ | нет | 314 (305–348) | 299 (293–353) |
| наш zero150 **нули** | 150 МБ | нет | 329 (310–376) | 302 (293–424) |
| Code.exe (VS Code) | 193 МБ | MS | 259 (255–270) | 244 (233–311) |
| Cursor.exe | 201 МБ | Anysphere | 270 (269–292) | 252 (248–300) |
| MRT.exe | 211 МБ | MS | 280 (273–295) | 263 (254–293) |
| electron.exe (копия из прототипа electron) | 235 МБ | нет | **481** (458–490) | **464** (451–775) |

(Системный `notepad.exe` не годится как эталон: 25 мс до объекта процесса — в Windows 11 это заглушка, перенаправляющая на Store-версию.)

Выводы:
- **Налог ≈1,85 мс на МБ для неподписанных и ≈1,2 мс/МБ для подписанных Microsoft EXE, на каждый запуск** [тихо]; не зависит от энтропии (случайные ≈ нули) и не снимается подписью (Code.exe, MRT.exe). Исключения — «известные» мелкие файлы (msedge.exe, python.exe, git-bash.exe: 4–6 мс), у node.exe бимодально (иногда 8 мс, обычно 127) → есть кэш «дружественных» файлов с ограничениями, в который наш свежий 4 КБ EXE не попадает (18–24 мс, 15–17 мс ядра).
- **Для DLL налога нет:** `LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES)` 150 МБ неподписанной DLL — **1,2 мс**, 332 МБ `msedge.dll` WebView2 — <1 мс, `nvgpucomp64.dll` 107 МБ — <1 мс (`results/map.txt`). Именно поэтому Chromium держит `chrome.exe`/`msedge.exe` маленьким, а код — в `chrome.dll`/`msedge.dll`.
- **Следствия для кандидатов:** Electron платит ≈0,46–0,48 с *за каждое создание* процесса (browser, GPU, renderer — все из одного 235 МБ EXE; время тратится в потоке, который создаёт процесс); VS Code/Cursor ≈0,25 с на процесс; .NET single-file 64 МБ — +≈130 мс (§7); NativeAOT 2–10 МБ — +3–20 мс; нативный C++/Rust 0,2–3 МБ — ≈0.

### 2.3 Первый запуск только что записанного EXE [замер]

`--fresh`: копия EXE с уникальным хвостом (новый хэш), запуск сразу или через 3 с:

| | объект процесса создан, мс | CreateProcessW вернул | main |
|---|---|---|---|
| 4 КБ, сразу / +3 с | 40,7 / 14,6 | 66,8 / 47,4 | 79,6 / 59,7 |
| 5 МБ, сразу / +3 с | 50,7 / 30,1 | 87,5 / 70,3 | 100,8 / 83,5 |
| 50 МБ, сразу / +3 с | 69,6 / 52,0 | 193 / 200 | 209 / 211 |
| 150 МБ, сразу / +3 с | 151 / 116 | 487 / 504 | 501 / 516 |
| для сравнения: тот же 4 КБ, n-й запуск | 0,9 | ≈27 | ≈40 |

Первый запуск нового бинаря платит ещё +20–150 мс сканирования **до** создания объекта процесса (Defender по USN-кэшу видит новый файл), пауза 3 с после записи почти не помогает. Для стенда: `first_ms` в `bench.py` — это «первый запуск после сборки», его нельзя сравнивать с медианой.

### 2.4 Подсистема, CRT, манифест [замер]

`proc`: median мс до `main`; «CPU» — циклы ребёнка. Под нагрузкой — 15 раундов; **[тихо]** — 10 раундов:

| вариант | CreateProcessW (нагр. / тихо) | main (нагр. / тихо) | CPU ребёнка (тихо) |
|---|---|---|---|
| GUI, без CRT, Windhawk внедрён | 32,1 / 21,0 | 47,9 / 31,8 | 15,3 |
| GUI, без CRT (`--msonly`) | 30,2 / 21,9 | 42,8 / 31,7 | 10,5 |
| то же, лаунчер тоже вне Windhawk | 28,5 / 21,6 | 39,1 / 31,2 | 10,3 |
| как `bench.py` (SUSPENDED+Job+Resume) | 27,5 / 20,9 | 39,3 / 30,2 | 10,2 |
| console, наследует консоль | 26,7 / 20,5 | 39,8 / 30,3 | 10,0 |
| console, `DETACHED_PROCESS` | 29,2 / 21,3 | 42,4 / 31,5 | 10,2 |
| **console, `CREATE_NEW_CONSOLE`** | 27,3 / 21,4 | **729 / 503** | 12,9 |
| static CRT `/MT` GUI | 39,1 / 21,7 | 56,0 / 31,7 | 10,9 |
| dynamic CRT `/MD` GUI | 36,9 / 22,4 | 52,5 / 34,0 | 12,5 |
| без манифеста / с полным манифестом (comctl6, PMv2) | 32,2 / 21,9 ; 28,3 / 22,1 | 47,4 / 32,0 ; 42,1 / 31,6 | ≈10 |
| + статический импорт user32 | 27,9 / 21,5 | 47,5 / 33,5 | 13,3 |
| + 5 / 50 / 150 МБ балласта (static CRT) | — / 31,9 ; 113,9 ; 299,5 | — / 42,5 ; 124,6 ; 310,2 | ≈11,5 |
| ShellExecuteEx(путь к EXE) из процесса | 82,0 / 64,0 | 94,8 / 73,0 | 15,2 |

- Консольная подсистема, запущенная без консоли (как из Explorer), в Windows 11 рождает новую консоль через Windows Terminal/`conhost` — **0,5–1,6 с**. FastMD — только `/SUBSYSTEM:WINDOWS`.
- CRT статический vs динамический, манифест — в пределах шума (±2 мс в тишине). Импорт user32 — +≈2 мс и +3 мс CPU (подключение win32k), он всё равно нужен. Windhawk в тишине не сдвигает `main`, но добавляет ≈5 мс CPU на процесс.
- `ShellExecuteEx` на EXE из обычного процесса добавляет ≈40 мс *до создания процесса* (инициализация shell32 в вызывающем процессе + ассоциации) — у Explorer это уже прогрето, см. §8.
- Интервал «CreateProcessW вернул → `main`» ≈9–10 мс и ≈10 мс CPU ребёнка даже для EXE без CRT: инициализация загрузчика, образные callback'и и т.п. — не оптимизируется со стороны приложения.

---

## 3. DLL и инициализация рантаймов [замер]

Процесс, статически импортирующий только kernel32; шаг = `LoadLibrary`/вызов (диапазон median…min, мс, под нагрузкой; **[тихо]** — абзацем ниже):

| шаг | мс |
|---|---|
| user32 (+GUI-поток) | 2,2–3,9 |
| gdi32 | 2,1–3,4 |
| uxtheme / dwmapi / comctl32 v5 / shell32 | 3–5,6 |
| imm32 + msctf | ≈3,5 + 1,5 |
| d3d11 / dxgi / dcomp | 3,3–5,5 |
| d2d1 / dwrite / windowscodecs | 0,7–1,9 |
| **`CoInitializeEx(STA)`** (с ole32) | **12,6–17,4** |
| **`OleInitialize`** | **16,3–20,7** |
| **`RoInitialize`** | **15,3–21,5** |
| + `CreateDispatcherQueueController` | +2,6–5,3 |
| + `RoActivateInstance(Windows.UI.Composition.Compositor)` | +2,9–3,9 |
| `D2D1CreateFactory` | 0,6–1,0 |
| `DWriteCreateFactory(SHARED)` / `GetSystemFontCollection` | 1,4–1,9 / 0,8–0,9 |
| `CreateTextFormat` + `CreateTextLayout` (лат. / смесь кириллица+CJK+эмодзи) | 1,0 + 2,2–2,7 / 4,4–4,8 |

Системные DLL из KnownDLLs/кэша стоят единицы мс; дорогие — не DLL, а **инициализация COM/WinRT-апартамента** (10–20 мс) и то, что за ней (WebView2, WinUI, WIC…). DirectWrite дешёв: shared-фабрика использует системный кэш шрифтов.

**[тихо]** (10 раундов, median/min): user32 2,2/1,8; gdi32 2,1/1,9; uxtheme 3,4/3,2; dwmapi 3,4/2,9; comctl32 3,1/2,7; shell32 3,4/2,9; imm32+msctf 2,2+1,2; d3d11 3,6; dxgi 3,2; dcomp 4,2; d2d1 0,8; dwrite 1,6; WIC 1,5; **`CoInitializeEx(STA)` 11,7/11,1; `OleInitialize` 15,6/14,6; `RoInitialize` 15,2/14,2**; DispatcherQueue +2,5; WinRT Compositor +3,1; «пустой» процесс до `main` 31,0 (min 29,4).

---

## 4. Графика: устройство, первый кадр, эмодзи [замер]

### 4.1 Устройства (без окна)

| шаг | median (min), мс |
|---|---|
| `D3D11CreateDevice` адаптер по умолчанию = **NVIDIA RTX 5070** | **258 (224)** |
| … с `SINGLETHREADED`+`PREVENT_INTERNAL_THREADING_OPTIMIZATIONS` / с FL 10_0 | 222 (205) / 214 (205) |
| … явно dGPU (`EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`) | 270 (246) (+ DXGI-фабрика 18–26) |
| … iGPU AMD (MINIMUM_POWER) | 84 (65) (+ DXGI-фабрика 16–25) |
| **WARP** | **22–29 (15–20)** |

Модули, которые тянет NVIDIA-устройство (`opt:moddiff`): `nvldumdx.dll` → WinTrust/Crypt32/`cryptnet`/`drvstore`/`devobj` (проверка подписи UMD) → `nvwgf2umx.dll` (86,8 МБ) + `nvgpucomp64.dll` (106,7 МБ, компилятор шейдеров) + `nvppex.dll`, `NvMemMapStoragex.dll`, `SHELL32`, `windows.storage`… Это цена **каждого процесса**, который создаёт аппаратное D3D11-устройство на этой машине: WebView2/Chromium (GPU-процесс), WinUI 3, WPF (D3D9), Avalonia/Skia-GL, wgpu/egui, D2D HwndRenderTarget по умолчанию.

**[тихо]** (10 раундов, median/min, мс): NVIDIA по умолчанию **207,5/200,5**, dGPU явно 206,3/199,2 (+DXGI-фабрика 16,4), iGPU 58,5/53,7 (+DXGI-фабрика 15,9), **WARP 18,2/17,0**; `D2D1CreateFactory` 0,9; `DWriteCreateFactory` 1,6 + `GetSystemFontCollection` 0,8 (shared и isolated одинаково); `CreateTextFormat` 0,8 + `CreateTextLayout` латиница 2,1 / смесь 4,0. CPU процесса с NVIDIA-устройством — 230 мс против 30 мс с WARP.

### 4.2 Первый кадр текста целиком (окно + текст + Present + DwmFlush)

`frame`, 10 раундов, 15 px @96 DPI, под нагрузкой; «done» = от `t0` до кадра на экране:

| путь | done, median (min) | узкое место |
|---|---|---|
| GDI `DrawTextW`, латиница | 116 (102) | — |
| GDI `DrawTextW`, смесь кир.+CJK+эмодзи | 186 (160) | font linking GDI 69–88 мс |
| DirectWrite → `IDWriteBitmapRenderTarget` (DIB) → BitBlt, латиница | 130 (96) | весь текст ≈2 мс |
| D2D, **WARP**, flip swap chain, латиница | **134 (106)** | устройство 18, отрисовка 1,7, Present 0,3 |
| D2D, WARP, DirectComposition, латиница | 139 (118) | — |
| D2D `ID2D1DCRenderTarget` (software) | 146 (120) | создание RT 24 мс |
| D2D `ID2D1HwndRenderTarget` (по умолчанию = hardware) | 381 (321) | внутреннее HW-устройство 247 мс |
| D2D, **NVIDIA**, flip | 431 (333) | устройство 278, **первый Present 20** |
| D2D, NVIDIA, DComp | 390 (332) | устройство 248, Present 19 |
| D2D, iGPU AMD, flip | 484 (404) | **swap chain 252 мс** (кросс-адаптер: монитор на NVIDIA) |
| D2D, NVIDIA, два кадра подряд | 411 | второй Present ещё 16 мс |

**[тихо]** (10 раундов; окно без `ImmDisableIME`, т.е. с ≈16 мс `ShowWindow`):

| путь | кадр на экране, median (min) мс | ключевые шаги |
|---|---|---|
| **DirectWrite → DIB → BitBlt**, латиница | **81,8 (76,1)** | фабрика 0,8, формат 0,6, вёрстка 1,0, RT 1,3, рисование 1,9 |
| DirectWrite → DIB, 5 эмодзи (монохром) | 84,1 (74,7) | рисование 1,4 |
| GDI `DrawTextW`, латиница / смесь | 90,3 / 143,8 | текст 10,3 / **67,4** (font linking) |
| D2D DC RT (software) | 97,2 (88,8) | создание RT 15,5, рисование 20,4 |
| **D2D WARP flip** | **100,2 (87,7)** | устройство 14,9, swap chain 2,6, рисование 1,4 |
| D2D WARP DComp | 101,9 (87,0) | swap chain+DComp 3,9 |
| D2D WARP flip, 5 цветных эмодзи / монохром | 121,0 / 97,0 | рисование **22,8** / 1,0 |
| D2D `HwndRenderTarget` (по умолчанию HW) | 287,9 (267,2) | создание RT 207 |
| D2D NVIDIA DComp | 311,9 (301,5) | устройство 209,6, Present 18,8 |
| **D2D NVIDIA flip** | **313,8 (290,9)** | устройство 205,3, **Present 18,6** |
| D2D NVIDIA, 5 цветных эмодзи | 335,8 (328,3) | рисование 37,0 |
| D2D NVIDIA, 2 кадра | 324,6 | второй Present 16,3 |
| D2D iGPU flip | 348,8 (339,1) | устройство 54,2 + фабрика 13,3, **swap chain 194** |

**Вывод:** на машине с дискретной NVIDIA аппаратный путь D2D добавляет к первому кадру **≈+210 мс [тихо]** (+250…+300 под нагрузкой) относительно WARP/CPU-пути, причём CPU процесса вырастает в 4 раза (281 против 70 мс). Для страницы текста WARP (многопоточный программный растеризатор) или DirectWrite→DIB — более чем достаточно; iGPU хуже всех (кросс-адаптерный swap chain ≈190–250 мс, т.к. монитор подключён к NVIDIA). Самый быстрый полноценный путь в тишине — **DirectWrite→DIB→BitBlt: 82 мс до кадра**, вровень с «голым» GDI и лучше него на смешанном тексте (GDI font linking — 67 мс).

### 4.3 Цветные эмодзи — скрытые 20–90 мс [замер]

Segoe UI Emoji (`seguiemj.ttf`, 12,4 МБ) в этой сборке содержит таблицу **COLR версии 1** (plus 3372 записи v0), `IDWriteFactory8::TranslateColorGlyphRun` отдаёт `COLR_PAINT_TREE`. Замеры первого `DrawTextLayout(..., ENABLE_COLOR_FONT)`:

| текст | WARP 1-й / 2-й кадр, мс | NVIDIA 1-й / 2-й, мс |
|---|---|---|
| латиница | 2,3–4,1 / ≈1 | 2,9–7,4 / ≈0,6 |
| 1 эмодзи | 12–17 / ≈1 | 17–29 / ≈1 |
| 5 разных эмодзи (как первый экран medium.md: ✅ ✨ ❌ 📄 🚀) | **26–49 / ≈3** | **59–88 / ≈2,5** |
| 4 разных × 12 = 48 эмодзи | 98–117 / **19–33** | 120–188 / ≈20 |
| тот же текст без `ENABLE_COLOR_FONT` (монохром) | 1,8–3,6 | — |
| CJK + кириллица (без эмодзи) | 2,4–4,8 | — |

- Стоимость — рендеринг paint-дерева COLRv1 в D2D (сам `TranslateColorGlyphRun` — ≈17 мс на 48 прогонов), пре-варм одним эмодзи не помогает (кэш — на глиф+размер). Даже повторный кадр с 48 эмодзи — 20 мс (нет растрового кэша).
- GDI рисует эмодзи монохромно, а смешанный текст через font linking стоит 70–90 мс; `IDWriteBitmapRenderTarget3::DrawGlyphRunWithColorSupport` на этой сборке недоступен (QI → нет), т.е. DWrite→DIB-путь даёт монохромные эмодзи за ≈2 мс.
- **Для FastMD:** рендерить только первый экран; хранить свой атлас растров эмодзи (рисовать каждый уникальный глиф один раз); если первый экран содержит много разных эмодзи — допустим монохром в первом кадре и «подкраска» следующим кадром.

### 4.4 DwmFlush и частота [замер]

Монитор 165 Гц: последовательные `DwmFlush` — 6,0–6,4 мс (период 6,06 мс). Протокольный `DwmFlush()` после Present добавляет 3–9 мс (ожидание ближайшей композиции).

---

## 5. Первое окно: где 40 мс между «окно создано» и «WM_PAINT» [замер]

`win`, 12 раундов под нагрузкой; трассировка первых вхождений сообщений в `WndProc` (median, мс):

| вариант | CreateWindowEx | ShowWindow | из них WM_ACTIVATE (вкл. IME_SETCONTEXT) | done (до DwmFlush) |
|---|---|---|---|---|
| базовый (как `baseline-win32`) | 23,9 | **41,2** | **23,5** (7,9) | 156 (включая 5 доп. `DwmFlush` ≈30 → ≈126) |
| **`ImmDisableIME(-1)`** до окна | 16,8 | **10,8** | 0,4 | **78** |
| `SW_SHOWNOACTIVATE` | 21,2 | 6,7 | — | 80 |
| `WS_POPUP` | 21,8 | 31,5 | 24,4 | 107 |
| без visual styles (`SetThemeAppProperties(0)`) | 19,7 | 38,1 | 22,4 | 119 |
| Mica / тёмный заголовок / свой кадр (Extend+NCCALCSIZE) | 22–24 | 29–44 | 19–27 | 107–134 |
| `DWMWA_TRANSITIONS_FORCEDISABLED` | 25,1 | 37,4 | 20,6 | 127 |
| noime + noactivate + popup | 15,9 | **4,6** | — | **76** |

**[тихо]** (10 раундов, median, мс; «кадр» = до `DwmFlush` после GDI-отрисовки; у базового варианта из «done» вычтены 5 дополнительных `DwmFlush` ≈30 мс):

| вариант | CreateWindowEx | ShowWindow | WM_ACTIVATE (IME_SETCONTEXT) | кадр |
|---|---|---|---|---|
| базовый | 16,5 | **17,4** | 10,9 (3,1) | ≈77 |
| + Windhawk | 14,8 | 21,8 | 13,5 (4,8) | 82 |
| **`ImmDisableIME(-1)`** | 12,6 | **5,7** | 0,2 | **63,6** |
| `SW_SHOWNOACTIVATE` | 15,4 | 3,8 | — | 63,9 |
| `WS_POPUP` | 14,6 | 14,5 | 11,1 | 71,8 |
| без visual styles | 13,7 | 15,3 | 10,1 | 72,0 |
| Mica / тёмный заголовок / свой кадр / без фона | 15,7–16,6 | 16,8–18,2 | 10,7–11,8 | 75–81 |
| **noime + noactivate + popup** | 13,1 | **2,5** | — | **58,2** |

В тишине TSF стоит ≈12–15 мс (под нагрузкой ≈25–35), `DwmFlush` после отрисовки — 5–10 мс, серия `DwmFlush` — 5,2–6,1 мс (165 Гц).

- При первом фокусе потока Windows 11 грузит `textinputframework.dll`, `CoreMessaging.dll`, `CoreUIComponents.dll`, `wintypes`, `OLEAUT32`… (видно по `opt:moddiff`) и общается с `TextInputHost`/`ctfmon`. На этой машине `TextInputHost.exe` набрал 127 736 с CPU за 5 дней — явно неисправен; один `ShowWindow` в опыте занял **2,0 с** (похоже на таймаут межпроцессного вызова TSF). `ImmDisableIME(-1)` ([docs](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immdisableime): вызывать до первого `WM_CREATE` верхнего окна потока) убирает это целиком.
- Это же объясняет калибровку `baseline-win32` (окно видно на ≈55 мс, первый `WM_PAINT` на ≈85): 17–30 мс внутри `ShowWindow` — активация+TSF, когда окно уже `WS_VISIBLE` (наблюдатель стенда его уже видит), плюс `UpdateWindow`.
- Цена `ImmDisableIME`: в этом потоке нет IME (CJK-ввод в поле поиска). Раскладки RU/EN — не IME, печатать можно. Альтернатива без потерь: `SW_SHOWNOACTIVATE` → отрисовать/`Present` → `SetForegroundWindow` (права на передний план у процесса, запущенного пользователем, есть).
- `CreateWindowEx` первого окна (15–24 мс) — загрузка uxtheme/combase/rpcrt4/msctf и регистрация тем; `WS_POPUP`/без тем почти не помогают.

---

## 6. Что видит пользователь: пиксели и анимация DWM [замер]

`dup.exe` (Desktop Duplication, 165 Гц): окно `probe` с заливкой #3A7BD5 (поверх всех, чтобы фоновый запуск не спрятал его под активное окно); «первые пиксели» = ≥10 % точек сетки «синие», «кадр готов» = ≥95 % точек ровно #3A7BD5. 7 запусков на вариант, под нагрузкой, median, мс от `t0` (сырые данные — `results/dup.json`):

| вариант | t_content (DwmFlush в приложении) | первые пиксели | окно полностью видно |
|---|---|---|---|
| по умолчанию | 122 | 116 | **159** |
| `DWMWA_TRANSITIONS_FORCEDISABLED` | 98 | 94 | **97** |
| `ImmDisableIME` | 78 | 77 | 140 |
| оба | 79 | 79 | **79** |

- t_content (метод протокола) ≈ «первые пиксели» (±1 кадр) — **протокол корректен**.
- Анимация открытия Windows 11 (fade+scale) отодвигает момент «окно полностью на экране» на **40–63 мс**; `DwmSetWindowAttribute(DWMWA_TRANSITIONS_FORCEDISABLED, TRUE)` ([docs](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)) — бесплатный перцептивный выигрыш. Бенчмарк это не увидит, пользователь — да.

---

## 7. Управляемые рантаймы: .NET и Windows App SDK

### 7.1 Анатомия запуска .NET (источники)
apphost `.exe` → `hostfxr.dll` (поиск рантайма/фреймворков, `runtimeconfig.json`) → `hostpolicy.dll` (разбор `deps.json`, списки сборок/путей) → `coreclr.dll` (инициализация VM, AppDomain, GC) → JIT/ReadyToRun `Main` ([native-hosting.md](https://github.com/dotnet/runtime/blob/main/docs/design/features/native-hosting.md), [разбор startup sequence](https://mihai-albert.com/2020/03/08/startup-sequence-of-a-dotnet-core-app/)). Framework-dependent-приложения используют уже R2R-скомпилированный `System.Private.CoreLib`; R2R для самого приложения важен, когда много своего кода. NativeAOT — нет host/VM/JIT, один EXE ([Native AOT](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)). Публичные цифры разнятся на порядок (например, 64 мс AOT vs 588 мс JIT у консольного CLI в [dev.to, 2025](https://dev.to/lzocate-li/net-native-aot-parte-2-jit-r2r-e-aot-em-benchmarks-367i)) — зависят от объёма кода на старте, поэтому я мерил сам.

### 7.2 [замер] `Program.cs`: метка в первой строке `Main` + немного работы (50 `string.Format` + `List`), `WinExe`

Два прогона: с `--msonly` (без Windhawk; FDD-варианты с неподписанной `LabDotnet.dll` так не запускаются — политика блокирует её загрузку) и без (все с Windhawk):

| вариант (.NET 8.0.30, SDK 8.0.424) | размер | main, min (median) мс | CPU мс (median) |
|---|---|---|---|
| C, static CRT (эталон) | 0,1 МБ | 30–31 (32–71) | 11–22 |
| **NativeAOT** | 1,9 МБ EXE | **35–39 (44–76)** | 22–30 |
| FDD apphost, JIT tiered | 0,2 МБ + общий рантайм | 46,7 (84) | 59 |
| FDD + ReadyToRun | | 45,7 (61) | 46 |
| FDD `TieredCompilation=false` | | 48,7 (90) | 58 |
| self-contained / + R2R | 70 МБ, 187 файлов | 45–46 (61–72) | 44–52 |
| рантайм .NET 9.0.14 (rollForward) | | 48,6 (102) | 63 |
| single-file | 64 МБ EXE | **163 (174–204)** | 40–48 |
| single-file + compression | 34 МБ EXE | 142 (152–201) | 77–99 |

- «Голый» CLR (hostfxr+hostpolicy+coreclr+JIT `Main`) стоит **≈+15 мс** к нативному минимуму, NativeAOT — **≈+4 мс**. R2R/TC для hello-world неразличимы.
- Single-file — худшее, что можно сделать для старта: платит налог Defender за 64 МБ EXE (≈+130 мс), сжатие меняет его на распаковку (≈+50 мс CPU).
- Эти цифры — только рантайм. GUI-фреймворк поверх (WPF: D3D9+XAML; WinUI 3: WinRT+XAML+D3D11; Avalonia: Skia) добавляет сотни мс — это меряют прототипы wpf/winui3/avalonia. Официальные рекомендации WinUI ([MS Learn, 2026-03](https://learn.microsoft.com/en-us/windows/apps/develop/performance/app-startup-performance)): «≈1 мс на XAML-элемент», откладывать всё, что не нужно для первого кадра, `x:Load`.

### 7.3 Windows App SDK bootstrap [замер]
Unpackaged-приложение с framework-зависимым WinAppSDK 1.8 (`Microsoft.WindowsAppRuntime.Bootstrap.dll` из прототипа winui3): `LoadLibrary` ≈1 мс, **`MddBootstrapInitialize2(1.8)` ≈3 мс** (на Windows 11 — через API динамических зависимостей ОС, вспомогательного DDLM-процесса не появилось), `RoInitialize` ≈10–14 мс, первая activation factory класса WinAppSDK ≈4,5 мс. **Bootstrap — не проблема**; self-contained WinAppSDK его убирает совсем. В WinAppSDK 2.0 из пути старта WinUI убрали лишнюю зависимость `version.dll` ([release notes 2.0](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0)); на машине установлены рантаймы 1.4–1.8 и 2.x.

### 7.4 Упакованные (MSIX) приложения
Проверить без админа нельзя: Developer Mode выключен (`AllowDevelopmentWithoutDevLicense=0`) → ни `Add-AppxPackage -Register`, ни sparse-пакет без доверенного сертификата. Из документации ([MSIX behind the scenes](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-behind-the-scenes)): full-trust упакованное приложение получает виртуализацию AppData и HKCU (copy-on-write в приватное хранилище пакета), активация идёт через AppX-инфраструктуру. Косвенный **[замер]** цены виртуализации — §11.1: файловые операции в AppData из процесса «внутри» пакета Claude в 1,3–2,4 раза медленнее. Для FastMD упаковка ничего не даёт по скорости и несёт накладные — только если нужна идентичность пакета (Store, некоторые API).

---

## 8. Двойной клик в Explorer: путь запроса и как не создавать процесс вовсе

### 8.1 Путь
Explorer (`CDefView` → `IContextMenu::InvokeCommand` → `ShellExecuteEx`): расширение → `UserChoice` (Hash-защищён; для http/https/.pdf ещё и драйвер UCPD — [kolbi.cz 2024](https://kolbi.cz/blog/2024/04/03/userchoice-protection-driver-ucpd-sys/), [2025](https://kolbi.cz/blog/2025/07/15/ucpd-sys-userchoice-protection-driver-part-2/)) или ProgID → `shell\open\command`, где возможны три механизма:
1. **`command` (строка запуска)** → `CreateProcess` из explorer.exe (+ AppCompat, Windhawk в explorer, Defender).
2. **`DelegateExecute` = CLSID** объекта `IExecuteCommand` + `IObjectWithSelection` ([docs](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexecutecommand), [Raymond Chen, 2010](https://devblogs.microsoft.com/oldnewthing/20100312-01/?p=14623), [пример MS](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/winui/shell/appshellintegration/ExecuteCommandVerb/ExecuteCommandVerb.cpp)) — поддерживает **out-of-proc (LocalServer32)**. Если класс-объект уже зарегистрирован работающим процессом (`CoRegisterClassObject(..., REGCLS_MULTIPLEUSE)`), COM соединяет Explorer с ним — **новый процесс не создаётся вообще**; если нет — COM (DcomLaunch) запускает `server.exe -Embedding`.
3. **DDE (`ddeexec`)** — устаревшее широковещание, «please feel free to stop using DDE» ([Raymond Chen, 2007](https://devblogs.microsoft.com/oldnewthing/20070226-00/?p=27863)). Не использовать.

`.md` у пользователя трогать нельзя программно: в Windows 10/11 выбор приложения по умолчанию делает пользователь (диалог «Открыть с помощью»), приложение регистрирует ProgID + `OpenWithProgids` (HKCU, без админа).

### 8.2 [замер] Все пути через настоящий `ShellExecute` внутри explorer.exe
Тестовые расширения `.fmdtest*` (HKCU, установка/удаление через контекст explorer — см. §11.1; после опыта всё удалено и проверено `reg query` из explorer). `t0` — перед RPC-вызовом `IShellDispatch2::ShellExecute` в explorer. Вьюер — тривиальное GDI-окно `srv.exe` (156 КБ, `/MT`, без `ImmDisableIME`). 10 запусков (2 чередующихся блока по 5), средняя нагрузка машины, median (min); сырые данные — `results/handoff.jsonl`:

| путь | запрос дошёл до приложения | окно показано | кадр на экране (DwmFlush) | новый процесс |
|---|---|---|---|---|
| классика: ассоциация → новый `srv.exe` | 59,4 (55,0) | 90,8 | **98,3** (92,3) | да (srv.exe) |
| DelegateExecute, сервер не запущен → COM стартует `srv.exe -Embedding` | 50,1 (46,7) | 95,6 | 104,3 (96,8) | да (srv.exe через DcomLaunch) |
| крошечный `handoff.exe` (5 КБ, без CRT) → `WM_COPYDATA` → резидент | 59,0 (55,2) | 77,3 | 82,6 (79,6) | да (handoff.exe) |
| **DelegateExecute → работающий резидент** | **28,2** (24,3) | 49,6 | **57,0** (46,4) | **нет** (снимок процессов: ни srv/handoff/dllhost) |
| … + окно создано заранее (скрыто) | 26,9 | 32,6 | **38,0** (32,4) | нет |
| … + окно заранее показано и «укрыто» `DWMWA_CLOAK` | 24,9 | 25,5 | **32,1** (25,8) | нет |
| эталон: `CreateProcess` из тестового процесса (без shell) | 32,4 | 69,9 | 76,1 | да |

- Explorer→ассоциация→`CreateProcess` добавляет ≈27 мс к «голому» CreateProcess (59 vs 32 мс до `main`).
- **DelegateExecute к живому процессу — единственный способ обойти «пол» процесса**: ≈25 мс от вызова в Explorer до `Execute()` в нашем процессе, ≈32 мс до готового кадра с предсозданным «плащ»-окном. Для тривиального вьюера выигрыш 98→32 мс; для движка с тяжёлой инициализацией (WebView2, WinUI) — сотни мс.
- Холодный DelegateExecute (сервер не запущен) почти не хуже классики (+6 мс) — значит одна регистрация может обслуживать оба режима: «резидент есть → мгновенно; нет → COM его запустит».
- Лаунчер-передатчик `handoff.exe` почти ничего не даёт для лёгкого приложения: создание процесса (даже 5 КБ) — это и есть основная цена.
- Передний план: процесс, запущенный пользователем, имеет право `SetForegroundWindow`; резиденту его передают `AllowSetForegroundWindow(pid)` (в `handoff.exe`) или `CoAllowSetForegroundWindow` ([docs](https://learn.microsoft.com/en-us/windows/win32/api/objbase/nf-objbase-coallowsetforegroundwindow), [AllowSetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-allowsetforegroundwindow)). В моём стенде (фоновый источник запуска) проверить активацию корректно нельзя — **риск для реализации**, проверить вручную двойным кликом.

### 8.3 Explorer preview handler для .md
`IPreviewHandler` всегда вне процесса Explorer: in-proc сервер в суррогате `prevhost.exe` (или свой local server); регистрация per-user в HKCU возможна ([Preview Handlers](https://learn.microsoft.com/en-us/windows/win32/shell/preview-handlers), [регистрация](https://learn.microsoft.com/en-us/windows/win32/shell/how-to-register-a-preview-handler)). PowerToys уже поставляет Markdown-превью на WebView2 ([previewpane](https://github.com/microsoft/PowerToys/tree/main/src/modules/previewpane)) с известными проблемами жизненного цикла WebView2 в `prevhost` ([#30828](https://github.com/microsoft/PowerToys/issues/30828)). Для FastMD это отдельная «витрина» (быстрый просмотр в панели), которую можно сделать на том же нативном движке (DLL, D2D/DWrite в чужом HWND) — но это не путь двойного клика. Не приоритет v1.

---

## 9. Резидентные стратегии: прецеденты и риски

- **Прецеденты от Microsoft:** Office «Startup Boost» — задачи Планировщика «Office Startup Boost»/«…Logon», предзагружающие Word (требования ≥8 ГБ ОЗУ, отключается в режиме энергосбережения; раскатка 2025) ([MS Learn](https://learn.microsoft.com/en-us/troubleshoot/microsoft-365/admin/miscellaneous/new-startup-boost-tasks-windows-task-scheduler), [The Register](https://www.theregister.com/2025/05/01/microsoft_will_preload_office_apps/)); предзагрузка File Explorer в Windows 11 (Insider 26220.7271, ноябрь 2025, флажок «Enable window preloading for faster launch times») ([Windows Central](https://www.windowscentral.com/microsoft/windows-11/windows-11-may-soon-preload-file-explorer-in-the-background-in-an-attempt-to-speed-it-up)) — и независимый тест показал, что даже с предзагрузкой Explorer медленнее Windows 10 и тратит лишнюю память ([Windows Latest](https://www.windowslatest.com/2025/11/28/tested-windows-11s-faster-file-explorer-preloaded-is-still-slower-than-windows-10-and-uses-additional-ram/)).
- **Варианты для FastMD:** (a) резидент в трее/без окон, старт по `HKCU\...\Run` или задаче «при входе»; (b) резидент + `DelegateExecute` (без нового процесса); (c) резидент + тонкий лаунчер (named pipe/`WM_COPYDATA`) — только если нельзя (b); (d) предсозданное скрытое/«укрытое» окно с готовыми D2D/DWrite-ресурсами (−20…−25 мс сверх (b)).
- **Риски:** постоянная память (для нативного D2D-процесса — единицы-десятки МБ; для WebView2 — 100+ МБ и несколько процессов), вытеснение рабочего набора после долгого простоя (жёсткие page faults на первом открытии), фоновое троттлинг/EcoQoS для процессов без окон (отключается `SetProcessInformation(ProcessPowerThrottling)`), восприятие пользователем «ещё одного фонового процесса», обновление бинаря при работающем резиденте, корректная передача переднего плана, ограничения протокола стенда (`-resident` варианты меряются отдельно, §6 PROTOCOL.md).

---

## 10. ETW/WPR: как профилировать старт на этой машине

- **Без админа доступно** (пользователь в группе «Пользователи журналов производительности»): `logman start <s> -p <provider> -ets` для user-mode провайдеров — `Microsoft-Windows-Win32k`, `Microsoft-Windows-DxgKrnl`, `Microsoft-Windows-Dwm-Core`, `Microsoft-Windows-DotNETRuntime`, **`Microsoft-Antimalware-Engine`/`-RTP`** (так и получена атрибуция §2.1); чтение — `Get-WinEvent -Path x.etl`.
- **Нужен админ [замер]:** `wpr -start CPU` → `0xc5585011`, `xperf -on PROC_THREAD+LOADER` → «Отказано в доступе», `Microsoft-Windows-Kernel-Process` и `Microsoft-Windows-CodeIntegrity` — Access denied. Значит сэмплинг стеков CPU, ReadyThread, Image Load, Disk I/O — только в повышенной сессии.
- **Рекомендуемый профиль для оркестратора (в админ-консоли):** `wpr -start GeneralProfile -start CPU -start DiskIO -start FileIO -filemode` → запуск прототипа → `wpr -stop start.etl`; в WPA: *CPU Usage (Precise)* с колонкой Ready Time, *Images* (время загрузки DLL), *Process Lifetimes*, *Generic Events* от провайдеров выше; свои маркеры — TraceLogging-события в коде (`TraceLoggingWrite`) и файл *Regions of Interest* для фаз «main→window→first frame» ([WPR/WPA](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-recorder), [Regions of Interest](https://needleinathreadstack.wordpress.com/2014/04/08/writing-a-wpr-regions-of-interest-file/)); готовый инструмент атрибуции старта процессов на ETW — [launchlab](https://github.com/SP42K/launchlab). Для задержки «present → экран» — PresentMon (тоже требует группу Performance Log Users/админа) или мой `dup.exe`.
- **Без ETW** хорошо работает то, что я использовал: метки в разделяемой памяти, `QueryProcessCycleTime`/`GetThreadTimes` вокруг вызова, трассировка сообщений в `WndProc`, дифф модулей по шагам, Desktop Duplication для пикселей.

---

## 11. Замечания к стенду и протоколу (важно для финальной прогонки)

### 11.1 Виртуализация MSIX у всего, что запущено из Claude Desktop [замер]
Claude Desktop — MSIX-пакет; дочерние процессы сессии (у них `GetCurrentPackageFullName` = нет пакета) тем не менее получают **виртуализацию записи в `%LOCALAPPDATA%`/`%APPDATA%` и `HKCU`**: запись `HKCU\Software\...` и файла в `%LOCALAPPDATA%` видна из сессии, но **не видна** из контекста explorer.exe (`reg query`/`if exist` через `launch.exe --mode explorer`). Данные лежат в `…\Packages\Claude_pzs8sxrjxfjjc\LocalCache\Local` и `SystemAppData\Helium\UserClasses.dat`. Уже сейчас там профили `FastMD\WebView2-cpp-webview2-*`, `FastMD\tauri`, `FastMD\wry`, `AvaloniaUI`…
- Стоимость **[замер]** (400 файлов по 4 КБ в `%LOCALAPPDATA%`, 3 повтора): чтение **50 vs 21 мс (×2,3)**, создание+запись 140 vs 112 мс (×1,25), stat+перечисление 110 vs 79 мс (×1,4), удаление 121 vs 70 мс (×1,7) — «сессия Claude» против «контекста explorer».
- Последствия: (1) веб-прототипы (WebView2/Tauri/wry/Electron), активно пишущие профиль при старте, в стенде немного медленнее, чем у пользователя; (2) любые регистрации в HKCU из сессии (COM, ассоциации) невидимы системе — именно поэтому мой первый опыт с DelegateExecute давал `REGDB_E_CLASSNOTREG`, хотя `CoCreateInstance` к уже запущенному серверу работал.
- **Рекомендация:** финальную серийную прогонку `bench.py` запускать вне дерева процессов Claude — из обычного терминала пользователя, через Планировщик заданий, или через explorer: `research/lab/startup/bin/launch.exe --mode explorer --timeout 3600000 -- C:\path\run_bench.cmd` (explorer исполняет `.cmd` в своём, не виртуализированном контексте). Хотя бы один раз сравнить результаты «из сессии» и «вне».

### 11.2 Прочее
- **Windhawk** (§1.2): постоянная надбавка ≈5 мс CPU/процесс (под нагрузкой — и ≈5 мс до `main`) и загрузка shell32/COM в каждый процесс; для всех прототипов одинакова по природе, но искажает «пол». Стоит сообщить пользователю; при желании он может исключить FastMD-процессы в настройках мода/движка (не трогал).
- **TextInputHost** на этой машине аномален (35 CPU-часов) → редкие многосекундные `ShowWindow` у всех прототипов с активируемым окном. Перед финальным замером — перезапустить `TextInputHost.exe` (пользователь) или сделать выход/вход.
- **`first_ms`** в отчёте `bench.py` = первый запуск после сборки, он включает первичное сканирование Defender (+20…+150 мс) — не путать с «холодным стартом ОС».
- **`launch_overhead_ms`** (CreateProcess+Job+Resume) честно включает налог Defender на размер EXE (§2.2) — это реальная цена для пользователя, её надо оставить, но показывать отдельной колонкой (объясняет «почему Electron такой»).
- **t_content не видит анимацию DWM** (§6): для перцептивного сравнения можно дополнительно мерить «окно полностью видно» `dup.exe` (он принимает любую командную строку и прямоугольник).
- Мелочь: в `run_once` наблюдатель-поток Python опрашивает `EnumWindows` каждую 1 мс, удерживая GIL, — на 32 потоках это не влияет на ребёнка, но `observed_ms` имеет погрешность ~1–2 мс.

---

## 12. Выводы для FastMD

### 12.1 Ранжированный список техник (оценки — для этой машины; на «чистой» машине абсолютные цифры меньше, соотношения те же)

| ранг | техника | ожидаемый выигрыш | цена/риск |
|---|---|---|---|
| 1 | **Не создавать аппаратное D3D-устройство до первого кадра** (WARP-D2D, DWrite→DIB или GDI; HW — лениво/в фоне/никогда) | **−210 мс [тихо]** (до −300 под нагрузкой) на ПК с NVIDIA: устройство ≈205 + первый Present ≈18; CPU процесса ×4 | WARP грузит CPU при скролле больших документов — для текста приемлемо; при необходимости переключаться на HW после первого кадра |
| 2 | **Маленький EXE** (<≈2–5 МБ), тяжёлое — в DLL; не single-file .NET/не Electron | **−1,2…−1,9 мс на каждый МБ** на каждый запуск (150 МБ → −300 мс) | — |
| 3 | **`DelegateExecute` (IExecuteCommand) + резидентный процесс** с предсозданным/«укрытым» окном | путь Explorer→кадр **98 → 32…57 мс** для лёгкого вьюера; для тяжёлого движка — сотни мс | постоянный процесс/память, установщик регистрирует COM+ProgID, передача foreground, режим «-resident» стенда |
| 4 | **`ImmDisableIME(-1)`** или показ без активации с активацией после первого кадра | **−12…−18 мс [тихо]** (−25…−35 под нагрузкой) и защита от многосекундных TSF-зависаний | без IME в потоке (CJK-ввод в поиске) — либо no-activate-first |
| 5 | **`DWMWA_TRANSITIONS_FORCEDISABLED`** | окно полностью видно на **40–60 мс** раньше (перцептивно) | нет анимации открытия (для «пули» — плюс) |
| 6 | **Viewport-first**: вёрстка/рендер только первого экрана, остальное — после Present | зависит от документа: large.md (3,7 МБ) — сотни мс; эмодзи/CJK/таблицы за пределами экрана не стоят ничего | сложнее прокрутка/поиск до окончания фоновой вёрстки |
| 7 | **Эмодзи:** свой растровый кэш, монохром в первом кадре при многих уникальных эмодзи | 22–37 мс [тихо] (до 90 под нагрузкой) на первом экране medium.md (5 уникальных эмодзи) | небольшое мигание при «подкраске» |
| 8 | **Без COM/WinRT на критическом пути** (`CoInitializeEx`/`RoInitialize`) | −11…−16 мс каждый [тихо] | WebView2/WinUI без COM невозможны → аргумент против них |
| 9 | **GUI-подсистема**, статический CRT, без .NET-хоста (или NativeAOT) | консоль: −0,5…−1,6 с; CLR: −11…−15 мс против NativeAOT | — |
| 10 | Подпись кода и «репутация» файла | мелкий известный EXE: 4–6 мс против 18–24 мс у неизвестного | зависит от распространённости, сразу не получить |
| 11 | Не делать упаковку MSIX ради скорости | виртуализация AppData/HKCU: I/O ×1,3–2,4 | только если нужна идентичность пакета |

### 12.2 Бюджет «пули» (нативный вариант, по моим замерам [тихо])
`CreateProcessW` ≈21 → `main` ≈31 → `CreateWindowEx` (+noime) ≈13 → `ShowWindow` без TSF ≈3–6 → DirectWrite: фабрика+формат+вёрстка первого экрана ≈3–5 → (a) DIB-путь: рисование ≈2 + BitBlt, или (b) WARP-устройство ≈15 + swap chain ≈3 + D2D-рисование ≈1,5 (+≈22 при 5 цветных эмодзи) → `DwmFlush` ≈5–8 ⇒ **≈65–80 мс до кадра** (замеренные комбинации: noime+popup GDI — 58 мс, DWrite→DIB — 82 мс и WARP-D2D — 100 мс с обычным `ShowWindow` ≈16 мс), без анимации DWM. Под нагрузкой ×1,3–1,5. Двойной клик добавляет ≈25–30 мс работы Explorer (§8.2). Через `DelegateExecute` к резиденту — **≈30–40 мс** от вызова в Explorer. Для сравнения: калибровочный пол стенда (`baseline-win32`) ≈90 мс включает TSF (≈12–30 мс) и Windhawk.

### 12.3 Риски
- Основные серии сняты при 80–90 % загрузке CPU, повторы [тихо] — при 13–39 %; они подтверждают выводы и порядок величин, но финальные сравнения — за серийной прогонкой оркестратора (желательно вне дерева процессов Claude, §11.1). Опыты с передачей через Explorer (§8.2) и «фотонные» (§6) — только под нагрузкой.
- Атрибуция налога на размер EXE к Defender основана на ETW-событиях движка и kernel-CPU родителя; без админ-трассировки ядра точный драйвер (WdFilter vs CI) не подтверждён. На машинах с другим AV/EDR цифры будут другими — меня здесь интересует вывод «маленький EXE», он от AV не зависит.
- Цена NVIDIA-устройства зависит от драйвера; на ноутбуках с Optimus устройство по умолчанию — iGPU (Intel/AMD), там свои цифры (у AMD iGPU здесь 65–84 мс + кросс-адаптерный swap chain 250 мс, если монитор на dGPU).
- `ImmDisableIME` меняет поведение ввода; «укрытое» предсозданное окно и резидент требуют аккуратной работы с foreground и Alt-Tab.
- COLRv1-эмодзи и `IDWriteBitmapRenderTarget3` зависят от сборки Windows (на 26200 интерфейс не доступен через QI).

---

## Источники
- Windhawk: [Implementing Global Injection and Hooking in Windows (m417z, 2022)](https://m417z.com/Implementing-Global-Injection-and-Hooking-in-Windows/); [Injection targets (wiki)](https://github.com/ramensoftware/windhawk/wiki/Injection-targets-and-critical-system-processes)
- Defender: [Set-MpPreference (EnableFileHashComputation и др.)](https://learn.microsoft.com/en-us/powershell/module/defender/set-mppreference); [Windows 11 + Device Guard slows down CreateProcess (Q&A)](https://learn.microsoft.com/en-us/answers/questions/5862177/windows-11-device-guard-slows-down-createprocess-p)
- DWM/окна: [DWMWINDOWATTRIBUTE](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute); [ImmDisableIME](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immdisableime)
- Эмодзи: [Bringing new emoji to Windows 11 (Microsoft Design)](https://microsoft.design/articles/bringing-new-emoji-to-windows-11/); [Segoe UI Emoji](https://learn.microsoft.com/en-us/typography/font-list/segoe-ui-emoji)
- .NET: [native-hosting.md](https://github.com/dotnet/runtime/blob/main/docs/design/features/native-hosting.md); [Startup sequence of a .NET Core app](https://mihai-albert.com/2020/03/08/startup-sequence-of-a-dotnet-core-app/); [Native AOT deployment](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/); [JIT vs R2R vs AOT benchmarks (dev.to)](https://dev.to/lzocate-li/net-native-aot-parte-2-jit-r2r-e-aot-em-benchmarks-367i)
- WinUI/WinAppSDK: [WinUI app startup performance (2026-03)](https://learn.microsoft.com/en-us/windows/apps/develop/performance/app-startup-performance); [MddBootstrapInitialize2](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/win32/mddbootstrap/nf-mddbootstrap-mddbootstrapinitialize2); [Windows App SDK 2.0 release notes](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0); [Single-instance WinUI (AppInstance)](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/applifecycle/applifecycle-single-instance)
- MSIX: [Understanding how packaged desktop apps run on Windows](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-behind-the-scenes)
- Shell: [IExecuteCommand](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexecutecommand); [Simplifying context menu extensions with IExecuteCommand (Old New Thing)](https://devblogs.microsoft.com/oldnewthing/20100312-01/?p=14623); [ExecuteCommandVerb sample](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/winui/shell/appshellintegration/ExecuteCommandVerb/ExecuteCommandVerb.cpp); [How to launch an unelevated process… (IShellDispatch2 в explorer)](https://devblogs.microsoft.com/oldnewthing/20131118-00/?p=2643); [Please feel free to stop using DDE](https://devblogs.microsoft.com/oldnewthing/20070226-00/?p=27863); [CoAllowSetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/objbase/nf-objbase-coallowsetforegroundwindow); [AllowSetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-allowsetforegroundwindow); [Merged view of HKCR / per-user COM](https://learn.microsoft.com/en-us/windows/win32/sysinfo/merged-view-of-hkey-classes-root); UCPD: [kolbi.cz 2024](https://kolbi.cz/blog/2024/04/03/userchoice-protection-driver-ucpd-sys/), [2025](https://kolbi.cz/blog/2025/07/15/ucpd-sys-userchoice-protection-driver-part-2/)
- Preview handlers: [Preview Handlers and Shell Preview Host](https://learn.microsoft.com/en-us/windows/win32/shell/preview-handlers); [How to Register a Preview Handler](https://learn.microsoft.com/en-us/windows/win32/shell/how-to-register-a-preview-handler); [PowerToys previewpane](https://github.com/microsoft/PowerToys/tree/main/src/modules/previewpane)
- Резиденты/предзагрузка: [Office Startup Boost tasks](https://learn.microsoft.com/en-us/troubleshoot/microsoft-365/admin/miscellaneous/new-startup-boost-tasks-windows-task-scheduler); [The Register 2025-05-01](https://www.theregister.com/2025/05/01/microsoft_will_preload_office_apps/); [Explorer preload (Windows Central)](https://www.windowscentral.com/microsoft/windows-11/windows-11-may-soon-preload-file-explorer-in-the-background-in-an-attempt-to-speed-it-up); [Explorer preload tested (Windows Latest)](https://www.windowslatest.com/2025/11/28/tested-windows-11s-faster-file-explorer-preloaded-is-still-slower-than-windows-10-and-uses-additional-ram/)
- ETW: [Windows Performance Recorder](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-recorder); [WPR start/stop](https://devblogs.microsoft.com/performance-diagnostics/wpr-start-and-stop-commands/); [Regions of Interest](https://needleinathreadstack.wordpress.com/2014/04/08/writing-a-wpr-regions-of-interest-file/); [launchlab](https://github.com/SP42K/launchlab)
