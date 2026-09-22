# FastMD

English · [Русский](README.ru.md)

**A lightning-fast Markdown reader for Windows.** About 65 ms pass between launching it and a rendered document on screen. An untuned empty Win32 window takes longer than that to open.

*C++20, Win32, DirectWrite, md4c. The interface speaks English and Russian; the rest of the documentation is in Russian.*

![FastMD 0.2: table of contents, code, lists](docs/img/fastmd-0.2-light.png)

## Download

**[FastMD 1.1.1 — latest release](https://github.com/beanbo/FastMD/releases/latest):**
1. `FastMD-Setup.exe` — a per-user install, no administrator rights: a Start menu shortcut, the `.md` association, a preview pane in Explorer. Uninstalls through Settings → Apps.
2. Or `FastMD-1.1.1-win-x64.zip` — unpack it anywhere and run `FastMD.exe`; to make `.md` files open on a double click, pick "Open .md files with FastMD…" from the context menu and confirm the choice in Settings.

The exe is not signed yet, so on the first run Windows SmartScreen may warn you: "More info" → "Run anyway". Code signing is waiting on a certificate (task 5.5).

## Why it is this fast

The stack was picked by measurement: 12 prototypes, 47 variants, one harness and one protocol. The details are in [REPORT.md](REPORT.md). Below is the time from process start to a rendered document (medium.md, 53 KB, median):

| What is opened | ms |
|---|---|
| **The FastMD prototype: C++, Win32, DirectWrite, first frame on the CPU** | **59** |
| An empty, untuned Win32 window | 78 |
| Qt 6 Widgets | 149 |
| WinUI 3 (C#, NativeAOT) | 324 |
| WebView2 / Tauri | 447–630 |
| Electron (the Typora, Obsidian class) | 753–903 |
| VS Code, the window alone | 1,138 |

FastMD itself in the control run: 66.7 ms against 79.2 ms for the empty window under the same conditions. Everything added since then works off the critical path of the first frame.

The techniques that matter:

- the first frame is drawn on the CPU (DirectWrite → GDI DIB), because a GPU device created before the first frame would have cost +150–250 ms;
- only the first screen is laid out at first, the rest is done in the background after the first frame;
- one small exe (~0.7 MB), static CRT, no runtimes;
- when the window is first shown, IME initialization on the UI thread and the open animation are both off; the search box, which needs IME, lives on a thread of its own.

### When a launch takes longer than usual

Now and then — usually the first time after an install or an update — the window appears noticeably later than it normally does. That is not FastMD: the antivirus is scanning an exe it has not seen before, and it does so inside `CreateProcessW`, before the program's first line of code runs at all. The launches after it are quick again, because the verdict is cached.

To be rid of it entirely, add the program's folder to the antivirus exclusions. For Microsoft Defender: Windows Security → Virus & threat protection → Virus & threat protection settings → Manage settings → Exclusions → Add or remove exclusions → Add an exclusion → Folder, and point it at

```
%LOCALAPPDATA%\Programs\FastMD
```

(or, if you unpacked the zip, at the folder you unpacked it into). The exclusion lifts scanning from that folder only; everything else on the machine stays protected exactly as before.

## Features

- **Markup:** CommonMark and GFM (tables, task lists, strikethrough, autolinks), GitHub Alerts, footnotes, YAML front matter as a table, emoji shortcodes, syntax highlighting for 55 languages, color emoji, CJK.
- **Like GitHub:** a subset of HTML (alignment, `<details>`, `<picture>`, tables, `<kbd>`, inline images), SVG and badges, images from the network with an on-disk cache.
- **Math and diagrams:** `$…$` and `$$…$$` are genuinely typeset (RaTeX, KaTeX-compatible), and a ```` ```mermaid ```` block is drawn as a diagram — `graph` / `flowchart` is laid out by our own layout engine, the other kinds are drawn by the library. A diagram takes the full width of the window, and where even that is not enough it scrolls sideways instead of shrinking into illegibility. Both libraries load only for a document that needs them.
- **Reading:**
  - a table of contents that follows the current section (Ctrl+Shift+O);
  - wide code and tables scroll sideways (Shift+wheel, touchpad);
  - column width from narrow to full (Ctrl+Alt+← / →);
  - the Sitka book font, and a text size;
  - GitHub light and dark themes, Mica, the Windows high-contrast theme.
- **Working with text:**
  - selection by mouse and by keyboard (Shift+arrows, by word, to the ends of the line and of the document);
  - copying with formatting (HTML and RTF) and as Markdown source (Ctrl+Shift+C);
  - dragging text, links and images into other applications;
  - printing and PDF export (Ctrl+P, Ctrl+Shift+P) — as text, not as a picture.
- **Search:** Ctrl+F, case sensitivity, whole words, match marks on the scrollbar, IME input.
- **Memory:**
  - a document opens at the place where you closed it;
  - the window opens where it was;
  - with no file given it shows recent documents with search by name; the same list is in the Jump List.
- **Navigation:** GitHub-style heading anchors, relative `.md` links with back / forward history, Tab through links, link and image context menus.
- **In Explorer:** a .md preview pane on the same engine (10–19 ms against 0.6–1.4 s for PowerToys) and page thumbnails in place of a file icon.
- **Environment:**
  - a per-user installer, no administrator rights, and an update check once a day (which can be turned off);
  - live reload when the file changes;
  - "Open in editor" with an editor of your choice;
  - a settings window, interface in Russian and English;
  - screen reader support: the document, and movement by word, line, paragraph and heading.

  A 3.7 MB document shows its first screen in the same ~66 ms.

## Privacy

FastMD neither collects nor sends anything about you: no telemetry, no identifiers, no analytics. It goes to the network in exactly two cases, and both are visible:

| What | When | Where |
|---|---|---|
| Images from the document | if the document has an `http(s)` image and the setting is "Always" (the default) or you allowed it | to the addresses in the document itself, https only, no cookies and no authentication |
| Update check | once a day after the first frame, unless turned off in settings | `api.github.com`, one GET; the installer is downloaded only after you agree to it and is verified against a SHA-256 |

Everything else stays on the machine: settings in `HKCU\Software\FastMD`, reading positions and recent documents in `%LOCALAPPDATA%\FastMD\positions.bin`, the image cache in `%LOCALAPPDATA%\FastMD\cache`, crash reports in `%LOCALAPPDATA%\FastMD\crashes` (they are sent nowhere). The Explorer preview pane does not go to the network at all.

Keys, settings and how the code is put together are described in [app/README.md](app/README.md), the limitations of the current version and the roadmap in [docs/PLAN.md](docs/PLAN.md), the history of changes in [CHANGELOG.md](CHANGELOG.md).

## Building from source

You need Windows 10/11 x64 and Visual Studio 2022 or newer: any edition, or the Build Tools, with the "Desktop development with C++" workload. CMake and Ninja come with VS.

```powershell
pwsh -File app/build.ps1    # → app/build/Release/FastMD.exe
```

## What is in the repository

| Path | What is there |
|---|---|
| `app/` | the viewer itself |
| `REPORT.md` | the final report on choosing the stack |
| `research/` | six background reports: native UI, web engines, GPU stacks, the Markdown pipeline, Windows application startup, a survey of the products |
| `bench/` | the measurement harness and protocol, the test corpus, the results, descriptions of the 12 prototypes |
| `docs/PLAN.md` | the roadmap |

## License

FastMD © 2026 seka. Distributed under the [GNU GPL v3.0](LICENSE).

Third-party libraries are bundled, all of them under MIT:

| Library | What for | License |
|---|---|---|
| [md4c](https://github.com/mity/md4c) | Markdown parsing | [app/third_party/md4c/LICENSE.md](app/third_party/md4c/LICENSE.md) |
| [lunasvg](https://github.com/sammycage/lunasvg) and plutovg | SVG rendering | [app/third_party/lunasvg/LICENSE](app/third_party/lunasvg/LICENSE) |
| RaTeX | math typesetting | [app/third_party/ratex/LICENSE-ratex.txt](app/third_party/ratex/LICENSE-ratex.txt) |
| mermaid-rs-renderer | Mermaid parsing and every diagram other than `graph` / `flowchart` | [app/third_party/ratex/LICENSE-mermaid-rs-renderer.txt](app/third_party/ratex/LICENSE-mermaid-rs-renderer.txt) |

The KaTeX fonts that are embedded into formulas are distributed under the [SIL Open Font License](app/third_party/ratex/OFL-katex-fonts.txt).
