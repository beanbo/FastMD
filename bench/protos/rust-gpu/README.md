# rust-gpu — FastMD на Rust GPU-UI (egui / iced)

Прототип просмотрщика Markdown на Rust-фреймворках «immediate-mode / GPU UI»:

* **egui 0.36 + egui_commonmark 0.25** — свой хост на winit 0.30 + egui-wgpu (wgpu 30, DX12), без eframe;
  stock-eframe (wgpu / glow) собран только для сравнения;
* **iced 0.14** со встроенным виджетом `markdown` (+ `highlighter` на syntect) и своим `Viewer`
  (стили по спецификации); рендереры tiny-skia (CPU) и wgpu (GPU).

Все цифры — общий харнесс `bench/harness/bench.py`, машина из брифа (Ryzen 9 7950X, RTX 5070 + AMD iGPU,
Defender on, MSI Afterburner/RTSS запущен). Машина делилась с другими агентами; итоговые таблицы сняты в
тихий период (загрузка CPU 15–30 %), но это всё равно грубое сравнение — финальный последовательный замер
делает оркестратор.

## TL;DR

| вариант | чем нарисован первый кадр | medium: медиана / мин, мс | вердикт |
|---|---|---|---|
| **rust-gpu-egui-softfirst** | egui-меши растеризуются **на CPU** (свой растеризатор) + GDI-blit в момент показа окна; GPU (wgpu/DX12, RTX 5070) подхватывает следующие кадры | **89 / 86** | лучший полноценный; baseline-win32 + ~15 мс |
| rust-gpu-egui-softfirst-noime | то же + `ImmDisableIME` (нет IME в UI-потоке) | 82 / 81 | ещё −5…10 мс, но без IME |
| rust-gpu-iced-tinyskia | iced, **CPU**-рендерер tiny-skia + softbuffer (wgpu не вкомпилирован) | 129 / 124 | неплохо, но нет виртуализации: large.md догружается ~10 с |
| rust-gpu-egui-wgpu | egui, **GPU** с первого кадра: wgpu 30 / D3D12 на RTX 5070 | 365 / 347 | честный «чистый GPU»: +270 мс на D3D12 |

baseline-win32 в том же прогоне: 73 / 68 мс. iced с GPU-рендерером (`out/iced-wgpu`, в proto.json не
включён): ~590 мс.

**Ответ на вопрос «что на самом деле рисует быстрые варианты»:** прежние dev-варианты `rust-gpu-egui`
(110 мс) и `rust-gpu-iced` (150 мс) действительно **не использовали GPU для первого кадра**: egui — свой CPU-
растеризатор + GDI (GPU инициализировался только после `t_content`), iced — чистый CPU-рендерер tiny-skia.
Теперь это отражено в именах: `rust-gpu-egui-softfirst`, `rust-gpu-iced-tinyskia`. Оба рисуют полный
стилизованный документ (скриншоты ниже просмотрены). Честный GPU-вариант egui добавлен как
`rust-gpu-egui-wgpu`; `rust-gpu-egui-glow` (stock eframe + OpenGL, ~600 мс) и `rust-gpu-iced-wgpu`
(~590 мс) убраны из proto.json — их цифры в разделе «Эксперименты».

**Главный вывод.** На Windows «GPU-UI» сам по себе для мгновенного открытия не годится: создание
D3D12-устройства на RTX 5070 стоит ~270 мс (драйвер NVIDIA грузит ~100 МБ DLL), на AMD iGPU устройство
готово за ~135 мс, но первый present стоит ещё ~200 мс, WARP исполняет первый кадр ~135 мс. Быстрым egui
становится, когда **первый кадр рисуется без GPU**: меши egui первого экрана растеризуются на CPU за ~9 мс,
окно получает кадр в момент показа, а GPU поднимается уже после того, как документ на экране (в обычном
режиме wgpu подхватывает рендер через ~0,3 с). После всех патчей путь egui **от `main()` до `t_content`
(~40 мс) такой же, как у голого Win32+GDI**; весь остаток отставания от baseline — запуск 7,5-МБ exe.

## Стек и версии

| компонент | версия | заметки |
|---|---|---|
| Rust | 1.98.1 stable, `x86_64-pc-windows-msvc` | cargo 1.98.1; линкер MSVC 14.44 (rustc находит сам, PATH не нужен) |
| egui / epaint / egui-winit / egui-wgpu / egui_extras | 0.36.2 | `default-features = false`; egui_extras: `file`, `image` (PNG) |
| egui_commonmark (+ _backend) | 0.25.0, **вендорен и пропатчен** (`egui/vendor/`) | типографика по спецификации, см. «Патчи» |
| pulldown-cmark | 0.13.4 (egui), 0.12.2 (iced) | GFM: таблицы, task lists, strikethrough, footnotes |
| wgpu / wgpu-core / wgpu-hal | 30.0.1 (egui), 27.0.x (iced) | только DX12 + WGSL; **wgpu-hal пропатчен** (`vendor/wgpu-hal-30`, `vendor/wgpu-hal-27`) |
| winit | 0.30.13, **пропатчен** (`vendor/winit-0.30`) | общий для egui и iced |
| eframe (+glow 0.17, glutin 0.32.3) | 0.36.2 | только `out/egui-eframe` для экспериментов |
| iced / iced_widget / iced_graphics / iced_wgpu / iced_highlighter | 0.14.0 / 0.14.2 / 0.14.0 | фичи `markdown, highlighter, image, crisp, web-colors, thread-pool, advanced` |
| iced_tiny_skia / tiny-skia / softbuffer | 0.14.1 / 0.11.4 / 0.4.8 | CPU-рендерер iced |
| cosmic-text / swash / fontdb | 0.15.0 / 0.2.10 / 0.23.0 | текст в iced (цветные emoji работают) |
| syntect / two-face | 5.3.0 / 0.4.5 | подсветка кода в iced |
| windows-sys | 0.61 | DwmFlush, GDI blit, IME, хук окна, working set |

