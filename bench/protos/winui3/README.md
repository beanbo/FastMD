# winui3 — C# .NET 8 + WinUI 3 (Windows App SDK 1.8, unpackaged) + Markdig

Нативный XAML-просмотрщик: Markdig парсит Markdown в фоновом потоке, AST превращается **в коде** (без XAML-разметки)
в дерево `TextBlock` / `Border` / `Grid` / `Image`; в первый кадр попадает только первый экран документа, остальное
дописывается после кадра. Вариант `winui3-webview2` — тот же exe, но документ показывается в WinUI-контроле WebView2
(Markdig → HTML + `bench/common/style.css`).

**Итог:** лучшая честная конфигурация (`winui3`: NativeAOT `OptimizationPreference=Speed` + WinAppSDK
self-contained, без XamlControlsResources/Mica) даёт **t_content ≈ 370–385 мс**, одинаково для small/medium/large
(3.7 МБ), при полу машины `baseline-win32` ≈ 73 мс. Из них ≈ 140 мс — ожидание первого layout/кадра, в основном
создание D3D11-устройства на NVIDIA внутри XAML-композитора, ≈ 100 мс — фиксированная инициализация WinUI (XAML core,
окно, `Activate`), ≈ 65 мс — старт процесса до `main`, 35–65 мс — рендер первого кадра. Разбор Markdown и построение
первого экрана — < 10 мс. Без резидентного процесса быстрее на этом стеке не получается; «как у всех
WinUI-приложений» (Mica + Fluent-ресурсы) — ещё +125…170 мс, WebView2 внутри WinUI — +220 мс, JIT+R2R вместо AOT —
+40…65 мс.

---

## 1. Стек и версии

| Компонент | Версия |
|---|---|
| .NET SDK / runtime | 8.0.424 / 8.0.30; NativeAOT — `Microsoft.DotNet.ILCompiler` 8.0.30, линковка MSVC 14.44 |
| TFM | `net8.0-windows10.0.22621.0`, `win-x64`, Windows SDK projection 10.0.22621.56 |
| Windows App SDK | компонентный пакет `Microsoft.WindowsAppSDK.WinUI` **1.8.260803003** (не метапакет, см. §9); для framework-dependent сборки — `Microsoft.WindowsAppSDK.Runtime` 1.8.260804001 (framework package 8000.946.1701.0 установлен на машине) |
| WebView2 | SDK `Microsoft.Web.WebView2` 1.0.3179.45 (приходит с WinAppSDK), Runtime 153 |
| Markdown | Markdig **1.3.2** (pipe tables, task lists, strikethrough, autolinks) |
| Подсветка кода | собственный однопроходный лексер (`src/Highlighter.cs`, ~110 строк, без regex) |
| Упаковка | unpackaged (`WindowsPackageType=None`), без MSIX, xcopy-папка |

Машина: Ryzen 9 7950X, 64 ГБ, NVIDIA RTX 5070 + AMD iGPU, Windows 11 26200, Defender включён.

## 2. Как собрать

```
pwsh -File bench/protos/winui3/build.ps1                 # все 5 конфигураций с нуля, ~55–70 с
pwsh -File bench/protos/winui3/build.ps1 -Only aot,r2r   # только нужные
```

- NuGet-пакеты кладутся в `bench/.tools/winui3/packages` (`nuget.config`), ничего глобального.
- Каждая конфигурация: `dotnet publish -c Release -r win-x64` в `out/<name>`, промежуточные файлы в `obj/<name>`;
  обе папки перед сборкой удаляются (скрипт чистит только папки внутри прототипа, через `[IO.Directory]::Delete`).
- NativeAOT-линковке нужен `vswhere.exe` в `PATH` (vcvarsall вызывает его без пути) — `build.ps1` добавляет
  `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer` сам.

| Папка | Свойства MSBuild | Размер | Для чего |
|---|---|---|---|
| `out/aot` | `FmAot=true`, `FmIlcPref=Speed`, `WindowsAppSDKSelfContained=true` | 58.1 МБ, 235 файлов (exe 7.0 МБ + 87 папок MUI-локализаций WinUI) | `winui3`, `winui3-webview2` |
| `out/aot-pri` | как `aot` + `EnableMsixTooling=true` (генерирует `fastmd-winui3.pri`) | 59.4 МБ | `winui3-fluent` (XamlControlsResources без PRI не работает) |
| `out/aot-fd` | `FmAot=true`, `WindowsAppSDKSelfContained=false` (bootstrapper + установленный WinAppSDK 1.8) | **8.2 МБ, 5 файлов** | только замер ручки |
| `out/r2r` | JIT + ReadyToRun, .NET 8 framework-dependent, WinAppSDK self-contained | 100.8 МБ (R2R-проекции CsWinRT) | `winui3-r2r` |
| `out/jit` | чистый JIT, .NET framework-dependent | 87.4 МБ | только замер ручки |

## 3. Как устроено (что делает старт быстрым)

1. **Свой `Main`** (`DISABLE_XAML_GENERATED_MAIN`, авто-инициализаторы WinAppSDK выключены): первая строка —
   `FastMdBench.Init()` (метка `main`), сразу стартует поток `parse` (чтение файла + Markdig), затем вручную то,
   что делает `UndockedRegFreeWinRT-AutoInitializer` (`MICROSOFT_WINDOWSAPPRUNTIME_BASE_DIRECTORY` +
   `WindowsAppRuntime_EnsureIsLoaded`), для framework-dependent — `Bootstrap.TryInitialize(1.8)`.
   `InvariantGlobalization=true` (без ICU), `UseSystemResourceKeys=true`.
