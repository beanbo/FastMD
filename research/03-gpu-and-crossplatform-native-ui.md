# 03. GPU / immediate-mode / кроссплатформенные нативные UI-тулкиты и их текст

*FastMD, фаза исследования. Дата: 2026-09-19. Автор: агент-исследователь «GPU & cross-platform native UI».*
*Лаборатория с исходниками всех замеров: [`research/lab/gpu-ui/`](lab/gpu-ui/README.md).*

> **Как читать.** Всё, что помечено **[замер]**, — мои собственные измерения на этой машине (сырые JSON лежат в
> `research/lab/gpu-ui/**/results-*.json`). Всё остальное — со ссылкой на источник (версия и дата указаны).
> Замеры делались, пока параллельно собирались прототипы других агентов, поэтому абсолютные числа «гуляют»
> в пределах ±2×. Сравнивать надёжно можно строки **одной серии** (они запускались вперемешку, round-robin).
> Финальные сравнительные цифры — за оркестратором на простаивающей машине.

---

## 0. TL;DR

1. **Главный тормоз GPU-тулкитов на этой машине — не сам тулкит, а первый аппаратный GPU-device в процессе.**
   **[замер]** `D3D11CreateDevice` на RTX 5070 (драйвер 32.0.16.1656) стоит **200–250 мс**, D3D12 — **~270 + 45 мс**,
   Vulkan instance+device — **~280 мс**, WGL (OpenGL) — **~330 мс**, D3D9Ex (WPF) — **~210 мс**. Для сравнения:
   весь baseline Win32+GDI до контента ≈ 90 мс, а **WARP** (программный D3D11) создаётся за **15–22 мс**.
   Это фиксированная цена user-mode драйвера NVIDIA; её независимо видят и другие (RTX 3090: 160 мс HW vs 3.6 мс WARP,
   [nene-nib #24, 2026-09-16](https://github.com/hideyukiMORI/nene-nib/issues/24)).
2. **iGPU не спасает**: device на AMD iGPU создаётся за 42–55 мс, но `CreateSwapChainForHwnd` для окна на мониторе,
   подключённом к NVIDIA, всё равно грузит драйвер NVIDIA (+210–240 мс). Создание device в фоне параллельно с окном
   экономит только ~50–60 мс (окно создаётся быстрее, чем драйвер).
3. **Direct2D + DirectWrite на WARP даёт первый кадр с текстом (ClearType, цветные эмодзи, CJK) за ~100–135 мс
   от `main`** против 320–410 мс на аппаратном device — **[замер]**; пиксели HW и WARP совпадают (кроме 161 пикселя
   в цветном эмодзи). Установившаяся прокрутка текста на WARP не медленнее HW (25–30 мс/кадр в обоих случаях на
   намеренно «тупом» тесте без отсечения) — для документа WARP достаточно.
4. **winit** (оконный слой egui, iced, Slint, Xilem, Floem, Freya, Blitz, Bevy) **добавляет 150–250 мс**: при первом
   `WM_SETFOCUS` он строит полную таблицу раскладки клавиатуры (16 состояний модификаторов × 256 VK × `ToUnicodeEx`).
   **[замер]** инструментированный winit 0.30.13: `prepare_layout` = 158–202 мс; C-репродукция 6800 вызовов
   `MapVirtualKeyExW/ToUnicodeEx` = 159–201 мс. Логика та же в winit 0.31-beta.3.
5. **wgpu добавляет свой налог на многоадаптерных машинах**: DX12-бэкенд при `request_adapter` создаёт D3D12-device
   на **каждом** адаптере (NVIDIA, AMD, WARP) → 350–530 мс, даже с `force_fallback_adapter` (WARP). Дефолт
   egui-wgpu 0.36 = `PRIMARY | GL` + `HighPerformance` (инициализирует Vulkan, DX12 и GL). **[замер]** минимальное
   winit+wgpu окно с одной заливкой: **680–990 мс от `main`** до первого Present.
6. **Системные шрифты**: стеки на `fontdb`/cosmic-text (iced, Floem, glyphon) сканируют все 474 начертания
   (449 МБ в `C:\Windows\Fonts`) — **30–90 мс** + первый шейпинг с fallback **25–75 мс**. Стеки на DirectWrite
   (fontique/Parley, Slint ≥1.16, gpui, Qt ≥6.8, Skia) — **1–3 мс**. egui не сканирует ничего — но и не умеет системные
   шрифты вообще (CJK и цветные эмодзи только вручную / не поддерживаются).
7. **Качество текста**: ClearType (субпиксельное AA) дают только нативные DirectWrite/D2D, Qt Widgets, WPF, а из
   GPU-мира — Zed/gpui с января 2026 (в crates.io-версии gpui 0.2.2 — grayscale). egui, iced, Slint, Makepad, Vello,
   Flutter — grayscale. На 100 % масштабе это заметная разница «на глаз» (скриншоты в §2.7).
8. **Измеренные тулкиты целиком [замер]**: Slint 1.18 (winit + software renderer) — **0.26–0.69 с** до кадра,
   Slint (winit + femtovg/OpenGL) — **0.73–1.48 с**, gpui 0.2.2 — **0.59–0.63 с**; минимальный winit+wgpu — 0.74–1.05 с.
   Ни один не приближается к 90 мс baseline и даже к ~150 мс «D2D на WARP».
9. **Вывод для FastMD**: GPU/immediate-mode/кроссплатформенные тулкиты **не подходят на роль «пули»** на машинах
   с дискретной NVIDIA без глубокой переделки их инициализации. Лучший путь — **Win32 + DirectWrite + Direct2D, первый
   кадр на WARP (или D2D software), аппаратный device — лениво/никогда**. Если понадобится кроссплатформенность —
   смотреть на **Qt 6 Widgets** (растеризация на CPU, без winit, DirectWrite + ClearType, md4c/GFM встроен) — его
   прототип делает агент `qt`; остальные кандидаты структурно платят «налог GPU» и/или «налог winit».

---

## 1. Методика и стенд

### 1.1 Машина (проверено)

| Параметр | Значение |
|---|---|
| ОС | Windows 11 Pro 26200, не elevated |
| CPU/RAM | Ryzen 9 7950X (16C/32T), 64 ГБ |
| GPU | NVIDIA GeForce RTX 5070, драйвер **32.0.16.1656** (20.08.2026, UMD `nvwgf2umx.dll` 90.9 МБ) + AMD Radeon iGPU, драйвер 32.0.21045.1000; адаптер 0 (монитор) — NVIDIA |
| Инжекции в процессы | **Windhawk** (`windhawk.dll` + мод-DLL в каждом процессе), **RTSS** (`RTSSHooks64.dll` появляется в процессах с D3D-окном), NVIDIA capture `nvspcap64.dll` в D3D-процессах; Vulkan implicit layers: RTSS, Steam overlay, Steam Fossilize |
| Раскладки | ru-RU (0419) + en-US (0409) |
| Шрифты | `C:\Windows\Fonts`: 645 файлов, 449 МБ; DirectWrite: 218 семейств; fontdb: 474 начертания |

### 1.2 Как мерил

* **Один сценарий — один процесс** (DLL и драйвер всегда «холодные в процессе», как при двойном клике), round-robin
  по 5–7 запусков, медиана. Таймер — `QueryPerformanceCounter` внутри процесса; `launch→main` — от
  `GetSystemTimePreciseAsFileTime` перед `CreateProcess` в Python-драйвере до первой строки `main`.
* «Первый кадр» — после `Present`/`EndDraw` **+ `DwmFlush()`**, как в [`bench/PROTOCOL.md`](../bench/PROTOCOL.md) §3.
  Для Slint (нет callback у software-рендерера) метка = первый `WindowEvent::RedrawRequested` → нулевой `slint::Timer`
  (срабатывает на следующем обороте цикла, т.е. после отрисовки) → `DwmFlush`. Для gpui — `Window::on_next_frame`
  (протокол разрешает «начало следующего кадра»).
* Инструменты: MSVC 14.44 (`/O2 /utf-8`), Rust 1.98.1 (`--release`, thin LTO), Python 3.13 + Pillow для скриншотов
  (`PrintWindow(PW_RENDERFULLCONTENT)`).
* Версии: wgpu **30.0.1**, winit **0.30.13**, fontdb **0.24.0**, cosmic-text **0.19.0**, fontique **0.11.1**,
  Slint **1.18.0**, gpui **0.2.2** (crates.io).

---

## 2. Практические измерения

### 2.1 Цена инициализации графических API (без окна) — [замер]

`gfxinit.exe <mode>`; «серия A» — 00:38 (нагрузка средняя), «серия B» — 01:14 (финальная, 7 прогонов).
`launch→main` для этого C-exe: 40–50 мс во всех режимах.

| Операция (в процессе, холодно) | Серия A, медиана, мс | Серия B, медиана (min), мс |
|---|---:|---:|
| `CreateDXGIFactory` + перечисление 3 адаптеров | 13.7 | 16.3 (12.8) |
| `D3D11CreateDevice` **WARP** | 15.8 | 21.7 (15.4) |
| `D3D11CreateDevice` AMD iGPU (`MINIMUM_POWER`) | 41.7 (+13 фабрика) | 48.0 (+13) |
| `D3D11CreateDevice` **NVIDIA** (адаптер по умолчанию) | 236.1 | 243.4 (199.5) |
| …второй D3D11-device в том же процессе | 32.3 | 33.7 |
| …из них `LoadLibrary(nvwgf2umx.dll)` отдельно | — | 42 (после этого device всё равно 206) |
| `D3D12CreateDevice` + очередь, AMD | 72.0 + 4.1 | 94.4 + 4.4 |
| `D3D12CreateDevice` + очередь, NVIDIA | 268.9 + 71.6 | 266.7 + 45.7 |
| D3D9Ex: `Direct3DCreate9Ex` + `CreateDeviceEx(HAL)` (то, что делает WPF) | — | 127.4 + 84.8 |
| Vulkan: `vkCreateInstance` (ICD NVIDIA+AMD, слой RTSS) | 162.7 | 209.4 |
| Vulkan: `vkCreateDevice` (NVIDIA) | 133.9 | 65.9 |
| WGL: `ChoosePixelFormat` + `wglMakeCurrent` (NVIDIA ICD) | 191.2 + 96.1 | 233.1 + 93.0 |
| **DirectWrite**: shared factory + system collection + layout с fallback (кириллица+CJK+эмодзи) | 0.5 + 0.7 + 1.0 | 0.8 + 0.7 + 1.2 |

Что грузится при `D3D11CreateDevice` на NVIDIA (**[замер]**, `EnumProcessModules` до/после, 38 → 66 модулей):
`nvldumdx.dll` (загрузчик, проверяет подписи: `wintrust`, `crypt32`, `imagehlp`), `nvgpucomp64.dll` (компилятор),
`NvMemMapStoragex.dll`, `nvwgf2umx.dll`, `nvspcap64.dll` (захват/оверлей NVIDIA), `nvppex.dll`, `nvapi64.dll`.

Vulkan с фильтрами загрузчика (**[замер]**, `run_env.py`, под нагрузкой): отключение implicit-слоёв
(`VK_LOADER_LAYERS_DISABLE=~implicit~`) **не дало выигрыша**; только ICD AMD — instance 150 мс; только NVIDIA — 370–410 мс
(под нагрузкой). То есть дорог сам драйвер NVIDIA, а не RTSS/Steam-слои.

### 2.2 Окно + первый кадр — [замер]

От первой строки `main` до «кадр предъявлен + `DwmFlush`». Прибавьте ~45–50 мс `launch→main`.

| Сценарий | Серия A, мс | Серия B, медиана (min), мс | Где время |
|---|---:|---:|---|
| **GDI**: `CreateWindow` + `ShowWindow` + `WM_PAINT` + `DwmFlush` | 50.6 | 75.4 (54.7) | окно 20 + show 30–42 |
| D3D11 **WARP** swapchain (flip), заливка | 79.3 | 99.3 (90.1) | device 17, swapchain 4.6, present 6.8 |
| D3D11 **HW NVIDIA** swapchain, заливка | 305.4 | 322.6 (305.8) | device 231, present 22 |
| D3D11 на **iGPU**, окно на мониторе NVIDIA | 386.0 | 402.5 (325.4) | device 55, **swapchain 211** (кросс-адаптер) |
| D3D11 HW, device в **фоновом потоке** параллельно с окном | 280.4 | 266.4 (252.8) | ждём поток ещё 174 мс после показа окна |
| **D2D + DirectWrite текст, WARP** swapchain | — | **106.0 (97.3)** | device 13.6, DrawTextLayout 18.5, present 6.1 |
| D2D + DirectWrite текст, HW swapchain | — | 409.6 (315.6) | device 259, draw 34, present 22 |
| D2D `ID2D1HwndRenderTarget` по умолчанию (= HW) | — | 317.5 (281.9) | создание RT 217 |
| D2D `ID2D1HwndRenderTarget` `D2D1_RENDER_TARGET_TYPE_SOFTWARE` | — | 134.7 (106.9) | RT 19.5, draw+present 33 |

**Качество HW vs WARP** (**[замер]**, `c-gfxinit/shot.py`): в D2D на flip-swapchain с `D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE`
HW и WARP дают **ClearType в обоих случаях** (11 638 vs 11 640 «цветных» пикселей бахромы); попиксельное различие —
**161 пиксель** только в области цветного эмодзи 🚀🎉.

**Установившаяся прокрутка** (`d2dscroll_*`, 120 кадров, `Present(0,0)`, 24 абзаца / 2414 px рисуются целиком каждый
кадр без отсечения — заведомо худший случай): WARP **25.9–30.3 мс/кадр**, HW **24.6–30.6 мс/кадр**. Время уходит на CPU
(DirectWrite glyph runs), а не на растеризацию — при нормальном отсечении невидимых блоков оба варианта уложатся в 60 fps.

### 2.3 wgpu 30.0.1 по бэкендам — [замер]

Headless (без окна), 5 прогонов, серия B. «pipeline» — один egui-подобный render pipeline (WGSL → бэкенд).

| Бэкенд / power preference | `Instance::new` | `request_adapter` | `request_device` | pipeline | Итого от `main` | Выбран адаптер |
|---|---:|---:|---:|---:|---:|---|
| DX12 / High | 21.8 | **378.5** | 99.0 | 7.2 | 510 | Dx12: RTX 5070 |
| DX12 / Low | 24.0 | **426.1** | 92.6 | 9.6 | 532 | Dx12: AMD |
| DX12 / `force_fallback_adapter` | 21–28 | **462–534** | 23–29 | 6.6–8.8 | 519–593 | Dx12: **WARP** |
| Vulkan / High | **217.3** | 12.2 | 68.6 | 0.9 | 299 | Vulkan: RTX 5070 |
| Vulkan / Low | **268.5** | 14.1 | 37.6 | 0.8 | 312 | Vulkan: AMD |
| GL (WGL) | **418.1** | 0.5 | 31.3 | 1.1 | 460 | Gl: RTX 5070 |
| PRIMARY (Vulkan+DX12) / None | 258.5 | 321.9 | 77.8 | 0.9 | 643 | Vulkan: RTX 5070 |
| all + `enumerate_adapters` | 457.4 | 280.6 + 356.1 | 80.3 | 1.4 | 1134 | (6 адаптеров) |

Почему DX12 так дорог даже при выборе WARP: `wgpu-hal` 30.0.1 в `dx12::Instance::enumerate_adapters` вызывает для
каждого DXGI-адаптера `Adapter::expose`, а тот создаёт полноценный `ID3D12Device` ради `CheckFeatureSupport`
(`wgpu-hal-30.0.1/src/dx12/instance.rs:166–175`, `adapter.rs:65–93`,
[docs.rs source](https://docs.rs/crate/wgpu-hal/30.0.1/source/src/dx12/adapter.rs)). На машине с NVIDIA+AMD+WARP это
~270 + ~90 + WARP ≈ 400 мс ещё до выбора. FXC-компиляция одного простого pipeline на DX12 — 7–10 мс против ~1 мс на
Vulkan (DX12 по умолчанию использует FXC: `Dx12Compiler::Fxc`, «old, slow and unmaintained» — из doc-комментария
`wgpu-types-30.0.1/src/backend.rs`). Тулкит с 5–10 pipeline (iced) или десятками compute-шейдеров (Vello) платит кратно.

С окном (winit 0.30.13 + wgpu 30, заливка + один pipeline, 4 прогона, серия B), от `main` до первого Present+DwmFlush:

| Бэкенд | Итого | Окно winit | Instance | Adapter | Device | `surface.configure` | Present |
|---|---:|---:|---:|---:|---:|---:|---:|
| DX12 / High | **780** | 269 | 26 | 346 | 95 | 9 | 8.6 |
| Vulkan / High | **835** | 232 | 232 | 20 | 63 | **253** | 1.3 |
| GL (WGL) | **681** | 263 | 281 | 0.4 | 23 | 62.5 | 4.1 |
| DX12 / Low (AMD, монитор на NVIDIA) | **994** | 240 | 24 | 391 | 74 | **176** | 31 |

Это **нижняя граница для любого тулкита «winit + wgpu»** (egui/eframe, iced, Xilem/Masonry на Vello, Floem,
Dioxus Native/Blitz на Vello) на этой машине — до загрузки шрифтов, шейпинга и вёрстки документа.

### 2.4 Налог winit — [замер]

`wgpu-probe --winonly` (только `EventLoop` + `create_window` + `DwmFlush`): `create_window` = **145–324 мс**
(медиана ≈ 245) против ~50–75 мс на голом Win32 (§2.2). Отключение drag&drop (`with_drag_and_drop(false)`) не влияет.

Инструментированная копия winit 0.30.13 (`research/lab/gpu-ui/rust/vendor/winit`, `winit-instr/`):

```
winit-t            before_ShowWindow    31.29
winit-msg 0x0007 took   212.35 ms        <- WM_SETFOCUS (вложен в WM_ACTIVATE 0x0006)
winit-layout prepare_layout(hkl=0x4090409) took   202.21 ms
winit-t             after_ShowWindow   341.96
```

Причина: на `WM_SETFOCUS` winit синтезирует нажатия (`KeyEventBuilder::synthesize_kbd_state`), а тот первым делом
вызывает `LAYOUT_CACHE.get_current_layout()` → `prepare_layout()`: для каждого из 16 состояний модификаторов и
каждого из 256 VK — `MapVirtualKeyExW` + `ToUnicodeEx` (+ `vkey_to_non_char_key`)
([keyboard.rs](https://docs.rs/crate/winit/0.30.13/source/src/platform_impl/windows/keyboard.rs),
[keyboard_layout.rs](https://docs.rs/crate/winit/0.30.13/source/src/platform_impl/windows/keyboard_layout.rs)).
C-репродукция того же цикла (`gfxinit tounicode`, 6800 вызовов) — **159–201 мс**, т.е. ~25–30 мкс на вызов
`ToUnicodeEx` на этой машине. Кэш — на процесс и на HKL: первое переключение ru↔en даст ещё одну такую паузу.
В winit **0.31.0-beta.3** (`winit-win32` crate) обработка `WM_SETFOCUS` та же. Открытых issue про это в
rust-windowing/winit я не нашёл (поиск `gh search issues`), ближайшее — #4584 (зависание с хуком Punto Switcher при
смене раскладки, открыто 2026-06-02, закрыто 2026-06-14).

Остальное, что winit делает при создании окна, дёшево (**[замер]** в C): `OleInitialize` 6.7 мс,
`CoCreateInstance(CLSID_TaskbarList)` + `HrInit` + `AddTab` 5.3 мс.

> Возможно, 25–30 мкс/вызов раздуты окружением (Windhawk внедряется в каждый процесс). Структурно проблема
> остаётся: ~6800 системных вызовов на старте ради таблицы, которая нужна только к первому нажатию клавиши.
> Это патчится в winit ~10 строками (строить таблицу лениво / не синтезировать нажатия, если ничего не нажато).

### 2.5 Системные шрифты — [замер]

`fontscan.exe <mode>`; серия B (7 прогонов, машина свободнее) и диапазон медиан ранних серий (под нагрузкой).

| Операция | Серия B, медиана | Ранние серии | Кто так делает |
|---|---:|---:|---|
| `fontdb::Database::load_system_fonts()` (474 начертания) | **32.4** | 66–89 | resvg, glyphon, cosmic-text, Floem, iced (через cosmic-text) |
| `cosmic_text::FontSystem::new()` (fontdb + локаль) | **33.9** | 82–95 | iced 0.14 (cosmic-text 0.15), Floem, glyphon |
| cosmic-text: первый `shape` строки «латиница+кириллица+CJK+эмодзи» с fallback | **24.2** | 59–76 | то же |
| `fontique::Collection::new(system_fonts: true)` (бэкенд DirectWrite) | **1.2–1.4** | 2–12 | Parley (Xilem/Masonry, Blitz), Slint ≥1.16 |
| fontique: запрос «Segoe UI Variable Text» + fallback для `Hani` | 0.7 | 1.1 | то же |
| DirectWrite (C): system collection + `CreateTextLayout` с fallback | 0.7 + 1.2 | 0.7 + 1.0 | D2D/DWrite, gpui (Windows), Qt ≥6.8, Skia (Flutter, Freya) |

Вывод: fontdb читает и парсит таблицы имён **всех** файлов шрифтов (memory map), DirectWrite же отдаёт готовый
системный кэш (служба FontCache) — разница 20–70×. fontique на Windows ходит в DirectWrite
([`backend/dwrite.rs`](https://docs.rs/crate/fontique/0.11.1/source/src/backend/dwrite.rs)).

Цветные эмодзи: **[замер]** `seguiemj.ttf` на этой машине — таблица **COLR v1** (+ 3372 base glyph записей v0 для
совместимости) и CPAL; SVG/CBDT нет. Рендерер без COLRv1 получит «плоские» v0-эмодзи, без COLR вообще — монохром/пусто.
`msyh.ttc` (CJK) — 19 МБ, `YuGothR.ttc` — 13.5 МБ: «просто вшить CJK-шрифт» (как предлагает egui) — это десятки МБ.

### 2.6 Тулкиты целиком — [замер]

`run_probe.py`, от `CreateProcess` до «кадр + DwmFlush». Окно 1000×800, заголовок 30 px, абзац с
**жирным**/*курсивом*/`code`, строка «你好，世界。こんにちは 🚀🎉✅».

| Тулкит / режим | launch→кадр, серия B (min) | другие серии | main→кадр | Разбивка |
|---|---:|---:|---:|---|
| **Slint 1.18**, `SLINT_BACKEND=winit-software` | **284 (228)** | 262–690 | 207 | компонент 18 мс, до первого `RedrawRequested` 178 (из них ~150–200 — winit/`WM_SETFOCUS`), рендер+present 10 |
| Slint 1.18, software, без feature `accessibility` | 295 (261) | 262–378 | 226 | влияние AccessKit в шуме (0–130 мс между сериями) |
| **Slint 1.18**, `winit-femtovg` (OpenGL/WGL) | **791 (729)** | 1095–1478 | 721 | + WGL/NVIDIA ≈ 300–450 мс + femtovg-шейдеры |
| **gpui 0.2.2** (Win32 + D3D11 + DirectWrite) | **630 (591)** | 613 | 556 | до `Application::run`-callback **485 мс** (платформа: OLE, 2× `D3D11CreateDevice`, DirectWrite, JumpList, VSync-поток), окно 55, кадр 16 |
| минимальный winit + wgpu (заливка) | ≈ 740–1060 | — | 681–994 | см. §2.3 |
| для сравнения: D2D-текст на WARP (C) | ≈ 150–180 | — | 106 | §2.2 |

Бинарники: `slint-probe.exe` 11.4–12.0 МБ, `gpui-probe.exe` 10.3 МБ (release, без UPX). Сборка под нагрузкой:
Slint 1 мин 30 с, gpui 2 мин 43 с (≈ 1.2–1.7 ГБ `target/`).

gpui 0.2.2 создаёт D3D11-device **дважды**: `get_adapter()` вызывает `D3D11CreateDevice` «чтобы проверить поддержку»,
затем создаёт настоящий ([`directx_devices.rs`](https://docs.rs/crate/gpui/0.2.2/source/src/platform/windows/directx_devices.rs)).
Второй на NVIDIA дешевле (~34 мс, §2.1), но это всё равно ~250–280 мс на device до первой строки UI.

### 2.7 Качество текста — [замер], скриншоты

`python shots.py` → [`lab/gpu-ui/shots/`](lab/gpu-ui/shots/). Одинаковые строки, масштаб 100 %.

![Slint software / Slint femtovg / gpui 0.2.2](lab/gpu-ui/shots/compare_top.png)

![Увеличено ×3: Slint software, Slint femtovg, gpui 0.2.2, D2D ClearType](lab/gpu-ui/shots/compare_zoom.png)

| Рендер | AA текста | «Цветных» пикселей* | Шрифт по умолчанию | Эмодзи | CJK |
|---|---|---:|---|---|---|
| D2D + DirectWrite (WARP или HW) | **ClearType** (субпиксель) | 11 640 | Segoe UI Variable (задан) | цветные (COLR) | fallback DWrite |
| Slint 1.18 software (swash) | grayscale | 142 | **не Segoe UI** (Arial-подобный) | **монохромные** 🚀/✅ | есть (fontique) |
| Slint 1.18 femtovg (swash) | grayscale | 528 | то же | цветные | есть |
| gpui 0.2.2 (DirectWrite, `DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE`) | grayscale | 409 | Segoe UI | цветные | есть |

\* пиксели с max(R,G,B) − min(R,G,B) > 40 — у grayscale-текста это только эмодзи, у ClearType — бахрома глифов.

Замечание по Slint: абзац `StyledText` в `VerticalLayout` у меня **не переносился** по ширине (ушёл за край окна) —
вероятно, нужна явная ширина; шрифт по умолчанию надо задавать `default-font-family`.

---

## 3. Обзор тулкитов

Версии и даты — по crates.io / GitHub на 2026-09-19 (`crate-src/` — распакованные исходники, по ним проверены дефолты).

### 3.1 egui / eframe (+ egui_commonmark)

* **Версия**: egui/eframe **0.36.2** (2026-09-08). eframe 0.36 зависит от **wgpu 30** и **winit 0.30.13**; дефолтная
  фича рендерера eframe — **`wgpu`** (glow опционален): `default = ["accesskit","default_fonts",…,"wgpu",…]`.
* **GPU-инициализация**: egui-wgpu `WgpuSetup::without_display_handle()` → `backends = from_env().unwrap_or(PRIMARY | GL)`,
  `power_preference = HighPerformance` ([setup.rs](https://docs.rs/crate/egui-wgpu/0.36.2/source/src/setup.rs), строки ~245–255).
  По §2.3 это худший вариант на двух-GPU машине (≈ 0.65–1.1 с только на wgpu). Можно сузить `WGPU_BACKEND=vulkan|dx12`
  или кодом (`NativeOptions.wgpu_options`), либо взять glow (WGL ≈ 0.3–0.45 с).
* **Текст**: с 0.34 (2026-03-26) растеризация **skrifa + vello_cpu**, включён hinting (#7694); с 0.35 (2026-06-25)
  шейпинг **harfrust** (кернинг, лигатуры, #8031), `subpixel_binning`, настраиваемый hinting target
  ([epaint CHANGELOG](https://github.com/emilk/egui/blob/main/crates/epaint/CHANGELOG.md)). AA — grayscale.
* **Шрифты**: только вшитые (Ubuntu-Light 362 КБ, Hack, NotoEmoji-Regular монохромный, emoji-icon-font). Системных шрифтов
  нет ([#5233 «Automatically load system fonts when needed»](https://github.com/emilk/egui/issues/5233) — открыт),
  **цветных эмодзи нет** ([#2551](https://github.com/emilk/egui/issues/2551), [#8559](https://github.com/emilk/egui/issues/8559) — открыты).
  CJK — только если вручную загрузить, например, `msyh.ttc` (19 МБ) в `FontDefinitions`.
* **Markdown**: [egui_commonmark 0.25.0](https://crates.io/crates/egui_commonmark) (2026-08-05, pulldown-cmark 0.13):
  таблицы, списки задач, картинки, подсветка кода (опц. syntect). Immediate mode — весь видимый документ
  перевёрстывается каждый кадр (галереи кэшируются), для 3.7 МБ `large.md` нужна виртуализация.
* **A11y/IME/выделение**: AccessKit → UIA (фича eframe по умолчанию); IME через winit (визуализация preedit с 0.35);
  выделение текста в `Label` (есть оптимизация выделения для больших документов, #7917; насколько удобно выделение
  через границы блоков egui_commonmark — не проверял).
* **Для FastMD**: winit-налог + wgpu-налог + нет системных шрифтов/цветных эмодзи/ClearType → «пулей» не будет
  (прототип `rust-gpu` у агента egui/iced даст точные цифры).

### 3.2 iced (встроенный `markdown`)

* **Версия**: **0.14.0** (2025-12-07; новых релизов нет, main активен). wgpu **27**, cosmic-text **0.15**, pulldown-cmark 0.12.
* **GPU**: `iced_wgpu` — `Backends::from_env().unwrap_or(PRIMARY)`, `HighPerformance`; есть **tiny-skia fallback**
  (`ICED_BACKEND=tiny-skia` — `iced_renderer/src/fallback.rs:278`) — это единственный способ уйти от GPU-налога.
  В 0.14 — «Lazy `Compositor` initialization in `winit` shell» (#2722).
* **Текст**: cosmic-text → fontdb-скан всех шрифтов (§2.5: 30–95 мс + 25–75 мс первый fallback), swash-растеризация,
  grayscale; фича `crisp` (привязка квадов к пикселям).
* **Markdown**: встроенный виджет — инкрементальный парсинг (#2776), картинки (#2786), цитаты (#3005), task lists (#3022)
  ([release 0.14.0](https://github.com/iced-rs/iced/releases/tag/0.14.0)). **Текст markdown не выделяется**
  ([discourse](https://discourse.iced.rs/t/markdown-widgets-text-should-be-selectable/1107)).
* **A11y**: нет ([#552 «Implement accessibility support»](https://github.com/iced-rs/iced/issues/552) — открыт). IME — с 0.14 (#2777).
* **Для FastMD**: при tiny-skia — оценочно ~0.35–0.5 с (winit + fontdb + CPU-рендер), при wgpu — ≥0.9 с. Невыделяемый
  текст для ридера — серьёзный UX-минус.

### 3.3 Slint (rich text)

* **Версия**: **1.18.0** (2026-09-16). Лицензия: GPL-3.0 / royalty-free 2.0 (для десктопа бесплатно с атрибуцией) / коммерческая.
* **Архитектура на Windows**: бэкенд **winit** (или Qt), рендереры: **femtovg (OpenGL)** — дефолт, **software**
  (softbuffer → GDI), опционально Skia (OpenGL/Vulkan/D3D), femtovg-wgpu, vello. Шрифты — через **fontique** (DirectWrite),
  растеризация — swash (с 1.16) ([Slint 1.16](https://slint.dev/blog/slint-1.16-released)).
* **Rich text**: `StyledText` + `@markdown(...)` появились в **1.16** (2026-04-16), рантайм-парсинг markdown из Rust/C++/JS/Python —
  в **1.17** (2026-06-24) ([1.17](https://slint.dev/blog/slint-1.17-released)). Поддерживается **только inline-подмножество**:
  курсив/жирный, зачёркивание, inline-код, ссылки (`link-clicked`), списки, `<u>`, `<font color>`
  ([docs](https://docs.slint.dev/latest/docs/slint/reference/elements/styledtext/)). **Заголовки — ошибка**
  («Markdown headings are not supported», тест `slint-1.18.0/tests/styled_text.rs`); код-блоков и таблиц нет.
  Документ пришлось бы собирать из элементов вручную (свой рендер блоков поверх md4c/pulldown-cmark).
* **Выделение текста**: `Text`/`StyledText` не выделяются (только `TextInput`/`TextEdit`, без rich text).
* **[замер]**: software — 0.26–0.69 с, femtovg — 0.73–1.48 с (§2.6); цветные эмодзи в software-рендерере — монохромные.
* **Для FastMD**: лучший из winit-тулкитов по старту (software renderer без GPU-налога), но rich text слишком беден,
  ClearType нет, winit-налог есть. Не кандидат на «пулю».

### 3.4 gpui (Zed) + gpui-component / gpui-kit

* **Версия**: crates.io **gpui 0.2.2** (2025-10-22); сам Zed — v1.20.2 (2026-09-17), gpui в монорепо ушёл вперёд.
  Windows-бэкенд: собственный Win32 + **DirectX 11** + **DirectWrite** (с Vulkan отказались из-за совместимости;
  Direct2D-растеризацию заменили на DirectWrite ради RenderDoc — [Zed blog, 2025-08-19](https://zed.dev/blog/windows-progress-report)).
  HLSL компилируется `fxc.exe` на этапе сборки (`build.rs`, `GPUI_FXC_PATH`).
* **Текст**: DirectWrite shaping + растеризация глифов в атлас; в 0.2.2 — `DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE`
  ([direct_write.rs](https://docs.rs/crate/gpui/0.2.2/source/src/platform/windows/direct_write.rs)). В Zed main —
  **субпиксельный (ClearType-подобный) рендер** с [PR #45423](https://github.com/zed-industries/zed/pull/45423)
  (merged 2026-01-06, `text_rendering_mode`), BGR-раскладка — [#55174](https://github.com/zed-industries/zed/pull/55174)
  (2026-04-29). Софтверный рендер «для систем без GPU» — открытый [PR #63936](https://github.com/zed-industries/zed/pull/63936).
* **Markdown**: [gpui-kit / gpui-component](https://github.com/longbridge/gpui-component) `TextView` — GFM, таблицы,
  подсветка кода, HTML, картинки, **выделение и копирование** ([docs](https://gpui-kit.com/component/text-view/)),
  кэширование шейпинга между кадрами (PR #3115). Функционально — лучший markdown-виджет в Rust-GPU мире.
* **A11y**: на Windows скринридер «missing completely» ([#41138](https://github.com/zed-industries/zed/issues/41138), открыт);
  AccessKit-интеграция идёт в main (PR #61926, 2026-08-07).
* **[замер]**: 0.59–0.63 с до кадра, из них 485 мс — инициализация платформы до первого пользовательского кода.
* **Для FastMD**: качество текста близко к нативному (DirectWrite), markdown-компонент сильный, но D3D11-налог
  (×2 device) и API нестабилен (crates.io отстаёт от монорепо). Не «пуля».

### 3.5 Makepad

* **Версия**: makepad-widgets **1.0.0** (2025-05-13; [HN](https://news.ycombinator.com/item?id=43971829)), репозиторий активен
  (push 2026-09-18), новых релизов на crates.io нет.
* **Windows**: собственный Win32 + **D3D11 на `EnumAdapters(0)`** (здесь — NVIDIA) и **компиляция HLSL в рантайме**
  через `D3DCompile` для каждого draw-шейдера, сгенерированного из собственного DSL
  ([d3d11.rs](https://docs.rs/crate/makepad-platform/1.0.0/source/src/os/windows/d3d11.rs), `D3d11Cx::new`, `compile_shader`).
  Итого на этой машине: D3D11-налог (~230 мс) + рантайм-FXC (по моей оценке десятки–сотни мс на набор виджетов) — не проверено.
* **Текст**: собственный стек (makepad-draw, SDF/растеризатор), **вшитые шрифты**, включая крейты
  `makepad-fonts-chinese-*` и `makepad-fonts-emoji` — системные шрифты не используются; grayscale.
* **Markdown**: виджеты `Markdown` (pulldown-cmark), `Html`, `TextFlow`.
* **A11y**: нет ([#196 «On Accessibility»](https://github.com/makepad/makepad/issues/196), открыт).
* **Для FastMD**: интересная архитектура, но D3D11-HW + рантайм-шейдеры + нет системных шрифтов/ClearType/UIA.

### 3.6 Xilem / Masonry (+ Vello, Parley)

* **Версия**: xilem/masonry **0.4.0** (2025-10-29); main активен. В Q1 2026 Masonry перешёл на абстракцию **`imaging`**
  (раньше был жёстко привязан к Vello classic) — теперь может работать на **Vello CPU** без GPU; Vello Hybrid — «roughly beta»
  ([Linebender Q1 2026, 2026-04-19](https://linebender.org/blog/tmil-25/)). Vello **0.10.0** (2026-08-14) на wgpu 29.
* **Текст**: **Parley + fontique** (DirectWrite-перечисление, §2.5: ~1–2 мс), skrifa/swash; grayscale.
* **A11y**: AccessKit встроен; IME через `ui-events`.
* **Markdown**: готового виджета нет.
* **Для FastMD**: экспериментальный фреймворк; при Vello-GPU — winit + wgpu + десятки compute-pipeline (DX12/FXC) —
  медленнее всех; при Vello CPU — только winit-налог. Для продукта преждевременно.

### 3.7 Floem

* **Версия**: **0.2.0** (2024-11-15) на crates.io; репозиторий lapce/floem активен (push 2026-06-21).
* Стек: форк winit (`floem-winit` 0.29.5), wgpu **22** (рендереры vger/vello) или tiny-skia, **cosmic-text** (fontdb-скан), taffy.
* A11y — не заявлено; markdown — нет.
* **Для FastMD**: устаревшие зависимости + оба налога + fontdb. Не кандидат.

### 3.8 Freya

* **Версия**: **0.5.0-rc.6** (2026-09-13); с 0.4 — собственное реактивное ядро вместо Dioxus ([freyaui.dev/posts/0.4](https://freyaui.dev/posts/0.4)).
* Стек: **Skia** (skia-safe) через winit + **glutin (OpenGL)** / **ash (Vulkan)**; при отключённой фиче `gpu` — softbuffer
  (CPU). Шрифты — Skia FontMgr (на Windows DirectWrite): системный fallback, цветные эмодзи (Skia умеет COLRv1).
  AccessKit. Markdown — `freya-markdown` (`MarkdownViewer`, pulldown-cmark, подсветка через `freya-code-editor`).
* **Для FastMD**: winit-налог + WGL/Vulkan-налог (≈0.3–0.45 с, §2.1) → оценочно ~0.7–1.0 с; с CPU-путём — ~0.35 с.
  Не проверено практикой.

### 3.9 Dioxus Native / Blitz

* **Версия**: dioxus-native **0.8.0-alpha.1**, blitz-dom **0.3.0-beta.2** (2026-08-24). Stylo (CSS Firefox) + Taffy + Parley,
  рендер через `anyrender` (vello / vello_cpu / vello_hybrid / skia), winit **0.31-beta**. Есть HTML/markdown-фронтенд
  (рендер HTML-строки без интерактивности — [README](https://github.com/DioxusLabs/blitz)); цель — beta к концу 2025,
  продакшн в 2026.
* Подробно разобран в отчёте `02-web-and-html-engines.md` (там же практическая лаборатория `research/lab/blitz`) — здесь
  только замечание: при GPU-рендере это тот же «winit + wgpu» (§2.3, ≥0.7 с на этой машине), при `vello_cpu` — winit-налог.

### 3.10 Qt 6 (Widgets / QML)

* **Версия**: документация Qt **6.11.2**. Лицензии: LGPLv3 / GPL / коммерческая. При **статической** линковке под LGPL
  нужно дать пользователю возможность перелинковать (объектные файлы) — [Qt: obligations](https://www.qt.io/development/open-source-lgpl-obligations);
  динамическая линковка — стандартный путь (Qt6Core/Gui/Widgets + `platforms/qwindows.dll`, ~20–30 МБ).
* **Рендеринг**: **Qt Widgets — растеризация на CPU** (raster paint engine) → **нет GPU-device вообще** (ни GPU-налога,
  ни winit). Qt Quick — QRhi, по умолчанию **D3D11** на Windows → GPU-налог ~230 мс + QML-движок; текст в Quick по
  умолчанию distance-field (grayscale), `NativeRendering` опционально.
* **Шрифты/текст**: с **6.8** шрифтовая БД по умолчанию — **DirectWrite**, «application start-up time has been improved»
  ([Qt 6.8 LTS, 2024-10-08](https://www.qt.io/blog/qt-6.8-released)); ClearType в Widgets на непрозрачных виджетах.
  С **6.9** — цветные шрифты **COLRv0/COLRv1** и сегментация эмодзи на Windows ([Emoji in Qt 6.9](https://www.qt.io/blog/emoji-in-qt-6.9)).
  Системный fallback CJK — DirectWrite.
* **Markdown**: `QTextDocument::setMarkdown(md, MarkdownDialectGitHub)` — CommonMark + таблицы, task lists,
  зачёркивание, автоссылки, front matter ([QTextDocument](https://doc.qt.io/qt-6/qtextdocument.html)); парсер —
  **md4c 0.5.3** ([attribution](https://doc.qt.io/qt-6/qtgui-attribution-md4c.html)). `QTextBrowser` даёт выделение,
  копирование, ссылки, картинки; стилизация — ограниченный CSS-поднабор QTextDocument (не «GitHub-like» один в один).
* **A11y/IME**: UIA, полноценный IME.
* **Для FastMD**: единственный кроссплатформенный тулкит в моём списке, который **структурно не платит ни GPU-, ни
  winit-налог** и даёт ClearType + md4c + UIA. Реальную цифру старта даст прототип агента `qt` (`bench/protos/qt`);
  риски — вес DLL, загрузка плагинов, LGPL-процедуры, «не совсем GitHub» вид без своего рендера блоков.

### 3.11 Dear ImGui (+ imgui_md)

* **Версия**: **1.92.9b** (2026-07-31). С **1.92.0** (июнь 2025) — динамические шрифты: «Glyphs are loaded and rasterized
  dynamically. No need to specify ranges, prebake etc.» при поддержке бэкендом `ImGuiBackendFlags_RendererHasTextures`
  ([release v1.92.0](https://github.com/ocornut/imgui/releases/tag/v1.92.0), [FONTS.md](https://github.com/ocornut/imgui/blob/master/docs/FONTS.md));
  цветные глифы — через FreeType (`ImGuiFreeTypeLoaderFlags_LoadColor`, plutosvg/lunasvg).
* Окно/GPU — выбирает приложение (`imgui_impl_win32` + dx11/dx12/gl/vulkan) → **можно взять WARP** и уложиться примерно
  в «GDI + 30–50 мс» (оценка по §2.2). Но: нет полноценного шейпинга (лигатуры/сложные письменности), нет системного
  fallback, grayscale, нет UIA, IME минимальный, выделения текста нет (только read-only `InputTextMultiline`).
* **Markdown**: [imgui_md](https://github.com/mekhontsev/imgui_md) (на md4c) — последний коммит 2022-05, 185★.
* **Для FastMD**: быстро, но «стильного» ридера с нормальной типографикой не выйдет. Не кандидат.

### 3.12 Flutter desktop

* Движок на Windows рисует через **ANGLE (GLES → D3D11)** ([Windows embedder, egl/manager](https://api.flutter.dev/windows-embedder/manager_8cc_source.html)).
  В 3.47 по умолчанию **Impeller** (OpenGLES через ANGLE) и он **заметно медленнее на старте**:
  Flutter 3.47.1 stable на Intel UHD 770: **Impeller 1232 мс** до первого кадра (из них `FlutterViewController` 1186 мс)
  против **Skia 165 мс**; main-канал: 618 vs 134 мс ([flutter#191860](https://github.com/flutter/flutter/issues/191860),
  см. также [#191353](https://github.com/flutter/flutter/issues/191353)); обходной путь — `ImpellerSwitch::Disabled` в runner.
  На этой машине к Skia-цифре добавится D3D11/ANGLE на NVIDIA (~230 мс) → оценочно **~0.35–0.45 с** (Skia) / **0.8–1.4 с**
  (Impeller). Не проверено практикой (Flutter SDK не установлен).
* Текст — Skia/Impeller, grayscale; системный fallback и цветные эмодзи — через Skia FontMgr (DirectWrite). A11y — UIA.
  Выделение — `SelectionArea`.
* **Markdown**: `flutter_markdown` объявлен **discontinued** (февраль 2025, [flutter#162966](https://github.com/flutter/flutter/issues/162966)),
  преемник — [flutter_markdown_plus](https://pub.dev/packages/flutter_markdown_plus) (Foresight Mobile); альтернатива — `markdown_widget`.
* **Для FastMD**: Dart VM + движок + ANGLE-D3D11 → не «пуля»; плюс ~20+ МБ рантайма.

---

## 4. Сводные таблицы

### 4.1 Стек на Windows

| Тулкит (версия) | Окна | GPU-путь по умолчанию | Без HW-GPU? | Шейпинг / растеризация | Системные шрифты | Markdown |
|---|---|---|---|---|---|---|
| **Win32 + D2D/DWrite** (эталон) | Win32 | D3D11 (можно **WARP**) | **да** (WARP / D2D software) | DirectWrite / DirectWrite | DirectWrite (~1 мс) | свой рендер + md4c |
| egui/eframe 0.36.2 | winit 0.30 | wgpu 30 (PRIMARY\|GL, HighPerf) | нет (glow = WGL) | harfrust / skrifa+vello_cpu | **нет** (вшитые) | egui_commonmark 0.25 |
| iced 0.14.0 | winit | wgpu 27 (PRIMARY, HighPerf) | **да**: tiny-skia | cosmic-text (rustybuzz/harfrust) / swash | fontdb (30–95 мс) | встроенный `markdown` |
| Slint 1.18.0 | winit (или Qt) | femtovg/OpenGL | **да**: software | Slint text layout / swash | fontique→DWrite | `StyledText` (inline-подмножество) |
| gpui 0.2.2 | свой Win32 | D3D11 (×2 device) | нет (PR #63936 открыт) | DirectWrite / DirectWrite | DirectWrite | gpui-kit `TextView` |
| Makepad 1.0.0 | свой Win32 | D3D11 adapter 0 + рантайм-FXC | нет | свой / SDF | **нет** (вшитые, вкл. CJK/emoji) | `Markdown`, `Html` |
| Xilem/Masonry 0.4 | winit | Vello (wgpu compute) | да: Vello CPU (main, 2026) | Parley / swash, skrifa | fontique→DWrite | нет |
| Floem 0.2.0 | форк winit 0.29 | wgpu 22 (vger/vello) | да: tiny-skia | cosmic-text / swash | fontdb | нет |
| Freya 0.5.0-rc.6 | winit | Skia/OpenGL (glutin) или Vulkan | да: softbuffer | Skia (HarfBuzz) / Skia | Skia→DWrite | `freya-markdown` |
| Blitz 0.3-beta / Dioxus Native 0.8-alpha | winit 0.31-beta | Vello (wgpu) | да: vello_cpu | Parley / vello | fontique→DWrite | HTML-путь (см. отчёт 02) |
| Qt 6.11 Widgets | свой QPA | **нет GPU** (raster) | **да, по умолчанию** | HarfBuzz / DirectWrite | DirectWrite (с 6.8) | `QTextDocument::setMarkdown` (md4c 0.5.3) |
| Qt 6.11 Quick | свой QPA | QRhi D3D11 | да: software adaptation | HarfBuzz / distance field | DirectWrite | тот же QTextDocument / свой |
| Dear ImGui 1.92.9b | приложение | приложение (можно WARP) | да | нет шейпинга / stb_truetype или FreeType | нет (вручную) | imgui_md (2022) |
| Flutter 3.47 | свой embedder | ANGLE→D3D11, Impeller | (software fallback без GPU) | Skia/Impeller (HarfBuzz) | Skia→DWrite | flutter_markdown_plus / markdown_widget |

### 4.2 Качество и функции текста

| Тулкит | ClearType / субпиксель | Hinting | Цветные эмодзи (Segoe UI Emoji = COLRv1) | CJK fallback | Выделение/копирование | IME | UIA |
|---|---|---|---|---|---|---|---|
| Win32 + D2D/DWrite | **да** [замер] | да (DWrite) | да, COLR через `ENABLE_COLOR_FONT` [замер] | да [замер] | своё | TSF/IMM | своё (UIA provider) |
| egui 0.36 | нет | да (с 0.34) | **нет** (#2551) | **нет** (вручную) | да (Label) | winit | AccessKit |
| iced 0.14 | нет | — | swash (COLRv0?) — не проверено | да (cosmic-text) [замер: 4 шрифта] | **нет** в markdown | да (0.14) | **нет** |
| Slint 1.18 | нет | — | femtovg: да; software: **монохром** [замер] | да [замер] | **нет** (Text) | winit | AccessKit |
| gpui 0.2.2 / Zed main | 0.2.2: нет; main: **да** (2026-01) | DWrite | да [замер] | да [замер] | да (gpui-kit TextView) | да | нет (в работе) |
| Makepad 1.0 | нет | — | вшитый эмодзи-шрифт | вшитые CJK | не проверено | не проверено ([#179](https://github.com/makepad/makepad/issues/179) про ввод CJK закрыт 2025-06) | нет |
| Xilem/Vello | нет | skrifa | не проверено | да | не проверено | да | AccessKit |
| Freya 0.5 | обычно нет (Skia GPU) | Skia | да (Skia COLRv1) | да | частично — не проверено | winit | AccessKit |
| Qt Widgets 6.9+ | **да** | DWrite | **да, COLRv1** (6.9) | да | да (QTextBrowser) | да | UIA |
| Dear ImGui 1.92 | нет | FreeType опц. | FreeType+plutosvg | вручную | нет | минимальный | нет |
| Flutter | нет | — | да (Skia) | да | да (`SelectionArea`) | да | UIA |

### 4.3 Время до первого кадра на ЭТОЙ машине

«Измерено» — §2; «оценка» — сумма измеренных компонентов (процесс ~45–70 + окно + GPU + шрифты), без вёрстки документа.

| Вариант | Время до кадра | Статус |
|---|---:|---|
| Baseline Win32 + GDI (оркестратор, без markdown) | ~90 мс | измерено оркестратором |
| **Win32 + D2D/DWrite на WARP**, текст с ClearType/эмодзи/CJK | **~150–180 мс** (main→кадр 97–135) | **[замер]** |
| Win32 + D2D `HwndRenderTarget SOFTWARE` | ~155–200 мс (main→кадр 107–150) | [замер] |
| Dear ImGui Win32 + DX11 **WARP** | ~150–200 мс | оценка |
| Win32 + D2D/DWrite на HW (NVIDIA) | ~360–460 мс | [замер] |
| Slint 1.18 winit + software | 0.26–0.69 с | [замер] |
| iced 0.14 + tiny-skia | ~0.35–0.5 с | оценка (winit + fontdb + CPU) |
| Qt 6 Widgets | ждём прототип `qt` (структурно без GPU/winit) | — |
| Flutter (Skia) | ~0.35–0.45 с | оценка по flutter#191860 + D3D11 |
| gpui 0.2.2 | 0.59–0.63 с | [замер] |
| Freya (Skia/OpenGL) | ~0.7–1.0 с | оценка |
| minimal winit + wgpu | 0.74–1.06 с | [замер] |
| Slint 1.18 winit + femtovg | 0.73–1.48 с | [замер] |
| egui/eframe 0.36 по умолчанию (wgpu PRIMARY\|GL) | ~1.0–1.6 с | оценка (winit ~0.25 + wgpu 0.65–1.1 + configure) |
| iced 0.14 + wgpu | ~0.9–1.5 с | оценка |
| Xilem (Vello GPU), Blitz (Vello GPU) | ≥1 с | оценка (winit + wgpu + compute-pipelines) |
| Flutter (Impeller, дефолт 3.47) | ~0.8–1.4 с | оценка по flutter#191860 |

---

## 5. Разбор по техническим осям

### 5.1 D3D11 vs D3D12 vs Vulkan vs OpenGL/ANGLE на Windows

* На NVIDIA **все аппаратные API стоят 200–350 мс** холодного старта драйвера в процессе (§2.1). Дешевле всего D3D11
  (~230 мс) и D3D9Ex (~210 мс); D3D12 + очередь (~310 мс) и Vulkan (~280 мс + ещё ~250 мс на Vulkan-swapchain в окне
  через wgpu) — дороже; OpenGL/WGL (~330 мс, из них `ChoosePixelFormat` ~190–230) — самый дорогой. ANGLE (Flutter,
  Chromium, Avalonia по умолчанию) = D3D11 + собственная трансляция шейдеров.
* **WARP** (D3D11/D3D12 software) — 15–22 мс, работает со swapchain и DirectComposition, D2D на нём рисует тот же
  ClearType. Для текстового документа производительности хватает (§2.2, прокрутка).
* **Два GPU** не помогают: `EnumAdapterByGpuPreference(MINIMUM_POWER)` даёт дешёвый device (42–55 мс), но первая же
  цепочка обмена в окно на мониторе dGPU поднимает драйвер dGPU (+210–240 мс, §2.2). А wgpu на DX12 вообще
  инициализирует все адаптеры (§2.3). На ноутбуках с MSHybrid картина другая (монитор на iGPU) — тут не проверить.
* Параллелизация (device в фоне, пока создаётся окно) экономит только ~50–60 мс: окно готово за ~60 мс, драйвер — за ~230.
  Честный вариант «окно сразу, контент потом» протокол засчитает только по `t_content`.

### 5.2 Поиск системных шрифтов

См. §2.5: fontdb/cosmic-text — 30–95 мс + 25–75 мс на первый fallback; fontique/DirectWrite/Skia — 1–3 мс. egui
экономит время, отказываясь от системных шрифтов, и за это платит отсутствием CJK/цветных эмодзи.

### 5.3 Качество текста рядом с DirectWrite

* Нативный D2D/DWrite (и Qt Widgets, WPF) — ClearType + hinting + gamma/contrast из системных настроек ClearType.
* GPU-тулкиты рисуют глифы из grayscale-атласа (egui: vello_cpu + hinting; iced/Slint: swash; Makepad: SDF; Vello:
  собственная растеризация). На 100 % масштабе это выглядит мягче/жирнее (скриншоты §2.7); на 150–200 % разница
  почти пропадает. Субпиксель в GPU-мире есть только у Zed main (2026-01).
* Субпиксельный AA несовместим с прозрачными/композиционными поверхностями (Mica, DirectComposition с альфой) — ещё одна
  причина держать фон документа непрозрачным.

### 5.4 Цветные эмодзи и CJK

Segoe UI Emoji на Windows 11 — COLRv1 (+v0 fallback) **[замер]**. DirectWrite/D2D, Qt ≥6.9, Skia (Flutter, Freya)
рисуют COLRv1; swash-стеки — в лучшем случае v0; egui — только монохромный NotoEmoji; Slint software — монохром
**[замер]**. CJK: DirectWrite/fontique/Skia/Qt — системный fallback; egui/ImGui — только вручную (+19 МБ шрифта).

### 5.5 IME, выделение, доступность

Для ридера IME нужен только в поиске. Выделение и копирование — обязательны для «читалки»: iced markdown и Slint
Text этого не умеют, egui и gpui-kit умеют, Qt/Flutter/нативный — да. UIA: AccessKit (egui, Slint, Xilem, Freya,
Blitz) даёт UIA-провайдер; iced, Makepad, gpui (Windows), ImGui — без доступности.

---

## 6. Выводы для FastMD

### 6.1 Рекомендации

1. **Не строить FastMD на GPU/immediate-mode/кроссплатформенных Rust- и Flutter-тулкитах**, если цель — «пуля».
   На типичной игровой/рабочей машине с дискретной NVIDIA каждый из них платит **200–350 мс** за драйвер, а
   winit-тулкиты — ещё **150–250 мс** за таблицу раскладки клавиатуры, а wgpu-тулкиты — ещё **до 0.5–0.8 с** за
   перебор адаптеров/бэкендов. Это не лечится настройками «снаружи» (разве что `ICED_BACKEND=tiny-skia`,
   `SLINT_BACKEND=winit-software`, `WGPU_BACKEND=…`), а только форками тулкитов.
2. **Эталонная архитектура «пули» (Windows-first)**: Win32-окно + **DirectWrite** (шейпинг, fallback, ClearType,
   COLRv1-эмодзи — всё «бесплатно», ~2 мс на инициализацию) + **Direct2D поверх D3D11 WARP** (или
   `D2D1_RENDER_TARGET_TYPE_SOFTWARE`) для первого кадра. **[замер]** ~100–135 мс от `main` до кадра с текстом,
   т.е. baseline + ~40 мс. Аппаратный device — только если понадобится (например, плавные анимации/зум): создавать
   **после** первого кадра в фоновом потоке и переключаться на следующем кадре (device-lost-логика у D2D всё равно нужна).
   Это прямая рекомендация прототипам `cpp-d2d` (и `cpp-richedit`, если он использует D2D-режим RichEdit).
3. Если позже понадобится **macOS/Linux**, из рассмотренных мною единственный структурно быстрый кандидат — **Qt 6 Widgets**
   (CPU-растеризация, DirectWrite, ClearType, md4c/GFM, UIA). Ждать цифр прототипа `qt`; при положительном результате —
   держать платформенный слой тонким (свой рендер блоков markdown поверх `QPainter`/`QTextLayout`, а не `QTextBrowser`
   с его ограниченным CSS). Альтернатива — общий C/C++/Rust-ядро (парсер md4c, вёрстка блоков) + тонкие нативные
   рендеры: DirectWrite/D2D на Windows, CoreText/CoreGraphics на macOS.
4. **Для прототипов других агентов** (чтобы сравнение было честным):
   * `rust-gpu` (egui, iced): показать и дефолт, и лучшие «ручки» — eframe с `WGPU_BACKEND=vulkan` (или glow),
     iced с `ICED_BACKEND=tiny-skia`; в README отметить, что ~150–250 мс съедает winit (§2.4).
   * `winui3`, `wpf`, `avalonia`: XAML/DComp, D3D9Ex и ANGLE/Skia тоже поднимают драйвер NVIDIA (~210–250 мс, §2.1);
     полезно отметить марку `device/renderer created`, чтобы увидеть долю.
   * `cpp-d2d`: сравнить `D3D_DRIVER_TYPE_WARP` vs `HARDWARE` для первого кадра (§2.2 — разница ~300 мс).
5. **Шрифты**: использовать системный DirectWrite-кэш; не тащить fontdb/cosmic-text; не вшивать CJK-шрифты.

### 6.2 Риски и оговорки

* **Специфика машины.** Драйвер NVIDIA 616.56 (32.0.16.1656), NVIDIA capture-хук `nvspcap64.dll`, `RTSSHooks64.dll`,
  Windhawk в каждом процессе и 30 мкс на `ToUnicodeEx` могут делать абсолютные числа хуже «чистой» машины. Но порядок
  подтверждён независимо (RTX 3090: HW 160 мс vs WARP 3.6 мс, [nene-nib #24](https://github.com/hideyukiMORI/nene-nib/issues/24)),
  а соотношения внутри одной серии стабильны.
* **Шум**: все мои серии шли под нагрузкой от сборок других агентов (одна и та же операция давала 1×–2× между сериями).
  Финальные цифры — только из серийного прогона оркестратора.
* **Не проверено практикой**: Makepad, Xilem, Floem, Freya, Flutter, Dear ImGui, Qt (последний — делает агент `qt`),
  AccessKit-стоимость (в шуме), поведение на ноутбуках с MSHybrid, на машинах только с iGPU (там HW-device должен быть
  дешевле — проверить на другой машине до финального решения).
* **WARP-риски**: на слабых CPU (2–4 ядра) прокрутка больших документов на WARP может проседать; D2D на WARP отдаёт
  ту же картинку, но GPU-эффекты (размытие, большие растры/изображения) будут на CPU. Для ридера это приемлемо,
  но стоит иметь переключение на HW после старта.
* **winit-налог** может исчезнуть в будущих версиях winit (патч тривиален), а gpui может получить software-рендер
  (PR #63936) — через год картина для Rust-тулкитов может улучшиться; на сегодня (2026-09-19) — нет.

### 6.3 Замечания к стенду (harness / протокол)

Багов в `bench/harness/bench.py` и `PROTOCOL.md` я не нашёл. Две заметки для финального прогона:
1. В каждый процесс внедряется Windhawk (`windhawk.dll`, мод `explorer-details-better-file-sizes_1.5_…dll`,
   `libunwind.whl`, `libc++.whl`), а в D3D-процессы — `RTSSHooks64.dll`; это часть «пола» 90 мс и GPU-цифр.
   Стоит зафиксировать в итоговом отчёте (или, с разрешения пользователя, сделать один контрольный прогон с
   отключёнными Windhawk/RTSS).
2. Для GPU-прототипов полезно просить марку «device создан», иначе в `t_content` не видно, что 60–80 % времени —
   драйвер, а не код прототипа.

---

## Источники

* wgpu-hal 30.0.1, DX12 enumerate/expose — <https://docs.rs/crate/wgpu-hal/30.0.1/source/src/dx12/adapter.rs>
* egui-wgpu 0.36.2 `setup.rs` (PRIMARY\|GL, HighPerformance) — <https://docs.rs/crate/egui-wgpu/0.36.2/source/src/setup.rs>
* winit 0.30.13 keyboard / keyboard_layout — <https://docs.rs/crate/winit/0.30.13/source/src/platform_impl/windows/keyboard_layout.rs>
* winit issue #4584 (Punto Switcher, 2026) — <https://github.com/rust-windowing/winit/issues/4584>
* fontique 0.11.1 DirectWrite backend — <https://docs.rs/crate/fontique/0.11.1/source/src/backend/dwrite.rs>
* egui/epaint CHANGELOG (0.34 skrifa+vello_cpu, 0.35 harfrust) — <https://github.com/emilk/egui/blob/main/crates/epaint/CHANGELOG.md>
* egui #2551 Color Emoji, #5233 system fonts, #8559 — <https://github.com/emilk/egui/issues/2551>, <https://github.com/emilk/egui/issues/5233>, <https://github.com/emilk/egui/issues/8559>
* egui_commonmark — <https://github.com/lampsitter/egui_commonmark>
* iced 0.14.0 release — <https://github.com/iced-rs/iced/releases/tag/0.14.0>; selectable markdown — <https://discourse.iced.rs/t/markdown-widgets-text-should-be-selectable/1107>; a11y #552 — <https://github.com/iced-rs/iced/issues/552>
* Slint 1.16 / 1.17 / StyledText / #9560 — <https://slint.dev/blog/slint-1.16-released>, <https://slint.dev/blog/slint-1.17-released>, <https://docs.slint.dev/latest/docs/slint/reference/elements/styledtext/>, <https://github.com/slint-ui/slint/issues/9560>
* Zed Windows (DX11, DirectWrite) — <https://zed.dev/blog/windows-progress-report>; subpixel PR #45423 — <https://github.com/zed-industries/zed/pull/45423>; BGR #55174 — <https://github.com/zed-industries/zed/pull/55174>; software render PR #63936 — <https://github.com/zed-industries/zed/pull/63936>; screen reader #41138 — <https://github.com/zed-industries/zed/issues/41138>; font rendering #39317 — <https://github.com/zed-industries/zed/issues/39317>
* gpui 0.2.2 `directx_devices.rs`, `direct_write.rs` — <https://docs.rs/crate/gpui/0.2.2/source/src/platform/windows/directx_devices.rs>
* gpui-kit / gpui-component TextView — <https://github.com/longbridge/gpui-component>, <https://gpui-kit.com/component/text-view/>
* Makepad 1.0 — <https://news.ycombinator.com/item?id=43971829>; `d3d11.rs` — <https://docs.rs/crate/makepad-platform/1.0.0/source/src/os/windows/d3d11.rs>; a11y #196 — <https://github.com/makepad/makepad/issues/196>
* Linebender Q1 2026 (imaging, Vello CPU/Hybrid) — <https://linebender.org/blog/tmil-25/>; Xilem releases — <https://github.com/linebender/xilem/releases>
* Freya 0.4 — <https://freyaui.dev/posts/0.4>; repo — <https://github.com/marc2332/freya>
* Blitz — <https://github.com/DioxusLabs/blitz>
* Qt 6.8 LTS (DirectWrite default) — <https://www.qt.io/blog/qt-6.8-released>; Qt 6.7 text — <https://www.qt.io/blog/text-improvements-in-qt-6.7>; Emoji in Qt 6.9 — <https://www.qt.io/blog/emoji-in-qt-6.9>; QTextDocument — <https://doc.qt.io/qt-6/qtextdocument.html>; md4c — <https://doc.qt.io/qt-6/qtgui-attribution-md4c.html>; LGPL obligations — <https://www.qt.io/development/open-source-lgpl-obligations>
* Dear ImGui 1.92.0 — <https://github.com/ocornut/imgui/releases/tag/v1.92.0>; FONTS.md — <https://github.com/ocornut/imgui/blob/master/docs/FONTS.md>; imgui_md — <https://github.com/mekhontsev/imgui_md>
* Flutter Impeller vs Skia startup on Windows — <https://github.com/flutter/flutter/issues/191860>, <https://github.com/flutter/flutter/issues/191353>; flutter_markdown discontinued — <https://github.com/flutter/flutter/issues/162966>; flutter_markdown_plus — <https://pub.dev/packages/flutter_markdown_plus>; embedder EGL/ANGLE — <https://api.flutter.dev/windows-embedder/manager_8cc_source.html>
* Независимое подтверждение цены D3D11 HW vs WARP на NVIDIA — <https://github.com/hideyukiMORI/nene-nib/issues/24>
* Vulkan loader env (`VK_LOADER_LAYERS_DISABLE`, `VK_LOADER_DRIVERS_SELECT`) — <https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md>
