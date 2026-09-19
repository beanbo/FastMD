# avalonia — C# .NET 8 + Avalonia 12 (Skia) + Markdig, NativeAOT

Прототип FastMD на Avalonia: Markdig AST → обычные контролы Avalonia (TextBlock/SelectableTextBlock с Inlines,
Border, Grid, Image), без XAML, без тем, без биндингов. Протокол — `bench/PROTOCOL.md` v1.

**Итог в одной строке:** лучшая честная конфигурация (`avalonia`) показывает содержимое за **~150–170 мс**
(медиана на small/medium/large, шумная машина), это ≈ 2× от пола `baseline-win32` (~85–90 мс).
Три рычага дали почти всё: **NativeAOT** (−195 мс против JIT/R2R), **software-рендер Skia вместо ANGLE/D3D11**
(−250 мс) и **маленький exe-лаунчер + NativeAOT DLL** (−15…45 мс, обычно ~35; из-за Defender, см. §7). Стоковый Avalonia
(JIT + FluentTheme + ANGLE) на том же коде — ~660 мс.

---

## 1. Стек и версии

| Компонент | Версия |
|---|---|
| .NET SDK / runtime | 8.0.424 / 8.0.30 (NativeAOT: Microsoft.DotNet.ILCompiler 8.0.30) |
| Avalonia (+ .Win32, .Skia, .HarfBuzz, .Themes.Fluent) | **12.1.2** (работает с net8.0; пакет содержит lib/net8.0) |
| SkiaSharp / HarfBuzzSharp | 3.119.4 / 8.3.1.3 |
| ANGLE (только для GPU-вариантов) | Avalonia.Angle.Windows.Natives 2.1.27548.20260419 (`av_libglesv2.dll`) |
| Markdig | 1.3.2 (pipe tables, task lists, strikethrough, autolinks) |
| Лаунчер | C, MSVC 14.44 (`/O1 /MT`), 100 KB |

NuGet-пакеты лежат в `bench/.tools/avalonia/packages` (см. `nuget.config`), глобально ничего не ставится.

## 2. Сборка

```
pwsh -File bench/protos/avalonia/build.ps1                 # варианты из proto.json: out/aot-lib, out/aot, out/jit-fluent (~1.5 мин)
pwsh -File bench/protos/avalonia/build.ps1 -Only aot-lib   # только лучший вариант (~25 с)
pwsh -File bench/protos/avalonia/build.ps1 -Only aot-fluent,jit,aot-size   # сборки "ручек", которые мерились при разработке
```

Каждая сборка идёт в свои `src/obj/<flavor>`, `src/bin/<flavor>`, `out/<flavor>` (чистятся перед сборкой).
Нужны .NET SDK 8 и VS 2022 C++ (линкер NativeAOT; лаунчер собирается через `bench/tools/msvc.cmd`).

| Папка | Что это |
|---|---|
| `out/aot-lib` | NativeAOT **shared library** `fastmd-core.dll` (13.8 MB) + лаунчер `fastmd-avalonia.exe` (100 KB) + `libSkiaSharp.dll`, `libHarfBuzzSharp.dll`, `av_libglesv2.dll` = 31.3 MB |
| `out/aot` | обычный NativeAOT `fastmd-avalonia.exe` (13.8 MB) + те же нативные DLL = 31.2 MB |
| `out/jit-fluent` | JIT + ReadyToRun, framework-dependent (нужен установленный .NET 8 runtime), FluentTheme = 37.5 MB |
| `out/aot-fluent`, `out/jit`, `out/aot-size` | только для замеров ручек (не в proto.json) |

## 3. Как устроен старт (`src/Program.cs`)

1. `Main` (или экспорт `fastmd_main` в DLL-сборке): `FastMdBench.Init()` → стартует поток `md-parse`
   (`File.ReadAllText` + `Markdig.Parse`) **параллельно** с инициализацией Avalonia на UI-потоке.
2. `AppBuilder.Configure<App>().UseWin32().UseSkia().UseHarfBuzz()` + `Win32PlatformOptions.RenderingMode`
   (`--render software|angle|wgl|vulkan`), classic desktop lifetime.
3. `App.OnFrameworkInitializationCompleted`: создаётся `Window` с **code-only минимальными шаблонами**
   (`Theme.cs/MinimalTemplates`: Window, ScrollViewer, ScrollBar, Thumb, ItemsControl — всё, что нужно читалке;
   FluentTheme не грузится), затем join парсера.
