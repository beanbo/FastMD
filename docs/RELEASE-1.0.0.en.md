[Русский](RELEASE-1.0.0.md) | **English**

# FastMD 1.0.0 — release notes

*Release notes as published on 21.09.2026: <https://github.com/beanbo/FastMD/releases/tag/v1.0.0>. The release commands are at the end of this file; the next release is [1.1.0](RELEASE-1.1.0.en.md).*

---

**A lightning-fast Markdown reader for Windows.** From launch to a rendered document: **66 ms**; an untuned empty
Win32 window opens in 79 ms on the same machine in the same run.

This is the first release that delivers everything planned for 1.0: stages 2–6 of the plan.

## What is new since 0.2.0

**A README from GitHub looks the way it does on GitHub.** A subset of HTML (alignment, `<details>`, `<picture>`, tables,
`<kbd>`, inline images), SVG and badges, images from the network with an on-disk cache, footnotes, emoji shortcodes, front
matter as a table, syntax highlighting for 55 languages.

**Math and diagrams.** `$E = mc^2$` and `$$…$$` are genuinely typeset — fractions, roots, sums with limits, matrices,
systems of equations (RaTeX, KaTeX-compatible). A ```` ```mermaid ```` block is drawn as a diagram: 29 kinds of diagrams, no browser.
Both libraries load only for a document that needs them.

**Working with text.** Keyboard selection, copying with formatting (HTML + RTF) and as Markdown source
(Ctrl+Shift+C), dragging text, links and images into other applications, printing and PDF export (Ctrl+P,
Ctrl+Shift+P) — as text, not as a picture: the PDF can be searched and selected.

**In Explorer.** A .md preview pane on the same engine — **10–19 ms** to a rendered document (PowerToys
Markdown Preview takes 0.6–1.4 s) — and page thumbnails in place of a file icon.

**Installing and updating.** A per-user installer, no administrator rights: `%LOCALAPPDATA%\Programs\FastMD`,
a Start menu shortcut, an entry in "Apps". An update check once a day; the download happens only with your consent and is verified
against a SHA-256; all of this can be turned off in the settings.

**Accessibility.** Narrator and NVDA read the document and move by word, line, paragraph and heading. The Windows
high-contrast theme is supported.

**Reliability.** A crash leaves a minidump in `%LOCALAPPDATA%\FastMD\crashes` (it is sent nowhere). The parser
is run through a fuzzer with AddressSanitizer — one hang on broken HTML was found and fixed before the release.

## What did not make it

- **No code signing** — it needs a certificate. On the first run SmartScreen will warn you: "More info" → "Run
  anyway".
- **No run on a low-end laptop** — it needs another computer. On this machine we checked that start-up is not bound by the
  number of cores (65 ms on four cores against 64 on sixteen).
- `sequenceDiagram` with activation (`A->>+B`) is shown as source for now — a limitation of the diagram renderer.

## Install

| How | What to do |
|---|---|
| Installer | download `FastMD-Setup.exe` and run it. No admin rights needed |
| Without installing | unpack `FastMD-1.0.0-win-x64.zip` anywhere and run `FastMD.exe` |

To check the download: the `.sha256` files sit next to it.

The full list of changes is in [CHANGELOG.md](../CHANGELOG.en.md), the plan and what remains in [docs/PLAN.md](PLAN.en.md).

---

## How to release (for the owner)

Starting with the version after 1.3.1, the release is built by GitHub Actions (`.github/workflows/release.yml`): it is enough to bump the
version in `app/CMakeLists.txt`, put the notes in `docs/RELEASE-<версия>.md` (or a `## [<версия>]` section in
CHANGELOG.md) and push the tag:

```powershell
git tag -a v1.4.0 -m "FastMD 1.4.0"
git push origin v1.4.0
```

The workflow checks that the tag matches the version, builds `app\package.ps1` on Windows, runs the edit tests, verifies the
hashes and creates a **draft** release with the zip, the installer, both `.sha256` files and the notes. The release reaches users
once the draft is published on GitHub (the update check reads `releases/latest`). The repository variable
`RELEASE_PUBLISH = true` publishes right away, with no draft; `WINGET_PUBLISH = true` together with the `WINGET_TOKEN` secret
sends a pull request to `microsoft/winget-pkgs` after publishing. "Run workflow" on the Actions tab makes a trial build
without a release.

By hand, the way versions up to 1.3.1 were released:

```powershell
pwsh -File app\package.ps1                 # zip + FastMD-Setup.exe + оба .sha256 в app\out\dist
git tag -a v1.0.0 -m "FastMD 1.0.0"
git push origin v1.0.0
gh release create v1.0.0 `
    app\out\dist\FastMD-1.0.0-win-x64.zip app\out\dist\FastMD-1.0.0-win-x64.zip.sha256 `
    app\out\dist\FastMD-Setup.exe app\out\dist\FastMD-Setup.exe.sha256 `
    --title "FastMD 1.0.0" --notes-file docs\RELEASE-1.0.0.md
```

After the release, the winget manifest: `packaging/winget/README.md`.
