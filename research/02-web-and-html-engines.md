# 02. Веб- и HTML-движки для FastMD

Исследование «веб-стека» для ридера Markdown, который должен открываться «как пуля»: WebView2 и всё, что поверх него
(Tauri 2 / wry, Neutralino, Wails), Electron / CEF, а также лёгкие HTML/CSS-движки: Sciter, litehtml, Ultralight,
RmlUi, Blitz, Servo.

- Дата: 2026-09-19. Машина: Windows 11 Pro 26200, Ryzen 9 7950X, 64 GB, NVMe, Defender real-time ON.
- WebView2 Runtime **153.0.4234.32** (evergreen), WebView2 SDK **1.0.4191.47** (static loader).
- Все собственные замеры помечены **[замер]**. Всё остальное — из источников, ссылки в тексте и в §17.
- Лаборатория: `C:/Main/Projects/FastMD/research/lab/web-engines/` (WebView2), `research/lab/blitz/`,
  `research/lab/servo/`, `research/lab/sciter/`. Как повторить — в Приложении.

> **Важная оговорка о шуме.** Замеры делались, пока ~11 других агентов параллельно собирали свои прототипы.
> Одна и та же конфигурация WebView2 (warm UDF, medium.md) давала **~590 мс** при лёгкой нагрузке (00:39)
> и **700–1050 мс** под тяжёлой. Поэтому сравнения вариантов сделаны **чередованием по кругу (round-robin)**,
> с **парными разностями** относительно базового варианта того же раунда (`multi.py` + `paired.py`).
> Абсолютные числа — ориентир. Финальный последовательный замер на простаивающей машине сделает оркестратор.

---

## 0. Коротко (TL;DR)

1. **WebView2 в режиме «новый процесс на каждый double-click» пулей не будет.** [замер] warm UDF, medium.md:
   `t_content` ≈ **590 мс** при лёгкой нагрузке (пол Win32 на этой машине ≈ 90 мс), **700–1000 мс** под нагрузкой.
   Почти всё время уходит на подъём дерева `msedgewebview2.exe` (browser → GPU + utility → renderer, ~285 мс)
   и первый кадр нового renderer'а (~180–240 мс). На сам документ (передача + разбор HTML medium) — ~50 мс.
   Размер документа до 53 KB на время **не влияет**: blank, small и medium совпадают в пределах шума.
2. **`CreateCoreWebView2Environment` почти бесплатен** [замер]: callback приходит через 3–7 мс,
   **ещё до того, как запущен browser-процесс**. Вся цена — в `CreateCoreWebView2Controller`:
   ≈ 285 мс в тихом режиме, 450–600 мс под нагрузкой.
3. **Свежий и переиспользуемый user data folder на NVMe дают одинаковое время** [замер]. InPrivate ничего не ускоряет.
4. **Общий browser-процесс** (другой процесс уже держит тот же UDF с теми же опциями) [замер]:
   `t_content` ≈ **400–545 мс** против 730–1040 мс без него в том же временном окне, то есть **≈ −45–50 %**.
   Остаётся запуск нового renderer'а и короткоживущего процесса-«посредника».
   `SpareRendererForSitePerProcess` заранее запущенный renderer не даёт.
   **Найден дефект runtime 153:** каждый клиент, подключившийся к уже работающему browser-процессу, оставляет
   лишний **`msedgewebview2.exe --type=crashpad-handler`**, который сам не завершается. 4 клиента → 4 «сироты»
   (живы и через 1.5 мин после выхода browser-процесса); 22 клиента → 21 «сирота» возрастом до 15 мин;
   в других сериях было 8 и 13.
5. **Флаги браузера почти ничего не дают** [замер, парные разности]:
   `--in-process-gpu` −90 мс в наборе B (7 раундов, сильная нагрузка), но только **−11 мс** в перепроверке G
   (10 раундов, нагрузка ниже), то есть эффект нестабилен;
   `--disable-gpu` −144 / −57 мс (но это софтверная растеризация, Microsoft прямо против);
   wry-дефолты, `--disable-background-networking`, `msWebView2CancelInitialNavigation`,
   `--edge-webview-no-dpi-workaround` и отключение Tracking Prevention — в пределах шума.
6. **Способ загрузки контента не важен, важно имя хоста** [замер]. NavigateToString, `file://`,
   virtual host и WebResourceRequested укладываются в ±60 мс друг от друга. Но хост в зоне **`.local`**
   (`https://fastmd.local/`) или **одноуровневое имя** (`https://demo/` из примера Microsoft,
   `https://localmdimages/` у PowerToys) добавляет **+1.9 с** на каждую навигацию, и с virtual host, и с
   WebResourceRequested; `--no-proxy-server` не помогает. С `.example`, `.invalid`, `.localhost` задержки нет.
   Лимит NavigateToString: 6.33 млн символов → `0x80070057` (E_INVALIDARG).
7. **Большой документ** (large.md, 3.7 MB, 158 тыс. DOM-узлов) [замер]: Chromium тратит ≈ **3.1 с** и
   ≈ **840 MB** private, Blitz ≈ 5.8 с. First Contentful Paint при потоковой загрузке ≈ 0.5 с.
   Нужна отдача документа кусками: первый экран сразу, остальное потом.
8. **KaTeX / highlight.js / Mermaid** работают в WebView2 без правок [замер, §13]:
   - highlight.js 11.12: 3–7 мс исполнение + 22–33 мс на 24 блока;
   - KaTeX 0.18.7: ~50–80 мс на 80 формул + шрифты ещё через ~110 мс;
   - **Mermaid 12.0.0: 5.5 MB JS, +300–700 мс** (исполнение 90–240 мс + рендер 2 диаграмм 290–480 мс;
     первая загрузка однажды заняла 5 с). Грузить только при наличии блока `mermaid` и после первого экрана.
9. **Blitz (DioxusLabs, Rust: Stylo + Taffy + Parley + Vello)** — самый интересный «тёмный конь» [замер].
   Собственные фазы движка на medium ≈ 40 мс (стили + layout).
   Сборка с CPU-растеризатором: `t_content` ≈ **375 мс** при средней нагрузке и ≈ 560 мс под тяжёлой
   (из них ~90–110 мс — старт процесса), working set 59 MB, один exe 22 MB.
   GPU-сборка (vello_hybrid): ≈ 1.2–1.5 с, съедает инициализация GPU.
   Качество вёрстки на корпусе хорошее. Минусы: статус beta и **нет JavaScript** (KaTeX/Mermaid/hljs только нативными аналогами).
10. **Servo nightly (2026-09-17)** [замер]: ≈ 0.8–1.2 с до контента, WS ≈ 230 MB, дистрибутив 276 MB.
    Видимые баги вёрстки: нумерация `0.` в `<ol>`, пропали bold/italic/semibold с Segoe UI Variable, эмодзи монохромные.
    Для FastMD не готов.
11. **Sciter.JS 6.0.5.0** [замер, предварительный]: `sciter.dll` весит **19.3 MB** (не «5 MB», как в маркетинге).
    `scapp.exe` на medium: окно через 1.7–2.5 с, центральная колонка уезжает (горизонтальный скролл); крошечная страница — 0.47 с.
    Нужен нормальный замер в прототипе `litehtml-sciter`.
12. **Electron** медленнее голого WebView2 по построению: тот же Chromium плюс Node, плюс своя копия Chromium
    (не разделяется с Edge через hardlink), плюс исполнение JS-бандла. Типичные жалобы на MD-приложения: Typora 7–8 с,
    MarkText 3 с (тёплый) / 12 с (холодный), Zettlr до 40 с, Obsidian 4–20+ с.
    Оптимизации Electron 2026 года (Node snapshot, code cache) экономят **десятки мс**, а не секунды.
13. **Вывод:** для требования «пуля» (цель ~150–250 мс до первого экрана) основной рендер должен быть **нативным**
    (Direct2D/DirectWrite; рассмотреть litehtml как layout-движок для HTML/CSS-подмножества).
    Веб-движок годится только как (а) **резидентный** режим с заранее прогретым WebView2
    или (б) ленивый «богатый режим» для Mermaid/KaTeX. Подробности в §15.
14. **Замечание к протоколу/harness** [замер]: 6 раз за несколько сотен запусков процесс «зависал» **до первой строки `main`**
    на 0.56 / 1.9 / 2.3 / 6.3 / 15.1 / 27.1 с. Это задержка ОС (загрузчик, Defender, page-in под нагрузкой), а не стека.
    Прототипы должны ставить метку `main`, а отчёт harness должен показывать `main` и помечать такие выбросы (§16).

---

## 1. Методика

### 1.1 Инструменты (всё в `research/lab/web-engines/`)

