[Русский](README.md) | **English**

# winget: the FastMD manifest (stage 5.7)

The three files next to this one are a ready manifest for `microsoft/winget-pkgs`. Three values are substituted into
them, and they only appear together with a release:

| Placeholder | Where to get it |
|---|---|
| `VERSION` | the release version without the `v`, e.g. `1.0.0` (the same one as in `app/CMakeLists.txt`) |
| `SHA256` | from `FastMD-Setup.exe.sha256`, which `app/package.ps1` puts out |
| `DATE` | the release date, `YYYY-MM-DD` |

## How to release

```powershell
pwsh -File app\package.ps1                      # соберёт zip, FastMD-Setup.exe и оба .sha256
$ver  = '1.0.0'
$hash = (Get-Content app\out\dist\FastMD-Setup.exe.sha256).Split(' ')[0]
Get-ChildItem packaging\winget\*.yaml | ForEach-Object {
    (Get-Content $_ -Raw).Replace('VERSION', $ver).Replace('SHA256', $hash).
        Replace('DATE', (Get-Date -Format 'yyyy-MM-dd')) | Set-Content $_.FullName -Encoding utf8
}
winget validate --manifest packaging\winget
winget install --manifest packaging\winget      # проверить установку у себя
```

After that the manifest goes into a fork of `microsoft/winget-pkgs` under
`manifests/b/beanbo/FastMD/<версия>/` and is submitted as a pull request. **The project owner does this:** publishing
to the shared catalog is an action on their behalf, and a live Microsoft reviewer takes part in it.

Good to know:

- `Scope: user` — the installer puts the program into the user's profile, no administrator rights needed;
- `Silent: /S` — the same switch the auto-update uses;
- `UpgradeBehavior: install` — a new version installs over the old one; settings and reading positions stay in the profile;
- while the exe is not signed (task 5.5), installing shows a SmartScreen warning; signing removes that too.
