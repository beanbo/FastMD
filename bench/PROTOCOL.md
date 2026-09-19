# FastMD bench protocol v1

Every prototype is measured by the same harness (`bench/harness/bench.py`) with the same corpus
(`bench/corpus/*.md`). The harness starts the app, the **app itself reports** when the document
content reached the screen. This makes WebView2, XAML, GPU and GDI stacks comparable.

## 1. Invocation

```
<exe> [variant args from proto.json] "<absolute path to .md>"
```

Environment set by the harness:

| Variable | Meaning |
|---|---|
| `FASTMD_BENCH_OUT` | Absolute path of the result file. If set, the app is in **bench mode**. |
| `FASTMD_BENCH_EXIT` | `1` (default) = exit right after writing the result. `0` = keep the window open (screenshot mode). |

Without `FASTMD_BENCH_OUT` the app just behaves as a normal viewer.

## 2. Timestamps

All timestamps are **UTC FILETIME ticks** (100 ns since 1601-01-01), i.e. the value of
`GetSystemTimePreciseAsFileTime`. The harness takes `t0` right before `CreateProcessW`.

| Language | How to get the tick value |
|---|---|
| C / C++ | `GetSystemTimePreciseAsFileTime(&ft)` → `((uint64)ft.dwHighDateTime << 32) \| ft.dwLowDateTime` (see `bench/common/fastmd_bench.h`) |
| C# | `DateTime.UtcNow.ToFileTimeUtc()` (.NET Core uses the precise API) — see `bench/common/FastMdBench.cs` |
| Rust | `SystemTime::now().duration_since(UNIX_EPOCH)` → `secs*10_000_000 + nanos/100 + 116_444_736_000_000_000` |
| JS (Node/Electron main) | `(performance.timeOrigin + performance.now()) * 10_000 + 116_444_736_000_000_000` |

## 3. What to mark

| Mark | Required | Definition |
|---|---|---|
| `t_content` | **yes** | The first viewport of the **rendered document** (styled headings/paragraphs, not raw text, not a blank page) has been presented. Procedure below. |
| `t_window` | recommended | The window became visible and painted *anything* (background). |
| `marks` | optional | Free-form `{name: ticks}` for a breakdown, e.g. `main`, `parsed`, `window_created`, `webview_ready`, `layout_done`. `main` = first line of *your* code (after runtime init) is very valuable. |

**Presentation procedure (must be followed for fairness):**

1. The content is drawn/submitted in the stack's normal way.
2. Wait until the stack confirms a frame containing it was produced:
   - Win32 GDI / Direct2D / DirectWrite: after `EndDraw()` / `Present()` of the frame that contains the document.
   - XAML (WinUI 3 / WPF / Avalonia): after the first *rendered* frame after the content is in the tree
     (WinUI: `CompositionTarget.Rendered` after `Loaded`/content set; WPF: `ContentRendered` then a `Dispatcher` callback at `Background` priority; Avalonia: two `TopLevel.RequestAnimationFrame` callbacks).
   - Web engines (WebView2 / Electron / Tauri / wry / Sciter): in the page, after the content is in the DOM,
     `requestAnimationFrame(() => requestAnimationFrame(() => notifyHost()))`; the host marks on receiving the message.
   - Immediate-mode GPU (egui/iced/…): at the end of the first frame that painted the document (after swap/present); if the framework can't tell, mark at the start of the *next* frame.
3. Call **`DwmFlush()`** once (dwmapi.dll) in the process that owns the window, then take `t_content`.

Never mark earlier than that (e.g. at "navigation started", "text parsed", "window created").
Never cheat: no resident/background process, no pre-rendered cache, no lazy "fake" first screen
(unless the variant id ends with `-resident`, see §6). Rendering only the first viewport first and the rest
later is **allowed and encouraged** — that is a legitimate technique; say so in the README.

## 4. Result file

Write JSON to a temp name in the same directory, then rename to `FASTMD_BENCH_OUT` (atomic):

```json
{
  "protocol": 1,
  "t_content": 133712345678901234,
  "t_window": 133712345678801234,
  "ws_bytes": 41234432,
  "marks": { "main": 133712345678701234, "parsed": 133712345678751234 },
  "notes": "optional free text"
}
```

`ws_bytes` = working set of the process that owns the window at `t_content` (optional).
Then, if `FASTMD_BENCH_EXIT` != `0`, exit cleanly (exit code 0) — close the window, quit the message loop.
Child processes (WebView2/Electron renderers) are killed by the harness via a Job Object anyway.

