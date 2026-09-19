# 01. Нативные UI-фреймворки Windows для быстрого и стильного Markdown-ридера

> Исследование для FastMD. Дата: 2026-09-19. Машина: Windows 11 Pro 26200, Ryzen 9 7950X, 64 GB, NVMe,
> NVIDIA RTX 5070 (драйвер 32.0.16.1656) + AMD iGPU, Defender real-time ON, сессия без админа.
>
> Обозначения: **[замер]** — собственные измерения в `research/lab/native-ui/` (методика в §1),
> **[инспекция]** — разбор бинарников и манифестов на этой машине, **[источник]** — публикация (ссылка рядом),
> **[оценка]** — вывод из замеров и источников, не прямое измерение.
>
> Полноценные прототипы (WinUI 3, WPF, Avalonia, cpp-d2d, RichEdit и т. д.) строят другие агенты в `bench/protos/*`.
> Здесь анатомия запуска, практические проверки «фольклора» и сравнение стеков.

---

## 0. Коротко

1. **Главный налог на этой машине — драйвер GPU, а не фреймворк.** [замер] `D3D11CreateDevice(HARDWARE)` на RTX 5070
   занимает **~185 мс в каждом новом процессе**, и прогретые кэши не помогают. WARP (программный D3D11) —
   **3,5–4 мс**, iGPU AMD — 45–60 мс. Сэмплирование стека показывает, что ~40 % этого времени NVIDIA-загрузчик
   `nvldumdx.dll` тратит на проверку Authenticode-подписей своих DLL (`imagehlp`/`wintrust`/`crypt32`). В процесс
   подгружается 212 МБ образов (`nvgpucomp64.dll` 106 МБ, `nvwgf2umx.dll` 86 МБ). Независимое подтверждение
   (RTX 3090, 2026-09-17): 159 мс против 9,5 мс на WARP, и вызов никак не ускорить.
   Следствие: **любой стек, который создаёт аппаратный D3D-девайс до первого кадра** (WinUI 3, WPF, Avalonia/ANGLE,
   Uno/OpenGL, egui/wgpu, собственный D2D на hardware), **теряет здесь 180–250 мс**.
2. **Пол Win32 без GPU — около 90 мс** (калибровка оркестратора). Текст через GDI или D2D (software/WARP) и
   отложенная активация окна дают на нагруженной машине 92–150 мс [замер]. Это единственный класс решений,
   которому доступна «пуля» на уровне 100–150 мс.
3. **Активация окна (IME/TSF) стоит ~25–60 мс** прямо на критическом пути [замер]. Если показать окно без активации
   (`SW_SHOWNOACTIVATE`), нарисовать первый кадр и только потом вызвать `SetForegroundWindow`, GDI-вариант
   ускоряется со 119 до 92 мс, а WARP-вариант — со 173 до 151 мс. Приём переносится на любые фреймворки
   (`AppWindow.Show(false)` в WinUI, `ShowActivated=false` в WPF и Avalonia). Оговорка: это замер в условиях
   харнесса, где окно не получает foreground; запуск из Explorer надо проверить отдельно (§2.3).
4. **Цветные эмодзи стоят ~30 мс при первой отрисовке** в DirectWrite/D2D (COLR-глифы Segoe UI Emoji).
   Без цвета — 3 мс. Fallback на CJK дешёвый: 1–2 мс [замер].
5. **Размер exe напрямую замедляет запуск.** Каждые +55 МБ «мёртвых» данных в конце exe дают +130 мс на каждом
   старте: Defender сканирует exe при создании процесса и не кэширует результат. У DLL скан кэшируется, платится
   только при первой загрузке [замер]. WinForms NativeAOT на .NET 8 даёт exe на 56,7 МБ и запускается медленнее JIT.