Профиль release: `opt-level=3, lto=true, codegen-units=1, panic=abort, strip=true`; `.cargo/config.toml`:
статический CRT (`+crt-static`) и `/DELAYLOAD` для dxgi/setupapi/ole32/oleaut32/shell32/shlwapi.

## Сборка

```
pwsh -File bench/protos/rust-gpu/build.ps1          # инкрементально; в свежем чекауте = сборка с нуля
pwsh -File bench/protos/rust-gpu/build.ps1 -Clean   # удалить target-*/out и собрать заново
```

`build.ps1` собирает 4 бинарника (`cargo build --release --locked`; крейты из crates.io по Cargo.lock,
пропатченные — из `vendor/` и `egui/vendor/`):

| выход | cargo | варианты |
|---|---|---|
| `out/egui/fastmd-egui.exe` (7,5 МБ) | `egui/`, фичи по умолчанию, `target/` | rust-gpu-egui-softfirst, -softfirst-noime, -egui-wgpu |
| `out/egui-eframe/fastmd-egui-eframe.exe` (9,5 МБ) | `egui/ --features eframe-shell`, `target-eframe/` | только эксперименты |
| `out/iced/fastmd-iced.exe` (7,8 МБ) | `iced/ --no-default-features --features cpu`, `target-cpu/` | rust-gpu-iced-tinyskia |
| `out/iced-wgpu/fastmd-iced-wgpu.exe` (11,7 МБ) | `iced/ --no-default-features --features gpu`, `target-gpu/` | только эксперименты |

Время: пересборка одного бинарника с LTO после правки кода — 1 мин 40 с … 2 мин 40 с; после правки winit
весь `build.ps1` — ~7 мин. Полная сборка с нуля (≈360 крейтов egui + ≈400 iced) в этой сессии целиком не
замерялась, ориентировочно 10–15 мин. `build.ps1` в итоговом виде прогнан несколько раз, выход 0.

## Варианты (proto.json)

| id | exe + args | кадр 0 → дальше |
|---|---|---|
| `rust-gpu-egui-softfirst` | `fastmd-egui.exe --first soft --adapter high --paint-on-show 1` | CPU-растеризация (soft.rs) + GDI в момент показа окна → wgpu DX12 (RTX 5070) после t_content |
| `rust-gpu-egui-softfirst-noime` | то же + `--ime off` | то же; `ImmDisableIME(0)` до создания первого окна |
| `rust-gpu-egui-wgpu` | `fastmd-egui.exe --first gpu --adapter high` | wgpu DX12 на RTX 5070 с первого кадра |
| `rust-gpu-iced-tinyskia` | `fastmd-iced.exe --renderer tiny-skia` | iced tiny-skia (CPU) + softbuffer |

Ключи `fastmd-egui.exe` (жирным — по умолчанию в коде):

| ключ | значения | смысл |
|---|---|---|
| `--first` | **soft** \| gpu \| cpu | кадр 0: CPU + GDI, затем GPU / сразу GPU / только CPU (GPU никогда) |
| `--paint-on-show` | **0** \| 1 | blit подготовленного кадра из хука в момент, когда окно стало видимым (см. ниже) |
| `--ime` | **on** \| off | `off` = `ImmDisableIME(0)` для UI-потока до первого окна |
| `--adapter` | **all** \| high \| low \| warp \| primary | пропатченный wgpu-hal (`WGPU_DX12_ADAPTER`) отдаёт ровно один DXGI-адаптер |
| `--power` | **high** \| low \| none | `PowerPreference` для `request_adapter` |
| `--backend` | **dx12** \| vulkan \| gl \| primary | vulkan/gl — только со сборкой `--features wgpu-all-backends` |
| `--present` | **vsync** \| novsync | `AutoVsync` / `AutoNoVsync` |
| `--gpu-delay` | **1** \| 0 | при `--first soft`: GPU стартует после t_content (1) или параллельно (0) |
| `--fonts` | **mmap** \| read \| builtin | системные шрифты через mmap / чтением / только встроенные (eframe-сборка) |
| `--order` | **gpu-first** \| el-first | порядок: запуск потоков → event loop или наоборот |
| `--shell`, `--renderer` | **direct**/eframe, **wgpu**/glow | stock eframe — только в `fastmd-egui-eframe.exe` |

`fastmd-iced*.exe`: `--renderer tiny-skia|wgpu`, `--adapter all|high|low|warp`, `--present`,
`--confirm-frames N` (2), `--prefix-kb N` (8), `--plain-first 0|1` (первый кадр без подсветки),
`--wait-parse 0|1` (первый `view` ждёт парсер вместо пустого кадра). Отладка: `FASTMD_LOG=<файл>` (egui:
кадры CPU/GPU, передача на GPU, трасса хука), `FASTMD_SCROLL_TO=<pt>` (egui: прокрутка для скриншотов).

## Как устроен быстрый путь (rust-gpu-egui-softfirst)