2. **Минимум XAML-инфраструктуры:** `Application` без `App.xaml`, без `XamlControlsResources`, без Mica; кисти и
   шрифты создаются один раз (`Theme.cs`) и разделяются всеми элементами (каждое создание WinRT-объекта — interop).
3. **Документ — код, не разметка** (`MdRenderer.cs`): абзац/заголовок = один `TextBlock` (легче `RichTextBlock`),
   простой абзац идёт одним `TextBlock.Text`; inline-код — `Run` моноширинным шрифтом + `TextHighlighter` с фоном;
   код-блок = `Border` + `TextBlock` с раскрашенными `Run`; таблица = `Grid`; списки — `Grid` с «висячим» маркером;
   цитата — `Border` с левой полосой. CSS-подобное схлопывание отступов эмулируется.
4. **First viewport first (разрешено протоколом):** в первый кадр идут только блоки верхнего уровня, покрывающие
   ≈1.5 высоты окна по грубой оценке высоты (`MdRenderer.Estimate`) — на medium это 13 из 391 блока. Остальное
   дописывается после кадра порциями ≤12 мс на `DispatcherQueuePriority.Low` (прокрутка работает сразу).
5. **Head-first parse для больших файлов (> 256 КБ):** сначала парсится голова ≥32 КБ, обрезанная по пустой строке
   вне fenced-кода; все блоки головы, кроме последнего, совпадают с полным разбором (последний не используется).
   Полный разбор стартует **после первого кадра** (его ~100 МБ аллокаций вызывают GC, который останавливает и
   UI-поток), затем блоки головы сопоставляются по `Span` и дописывается хвост. Если после головы есть
   reference-definitions ссылок — первый экран перерисовывается из полного разбора.
6. **Порядок на UI-потоке:** сначала `new Window()` (запускает композитор), потом ожидание разбора (к этому моменту
   готов), построение первого экрана (~2–4 мс), `Content`, `Activate()`.
7. **t_content (PROTOCOL §3):** `Loaded` корня → первый `CompositionTarget.Rendered` → `DwmFlush()` → запись
   результата. `t_window` — событие `Window.Activated`. В `notes` пишется `blocks_in_first_frame=13/391`.
8. **WebView2-вариант** (`WebHost.cs`): `CoreWebView2Environment` создаётся первым делом в `OnLaunched`
   (параллельно созданию XAML-окна), HTML первого экрана строится в потоке разбора, страница = `style.css` + статья +
   `rAF(rAF(postMessage))`; хвост — `PostWebMessageAsString` после кадра (заодно обходит лимит 2 МБ у
   `NavigateToString`). Картинки — через `SetVirtualHostNameToFolderMapping`. Браузерные аргументы — «lean»-набор,
   найденный прототипом `cpp-webview2` (`--disable-gpu --in-process-gpu`, network service in-process, без фоновых
   сервисов): на этом же runtime −84 мс и 5 процессов вместо 7 (§5).

## 4. Варианты (`proto.json`)

| id | exe / args | Что это |
|---|---|---|
| **`winui3`** (лучший) | `out/aot/fastmd-winui3.exe --id winui3` | NativeAOT (Speed), WinAppSDK self-contained, всё из §3 |
| `winui3-webview2` | `out/aot/... --webview` | тот же exe, документ в WinUI WebView2 + `style.css`, lean browser args |
| `winui3-r2r` | `out/r2r/fastmd-winui3.exe` | «классический .NET»: JIT + ReadyToRun, .NET 8 из системы — цена JIT |
| `winui3-fluent` | `out/aot-pri/... --fluent --mica` | «типичное красивое WinUI-приложение»: `XamlControlsResources` + `MicaBackdrop` + контент в заголовке окна |

Все ручки остались флагами командной строки (для воспроизведения §5):

| Флаг | Действие |
|---|---|
| `--id <id>` | id варианта в заголовке окна |
| `--webview` | WebView2 вместо нативного XAML |
| `--wvdefault` | WebView2 со стандартными browser args (без lean-набора) |
| `--fluent` | `XamlControlsResources` (нужна сборка `out/aot-pri`, иначе падение — см. §9) |
| `--mica` | `MicaBackdrop` + `ExtendsContentIntoTitleBar` + свой заголовок (светлая тема) |
| `--full` | весь документ до первого кадра (без first-viewport-first и head-first) |
| `--nohead` | без head-first parse (большие файлы ждут полный разбор) |
| `--noselect` | `IsTextSelectionEnabled=false` |
| `--d3dwarm` | «прогрев» D3D11-устройства в фоновом потоке с первой строки `Main` |
| `--dwwarm` | «прогрев» DirectWrite (layout строки латиница/кириллица/CJK/emoji + моноширинной) в фоновом потоке |
| `--scrollto <dip>` | отладка: догрузить документ и прокрутить (для скриншотов середины документа) |

## 5. Проверенные ручки (knobs)

Замеры `bench.py run`, чередующийся порядок, медиана `t_content`, мс. Машину параллельно грузили другие агенты —
разница < ~20 мс считается шумом, сравнивать можно только строки одного прогона. Сырые данные — `out/*.json`.

**Прогон 1 — `knobs-medium.json`** (medium, n=9; сборка ещё с «blended» `-O`, см. прогон 3):

