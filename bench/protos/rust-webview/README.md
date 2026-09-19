# rust-webview — Rust + wry / Tauri → WebView2

Прототип FastMD на Rust поверх системного WebView2: `pulldown-cmark` превращает Markdown в HTML,
общий `bench/common/style.css` отвечает за типографику, WebView2 (Chromium) рисует страницу.
Здесь четыре варианта: wry с минимальным Win32-окном (лучший), wry с окном tao (каноничная связка),
минимальное приложение на Tauri 2 (чтобы оценить накладные расходы Tauri) и небезопасный
`--single-process` (чтобы понять потолок того, что вообще можно выжать из WebView2).

## TL;DR

| вариант | small | medium | large | t_window | процессов | dist |
|---|---|---|---|---|---|---|
| **rust-webview** (wry + raw Win32) | **558** | **563** | **598** | 93 | 7 | 2.5 MB |
| rust-webview-tao (wry + tao) | 579 | 571 | 603 | 231–260 | 7 | 2.5 MB |
| rust-webview-tauri (Tauri 2.11) | 705 | 731 | 804 | 606–625* | 7 | 5.6 MB |
| rust-webview-single (⚠ `--single-process`) | 462 | 462 | 487 | 92–103 | 3 | 2.5 MB |
| *baseline-win32 (пустое GDI-окно, пол)* | ~90 | | | ~55 | 1 | |

Медиана `t_content` в мс, 5 прогонов после прогревочного, `bench.py run … --runs 5`, файл
`results/quick-3.json`. Машина в этот момент была относительно спокойной, но параллельно шли чужие
сборки, так что точность порядка ±30 мс. \*У Tauri окно видно уже на ~75 мс (`observed_ms`), а
`t_window` мы честно ставим только после `build()`, см. ниже.

**Выводы:**

1. **WebView2 сам по себе стоит ~450–500 мс, и обойти это нельзя.** Из ~560 мс на код прототипа
   приходится ~60 мс: старт процесса, окно, запрос контроллера. Всё остальное внутри WebView2:
   браузерный процесс ~150 мс только собственной инициализации до запуска GPU- и сетевого процессов,
   готовый контроллер на ~360 мс, затем renderer, DOM и первый кадр ещё ~180 мс. Парсинг Markdown и
   подсветка идут в параллельном потоке и в критический путь не попадают.
2. **Rust/wry не прибавляет ничего сверх WebView2**, если не использовать tao наивно. На первом
   `WM_SETFOCUS` tao строит кэш раскладок клавиатуры (~250–400 мс, измерено отдельно). Если окно
   активируется *во время* создания контроллера (наша техника «show in pump»), эта задержка
   прячется за запуском браузера: tao ≈ raw (571 против 563), но `t_window` у tao ~250 мс.
3. **Tauri 2 добавляет ~+150–200 мс** к лучшему wry-варианту: окно tao активируется до WebView,
   а не параллельно с ним, плюс больше init-скриптов и IPC через `invoke`.
4. **`--single-process` экономит ~100 мс**, почти всё на первом кадре: нет запуска GPU- и
   renderer-процессов и межпроцессного обмена кадрами. Но это режим без песочницы, официально не
   поддерживаемый, и только для замеров.
5. **Молнии на WebView2 не получится.** Пол для WebView2 на этой машине ≈ 0.46 с (небезопасно) или
   ≈ 0.55 с (безопасно), а у GDI-окна ≈ 0.09 с. Если нужно открытие «как вспышка», надо рисовать
   нативно (DirectWrite/D2D) или держать резидентный процесс с прогретым WebView2 (см. «Идеи»).

## Стек и версии