```
main thread  : main → read (≤1 мс) → split первых ≤64 КБ на блоки (pulldown-cmark) → mmap шрифтов
               → [старт layout-потока] → EventLoop::new → create_window(visible):
                   ├ CreateWindowEx (скрыто) → размер 1000×800 → ShowWindow
                   ├ ★ хук WH_CALLWNDPROC: первое сообщение видимому окну → SetDIBitsToDevice + GdiFlush
                   │   → DwmFlush → t_content          (в bench-режиме)
                   └ активация / IME / фокус (~20 мс) → create_window возвращается
               → (обычный режим) старт GPU-потока, фоновый прогрев раскладок клавиатуры
layout thread: egui::Context + set_fonts → egui-pass только по блокам 1-го экрана → tessellate
               → CPU-растеризация 1000×800 в DIB (≈9 мс)            — успевает до показа окна
bg thread    : (файлы > 96 КБ) парсинг остатка документа на блоки
GPU thread   : (после t_content) wgpu Instance → Adapter (RTX 5070) → Device → egui_wgpu::Renderer;
               главный поток создаёт surface, передаёт текстуры egui (атлас, картинки), дальше рисует GPU
```

* **Растеризатор** `egui/src/soft.rs` (~200 строк) повторяет шейдер egui-wgpu (`fs_main_gamma_framebuffer`):
  premultiplied-цвета в гамма-пространстве, bilinear-выборка атласа, blend (ONE, ONE_MINUS_SRC_ALPHA).
  Визуально кадр не отличается от GPU-кадра (`shots/egui-cpu-only-medium.png` vs
  `shots/rust-gpu-egui-wgpu-medium.png`).
* **Paint-on-show** (`shell_direct.rs`, модуль `early`): winit внутри `create_window` показывает окно и ещё
  ~20 мс занимается активацией (WM_ACTIVATEAPP/NCACTIVATE/ACTIVATE, WM_IME_SETCONTEXT → IMM/TSF, фокус).
  Хук потока `WH_CALLWNDPROC` на время `create_window` ловит первое сообщение, пришедшее окну, когда оно
  уже видимо и имеет итоговый клиентский размер (по трассе — `WM_WINDOWPOSCHANGING` внутри ShowWindow), и
  делает blit готового кадра до обработки этого сообщения — то же, что показал бы `WM_PAINT` обычного
  Win32-приложения. Внешний наблюдатель (`experiments/visprobe.py`) видит окно видимым за ~1 мс до blit;
  харнесс-`observed` ≈ `t_window`. Скриншот без выдержки (`shots/egui-paint-on-show-settle0-medium.png`,
  снят через ~0 мс после `t_content` = 83 мс) — полный документ. Если размер не совпал — обычный путь.
* **`t_content`** по PROTOCOL §3: кадр с документом отрисован (GDI: `SetDIBitsToDevice` + `GdiFlush`; GPU:
  `queue.present` + `device.poll(wait)`), затем `DwmFlush()`.
* **Виртуализация**: на кадре раскладываются только блоки верхнего уровня, пересекающие viewport; остальные
  «доизмеряются» в фоне по ≤4 мс за кадр (честный скроллбар). Большие файлы: синхронно парсятся первые
  ~64 КБ (срез перед ATX-заголовком вне fenced-кода), остальное — на фоне. Это разрешённая PROTOCOL техника
  «first viewport first».
* **Передача на GPU** в обычном режиме (`FASTMD_LOG`, medium): 15 CPU-кадров по 9–14 мс (идёт фоновое
  измерение блоков), через ~320 мс после первого кадра — `gpu adopted`, дальше GPU-кадры 1,5–5 мс.
  Зависаний UI > 50 мс нет ни на medium, ни на large (`experiments/responsive.py`).

## Поддержка спецификации (PROTOCOL §5)

| элемент | egui (все egui-варианты) | iced |
|---|---|---|
| PMv2 DPI, 1000×800 DIP, заголовок окна | ✅ | ✅ |
| тело Segoe UI Variable 15 px, line-height 1.6, `#1f2328` | ✅ `SegUIVar.ttf`, 24 px | ⚠️ `Segoe UI` (Variable через fontdb/cosmic-text не выбирается), 1.6 ✅ |
| h1–h6 30/24/20/17/15/14, semibold, бордюр h1/h2, h6 `#59636e` | ✅ (semibold = ось `wght` 600) | ✅ (Segoe UI Semibold) |
| **жирный** / *курсив* / ~~зачёркнутый~~ | ✅ / ⚠️ синтетический наклон / ✅ | ✅ / ✅ настоящий italic / ✅ |
| `inline code` фон `#eff1f3`, радиус 4 | ✅ (padding — тонкие пробелы) | ✅ |
| ссылки `#0969da`, кликабельные | ✅ (в т. ч. внутри цитат — патч) | ✅ |
| списки: маркированные, нумерованные, вложенные, task list | ✅ (task-пункты без буллета) | ✅ (чекбоксы в стиле GitHub; вложенные буллеты все «•») |
| цитата: полоса 4 px `#d1d9e0`, текст `#59636e` | ✅ | ✅ |
| горизонтальная линия | ✅ | ✅ |
| блок кода: Cascadia Mono 13,5, фон `#f6f8fa`, радиус 6, padding 16 | ✅, **с переносом строк** | ✅, без переноса, горизонтальный скролл |
| подсветка синтаксиса | ⚠️ простая встроенная egui_extras (ключевые слова/строки/комментарии) | ✅ syntect (base16-тема, приглушённые цвета) |
| GFM-таблицы: сетка 1 px, жирная шапка, зебра, padding 6/13, выравнивание | ✅ свой виджет `table.rs` (ширина по содержимому) | ✅ свой `Viewer::table` (на всю ширину колонки, доли по длине текста) |
| изображения (локальные PNG) | ✅ egui_extras (`file://`) | ✅ |
| кириллица / CJK / emoji | ✅ / ✅ (fallback `msyh.ttc`) / ⚠️ **монохромные** (egui не умеет COLR) | ✅ / ✅ / ✅ цветные |
| прокрутка | ✅ колесо/тачпад, плавающий скроллбар | ✅ |
| качество текста | grayscale AA, растеризатор egui (без ClearType) | grayscale AA (swash) |
| большой файл (3,7 МБ) | ✅ виртуализация, без фризов | ⚠️ нет виртуализации: остаток парсится ~10 с (syntect), затем фриз UI ~2,7 с |