| Конфигурация | t_content | Δ к `winui3` | Что показывают метки |
|---|---|---|---|
| **`winui3`** (AOT, WinAppSDK self-contained) | **416** | — | — |
| + app PRI (`aot-pri`, без Fluent) | 408 | шум | наличие `.pri` ничего не стоит |
| + `--noselect` | 410 | шум | выделение текста в `TextBlock` не влияет на старт |
| + `XamlControlsResources` | 423 | +7 (шум) | `xcr_merged` ≈ 6 мс; словарь стилей грузится лениво |
| AOT + WinAppSDK **framework-dependent** (`aot-fd`) | 427 | +11 (шум) | bootstrapper ~2 мс; dist 8 МБ вместо 58 |
| + `--d3dwarm` | 434 | шум | прогревочное устройство готово к ~280 мс, но XAML всё равно ждёт своё — выигрыша нет (драйвер/loader lock сериализуют) |
| JIT, .NET framework-dependent (`jit`) | 487 | +71 | `app_start` +37…60 мс (JIT проекций CsWinRT), разбор 20–45 мс, построение экрана +20…30 мс |
| JIT + ReadyToRun (`r2r`) | 506 | +90 | R2R почти не спасает: проекции CsWinRT частично JIT-ятся; `main` на ~7 мс позже (hostfxr + coreclr), хотя `CreateProcess` на 16 мс быстрее (apphost 0.26 МБ против 7-МБ AOT-exe) |
| + `--mica` | 594 | **+178** | присваивание `SystemBackdrop` синхронно создаёт GPU-устройство (+240…280 мс на UI-потоке) |
| `winui3-fluent` (XamlControlsResources + Mica) | 669 | +253 | то же + окно появляется на ~250 мс позже |
| `--full` (весь документ в первый кадр) | **1042** | **+626** | 391 блок: layout +340 мс, первый кадр +430 мс — first-viewport-first даёт ×2.5 |
| WebView2 со стандартными browser args | 786 | +370 | 7 процессов, CPU ~1.9 с |

**Прогон 2 — `knob-wvargs.json`** (medium, n=8): WebView2 стандартные args **722** → lean args **638** (−84 мс,
5 процессов вместо 7, CPU 1.9 → 1.6 с). Lean-набор оставлен в `winui3-webview2`.

**Прогон 3 — `knob-ilcpref.json`** (n=11). Предыдущий агент задавал `IlcOptimizationPreference=Speed` — в .NET 8
это **не действует** (свойство называется `OptimizationPreference`; в `.ilc.rsp` было `-O`, т.е. blended):

| NativeAOT | medium | small | exe |
|---|---|---|---|
| blended `-O` (было) | 374 | 349 | 6.61 МБ |
| **`OptimizationPreference=Speed`** (`--Ot` + static PGO), выбрано | **366** | **344** | 6.96 МБ |
| `OptimizationPreference=Size` (`--Os`) | 375 | 356 | 6.36 МБ |

Speed стабильно на 5–9 мс быстрее (меньше время на UI-потоке: `window_created`/`activate_ret`), Size — в шуме.

**Прогон 4 — `knob-dwwarm.json`** (n=13): `--dwwarm` 379 / 347 против 376 / 354 (medium / small) — эффекта нет:
весь прогрев DirectWrite занимает ~1 мс (кэш шрифтов системной службы), т.е. разница первого кадра medium (≈65 мс)
и small (≈35 мс) — не fallback-шрифты, а объём растеризации.

**Большой файл** (large.md, 3.7 МБ), `knob-head-large2.json`, n=7:

| Конфигурация | t_content | Комментарий |
|---|---|---|
| `winui3` (head-first, полный разбор после кадра) | **428** | голова разобрана за 1.4 мс; large ≈ medium |
| `--nohead` (ждать полный Markdig) | 523 | полный разбор 3.7 МБ ≈ 280 мс в фоне + его GC-паузы тормозят UI-поток (`window_created` +108 мс вместо +45) |
| head-first, но полный разбор параллельно с UI (промежуточная версия, `knob-head-large.json`) | 469 | GC полного разбора всё ещё мешал UI-потоку → разбор перенесён на после кадра |

Итог для `winui3`: NativeAOT + Speed (−40…65 мс к R2R), WinAppSDK self-contained (framework-dependent равен по
скорости в пределах шума, но требует установленного runtime), без Fluent-ресурсов и Mica, first-viewport-first +
head-first parse.

## 6. Быстрые цифры (`out/quick.json`, `bench.py run … --runs 5`, чередование, медиана 6 запусков)

| Вариант | small | medium | large | t_window | CPU, мс | Процессы | WS, МБ | Commit, МБ | dist, МБ |
|---|---|---|---|---|---|---|---|---|---|
| **`winui3`** | **372** | **384** | **384** | 143–157 | 390–485 | 1 | 96–110 | 154–175 | 58.1 |
| `winui3-r2r` | 414 | 436 | 447 | 226–238 | 530–595 | 1 | 127–142 | 161–182 | 100.8 |
| `winui3-fluent` | 497 | 537 | 556 | 395–408 | 470–500 | 1 | 109–122 | 166–185 | 59.4 |
| `winui3-webview2` | 593 | 607 | 622 | 163–176 | 1405–1440 | 5 | 95–108 (только хост) | 276–291 | 58.1 + Runtime |
| `baseline-win32` (калибровка, в тот же час, `baseline-ref.json`) | — | 73 | — | 65 | 47 | 1 | 14 | 3 | 0.15 |

Самый первый запуск после пересборки может занимать до ~2.7 с (Defender сканирует 235 новых файлов); последующие —
как в таблице. В цифрах шум соседних агентов; окончательное сравнение — последовательный прогон оркестратора.