| Файл | Что делает |
|---|---|
| `wv2probe.cpp` → `out/wv2probe.exe` | C++ / Win32 + WebView2 (static loader, 268 KB). Ставит метки на каждом шаге: `main`, `env_call`, `env_ready`, `window_created`, `host_first_paint`, `controller_call`, `controller_ready`, `navigate_called`, `dom_content_loaded`, `nav_completed`, `msg_received`, `t_content`. Контент считается показанным по правилу PROTOCOL.md §3: в странице `requestAnimationFrame` ×2 → `postMessage` → хост вызывает `DwmFlush()` → `t_content`. После этого через `ICoreWebView2Environment8::GetProcessInfos` снимает дерево процессов: тип, время создания (`GetProcessTimes`), WS и private bytes. Опции: `--udf`, `--args` (AdditionalBrowserArguments), `--load string\|file\|vhost\|resreq`, `--host`, `--inprivate`, `--notrack`, `--late`, `--hidden`, `--hold`. |
| `drive.py` | Серия запусков одного сценария. `t0` = `GetSystemTimePreciseAsFileTime` прямо перед `CreateProcess`; между запусками ждёт выхода browser-процесса. Режим `--udf shared` поднимает скрытого «держателя» того же UDF. |
| `multi.py` + `paired.py` | Чередование сценариев по кругу (прямой/обратный порядок) и парные разности с базой того же раунда. |
| `gen_pages.mjs` | markdown-it 15.0.2 + task-lists рендерит корпус в HTML со встроенным `bench/common/style.css`. Страницы: `p-*` (готовый HTML), `cm-*` (markdown-it в странице), `hl-*` (highlight.js 11.12.0), `kx-ext` (KaTeX 0.18.7, 80 формул), `mm-ext` (Mermaid 12.0.0, 2 диаграммы), `blank`. |
| `title_watch.py` | Для движков без канала к хосту (Servo, Sciter): страница после rAF×2 ставит `document.title`, скрипт опрашивает заголовок окна раз в ~1 мс и делает скриншот. |
| `proto_run.py` | Мини-раннер по контракту `FASTMD_BENCH_OUT` для сборок вне `bench/protos` (Blitz rdme). |

Страница medium: 95 тыс. символов HTML, 2 344 DOM-узла. Large: 7.1 MB HTML, 158 863 узла.

### 1.2 Ограничения

- Нагрузка на CPU от других агентов (см. оговорку выше).
- Не измерялся по-настоящему холодный старт после перезагрузки: нет прав и нельзя мешать другим.
  «Первый запуск серии» иногда ловил выбросы, но их причина — задержки до `main` (§2.12).
- На этой машине **Edge запущен** (14 процессов `msedge.exe`), и `msedge.dll` (332 MB) у Edge, EdgeCore, Copilot и
  WebView2 одной версии — **один и тот же файл через hardlink** (`fsutil hardlink list`).
  Страницы главной DLL WebView2, скорее всего, уже в памяти. Это благоприятный случай: на машине без
  запущенного Edge холодный старт WebView2 будет медленнее.

---

## 2. WebView2

### 2.1 Процессная модель

По документации ([Process model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/process-model)):
на каждый user data folder (UDF) приходится **одна группа процессов** — один browser-процесс, renderer'ы, GPU и
utility (network service, storage и т. п.). Все процессы привязаны к browser-процессу, а тот — к одному UDF.
Несколько `CoreWebView2Environment` с тем же UDF **и одинаковыми опциями** (в том числе
`AdditionalBrowserArguments`, `Language`) делят одну группу, даже из разных приложений.
При других опциях создание WebView **падает**
([Performance best practices](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance),
обновлено 2026-09-02: «Multiple apps can share a browser process by using the identical user data folder and
CoreWebView2EnvironmentOptions»).

[замер] Дерево для medium.md (WebView2 153, база из набора B, медианы 7 раундов):

| Процесс | Появляется через (от t0), тихо / под нагрузкой | Working set | Private |
|---|---|---|---|
| хост (`wv2probe.exe`) | 0.7 мс (создан) / `main` 35 / 70–110 мс | 21 MB | 4 MB |
| browser (`msedgewebview2.exe`) | 84 / 130–220 мс | 113 MB | 44 MB |
| GPU | 227 / 330–500 мс | 57 MB | 59 MB |
| utility ×2 (`--type=utility`; по документации это сервисы вроде network/storage, `GetProcessInfos` их не различает) | 229–255 / 400–540 мс | 27 + 18 MB | 11 + 8 MB |
| renderer | 336 / 470–670 мс | 73 MB | 37 MB |
| crashpad-handler | в `GetProcessInfos` не виден | — | — |
| **Итого** | 5 процессов + хост | **≈ 320 MB** (WS суммируется с общими страницами) | **≈ 165–185 MB** |

### 2.2 Анатомия запуска [замер]

Прогон в тихом окне (smoke, 00:39, persistent UDF, NavigateToString medium):

| Метка | мс от t0 | Комментарий |
|---|---|---|
| процесс создан | 0.7 | `GetProcessTimes` |
| `main` | 34.5 | как у baseline-win32 (35–40 мс): цена CreateProcess + Defender |
| `env_call` → `env_ready` | 49.1 → 52.5 | **3.4 мс**. Browser-процесса ещё нет |
| окно создано / первый WM_PAINT хоста | 57.5 / 74.8 | нативное окно видно почти сразу |
| `controller_call` | 74.9 | |
| browser-процесс создан | 83.7 | +9 мс после controller_call |
| GPU / utility созданы | 226.9 / 229.3 | **~140 мс инициализации browser-процесса** |
| renderer создан | 336.3 | |
| `controller_ready` | **360.2** | 285 мс на контроллер |
| `navigate_called` (NavigateToString 95 тыс. символов) | 362.9 | чтение файла + UTF-16: 1.2 мс |
| `performance.timeOrigin` страницы | 370.1 | |
| `DOMContentLoaded` (событие хоста) | 442.6 | в странице `responseEnd` = +54 мс, DCL = +57 мс |
| `NavigationCompleted` | 538.3 | |
| rAF×2 в странице → сообщение хосту | 591.6 | rAF×2 = +219 мс от начала навигации |
| **`t_content`** (после DwmFlush) | **598.4** | медиана 3 прогонов: **591.5** |

Выводы:

- **Три части:** ~40 мс старт процесса хоста (как у любого exe); ~285 мс подъём browser/GPU/renderer;
  ~230 мс навигация и первый кадр нового renderer'а. На нативное окно (создание + первый WM_PAINT) — ~25 мс.
- **Первый кадр renderer'а стоит ~180–240 мс при любом размере документа.** Для `blank.html` (12 узлов) и
  medium (2 344 узла) интервал «начало навигации → второй rAF» одинаков: 243 мс в обоих случаях (набор C).
  Это цена первого композиторного кадра (GPU-канал, растр, vsync), а не layout.
  С `--in-process-gpu` и `--disable-gpu` он короче (≈ 190–220 мс).
- Даже NavigateToString на 3 KB отдаёт `responseEnd` через ~50 мс после начала навигации: данные идут через IPC.

### 2.3 Environment против Controller