Скриншоты (все просмотрены): `shots/<variant>-medium.png`, `shots/<variant>-small.png`,
`shots/rust-gpu-egui-softfirst-large.png`, `shots/rust-gpu-iced-tinyskia-large.png`, прокрутка egui —
`shots/egui-softfirst-medium-y780.png` (код, цитата, таблица, картинка), `shots/egui-softfirst-medium-y2400.png`,
`shots/rust-gpu-iced-wgpu-*.png` (старый iced-GPU, до правок таблицы).

## Быстрые цифры

`python bench/harness/bench.py run <4 варианта>,baseline-win32 --doc small,medium,large --runs 5
--out bench/protos/rust-gpu/results/quick-run-final.json` — 5 замеров после прогрева, загрузка CPU 16–27 %.
Медиана / мин / p90, мс от `CreateProcessW` до `t_content`:

| вариант | small | medium | large | WS, МБ | peak commit, МБ | CPU, мс | dist, МБ |
|---|---|---|---|---|---|---|---|
| baseline-win32 (пол) | 79 / 71 / 99 | 73 / 68 / 84 | 74 / 73 / 85 | 14 | 3 | 31 | 0,15 |
| **rust-gpu-egui-softfirst** | **86 / 85 / 98** | **89 / 86 / 93** | **91 / 90 / 96** | 23 (large 28) | 10 (large 41) | 63 | 7,5 |
| rust-gpu-egui-softfirst-noime | 88 / 81 / 96 | 82 / 81 / 87 | 93 / 87 / 94 | 23 | 10 (large 41) | 47–78 | 7,5 |
| rust-gpu-iced-tinyskia | 136 / 124 / 151 | 129 / 124 / 143 | 194 / 189 / 199 | 78 (large 111) | 63 (large 103) | 234 | 7,8 |
| rust-gpu-egui-wgpu | 352 / 352 / 374 | 365 / 347 / 371 | 365 / 353 / 369 | 89 | 382 | 313 | 7,5 |

Разброс между сессиями большой: те же варианты при загрузке 40–100 % от чужих сборок давали 130–180 мс
(egui-softfirst до последних патчей) и 180–240 мс (iced) — `results/quick-run-1.json`, `quick-run-2.json`;
единичные выбросы до 2–3 с при пиках нагрузки. Сравнивать только внутри одного прогона.

### Разбивка (медианы отметок, medium, quick-run-final; мс от CreateProcessW)

**baseline-win32:** `CreateProcessW вернул 21.6` → `main 32.0` → окно видно `47.8` → `t_window 66.1` →
**`t_content 72.9`**.

**rust-gpu-egui-softfirst:** `CreateProcessW вернул 37.8` → `main 48.9` → `read 49.1` → `parsed 49.7` →
`fonts 49.9` → ‖ layout-поток: `layout_ui 59.2` (контекст egui + шрифты + раскладка 1-го экрана, ~9 мс) →
`layout_tess 59.3` → `layout_raster 67.7` (CPU-растеризация ~8,5 мс) ‖ главный поток: `event_loop 66.0`
(~17 мс: первый `CreateWindowEx` в процессе, IMM, хуки RTSS) → окно видно `78.0` → blit
`frame0_presented 79.0` → `DwmFlush` (~10 мс) → **`t_content 88.8`**.
От `main` до `t_content`: egui 39,9 мс, baseline 40,9 мс — **внутрипроцессный путь не медленнее голого
Win32**; вся разница (~16 мс) — до `main` (размер exe).

**rust-gpu-egui-softfirst-noime:** `event_loop 61.1` (13,6 мс после main вместо 17) → окно видно `74.5` →
**`t_content 82.3`**.

**rust-gpu-egui-wgpu:** до `window_created 105` так же; GPU-поток: `gpu_thread 51` → `gpu_instance 80`
(загрузка dxgi/d3d12) → `gpu_adapter 271` (~190 мс: D3D12-адаптер NVIDIA, инициализация драйвера) →
`gpu_device 337` (~66 мс) → `gpu_renderer 349` (пайплайны egui) → `surface 354` → `frame0_presented 360` →
`poll(wait)` → **`t_content 365`**.

**rust-gpu-iced-tinyskia:** `main 50.1` → `boot 68.7` (рантайм iced + event loop) → `font_system 82`
(cosmic-text сканирует все системные шрифты, отдельный поток) → пустой первый кадр `view_empty 90.9` →
`parsed 103` (первые 8 КБ + syntect ~50 мс, отдельный поток) → `view_doc 112` → `frame1_msg 123.5` →
**`t_content 129.4`** (подтверждение на 2-м сообщении `window::frames()`).

### Куда уходит время (egui-softfirst, ~89 мс при поле ~73)

1. **Запуск процесса: ~38 мс до возврата `CreateProcessW` (baseline 22) + 11 мс до `main`.** Разница с
   baseline ~16 мс — это 7,5-МБ exe против 150 КБ (Defender сканирует образ). По `experiments/compare_size.py`:
   0,15 МБ → 24 мс, 6 МБ → 35, 7,5 МБ → 38, 7,8 МБ → 41, 11,7 МБ → 48 мс ≈ **+2 мс на каждый МБ exe**.
   Но `opt-level="s"` (6,1 МБ) выигрыша не дал: −0,6 мс на запуске, зато раскладка/растеризация +8 мс и
   попадают на критический путь.