4. Top-level блоки → `ItemsControl` с `VirtualizingStackPanel` (`MdBuilder.BuildVirtualized`): контролы
   создаются, шейпятся и раскладываются **только для видимых блоков** (first-viewport-first; это разрешённая
   техника, остальное строится при прокрутке).
5. **Большие файлы (> 256 KB)**: поток парсинга сначала парсит только префикс (~64 KB, разрез по пустой строке),
   отдаёт его первому кадру, затем парсит весь текст; после первого кадра `MdBuilder.ApplyFull` заменяет
   последний (возможно обрезанный) блок и дописывает хвост. Первые блоки не пересоздаются (top-level блок закрыт,
   как только начался следующий). Если в файле есть link reference definitions — полная замена списка.
6. `Window.Opened` → `TopLevel.RequestAnimationFrame` ×2 (+ ожидание `Compositor.RequestCompositionBatchCommitAsync().Rendered`,
   что наступит позже) → `DwmFlush()` → `t_content` (PROTOCOL §3 для Avalonia). В bench-режиме с `FASTMD_BENCH_EXIT=1`
   окно закрывается, процесс выходит с кодом 0.

**Лаунчер (`launcher/launcher.c`)**: `CoInitializeEx(STA)` (то же, что `[STAThread]`), `LoadLibraryExW("fastmd-core.dll")`,
вызов экспорта `fastmd_main` (он берёт аргументы через `CommandLineToArgvW`). Манифест (PerMonitorV2) встроен в лаунчер.
Никакого резидентного процесса и кэша — каждый запуск грузит и инициализирует всё с нуля.

## 4. Варианты (proto.json)

| id | exe | параметры | смысл |
|---|---|---|---|
| **`avalonia`** (лучший) | `out/aot-lib/fastmd-avalonia.exe` | `--render software` | NativeAOT DLL + лаунчер, Skia software, минимальная тема, виртуализация, префикс-парсинг больших файлов |
| `avalonia-exe` | `out/aot/fastmd-avalonia.exe` | `--render software` | то же, но обычный одиночный NativeAOT exe 13.8 MB — показывает цену размера exe |
| `avalonia-gpu` | `out/aot-lib/fastmd-avalonia.exe` | `--render angle` | то же, но штатный GPU-рендер Avalonia (ANGLE/EGL поверх D3D11) |
| `avalonia-stock` | `out/jit-fluent/fastmd-avalonia.exe` | `--render angle` | «Avalonia по умолчанию»: JIT + R2R, FluentTheme, ANGLE (тот же Markdown-билдер) |

Все ручки командной строки (для воспроизведения таблицы ручек ниже):
`--id <variant>` (заголовок окна), `--render software|angle|wgl|vulkan`, `--comp default|winui|dcomp|lowlat|redir`
(Win32CompositionMode), `--ui-thread-render`, `--layout virtual|stack`, `--full` (stack: весь документ до первого
кадра), `--text selectable|plain`, `--chunk off` (без префикс-парсинга), `--warm` (предзагрузка DLL и шрифтов
в фоновом потоке), `--scroll <dip>` (отладка: прокрутить после первого кадра, для скриншотов).

Общие настройки проекта (`src/FastMdAvalonia.csproj`): `InvariantGlobalization`, `UseSystemResourceKeys`,
`ConcurrentGarbageCollection=false`, `EventSourceSupport/DebuggerSupport/MetadataUpdaterSupport=false`,
`BuiltInComInteropSupport=false`; для AOT — `OptimizationPreference=Speed`, `IlcInstructionSet=x86-x64-v3`,
`StackTraceSupport=false`, `TrimMode=full`.

## 5. Что поддерживается (vs PROTOCOL §5)