### Разбивка по меткам (медианы, мс от `t0`, без первого запуска)

**`winui3`, medium** (large — в пределах ±10 мс; у small последний этап 34 мс вместо 66):

| Метка | мс | Δ | Что происходит |
|---|---|---|---|
| `CreateProcess` вернулся | 39 | 39 | Defender + отображение 7-МБ exe (у 0.26-МБ apphost JIT-сборок — 24 мс) |
| `main` | 65 | 26 | загрузчик, NativeAOT runtime init |
| `parsed` (поток `parse`) | 67 | 2 | чтение + Markdig 53 КБ — вне критического пути |
| `winappsdk_init` | 69 | 2 | `WindowsAppRuntime_EnsureIsLoaded` |
| `app_start` | 91 | 22 | `Application.Start`: XAML core (Microsoft.ui.xaml.dll 15 МБ и др.) |
| `launched` | 97 | 6 | `OnLaunched` |
| `window_created` | 138 | 41 | `new Window()`: HWND, AppWindow, in-proc композитор (dcompi/dwmcorei), ввод |
| `first_viewport_built` | 145 | 7 | ожидание разбора (уже готов) + 13 блоков XAML |
| `activate_ret` | 176 | 31 | `Content` + `Activate()` |
| `loaded` | 316 | **141** | первый layout (≈7 мс на 13 блоков, по данным `--full`) + **ожидание D3D11-устройства NVIDIA** (§7) |
| `frame` (`CompositionTarget.Rendered`) | 383 | 66 | рендер первого кадра: растеризация текста, emoji, чекбоксы |
| `t_content` | 384 | ~2–8 | `DwmFlush()` |

**`winui3-webview2`, medium:** `main` 63 → HTML первого экрана 66 → `wv_env_started` 102 → `window_created` 139 →
`wv_env_ready` 144 → `activate_ret` 174 → **`wv_core_ready` 452 (+278: процессы браузера/рендерера, контроллер)** →
`NavigateToString` 453 → rAF×2 → `frame` 599 (+146: рендерер, парсинг HTML/CSS, layout, paint) → 607.

**`winui3-r2r`, medium:** `main` 67 → `parsed` 92 (Markdig JIT-ится) → `app_start` 148 (**+56** против +22 у AOT) →
`window_created` 189 → `first_viewport_built` 219 (+19 JIT рендерера против +2) → `loaded` 366 → `frame` 431 → 436.

**`winui3-fluent`, medium:** `window_created` 142 → `SystemBackdrop = new MicaBackdrop()` → 386 (**+244**) →
`activate_ret` 428 → `loaded` 472 (+43: устройство уже есть) → `frame` 532 → 537.

## 7. Куда уходит время

1. **GPU-устройство (≈100–140 мс на критическом пути, главный пункт).** `tools/d3dprobe.c` (свежий процесс,
   `D3D11CreateDevice`): NVIDIA RTX 5070 — **190–213 мс** на первое устройство в процессе (загрузка и инициализация
   UMD драйвера), 26–45 мс на следующие; AMD iGPU — 44–48 / 23–31 мс; WARP — 15–21 / 1–2 мс. XAML-композитор
   WinUI 3 создаёт устройство на адаптере по умолчанию (здесь NVIDIA) асинхронно после создания окна и ждёт его
   к первому кадру; выбрать адаптер или WARP через API нельзя. С Mica это видно напрямую: +244 мс синхронно в
   `SystemBackdrop`. Прогрев в фоновом потоке не помог (§5).
2. **Фиксированная инициализация WinUI ≈ 100 мс:** XAML core 22 мс, окно 41 мс, `Activate` 31 мс, прочее ~6.
3. **Первый кадр 35–65 мс:** растеризация текста первого экрана (DirectWrite-прогрев не влияет, §5).
4. **Старт процесса ≈ 65 мс до `main`** — почти столько же, сколько весь `baseline-win32`: Defender + 7-МБ AOT-образ.
5. Разбор Markdown и построение XAML первого экрана — **< 10 мс** суммарно, в т.ч. на 3.7 МБ (голова).
6. JIT/R2R (вне AOT) добавляет 40–70 мс, WebView2 — ≈ +220 мс к нативному XAML (старт процессов браузера и рендерера).

## 8. Поддержка спецификации (PROTOCOL §5)

Скриншоты (все просмотрены): `out/shots/winui3-{medium,small,large}.png`, `winui3-webview2-{medium,small}.png`,
`winui3-r2r-{medium,small}.png`, `winui3-fluent-{medium,small}.png`. Середина medium (C#-код, таблица, h5/h6,
картинка, цитата) проверена отладочными скриншотами с `--scrollto`.