## 5. Window & style requirements (so screenshots are comparable)

- **Per-monitor DPI aware v2** (manifest or API), crisp text.
- Initial **client size ≈ 1000 × 800 DIP**, normal (not maximized) window, shown activated.
- Window title **must** be `<file name> — FastMD (<variant id>)`, e.g. `medium.md — FastMD (cpp-d2d)`
  (the harness uses the `FastMD (` part to never confuse prototypes with third-party apps in external mode).
- Light theme following the style spec below; the doc must be **scrollable**.
- Must render: headings h1–h6 with sizes, paragraphs with word wrap, **bold** / *italic* / `inline code` / ~~strike~~ /
  links (colored), bullet + ordered + nested + task lists, blockquotes, horizontal rules, fenced code blocks
  (monospace, background, no wrap or wrap — say which), GFM tables. Images (local PNG) and syntax highlighting are
  *nice to have* — document what is and is not supported in the README.
- Correct Unicode: the corpus contains Cyrillic, CJK and emoji.

### Style spec (GitHub-like)

| Element | Value |
|---|---|
| Body font | `Segoe UI Variable Text`, fallback `Segoe UI`; 15 px (DIP); line height ≈ 1.6; color `#1f2328` |
| Page | background `#ffffff`; content column max-width 860 DIP, centered, 32 DIP side padding |
| Headings | semibold; h1 30 px / h2 24 / h3 20 / h4 17 / h5 15 / h6 14 (`#59636e`); h1, h2 with a 1 px bottom border `#d1d9e0` |
| Code font | `Cascadia Mono`, fallback `Consolas`; 13.5 px |
| Code block | background `#f6f8fa`, radius 6, padding 16 |
| Inline code | background `#eff1f3` (≈ `rgba(129,139,152,0.12)`), radius 4, padding 0.2em 0.4em |
| Link | `#0969da` |
| Blockquote | 4 px left bar `#d1d9e0`, text `#59636e` |
| Table | 1 px borders `#d1d9e0`, header semibold, even rows `#f6f8fa`, cell padding 6/13 |

Web-engine prototypes must use the shared stylesheet `bench/common/style.css` (inline it into the page).

## 6. Resident / hand-off variants (optional)

A variant id ending in `-resident` may use a pre-started process. In `proto.json` give
`"resident": {"exe": "...", "args": [...]}`; the harness starts it once before the runs (not timed), then times
the normal `exe` (a thin launcher that forwards the path, e.g. via named pipe / `WM_COPYDATA`). The **resident**
process writes the result file, then (if `FASTMD_BENCH_EXIT` != `0`) closes the document window but keeps running.
The launcher passes `FASTMD_BENCH_OUT`/`FASTMD_BENCH_EXIT` along with the path.

## 7. proto.json (one per prototype directory `bench/protos/<id>/`)

```json
{
  "id": "cpp-d2d",
  "name": "C++ Win32 + Direct2D/DirectWrite + md4c",
  "build": "build.ps1",
  "variants": [
    {
      "id": "cpp-d2d",
      "exe": "out/fastmd-d2d.exe",
      "args": [],
      "env": {},
      "dist_dir": "out",
      "mode": "self",
      "notes": "release /O2, static CRT"
    }
  ]
}
```

- Paths are relative to the proto directory. `dist_dir` = everything that would ship (for size measurement).
- The process starts with its **exe's folder as the working directory**. In `args` / `env` values the token
  `{proto_dir}` is replaced by the absolute proto directory (e.g. Electron: `"args": ["{proto_dir}/app"]`).
  The harness appends the absolute `.md` path as the **last** argument.
- `mode`: `self` (the app follows this protocol) or `external-title` (third-party app; the harness waits for any
  visible top-level window whose title contains the file name — used for baselines like VS Code / Notepad).
- `build.ps1` must rebuild the release artifacts from scratch non-interactively (`pwsh -File build.ps1`).

## 8. Harness commands

```
python bench/harness/bench.py list
python bench/harness/bench.py check  <variant-id> [--doc medium]      # 1 run, validates the protocol
python bench/harness/bench.py run    <variant-id|all> [--doc small,medium,large] [--runs 15] [--out file.json]
python bench/harness/bench.py shot   <variant-id> [--doc medium] [--settle 1500]
python bench/harness/bench.py report <results.json>
```

Numbers measured while other builds run are noisy — use them only for rough comparisons; the final
comparison is run serially on an idle machine.