| Элемент | Статус |
|---|---|
| h1–h6: размеры 30/24/20/17/15/14, semibold, h6 `#59636e`, линия под h1/h2 | ✅ |
| Абзацы, перенос по словам, line-height 24 (15 × 1.6), колонка 860 DIP по центру, поля 32 | ✅ |
| **bold** / *italic* / ~~strike~~ (приглушённый цвет) / ссылки `#0969da` | ✅ ссылки кликабельны (http/https/mailto → браузер), курсор-рука |
| `inline code` | ⚠️ фон `#eff1f3` и Cascadia Mono есть; **нет скругления и padding** (Run не умеет) — padding имитирован пробелами |
| Маркированные / нумерованные (со start) / вложенные / task-списки | ✅ маркеры • ◦ ▪ по уровням, чекбоксы рисуются |
| Цитаты (полоса 4 px `#d1d9e0`, текст `#59636e`) | ✅ |
| Горизонтальная линия (2 px, как в style.css) | ✅ |
| Блоки кода: Cascadia Mono 13.5, фон `#f6f8fa`, радиус 6, padding 16 | ✅ **без переноса**, горизонтальная прокрутка внутри блока |
| Подсветка синтаксиса | ✅ бонус: свой однопроходный лексер (c/cpp, csharp, js/ts, json, powershell, python, rust), цвета GitHub light |
| GFM-таблицы: рамки, жирная шапка, зебра, padding 6/13, выравнивание колонок | ✅ широкая таблица прокручивается горизонтально |
| Локальные PNG | ✅ (Skia, уменьшаются до ширины колонки); удалённые URL и SVG — нет |
| Кириллица, CJK, цветные эмодзи | ✅ (fallback шрифтов Skia/DirectWrite, эмодзи цветные) |
| Прокрутка: колесо, ↑↓, PgUp/PgDn/Space, Home/End; Esc закрывает | ✅ |
| Выделение и копирование | ⚠️ только **в пределах одного абзаца/блока** (SelectableTextBlock на блок); сквозного выделения нет |
| HTML-блоки | ⚠️ показываются как моноширинный текст; inline HTML выкидывается (кроме `<br>`) |
| Per-monitor DPI v2, окно 1000×800 DIP, заголовок `<file> — FastMD (<id>)` | ✅ |
| Нет: сноски, якоря заголовков/TOC, math, mermaid, поиск, zoom, тёмная тема | ❌ |

Скриншоты (просмотрены): `shots/avalonia-medium.png`, `shots/avalonia-small.png`, `shots/avalonia-large.png`,
прокрученные `shots/avalonia-dev-s1-medium.png` (код/цитата/таблица/картинка), `shots/avalonia-dev-s2-small.png`,
`shots/avalonia-dev-s3-large.png` (хвост large после слияния полного парсинга),
`shots/avalonia-gpu-medium.png`, `shots/avalonia-stock-medium.png` (FluentTheme — визуально то же, другой скроллбар).

## 6. Быстрые цифры

`python bench/harness/bench.py run avalonia,avalonia-exe,avalonia-gpu,avalonia-stock --doc small,medium,large --runs 5`
(interleaved, 5 замеров + 1 первый; **машина делилась с другими агентами — шум ±20–40 мс**; сырые данные
`results/quick-final.json`). Время — от `t0` перед CreateProcessW, мс.

| вариант | small медиана (min) | medium медиана (min) | large медиана (min) | окно видно (obs, medium) | CPU, мс (medium) | WS, MB (s/m/l) | dist |
|---|---|---|---|---|---|---|---|
| **avalonia** | **152.8** (138.8) | **169.9** (151.3) | **158.1** (153.5) | 129 | 109 | 48 / 66 / 163 | 31.3 MB |
| avalonia-exe | 168.9 (164.7) | 200.3 (182.3) | 203.6 (186.1) | 164 | 125 | 48 / 67 / 168 | 31.2 MB |
| avalonia-gpu | 395.6 (370.0) | 404.9 (380.1) | 386.1 (346.9) | 331 | 391 | 93 / 112 / 214 | 31.3 MB |
| avalonia-stock | 661.1 (623.9) | 654.5 (650.2) | 670.9 (631.3) | 563 | 688 | 120 / 139 / 240 | 37.5 MB |
| *baseline-win32 (для сравнения, small)* | *88.1 (84.2)* | | | | | | *0.15 MB* |

Холодный первый запуск сразу после пересборки (Defender впервые сканирует новые бинарники): `avalonia` 309 мс,
`avalonia-exe` 332, `avalonia-gpu` 444, `avalonia-stock` 1210 мс (medium, по одному замеру).

### Разбивка по меткам (медианы, `avalonia`, мс от t0)