| Требование | `winui3` (нативный XAML) | `winui3-webview2` |
|---|---|---|
| Per-monitor DPI v2, клиент 1000×800 DIP, заголовок по протоколу | да (`app.manifest`, `AppWindow.ResizeClient` × DPI) | да |
| Шрифт тела Segoe UI Variable Text 15 px, line-height 1.6, `#1f2328`, колонка 860 / поля 32 | да (`LineHeight=24`, `BlockLineHeight`) | да (`style.css`) |
| h1–h6 размеры, semibold, h1/h2 с линией `#d1d9e0`, h6 `#59636e` | да | да |
| **bold** / *italic* / ~~strike~~ / ссылки `#0969da` (кликабельны, открываются в браузере) | да (semibold 600; зачёркнутое приглушено) | да |
| `inline code`: фон `#eff1f3`, Cascadia Mono | да, фон через `TextHighlighter`; **без скругления**, поля эмулируются тонкими пробелами | да |
| Списки маркированные / нумерованные / вложенные / task lists | да (маркеры • ◦ ▪, свои чекбоксы) | да (нативные checkbox) |
| Цитаты (полоса 4 px, текст `#59636e`), hr | да | да |
| Fenced code: моно 13.5 px, фон `#f6f8fa`, радиус 6, поле 16 | да; **длинные строки переносятся** (без горизонтальной прокрутки — дешевле) | да (прокрутка по CSS) |
| Подсветка синтаксиса | **да**, свой лексер: C/C++/Java, C#, JS/TS, Rust, Python, PowerShell, Bash, JSON, Go | нет (Markdig HTML без подсветки) |
| GFM-таблицы: рамки, жирная шапка, зебра, выравнивание, поля 6/13 | да (`Grid`) | да |
| Локальные PNG | да, если картинка — отдельный абзац (размер резервируется из заголовка PNG, декодирование асинхронное); **inline-картинки внутри текста — заглушка «🖼 alt»** (в корпусе таких нет) | да, все |
| Кириллица / CJK / emoji | да (цветные emoji через fallback) | да |
| Прокрутка | да; клавиатура (стрелки/PgDn/Home/End) сразу после кадра | да |
| Выделение текста | в пределах одного блока (абзаца/ячейки); **через несколько блоков — нет** | да |
| Сырые HTML-блоки | показываются как текст | рендерятся |

**Ограничение прототипа — нет виртуализации.** Каждый блок — живое XAML-поддерево (~100+ КБ вместе с поддержкой
выделения). Последовательное добавление всех 26 580 блоков large.md приводило к зависанию процесса (0 % CPU,
«не отвечает») примерно на 5 200 блоках / 680 МБ, плюс `StackPanel` меряет всех детей на каждом проходе (O(n²)).
Поэтому остаток документа дописывается **по мере прокрутки** (на 12 экранов вперёд, `ScrollViewer.ViewChanged`) и
ограничен 3 000 блоками верхнего уровня с видимой пометкой в конце (medium — 391 блок, целиком). Проверено
прокруткой large.md через UI Automation: процесс отвечает, ~260 МБ. Продукту нужна настоящая виртуализация
(`ItemsRepeater` + `IElementFactory` или свои «чанки» с оценкой высоты).

Не поддерживается ни в одном варианте (нет в пайплайне Markdig): сноски, математика, mermaid, якоря заголовков и
переход по внутренним ссылкам; также нет тёмной темы, масштаба, поиска.

## 9. Подводные камни (на что ушло время)

- **Метапакет `Microsoft.WindowsAppSDK` 1.8** тянет AI/ML-компоненты (onnxruntime, DirectML, ~40 МБ): dist 105 МБ
  против 58 МБ с компонентным `Microsoft.WindowsAppSDK.WinUI`. Для framework-dependent WinAppSDK нужна ещё явная
  ссылка на `Microsoft.WindowsAppSDK.Runtime`, иначе ошибка сборки.
- **`OptimizationPreference`, а не `IlcOptimizationPreference`** (последнее в .NET 8 молча игнорируется).
- **`XamlControlsResources` в unpackaged-приложении без `App.xaml`:** (1) `Application.Resources` в конструкторе
  code-only `App` бросает `E_UNEXPECTED` — словарь добавляется в `OnLaunched`; (2) без PRI приложения
  (`EnableMsixTooling=true` → `fastmd-winui3.pri`) — `COMException: Cannot locate resource from
  'ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml'`, процесс падает с `0xC000027B`. Нужен и
  `IXamlMetadataProvider` на `Application` (`FluentApp`).
- **Mica дорогая**: +180…240 мс на этой машине (синхронное создание GPU-устройства). При тёмной теме Windows Mica и
  кнопки заголовка тёмные — выставлены `TitleBarTheme.Light` и `RequestedTheme=Light`. Программный фокус на
  `ScrollViewer` рисовал рамку вокруг всего документа — `UseSystemFocusVisuals=false`.
- **NativeAOT + MSVC:** `findvcvarsall.bat` → `vcvarsall.bat` вызывает голый `vswhere.exe` — нужен в `PATH`.
- Сборки с разными свойствами должны иметь раздельные `obj` (`BaseIntermediateOutputPath`), а `Compile` — явный
  список: иначе старый `src/obj/**/*.cs` попадает в компиляцию.
- **Нет виртуализации → зависание на больших документах** (§8): прежняя версия прототипа дописывала весь документ и
  на large.md зависала через ~12 с после открытия (на замер не влияло: в bench-режиме выход сразу после кадра).
- `TextBlock` на абзац ⇒ нет выделения через несколько абзацев; `RichTextBlock` с `Paragraph`-ами дал бы его, но
  тяжелее и не вмещает таблицы/код-блоки без `InlineUIContainer`. `TextHighlighter` не умеет скругление/поля.
- Большой AOT-exe (7 МБ) стоит ~15 мс в `CreateProcess` против 0.26-МБ apphost (Defender), но JIT съедает больше.

**Наблюдение по харнессу (не правил):** `bench.py shot` зависает навсегда, если UI-поток приложения завис —
`capture_window` (по-видимому, `PrintWindow`/`SendMessage` без таймаута) ждёт окно. Так было с зависанием из §8 до
исправления; харнесс освободился только после того, как я завершил свой процесс. Предложение: `SendMessageTimeout`
или захват в потоке с таймаутом.

