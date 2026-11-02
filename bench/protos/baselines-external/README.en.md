[Русский](README.md) | **English**

# External baselines (baselines-external)

Third-party programs the user currently opens `.md` files with, measured by the same harness in
`external-title` mode: `t_content` = the moment the harness (polling every ~1 ms) saw a **visible top-level window whose
title contains the file name** (FastMD prototype windows are excluded by `FastMD (`). These programs do not implement
the protocol from the inside, so this is "time to a window with the file name", not a strict "document on screen".
After the measurement the harness sends the window `WM_CLOSE`.

## Variants

| id | Program | Version | What it shows |
|---|---|---|---|
| `ext-notepad` | `C:\Windows\System32\notepad.exe` → the Notepad Store app | 11.2607.14.0 | raw text, "Markdown syntax" mode |
| `ext-vscode` | VS Code (`%LOCALAPPDATA%\Programs\Microsoft VS Code\Code.exe`) | 1.113.0, signed by Microsoft | text editor with Markdown highlighting (not the preview) |
| `ext-cursor` | Cursor (`%LOCALAPPDATA%\Programs\cursor\Cursor.exe`) | 2.5.26, signed by Anysphere | Sign Up / Log In screen over the workbench (see below) |
| `ext-void` | Void (`C:\Program Files\Void\Void.exe`) — the user's current `.md` association | 1.99.30039, unsigned | "Welcome to Void" screen over the workbench (see below) |

VS Code, Cursor and Void are launched with an **isolated profile**: `--new-window --user-data-dir
{proto_dir}/profiles/<name>/data --extensions-dir {proto_dir}/profiles/<name>/ext --disable-workspace-trust`.
A different `--user-data-dir` means a different instance (with its own IPC channel), so the user's already running VS Code /
Cursor / Void are not affected, and their settings and extensions are not read.

## Build

```powershell
pwsh -File bench/protos/baselines-external/build.ps1            # проверить exe, пересоздать пустые профили
pwsh -File bench/protos/baselines-external/build.ps1 -KeepProfiles
```

There is nothing to compile. The script prints found/MISSING for each exe and recreates `profiles/` (the first launch
after that is a "first launch of the program").

## Results

`bench.py run ext-vscode,ext-cursor,ext-void --doc small,medium,large --runs 5` and
`bench.py run ext-notepad --doc medium --runs 5` (5 measurements + 1 warm-up; the machine was not idle). Files:
`results/quick-vscode-family.json`, `results/quick-notepad.json`.

| variant | doc | median, ms | min | of which CreateProcessW*, ms | CPU, ms | commit, MB | processes |
|---|---|---|---|---|---|---|---|
| ext-notepad | medium | **512** | 492 | 28 (System32 stub) | 672 | 92 | 2 |
| ext-vscode | small | **1437** | 1431 | 260 | 2203 | 334 | 5 |
| ext-vscode | medium | **1517** | 1483 | 263 | 2141 | 334 | 5 |
| ext-vscode | large | **1471** | 1386 | 253 | 2063 | 334 | 5 |
| ext-void | small | 1888 | 1753 | 444 | 2547 | 353 | 4 |
| ext-void | medium | 1759 | 1611 | 431 | 2078 | 353 | 4 |
| ext-void | large | 1785 | 1693 | 439 | 2219 | 351 | 4 |
| ext-cursor | small | 3759 | 3558 | 278 | 4672 | 638 | 8 |
| ext-cursor | medium | 4132 | 3865 | 277 | 5188 | 636 | 8 |
| ext-cursor | large | 3878 | 3612 | 268 | 4969 | 638 | 8 |

\* The harness's `launch_overhead_ms`. A separate measurement of a bare `CreateProcessW` (`../electron/tools_createproc.py`):
Code.exe 252 ms, Cursor.exe 271 ms, Void.exe 436 ms — this is Defender scanning large exes (Void is unsigned, so it costs more).

**First launch on a fresh profile** (right after `build.ps1`): VS Code 2555 ms, Void 10 642 ms, Cursor 23 500 ms;
second launch: 1687 / 2445 / 5853 ms. Notepad on its first launch in a session: 1270 ms (screenshot run: 875 ms).

For comparison (prototypes, same conditions): `electron` 880–930 ms, `baseline-win32` 70–90 ms.

## Screenshots (`results/shots/`)

* `ext-notepad-medium.png` — raw Markdown in a monospace font, dark theme. **The tabs of the user's Notepad session
  are blurred** (`tools_mask_tabs.py`): on every launch Notepad restores all tabs from the user's previous
  session.
* `ext-vscode-medium.png` — editor with Markdown syntax highlighting, dark theme, minimap.
* `ext-cursor-medium.png` — only the Sign Up / Log In screen: the document text is **not visible**.
* `ext-void-medium.png` — only the "Welcome to Void" onboarding: the document text is **not visible**.

## Caveats (important when comparing)

1. **On an empty profile, Cursor and Void show onboarding/login over the workbench.** The window with the file name in
   its title appears, but the text itself is not drawn; the numbers are "time to a workbench window with the file". The
   onboarding cannot be skipped with flags: Void stores its `isOnboardingComplete` flag in the encrypted (safeStorage/DPAPI)
   state `void.settingsServiceStorageII`, and Cursor requires a login. Copying the user's real profile (tokens, settings)
   into the project is unacceptable. For a user with a real profile and extensions the time is almost
   certainly **longer** (extension activation).
2. **The VS Code family opens the text editor, not the Markdown preview** (the CLI has no "open preview" switch).
   Rendering the preview would add yet another webview launch.
3. **Title ≠ pixels.** VS Code sets the title `medium.md - Visual Studio Code` once the editor is open;
   the text is drawn within a few frames, so the error is on the order of tens of ms.
4. **Notepad means someone else's state.** The time includes restoring all tabs of the user's session (they have ~20), so
   the number depends on their session. Only `medium` was measured: every newly opened file stays as a tab in the
   user's session.
5. Noisy: other agents were running in parallel.

## Run safety

* Before every Notepad launch, `Get-Process Notepad` was checked — the user did not have Notepad open
  (the harness closes the window in which the file was found via `WM_CLOSE`; with the user's Notepad open, the file
  would have opened as a tab in their window, and `WM_CLOSE` would have closed their window). **Rule: if Notepad is open, skip
  `ext-notepad`.**
* **Side effect:** a `medium.md` tab was added to the user's Notepad session (Notepad saves the session on
  close and restores it on the next launch). It can simply be closed.
* No VS Code / Cursor / Void processes remain after the measurements (the harness's Job Object).

## Files

| Path | Purpose |
|---|---|
| `proto.json` | 4 `external-title` variants |
| `build.ps1` | exe check + recreation of the isolated profiles |
| `profiles/<vscode,cursor,void>/` | isolated `--user-data-dir` / `--extensions-dir` |
| `tools_mask_tabs.py` | blurring of the user's session tabs in the Notepad screenshot |
| `results/` | runs and screenshots |