| метка | small | medium | large | что происходит до неё |
|---|---|---|---|---|
| CreateProcess вернулся | 27 | 27 | 25 | создание процесса (лаунчер 100 KB) |
| `main` | 57 | 60 | 55 | загрузка `fastmd-core.dll` 13.8 MB, init NativeAOT runtime, CoInitialize |
| `parsed` / `parsed_prefix` | 58 | 62 | 66 | параллельный поток: чтение + Markdig (large: только 64 KB префикса) |
| `platform_ready` | 74 | 74 | 71 | Avalonia Win32 + Skia (software) platform init |
| `window_created` | 93 | 91 | 96 | Application, объект Window, шаблоны |
| `content_set` | 93 | 92 | 96 | join парсера, ItemsControl с блоками |
| `layout_done` | 105 | 120 | 113 | первый Measure/Arrange: контролы видимых блоков, HarfBuzz-шейпинг, fallback шрифтов (CJK/эмодзи) |
| `opened` (= t_window) | 131 | 144 | 135 | Show(): HWND, активация |
| `raf2` | 148 | 164 | 153 | два кадра: растеризация Skia 1000×800 в software + вывод |
| **t_content** | **153** | **170** | **158** | DwmFlush |

Для сравнения `avalonia-gpu` (medium): `main` 54 → `platform_ready` **279** (ANGLE/EGL/D3D11 init ≈ 220 мс) →
`opened` 344 → `raf2` 400 (первый GPU-кадр ≈ 55 мс) → 405. `avalonia-stock`: `main` 66 → `platform_ready` 352
(JIT + ANGLE) → `layout_done` 555 (JIT кода раскладки/текста + Fluent) → 654.

### Ручки (все мерились; medium, 6 замеров interleaved, `results/knobs-medium.json`)

| конфигурация | медиана | min | вывод |
|---|---|---|---|
| **avalonia** (лучший) | **161** | 158 | |
| + `--ui-thread-render` (рендер на UI-потоке) | 161 | 142 | в пределах шума; в других прогонах то лучше, то хуже — не включён |
| + `--text plain` (TextBlock вместо SelectableTextBlock) | 175 | 151 | выигрыша нет → оставлен Selectable |
| `--layout stack` (≈1400 DIP блоков синхронно, остальное после 1-го кадра) | 183 | 168 | виртуализация чуть лучше и нужна для large |
| одиночный exe 13.8 MB вместо лаунчера (`avalonia-exe`) | 200 | 177 | **+38 мс** (см. §7) |
| FluentTheme (одиночный exe, software) | 245 | 223 | **+45 мс** к `avalonia-exe` |
| JIT + ReadyToRun (одиночный, software, минимальная тема) | 394 | 365 | **+195 мс** к `avalonia-exe` |
| ANGLE/D3D11 (`avalonia-gpu`) | 410 | 381 | **+250 мс** |
| JIT + Fluent + ANGLE (`avalonia-stock`) | 671 | 649 | +510 мс |
| `--layout stack --full` (весь документ до первого кадра) | 713 | 673 | first-viewport-first экономит ~550 мс на medium |
| WGL (OpenGL) | 762 | 668 | хуже всех |

Ещё (`results/knobs-large.json`, large): `avalonia` 208 мс, `--chunk off` 293 (+85: весь Markdig-парс 3.7 MB ≈ 150 мс
становится критическим путём), `--layout stack` 288.
Ранние прогоны (`results/dev-knobs-*.json`, частично на старой сборке): `--warm` (предзагрузка Skia/HarfBuzz DLL и
системных шрифтов в фоне) — 170.5 vs 172.1, эффекта нет; `OptimizationPreference=Size` (exe −1.9 MB) — в пределах шума;
`--comp dcomp|lowlat|redir|winui` при ANGLE — в пределах шума (470–550). Для software композиция не применяется.

## 7. Находка: размер EXE стоит ~2.5 мс/MB на каждый запуск (Defender)

Эксперимент `experiments/exe-size/` (`pwsh -File run.ps1`): NativeAOT hello (WinExe, 1.3 MB) печатает время в первой строке
Main; к копиям дописан 12 MB «хвост» (overlay — никогда не маппится и не исполняется).

| exe | размер | CreateProcess, медиана | до Main, медиана |
|---|---|---|---|
| hello.exe | 1.3 MB | 22.9–30.2 мс | 37.8–45.2 мс |
| hello + 12 MB random | 13.3 MB | 54.2–58.1 мс | 68.6–73.1 мс |
| hello + 12 MB нулей | 13.3 MB | 56.6 мс | 71.9 мс |