2. **Окно: ~17 мс `EventLoop::new` + ~12 мс до видимости окна** — критический путь; раскладка и
   растеризация (~18 мс CPU) идут параллельно и заканчиваются раньше.
3. **`DwmFlush` ~10 мс** — ожидание композиции (часть протокола, у всех одинаково).

## Эксперименты и A/B (не в proto.json)

`experiments/compare_all.py` — чередующиеся прогоны через `bench.run_once`, medium, 4 замера после прогрева,
загрузка 16–26 %, итоговые бинарники:

| конфигурация | t_content медиана / мин, мс | комментарий |
|---|---|---|
| egui `--first soft --paint-on-show 1` (= softfirst) | 93 / 87 | |
| egui `--first cpu` (GPU нет вообще) | 97 / 88 | в bench-режиме то же, что soft |
| egui `--first gpu --adapter high` (RTX 5070) | 367 / 348 | adapter 269, device 335 |
| egui `--first gpu --adapter low` (AMD iGPU) | 412 / 406 | device на 187 мс, но первый present ~200 мс |
| egui `--first gpu --adapter warp` (программный D3D12) | 248 / 244 | device 100 мс, исполнение первого кадра ~125 мс |
| egui `--first gpu --adapter all` (stock wgpu: все адаптеры) | 448 / 436 | wgpu создаёт D3D12-устройство на **каждом** GPU |
| stock eframe + wgpu (DX12) | 520 / 504 | eframe прячет окно до первого кадра, сам выбирает адаптер |
| eframe + wgpu, устройство заранее в отдельном потоке | 460 / 414 | |
| stock eframe + **glow** (OpenGL/WGL) | 604 / 564 | бывший `rust-gpu-egui-glow`; t_content на старте 2-го кадра |
| iced tiny-skia | 155 / 136 | |
| iced wgpu `--adapter high` (бывший `rust-gpu-iced-wgpu`) | 592 / 581 | компоситор создаётся синхронно до первого `view` (`view_doc` 489) |
| iced wgpu `--adapter low` (AMD iGPU) | 644 / 611 | |
| iced wgpu `--adapter all` | 654 / 636 | |

A/B патчей старта (`experiments/compare_ab.py`, medium):

| сравнение | результат (медиана / мин, мс) | условия |
|---|---|---|
| egui softfirst: до патчей winit → + `GetSystemMenu`/first-focus-патчи | 170 / 118 → 139 / 127 | 8 замеров, загрузка ~66 %, шумно |
| + `--ime off` | 139 → 115 / 100 | там же |
| + size-first-патч winit, без / с paint-on-show | 112 / 95 → **101 / 87** | 12 замеров, загрузка ~14 % |
| `--ime off`, без / с paint-on-show | 96 / 88 → **90 / 80** | там же (baseline 73 / 64) |
| iced: до патчей winit → после | 190 / 162 → 169 / 156 | 8 замеров, ~66 % |
| iced `--wait-parse 1` (без пустого кадра) | 200 / 178 — **хуже**: первый `view` идёт до создания окна, блокировка задерживает окно | там же |
| iced `--plain-first 1` (первый кадр без подсветки) | 177 / 147 | парсинг 8 КБ падает с ~70 до ~10 мс, но критический путь — окно + первый рендер |
| iced `--prefix-kb 4` | 169 / 136 | в пределах шума |
| egui `opt-level="s"` (6,1 МБ) vs `3` (7,5 МБ) | 94 / 82 vs 92 / 81 | `experiments/compare_size.py final` |

Выводы по ручкам:
* **Выбор адаптера:** DXGI-патч «ровно один адаптер» экономит ~80–120 мс — stock wgpu открывает D3D12-устройство
  на всех GPU (NVIDIA ~240 мс + AMD ~90 мс + WARP), чтобы их перечислить. `HighPerformance` → RTX 5070;
  `LowPower` → AMD iGPU (устройство быстрее, итог хуже из-за первого present).
* **Отложенный GPU (`--gpu-delay 1`)**: создание D3D12-устройства NVIDIA под loader lock грузит ~100 МБ DLL и
  замедляет параллельное создание окна на 5–10 мс, поэтому в softfirst GPU стартует после `t_content`.
* **Шрифты:** mmap системных TTF (SegUIVar, CascadiaMono, seguiemj, msyh.ttc ~20 МБ, seguisym) — < 1 мс;
  реальная цена — растеризация встреченных глифов в атлас на первом проходе (внутри ~9 мс `layout_ui`).
* **`ImmDisableIME`** — общий для любых Win32-стеков приём: минус инициализация IMM при первом
  `CreateWindowEx` и активация IMM/TSF в `ShowWindow` (5–20 мс в зависимости от прогона). Цена — нет IME в
  окнах UI-потока (поиск по-китайски/японски не набрать), поэтому это отдельный вариант, а не основной.
* **Статический CRT + delay-load** DLL: минус загрузка vcruntime/ucrt и dxgi/ole32/shell32 до первого кадра.

## Патчи зависимостей (все помечены `FastMD patch` в коде)

* `vendor/wgpu-hal-30`, `vendor/wgpu-hal-27` (`src/auxil/dxgi/factory.rs`): `WGPU_DX12_ADAPTER=high|low|warp|primary`
  → `EnumAdapterByGpuPreference` отдаёт один адаптер вместо перечисления всех.