## 10. Идеи дальнейшего ускорения

1. **GPU-адаптер:** на гибридных машинах per-app GPU preference «энергосбережение» (то, что пишет «Параметры →
   Дисплей → Графика», ключ `HKCU\Software\Microsoft\DirectX\UserGpuPreferences`) заставит DXGI отдать композитору
   iGPU: по `d3dprobe` первое устройство 45 мс вместо ~200 — потенциально **−100…150 мс**. Здесь не проверялось
   (менять пользовательские настройки/реестр запрещено правилами); это мог бы делать установщик. На машинах с одной
   дискретной картой не поможет.
2. **Резидентный вариант (`-resident`, PROTOCOL §6):** держать процесс с инициализированным XAML и готовым
   D3D-устройством, новое окно документа — `new Window()` + первый кадр, оценочно 100–150 мс. Единственный путь к
   «пуле» на WinUI; не делался.
3. **Виртуализация** (`ItemsRepeater`/чанки) — не ускорит первый кадр, но уберёт лимит 3 000 блоков и память.
4. Меньше работы в первом кадре: бюджет 1.0 экрана вместо 1.5 — единицы мс (layout ≈0.5 мс/блок).
5. Размер: убрать 87 MUI-папок локализаций WinUI, если не нужны; framework-dependent WinAppSDK (8 МБ) — скорость в
   пределах шума, но нужен установленный Windows App Runtime 1.8.

## 11. Оценка трудозатрат на полноценный продукт на этом стеке