То есть **CreateProcess растёт на ~30 мс при +12 MB файла, даже если это нули** — при каждом запуске, не только
первом. Скорее всего это real-time protection Defender, который читает/хэширует образ процесса при создании
(проверить отключением нельзя — нет админа). Загрузка той же 13.8 MB кодовой базы как **DLL** такой платы не
требует: у `avalonia` (лаунчер 100 KB + DLL) CreateProcess 25–27 мс, у `avalonia-exe` 55–57 мс; `main` 57–60 против 84–88.
**Это касается всех прототипов с большим exe** (NativeAOT, Rust, статический Qt): маленький exe + основная DLL —
честный приём (нет резидента и кэша), который стоит проверить и там.

## 8. Куда уходит время (avalonia, medium, ~170 мс)

- ~27 мс — CreateProcess (как у baseline-win32).
- ~33 мс — от CreateProcess до `main`: маппинг/релокации 13.8 MB DLL, init NativeAOT (у hello-world ≈ 13 мс), CoInitialize.
- ~31 мс — инициализация Avalonia (Win32 + Skia platform ≈ 14, Application/Window ≈ 17).
- ~29 мс — первая раскладка видимых блоков (создание контролов, HarfBuzz, fallback шрифтов для CJK/эмодзи на medium; на small 12 мс).
- ~24 мс — Show(): HWND, активация, DWM.
- ~20 мс — два кадра (software-растеризация 1000×800 + вывод), ~6 мс — DwmFlush.
- Парсинг Markdig идёт параллельно и на small/medium не на критическом пути (1–2 мс); на large 64 KB префикс ≈ 2 мс.

## 9. Идеи дальнейшего ускорения

1. Убрать `av_libglesv2.dll` из дистрибутива software-варианта (−5.4 MB дистрибутива; на старт не влияет — не грузится).
2. Сократить `fastmd-core.dll` (13.8 MB): trimming Avalonia-функциональности (Automation/UIA, Dialogs, OpenGL/Vulkan-пути
   не нужны), `IlcDisableReflection`/rd.xml — меньше страниц и релокаций до `main` (потенциал ~10 мс).
3. Показ окна раньше: сейчас HWND показывается после первой раскладки; можно показать фон сразу и досчитать
   раскладку к следующему кадру (t_window раньше, t_content почти тот же).
4. Облегчить первый экран: не создавать `ScrollViewer` для каждого блока кода/таблицы до наведения/переполнения,
   упростить дерево (один `Border` вместо `Border+ScrollViewer+TextBlock`).
5. Свой лёгкий контрол «документ» (один `Control` с `TextLayout` на блок и собственным hit-test) вместо дерева контролов —
   меньше объектов/стилей/привязок; заодно даёт сквозное выделение.
6. DirectPInvoke для user32/gdi32/dwmapi в NativeAOT (убрать ленивое связывание) — единицы мс.
7. Если допустимо для продукта — вариант `-resident` (не делал: запрещено без суффикса, и это другой класс решения).

## 10. Оценка трудоёмкости полного продукта на этом стеке

Базовая читалка (этот прототип) ≈ 2–3 дня. До продукта:
- сквозное выделение и копирование через блоки + поиск (Ctrl+F) с подсветкой — самое дорогое: свой документ-контрол, **2–3 недели**;
- тёмная тема, zoom, настройки шрифта — 3–5 дней; ассоциация .md, drag&drop, недавние файлы, live-reload (FileSystemWatcher) — 3–5 дней;
- сноски, якоря/TOC-панель, относительные ссылки на другие .md, удалённые картинки и SVG (Svg.Skia — проверить AOT) — 1–2 недели;
- нормальная подсветка синтаксиса (TextMateSharp или свои лексеры — проверить AOT-совместимость) — 1 неделя; math/mermaid — отдельные большие задачи;
- установщик (MSIX/Inno), подпись, автообновление, тесты — 1–2 недели.

Итого MVP ≈ **4–6 недель** одного разработчика, «полированный» продукт ≈ **2.5–3.5 месяца**. Плюс стека: один код на
C#, быстрый software-рендер, хорошая типографика Skia/HarfBuzz, кроссплатформенность. Минусы: дистрибутив ~26–31 MB,
~2× от нативного пола по старту, NativeAOT + Avalonia требуют аккуратности (без XAML-рефлексии), выделение текста
придётся писать самим.