* `vendor/winit-0.30`:
  1. `window.rs::on_create` — **размер окна применяется до показа** (upstream показывает окно размером
     CW_USEDEFAULT, 2564×984 на этой машине, и только потом уменьшает: вспышка огромного окна + лишний resize);
  2. `window_state.rs::apply_diff` — не трогать системное меню (`GetSystemMenu` делает копию меню, ~5 мс),
     если кнопка «закрыть» не меняется;
  3. `keyboard.rs` — на первый `WM_SETFOCUS` нового окна не опрашивать 256 × `GetAsyncKeyState`
     (не синтезировать нажатия клавиш, зажатых ещё в Проводнике);
  4. `synthesize_kbd_state` / `get_agnostic_mods` — не строить кеш раскладки клавиатуры (тысячи
     `ToUnicodeEx`, ~100–150 мс на раскладку) в `ShowWindow`; `prewarm_keyboard_layouts()` строит его в фоне
     после первого кадра.
* `egui/vendor/egui_commonmark*`: типографика GitHub (размеры h1–h6, `wght` 600, line-height 24, inline code
  85 % с тонкими пробелами), код-блок padding 16 / радиус 6 без рамки, квадратные чекбоксы, task-пункты без
  буллета, цитата 4 px + 15 px, без лишних пустых строк вокруг блоков, цвет ссылок внутри цитат.
* iced не патчится; `markdown::Row::cells` приватно, поэтому своя таблица читает его через pointer cast
  (с compile-time проверкой размера) — хак прототипа, в продукте — патч iced.

## Идеи дальнейшего ускорения

1. **Размер exe** (~2 мс/МБ на запуске): вынести wgpu/naga (не нужны до первого кадра) в отдельную DLL с
   delay-load или во второй процесс; выбросить `image`/`egui_extras` (PNG декодировать лениво, WIC); без
   этого остаётся ~15 мс до baseline.
2. **Свой Win32-хост вместо winit** (как baseline): `EventLoop::new` (~17 мс) + `create_window` делают много
   лишнего; paint-on-show уже снимает большую часть, но создание event loop остаётся.
3. **Меньше работы в layout-потоке** — сейчас он не на критическом пути (заканчивает ~10 мс раньше показа),
   но на слабых машинах будет: запечённый атлас нельзя (запрещённый кеш), можно не подключать `msyh.ttc`
   до встречи CJK, ASCII-быстрый путь.
4. **iced**: CPU-first-кадр как у egui требует форка `iced_winit` (компоситор создаётся синхронно);
   виртуализация списка и фоновая подсветка — обязательны для больших файлов.

## Оценка трудозатрат на полноценный продукт

* **egui (softfirst-архитектура):** 6–10 человеко-недель до качественного просмотрщика: поиск по документу,
  выделение/копирование через виртуализированные блоки, якоря/оглавление, цветные emoji (свой COLR-растеризатор
  или bitmap-emoji), лучший рендер текста (ClearType-подобного в egui нет — только замена растеризатора),
  тёмная тема, DnD, перезагрузка при изменении файла, перенос между мониторами с разным DPI (перераскладка и
  новый CPU-кадр), тесты рендеринга. Плюс сопровождение трёх форков (winit, wgpu-hal, egui_commonmark).
* **iced:** 5–8 недель по функциям (цветные emoji, подсветка, настоящий italic уже есть), но нужна своя
  виртуализация и фоновый парсер без блокировки UI; стартовое время ≥125 мс (CPU) без форка iced не
  опустить; GPU-рендерер iced для «открытия как пуля» не годится.

## Подводные камни

* **D3D12 на NVIDIA дорогой**: ~270 мс до готового устройства на RTX 5070; AMD iGPU — устройство ~135 мс,
  но первый present ~200 мс; WARP — первый кадр ~125–135 мс. Ни один GPU-путь не укладывается в 200 мс.
* **wgpu по умолчанию создаёт устройство на каждом адаптере** при перечислении — без патча +80…120 мс.
* **eframe** прячет окно до первого кадра и сам перечисляет адаптеры — поэтому свой хост.
* **winit**: кеш раскладки клавиатуры в `ShowWindow` (~100–150 мс), показ окна с размером по умолчанию и
  последующий resize, копия системного меню, 256 × `GetAsyncKeyState` — всё пропатчено; `EventLoop::new`
  всё равно ~15 мс.
* **egui** не умеет цветные emoji — Segoe UI Emoji рисуется контурами; нет ClearType; курсив синтетический.
* **iced**: `markdown::parse` сразу подсвечивает все блоки кода syntect (регекспы компилируются при первом
  использовании языка, ~15–20 мс на язык) — отсюда срез первых 8 КБ; нет виртуализации — `large.md`
  доступен целиком только через ~10–13 с с фризом UI ~2,7 с; `Segoe UI Variable` через fontdb не выбирается;
  приватные поля `markdown::Row`.
* **iced wgpu**: компоситор создаётся синхронно до первого `view` — вся стоимость D3D12 на критическом пути.
* **Defender** сканирует exe при каждом запуске: ~+2 мс на МБ; первый запуск после пересборки — 300–600 мс.
* **RTSS (MSI Afterburner)** на этой машине внедряет `RTSSHooks64.dll` во все процессы с окнами и виден в
  профиле первого `CreateWindowEx` — одинаково для всех прототипов.
* Замечаний к харнессу нет; `bench.py shot` по умолчанию пишет в `bench/results/shots/`, здесь везде
  использован `--out` внутрь каталога прототипа.

## Файлы

* `proto.json`, `build.ps1`, `README.md`
* `common/bench.rs` — протокол (отметки, DwmFlush, атомарная запись результата)
* `egui/src/`: `main.rs` (аргументы, шрифты, IME, GPU init), `shell_direct.rs` (свой winit-хост, конвейер потоков,
  paint-on-show), `soft.rs` (CPU-растеризатор + GDI blit), `view.rs` (виртуализированный документ),
  `blocks.rs` (разбиение на блоки, срез префикса), `table.rs` (GFM-таблица), `shell_eframe.rs` (stock eframe)