- Rust 1.98.1 (x86_64-pc-windows-msvc), release: `opt-level=3, lto="fat", codegen-units=1, panic="abort", strip=true`.
- **wry 0.57.0**, webview2-com 0.39.1, windows 0.62.2 / windows-sys 0.61.2, raw-window-handle 0.6.2.
- **tao 0.37.0**: только в варианте `rust-webview-tao`.
- **Tauri 2.11.5** (tauri-build 2.6.3, tauri-runtime-wry 2.11.4, внутри wry 0.55.1 и tao 0.35.3, default features, без плагинов).
- **pulldown-cmark 0.13.4**: GFM-таблицы, зачёркивание, task lists, сноски, `ENABLE_GFM`.
- **syntect 5.3.0 + two-face 0.5.2** (синтаксисы из bat 0.26), движок **Oniguruma** (onig 6.5.3, собирается crate'ом `cc` с MSVC автоматически).
- WebView2 Runtime 153.0.4234.32 (evergreen), Windows 11 26200.

## Сборка

```
pwsh -File build.ps1          # cargo build --release --workspace и раскладка по out/wry и out/tauri
pwsh -File build.ps1 -Clean   # полная пересборка с cargo clean (~5–8 мин: Tauri, onig, LTO)
```

Результат: `out/wry/fastmd-wry.exe` (2.5 MB, один файл; WebView2Loader слинкован статически) и
`out/tauri/fastmd-tauri.exe` (5.6 MB). `target/` (~2 GB) лежит внутри папки прототипа.
Интернет нужен только при первой сборке, для crates.io.

Структура:

```
Cargo.toml           workspace + release-профиль
mdcore/              общее ядро: протокол бенча (bench.rs), md→html + чанки + подсветка (render.rs),
                     состояние документа и custom protocol (serve.rs), шаблон страницы/JS (lib.rs)
  examples/hlbench.rs  замер подсветки по языкам
wry-app/             fastmd-wry.exe: main.rs, rawwin.rs (Win32-окно), showlater.rs (show-in-pump)
  examples/          taoprobe.rs, rawprobe.rs: пробы стоимости активации окна tao / Win32
tauri-app/           fastmd-tauri.exe: main.rs, build.rs, tauri.conf.json, icons/
results/             quick-3.* (итоговый быстрый прогон), ab-final-medium.* (A/B ручек);
                     quick-1/2 испорчены нехваткой памяти, см. «Грабли»
shots/               скриншоты всех вариантов (small/medium)
```

## Как это работает (критический путь лучшего варианта)

```
t0 CreateProcess ─22ms─► main(≈38) ─► CreateCoreWebView2EnvironmentWithOptions (≈16 мс, loader)
                                   ─► поток рендера: read .md → pulldown-cmark → подсветка → первая страница (≈+15…35 мс)
                                   ─► скрытое Win32-окно (≈5 мс) → PostMessage(WM_SHOW_LATER)
                                   ─► wry::build(): create controller  ── браузер msedgewebview2 стартует (≈+8 мс)
                                        └ внутри wait_with_pump: WM_SHOW_LATER → ShowWindow+UpdateWindow → t_window (≈93)
                                   ─► контроллер готов (≈360) → Navigate("http://fastmd.localhost/")
                                   ─► WebResourceRequested → отдаём страницу из памяти (≈2 мс)
страница: CSS инлайн + первые ≥64 KB HTML + скрипт → DOM ready (≈+50) → rAF → rAF (≈+120)
         → chrome.webview.postMessage('painted …') → DwmFlush() → t_content (≈560)
после t_content: fetch('/__fastmd/rest') → остаток документа кусками по 512 KB (insertAdjacentHTML + setTimeout)
```

Хронология процессов (мс от создания нашего процесса, спокойная машина, `medium`):

| событие | мс |
|---|---|
| `main()` | 38–41 |
| env запрошен / окно создано, контроллер запрошен | 54–57 / 59–62 |
| msedgewebview2 (browser) создан | 67–70 |
| crashpad-handler | 102–114 |
| **gpu-process + NetworkService** | **219–228** (≈150 мс собственной инициализации браузера) |
| StorageService | 247–257 |
| renderer | 326–337 |
| контроллер готов (`webview_built`), навигация | 360–373 |
| DOM готов (`js_dom_ready`) | 410–430 |
| 1-й rAF / 2-й rAF | 518–536 / 535–554 |
| `t_content` (после DwmFlush) | 547–565 |

## Варианты (proto.json)

| id | аргументы | что это |
|---|---|---|
| `rust-webview` | `--id rust-webview` | **Лучший безопасный.** wry 0.57 + минимальное Win32-окно, show-in-pump, env создаётся первым, custom protocol, первый чанк 64 KB, подсветка в потоке рендера, `chrome.webview.postMessage`, постоянный UDF `%LOCALAPPDATA%\FastMD\wry`, песочница включена. |
| `rust-webview-tao` | `--tao` | То же, но окно из tao 0.37, как в примерах wry. Тоже show-in-pump: окно создаётся невидимым, `set_visible(true)` вызывается внутри pump через subclass. |
| `rust-webview-tauri` | — | Минимальное приложение на Tauri 2: `register_asynchronous_uri_scheme_protocol("fastmd")`, `WebviewWindowBuilder` в `setup`, IPC через `__TAURI_INTERNALS__.invoke('msg')`, та же страница из mdcore. Для одинаковых скриншотов заданы FluentOverlay-скроллбар и светлая тема. UDF `%LOCALAPPDATA%\FastMD\tauri`. |
| `rust-webview-single` | `--data wry-single --extra-args --single-process` | ⚠ **Небезопасно, только для замеров.** Chromium `--single-process`: renderer и GPU внутри браузерного процесса, без песочницы. Отдельный UDF. |

Флаги `fastmd-wry.exe` для экспериментов: `--tao`, `--show-first` (показать окно до контроллера),
`--late-env` (env создаёт сам wry), `--env-first` (диагностика), `--nav html` (NavigateToString
вместо custom protocol), `--chunk N` (первый чанк в байтах, 0 = весь документ), `--piece N`,
`--wry-ipc` (`window.ipc` из wry вместо `chrome.webview`), `--private` (InPrivate),
`--browser-args "…"` / `--extra-args "…"`, `--data <подпапка UDF>`, `--no-highlight`,
`--default-scrollbar`, `--tracking-prevention`, `--shared-udf`, `--eval <js>` (отладка и скриншоты).
Переменная `FASTMD_TRACE=<файл>` пишет события после `t_content` (например `rest_done`).

## Поддержано и чего нет относительно спеки (PROTOCOL §5)

Поддержано:
- Заголовки h1–h6, абзацы с переносом, **жирный**, *курсив*, `inline code`, ~~зачёркивание~~, ссылки.
  Внешние ссылки открываются в браузере по умолчанию (NavigationStarting → ShellExecute).
- Маркированные, нумерованные, вложенные и task-списки. Классу `contains-task-list`, который есть
  в style.css, в pulldown-cmark нет аналога, поэтому мы проставляем его сами.
- Цитаты, `hr`, GFM-таблицы с выравниванием, сноски.
- Fenced code: моноширинный Cascadia Mono, фон, без переноса, горизонтальный скролл. Есть
  **подсветка синтаксиса** (syntect + Oniguruma, цвета в духе GitHub, компактные `<i class=k>`):
  rust, c, csharp, js/ts, json, python, powershell и ещё около 200 языков. Неизвестный язык
  выводится обычным текстом.
- Локальные картинки по относительным путям: custom protocol отдаёт файлы рядом с `.md`, выход через `..` запрещён.
- Unicode: кириллица, CJK, цветные emoji (Segoe UI Emoji) работают через шрифтовой fallback Chromium.
- PMv2 DPI, клиентская область 1000×800 DIP, окно показывается активным, заголовок по протоколу,
  светлая тема, прокрутка, Fluent overlay scrollbar, Esc закрывает окно, Ctrl+колесо масштабирует.
- Большие файлы: первый чанк не меньше 64 KB HTML режется по границе блока верхнего уровня, остаток
  догружается после первого кадра. Для `large.md` (3.7 MB) `t_content` почти такой же, как для
  `small`. Полностью документ (7 MB HTML) дорисовывается за ~3.1 с после старта (`rest_done`),
  интерфейс всё это время живой.

Нет или ограничено:
- Якоря у заголовков и оглавление не сделаны: pulldown-cmark не генерирует id.
- Нет GFM-автоссылок на голые `www.`/`http://` (pulldown-cmark их не поддерживает). Нет алертов `> [!NOTE]`.
- Нет поиска, тёмной темы, перечитывания при изменении файла, drag&drop.
- Сырые HTML-блоки из Markdown попадают в страницу как есть. Для вьювера нужна CSP/санитизация, см. «Идеи».
- `t_window` у tao и Tauri консервативный: ставится после возврата `set_visible`/`build()`, хотя
  окно видно раньше (`observed_ms` ≈ 75–95 мс).

## Быстрые числа (шумные)

`results/quick-3.json` (медианы, мс от CreateProcess):

| вариант / doc | content | window | main | контроллер готов | page served | DOM | rAF1 | rAF2 | ipc |
|---|---|---|---|---|---|---|---|---|---|
| rust-webview / small | 558 | 93 | 34 | 365 | 370 | 429 | 543 | 551 | 555 |
| rust-webview / medium | 563 | 94 | 41 | 367 | 371 | 419 | 536 | 553 | 554 |
| rust-webview / large | 598 | 105 | 37 | 391 | 394 | 447 | 576 | 593 | 594 |
| rust-webview-tao / medium | 571 | 250 | 40 | 391 | 392 | 437 | 545 | 563 | 565 |
| rust-webview-tauri / medium | 731 | 606 | 43 | 606 (окно+webview) | 607 | 622 | 622 | 714 | 726 |
| rust-webview-single / medium | 462 | 101 | 36 | 374 | 385 | 396 | 439 | 452 | 453 |

Прочее из того же прогона: CPU всей job ≈1.6 с (7 процессов) против ≈1.05 с у single; peak commit
≈185 MB; working set нашего процесса 26–34 MB; первый запуск на уже существующем профиле почти не
медленнее. На **новом** UDF первый запуск создаёт профиль, это ~+0.3–1 с.

### A/B по ручкам (medium, 6 прогонов вперемешку, спокойная машина: `results/ab-final-medium.*`)

| конфигурация | median t_content | Δ к лучшему |
|---|---|---|
| **лучший** (raw + show-in-pump + custom protocol + подсветка) | **565** | — |
| `--show-first` (окно показано до запроса контроллера) | 614 | +49 |
| `--tao --show-first` (наивный tao) | 737 | +173 |
| `--no-highlight` | 581 | ≈0 (подсветка бесплатна) |
| `--nav html` (NavigateToString) | 600 | +36 (в других сериях −20…−40: в пределах шума) |
| `--extra-args --no-sandbox` | 573 | ≈0 |
| `--extra-args --single-process` | ≈462 (quick-3) | −100 |

Ранние серии шли под сильной параллельной нагрузкой, абсолютные значения там 650–800 мс, но
знак эффекта надёжен:
- `--disable-gpu --disable-gpu-compositing`: хуже (+135). `--use-angle=warp`: ≈0. `--in-process-gpu`: ≈0…+40.
  `--disable-gpu-sandbox`: ≈0…+50.
- `--enable-features=NetworkServiceInProcess2`: на один процесс меньше, по времени ≈0.
  `--disable-crash-reporter --no-crashpad`: ≈0.
- Набор «фоновых» флагов (`--disable-background-networking --disable-component-update
  --disable-sync --no-first-run --disable-domain-reliability …`, `--disable-features=Translate,
  OptimizationHints,MediaRouter,AutofillServerCommunication,CalculateNativeWinOcclusion,
  RendererCodeIntegrity,SpareRendererForSitePerProcess`): ≈0.
- `--private` (InPrivate): ≈−50…0, шум. `--wry-ipc`: ≈0. `--late-env`: ≈0 (см. грабли).
- Первый чанк 16 KB / 64 KB / весь документ: для medium разницы нет. Для large весь документ
  означал бы 7 MB HTML до первого кадра.
- DOM → первый кадр ≈130–150 мс независимо от содержимого (ASCII, без CJK/emoji, small):
  это инициализация композитора и GPU, а не layout и не шрифты.

## Куда уходит время

1. **Браузерный процесс WebView2, ~300 мс от запроса до готового контроллера.** Около 150 мс он
   занят собой (загрузка msedge.dll, prefs, variations, профиль) и только потом запускает GPU-,
   сетевой и storage-процессы. Renderer появляется лишь при навигации. Ни один безопасный флаг это
   заметно не ускорил.
2. **Первый кадр, ~180 мс от ответа на запрос страницы.** Примерно 50 мс до DOM ready (renderer
   запускается и разбирает HTML), примерно 120 мс до первого rAF (композитор и GPU-канал). В
   single-process из этого остаётся ~70 мс.
3. **Наш код, ~60 мс до запроса контроллера**: CreateProcess ~22 мс, Defender в том числе; до
   `main` ~15 мс; loader WebView2 ~16 мс; скрытое окно ~5 мс. Парсинг и подсветка (medium ~20 мс,
   large ~50 мс для первого чанка и ~500 мс на весь документ) идут параллельно и никогда не
   ограничивают.
4. **IPC и DwmFlush, ~5–10 мс.**

## Идеи для дальнейшего ускорения

- **Резидентный режим (`-resident`)** — единственный путь к ~100–200 мс на WebView2. Держать
  environment и прогретый скрытый контроллер (renderer уже живой), новый файл открывать как
  Navigate и show. Техника не зависит от языка, ср. `cpp-webview2-resident` у соседнего прототипа.
  На Rust это ~1–2 дня: named pipe или `WM_COPYDATA`, трей, выгрузка по таймеру.
- **Гибрид.** Сразу рисовать первый экран нативно (DirectWrite по тому же AST), пока стартует
  WebView2, потом подменить. По протоколу это допустимо, только если нативный кадр честно
  отрендерен по спеке. Фактически это второй рендерер, то есть дорого.
- `ICoreWebView2CompositionController` (visual hosting) вместо HWND-хостинга: возможно, убирает
  синхронизацию с дочерним окном браузера. Не проверялось.
- Прогрев page cache и prefetch msedge.dll при логоне: это системная настройка, не для продукта.
- Прямой webview2-com вместо wry почти ничего не даст: wry не держит критический путь, с
  `with_environment` и show-in-pump всё нужное доступно.

## Оценка трудозатрат до полноценного продукта

MVP вьювера на этом стеке: 1.5–2 недели одного разработчика. Туда входят ассоциация `.md` и
single-instance, drag&drop, перечитывание по изменению файла (`notify`), поиск (WebView2 Find API
или JS), тёмная тема (`prefers-color-scheme` уже работает через CSS), якоря и оглавление, печать
и PDF (`PrintToPdf`), CSP и санитизация сырого HTML, настройки, установщик (MSIX или Inno). Ещё
неделя на Mermaid и KaTeX (ленивые JS-бандлы из custom protocol) и полировку. Резидентный режим:
+1–2 дня. Чистого Rust ~1.5–2 KLOC. WebView2 Runtime в Windows 11 уже есть.

## Грабли (на что наступили)

- **tao 0.37: первая активация окна стоит 250–450 мс.** На `WM_SETFOCUS` вызывается
  `gain_active_focus` → `LAYOUT_CACHE.get_current_layout()` → `prepare_layout`, а это тысячи
  `ToUnicodeEx` по всем клавишам и модификаторам. Проба `examples/taoprobe.rs`: focused 287–454 мс
  против unfocused 80–87. Голое Win32-окно (`examples/rawprobe.rs`) активируется за 30–45 мс.
  То же касается winit (tao его форк) и Tauri.
- **`CreateCoreWebView2EnvironmentWithOptions` завершается мгновенно** (`--env-first`: callback
  приходит в ту же миллисекунду), а браузерный процесс запускается только при создании
  **контроллера**. Поэтому «ранний env» сам по себе ничего не даёт. Выигрыш дают только ранний
  HWND и показ окна **внутри** pump'а создания контроллера (`showlater.rs`): −50 мс.
- `ShowWindow` во время старта браузера может блокироваться: дочернее окно WebView2 живёт в чужом
  процессе, input-очереди связаны. Поэтому `t_window`, поставленный после возврата, консервативен.
- `with_ipc_handler` в wry добавляет `AddScriptToExecuteOnDocumentCreated` с ожиданием
  round-trip до навигации. Мы вешаем `add_WebMessageReceived` сами и шлём через
  `chrome.webview.postMessage`: никакого init-скрипта, и сообщение не теряется, потому что
  обработчик регистрируется до запуска message loop.
- По умолчанию wry и WebView2 кладут UDF рядом с exe (`<exe>.WebView2`), то есть мусорят в `dist`.
  Мы явно задаём `%LOCALAPPDATA%\FastMD\<вариант>`. Разные browser args требуют разных UDF, если
  экземпляры живут одновременно.
- NavigateToString: лимит ~2 MB, origin `about:blank`. Относительным картинкам нужен
  `<base href="http://fastmd.localhost/">`, а `#якоря` при `<base>` уводят со страницы. Поэтому
  по умолчанию страница отдаётся через custom protocol с того же origin.
- Tauri 2: `WebviewUrl` импортируется из `tauri::`, а не из `tauri::webview::`. tauri-build требует
  `icons/icon.ico`. По умолчанию скроллбар классический и тема тёмная, поэтому для идентичных
  скриншотов нужно задать FluentOverlay и Light. Custom scheme на Windows выглядит как
  `http://<scheme>.localhost/` и для IPC считается «локальным», так что app-команды работают без capabilities.
- pulldown-cmark делает первый проход по **всему** документу (блочная структура), до первого
  события. Для large это ~14 мс. Чанкинг экономит HTML-генерацию и layout, но не этот проход.
- syntect: с `regex-fancy` первая подсветка каждого языка стоит 14–57 мс (компиляция regex, ~160 мс
  на первый чанк medium), а PowerShell в two-face с fancy недоступен. С **Oniguruma** 1.6–4 мс на
  язык. Штатный `ClassedHTMLGenerator` вкладывает по span на каждый scope, и HTML раздувается вдвое
  (остаток large 13.4 MB вместо 6.6). Свой компактный вывод даёт 7.0 MB.
- **Шум окружения.** Чужой процесс из общего scratchpad (`qtw\sampler.exe`) дважды съедал 44–46 GB
  RAM, свободной памяти оставалось <2 GB. Старт msedgewebview2 при этом стабильно зависал на
  +2.1 с (браузер создавался вовремя, GPU-процесс только через ~2.2 с), у Tauri на +4 с, а одна
  пустая прогонка дала `main` через 9.8 с. Прогоны `results/quick-1-INVALID-memory-pressure.*` и
  `quick-2-NOISY.*` испорчены. Валидный `quick-3` снят после освобождения памяти.

## Аудит (независимая проверка, 2026-09-19)

Проверял отдельный агент-аудитор. Параллельно работали сборки других агентов, поэтому все числа ниже
шумные и годятся только для относительного сравнения.

### Что проверено

| пункт | результат |
|---|---|
| `t_content` по PROTOCOL §3 | ✅ `page_script()` (mdcore/src/lib.rs) стоит в конце `<body>`, после `<article>` с первым чанком. Затем rAF → rAF → `chrome.webview.postMessage` (в Tauri `invoke`) → `handle_message` → `bench::content_presented()`: `DwmFlush()` вызывается в UI-потоке процесса, который владеет окном, и **сразу после** него берётся `t_content`. Раньше ничего не отмечается. |
| Резидентность, кэш, пререндер | ✅ Резидентных процессов нет: после прогонов нет ни одного `msedgewebview2` с UDF `FastMD\wry\|tauri`. HTML строится заново при каждом запуске в потоке рендера. Ответы custom protocol идут с `Cache-Control: no-store`. Постоянный UDF — обычный профиль WebView2, такой же будет у реального приложения. |
| Данные корпуса в бинарнике | ✅ Нет. `include_str!` подключает только `bench/common/style.css`. Синтаксисы syntect/two-face — это стандартные дампы, к корпусу отношения не имеют. |
| Release-сборка | ✅ Профиль `opt-level=3, lto="fat", codegen-units=1, panic="abort", strip`. `cargo build --release --workspace` был актуален: у `out/wry/fastmd-wry.exe` тот же sha1, что у `target/release`. Tauri-exe пересобирается при каждом `cargo build`, sha1 отличается, но размер тот же: `generate_context!` каждый раз генерирует новый случайный invoke key. |
| `check` medium/large, все 4 варианта | ✅ Всё `CHECK OK`, предупреждений нет, `clean_exit=true`, `exit_code=0`. `observed_ms` (66–86 мс, у Tauri 74–122) всегда **раньше** `t_window` и `t_content`, признаков слишком ранней метки нет. |
| Когда контент реально появляется на экране | ✅ Для проверки написан `audit/pixwatch.py`: BitBlt верхней полосы клиентской области с экранного DC (то, что реально скомпоновал DWM) в цикле, ~12 мс на кадр захвата. Прогоны wry, tao, single, Tauri (17 штук): кадр с текстом появляется **в пределах одного интервала захвата от `t_content`**. В 15 из 17 прогонов последний пустой кадр начат до `t_content`. В 2 прогонах пустой кадр был начат через +2.7 мс (wry) и +15.6 мс (tao) после `t_content`, то есть метка может опережать экран на 1–3 кадра при 165 Гц. Так устроен сам double-rAF для WebView2, это одинаково для всех web-прототипов, систематического «раннего» смещения нет. У Tauri метка даже чуть поздняя: текст на экране за ~10 мс до `t_content`, потому что `invoke` медленнее. |
| Первый чанк и дозагрузка | ✅ `audit/domcheck.py` считает элементы итогового DOM после дозагрузки и сравнивает с исходником. small, medium и large совпадают **полностью** (large: h2=1659, h3=3318, pre=1659, table=1659, img=34, checkbox=6636). Висячего текста на верхнем уровне нет, битых картинок нет, все чекбоксы внутри `ul.contains-task-list`. Первый кадр `large` — настоящий стилизованный документ (`shots/audit-rust-webview-large.png`). |
| Скриншоты | ✅ `shots/audit-*.png`. Ниже заголовка окна tao и Tauri попиксельно совпадают с `rust-webview`, у single отличается 71 пиксель (антиалиасинг). |

### Что исправлено

1. **mdcore/src/render.rs, `TaskListClass`: латентная ошибка в балансе глубины чанкера.** `Chunker`
   узнаёт замену `<ul class="contains-task-list">` по указателю (`std::ptr::eq(*h, TASK_UL)`), а
   `TaskListClass` отдавал отдельный строковый литерал с тем же текстом. Работало это только потому,
   что LLVM/rustc склеили одинаковые константы. Без склейки (debug-сборка, другой codegen) глубина
   после первого task-списка ушла бы в −1, и документ резался бы посреди блока: висячие `</h2>`,
   текст вне элементов. Теперь `TaskListClass` отдаёт сам `TASK_UL`. После `pwsh -File build.ps1`
   все проверки прогнаны заново: `check` OK, `domcheck` для medium и large всё совпадает. На
   скорость это не влияет, код-путь тот же.

Больше ничего не менялось, ни в логике меток, ни в `proto.json`.

### Что не исправлено, но важно для итогового сравнения

- **`t_window` у `rust-webview-tauri` — верхняя граница** (~510–960 мс), ставится после `build()`.
  Окно видно (`observed_ms`) уже на ~75–120 мс, а белая полоса на экране появляется на ~190–210 мс
  так же, как у wry (анимация открытия DWM). Точный момент первой отрисовки фона в Tauri без
  вмешательства в tao не поймать, поэтому метку я не трогал. Для окна у всех вариантов лучше
  сравнивать `observed_ms`. На `t_content` это не влияет.
- **`t_window` у `rust-webview-tao` (~200–240 мс)**, скорее всего, **честная**: окно видимо на ~80 мс,
  но UI-поток заблокирован в `WM_SETFOCUS` tao (кэш раскладок), и фон рисуется только после этого.
- **`rust-webview-single` не кандидат для продукта.** Chromium `--single-process` без песочницы,
  Microsoft это не поддерживает. Протоколу вариант соответствует, но в итоговой таблице его стоит
  показывать только как «пол WebView2» с явной пометкой, а не наравне с безопасными стеками.
- `--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection` — штатные аргументы wry по
  умолчанию, то есть поведение обычного wry-приложения, а не специальный трюк.

### Визуальная оценка (PROTOCOL §5)

Шрифты, размеры, цвета, колонка 860 DIP с отступами 32 полностью соответствуют спецификации, потому что
используется общий `style.css`. Корректно отображаются h1–h6 (у h1/h2 есть бордюр, h6 серый),
таблицы с выравниванием и зеброй, подсветка кода в стиле GitHub (rust, csharp, python, js, json,
powershell, c), цитаты, `hr`, вложенные списки, task-чекбоксы (disabled, как на GitHub),
кириллица, CJK, цветные emoji и локальные PNG. Мелкие недочёты: вложенный `ol` получает
`margin-bottom:16px` из общего `style.css`, из-за чего после него лишний отступ (так у всех
web-прототипов). Нет автоссылок, алертов `[!NOTE]` и якорей заголовков. **Визуальное качество 9/10,
соответствие GitHub 9/10.**

### Контрольный прогон аудитора (шумный)

`bench.py run rust-webview,rust-webview-tao,rust-webview-tauri,rust-webview-single --doc small,medium --runs 5`,
файл `results/audit-run.json` (медиана 5 прогонов после прогревочного, мс):

| вариант | small | medium | win (med) | obs (med) | CPU job | процессов |
|---|---|---|---|---|---|---|
| rust-webview | 567.5 | 553.0 | 91 | 72–75 | ~1.5 с | 7 |
| rust-webview-tao | 580.7 | 549.3 | 202–239 | 87–88 | ~1.6 с | 7 |
| rust-webview-tauri | 673.8 | 747.4* | 580–605 | 80–81 | ~1.6 с | 7 |
| rust-webview-single ⚠ | 452.8 | 451.4 | 88–90 | 72 | ~1.0 с | 3 |

\*Один выброс Tauri/medium на 3.5 с из-за шума машины (p90 2438). Числа сходятся с `quick-3`
автора с точностью ±30 мс.

Этот прогон сделан **до** пересборки с исправлением `TASK_UL`. После пересборки `check medium` дал
564, 575, 851 (шум) и 457 мс для wry, tao, Tauri и single, всё OK.
Скрипты аудита лежат в `audit/`: `pixwatch.py <variant> <doc> [runs]` и `domcheck.py <doc>`. Они
только импортируют `bench/harness/bench.py` и ничего в нём не меняют.