## 11. Грабли (всё реально встретилось)

1. **Относительные `BaseIntermediateOutputPath`/`MSBuildProjectExtensionsPath` в командной строке** (глобальные свойства
   MSBuild не перенормируются) → `nuget.g.props/targets` не импортируются → `PublishAot=true` **молча** даёт обычный
   self-contained JIT-вывод на 95 MB (без ILCompiler и триммера). Пути в `build.ps1` абсолютные.
2. Линковка NativeAOT: `findvcvarsall.bat` → `vcvarsall.bat` вызывает `vswhere.exe` из PATH → «vswhere.exe не является
   командой» и ошибка MSB3073. Решение: добавить `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer` в PATH (так и в `bench/tools/msvc.cmd`).
3. Несколько flavor-ов в одном проекте: стандартный glob `**/*.cs` подбирает `AssemblyInfo.cs` из `obj/<другой flavor>` →
   CS0579 «повторяющийся атрибут». Решение: `DefaultItemExcludes += obj/**;bin/**`.
4. `IlcOptimizationPreference` в .NET 8 ничего не делает — ILCompiler читает `OptimizationPreference` (Speed → `--Ot`, Size → `--Os`).
5. Минимальный code-only шаблон Window обязан рисовать `Background` (биндинг `ContentPresenter.Background`), иначе в software-режиме окно **чёрное**.
6. `pwsh -File build.ps1 -Only a,b` передаёт одну строку `"a,b"`, а не массив — в скрипте split по запятой.
7. Avalonia 12: шейпинг текста вынесен в `Avalonia.HarfBuzz` — нужен `.UseHarfBuzz()`. Анализаторы Avalonia 12 собраны под
   Roslyn 4.14, SDK 8.0.4xx даёт предупреждения CS9057 (генераторы `x:Name` не работают; XAML здесь не используется).
8. ANGLE на этой машине (RTX 5070 + AMD iGPU) инициализируется ~220 мс, WGL ещё дольше — для читалки software-рендер Skia быстрее и визуально идентичен.
9. В DLL-сборке нет `[STAThread]` — STA делает лаунчер (`CoInitializeEx`), иначе OLE (буфер обмена, drag&drop) в Avalonia не инициализируется;
   аргументы берутся из `GetCommandLineW` (у NativeAOT-библиотеки нет `argv`).
10. Виртуализированный список оценивает высоту непостроенных блоков → ползунок скроллбара «плавает» при прокрутке длинного документа.
11. Большой файл со ссылками вида `[текст][ref]`, где `ref` определён после 64 KB: первые ~100 мс такие ссылки видны как текст, затем полная замена.

## 12. Файлы

- `src/Program.cs` — запуск, аргументы, параллельный парсинг, префикс-парсинг, метки, rAF×2 → DwmFlush, экспорт `fastmd_main`.
- `src/MdBuilder.cs` — Markdig AST → контролы Avalonia, виртуализация, `ApplyFull`, `MdText` (выделение + клики по ссылкам).
- `src/Theme.cs` — стиль по спецификации + минимальные code-only шаблоны; `src/CodeHighlighter.cs` — лексер подсветки.
- `launcher/launcher.c` — лаунчер DLL-сборки; `src/app.manifest` — DPI PerMonitorV2 (встраивается и в exe, и в лаунчер).
- `build.ps1`, `proto.json`, `nuget.config`; `results/*.json` — сырые прогоны; `shots/*.png` — скриншоты;
  `experiments/exe-size/` — эксперимент «размер exe vs CreateProcess».

---

## Аудит (независимая проверка, 2026-09-19)

**Итог: прототип честный, изменений в исходниках, сборке и `proto.json` не потребовалось. Все четыре варианта годны
для финального замера.** Ниже — что именно проверено и какие оговорки стоит держать в голове при сравнении.

### Что проверено