Распространённое мнение, что «медленно создаётся environment», на runtime 153 **неверно**.
`CreateCoreWebView2EnvironmentWithOptions` возвращает управление за 3–7 мс, callback приходит раньше, чем создан
browser-процесс [замер]. Процесс `msedgewebview2.exe` запускается только в `CreateCoreWebView2Controller`,
и всё ожидание — там. Это совпадает со старыми issue:
[#1910](https://github.com/MicrosoftEdge/WebView2Feedback/issues/1910) (задержка «между вызовом
CreateCoreWebView2Controller и его handler'ом», ~2 с на Surface Go 2) и
[#1540](https://github.com/MicrosoftEdge/WebView2Feedback/issues/1540).

Практика:
- Вызывать `CreateCoreWebView2EnvironmentWithOptions` **первой строкой**, до создания окна. Экономия небольшая
  (набор B: вариант `--late` +15 мс по медиане, в пределах шума), но бесплатная.
- Controller создаётся на уже **видимом** окне. У скрытого окна нет кадров: rAF не срабатывает, `t_content` не наступает.
  На этом я сам поймал зависание «держателя» — см. §2.5.

### 2.4 User data folder, InPrivate, Tracking Prevention [замер, набор B, 7 раундов, парные разности к базе]

| Вариант | t_content, медиана | Δ к базе (медиана) | Лучше базы в раундах |
|---|---|---|---|
| база (persistent UDF в папке лаборатории) | 965 | 0 | — |
| **свежий UDF на каждый запуск** | 847 | +71 | 2/7 |
| InPrivate (`ControllerOptions.IsInPrivateModeEnabled`) | 941 | +154 | 1/7 |
| `EnableTrackingPrevention = FALSE` | 917 | +33 | 3/7 |
| env после показа окна (`--late`) | 904 | +15 | 3/7 |

Первое создание UDF (десятки файлов и папок `EBWebView\...`) на NVMe **не заметно**. Рекомендация Microsoft
держать UDF на локальном быстром диске в `%LOCALAPPDATA%` остаётся в силе
([Performance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance)).
Для ридера InPrivate не нужен и ничего не ускоряет.

### 2.5 Общий browser-процесс, прогрев, резидентность

[замер] Сценарий «держатель»: скрытый процесс-хозяин с тем же UDF и теми же опциями уже работает (набор matrix2, 7 запусков):

| Сценарий | env_ready | controller_ready | t_content | Что создаётся после t0 |
|---|---|---|---|---|
| без держателя (то же окно времени) | 100 | 620–730 | **870–1040** | browser + GPU + 2 utility + renderer |
| **с держателем** | 53 | 203–304 | **437–520 (медиана 460)** | только новый renderer (≈ 180–200 мс) |
| с держателем + `SpareRendererForSitePerProcess` | см. §2.5.1 | | | |

- Экономия ≈ 50 % (~450 мс под нагрузкой). Но и с держателем контроллер стоит ~150–250 мс: запускается
  **короткоживущий** `msedgewebview2.exe`, который находит работающий браузер (process singleton), передаёт
  запрос и выходит, а затем браузер поднимает новый renderer.
- **Дефект:** после каждого такого подключения остаётся **висящий `crashpad-handler`**. 8 запусков → 8 процессов
  с `--type=crashpad-handler --user-data-dir=<наш UDF>`; они жили и после закрытия держателя (снимал сам).
  Для схемы «ридер делит browser-процесс с резидентом» это утечка процессов, которую придётся обходить
  (например, резидент сам хостит окна) или сообщить в WebView2Feedback.
- Ловушка при прогреве: у **скрытого** окна (и у WebView, созданного на невидимом окне) нет кадров,
  `requestAnimationFrame` не вызывается. Rick Strahl описывает то же для Markdown Monster: «The WebView2 control has a
  feature that doesn't fully initialize if it's not UI visible»
  ([West Wind, 2022](https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization)).
  Microsoft рекомендует заранее создавать невидимый WebView2
  ([Performance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance)),
  но готовность надо ждать по `NavigationCompleted`/DOM, а не по кадрам.
- **Полностью резидентный** вариант (процесс с прогретым контроллером и renderer'ом лишь показывает окно и
  подставляет контент) — отдельный прототип `cpp-webview2-resident` у другого агента. По моим данным от него стоит
  ждать уровня «показать окно + `ExecuteScript`/`PostWebMessage` + один кадр»: ~100–200 мс. Это единственный путь,
  где WebView2 приближается к «пуле». Цена — постоянный фон ~170–260 MB private (§2.9) и автозапуск.

#### 2.5.1 Spare renderer и повтор серии [замер, набор E, 7 запусков подряд, одно окно времени]

| Сценарий | controller_ready | t_content (медиана) | Renderer создан после t0 | Private дерева |
|---|---|---|---|---|
| без держателя | 686 | **929** | 518–717 мс (весь набор процессов) | 186 MB |
| с держателем | 306 | **544** | 198–315 мс (только renderer) | 255 MB (2 клиента) |
| с держателем + `--enable-features=SpareRendererForSitePerProcess` | 271 | **503** | 169–315 мс | 254 MB |

Флаг `SpareRendererForSitePerProcess` (из официального списка) **не отдаёт новому клиенту заранее запущенный
renderer**: renderer в каждом запуске создавался после t0. Разница 544 → 503 мс в пределах шума.

**Утечка crashpad-handler, отдельная проверка** (`crashpad_check.py`): держатель поднят → 1 crashpad-handler. После
каждого из 4 клиентов: 2, 3, 4, 5. Держатель убит, browser-процесс вышел → **осталось 4 `crashpad-handler`**, и они
живы через 30 с и через ~1.5 мин. В matrix1 было 13 таких «сирот», в matrix2 — 8. В наборах E и Q за 22 подключения
осталось **21** процесс, самому старому было **14.8 мин**, когда я их снял. Сами они не завершаются.
(Уборка после набора E сначала показала 0, но это ошибка моего фильтра: относительный `--udf-dir` положил UDF в
`out/udf/…`.) Каждый процесс весит ~1–2 MB, но на каждое открытие файла добавляется новый.

### 2.6 AdditionalBrowserArguments [замер, набор B, 7 раундов, парные разности]

| Флаги | Медиана | Δ к базе | Лучше базы | Процессов | Private MB | Комментарий |
|---|---|---|---|---|---|---|
| база | 965 | 0 | — | 5 | 167 | |
| `--in-process-gpu` | **765** | **−90** | 6/7 | 4 | 217 | GPU-поток внутри browser; нет отдельного процесса. Chromium-флаг, в списке WebView2 не документирован |
| `--disable-gpu` | 807 | **−144** | 6/7 | 5 | 124 | Софтверная растеризация. Microsoft: «Don't disable the use of the GPU … except when you're troubleshooting» |
| `--no-sandbox` | 817 | −70 | 5/7 | 5 | 225 | Только для экспериментов: убирает песочницу renderer'а |
| combo (in-process-gpu + wry-дефолты + CancelInitialNavigation + no-bg-network) | 818 | −54 | 5/7 | 4 | 219 | |
| `--enable-features=msWebView2CancelInitialNavigation` | 855 | +22 | 3/7 | 5 | 172 | По документации «to improve startup performance»; эффекта не видно |
| `--disable-background-networking --disable-component-update --no-first-run --disable-sync --disable-extensions` | 885 | +9 | 3/7 | 5 | 173 | |
| wry-дефолты `--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection` | 933 | −26 | 5/7 | 5 | 172 | |
| `--edge-webview-no-dpi-workaround` | 919 | +42 | 3/7 | 5 | 181 | |

**Перепроверка (набор G, 10 раундов, нагрузка ниже, база 794 мс):**

| Флаги | Медиана | Δ к базе (парная) | Лучше базы | Private MB | nav start → rAF×2 |
|---|---|---|---|---|---|
| база | 794 | 0 | — | 186 | 220 |
| `--in-process-gpu` | 749 | **−11** | 6/10 | 218 | 204 |
| `--disable-gpu` | 755 | **−57** | 6/10 | 125 | 182 |

Итог: **даже лучшие флаги дают 0–90 мс в зависимости от условий**. `--in-process-gpu` убирает процесс, но добавляет
~+30 MB private browser-процессу. Выигрыш `--disable-gpu` — это более дешёвый первый кадр без GPU-канала, но
долгий скролл и анимации пойдут в софт. Microsoft прямо пишет, что флаги в продакшене
использовать не следует («might be removed or altered at any time»,
[WebView2 browser flags](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/webview-features-flags),
обновлено 2026-08-31). В issue [#5321](https://github.com/MicrosoftEdge/WebView2Feedback/issues/5321)
(июль 2025) разработчик перебрал флаги производительности, получил «largely unchanged» и не дождался ответа от Microsoft.
Мои данные это подтверждают: **флагами старт WebView2 не лечится**.

### 2.7 Способы загрузки контента [замер, набор C, 5 раундов]

| Способ | Медиана t_content | Δ к NavigateToString | Примечание |
|---|---|---|---|
| NavigateToString | 757 | 0 | origin `null`, относительные картинки **не грузятся**, лимит 2 MB |
| `file:///...html` | 718 | −12 | самый простой; относительные ресурсы работают; ресурсы разрешаются в процессах WebView2 |
| virtual host `https://fastmd.example/` | 678 | −55 | HTTPS-origin, относительные ресурсы, резолв в процессах WebView2 |
| virtual host `https://fastmd.localhost/` | 733 | −24 | так же (Tauri использует `tauri.localhost`) |
| WebResourceRequested `https://fastmd.req/` | 766 | +11 | ответ из памяти хоста; каждый запрос проходит через UI-поток хоста |
| **virtual host `https://fastmd.local/`** | **2 639** | **+1 864** | **mDNS/LLMNR-резолв зоны `.local`** |
| virtual host `.local` + `--no-proxy-server` | 2 662 | +1 883 | прокси (WPAD) ни при чём |
| **WebResourceRequested `https://fastmd.local/`** | **2 772** | **+1 955** | задержка сидит в резолве имени, до перехвата |

- Похоже на [WebView2Feedback #2381](https://github.com/MicrosoftEdge/WebView2Feedback/issues/2381) («2 seconds delay …
  when the domain is non-existent»). Там предлагают прописать имя в `hosts`. **Проще выбрать правильное имя.**
  [замер, набор F, 4 раунда, virtual host, medium]:

  | Имя хоста | t_content (медиана) | nav start → rAF×2 |
  |---|---|---|
  | `fastmd.invalid` | 724 | 171–300 |
  | `fastmd.example` | 862–907 | 231–247 |
  | `tauri.localhost` | 627–827 | 207–270 |
  | **`demo`** (как в примере из документации Microsoft) | **2 690** | **2 130–2 160** |
  | **`localmdimages`** (как у PowerToys) | **2 688** | **2 126–2 175** |
  | **`fastmd.local`** (набор C) | **2 639** | **2 135** |

  Одноуровневые имена (без точки) и зона `.local` уходят в системный резолв (LLMNR/mDNS/NetBIOS) и ждут его таймаут
  ~2 с **на каждую навигацию**. Имена из зарезервированных зон `.example`, `.invalid`, `.localhost` работают без задержки.
  Следствие для PowerToys: картинки его Markdown-превью (`https://localmdimages/...`) должны приходить примерно на 2 с
  позже текста (сам не проверял). Пример из документации Microsoft (`https://demo/index.html`) тоже медленный.
- **Лимит NavigateToString:** 6 330 415 символов → `0x80070057` [замер]. По документации лимит 2 MB, ошибка
  «Value does not fall within the expected range»
  ([Using local content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content)).
  PowerToys меряет **UTF-8 байты** и при > 1 500 000 переходит на временный файл
  ([MarkdownPreviewHandlerControl.cs](https://github.com/microsoft/PowerToys/blob/main/src/modules/previewpane/MarkdownPreviewHandler/MarkdownPreviewHandlerControl.cs),
  фикс [PR #47391](https://github.com/microsoft/PowerToys/pull/47391): раньше проверяли `.Length` в UTF-16 и падали на CJK).
  Rick Strahl: временный файл + `file://` работает «slightly faster»
  ([2024](https://weblog.west-wind.com/posts/2024/Jul/22/Work-around-the-WebView2-NavigateToString-2mb-Size-Limit)).
- Документация Microsoft: WebResourceRequested «slower than other approaches», потому что каждый запрос ждёт UI-поток хоста.
  Для одной HTML-страницы разница не видна (+11 мс); при десятках картинок и скриптов будет видна.
- **Рекомендация для FastMD:** документ отдавать через NavigateToString, пока UTF-8 < 1.5 MB, иначе временный файл
  или virtual host. Картинки и ассеты — через `SetVirtualHostNameToFolderMapping` на хост в зоне `.example`
  (папка документа + папка ассетов приложения).

### 2.8 Размер документа [замер, набор C]

| Страница | DOM-узлов | t_content | nav start → rAF×2 | Private MB (дерево) |
|---|---|---|---|---|
| blank | 12 | 854 | 243 | 153 |
| small (3.4 KB md) | 152 | 864 | 279 | 172 |
| medium (53 KB md) | 2 344 | 757 | 243 | 182 |
| medium, markdown-it **в странице** | 2 251 | 939 | 295 | 186 |
| **large** (3.7 MB md), `file://` | 158 863 | **3 109** | 2 418 | **838** |
| large, markdown-it в странице | 152 230 | 3 688 | 3 112 | 1 030 |
| large через NavigateToString | — | **ошибка 0x80070057** | — | — |

- До ~50 KB Markdown размер не влияет вообще: время определяет старт движка.
- markdown-it в странице для medium: 8 мс на исполнение библиотеки (120 KB), 30 мс на парсинг и рендер,
  4 мс на `innerHTML`, итого +50 мс к rAF×2. Для large: 520–590 мс парсинг + 190 мс `innerHTML` + 2.1 с layout.
  Разбирать Markdown выгоднее в нативном хосте (md4c/cmark-gfm) и отдавать готовый HTML.
- **Потоковая отрисовка работает:** у `file-large` First Contentful Paint = **516 мс** от начала навигации, хотя
  документ целиком готов только к 2.1–2.8 с. Отсюда приём: сначала отдать HTML **первых ~50–100 KB** (первый экран),
  затем дописывать остальное (`PostWebMessage` порциями / `insertAdjacentHTML` / `content-visibility: auto`).
  PROTOCOL.md разрешает это явно.

### 2.9 Память [замер]

- medium.md: всё дерево ≈ **165–185 MB private**, ≈ 320 MB WS (сумма WS завышена общими страницами).
  Хост 4 MB private. `--in-process-gpu`: ~217 MB private (GPU в browser-процессе).
- large.md: ≈ **840 MB private** (готовый HTML), ≈ 1 GB (markdown-it в странице).
- Два окна / два клиента на одном UDF: ≈ 250–260 MB private на два документа (набор shared, 6 процессов).
- PowerToys Peek, по отзывам, держит в простое 100–200 MB
  ([WindowsForum, 2026](https://windowsforum.com/windows-news.4/windows-11-quick-look-quicklook-vs-powertoys-peek-spacebar-preview.402847)).

### 2.10 Evergreen и Fixed Version

- Evergreen **входит в состав Windows 11**, на Windows 10 «the vast majority» устройств его уже имеют
  ([Distribution](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution), обновлено 2026-09-14).
  Для FastMD это **0 MB** к дистрибутиву, плюс статический `WebView2LoaderStatic.lib` (~100–200 KB в exe).
- «On eligible systems, binaries for Microsoft Edge and the Evergreen WebView2 Runtime are hard-linked together when they
  are on the same version. This linking provides benefits for disk footprint, memory, and performance» (там же).
  [замер] Здесь `msedge.dll` 331.9 MB — один файл на 4 пути (Edge, EdgeCore, Copilot, EdgeWebView).
- Fixed Version: «over 250 MB» к дистрибутиву, без автообновлений безопасности. Hardlink с Edge не будет, поэтому и
  выигрыша «Edge уже в памяти» тоже: холодный старт будет медленнее. Microsoft рекомендует evergreen и по
  производительности («Using a fixed version risks missing out on recent optimizations»,
  [Performance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance)).
- Риск evergreen: версия меняется под ногами (здесь рядом лежат 152.0.4191.66 и 153.0.4234.32).
  Нужны тесты на Beta/Dev-каналах и feature-detection новых API.

### 2.11 Известные пользователи WebView2 для Markdown

- **PowerToys (File Explorer preview + Peek):** Markdig → HTML → WebView2. UDF
  `%USERPROFILE%\AppData\LocalLow\Microsoft\PowerToys\MarkdownPreview-Temp`, флаг `--block-new-web-contents`,
  **скрипты выключены** (`IsScriptEnabled=false`), NavigateToString < 1.5 MB, иначе temp-файл; картинки через
  virtual host `localmdimages`
  ([исходник](https://github.com/microsoft/PowerToys/blob/main/src/modules/previewpane/MarkdownPreviewHandler/MarkdownPreviewHandlerControl.cs)).
  Peek работает как фоновый процесс PowerToys (резидентность). Есть жалобы на задержку превью ~400 мс
  ([#46087](https://github.com/microsoft/powertoys/issues/46087), 2026-03) и предложение отдельного «Markdown Reader»
  на WinUI 3 + WebView2 + Mermaid ([#45267](https://github.com/microsoft/PowerToys/issues/45267), 2026-02).
- **Markdown Monster** (WPF + несколько WebView2): кэширует и переиспользует один `CoreWebView2Environment`
  ([2023](https://weblog.west-wind.com/posts/2023/Oct/31/Caching-your-WebView-Environment-to-manage-multiple-WebView2-Controls)),
  ждёт загрузки документа
  ([2025](https://weblog.west-wind.com/posts/2025/May/06/WebView2-Waiting-for-Document-Loaded)),
  воюет с невидимой инициализацией ([2022](https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization)),
  держит временные файлы рендера в `%appdata%\Markdown Monster\temp` (судя по посту про лимит 2 MB,
  превью грузится через файл).

### 2.12 Задержки до `main` (важно для всех прототипов) [замер]

За несколько сотен запусков `wv2probe` шесть раз процесс был создан вовремя (`GetProcessTimes`: 0.7–4 мс от t0), но первая строка
`wWinMain` выполнилась через **27.1 с, 15.1 с, 6.3 с, 2.3 с, 1.9 с и 0.56 с** (обычно — через 22–50 мс). Внутри exe до `main` нет ничего своего (статический CRT
и системные DLL), и WebView2 здесь ни при чём. Это ОС: загрузчик образа, сканирование Defender, page-in при
забитом диске и CPU от параллельных сборок. Без метки `main` такой выброс выглядел бы как
«WebView2 иногда стартует 27 секунд». См. рекомендации к harness в §16.

---

## 3. Tauri 2 и wry

- Версии: **tauri 2.11.5** (2026-07-01, стабильная), **tauri 3.0.0-alpha.1** (2026-09-15), **wry 0.57.0**,
  **webview2-com 0.39.1** (crates.io). В Tauri 3 alpha рантайм выбирается при сборке: `tauri-runtime-wry`
  (системный WebView) **или `tauri-runtime-cef`** (свой Chromium)
  ([release tauri-runtime-cef v3.0.0-alpha.1](https://github.com/tauri-apps/tauri/releases/tag/tauri-runtime-cef-v3.0.0-alpha.1)).
- **Накладные расходы поверх голого WebView2 на Windows.** wry вызывает те же
  `CreateCoreWebView2EnvironmentWithOptions` / `CreateCoreWebView2Controller` через webview2-com, поэтому пол тот же
  (§2.2). Добавляются:
  1. окно через tao/winit и инициализация Rust-рантайма (единицы мс);
  2. IPC-скрипты через `AddScriptToExecuteOnDocumentCreated` (Tauri);
  3. **ассеты приложения идут через custom protocol**. На Windows это `http://tauri.localhost/...`, реализованный
     через **WebResourceRequested** (коммит
     [«custom protocol on Windows now uses the http scheme»](https://github.com/tauri-apps/tauri/commit/4cb51a2d56cfcae0749062c79ede5236bd8c02c2)).
     [замер] Для одной страницы WebResourceRequested против NavigateToString даёт +11 мс, то есть шум. Зона `.localhost`
     ловушку `.local` не задевает (vhost-localhost −24 мс). Для больших ассетов есть жалобы на пропускную способность
     ([#4197](https://github.com/tauri-apps/tauri/issues/4197));
  4. **wry по умолчанию** передаёт `--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection`, и если переопределить
     аргументы, дефолты теряются ([tauri #6416](https://github.com/tauri-apps/tauri/issues/6416)).
     [замер] −26 мс, в шуме.
- **Ожидание:** Tauri/wry ≈ голый WebView2 + 10–50 мс. Точные цифры даст прототип `rust-webview` на общем harness.
- Известные проблемы старта: «белая вспышка» до первого кадра (обход: `visible:false` + показ после готовности),
  на отдельных машинах старт больше 20 с до `setup` без найденной причины
  ([#13727](https://github.com/tauri-apps/tauri/issues/13727), статус «upstream»). Похоже на мои задержки до `main` (§2.12).
- Опубликованные сравнения «Tauri против Electron» (N=1 на MacBook, «difference was negligible» по старту —
  [Hopp, 2025-04](https://www.gethopp.app/blog/tauri-vs-electron); маркетинговые «Electron 12 s против Tauri 1.8 s»)
  для нашего вопроса бесполезны.

## 4. Electron (и почему MD-приложения на нём медленные)

- Версия: **electron 44.4.3** (npm, 2026-09-18).
- Механика старта: тот же Chromium (browser → GPU/utility → renderer), что у WebView2, **плюс** Node.js в main-процессе,
  **плюс** своя копия Chromium (~150–250 MB бинарников, **без hardlink с Edge**, то есть холодный page-in с диска),
  **плюс** загрузка и исполнение JS-бандла приложения.
- Почему именно MD-приложения стартуют секундами:
  - **Загрузка JS:** «the biggest bottleneck in app launch is obviously the process to load JavaScript … 1–2 secs in my app»
    (Inkdrop, [devas.life, 2020](https://www.devas.life/how-to-make-your-electron-app-launch-1000ms-faster-32ce1e0bb52c)).
    Там же V8-snapshot дал ≈ 1 с (4 → 3 с на macOS), Atom получил ~500 мс. Официальный гайд
    ([Performance](https://www.electronjs.org/docs/latest/tutorial/performance)): `require()` дорог, надо бандлить
    и откладывать загрузку модулей.
  - **Инициализация «рабочего пространства»:** плагины, индексация хранилища, восстановление вкладок.
    - Obsidian: 4–5 с и до 20+ с с плагинами ([форум](https://forum.obsidian.md/t/obsidian-time-to-start-is-wildly-inconsistent/110681)).
    - Typora 0.9.53: 7–8 с на double-click ([#1670](https://github.com/typora/typora-issues/issues/1670), 2018).
    - MarkText: 12 с холодный / 3 с тёплый ([#3862](https://github.com/marktext/marktext/issues/3862), 2024, M1).
    - Zettlr 2.2.5: 40 с на Windows 10 ([#3391](https://github.com/Zettlr/Zettlr/issues/3391), 2022).
  - **Редактор вместо ридера:** CodeMirror/ProseMirror-движки, подсветка, spellcheck — всё это грузится до первого экрана.
- Прогресс Electron в 2026: Node startup snapshot + build-time V8 code cache
  ([PR #51831](https://github.com/electron/electron/pull/51831), 2026-06, Electron 42;
  [PR #53136](https://github.com/electron/electron/pull/53136), 2026-08).
  Цифры авторов (Linux): utilityProcess.fork 86 → 40 мс, renderer с Node 91 → 65 мс, main-процесс «within
  run-to-run noise». Это **десятки миллисекунд**, пол Chromium остаётся.
- Для FastMD: Electron — худший из веб-вариантов по старту и размеру (~250 MB, у Hopp 244 MiB).
  Имеет смысл только как внешний baseline (есть у агента `electron`).

## 5. Neutralinojs и Wails

- **Neutralinojs 6.9.0** (2026-07-24). C++-бинарник ~2 MB + системный WebView (WebView2 на Windows). Архитектура:
  **локальный HTTP static server + WebSocket-сервер для IPC**, окно грузит `http://localhost:<port>/`
  ([docs](https://neutralino.js.org/docs/), [DeepWiki](https://deepwiki.com/neutralinojs/neutralinojs)).
  На старте поверх WebView2 добавляются подъём сервера, HTTP-запрос и WebSocket-рукопожатие.
  **Быстрее голого WebView2 быть не может.**
- **Wails**: v2.12.0 (стабильная, 2026-03-26) и v3.0.0-beta.23 (2026-09-16). Go + WebView2 (в v3 чистый Go-лоадер
  вместо `WebView2Loader.dll`); ассеты через встроенный asset server. Про «~15 MB, sub-0.5 s startup» пишет сама Wails,
  независимо не подтверждено ([digitalapplied, 2026](https://www.digitalapplied.com/blog/desktop-apps-web-stack-tauri-electron-deno-wails-2026)).
  Пол тот же, что у WebView2.
- Для FastMD оба — «WebView2 с лишним слоем». Смысла нет: если WebView2, то напрямую (C++) или через wry.

## 6. Sciter (Sciter.JS)

- Версия **6.0.5.0** (коммит SDK 2026-09-05, [gitlab sciter-js-sdk](https://gitlab.com/sciter-engine/sciter-js-sdk)).
  [замер] `bin/windows/x64/sciter.dll` = **19.3 MB**, `scapp.exe` = 19.3 MB (движок статически внутри), без подписи
  и без version resource. Маркетинговое «5+ MB» ([sciter.com](https://sciter.com/)) устарело.
- Однопроцессный движок: свой HTML/CSS (+ собственная раскладка `flow`/`flex`; `display:flex/grid` поддержаны
  «для популярных случаев», не полностью — [Sciter docs](https://docs.sciter.com/docs/CSS/flows-and-flexes/),
  [flexbox vs flow](https://sciter.com/flexbox-browser-versus-flow-flex-sciter/)), JS-движок на базе QuickJS,
  рендер Direct2D/Skia.
- Лицензия ([prices](https://sciter.com/prices/)): бинарник бесплатно «as it is published». Исходники: Indie $310
  (≤3 сотрудника, только Windows), Indie+ $620, Business $1 620–2 720. Требование упоминать «this code contains Sciter
  engine» в About снимается только на Enterprise++.
- [замер, предварительный]:
  - крошечная страница: окно с контентом ≈ **0.47 с** под нагрузкой (без `<meta charset>` Sciter читает файл в cp1251);
  - medium (готовый HTML со `style.css`): окно появилось через **1.7–2.5 с** (5 запусков), контент на скриншоте через
    ~0.7 с после появления ещё не нарисован, через 6 с нарисован;
  - типографика хорошая, цветные эмодзи есть, но **колонка `max-width:860px; margin:0 auto` уезжает вправо**
    (горизонтальный скролл). Совместимость с CSS не браузерная;
  - через `file:///` URL заголовок страницы так и не появился.

  Методика тут грубая: заголовок окна и скриншот. Честный замер должен сделать прототип `litehtml-sciter`.
- Fidelity: highlight.js (чистый JS) должен работать в QuickJS. KaTeX (CSS-вёрстка формул) и Mermaid (SVG + `getBBox` +
  много DOM API) — высокий риск.

## 7. litehtml

- **v0.10** (2026-06-13), последний коммит 2026-08-31, лицензия BSD-3 (+ gumbo, Apache-2.0)
  ([GitHub](https://github.com/litehtml/litehtml)).
- Суть: парсит HTML/CSS и **раскладывает** блоки. Шрифты, текст, картинки и отрисовку предоставляет приложение через
  `document_container`. Для FastMD это значит: DirectWrite/Direct2D для текста и рисования, свой кэш шрифтов,
  своя подсветка. По сути «нативный рендер с готовым CSS-layout».
- Поддержка: «most HTML tags and CSS properties» (CSS2 + часть CSS3: таблицы, float, позиционирование, селекторы;
  по [DeepWiki](https://deepwiki.com/litehtml/litehtml) есть и flex-раскладка, в README она не упомянута, надо проверять).
  Автор: «not fully compatible with HTML/CSS standards»,
  «not recommended as a full-featured HTML engine». Нет JS, значит KaTeX/Mermaid/hljs только нативно или пререндером в SVG.
- Кто использует: **Qt Creator** — help viewer по умолчанию на litehtml (`qlitehtml`,
  [doc.qt.io](https://doc.qt.io/qtcreator/creator-how-to-get-help.html),
  [attribution](https://doc.qt.io/qtcreator/qtassistant-attribution-litehtml.html)), демо litebrowser (Win/Linux/Haiku).
- Плюсы для FastMD: один процесс, мгновенный старт (ограничен только нашим кодом), CSS-тема «как GitHub» переносится
  почти один в один, сотни KB кода.
  Риски: пробелы CSS (grid, `:not()`, `max-content`, некоторые единицы), нет инкрементального layout для 100k+
  узлов, текст (selection, shaping, bidi) полностью на нас. Практику покажет прототип `litehtml-sciter`.

## 8. Ultralight

- **1.4** ([2025-04-21](https://ultralig.ht/blog/ultralight-1-4-out-now)), ядро **WebKit 615.1.18** (≈ Safari 16.4),
  один процесс, GPU- или CPU-рендер, JavaScriptCore.
- Лицензия ([pricing](https://ultralig.ht/pricing)): Free — «Limited performance, Limited feature-set, Indie devs only
  (< $100K)», только PC. Pro — **$3 000/год за приложение** (< $10M выручки). Исходники только в Enterprise.
- Скачивание SDK теперь **только после входа в аккаунт** ([download](https://ultralig.ht/download/)), поэтому
  практически не проверял. Заявлено «near-instant cold starts», «~1/10 RAM of Chromium»; чисел нет.
- Для FastMD: закрытый код, отстающий WebKit, «limited performance» в бесплатной версии. **Не рекомендую.**

## 9. RmlUi

- **RmlUi 6.3** (2026-08-22, [GitHub](https://github.com/mikke89/RmlUi)). «Based around XHTML1 and CSS2 while borrowing
  features from HTML5 and CSS3»: flexbox, transforms, анимации, media queries, border-radius, data binding, декораторы.
  **Не стремится к совместимости с CSS/HTML**, документы пишутся «под RmlUi». JS нет (есть Lua-плагин).
- Ориентирован на игровые UI (рендер через бэкенды OpenGL/Vulkan/DX). Для типографики длинных текстов (выделение,
  копирование, CJK/bidi, таблицы GFM) придётся многое дописывать. **Для FastMD не подходит.**

## 10. Blitz (DioxusLabs) [замер]

- Репозиторий [DioxusLabs/blitz](https://github.com/DioxusLabs/blitz), коммит `e7bf7bc` (2026-09-18), пакеты
  `0.3.0-beta.2`. README: «Blitz is currently in a **beta** state. It can already render many popular no-JS websites».
  Состав: **Stylo** (CSS-движок Firefox), html5ever, **Taffy** (flex/grid/block), **Parley** (текст), AccessKit,
  рендер через `anyrender` (Vello GPU, **vello_hybrid**, **vello_cpu**, Skia). Поддержка: «flexbox, grid, table, block,
  inline, absolute/fixed», сложные селекторы, media queries, CSS-переменные, float (feature).
- В репозитории есть готовый **Markdown-вьюер `rdme`** (`apps/readme`: comrak 0.52 + github-markdown.css).
  Собрал его с патчем меток по PROTOCOL (`research/lab/blitz/src/apps/readme/src/fmb.rs`: `main`, `parsed`,
  `dom_built`, `t_content` = первый `RedrawRequested` + DwmFlush). Сборка `cargo build --release -p rdme` —
  5 мин 36 с при `-j 8` под нагрузкой.

| Сборка | exe | t_content medium (медиана 5) | `main` | parse (comrak) | DOM build | Окно видно | WS |
|---|---|---|---|---|---|---|---|
| по умолчанию (`hybrid` = vello_hybrid на wgpu) | 28.4 MB | **1 577 мс** (1 491–2 306) | 91–180 | ~23–60 мс | ~7–13 мс | ~125 мс | 205 MB |
| `cpu-softbuffer` (vello_cpu) | 22.5 MB | **564 мс** (477–658) | 97–141 | ~30–45 мс | ~8–13 мс | ~145 мс | **59 MB** |
| `cpu-softbuffer`, **large.md** | | **5 780–6 300 мс** | 160–208 | ~430–450 мс | ~760–860 мс | | |

- Фазы движка по логам `log-phase-times` для medium: **Resolve 40 мс** (style 11, construct 8 + 10, layout 8.8 мс),
  растр vello_cpu 7 мс, softbuffer present 8 мс. Сам движок быстрый. Разрыв «DOM построен → первый кадр» (~400 мс в
  CPU-сборке, ~1.35 с в GPU-сборке) — это создание окна, инициализация растеризатора/GPU (wgpu adapter/device,
  шейдеры) и, вероятно, перечисление системных шрифтов (fontique). **Это место для оптимизации, а не предел движка.**
- Large: первый Resolve 3.5 с (style 1.9 с, layout 0.85 с, construct 0.4 с). Layout по видимой области не делается,
  документ раскладывается целиком.
- Fidelity (скриншот `research/lab/blitz/rdme-medium.png`): цветные эмодзи, CJK, кириллица, bold/italic, вложенные
  списки (`a.`/`b.` по GitHub CSS), таблицы, код. Тема следует системной (тёмная). Выглядит как GitHub.
- Минусы: **нет JavaScript** (KaTeX/Mermaid/hljs только нативно: подсветка через syntect/tree-sitter, формулы —
  через MathML или свой рендер), beta, API меняется, Windows-специфика (IME, выделение текста, печать) не проверена,
  холодный старт GPU-бэкенда.
- Вердикт: **главный кандидат среди «лёгких HTML-движков»**, если нужен настоящий CSS без Chromium. Имеет смысл
  отдельный прототип `rust-blitz` на общем harness и разбор 400 мс до первого кадра.

## 11. Servo [замер]

- Статус: `servo` 0.1.0 впервые опубликован на crates.io **2026-04-13** ([блог](https://servo.org/blog/2026/04/13/servo-0.1.0-release/)),
  затем 0.5.0 (2026-08-17). Есть план LTS-релизов ([Phoronix](https://www.phoronix.com/news/Servo-Embed-Crates-LTS)).
  Встраиваемый API развивается, «many WebView related APIs are still missing» ([PR #44984, C API](https://github.com/servo/servo/pull/44984)).
- Nightly **2026-09-17** (`servo-x86_64-windows-msvc.zip` 103 MB, распакован **276 MB**: много GStreamer DLL).
  Ссылка `download.servo.org` из Servo Book сейчас отдаёт 404 (GitHub Pages «Site not found»), брал из
  [servo/servo-nightly-builds](https://github.com/servo/servo-nightly-builds).
- [замер] `servoshell.exe file:///…/t-medium.html` (готовый HTML + style.css, сигнал через `document.title`):
  окно ≈ 350–640 мс, **контент ≈ 1.2 с** (1 210–1 496 мс, 3 запуска под нагрузкой; первый 3.1 с), WS ≈ 230 MB.
- Fidelity (скриншот `research/lab/servo/servo-medium.png`): **нумерация `<ol>` начинается с `0.`**, **нет bold/italic и
  semibold-заголовков** (похоже, веса variable-шрифта Segoe UI Variable не применяются), эмодзи монохромные.
  CJK и кириллица в порядке.
- Вердикт: для FastMD сейчас **нет** (старт, размер, баги вёрстки). Наблюдать за LTS и C API.

## 12. CEF

- Свой Chromium в процессе приложения. Бинарная дистрибуция: `libcef.dll` 100+ MB, минимальный набор ~117 MB и больше
  ([CEF forum](https://magpcss.org/ceforum/viewtopic.php?f=6&t=15213)). Версия идёт за Chromium
  (CEF 143.0.13 / Chromium 143 — декабрь 2025).
- Старт по построению не лучше WebView2: та же мультипроцессная модель, **но без разделения бинарников с Edge**, то есть
  холодный page-in своих ~200+ MB. Плюс: версия зафиксирована и не обновляется под ногами. В 2026 CEF стал опцией
  Tauri 3 (`tauri-runtime-cef`).
- Для FastMD: +200 MB к дистрибутиву ради фиксированной версии. **Не нужен.**

## 13. Fidelity: KaTeX, highlight.js, Mermaid в WebView2 [замер, набор D]

Все три библиотеки в WebView2 работают **без правок**: это обычный Chromium 153. Вопрос только в цене. Время внутри
страницы (`performance.now()`) не зависит от старта процессов, поэтому оно надёжнее, чем `t_content` под нагрузкой.
Скрипты подключались внешним файлом через WebResourceRequested (`https://fastmd.req/lib/...`), если не сказано иное.

| Библиотека (версия, размер) | Исполнение скрипта | Работа | Итог к первому кадру | Комментарий |
|---|---|---|---|---|
| **highlight.js 11.12.0** (`highlight.min.js` 129 KB, common-набор) | **3–7 мс** | **22–33 мс** на 24 блока medium (синхронный `highlightElement`) | ~+30–40 мс | `hljs.highlightAll()` при `readyState=loading` откладывает работу до `DOMContentLoaded`, то есть до первого кадра. Для первого экрана лучше подсветка нативно или только видимых блоков |
| **KaTeX 0.18.7** (`katex.min.js` 273 KB + CSS 25 KB + woff2) | 4–15 мс (первый раз 35 мс) | 35–62 мс на 80 формул | +50–80 мс, **шрифты приходят ещё через ~110 мс** (`document.fonts.ready`) | Если показывать до загрузки шрифтов — будет перекладка. Шрифты можно вшить (data: URI) или предзагрузить |
| **Mermaid 12.0.0** (`mermaid.min.js` **5.5 MB**) | 87–240 мс (тёплый), **первая загрузка одной серии — 4.96 с** | 290–480 мс на 2 диаграммы | **+300–700 мс** | Грузить **только** при наличии блока `mermaid` и **после** первого экрана. Первая загрузка 5.5 MB, скорее всего, упёрлась в сканирование файла Defender'ом при чтении хостом |
| Mermaid через `file://` | 200–280 мс | 280–420 мс | | исполнение `file://`-скрипта в 2 раза медленнее, чем через WebResourceRequested (видимо, нет потоковой компиляции / кэша) |
| Mermaid + `--enable-features=msWebView2CodeCache` | 87–120 мс с 3-го запуска (без флага 97–123) | | ≈ −10–15 % на исполнение | Флаг байткод-кэша для ресурсов из vhost/WebResourceRequested (из официального списка) даёт мало |
| markdown-it 15.0.2 в странице (120 KB) | 8 мс | 30 мс на medium, 520–590 мс на large | +50 мс (medium) | парсить лучше нативно |
| highlight.js **встроенный** в HTML (NavigateToString) | интервал 118–182 мс | | | интервал включает уступку парсера и первый кадр, не сравнимо; встроенные скрипты не кэшируются и компилируются в главном потоке |

Вывод для FastMD: полная GitHub-совместимость (подсветка + формулы + диаграммы) в WebView2 есть «из коробки».
highlight.js и KaTeX почти бесплатны (десятки мс), Mermaid дорог (сотни мс + 5.5 MB).
**Если основной рендер нативный**, есть два пути:
(а) подсветка нативно (tree-sitter/syntect/свой лексер), формулы — через MathML или микро-TeX, Mermaid — фоновым
WebView2 в SVG с кэшем по хэшу блока;
(б) «богатый режим» целиком на WebView2 по запросу.

---

## 14. Сводная таблица

**Контрольный повтор в одном окне времени** [замер, ~01:25, нагрузка средняя, 5 запусков, medium]:

| Вариант | t_content (медиана) | `main` | Комментарий |
|---|---|---|---|
| WebView2, warm UDF, NavigateToString | 731 | 50 | controller_ready 481, browser/GPU/renderer созданы на 123/294/443 мс |
| WebView2 + держатель (общий browser-процесс) | **396** | 22 | controller_ready 204 |
| Blitz rdme, CPU-растр (vello_cpu) | **375** | 92 | разбор 21 мс, DOM 7 мс, окно + растеризатор → кадр 248 мс |
| Blitz rdme, GPU (vello_hybrid) | 1 226 | 103 | ~1.1 с на инициализацию GPU-рендера |
| Servo nightly (servoshell) | 815 | — | окно 305 мс |

Отдельно: `main` у 22-мегабайтного `rdme.exe` стабильно ~90 мс, у 268-килобайтного `wv2probe.exe` — 22–50 мс.
Размер exe влияет на время до `main` (загрузка образа и сканирование Defender'ом).

| Движок | Версия (сентябрь 2026) | t_content medium, **[замер]** на этой машине | Память | Размер к дистрибутиву | CSS / вёрстка | JS: hljs / KaTeX / Mermaid | Лицензия | Главный риск |
|---|---|---|---|---|---|---|---|---|
| Win32 GDI (пол, baseline) | — | ~90 мс (тихо) | ~5 MB | ~0.1 MB | — | — | — | — |
| **WebView2** (C++, NavigateToString) | runtime 153, SDK 1.0.4191.47 | **~590** тихо / 700–1000 под нагрузкой | 165–185 MB priv (5 процессов) | 0 (evergreen) + ~0.2 MB loader | эталон (Chromium) | да / да / да | бесплатно | пол ~0.5 с; зависит от ОС и Edge; утечка crashpad при shared UDF |
| WebView2 + общий browser-процесс | 〃 | **~400–545** (без держателя в том же окне 730–930) | +резидент ~170 MB | 〃 | 〃 | 〃 | 〃 | резидентный процесс, утечка crashpad |
| WebView2 полностью резидентный | 〃 | см. прототип `cpp-webview2-resident` | постоянно ~170–260 MB | 〃 | 〃 | 〃 | 〃 | автозапуск, «честность» замера |
| Tauri 2 / wry | 2.11.5 / 0.57 | ≈ WebView2 + 10–50 мс (оценка) | ≈ WebView2 | 3–10 MB | 〃 | 〃 | MIT/Apache | как у WebView2 |
| Neutralino / Wails | 6.9.0 / 2.12, 3.0-β23 | ≥ WebView2 (архитектурно) | ≈ WebView2 | 2–15 MB | 〃 | 〃 | MIT | лишний слой (HTTP/WS) |
| Electron | 44.4.3 | не мерил (агент `electron`); по построению ≥ WebView2 | 200–450 MB | ~250 MB | 〃 | 〃 | MIT | размер, свой Chromium без hardlink |
| CEF | 14x | ≥ WebView2 (оценка) | ≈ Electron | 120–250 MB | 〃 | 〃 | BSD | размер |
| **Blitz** (rdme, CPU-растр) | 0.3.0-beta.2 | **375–560** (средняя/тяжёлая нагрузка; движок 40 мс) | **59 MB WS** | **22 MB exe** | Stylo: flex/grid/table/float, очень близко к GitHub | **нет JS** | MIT/Apache | beta; 400 мс init до кадра; large 5.8 с |
| Blitz (GPU, vello_hybrid) | 〃 | 1 200–1 500 | 205 MB WS | 28 MB | 〃 | нет | 〃 | инициализация GPU |
| Servo (servoshell) | nightly 2026-09-17 / crate 0.5.0 | 800–1 200 | ~230 MB WS | 276 MB | заметные баги (ol, weights) | да (SpiderMonkey) | MPL-2.0 | fidelity, размер |
| Sciter.JS | 6.0.5.0 | предварительно 1.7–2.5 с (medium), 0.47 с (tiny) | ~95–120 MB WS | 19.3 MB | своя модель (flow/flex), колонка «уехала» | QuickJS: hljs вероятно, KaTeX/Mermaid под вопросом | бинарник free, исходники от $310 | совместимость CSS, закрытый код |
| litehtml | 0.10 | не мерил (агент `litehtml-sciter`); ограничен нашим кодом | единицы MB | ~1 MB | CSS2 + часть CSS3 | нет | BSD-3 | пробелы CSS, весь текст на нас |
| Ultralight | 1.4 (WebKit 615) | не мерил (SDK за логином) | «1/10 Chromium» (заявлено) | ~30–40 MB (оценка) | WebKit 2023 года | JSC: да | free < $100K с «limited performance»; Pro $3k/год | лицензия, закрытость |
| RmlUi | 6.3 | не мерил | малые | ~1–2 MB | подмножество, «под RmlUi» | нет (Lua) | MIT | не для документов |

---

## 15. Выводы для FastMD

### 15.1 Главное

1. **Не строить FastMD на «новом процессе WebView2 на каждый double-click».** На этой машине, в благоприятном
   случае (Edge запущен, DLL в памяти, NVMe), пол ≈ 0.5–0.6 с до контента. Флаги почти не помогают, размер документа
   не важен, поэтому оптимизировать нечего: время съедают browser/GPU/renderer. Electron, CEF, Tauri, Neutralino и Wails
   живут на том же или более толстом Chromium и **быстрее этого пола не будут**.
2. **Основной путь для «пули» — нативный рендер** (Direct2D/DirectWrite + md4c; результаты у агентов `cpp-d2d`,
   `winui3`, `rust-gpu`). У веб-движков взять **CSS-модель и тему**:
   - вариант A: **litehtml** как layout-движок с контейнером на DirectWrite. GitHub-CSS почти как есть, один процесс,
     старт ограничен только нашим кодом;
   - вариант B: **Blitz** (Rust) — реальный CSS через Stylo, один exe 22 MB, 59 MB памяти, качество вёрстки хорошее.
     Сейчас 0.37–0.56 с, но сам движок работает 40 мс, остальное — старт процесса и инициализация окна/растеризатора, их можно разобрать.
     Beta, без JS.
3. **WebView2 оставить как «богатый режим» или резидент:**
   - **ленивый богатый режим:** первый экран рисует нативный рендер, а WebView2 поднимается в фоне только если в
     документе есть `mermaid`/`math` или пользователь попросил «как в GitHub». Mermaid/KaTeX можно отрендерить в
     фоновом WebView2 в SVG и закэшировать;
   - **резидентный вариант** (`-resident` по PROTOCOL §6): процесс держит прогретый контроллер и renderer, лаунчер
     передаёт путь. Это единственный способ получить вёрстку Chromium за ~100–200 мс. Цена: +170–260 MB в фоне,
     автозапуск, поведение после обновления runtime (`NewBrowserVersionAvailable` → перезапуск).

### 15.2 Если всё-таки WebView2 — чек-лист (всё подтверждено замерами выше)

1. `CreateCoreWebView2EnvironmentWithOptions` — первой строкой `WinMain`, окно создавать параллельно,
   контроллер — на видимом окне. `put_DefaultBackgroundColor` в цвет темы (без белой вспышки).
2. UDF постоянный, в `%LOCALAPPDATA%\FastMD\WebView2`, **одни и те же опции** во всех экземплярах (иначе общий
   browser-процесс не подключится и создание упадёт). InPrivate не включать.
3. Документ: Markdown парсить нативно, отдавать готовый HTML. NavigateToString при UTF-8 < 1.5 MB, иначе temp-файл
   (`file://`) или vhost. Для больших документов отдавать **первый экран сразу**, остальное дописывать
   (`PostWebMessage` порциями, `content-visibility: auto`).
4. Картинки и ассеты: `SetVirtualHostNameToFolderMapping` на хост **`*.example`** или `*.invalid`
   (никогда не `.local` и не имя без точки: +1.9 с на навигацию).
5. Флаги: ни один не даёт стабильного выигрыша (`--in-process-gpu` −90 мс в одной серии и −11 мс в другой).
   Лучше не использовать, Microsoft против флагов в продакшене. `--disable-gpu` и `--no-sandbox` не использовать точно.
6. Mermaid (5.5 MB) и KaTeX грузить **лениво**, только при наличии соответствующих блоков. highlight.js — либо лениво,
   либо подсветка нативно (tree-sitter/syntect) на этапе генерации HTML. Подробности в §13.
7. Общий browser-процесс между экземплярами (второй документ открывается быстрее) — да, но с учётом утечки
   `crashpad-handler` (§2.5): лучше, чтобы первый экземпляр сам открывал новые окна (single-instance + `WM_COPYDATA`).
8. Evergreen, не Fixed: 0 MB, hardlink с Edge, обновления безопасности. Тестировать на Beta/Dev-каналах.

### 15.3 Риски

| Риск | Где | Что делать |
|---|---|---|
| Пол старта WebView2 ~0.5 с и **нестабильность** (Defender, память, Edge не запущен, обновление runtime) | все Chromium-стеки | нативный первый экран; резидент |
| Утечка `crashpad-handler` при подключении к общему browser-процессу | WebView2 153 | single-instance хост; issue в WebView2Feedback |
| Задержки до `main` в секунды под нагрузкой | ОС, любой стек | метка `main` обязательна; подписанный exe и исключения Defender не лечат гарантированно |
| `.local` и имена без точки → системный резолв ~2 с | WebView2, Tauri (если свой хост) | зона `.example` / `.invalid` / `.localhost` |
| Большие документы: 3–6 с и 0.8–1 GB | Chromium, Blitz | порции, виртуализация, `content-visibility` |
| Нет JS → нет Mermaid/KaTeX | litehtml, Blitz, RmlUi, D2D | нативные аналоги или фоновый WebView2 → SVG-кэш |
| Лицензии и закрытость | Sciter (исходники платно), Ultralight ($3k/год, limited free) | предпочитать BSD/MIT-стеки |
| Незрелость | Blitz (beta), Servo (fidelity) | прототип + фиксация версии/коммита |

---

## 16. Замечания к harness и протоколу

1. **Выбросы до `main`.** Задержки 0.56–27 с между `CreateProcess` и первой строкой `main` (§2.12) под нагрузкой — не
   проблема стека. Предлагаю: в `bench.py report` показывать `main` отдельной колонкой и помечать запуски, где
   `main` > 200 мс (или больше медианы×3). В финальном прогоне на тихой машине такие запуски повторять, а не усреднять.
2. **Скрытые окна и rAF.** Если `-resident` вариант держит WebView2 в скрытом окне, rAF там не срабатывает. Резидент
   должен помечать `t_content` после показа окна (как в протоколе), иначе зависнет.
3. **Порядок и нагрузка.** Под нагрузкой разброс одной конфигурации — сотни мс. Для сравнений полезен режим
   `--order interleaved` (он есть) и вывод парных разностей по раундам.
4. **Хвост browser-процесса — проверено, harness в порядке.** [замер] После чистого выхода хоста browser-процесс
   WebView2 живёт ещё **150–290 мс**, один раз **2.5 с** (4 пробы). `bench.py` делает `TerminateJobObject` +
   `wait_job_empty`, то есть убивает browser посреди штатного завершения. Я проверил, не замедляет ли «грязный» выход
   следующий запуск (`kill_vs_graceful.py`, 7 пар, чередование): медиана **612 мс** после штатного выхода и
   **618 мс** после убийства, разницы нет.

---

## 17. Источники

WebView2 (Microsoft Learn): [Performance best practices](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance) (обновлено 2026-09-02) ·
[Process model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/process-model) ·
[Browser flags](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/webview-features-flags) (2026-08-31) ·
[Using local content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content) ·
[Distribution](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution) (2026-09-14) ·
[Evergreen vs fixed](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/evergreen-vs-fixed-version).
WebView2Feedback: [#1910](https://github.com/MicrosoftEdge/WebView2Feedback/issues/1910) · [#1540](https://github.com/MicrosoftEdge/WebView2Feedback/issues/1540) ·
[#2381](https://github.com/MicrosoftEdge/WebView2Feedback/issues/2381) · [#3398](https://github.com/MicrosoftEdge/WebView2Feedback/issues/3398) ·
[#5321](https://github.com/MicrosoftEdge/WebView2Feedback/issues/5321) · [#2323](https://github.com/MicrosoftEdge/WebView2Feedback/issues/2323).
Rick Strahl: [2 MB limit](https://weblog.west-wind.com/posts/2024/Jul/22/Work-around-the-WebView2-NavigateToString-2mb-Size-Limit) ·
[Visibility](https://weblog.west-wind.com/posts/2022/Jul/14/Fighting-WebView2-Visibility-on-Initialization) ·
[Environment caching](https://weblog.west-wind.com/posts/2023/Oct/31/Caching-your-WebView-Environment-to-manage-multiple-WebView2-Controls) ·
[Waiting for document](https://weblog.west-wind.com/posts/2025/May/06/WebView2-Waiting-for-Document-Loaded).
PowerToys: [MarkdownPreviewHandlerControl.cs](https://github.com/microsoft/PowerToys/blob/main/src/modules/previewpane/MarkdownPreviewHandler/MarkdownPreviewHandlerControl.cs) ·
[PR #47391](https://github.com/microsoft/PowerToys/pull/47391) · [#45267](https://github.com/microsoft/PowerToys/issues/45267) · [#46087](https://github.com/microsoft/powertoys/issues/46087).
Tauri/wry: [tauri #6416](https://github.com/tauri-apps/tauri/issues/6416) · [#13727](https://github.com/tauri-apps/tauri/issues/13727) ·
[#4197](https://github.com/tauri-apps/tauri/issues/4197) · [http scheme commit](https://github.com/tauri-apps/tauri/commit/4cb51a2d56cfcae0749062c79ede5236bd8c02c2) ·
[tauri-runtime-cef v3.0.0-alpha.1](https://github.com/tauri-apps/tauri/releases/tag/tauri-runtime-cef-v3.0.0-alpha.1) ·
[Hopp: Tauri vs Electron](https://www.gethopp.app/blog/tauri-vs-electron).
Electron: [Performance](https://www.electronjs.org/docs/latest/tutorial/performance) · [PR #51831](https://github.com/electron/electron/pull/51831) ·
[PR #53136](https://github.com/electron/electron/pull/53136) · [Inkdrop/devas.life](https://www.devas.life/how-to-make-your-electron-app-launch-1000ms-faster-32ce1e0bb52c) ·
[Typora #1670](https://github.com/typora/typora-issues/issues/1670) · [MarkText #3862](https://github.com/marktext/marktext/issues/3862) ·
[Zettlr #3391](https://github.com/Zettlr/Zettlr/issues/3391) · [Obsidian forum](https://forum.obsidian.md/t/obsidian-time-to-start-is-wildly-inconsistent/110681).
Neutralino/Wails: [neutralino.js.org](https://neutralino.js.org/docs/) · [DeepWiki](https://deepwiki.com/neutralinojs/neutralinojs) · [Wails v3](https://v3.wails.io/whats-new/).
Sciter: [sciter.com](https://sciter.com/) · [prices](https://sciter.com/prices/) · [flows & flexes](https://docs.sciter.com/docs/CSS/flows-and-flexes/) · [SDK](https://gitlab.com/sciter-engine/sciter-js-sdk).
litehtml: [GitHub](https://github.com/litehtml/litehtml) · [Qt Creator help](https://doc.qt.io/qtcreator/creator-how-to-get-help.html).
Ultralight: [1.4](https://ultralig.ht/blog/ultralight-1-4-out-now) · [pricing](https://ultralig.ht/pricing).
RmlUi: [GitHub](https://github.com/mikke89/RmlUi).
Blitz: [GitHub](https://github.com/DioxusLabs/blitz) · [Web Engines Hackfest 2024 slides](https://webengineshackfest.org/2024/slides/blitz_a_truly_modular_hackable_web_renderer_by_nico_burns.pdf).
Servo: [0.1.0 release](https://servo.org/blog/2026/04/13/servo-0.1.0-release/) · [Phoronix LTS](https://www.phoronix.com/news/Servo-Embed-Crates-LTS) ·
[nightly builds](https://github.com/servo/servo-nightly-builds) · [C API PR #44984](https://github.com/servo/servo/pull/44984) · [Servo Book](https://book.servo.org/trying/getting-servoshell.html).
CEF: [CEF forum: footprint](https://magpcss.org/ceforum/viewtopic.php?f=6&t=15213) · [Wikipedia](https://en.wikipedia.org/wiki/Chromium_Embedded_Framework).

---

## Приложение: как повторить

```powershell
cd C:/Main/Projects/FastMD/research/lab/web-engines
npm install                      # markdown-it, highlight.js, katex, mermaid (уже установлены)
node gen_pages.mjs               # pages/*.html
# сборка пробника (SDK распакован в wv2sdk/)
../../../bench/tools/msvc.cmd cl /nologo /utf-8 /O2 /MT /EHsc /std:c++20 /DUNICODE /D_UNICODE /I wv2sdk/build/native/include wv2probe.cpp /Fo:out/ /Fe:out/wv2probe.exe /link /SUBSYSTEM:WINDOWS wv2sdk/build/native/x64/WebView2LoaderStatic.lib
python drive.py warm --page pages/p-medium.html --runs 7                 # один сценарий
python drive.py shared --page pages/p-medium.html --udf shared --runs 7  # с «держателем»
python multi.py B-flags sets/B-flags.json --rounds 7                     # чередование
$env:PYTHONIOENCODING='utf-8'; python paired.py results/B-flags.json base
# Blitz
cd ../blitz/src; $env:CARGO_TARGET_DIR='../target-cpu'
cargo build --release -p rdme --no-default-features --features "cpu-softbuffer,comrak,floats,scrollbars,log-phase-times"
cd ../../web-engines; python proto_run.py --runs 5 -- ../blitz/target-cpu/release/rdme.exe ../../../../../bench/corpus/medium.md
# Servo / Sciter
python title_watch.py --runs 3 -- ../servo/servo/servo/servoshell.exe file:///C:/Main/Projects/FastMD/research/lab/web-engines/pages/t-medium.html
```

Сырые результаты: `research/lab/web-engines/results/*.json|*.log`:
- matrix1/matrix2 — ранние последовательные серии (в matrix1 часть сценариев испорчена зависшим держателем,
  описано в §2.5; в matrix2 сценарии с `--flags`/`--args=` без пробелов не запустились из-за argparse и
  перемерены в наборах B/C);
- B-flags, C-load, D-libs, F-hosts, G-gpu — чередующиеся наборы;
- E-*, Q-* — серии с держателем и контрольный повтор.

Дополнительные скрипты: `crashpad_check.py` (утечка crashpad-handler), `kill_vs_graceful.py` (влияние убийства
browser-процесса на следующий запуск). Скриншоты: `research/lab/servo/servo-medium.png`,
`research/lab/blitz/rdme-medium.png`, `research/lab/sciter/sciter-medium-settle6.png`, `sciter-tiny.png`.
Диск: `research/lab/blitz/target*` занимают ~3 GB (промежуточные файлы cargo); exe лежат в `target*/release/rdme.exe`,
папки можно удалить, если пересборка не нужна.