| Блок | Оценка |
|---|---|
| Виртуализация документа (`ItemsRepeater`), якоря, переход по ссылкам, поиск по тексту | 2–3 недели |
| Выделение и копирование через блоки (свой слой выделения или `RichTextBlock` на секцию) | 1–2 недели |
| Картинки (inline, удалённые, SVG), подсветка через TextMate-грамматики, сноски, математика | 2–3 недели |
| Тёмная тема, масштаб, настройки, печать/экспорт, автообновление при изменении файла | 2 недели |
| Ассоциация `.md`, установщик (MSIX или xcopy + регистрация), подпись, автообновление | 1 неделя |
| Итого MVP / отполированный продукт | **~6–8 / 12–14 человеко-недель** (C#-разработчик со знанием WinUI) |

Плюсы стека: родной Fluent-вид, хорошая типографика DirectWrite, C# и Markdig (быстрый, расширяемый), доступность
(UIA) из коробки. Минусы: пол ~370–400 мс холодного старта на этой машине (большая часть — GPU/XAML-инициализация,
кодом не лечится), 58 МБ self-contained или зависимость от установленного Windows App Runtime, нет готового
rich-text-контрола уровня браузера (выделение через блоки, inline-объекты).

## 12. Файлы

| Путь | Что |
|---|---|
| `src/Program.cs` | свой `Main`, аргументы, поток разбора (head-first), инициализация WinAppSDK, `--d3dwarm`, `--dwwarm` |
| `src/App.cs` | окно, first-viewport-first, дозагрузка по прокрутке, метки, Mica/Fluent |
| `src/MdRenderer.cs` | Markdig AST → XAML |
| `src/Highlighter.cs` | лексер подсветки кода |
| `src/Theme.cs` | цвета/шрифты по спецификации |
| `src/WebHost.cs` | вариант WebView2 |
| `src/FastMdWinUI.csproj`, `src/app.manifest`, `nuget.config` | проект (ручки MSBuild описаны в шапке csproj) |
| `build.ps1`, `proto.json` | сборка всех конфигураций, варианты |
| `tools/d3dprobe.c` (+ `.exe`) | замер стоимости создания D3D11-устройств (`bench/tools/msvc.cmd cl /nologo /utf-8 /O2 d3dprobe.c d3d11.lib dxgi.lib dxguid.lib`; запуск `d3dprobe hw|warp|<adapter> [count]`) |
| `out/quick.json`, `out/baseline-ref.json` | итоговый быстрый прогон 4 вариантов × 3 документа + калибровка |
| `out/knobs-medium.json` (+ более ранний `knobs-medium-1.json`), `knob-wvargs.json`, `knob-ilcpref.json`, `knob-dwwarm.json`, `knob-head-large*.json`, `knob-warm.json` | сырые данные ручек |
| `out/shots/*.png` | скриншоты |
| `tools/pixelprobe.py`, `out/audit-run.json`, `out/shots/audit-*.png` | инструменты и данные аудита (§13) |

## 13. Аудит (независимая проверка, 2026-09-19)

Проверял отдельный агент-аудитор: исходники, сборку, метки, скриншоты, прогоны харнесса и отдельная пиксельная
проверка «когда документ действительно на экране». Итог: **замер честный, все 4 варианта годятся для финального
прогона**; исправлен один дефект (падение `winui3-webview2` при выходе), на время старта он не влиял.

### 13.1 Что проверено

| Пункт | Результат |
|---|---|
| `t_content` по PROTOCOL §3 | ок. Нативный XAML: `Loaded` корня → первый `CompositionTarget.Rendered` → `DwmFlush()` → запись (`App.OnRootLoaded` / `OnFirstRendered` / `Presented`, `FastMdBench.ContentPresented`). WebView2: страница `rAF(rAF(postMessage))` → `WebMessageReceived` → тот же `Presented` → `DwmFlush()`. Раньше ничего не метится (метки `parsed`, `window_created`, `activate_ret` — только разбивка) |
| Нет резидента / кэша / пре-рендера | ок. Один процесс (у WebView2 — 5 процессов в Job), ничего не стартует заранее, нет файлового кэша. Из общего в сборку вшит только `bench/common/style.css` (так требует протокол для веб-вариантов). Из корпуса ничего не вшито (grep по `src/`: ни имён, ни содержимого документов). Профиль WebView2 (`%LOCALAPPDATA%\FastMD\winui3-webview2`) переживает запуски — это обычный UDF, документ идёт через `NavigateToString`, не из кэша |
| First viewport first и head-first parse | легитимно (§3 протокола разрешает). Файл читается целиком (`File.ReadAllText`, в т.ч. 3.7 МБ), первый кадр — настоящий рендер первых 13–14 блоков; голова large.md режется по пустой строке вне fenced-кода, последний блок головы не используется, при ссылочных определениях после среза — перерисовка |
| Release / оптимизация | ок. `dotnet publish -c Release`; в `obj/aot/.../fastmd-winui3.ilc.rsp` есть `--Ot` + 6 `--mibc` (т.е. `OptimizationPreference=Speed` действует); у `r2r` есть `obj/r2r/.../R2R`, `Markdig.dll` 1.4 МБ и `Microsoft.WinUI.dll` 16.7 МБ — R2R-образы. `build.ps1` полностью пересобран аудитором с нуля: 5 конфигураций за 52 с, без ошибок |
| `bench.py check` medium / large, все 4 варианта | все `CHECK OK`, предупреждений нет (кроме исправленного exit code, см. 13.2). `observed_ms` ≈ `window_ms` − 3 мс (окно видно в момент `Activated`) и всегда < `content_ms` — противоречий нет, но и проверить `t_content` этим нельзя (см. 13.3) |
| Окно, DPI, заголовок | клиент 1000×800 при 100 % (скриншот 1002×832 с рамкой), PerMonitorV2 в манифесте, заголовок `medium.md — FastMD (winui3)` |

### 13.2 Найдено и исправлено: `winui3-webview2` падал при выходе (0xC000027B)

В bench-режиме после записи результата `Presented()` вызывает `Application.Exit()`, но обработчик
`WebMessageReceived` (`WebHost.cs`) после этого всё равно ставил в `DispatcherQueue` отправку остатка документа
`PostWebMessageAsString` в уже закрывающийся WebView2 → исключение в колбэке диспетчера → stowed exception,
процесс завершался с кодом **0xC000027B** (`CoreMessagingXP.dll`, журнал Application, событие 1000) **и создавал
отчёт WER**. В `out/quick.json` строителя так завершились **12 из 18** запусков `winui3-webview2` (все small/medium;
large — нет, там остаток готов позже). На `t_content` это не влияло (результат уже записан), но нарушало PROTOCOL §4
(«exit cleanly, exit code 0»), а `WerFault` работает вне Job Object и мог шуметь в соседних замерах финального
последовательного прогона.

Исправление (минимальное, после `t_content`): `App.Exiting` выставляется перед `Exit()`; `WebHost` не ставит
отправку остатка, если приложение выходит, и оборачивает `PostWebMessageAsString` в `try/catch`. Пересобраны все
конфигурации (`build.ps1`). Проверка: 4 `check` подряд → exit 0; в `out/audit-run.json` все 60 запусков
(5 вариантов × 2 документа × 6) — `exit_code 0`, `clean_exit true`.

### 13.3 Независимая проверка: когда документ реально на экране (`tools/pixelprobe.py`)

`observed_ms` харнесса видит только появление окна (WinUI показывает окно в момент `Activate`, ~140 мс, а документ —
через ~230 мс после этого), поэтому сделал отдельный пробник: запуск варианта как в харнессе (Job,
`FASTMD_BENCH_EXIT=0`), опрос **композированного** экрана (`BitBlt` с экранного DC = то, что показывает DWM) по полосе
клиентской области каждые ~7 мс; через 1.2 с после отчёта берётся эталон, и ищется первый захват, совпадающий с ним
(≥85 % «тёмных» пикселей текста эталона тёмные, ≥95 % белых — белые, допускается сдвиг ±8/±3 px). Интервал
появления: (начало последнего несовпавшего захвата; конец первого совпавшего]. Монитор 165 Гц (кадр 6 мс).

| Вариант / документ | n | документ ещё НЕ на экране через … после `t_content` | уже на экране не позже … после `t_content` |
|---|---|---|---|
| `winui3` / medium | 8 | +5.6 … +15.7 мс | +24.8 … +37.4 мс |
| `winui3` / small | 3 | +5.9 … +7.5 | ≈ +27 (медиана) |
| `winui3` / large | 3 | +6.5 … +29.6 (медиана +7.0) | ≈ +33 (медиана) |
| `winui3-r2r` / medium | 3 | +7.9 … +13.4 | ≈ +33 (медиана) |
| `winui3-webview2` / medium | 3 | +0.8 … +7.4 | +19.7 … +25.2 |
| `winui3-fluent` / medium | 1 (таймлайн) | +19.4 (документ появляется серым и ~80 мс «проявляется» — анимация DWM) | +37 (полная яркость ≈ +100) |
| **эталон другого стека:** `cpp-webview2` / medium | 2 | −7.6 / +19.9 | +12.8 / +45.7 |

Вывод: у всех композиторных стеков (XAML, WebView2) фотоны появляются на 1–4 кадра (≈ +6…+35 мс) позже
`DwmFlush()`, и `winui3` здесь не выделяется среди стеков, следующих той же процедуре §3 (`cpp-webview2`,
`winui3-webview2`). Метка **не раньше протокола и не систематически раньше других** — не исправлял (сдвиг
метки только у WinUI сделал бы сравнение нечестным в обратную сторону). У `winui3-fluent` (Mica) лаг на ~15 мс
больше, т.е. его «цена Fluent» в таблицах ещё чуть занижена. Для `baseline-win32` (пустое окно без документа)
проба меряет только конец анимации открытия окна DWM (+42…+68 мс после его `t_content`) — это одинаковая для всех
задержка оконного менеджера, не приложения. Запуск: `python bench/protos/winui3/tools/pixelprobe.py winui3 --doc
medium --runs 3 [--timeline] [--save-dir DIR]`.

### 13.4 Замечания (не влияют на честность)

- **Чёрное окно до первого кадра.** Таймлайн пробника: с момента появления окна (~140 мс) до документа (~370 мс)
  клиентская область **чёрная** (белых пикселей 0 %), потом сразу документ. Для «стильного» просмотрщика это заметная
  вспышка; `t_window` (событие `Activated`) формально «что-то нарисовано», но это чёрный фон DWM, а не белая
  страница. В продукте стоит не показывать окно до первого кадра (cloak/`ShowWindow` после `Rendered`) или задать фон.
- **Сдвиг вёрстки на 3 px** по горизонтали через кадр-два после первого кадра (колонка перецентрируется, вероятно,
  из-за полосы прокрутки без Fluent-стилей). Пробник это видит (`shift_px=[3,0]`); глазом почти незаметно.
- Полоса прокрутки в нативных вариантах авто-скрывается (стиль WinUI), на стоп-кадрах её не видно; прокрутка
  проверена через `--scrollto` (780/1560/2400 DIP) — код C#/Rust, цитаты, таблицы, картинка, h5/h6, hr в порядке.
- `winui3-webview2` использует system WebView2 Runtime 153 (в `dist_mb` не входит); `winui3-r2r` — установленный
  .NET 8 (тоже не входит).

### 13.5 Контрольный прогон аудитора (`out/audit-run.json`, `--runs 5`, чередование, медиана 5 запусков без первого, машина шумная)

| Вариант | small | medium | t_window | CPU, мс | WS, МБ | процессы | dist, МБ |
|---|---|---|---|---|---|---|---|
| **`winui3`** | **339.6** | **364.1** | 144–151 | 375–422 | 96–99 | 1 | 58.08 |
| `winui3-r2r` | 393.8 | 414.5 | 220–238 | 484–547 | 127–130 | 1 | 100.77 |
| `winui3-fluent` | 480.0 | 503.4 | 380–385 | 438–516 | 109–110 | 1 | 59.37 |
| `winui3-webview2` | 574.9 | 569.2 | 149–160 | 1297–1313 | 95–97 | 5 | 58.08 |
| `baseline-win32` (калибровка) | 78.1 | 75.6 | 70–73 | 47–63 | 14 | 1 | 0.15 |

Совпадает с цифрами строителя (§6) в пределах шума. `check` large: `winui3` 377/424 мс, `winui3-r2r` 455,
`winui3-fluent` 536, `winui3-webview2` 611/561 — large ≈ medium, как и заявлено.

### 13.6 Визуальная оценка (PROTOCOL §5; скриншоты `out/shots/audit-*.png` + прокрутка)

- **`winui3` / `winui3-r2r` / `winui3-fluent` (нативный XAML): 8/10.** Очень близко к GitHub: Segoe UI Variable
  15/24, заголовки h1–h6 с линиями, ссылки `#0969da`, таблицы с рамками/зеброй/выравниванием, код-блоки с фоном,
  радиусом и **подсветкой синтаксиса** (Rust, C#, Python, JS, JSON, PowerShell, C), цитаты с полосой, task-list с
  аккуратными чекбоксами, цветные emoji, кириллица и CJK без «тофу», локальные PNG. Минусы: inline-code без
  скругления, длинные строки кода переносятся (нет горизонтальной прокрутки), выделение только внутри блока,
  inline-картинки — заглушка, чёрная вспышка до первого кадра (не видна на стоп-кадре).
- **`winui3-webview2`: 9/10 по вёрстке** (ровно `style.css`: inline-code со скруглением, нативные чекбоксы, выделение
  через блоки), но **без подсветки синтаксиса** и с CJK-фолбэком браузера.

### 13.7 Что изменено аудитором

| Файл | Изменение |
|---|---|
| `src/App.cs` | поле `Exiting`, выставляется перед `Exit()` в `Presented` |
| `src/WebHost.cs` | не отправлять остаток документа после `Exit()`; `try/catch` вокруг `PostWebMessageAsString` |
| `tools/pixelprobe.py` | новый: независимая пиксельная проверка момента появления документа на экране |
| `out/*` | пересборка `build.ps1`; `audit-run.json`, `shots/audit-*.png` |

`proto.json` не менялся: все 4 варианта валидны для финального прогона.

**Харнесс (не правил, для оркестратора):** (1) `observed_ms` проверяет только видимость окна и у WinUI (окно
показывается чёрным за ~230 мс до документа) не может поймать ранний `t_content`; пиксельная проверка вроде
`tools/pixelprobe.py` была бы полезна в самом харнессе, с оговоркой про анимацию открытия окна DWM. (2) `run` не
показывает ненулевые `exit_code` в сводке — падения при выходе (как 13.2) видны только в `check` или в сыром JSON.
(3) Замечание строителя про `shot`, висящий на зависшем окне (`PrintWindow` без таймаута), по коду правдоподобно;
не воспроизводил.