1. **Метка `t_content`** (`src/Program.cs` → `common/FastMdBench.cs`): документ попадает в дерево (`content_set`) до
   `Show()`; затем `Window.Opened` → `RequestAnimationFrame` ×2 **и** ожидание
   `Compositor.RequestCompositionBatchCommitAsync().Rendered` (берётся более позднее) → `DwmFlush()` → `t_content`
   в процессе, владеющем окном. Это PROTOCOL §3 для Avalonia плюс дополнительная страховка. Ожидание `Rendered` строже
   протокола: при 165 Гц `DwmFlush` занимает ≤ 6 мс, а `raf2 → t_content` в 15 замерах — 4.6–25.5 мс (медиана ~13),
   то есть страховка добавляет ~5–10 мс. Она оправдана: к `raf2` render-поток может ещё не отрисовать batch с документом.
   Все метки ≤ `t_content`, `check` без предупреждений на всех четырёх вариантах (medium и large).
2. **Независимая пиксельная проверка** (скрипт аудитора `audit/probe.py`). Запускает вариант, как harness, и примерно
   каждые 12 мс снимает **композитный экран** (`GetDC(NULL)` + `BitBlt`, окно приложения не трогается). 17 запусков
   `avalonia` на medium:
   - если DWM проигрывает анимацию открытия (fade-in ~50–80 мс; `SPI_GETCLIENTAREAANIMATION` = on), текст впервые
     виден в полупрозрачном окне в интервалах снимков [212..223] мс при `t_content` 219 (на предыдущем снимке
     [200..212] окно ещё пустое белое), [210..222] при 219 (предыдущий [201..210] пустой) и [180..203] при 211
     (кадры просмотрены глазами);
   - если анимации нет, документ целиком на экране **уже на первом снимке**, на 23–54 мс **раньше** `t_content`
     (5 запусков: ≤ 158 мс при 182, ≤ 143 при 173, ≤ 147 при 178, ≤ 220 при 250, ≤ 248 при 301).

   Содержимое ни разу не появилось на экране заметно позже `t_content`, то есть **метка не ранняя, скорее консервативная**
   (на 0…~50 мс, в среднем ~1–2 кадра). Для сравнения, `baseline-win32` под тем же зондом: белый кадр с текстом
   появляется через 26–45 мс после его `t_content` (та же анимация DWM). Эффект анимации общий для всех прототипов.
   `PrintWindow` для такого зонда не годится: он блокируется на UI-потоке приложения (~40 мс) и искажает замер.
3. **`observed_ms` (harness увидел окно) < `t_window`** во всех `check` (medium 184 против 203, large 136 против 170):
   `Show()` делает окно видимым раньше, чем срабатывает `Opened`. `t_window` тоже консервативна.
4. **Нет резидента, кэша и данных корпуса.** Лаунчер (`launcher/launcher.c`) делает только `CoInitializeEx` +
   `LoadLibraryExW(fastmd-core.dll)` + вызов экспорта. Импорты — только ole32/user32/kernel32 (dumpbin); в job ровно
   1 процесс; на диск пишется только файл результата. В сборке нет `EmbeddedResource`; `CodeHighlighter` — общий
   лексер для 7 языков, текста корпуса в нём нет. Приём «маленький exe + NativeAOT DLL» честный: каждый запуск грузит
   и инициализирует всё с нуля.
5. **Префикс-парсинг large** (файл > 256 KB: первый кадр строится по 64 KB до пустой строки, после него — полный парс
   и слияние) — законный вариант first-viewport-first. Top-level блоки префикса, кроме последнего, совпадают с полным
   парсом, последний заменяется; в корпусе нет link reference definitions. Проверено снимками:
   `shots/audit-avalonia-large-scroll400000.png` (граница слияния, разделы 36–37, отрисована правильно) и
   `shots/audit-avalonia-large-end.png` (после End виден хвост `large.md`: раздел 1659.3, h5/h6, hr — документ
   сливается целиком). Полный парс 3.7 MB в check завершался до `t_content` (`parsed_full` 173 мс при `t_content` 201),
   но первый кадр строится по префиксу.
6. **Сборка.** `build.ps1` пересобран аудитором с нуля (`pwsh -File build.ps1`, 57 с, exit 0; артефакты в `out/` — от
   этой пересборки). `out/aot-lib` и `out/aot` — настоящий NativeAOT: управляемых сборок нет, `fastmd-core.dll`
   экспортирует ровно `fastmd_main`. `out/jit-fluent` — действительно ReadyToRun: `Avalonia.Base.dll` 2.35 MB IL →
   7.15 MB, `Markdig.dll` 0.49 → 1.40 MB. Настройки: Release, `OptimizationPreference=Speed`,
   `IlcInstructionSet=x86-x64-v3`, `TrimMode=full`; лаунчер `/O1 /MT`, subsystem GUI.