6. **WinUI 3 / Windows App SDK жив и активно ускоряется.** Есть SemVer-линия 2.x: 2.0.1 от 29.04.2026, 2.5.1 от 16.09.2026.
   В 2.3.1 добавлены opt-in оптимизации XAML. Сам WinUI недорог: бутстраппер 3–9 мс, загрузка `Microsoft.UI.Xaml.dll`
   6–15 мс, фабрика `Application` 13–29 мс [замер]. Дорого другое: инициализация XAML-ресурсов, аппаратный D3D и
   отсутствие синхронизации «показ окна ↔ первый кадр» (issue #11677, август 2026).
7. **Inbox-Notepad 11.2607 [инспекция]** устроен так: Win32-хост, WinUI 3 (Windows App SDK **2.x**) через
   XAML Islands только для «хрома», **собственная копия Office RichEdit** (D2D/DirectWrite, `msptls.dll` для таблиц)
   как поверхность документа, **cmark-gfm 0.29.0.gfm.13** статически внутри `Notepad.exe`. Markdown у Microsoft
   рендерит RichEdit, а не XAML.
8. **WPF (минимальное окно с TextBlock) — 630–670 мс [замер]**. `RenderMode.SoftwareOnly` не спасает: драйвер
   NVIDIA для D3D9 всё равно грузится. NativeAOT для WPF нет (dotnet/wpf#3811, milestone Future). Для «пули»
   WPF не подходит. MAUI на Windows = WinUI 3 + слой MAUI, NativeAOT там из коробки не работает.
   WinForms — только как хост и без выигрыша над Win32.
9. **Рекомендация.** Ядро FastMD: C/C++ Win32 + DirectWrite/Direct2D. Первый кадр рисуется на WARP или software-D2D
   через DComp flip, окно активируется после первого кадра, парсер md4c, один маленький exe. Аппаратный GPU,
   WinUI-«хром» и WebView (если понадобится) подгружаются **после** первого кадра. Avalonia 12 имеет смысл
   рассматривать только при требовании кроссплатформенности, и её обязательно надо проверить в `RenderingMode=Software`.

---

## 1. Методика и оговорки

- **Раннер** `research/lab/native-ui/runner/labrun.py` импортирует примитивы `bench/harness/bench.py` без изменений:
  CreateProcess с job object, `t0` прямо перед `CreateProcessW`, результат в формате PROTOCOL.md.
  Приложения делают то же, что требует протокол: первый кадр → `DwmFlush()` → `t_content`. Выходные данные раннера
  лежат только в `research/lab/native-ui/`.
- **Лабораторные приложения.** Это не markdown-вьюеры, а «пол» соответствующего стека: окно 1000×800 DIP и первые
  2,5 КБ `medium.md` как обычный переносимый текст (Segoe UI Variable Text, 15 px). В этот фрагмент попадают
  кириллица, CJK и эмодзи.
- **Прогон.** 11 чередующихся запусков на вариант, перед ними один прогревочный. Показаны медиана и минимум.
  Тёплый старт: файлы в кэше.
- **Шум.** Во время замеров параллельно шли сборки других агентов, загрузка CPU 35–100 %. В финальном прогоне
  `baseline-win32` дал медиану **117 мс** против калибровочных **90 мс** на простое. Абсолютные цифры поэтому завышены
  примерно на 25–30 мс. **Для выводов используются разности между вариантами одного прогона.**
- **Холодный старт** (сразу после перезагрузки или очистки standby list) не измерялся: очистка требует прав
  администратора. Там, где он упоминается, это **[оценка]**.
- **Конфаундеры машины** [инспекция]:
  - **Windhawk 1.7.3 внедряет 4 модуля в каждый новый процесс**, включая процессы, запущенные из python-харнесса
    (проверено `w32/out/modlist.exe`): `windhawk.dll`, мод `explorer-details-better-file-sizes_1.5`,
    `libc++.whl`, `libunwind.whl`. Эта цена сидит внутри «пола» 90 мс у **всех** прототипов.
  - NVIDIA App/ShadowPlay подгружает `nvspcap64.dll` в каждый D3D-процесс.
  - Запущен MSI Afterburner.
  - `TextInputHost.exe` накопил аномально много CPU-времени, что может раздувать стоимость активации IME (§2.3).

---

## 2. Анатомия «до первого контента» на этой машине [замер]

### 2.1 Сводная таблица финального прогона

Прогон `runner/final.json` → `runner/results/final.json`, 11 запусков, `medium.md`, машина под нагрузкой 35–50 %.
Метки — медианы в мс от `t0`.

| Вариант | Что делает | Медиана | Мин | Разбивка (медианы, мс) |
|---|---|---:|---:|---|
| `baseline-win32` | эталон оркестратора (GDI, одна строка) | **117** | 88 | main 44 |
| `gdi` | Win32 + GDI `DrawTextW` 2,5 КБ | **119** | 102 | main 47 → window 67 → **ShowWindow 98** → drawn 110 |
| `gdi-noime` | + `ImmDisableIME(-1)` | **94** | 77 | window 59 → **ShowWindow 74** → drawn 87 |
| `gdi-lateact` | `SW_SHOWNOACTIVATE`, активация после кадра | **92** | 74 | window 64 → **ShowWindow 72** → drawn 86 |
| `d2d-hwnd-sw` | D2D HwndRenderTarget SOFTWARE + DirectWrite | **176** | 142 | shown 96 → RT 120 → layout 123 → drawn 168 |
| `warp-dcomp` | D3D11 **WARP** + DXGI flip + DComp + D2D/DWrite | **173** | 128 | shown 99 → device 119 → layout 126 → EndDraw 157 → present 164 |
| `warp-dcomp-lateact` | то же + отложенная активация | **151** | 119 | shown 70 → device 92 → layout 100 → EndDraw 135 → present 142 |
| `d2d-hwnd-hw` | D2D HwndRenderTarget, по умолчанию (NVIDIA) | **358** | 327 | shown 104 → **RT 306 (+202)** → drawn 349 |
| `d3d-dcomp-hw` | D3D11 HARDWARE (NVIDIA) + DComp + D2D | **459** | 369 | shown 107 → **device 362 (+254)** → EndDraw 433 → present 453 |
| `igpu-dcomp` | D3D11 на AMD iGPU (MINIMUM_POWER) | **447** | 383 | device 171 (+50) → **swapchain 375 (+205)** → present 438 |
| `con-aot` | .NET 8 NativeAOT, только `Main` (без окна) | **51** | 45 | main 51 |
| `con-jit` | .NET 8 JIT (framework-dependent), только `Main` | **70** | 61 | main 70 |
| `con-aot+55MB-overlay` | тот же exe + 55 МБ «хвоста» в файле | **182** | 161 | main 182 |
| `wf-jit` | WinForms .NET 8, JIT, `TextRenderer` | **248** | 199 | main 89 → form 133 → painted 237 |
| `wf-sc-r2r` | WinForms self-contained ReadyToRun | **243** | 186 | main 96 → painted 237 |
| `wf-aot` | WinForms NativeAOT (exe 56,7 МБ) | **305** | 272 | **main 199** → painted 296 |
| `wpf-r2r-hw` | WPF R2R, `Window` + `TextBlock` | **672** | 516 | main 103 → window 232 → **ContentRendered 648** |
| `wpf-r2r-sw` | то же + `RenderMode.SoftwareOnly` | **632** | 524 | main 92 → window 222 → ContentRendered 611 |

Что видно из таблицы:

- **Процесс до `main` стоит ~45 мс**, у .NET JIT — ~70 мс, у NativeAOT — ~51 мс. В эту цифру входят Defender и
  инъекция Windhawk.
- **`ShowWindow` с активацией стоит ~31 мс**; без IME или с отложенной активацией — ~9–15 мс (§2.3).
- Текст GDI (2,5 КБ, с font linking) рисуется за ~12 мс. DirectWrite-раскладка того же текста — 2–4 мс, но
  первая отрисовка с цветными эмодзи — ~30–45 мс (§2.4).
- **Аппаратный D3D-девайс добавляет 200–250 мс** в любом сочетании (HwndRenderTarget, DComp, flip).
  Вариант с iGPU не спасает: девайс создаётся быстро (50 мс), зато swapchain для монитора, подключённого к NVIDIA,
  стоит 205 мс. **Гипотеза:** требуется кросс-адаптерное представление, и драйвер NVIDIA всё равно грузится.

### 2.2 GPU-драйвер — главный налог [замер]

`w32/d3dprobe.cpp`: консольный процесс без окна, каждое измерение — новый процесс (`results/d3dprobe.txt`).

| Что | Время | Комментарий |
|---|---:|---|
| `CreateDXGIFactory2` | 11–20 мс | уже сама по себе дорогая: dxcore, directxdatabasehelper |
| `D3D11CreateDevice(HARDWARE)`, NVIDIA RTX 5070 | **181–237 мс, медиана ~187** | 9 процессов; одинаково в первом и десятом запуске |
| то же, второй девайс в том же процессе | 23–33 мс | даже при уже загруженном драйвере |
| `D3D11CreateDevice(WARP)` | **3,4–4,2 мс** | программный растеризатор D3D11 (SIMD, многопоточный) |
| `D3D11CreateDevice` на AMD iGPU | 44–60 мс | |
| `D3D12CreateDevice` (NVIDIA) | 201–285 мс | |

После создания NVIDIA-девайса в процессе появляется **30 новых модулей общим размером 211,8 МБ**:
`nvgpucomp64.dll` 105,7 МБ, `nvwgf2umx.dll` 86,0 МБ, `nvppex.dll` 7,2 МБ, `NvMemMapStoragex.dll`, `nvldumdx.dll`,
`nvspcap64.dll`. Кроме них — **`wintrust.dll`, `CRYPT32.dll`, `imagehlp.dll`, `wldp.dll`, `drvstore.dll`**.

**Куда уходит время.** `w32/stackprobe.cpp` — самодельный сэмплирующий профайлер уровня модулей: он
приостанавливает поток каждые 0,25 мс, копирует стек и раскладывает его по модулям (`results/stackprobe.txt`,
два прогона по 847 и 955 сэмплов):

| Модуль | Эксклюзивно (исполняется) | Инклюзивно (есть на стеке) |
|---|---:|---:|
| `imagehlp.dll` (Authenticode-хэш PE, `ImageGetDigestStream`) | **39–40 %** | 40 % |
| `ntdll.dll` (загрузчик, page faults) | 37–38 % | 98 % |
| `win32u.dll` (syscalls D3DKMT) | 11–12 % | 14 % |
| код самих `nv*.dll` | ~7 % | — |
| `nvldumdx.dll` (NVIDIA UMD loader) | 0,1 % | **65–66 %** |
| `CRYPT32.dll` / `wintrust.dll` | — | **47 % / 40–41 %** |

Интерпретация [оценка на основе замера]: примерно **40 % создания девайса — это проверка цифровых подписей
~200 МБ драйверных DLL**, которую `nvldumdx.dll` выполняет через WinVerifyTrust в каждом новом процессе. Ещё
~40 % — отображение и связывание самих образов. Со стороны приложения это не лечится.

**Независимое подтверждение** [источник]: проект nene-nib (C++ редактор, D3D11/D2D/DComp), замер от 2026-09-16/17
на RTX 3090 с драйвером 32.0.16.1088. `device_created` = **159 мс из 184 мс всего старта**, на WARP — 9,5 мс.
Не помогли ограничение feature levels, явный адаптер, `SINGLETHREADED` и повторный запуск (все отклонения в
пределах ±6 мс). Проект решил показывать окно до создания девайса:
<https://github.com/hideyukiMORI/nene-nib/issues/24>,
`docs/quality/speed-reference.md` в том же репозитории.

**Попытка распараллелить не помогла** [замер]. Вариант `d3d-dcomp-bgdev` создавал девайс в фоновом потоке, начиная с
первой строки `main`, и стал медленнее: окно создавалось 132 мс вместо 92, `ShowWindow` занимал 193 мс вместо 138.
Загрузка драйвера держит loader lock и тормозит загрузку DLL в основном потоке (uxtheme, IME и т. д.).

**Что это значит для стеков** [оценка]:

| Стек | Как рисует по умолчанию | Налог NVIDIA на пути к первому кадру |
|---|---|---|
| WinUI 3 / UWP XAML | D3D11 + DComp (lifted compositor в процессе: `dcompi.dll`, `dwmcorei.dll`) | да, публичного software-режима нет |
| WPF | D3D9Ex (`nvd3dumx.dll` грузится даже при `SoftwareOnly`, см. §5) | да |
| Avalonia | `RenderingMode = [AngleEgl, Software]` → ANGLE → D3D11 | да по умолчанию, **нет при `Software`** |
| Uno Skia (Win32) | Skia + OpenGL (по умолчанию), Vulkan опционально | да (OpenGL ICD NVIDIA) |
| egui / iced (wgpu) | D3D12 / Vulkan | да, 200–285 мс только на девайс |
| Chromium / WebView2 / Electron | отдельный GPU-процесс | параллельно, но первый кадр композитора его ждёт [гипотеза] |
| Свой D2D на HARDWARE | D3D11 | да |
| Свой D2D на WARP / software, GDI | CPU | **нет** |

### 2.3 Активация окна, IME и TSF [замер]

В варианте `gdi` вызов `ShowWindow(SW_SHOWNORMAL)` занимает ~31 мс медианы (в более шумных прогонах 42–62 мс).
С `ImmDisableIME(-1)` — 15 мс. С `SW_SHOWNOACTIVATE` и `SetForegroundWindow` после первого кадра — **9 мс**.
`ImmAssociateContextEx(hwnd, NULL, 0)` не помогает (прогон `runner/w32.json`). Значит, дорог не контекст IME окна,
а активация потока/окна в TSF: WM_ACTIVATE → WM_SETFOCUS → инициализация Text Services Framework и связь с
`TextInputHost`.

Приём **«первый кадр до активации»**: окно показывается неактивным, рисуется, `DwmFlush`, затем
`SetForegroundWindow`. Процесс, запущенный двойным кликом из Explorer, имеет право на foreground. Экономия на этой
машине: **−27 мс (GDI) и −23 мс (WARP)** по медиане. IME в строке поиска при этом продолжает работать, в отличие
от `ImmDisableIME`. Аналоги в фреймворках: WinUI — `AppWindow.Show(activateWindow: false)` с последующим
`Activate()`, WPF — `Window.ShowActivated = false`, Avalonia — `Window.ShowActivated = false`.
Работоспособность в каждом фреймворке надо проверять в прототипах.

**Важная оговорка** [замер, `runner/fgcheck.py`, `results/fgcheck.txt`]. Окно, запущенное из фонового
python-процесса, **не становится foreground ни в одном варианте**: через 400 мс после кадра `GetForegroundWindow`
указывает на чужой процесс. Срабатывает foreground lock: у харнесса нет прав foreground, чтобы передать их дочернему
процессу. Поэтому «активация» в харнессе — это активация внутри своей очереди ввода без реального переключения
фокуса. При двойном клике из Explorer процесс получает права foreground, и цена активации с TSF/фокусом может быть
**другой, вероятно не меньшей**. Эффект −25 мс надо перепроверить запуском из Explorer.

> Для оркестратора: PROTOCOL.md §5 требует «shown activated». Вариант с активацией через ~1 кадр после контента
> стоит явно разрешить или запретить для всех прототипов одинаково.

### 2.4 Шрифты: fallback и цветные эмодзи [замер]

`w32/dwprobe.cpp`: DirectWrite-раскладка и отрисовка через D2D в WIC-битмап (software, без окна и GPU),
первые 2,5 КБ `medium.md`, 5 новых процессов на вариант (`results/dwprobe.txt`):

| Вариант | Раскладка (`GetMetrics`) | **Первая отрисовка** | Вторая отрисовка |
|---|---:|---:|---:|
| Полный текст (кириллица + CJK + эмодзи), `ENABLE_COLOR_FONT` | 1,7–2,4 мс | **29,9–41,4 мс** | 3,1–5,2 мс |
| Тот же текст, без цветных шрифтов | 1,9–2,6 мс | 2,6–4,1 мс | 0,4–1,0 мс |
| Без CJK и эмодзи («latin + кириллица») | 1,3–1,7 мс | 2,1–2,9 мс | 0,6–0,9 мс |

Выводы:

- Fallback на CJK-шрифты в DirectWrite почти бесплатен: системный кэш шрифтов и сервис FontCache делают своё дело.
- **Цветные эмодзи Segoe UI Emoji (COLR) стоят ~30 мс при первой отрисовке** и 3–5 мс при каждой следующей.
  В первом экране `medium.md` эмодзи есть уже в строке 3.
- Возможные меры: кэшировать растр эмодзи в атласе; на первом кадре рисовать эмодзи монохромно и перерисовывать
  в цвете следующим кадром (приём допустим, но о нём надо написать в README); растеризовать эмодзи в фоне.
- GDI (`DrawTextW`) цветные эмодзи не рисует вовсе: только монохромные контуры через font linking. Поэтому GDI
  дешёвый, но и некрасивый.
- Сама настройка D2D (фабрика + WIC-фабрика + render target) — 20–29 мс. Из них заметная часть — `CoCreateInstance`
  WIC. Для первого кадра WIC не нужен; изображения можно декодировать позже.

### 2.5 Размер exe и Defender [замер]

| Эксперимент | Медиана до `main` |
|---|---:|
| `conlab.exe` NativeAOT, 1,3 МБ | 51 мс |
| тот же exe + 55 МБ случайных байт в конце файла (не отображаются в память) | **182 мс (+131)** |
| тот же exe + 55 МБ нулей | 173 мс (+122) |
| WinForms NativeAOT, 56,7 МБ | 199 мс |

Каждый запуск стоит одинаково: 11 из 11 прогонов, плюс 10 последовательных запусков дают 164–181 мс против 42–44 мс.
CPU-время job'а при этом не растёт, то есть работа идёт вне процесса. Наиболее вероятный виновник — Defender
real-time scan exe при создании процесса. Smart App Control выключен (`VerifiedAndReputablePolicyState=0`).

DLL ведёт себя иначе: та же +55 МБ «нагрузка» стоит 35–55 мс **только при первой загрузке**, дальше 0,4–0,7 мс
(`results/dllload.txt`).

Практические следствия:

- **Держать exe маленьким**, выносить редко нужное в DLL, загружаемые позже.
- Не делать single-file-бандлов на десятки мегабайт.
- Цифра `cpu_ms` у харнесса эту стоимость не видит, а `content_ms` видит.
- **Открытый вопрос:** снимает ли подпись кода (Authenticode с репутацией) пересканирование exe на каждом запуске.
  Проверить нечем: сертификата нет.

### 2.6 Бюджет «пули»

При 90 мс пола [источник: калибровка `bench/results/calib-baseline.json`] для нативного Win32-ридера на этой машине
[оценка]:

- CreateProcess → `main`: ~35–45 мс (Defender, Windhawk);
- окно без активации: +15–20 мс;
- чтение файла и парсинг первого экрана md4c: 1–3 мс;
- D2D на WARP (девайс 4 мс, DComp/swapchain 3–5 мс) и DirectWrite-раскладка видимых блоков: 3–10 мс;
- первая отрисовка: 5–15 мс, плюс ~30 мс, если в первом экране есть цветные эмодзи;
- `Present` + `DwmFlush`: ≤ 1 кадр.

**Итого на простое ~110–150 мс**, то есть пол + 20–60 мс. Любой стек с аппаратным GPU на критическом пути не может
быть быстрее ~300 мс на этой машине. Стек с управляемым рантаймом и XAML — ~500+ мс.

---

## 3. WinUI 3 / Windows App SDK

### 3.1 Состояние на сентябрь 2026 [источник + инспекция]

- **Версии.** Ветка 1.8 обслуживается (Foundation 1.8.260803002 — август 2026). С 2.0 проект перешёл на
  SemVer: 2.0.1 (29.04.2026), 2.1.3 (21.05), 2.2.0 (09.06), 2.3.1 (16.07), 2.4.0 (13.08), 2.5.1 (16.09.2026).
  Имя семейства пакетов теперь содержит только major: `Microsoft.WindowsAppRuntime.2`.
  Источник: <https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0>.
- **Производительность в 2.3.1.** Появились opt-in оптимизации XAML через `XamlOptionalChanges` / `XamlChangeId`:
  `DefaultStyleOptimizations`, `OptimizeApplyStyles`, `Xaml_FasterResourceDictionary`,
  `Xaml_ActivationFactoryFastPaths`, `Xaml_FasterXbfIntLoading` и др. Из startup-пути убрана зависимость от
  `version.dll`. Снижена конкуренция за D3D lock при фоновом создании девайса.
  Отсюда же видно, что **WinUI создаёт D3D-девайс в фоне**, но первый кадр всё равно его ждёт.
- **Курс Microsoft.** Windows Insider Blog, 2026-03-20: «moving core Windows experiences to WinUI3».
  Discussion microsoft-ui-xaml#11096 (2026-05-11): File Explorer и Notepad выбраны эталонами launch time.
  Для WinUI-части запуска Explorer заявлено −41 % аллокаций, −45 % вызовов функций, **−25 % времени в коде WinUI**.
  Сколько это даёт полному запуску, не сказано: в комментариях об этом прямо спрашивают.
  Ссылки: <https://github.com/microsoft/microsoft-ui-xaml/discussions/11096>,
  <https://blogs.windows.com/windows-insider/2026/03/20/our-commitment-to-windows-quality/>.
- **Установлено на этой машине** [инспекция]: рантаймы 1.4–1.8, **2.2–2.5.1**, а также
  `Microsoft.WindowsAppRuntime.CBS.1.8`. CBS-пакет — это inbox-копия, через которую работает WinUI-часть File Explorer.
- **Кто уже на 2.x** [инспекция]:
  - Notepad 11.2607.14.0 зависит от `Microsoft.WindowsAppRuntime.2 ≥ 2.1.3`.
  - PowerToys 0.100.2: Windows App SDK 2.0.1, .NET 10.0.9 self-contained, `Microsoft.ui.xaml.dll` 3.2.0.

### 3.2 Анатомия запуска unpackaged-приложения

| Этап | Стоимость | Основание |
|---|---|---|
| CreateProcess → `main` | ~35–45 мс (C++), ~70 мс (C# JIT), ~50 мс (C# NativeAOT) | [замер] §2.1 |
| `MddBootstrapInitialize2` (Dynamic Dependencies, добавление framework-пакета в package graph) | **3–9 мс** типично (1.8: 3,5–5 мс; 2.x: 3,1–4 мс в тихий момент; до 20–60 мс под нагрузкой). Дочернего процесса DDLM на 26200 нет | [замер] `wasdk/bootprobe.cpp`, `results/bootprobe.txt` |
| `LoadLibrary("Microsoft.UI.Xaml.dll")` (14,6 МБ) | **6–15 мс** | [замер] |
| `RoGetActivationFactory("Microsoft.UI.Xaml.Application")` | **13–29 мс** | [замер] |
| `Application.Start`, парсинг `XamlControlsResources` (generic/themeresources XBF), стили | десятки мс, зависит от числа элементов; Microsoft: «rough benchmark ~1 ms на элемент» | [источник] Learn: <https://learn.microsoft.com/en-us/windows/apps/develop/performance/app-startup-performance> (ms.date 2026-03-16) |
| Создание HWND окна с GDI redirection surface (лишние ~7,9 МБ на 1080p в DWM) | мало по времени, но окно появляется пустым | [источник] microsoft-ui-xaml#11523 (2026-08-13) |
| **D3D11-девайс** для композиции и рендера | **~185 мс на этой машине (NVIDIA)**, в фоне, но первый кадр его ждёт | [замер] §2.2; [источник] release notes 2.3.1 |
| Layout, render, первый кадр; **окно показано раньше кадра** — вспышка и пустая рамка | +1–3 кадра | [источник] microsoft-ui-xaml#11677 (2026-08-27): «Every WinUI 3 app flickers on launch today» |

**[Оценка] для этой машины (тёплый старт, простая страница с RichTextBlock):**

| Вариант | Оценка |
|---|---|
| C++/WinRT | ~350–600 мс |
| C# NativeAOT | ~400–700 мс |
| C# JIT (framework-dependent) | ~600–1200 мс |
| Холодный старт | +100–400 мс: ~50 МБ «UI core» DLL рантайма 2.x и .NET, если он есть |

Точные цифры даст прототип `bench/protos/winui3` (C# .NET 8, WinAppSDK 1.8, unpackaged, NativeAOT).

**Packaged (MSIX) или unpackaged.**

- Packaged: не нужен бутстраппер, framework-пакет разрешает ОС, есть identity (share target, уведомления,
  file association через манифест). Активация идёт через AppX-активацию (COM/broker).
- Unpackaged: бутстраппер стоит всего 3–9 мс [замер].
- Опубликованных цифр накладных расходов MSIX-активации найти не удалось. Замерить здесь нельзя: Developer Mode
  выключен, прав администратора нет. **Открытый вопрос.**

**Self-contained или framework-dependent.**

- Framework-dependent требует установленного рантайма нужной major/minor-версии (установщик или Store-зависимость).
- Self-contained кладёт DLL рядом с exe. Пакет рантайма 2.5.1 весит **115,8 МБ / 301 файл** [инспекция], из них
  «UI core» — ~47,5 МБ в 24 DLL (`Microsoft.ui.xaml.dll` 14,6 МБ, `Microsoft.UI.Xaml.Controls.dll` 6,6 МБ,
  `dcompi.dll`, `dwmcorei.dll`, `DWriteCore.dll` 3,1 МБ, `WinUIEdit.dll` 3,3 МБ…), плюс AI/ML (onnxruntime 20,7 МБ,
  DirectML 17,8 МБ), которые можно исключить.
- Defender сканирует DLL один раз, дальше результат кэшируется (§2.5), поэтому на тёплом старте разница мала.

**NativeAOT.**

- C# (CsWinRT 2.x): официально с WinAppSDK 1.6. Microsoft сообщает о «50% reduction in start time» на
  примере Contoso Camera и о ~8× меньшем пакете при framework-зависимости:
  <https://blogs.windows.com/windowsdeveloper/2024/09/04/whats-new-in-windows-app-sdk-1-6/>.
- CsWinRT 3.0 — переписан под .NET 10 и AOT. Пока preview (`3.0.0-preview.260319.2`, март 2026, нужен .NET ≥ 10.0.5).
- C++/WinRT вообще не несёт управляемого рантайма, но разработка на нём заметно медленнее.

### 3.3 Markdown в WinUI

- **`RichTextBlock`** умеет: `Paragraph`, `Run`, `Bold`, `Italic`, `Underline`, `Span`, `Hyperlink`, `LineBreak`,
  `InlineUIContainer`, `TextDecorations` (включая strikethrough), `TextHighlighters`,
  `IsTextSelectionEnabled` (выделение по всему блоку), `RichTextBlockOverflow`.
  Ограничения:
  - **Нет таблиц** и фона блока: code block и таблица делаются `Border`/`Grid` внутри `InlineUIContainer`, а такой
    контент **не участвует в текстовом выделении и копировании**.
  - **Нет виртуализации.** 3,7 МБ `large.md` целиком в одном RichTextBlock — это секунды раскладки. Разбиение на
    блоки в `ItemsRepeater` ломает выделение через границы блоков.
- **CommunityToolkit Labs `MarkdownTextBlock`** [источник + инспекция]:
  - Последняя сборка `0.1.251217-build.2433` (декабрь 2025), стабильного релиза нет. Трекер «Road to 8.3 stable» #643
    открыт с февраля 2025.
  - Внутри Markdig. Всё рендерится в **один `RichTextBlock`**, таблицы — через `MyTableUIElement` в
    `InlineUIContainer` (исходники `components/MarkdownTextBlock/src/TextElements`).
  - Открытые проблемы: утечки памяти (#759, #772), крэш под AOT (#780, закрыт 2026-08-15), ничего о подсветке
    синтаксиса «из коробки».
  - Используется в PowerToys 0.100 (Settings, QuickAccess, KeyboardManagerEditor, PowerDisplay), но не для
    превью файлов.
  - Ссылка: <https://github.com/CommunityToolkit/Labs-Windows/issues/606>.
- **WebView2 внутри WinUI.** Максимальная fidelity (HTML/CSS), но вместе с ней приходит вся стоимость WebView2:
  см. отчёт `02-web-and-html-engines.md`.

### 3.4 Стиль

Сильная сторона WinUI:

- Fluent-контролы, Mica/Acrylic (в 2.0 добавлен `SystemBackdropElement` с `CornerRadius`), кастомный title bar
  (`TitleBar` с умными drag-регионами в 2.1.3), тёмная и светлая тема «бесплатно».
- Типографика: DirectWrite / DWriteCore, Segoe UI Variable, цветные шрифты, OpenType-свойства `Typography`.

Для ридера всё это нужно только в «хроме» — поиске, настройках, меню. Сам документ — это текст.

### 3.5 Риски WinUI 3

1. Аппаратный D3D всегда на критическом пути: публичного software/WARP-переключателя нет. В `Microsoft.UI.Xaml.dll`
   есть строки `ForceWarp`/`SoftwareRendering`, но это внутренние имена, API нет.
2. Пустое окно перед первым кадром (#11677) и GDI redirection surface (#11523): оба в статусе предложений, не
   исправлены.
3. Toolchain: XAML-компилятор, CsWinRT, AOT-предупреждения, сборка медленная. Это типичные жалобы в #11096.
4. Жалобы на лаги при ресайзе и таскании окна (#11096) для ридера некритичны.
5. Производительность улучшается, но главным образом для крупных приложений (Explorer): для hello-world-класса выигрыш
   ограничен налогом D3D и рантайма.

---

## 4. UWP / WinUI 2 и как сделаны inbox-приложения

- **UWP** — это CoreWindow и системный XAML (`Windows.UI.Xaml` в ОС). У UWP были настоящий системный splash screen,
  prelaunch и **отложенный показ окна до первого кадра** (`NotifyFirstFrameDrawn`, см. #11677). Этих механизмов у
  WinUI 3 пока нет.
- **UWP на современном .NET с NativeAOT — GA** и шаблон по умолчанию в VS 2026 (документ от 2026-01-26). По
  заявлению Microsoft, запуск «~5% improvement» относительно .NET Native, пакет примерно на 4 МБ больше:
  <https://learn.microsoft.com/en-us/windows/uwp/dotnet-native/modernize-uwp-apps-with-dotnet>.
- Для нового десктопного ридера UWP не подходит: песочница AppContainer, доступ к произвольным файлам и
  относительным картинкам затруднён, стратегическое направление — WinAppSDK.

Как устроены inbox-приложения:

| Приложение | Стек | Основание |
|---|---|---|
| **Notepad 11.2607.14.0** | Win32-хост `Notepad.exe` (3,3 МБ, импортирует `Microsoft.UI.Windowing.Core.dll`). UI-хром — **WinUI 3 через XAML Islands** (`NotepadXamlUI.dll` 6,4 МБ, C++/WinRT, `DesktopWindowXamlSource`, `XamlIslandsManager`, 70+ `.xbf`). **Документ — своя копия Office RichEdit** `riched20.dll` 4,0 МБ (импортирует `d2d1`, `DWrite`, `msptls.dll` = Page/Table Layout Services, `MSO*.dll`), windowless-хост (`RichEditHostMessageFilter`, `RichEditOleCallback`…). **Парсер markdown: cmark-gfm 0.29.0.gfm.13**, статически в `Notepad.exe`. Пакет зависит от `Microsoft.WindowsAppRuntime.2 ≥ 2.1.3` | [инспекция] dumpbin + строки + AppxManifest |
| Notepad: история markdown | 11.2504.50.0 (май 2025): bold/italic/ссылки/списки/заголовки; 11.2510: таблицы; 11.2512.10.0 (янв. 2026): strikethrough, вложенные списки; далее — картинки | [источник] <https://blogs.windows.com/windows-insider/2025/05/30/text-formatting-in-notepad-begin-rolling-out-to-windows-insiders/>, <https://blogs.windows.com/windows-insider/2026/01/21/notepad-and-paint-updates-begin-rolling-out-to-windows-insiders/> |
| Calculator | классический UWP + **.NET Native** (`UseDotNetNativeToolchain`) + WinUI 2.8.7, коммиты в августе 2026 | [источник] github.com/microsoft/calculator `src/Calculator/Calculator.csproj` |
| Windows Terminal | WinUI 2.8.4 в XAML Islands. Текст — **AtlasEngine**: DirectWrite/D2D только растеризуют глифы, размещение и блендинг — D3D11 + HLSL, атлас глифов | [источник] <https://github.com/microsoft/terminal/pull/11623> |
| File Explorer | WinUI 3 через inbox-пакет `WindowsAppRuntime.CBS` (command bar, Home, адресная строка), остальное — классический Win32/DirectUI | [инспекция] CBS-пакет; [источник] #11096 |

**Урок Notepad для FastMD.** Microsoft для «быстрого» inbox-редактора с markdown не рендерит документ XAML-ом.
Документ — это RichEdit (DirectWrite/D2D, выделение, копирование, таблицы, инкрементальная раскладка), а XAML
отвечает только за хром. Это прямой аргумент за «нативное ядро документа + опциональный XAML-хром».

---

## 5. WPF (.NET 8/9/10)

- **Замер** (`dotnet/wpf`, окно + `ScrollViewer` + `TextBlock` 2,5 КБ, без XAML-файлов, .NET 8.0.30):

  | Вариант | Медиана | Разбивка |
  |---|---:|---|
  | R2R | **672 мс** | main 103 → `Application` 147 → `Window` 232 → `ContentRendered` 648 (+415 мс) |
  | R2R + `RenderOptions.ProcessRenderMode = SoftwareOnly` | 632 мс | NVIDIA DLL всё равно грузятся (§ниже) |
  | JIT (более шумный прогон `runner/wpf.json`) | 822 мс | с `SoftwareOnly` — 907 мс |

  Модули в процессе при `SoftwareOnly`: `d3d9.dll`, `nvd3dumx.dll`, `nvgpucomp64.dll`, `nvldumdx.dll`…,
  `RenderCapability.Tier = 2`. WPF создаёт D3D9-девайс для определения возможностей даже в software-режиме,
  так что программный режим налог NVIDIA не снимает.
- Черновые прогоны прототипа `bench/protos/wpf` с FlowDocument и markdown (не финальные, машина нагружена):
  700–1270 мс.
- **NativeAOT нет.** dotnet/wpf#3811 «WPF is not trim-compatible» открыт с 2020 года, milestone **Future**,
  последняя активность — ноябрь 2025. Частичный trimming — обходной путь сообщества.
  Ссылка: <https://github.com/dotnet/wpf/issues/3811>.
- **ReadyToRun** есть. На этой машине R2R дал небольшой выигрыш: доминирует не JIT, а инициализация
  WPF/milcore/D3D9.
- **Контент.** `FlowDocument` — самая богатая нативная модель документа в .NET: `Table`, `List`, `Section`,
  `BlockUIContainer`, `Figure`/`Floater`, выделение и копирование через весь документ, печать.
  Минусы: `FlowDocumentScrollViewer` раскладывает документ целиком (для 3,7 МБ это тяжело), у таблиц есть
  особенности (dotnet/wpf#9166).
  Рендереры: Markdig.Wpf (таблицы pipe/grid поддерживаются частично), MdXaml, Markdig.FlowDocument.
- **Markdown Monster** (Rick Strahl) — WPF-оболочка + **WebView2** для превью, по одному WebView2 на документ.
  Автор много писал о проблемах асинхронной инициализации и мерцании:
  <https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization>.
- **Стиль.** В .NET 9 появилась Fluent-тема (Windows 11). Есть WPF UI (lepoco). Mica доступна через
  `DwmSetWindowAttribute`. Типографика хорошая (OpenType через `Typography.*`), но стек рендеринга текста у WPF свой.
- **Вердикт:** хорош как «IDE-подобный» редактор, но для «пули» не подходит: ~0,6–1 с на этой машине и нет AOT.

---

## 6. WinForms

- **Замер** (`dotnet/wf`, `Form` + `TextRenderer.DrawText`):

  | Вариант | Медиана | Комментарий |
  |---|---:|---|
  | JIT | 248 мс | |
  | self-contained R2R | 243 мс | |
  | **NativeAOT** (`_SuppressWinFormsTrimError`) | **305 мс** | exe **56,7 МБ**: trimming не работает, в сборку попадают WPF-нативы `wpfgfx_cor3`, `PresentationNative_cor3`, `D3DCompiler_47_cor3`. `main` на 199 мс из-за скана exe (§2.5) |
  | Для сравнения: C + GDI с тем же текстом | 119 мс | |

- **Статус NativeAOT.** Официально не поддерживается в .NET 8/9/10: WinForms опирается на built-in COM
  (dotnet/winforms#4649, dotnet/sdk#34129). Есть сторонний WinFormsComInterop.
- **Контент.** `RichTextBox` (RichEdit через msftedit), WebView2 или своя GDI+-отрисовка.
- **Вердикт:** выигрыша над голым Win32 нет. Уместен только как быстрый хост для RichEdit или WebView2, если
  команда пишет на C#.

---

## 7. .NET MAUI

- На Windows MAUI — это WinUI 3 + слой handlers MAUI, поэтому запуск по определению не быстрее WinUI C#.
- **NativeAOT на Windows из коробки не работает.** dotnet/maui#31227 открыт в Backlog: «NativeAOT only worked with
  packaged apps», нужны ручные правки. Ссылка: <https://github.com/dotnet/maui/issues/31227>.
- Версии: 10.0.101 stable, 11.0.0-rc.1 [NuGet].
- Для сравнения [источник]: Uno.Chefs на Windows — самый медленный из всех платформ (1,605 с), см. §9.
- **Вердикт:** не рассматривать. Кроссплатформенность нужна мобилкам, а не md-ридеру для Windows.

---

## 8. Avalonia 11/12

- **Версии.** Avalonia 12.0 вышла 2026-04-07: .NET 10, SkiaSharp 3, бэкенд Direct2D удалён, только Skia.
  Сейчас 12.1.2.
  Ссылки: <https://avaloniaui.net/blog/avalonia-12>,
  <https://docs.avaloniaui.net/docs/avalonia12-breaking-changes>.
- **NativeAOT поддерживается**: `IsAotCompatible`, compiled bindings; флаг `BuiltInComInteropSupport=false` нужен
  только до 12.0.
  Опубликованные цифры есть только для Android: **1960 → 460 мс** с NativeAOT. Для Windows desktop официальных
  цифр не найдено.
- **Рендер на Windows.** По умолчанию `Win32PlatformOptions.RenderingMode = [AngleEgl, Software]` и
  `CompositionMode = [WinUIComposition, DirectComposition, RedirectionSurface]`
  (<https://api-docs.avaloniaui.net/docs/P_Avalonia_Win32PlatformOptions_RenderingMode>).
  ANGLE работает поверх D3D11, так что **по умолчанию Avalonia платит налог NVIDIA** (§2.2).
  **Гипотеза для прототипа `avalonia`:** `RenderingMode = [Software]` (Skia CPU-растр) +
  `CompositionMode = [RedirectionSurface]` или DirectComposition должны убрать 180–250 мс. Цена — нагрузка на CPU
  при скролле больших окон.
- **Текст.** HarfBuzz + Skia, а не DirectWrite. Нет ClearType, свой fallback. Текст выглядит «почти как нативный»,
  но не идентично Windows. Сравнивать скриншоты харнесса.
- **Markdown.**
  - Markdown.Avalonia 11.0.3 (май 2026); ветка 12.0.0-a3 — alpha от апреля 2026. Строит `Grid`/`TextBlock`/`Border`,
    подсветка через AvaloniaEdit (тяжёлая), выделение — в пределах отдельных `SelectableTextBlock`.
  - LiveMarkdown.Avalonia ориентирован на стриминг LLM.
  - Есть коммерческий компонент Markdown в Avalonia Accelerate.
- **Размер** [оценка]: NativeAOT exe + `libSkiaSharp` + `libHarfBuzzSharp` + ANGLE `av_libglesv2` — десятки МБ.
  Есть проекты статической линковки (avalonia-static). Большой exe дорог на каждом запуске (§2.5).
- **Кроссплатформенность:** настоящая (Windows/macOS/Linux) — **главный довод за Avalonia**.
- **Вердикт:** лучший из .NET-вариантов, если нужен macOS/Linux. Для Windows-only «пули» проигрывает нативу на
  рантайме (~50–70 мс до `main`), на Skia-инициализации и на GPU.

---

## 9. Uno Platform

- 6.7.30 stable, 7.0 в dev.
- С Uno 6.0 на Windows есть собственный **Win32-хост со Skia** без зависимости от WPF. Self-contained стал меньше
  на ~100 МБ.
- NativeAOT для всех desktop-таргетов — с 6.6 (2026-07-29). Опубликованные цифры на полноценном приложении
  Uno.Chefs (.NET 10), без минимального сэмпла [источник]:

  | Платформа | Default runtime | NativeAOT |
  |---|---:|---:|
  | Windows | 1,605 с | **0,824 с** (−49 %) |
  | Linux | 0,870 с | 0,350 с |
  | Android | 0,895 с | 0,348 с |

  Источник: <https://platform.uno/blog/uno-platform-6-6/>.
- Рендер на Windows: Skia + OpenGL по умолчанию, Vulkan опционально. Значит, GPU-драйвер на критическом пути.
- Плюс: API совместим с WinUI, код можно разделять с WinUI-хромом.
- **Вердикт:** тяжёлый стек, для ридера избыточен.

---

## 10. Голый Win32 + DirectWrite / Direct2D (+ DirectComposition)

### 10.1 Что даёт платформа

- **DirectWrite.**
  - `IDWriteTextLayout`: форматирование по диапазонам (шрифт, насыщенность, стиль, размер, подчёркивание,
    зачёркивание, drawing effects для цвета).
  - `IDWriteInlineObject`: картинки, «таблетки» inline code, чекбоксы task list.
  - Hit-testing (`HitTestPoint`, `HitTestTextRange`) для выделения.
  - Системный fallback, цветные шрифты, OpenType-фичи, вариативные оси (`DWRITE_FONT_AXIS_VALUE`).
  - Кэш шрифтов системный: раскладка 2 КБ текста — 1–3 мс [замер].
- **Direct2D.** Скруглённые прямоугольники (code block radius 6, inline code radius 4), линии таблиц, битмапы через
  WIC. Режимы: HwndRenderTarget (hardware или software) либо D2D device context поверх DXGI swapchain.
- **DirectComposition + flip-model swapchain.** Нет redirection bitmap (`WS_EX_NOREDIRECTIONBITMAP`), визуальные
  слои для плавного скролла.
- **Стиль Windows 11** без фреймворка: `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)` для Mica,
  `DWMWA_USE_IMMERSIVE_DARK_MODE`, `DWMWA_WINDOW_CORNER_PREFERENCE`. Кастомный title bar — через
  `DwmExtendFrameIntoClientArea` и `WM_NCHITTEST`.

### 10.2 Что делают «мгновенные» приложения

| Приложение | Стек | Что взять для FastMD |
|---|---|---|
| **Windows Terminal (AtlasEngine)** | DirectWrite/D2D только растеризуют глифы, D3D11 + HLSL размещают их из атласа | атлас глифов для скролла; но у Terminal есть XAML Islands и аппаратный D3D, «мгновенным» он не является |
| **Notepad++** | Win32 + Scintilla; рендер через GDI или DirectWrite (`SCI_SETTECHNOLOGY`, опция в настройках) | маленький exe, никакого D3D-девайса на старте |
| **Sublime Text** | C++, свой UI-тулкит (skyline/px), растеризация Skia на CPU. OpenGL-ускорение появилось в 2021 году и **на Windows было выключено по умолчанию** (по состоянию блог-поста 2021 года); авторы столкнулись с «~8 separate driver bugs» | <https://www.sublimetext.com/blog/articles/hardware-accelerated-rendering>, <https://thume.ca/2016/12/03/disassembling-sublime-text/>. CPU-растр по умолчанию, GPU — опция |
| **File Pilot** | C, **собственный OpenGL-рендерер + IMGUI**, exe ~1,8 МБ. На момент бета-версии текст — только латиница и кириллица, без CJK | <https://news.ycombinator.com/item?id=43091466>. Мгновенность частично обусловлена отсутствием сложного Unicode |
| **RemedyBG** | Dear ImGui + D3D11, шейдеры зашиты в exe | <https://remedybg.handmade.network/> |
| **10x Editor** | свой GPU-рендерер (DX11) | <https://10xeditor.com/> |
| **nene-nib** (2026) | D3D11 + D2D + DComp + Mica, свой редактор | замерили налог NVIDIA 159 мс и решили **показывать окно до создания девайса** |

Общий паттерн: один маленький exe, никакого рантайма, свой рендерер и своя текстовая раскладка. GPU либо не
используется на старте (Sublime, Notepad++), либо его цену принимают или прячут за ранним показом окна.
**Готового решения «нативный markdown с GFM-таблицами и выделением» среди них нет** — это работа FastMD.

### 10.3 Что придётся написать самим (трудоёмкость — высокая)

- **Парсер.** md4c (C, CommonMark + GFM-таблицы, strikethrough, task lists, SAX-колбэки) или cmark-gfm, как у Notepad.
- **Блочная раскладка.** Заголовки, абзацы, вложенные списки с маркерами, цитаты с полосой, code block с фоном и
  горизонтальным скроллом или переносом, таблицы с выравниванием колонок, `<hr>`, картинки (WIC, асинхронно).
- **Виртуализация** под `large.md`: 3,7 МБ, 97 тыс. строк. Сначала раскладываются блоки первого экрана, остальное —
  лениво или в фоне. Протокол это явно разрешает.
- **Выделение через блоки и копирование** (plain text и, желательно, HTML/RTF), поиск Ctrl+F с IME, ссылки,
  контекстное меню, зум, тёмная тема.
- **Доступность.** UI Automation provider с `ITextProvider` для экранных дикторов — крупная и часто забываемая
  работа. WinUI, WPF и RichEdit дают её бесплатно.
- **DPI.** Per-monitor v2, `WM_DPICHANGED`, пересоздание ресурсов.

Замеренный потенциал: **92–150 мс** на нагруженной машине и **~110–150 мс** [оценка] на простое. Больше этого
не даёт ни один другой класс решений.

---

## 11. RichEdit как «нативная середина»

Подробности — в прототипе `cpp-richedit`, здесь только позиция в сравнении.

- `msftedit.dll` (inbox) или Office RichEdit (как в Notepad): DirectWrite/D2D-рендер (режим D2D в RichEdit 8+),
  таблицы, гиперссылки, картинки (`ITextRange2::InsertImage`), выделение и копирование, IME, UIA — всё
  «бесплатно», инкрементальная раскладка.
- Минусы: стилизация ограничена моделью RTF/TOM. Скруглённые фоны code block, полосы цитат и зебра таблиц «как на
  GitHub» делаются трудно или никак, поэтому стиль получается «офисный».
- Черновые прогоны `cpp-richedit` (не финальные, под нагрузкой): 256–389 мс при `baseline` 120–194 мс в тех же
  прогонах.
- `WinUIEdit.dll` (3,3 МБ) в рантайме WinAppSDK — по имени и размеру это движок `RichEditBox` в WinUI, то есть та же
  технологическая линия RichEdit [инспекция: имя и размер файла, дизассемблирование не проводилось].

---

## 12. Preview handlers Explorer и PowerToys Peek [инспекция]

- **Встроенного preview handler для `.md` в Windows нет.** На этой машине в
  `HKCR\.md\shellex\{8895b1c6-b41f-4c1c-a562-0d564250836f}` зарегистрирован
  `{60789D87-9C3C-44AF-B18C-3DE2C2820ED3}` «Markdown Preview Handler» из PowerToys 0.100.2.
- **PowerToys Markdown preview handler** устроен в три слоя:
  - COM-шим `PowerToys.MarkdownPreviewHandlerCpp.dll` (C++, грузится в prevhost);
  - он запускает **отдельный .NET 10 процесс** `PowerToys.MarkdownPreviewHandler.exe` (WinForms + **WebView2**
    1.0.3719.77 + **Markdig** 0.34, `NavigateToString`) и встраивает его окно;
  - на каждое превью платится запуск .NET и WebView2-окружения.
- **PowerToys Peek** — WinUI 3 (WinAppSDK 2.0.1, .NET 10). Markdown показывается через `WebBrowserPreviewer`
  (WebView2 + `MarkdownHelper` → HTML). **Peek — резидентный процесс:** модуль запускает `PowerToys.Peek.UI.exe`
  при включении (`enable()` → `launch_process()`, `src/modules/peek/peek/dllmain.cpp`), а горячая клавиша только
  сигналит уже живому процессу. На этой машине Peek выключен, поэтому скорость показа не замерялась.
- **Вывод.** Даже Microsoft для «мгновенного» превью в WinUI/WebView2 держит процесс резидентным. Для FastMD это
  аргумент в пользу опционального `-resident` варианта (трей), но основной путь должен быть быстрым и без него.

---

## 13. Сводная таблица

Времена — **тёплый старт до первого экрана документа на этой машине (RTX 5070)**. «Замер» — лабораторный пол
стека без markdown под нагрузкой, «оценка» — с учётом типичной markdown-страницы. Итоговые цифры даст серийный
прогон прототипов.

| Стек | Время до контента, тёплый (холодный) | Основание | GFM-fidelity: таблицы / код / картинки / выделение | Стиль (Fluent, Mica, типографика) | Трудоёмкость | Размер дистрибутива | Кроссплатформенность | Главные риски |
|---|---|---|---|---|---|---|---|---|
| **Win32 + D2D (WARP/software) + DirectWrite + md4c** | **~110–150 мс** (≈ так же) | замер: пол 92–151 мс | всё возможно, всё своими руками; выделение и копирование — своё | полный контроль; Mica через DWM; лучшая типографика DirectWrite | **высокая** | **0,3–3 МБ**, один exe | нет (ядро раскладки можно абстрагировать) | объём своей раскладки, UIA/доступность, скролл 4K на CPU |
| Win32 + D2D **hardware** | ~330–460 мс | замер: 358–459 мс | то же | то же | высокая | то же | нет | налог NVIDIA 180–250 мс |
| Win32 + GDI | ~90–120 мс | замер: 92–119 мс | таблицы и код вручную, **без цветных эмодзи**, хуже сглаживание | устаревший вид | высокая | < 1 МБ | нет | «некрасиво» |
| Win32 + RichEdit (путь Notepad) | ~150–300 мс | черновые прогоны `cpp-richedit` | таблицы, картинки, выделение, IME, UIA — «из коробки» | «офисный», GitHub-стиль труднодостижим | средняя | < 1 МБ + inbox `msftedit` | нет | ограничения стилизации |
| WinUI 3, C++/WinRT, unpackaged | ~350–600 мс (+100–300) | оценка; бутстрап и загрузка DLL замерены | RichTextBlock: таблицы только UIElement, без выделения в них, без виртуализации | **лучший Fluent**: Mica, TitleBar, темы | высокая (C++/WinRT, XAML) | 2–5 МБ + рантайм (FD) или +50–120 МБ (SC) | нет (Uno — частично) | D3D на пути, окно раньше кадра, toolchain |
| WinUI 3, C# NativeAOT | ~400–700 мс (+150–400) | оценка; см. `bench/protos/winui3` | то же + Labs MarkdownTextBlock (preview) | то же | средняя | ~15–40 МБ + рантайм | нет | AOT-совместимость библиотек, preview-контролы |
| WPF (.NET 8/9/10, R2R) | **~630–900 мс** (+200–500) | замер: 632–672 мс для TextBlock | FlowDocument: таблицы, выделение, печать; без виртуализации | Fluent-тема .NET 9, WPF UI | низкая–средняя | 1,6 МБ FD + .NET Desktop runtime / ~160 МБ SC | нет | нет AOT, D3D9 + NVIDIA, медленно |
| WinForms (.NET) | ~240–310 мс (+100–300) | замер: 243–305 мс | RichTextBox или WebView2 | устаревший | низкая | 1 МБ FD / 57 МБ AOT (плохо) | нет | AOT не поддержан |
| .NET MAUI (Windows) | ≥ WinUI C# JIT, ~0,8–2 с | оценка | как WinUI | Fluent через WinUI | средняя | 100+ МБ | да (mobile/mac) | нет AOT на Windows, лишний слой |
| Avalonia 12 NativeAOT | ~300–600 мс (ANGLE); **~200–350 мс в Software** — гипотеза | оценка; см. `bench/protos/avalonia` | Markdown.Avalonia: таблицы и код, выделение по блокам | свой Fluent-тема, не нативный текст (HarfBuzz/Skia) | средняя | ~30–60 МБ | **да** | GPU по умолчанию, большой exe, alpha-версии md-пакетов |
| Uno Skia (Win32) NativeAOT | ~0,4–0,9 с | источник: Uno.Chefs 0,824 с (полное приложение) | WinUI-подобный API | Fluent-подобный | средняя | 50–100 МБ | да | тяжело, OpenGL на пути |

---

## 14. Выводы для FastMD

### 14.1 Рекомендация по стеку

1. **Ядро — C или C++ Win32 + DirectWrite + Direct2D, первый кадр без аппаратного GPU.**
   - Девайс D3D11 **WARP** (4 мс) + DXGI flip + DirectComposition, или D2D software HwndRenderTarget.
   - Если профилирование скролла 4K или больших документов покажет нехватку CPU, аппаратный девайс создаётся **после**
     первого кадра в фоне, и рендер переключается на него. Пользователь не ждёт 185 мс драйвера NVIDIA.
   - Отдельный вопрос — сосуществование фонового создания девайса с loader lock (§2.2): делать это после первого
     кадра, а не параллельно с ним.
2. **Окно показывается без активации**, активация — сразу после первого `Present`/`DwmFlush` (−25–60 мс, §2.3).
   Строка поиска и IME создаются лениво.
3. **Первый экран раньше всего остального.**
   - md4c парсит весь файл: парсинг — сотни МБ/с, так что это недорого. Раскладываются только блоки, видимые в
     первом экране. Остальное — в фоне или по скроллу; протокол это разрешает, это надо описать в README.
   - Картинки декодируются асинхронно (WIC), место под них резервируется.
   - Цветные эмодзи: атлас, либо монохром на первом кадре с дорисовкой (§2.4).
4. **Один маленький exe (< 3 МБ), без рантаймов.** Статическая CRT, без single-file-бандлов. Редко нужное — в DLL
   с поздней загрузкой (§2.5).
5. **XAML — только если нужен «богатый хром»** (настройки, диалоги), и только через XAML Islands **после** первого
   кадра, как в Notepad. Документ никогда не рендерится XAML-ом.
6. **Запасной путь с меньшей трудоёмкостью — RichEdit**, если прототип `cpp-richedit` покажет приемлемый стиль:
   так Microsoft и сделала Notepad.
7. **Не брать** WPF, WinForms, MAUI и Uno для основного продукта. **Avalonia** — только если кроссплатформенность
   станет требованием, и сначала измерить её в `RenderingMode=Software`.
8. **Резидентный режим** (трей-процесс, передача пути через `WM_COPYDATA` или named pipe) — отдельная честная опция
   `-resident`. Так делает PowerToys Peek. Базовый путь должен быть быстрым и без неё.

Ожидаемое время: **~110–150 мс до первого экрана на простое** этой машины. Это в 1,2–1,7 раза медленнее пустого
окна-эталона и в 4–8 раз быстрее WPF или WinUI-C# в той же конфигурации.

### 14.2 Риски

| Риск | Серьёзность | Смягчение |
|---|---|---|
| Трудоёмкость своей раскладки (таблицы, вложенные списки, выделение через блоки, копирование HTML) | высокая | инкрементально: сначала визуал первого экрана, потом выделение; md4c для структуры; при провале — RichEdit-путь |
| Доступность (UIA `ITextProvider`, экранные дикторы) | средняя–высокая | заложить UIA-провайдер в архитектуру с самого начала |
| Скролл больших документов на WARP/CPU при 4K и 144 Гц | средняя | атлас глифов + DComp-слои; переход на аппаратный девайс после первого кадра |
| Цветные эмодзи (~30 мс на первый кадр) | низкая | атлас или отложенная перерисовка |
| IME/TSF в строке поиска и отложенная активация (права foreground) | низкая | ленивое создание; проверить запуск из Explorer, не только из харнесса |
| Defender пересканирует exe на каждом запуске | средняя | маленький exe; проверить эффект подписи кода (открытый вопрос) |
| Разнообразие машин: другие GPU, iGPU-only ноутбуки, RDP (WARP) | средняя | WARP-путь одинаков везде, это плюс; аппаратный путь — только опция |
| Сторонние инжекторы (Windhawk, оверлеи GPU) раздувают «пол» | внешняя | учитывать при сравнении; в отчёте для пользователя указать, что замер — на машине с Windhawk |

### 14.3 Замечания к харнессу и протоколу (для оркестратора, правок не делал)

1. **Windhawk 1.7.3** внедряет 4 модуля во все процессы, включая запущенные харнессом (§1). Пол 90 мс это включает.
   При желании можно разово замерить с приостановленным Windhawk — только с согласия пользователя.
2. **`cpu_ms` не видит время Defender** (оно вне процесса), а `content_ms` видит. Для больших exe (Electron, AOT-бандлы)
   разница с «чистым» CPU будет большой; стоит смотреть на `dist_mb` вместе с `content_ms`.
3. **Отложенная активация** (§2.3) — легитимная оптимизация или нарушение «shown activated»? Решить явно для всех.
   Связанная проблема харнесса: приложения, запущенные из фонового python, **не получают foreground**
   (`runner/fgcheck.py`). Путь активации (фокус, TSF) поэтому отличается от реального двойного клика в Explorer.
   Стоит хотя бы выборочно замерить запуск через `ShellExecuteEx` из foreground-процесса или через сам Explorer.
4. **Выбор GPU** (hardware / WARP / iGPU) влияет сильнее, чем выбор фреймворка. Рекомендую, чтобы каждый прототип
   указывал в `notes`, какой девайс он создаёт до первого кадра. Для прототипов с опцией software-рендера
   (Avalonia `Software`, Chromium `--disable-gpu` [гипотеза для web-отчёта]) полезен вариант с ней.
5. Багов в `bench.py` не найдено. Всё, что делал раннер лаборатории, — импорт `bench.py` без изменений.

### 14.4 Открытые вопросы

- Накладные расходы MSIX-активации против unpackaged: не замерено, нужен Developer Mode или сертификат.
- Влияние подписи Authenticode на пересканирование exe Defender'ом.
- Реальные цифры WinUI 3 (C++ и C# AOT) и Avalonia (Software и ANGLE) на этом стенде — ждём серийный прогон
  прототипов.
- Холодный старт после перезагрузки (нужны права администратора, чтобы чистить standby list).
- Есть ли у WinUI поддерживаемый способ рендера через WARP. Строки `ForceWarp`/`SoftwareRendering` в
  `Microsoft.UI.Xaml.dll` есть, публичного API нет.

---

## Приложение A. Воспроизведение (всё в `research/lab/native-ui/`)

| Файл | Что это |
|---|---|
| `runner/labrun.py` | раннер: импортирует `bench/harness/bench.py`, чередует запуски, пишет `runner/results/<config>.json` |
| `runner/final.json`, `w32.json`, `ime.json`, `dotnet.json`, `wpf.json`, `size.json` | конфигурации прогонов; результаты — в `runner/results/` |
| `w32/w32lab.cpp` → `w32/out/w32lab.exe` | Win32-лаборатория: `gdi \| d2d-hwnd \| d2d-hwnd-sw \| d3d-flip \| d3d-dcomp \| warp-dcomp \| igpu-dcomp` + флаги `--noime --noimc --lateact --notrans --latin --bgdevice --cloak` |
| `w32/d3dprobe.cpp` | стоимость создания DXGI/D3D11/D3D12-девайсов + список подгруженных модулей (`results/d3dprobe.txt`) |
| `w32/stackprobe.cpp` → `stackprobe2.exe` | сэмплирующий профайлер уровня модулей для `D3D11CreateDevice` (`results/stackprobe.txt`) |
| `w32/dwprobe.cpp` | DirectWrite/D2D: раскладка, первая и вторая отрисовка, цвет, fallback (`results/dwprobe.txt`) |
| `w32/dllload.c`, `w32/tinydll.c`, `w32/modlist.c` | скан DLL Defender'ом, список внедрённых модулей (Windhawk) |
| `wasdk/bootprobe.cpp` + `wasdk/pkgs/` (NuGet Foundation 1.8.260803002 и 2.3.12) | `MddBootstrapInitialize2`, загрузка `Microsoft.UI.Xaml.dll`, фабрика `Application` (`results/bootprobe.txt`) |
| `dotnet/con`, `dotnet/wf`, `dotnet/wpf` | .NET 8: консоль (JIT/AOT), WinForms (JIT/R2R/SC-R2R/AOT), WPF (JIT/R2R, hw/sw); публикации в `dotnet/out/` |
| `runner/defender_cpu.py` | exe + 55 МБ «хвоста»: 10 последовательных запусков (`results/defender_cpu.txt`) |
| `runner/fgcheck.py` | проверка, становится ли окно foreground после запуска из харнесса (`results/fgcheck.txt`) |

Сборка C/C++: `bench/tools/msvc.cmd cl /nologo /utf-8 /O2 /MT /EHsc /DUNICODE /D_UNICODE <file> /link ...`.
.NET AOT: `bench/tools/msvc.cmd dotnet publish ... -p:PublishAot=true`. Через `msvc.cmd`, потому что ILCompiler
ищет `vswhere.exe` в PATH.

Прогон: `python research/lab/native-ui/runner/labrun.py research/lab/native-ui/runner/final.json --runs 11`.

## Приложение B. Источники

- Windows App SDK 2.0 release notes (2.0.1 … 2.5.1, 2026): <https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0>
- WinUI 3 Performance: A Leap Forward, 2026-05-11: <https://github.com/microsoft/microsoft-ui-xaml/discussions/11096>
- Our commitment to Windows quality, 2026-03-20: <https://blogs.windows.com/windows-insider/2026/03/20/our-commitment-to-windows-quality/>
- WinUI: smooth launch / окно до первого кадра, #11677 (2026-08-27): <https://github.com/microsoft/microsoft-ui-xaml/issues/11677>
- WinUI: GDI redirection surface, #11523 (2026-08-13): <https://github.com/microsoft/microsoft-ui-xaml/issues/11523>
- WinUI startup best practices (ms.date 2026-03-16): <https://learn.microsoft.com/en-us/windows/apps/develop/performance/app-startup-performance>
- Why is WinUI so slow (2021, историческое): <https://github.com/microsoft/microsoft-ui-xaml/discussions/8595>
- XamlMetadataProvider startup blowup, #8281 (исправлено в 1.3.2): <https://github.com/microsoft/microsoft-ui-xaml/issues/8281>
- What's new in Windows App SDK 1.6 (NativeAOT, −50% старта): <https://blogs.windows.com/windowsdeveloper/2024/09/04/whats-new-in-windows-app-sdk-1-6/>
- CsWinRT releases (3.0 preview): <https://github.com/microsoft/CsWinRT/releases>
- Bootstrapper API: <https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/use-windows-app-sdk-run-time>
- CommunityToolkit Labs MarkdownTextBlock: <https://github.com/CommunityToolkit/Labs-Windows/issues/606>, #643, #750, #759, #772, #780
- UWP + modern .NET + NativeAOT GA (2026-01-26): <https://learn.microsoft.com/en-us/windows/uwp/dotnet-native/modernize-uwp-apps-with-dotnet>
- Notepad formatting (2025-05-30): <https://blogs.windows.com/windows-insider/2025/05/30/text-formatting-in-notepad-begin-rolling-out-to-windows-insiders/>
- Notepad 11.2512.10.0 (2026-01-21): <https://blogs.windows.com/windows-insider/2026/01/21/notepad-and-paint-updates-begin-rolling-out-to-windows-insiders/>
- WPF trimming/AOT, #3811: <https://github.com/dotnet/wpf/issues/3811>
- WinForms AOT: <https://github.com/dotnet/winforms/issues/4649>, <https://github.com/dotnet/sdk/issues/34129>
- MAUI Windows NativeAOT, #31227: <https://github.com/dotnet/maui/issues/31227>
- Avalonia 12 (2026-04-07): <https://avaloniaui.net/blog/avalonia-12>
- Avalonia Win32 RenderingMode: <https://api-docs.avaloniaui.net/docs/P_Avalonia_Win32PlatformOptions_RenderingMode>
- Avalonia NativeAOT: <https://docs.avaloniaui.net/docs/deployment/native-aot>
- Uno Platform 6.6 (2026-07-29): <https://platform.uno/blog/uno-platform-6-6/>
- Uno Skia desktop: <https://platform.uno/docs/articles/features/using-skia-desktop.html>
- Markdown Monster / WebView2: <https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization>
- Windows Terminal AtlasEngine, PR #11623: <https://github.com/microsoft/terminal/pull/11623>
- Sublime Text hardware acceleration (2021): <https://www.sublimetext.com/blog/articles/hardware-accelerated-rendering>
- Disassembling Sublime Text: <https://thume.ca/2016/12/03/disassembling-sublime-text/>
- File Pilot (HN, автор о стеке): <https://news.ycombinator.com/item?id=43091466>; <https://filepilot.tech/about>
- RemedyBG: <https://remedybg.handmade.network/>
- nene-nib — налог D3D11CreateDevice на NVIDIA (2026-09-16/17): <https://github.com/hideyukiMORI/nene-nib/issues/24>
- PowerToys Peek, резидентный запуск: `src/modules/peek/peek/dllmain.cpp` в <https://github.com/microsoft/PowerToys>
- Calculator (UWP + .NET Native + WinUI 2.8.7): <https://github.com/microsoft/calculator>