* `iced/src/main.rs` — iced-приложение, свой `markdown::Viewer` (заголовки, списки, цитаты, код, таблица)
* `vendor/`, `egui/vendor/` — пропатченные крейты
* `experiments/`: `compare_all.py`, `compare_ab.py`, `compare_size.py`, `exp.py` (общий раннер),
  `shot.py` (скриншот произвольной конфигурации), `marks.py` (разбивка по отметкам), `responsive.py`
  (зависания UI в обычном режиме), `visprobe.py` (внешний наблюдатель видимости окна), `winit-probe/`
  (сэмплирующий профилировщик создания окна), копии старых бинарников для A/B, логи
* `results/` — JSON быстрых прогонов; `shots/` — скриншоты

## Аудит (независимая проверка, 2026-09-19)

Аудитор прототипа `rust-gpu`: проверка честности отметок по PROTOCOL §3, сборок, работоспособности и
визуального качества. **Код, `build.ps1` и `proto.json` не менялись**: нарушений протокола не найдено.
Добавлены только инструменты и артефакты аудита: `experiments/audit_pixprobe.py`, `experiments/audit-notrans/`,
`shots/audit-*.png`, `results/audit-run.json`.

### Что проверено

| пункт | результат |
|---|---|
| Release-сборки | `cargo build --release --locked` в `egui/` и `iced/` (target-cpu) — no-op за 0,3 с (артефакты свежие); SHA-256 `out/egui/fastmd-egui.exe` и `out/iced/fastmd-iced.exe` совпадают с `target*/release/`. Профиль: `opt-level=3`, `lto=true`, `codegen-units=1`, `panic=abort`, `strip`, `+crt-static` — как заявлено |
| Кеши / резиденты / встраивание корпуса | нет: в `egui/src`, `iced/src`, `common/` нет `include_bytes!/include_str!`, записи в temp/AppData, упоминаний корпуса; шрифты — mmap системных TTF при каждом запуске; GPU в softfirst инициализируется только после `t_content` (в bench-режиме не трогается вообще — это честно отражено в имени `softfirst`) |
| `t_content` egui softfirst | кадр документа, растеризованный на CPU (`soft.rs`), выводится `SetDIBitsToDevice` + `GdiFlush` → `DwmFlush` → отметка. С `--paint-on-show 1` это делается из хука `WH_CALLWNDPROC` на первом сообщении уже видимому окну (по `notes` — `WM_WINDOWPOSCHANGING` 0x46) с проверкой размера клиентской области; кадр — полный документ, не заглушка. Скриншот без выдержки (`shots/audit-rust-gpu-egui-softfirst-medium-settle0.png`) отличается от GPU-кадра только сглаживанием: 1,2 % пикселей, из них > 32 уровней — 287 пикс. |
| `t_content` egui wgpu | `queue.present` → `device.poll(wait)` → `DwmFlush`; пиксельная проба (ниже): документ появляется на экране в первом сэмпле после `t_content` (+2,4…2,5 мс), в предыдущем сэмпле (−7…12 мс) окно ещё пустое — отметка точная |
| `t_content` iced tiny-skia | 2-е сообщение `window::frames()` после того, как `view` получил документ (+ `DwmFlush`). Кадры считаются только после `view` с документом, поэтому раньше времени отметка не срабатывает; `confirm_frames=2` консервативно (на один кадр позже необходимого, по отметкам `frame1_msg → t_content` ≈ 6 мс вместе с `DwmFlush`) — оставлено как есть |
| Первый экран больших файлов | egui — синхронно первые ~64 КБ, остальное в фоне; iced — первые 8 КБ. Разрешено PROTOCOL §3 («first viewport first»), в README описано |

### `bench.py check` (машина делилась с другими агентами)

| вариант | medium: content / window / observed, мс | large: content / window / observed, мс |
|---|---|---|
| rust-gpu-egui-softfirst | 253,9 / 246,3 / 245,5 (холодный старт после чужих сборок, launch 202 мс; дальнейшие запуски 80–100) | 86,2 / 80,1 / 79,3 |
| rust-gpu-egui-softfirst-noime | 82,2 / 71,5 / 70,8 | 81,2 / 76,6 / 75,9 |
| rust-gpu-egui-wgpu | 370,1 / 100,8 / 82,0 | 359,4 / 102,2 / 85,3 |
| rust-gpu-iced-tinyskia | 291,0 / 286,6 / 251,8 (холодный старт, launch 201 мс) | 195,7 / 190,6 / 102,9 |

Во всех случаях `observed_ms` ≤ `t_window` ≤ `t_content` — харнесс не видит окно позже отметки; предупреждений
`check` нет, выход 0.

### Независимая проба экрана (`experiments/audit_pixprobe.py`)

Харнесс проверяет только видимость окна. Проба читает **экран** (BitBlt с экранного DC = кадр,
скомпонованный DWM) по верхним 240 px клиентской области каждые 6–12 мс и сравнивает с итоговым кадром.
`reveal` = первый сэмпл, где ≥ 50 % пикселей совпадают с итогом (DWM показал окно). medium, медианы 4–5
запусков, мс от `CreateProcessW`:

| конфигурация | t_content | окно видимо (проба) | reveal на экране | reveal − t_content | reveal − видимость |
|---|---|---|---|---|---|
| baseline-win32 | 82 | 51 | 144 | +63 | 95 |
| egui softfirst, `--paint-on-show 1` (proto.json) | 100 | 84 | 176 | +75 | 95 |
| egui softfirst, `--paint-on-show 0` | 112 | 79 | 176 | +63 | 97 |
| egui softfirst-noime, `--paint-on-show 1` | 97 | 80 | 176 | +79 | 98 |
| egui softfirst-noime, `--paint-on-show 0` | 95 | 78 | 171 | +75 | 97 |
| iced tiny-skia | 141 | 100 | 198 | +55 | 97 |
| egui wgpu | 365 | 81 | 171 (пустое белое окно), документ — t_content + 2,5 | — | 91 |
| копия baseline (`audit-notrans`) | 80 | 49 | 143 | +63 | 95 |
| то же + `DWMWA_TRANSITIONS_FORCEDISABLED` | 79 | 50 | 76 | **≈ 0** (−8…+0,3) | 27 |

(У egui softfirst один из 5 запусков — выброс ~2,9 с из-за нагрузки; медианы от него не зависят.
Разрешение пробы — один сэмпл, 6–12 мс.)

Системные настройки: `SPI_GETCLIENTAREAANIMATION = 1`, анимация окон включена.

Выводы:

1. **Анимация открытия окна Windows 11 скрывает любое окно ~95 мс после того, как оно стало видимым**,
   независимо от того, когда приложение нарисовало кадр (если успело раньше). У всех стеков `t_content`
   наступает до реального появления на экране. Отметки при этом честные: кадр с документом в DWM уже есть.
2. **Paint-on-show соответствует протоколу**: это настоящий кадр документа, `GdiFlush` + `DwmFlush`. Но
   при включённой анимации его выигрыш (−10…12 мс `t_content` против `--paint-on-show 0`) **на экране не
   виден**: reveal одинаковый (176 / 176 мс). Вариант оставлен в `proto.json`. Приём реально работает, если
   переход отключён (см. п. 3), и применим к любому Win32-стеку. Но при сравнении стеков надо помнить, что
   у egui-softfirst `t_content` снят в более ранней точке `ShowWindow` (до активации окна), чем у стеков,
   которые рисуют в `WM_PAINT`, — это ~10 мс.
3. **Находка для всего проекта**: `DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, TRUE)` до
   `ShowWindow` убирает эту задержку: окно появляется на экране сразу с содержимым (reveal − t_content ≈ 0
   вместо +63 мс). Для «открытия как пуля» это **~60–65 мс реальной видимой задержки** почти даром для любого
   Win32-стека (в т. ч. winit: достаточно вызвать до показа окна). Харнесс этого не видит, потому что
   `t_content` наступает раньше reveal. Видимое пользователю время для быстрых стеков ≈
   max(`observed_ms` + ~95, `t_content`).
4. Время для egui-softfirst на экране (~176 мс) определяется моментом `ShowWindow` (~80 мс), а не
   растеризацией. Отставание от baseline (reveal 144) — это ~30 мс до показа окна: exe 7,5 МБ плюс
   `EventLoop::new` / `create_window` winit.

### Визуальная оценка (скриншоты `shots/audit-*`: medium, small, large, прокрутка medium y=700/1300)

* **egui (softfirst / noime / wgpu рисуют одинаково)** — визуально **7/10**, соответствие спецификации
  **7,5/10**. Всё обязательное есть и выглядит как на GitHub: h1–h6 (h1/h2 с линией, h6 серый), Segoe UI
  Variable, межстрочный 1,6, ссылки `#0969da`, inline code с фоном, вложенные маркированные и нумерованные
  списки, task list, цитата с полосой, hr, блок кода (фон, радиус, отступы, подсветка), GFM-таблица
  (сетка, жирная шапка, зебра, выравнивание, ширина по содержимому), PNG, кириллица и CJK. Минусы:
  **монохромные emoji** (✅ ❌ 🚀 — контуры), синтетический курсив, серое сглаживание без ClearType (текст
  чуть тоньше и мягче, чем у DirectWrite), чекбоксы в стиле egui (серые, не GitHub), пёстрая простая
  подсветка кода, лишняя иконка копирования в блоке кода.
* **iced tiny-skia** — визуально **6,5/10**: цветные emoji, настоящий курсив, чекбоксы как на GitHub,
  приглушённая подсветка syntect. Но есть **дефект рендера**: знак препинания после стилизованного
  фрагмента наследует его стиль — «**bold,**» (запятая жирная), запятая после `inline code` попадает внутрь
  моноширинной плашки. Модель спанов в `iced_widget::markdown` при этом корректна, вероятно дело в
  itemization cosmic-text. Кроме того: inline code слипается с предыдущим словом, кана в CJK-строке
  тоньше иероглифов (fallback-шрифт другого начертания), Segoe UI вместо Segoe UI Variable, таблицы
  растянуты на всю ширину, все вложенные маркеры «•».

### Контрольный прогон (`results/audit-run.json`, `--runs 5`, машина общая)

| вариант | small: медиана / мин | medium: медиана / мин | observed medium | CPU, мс | WS, МБ |
|---|---|---|---|---|---|
| baseline-win32 | 73,7 / 68,3 | 74,2 / 68,0 | 48,4 | 47 | 14 |
| rust-gpu-egui-softfirst | 85,5 / 84,5 | 84,7 / 83,0 | 77,7 | 47 | 23 |
| rust-gpu-egui-softfirst-noime | 86,8 / 80,8 | 82,6 / 81,1 | 74,6 | 47 | 23 |
| rust-gpu-iced-tinyskia | 118,5 / 117,1 | 127,3 / 123,2 | 89,7 | 250 | 78 |
| rust-gpu-egui-wgpu | 351,9 / 346,0 | 352,1 / 346,3 | 81,9 | 313 | 89 |

Совпадает с цифрами автора (85 против 89, 83 против 82, 127 против 129, 352 против 365 мс): заявления
подтверждены. Все 4 варианта годны для финального последовательного замера.