7. **Окно**: клиент 1000×800 DIP (снимок с рамкой 1002×832 при 100 %), заголовок `<file> — FastMD (<id>)`,
   PerMonitorV2 (манифест встроен и в лаунчер, и в exe), выход с кодом 0.

### Визуальное качество (скриншоты `shots/audit-*.png`)

Просмотрены medium/small всех вариантов плюс прокрученные `audit-avalonia-medium-scroll780.png`,
`audit-avalonia-medium-scroll1500.png` и `audit-avalonia-large-end.png`. Очень близко к GitHub:
- заголовки h1–h6 с правильными размерами, линии под h1/h2, h6 приглушён;
- абзацы с line-height 24, колонка 860;
- ссылки `#0969da`, приглушённое зачёркивание;
- маркеры • ◦ и вложенная нумерация, чекбоксы task-list;
- цитата с полосой;
- блок кода `#f6f8fa` со скруглением и подсветкой в цветах GitHub;
- GFM-таблица с рамками, жирной шапкой, зеброй и выравниванием;
- локальный PNG, кириллица, CJK, цветные эмодзи.

Отличия:
- у inline code нет скругления, а padding имитирован пробелами — перед запятой виден лишний пробел (`inline code ,`);
- комментарии в коде курсивом (у GitHub — нет);
- чекбоксы задач стоят в колонке маркеров.

GPU- и Fluent-варианты визуально идентичны software, у Fluent другой скроллбар. **Оценка: визуал 8/10, полнота 8.5/10.**

### Оговорки (на честность не влияют)

- Старые `shots/avalonia-dev-s*.png` сняты в 03:30, **до** последней правки `MdBuilder.cs` (03:31) и пересборки;
  актуальны `shots/audit-*.png`.
- `avalonia-stock` — не совсем «стоковый» Avalonia. Код тот же оптимизированный: виртуализация, параллельный парс,
  `InvariantGlobalization`, `TieredPGO=false`. «Стоковые» только JIT+R2R, FluentTheme и ANGLE. Вариант
  framework-dependent: 37.5 MB дистрибутива — без .NET runtime.
- `dist` у `avalonia` включает `av_libglesv2.dll` (5.4 MB), нужный только `avalonia-gpu`.
- `IlcInstructionSet=x86-x64-v3` требует AVX2. Для замера на 7950X это нормально; для продукта нужен v2 или проверка CPU.
- `--scroll` (отладка) применяется до слияния полного парса large, поэтому прокрутка ограничена префиксом.
- Harness: `observed_ms` отмечает только видимость окна, а не пиксели документа, поэтому ранний `t_content` при
  рано показанном окне он не поймал бы. Для этого аудитору понадобился пиксельный зонд. Багов harness не найдено.

### Цифры аудитора (шумная машина, другие агенты работают параллельно)

`bench.py run avalonia,avalonia-exe,avalonia-gpu,avalonia-stock --doc small,medium --runs 5`
(`results/audit-run.json`), медиана (min), мс:

| вариант | small | medium | obs (medium) | CPU, мс (medium) | peak commit, MB (medium) |
|---|---|---|---|---|---|
| avalonia | 157.3 (142.6) | 182.6 (159.1) | 132.8 | 125 | 42.6 |
| avalonia-exe | 205.2 (165.2) | 191.5 (169.0) | 153.8 | 125 | 42.2 |
| avalonia-gpu | 405.7 (358.9) | 440.9 (413.7) | 357.8 | 406 | 184.8 |
| avalonia-stock | 696.0 (637.9) | 672.4 (630.6) | 565.4 | 625 | 186.0 |

Более тихий момент, interleaved с полом (`results/audit-vs-baseline.json`, 4+1 запуска): `baseline-win32` —
80.4 / 89.0 мс (small / medium), `avalonia` — 129.3 / 140.8 мс, то есть ≈ +50 мс (≈1.6×) к полу.

Скрипты аудитора: `audit/probe.py` (пиксельный зонд: `python audit/probe.py <variant> <doc> [runs] [--save]`) и
`audit/scrollshot.py` (скриншот с доп. аргументами / клавишей End).
