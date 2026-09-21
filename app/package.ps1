# Packs a release: app\out\dist\FastMD-<version>-win-x64.zip (exe, short readme, licences, changelog) + .sha256.
# The version comes from project(FastMD VERSION …) in CMakeLists.txt; the built exe must carry the same one.
# Usage: pwsh -File package.ps1 [-NoBuild]
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$ver = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\(FastMD VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
if (-not $ver) { throw 'version not found in CMakeLists.txt' }
if (-not $NoBuild) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Config Release
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$exe = Join-Path $PSScriptRoot 'build\Release\FastMD.exe'
$fileVer = (Get-Item $exe).VersionInfo.FileVersion
if ($fileVer -ne $ver) { throw "FastMD.exe is version $fileVer, CMakeLists.txt says ${ver}: rebuild" }

$dist = Join-Path $PSScriptRoot 'out\dist'
$stage = Join-Path $dist "FastMD-$ver"
$zip = Join-Path $dist "FastMD-$ver-win-x64.zip"
New-Item -ItemType Directory -Force $stage | Out-Null
Get-ChildItem $stage -File | Remove-Item
Copy-Item $exe $stage
# loaded only when a document needs them: SVG pictures, TeX formulas, Mermaid diagrams
foreach ($name in 'fastmd-svg.dll', 'fastmd-tex.dll', 'fastmd-mermaid.dll', 'fastmd-preview.dll') {
    Copy-Item (Join-Path $PSScriptRoot "build\Release\$name") $stage
}
Copy-Item (Join-Path $PSScriptRoot '..\LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $PSScriptRoot 'third_party\md4c\LICENSE.md') (Join-Path $stage 'THIRD-PARTY-md4c.txt')
Copy-Item (Join-Path $PSScriptRoot 'third_party\lunasvg\LICENSE') (Join-Path $stage 'THIRD-PARTY-lunasvg.txt')
Copy-Item (Join-Path $PSScriptRoot 'third_party\plutovg\LICENSE') (Join-Path $stage 'THIRD-PARTY-plutovg.txt')
Copy-Item (Join-Path $PSScriptRoot 'third_party\ratex\LICENSE-ratex.txt') (Join-Path $stage 'THIRD-PARTY-ratex.txt')
Copy-Item (Join-Path $PSScriptRoot 'third_party\ratex\OFL-katex-fonts.txt') (Join-Path $stage 'THIRD-PARTY-katex-fonts.txt')
Copy-Item (Join-Path $PSScriptRoot 'third_party\ratex\LICENSE-mermaid-rs-renderer.txt') (Join-Path $stage 'THIRD-PARTY-mermaid-rs-renderer.txt')
Copy-Item (Join-Path $PSScriptRoot '..\CHANGELOG.md') $stage
$readme = @"
FastMD $ver - lightning-fast Markdown reader for Windows
https://github.com/beanbo/FastMD

Run FastMD.exe: no installation, no admin rights. FastMD-Setup.exe from the same release installs it per user,
adds a Start menu shortcut, the .md association and the Explorer preview pane; it uninstalls from Settings -> Apps.
To associate .md without the installer: right-click inside the window -> "Open .md files with FastMD..." and
confirm in Settings -> Default apps.

Keys: Ctrl+F find, Ctrl+Shift+O outline, Ctrl+P print, Ctrl+Shift+P export to PDF, Ctrl+C / Ctrl+Shift+C copy as
text or as Markdown, Shift+arrows select, Ctrl+, settings, Ctrl+E open in editor, Ctrl+Alt+Left/Right column width.

The files beside the exe are loaded only when a document needs them: fastmd-svg.dll for SVG, fastmd-tex.dll for
formulas, fastmd-mermaid.dll for diagrams, fastmd-preview.dll for the Explorer preview pane and thumbnails.

The exe is not code-signed yet: Windows SmartScreen may warn on the first run (More info -> Run anyway).
License: GNU GPL v3.0 (LICENSE.txt). Source code: https://github.com/beanbo/FastMD/tree/v$ver
md4c (Markdown parser): MIT (THIRD-PARTY-md4c.txt). Formulas: RaTeX, MIT, with KaTeX fonts under the
SIL Open Font License. Diagrams: mermaid-rs-renderer, MIT. Pictures: lunasvg and plutovg, MIT.

FastMD $ver - молниеносный просмотрщик Markdown для Windows
Запустите FastMD.exe: установка и права администратора не нужны. FastMD-Setup.exe из этого же выпуска ставит
программу для одного пользователя: ярлык в «Пуске», ассоциация .md и панель просмотра в Проводнике; удаляется
через «Параметры -> Приложения». Ассоциировать .md без установщика: правый клик в окне -> «Открывать .md в
FastMD…» и подтвердить выбор в «Параметрах».

Клавиши: Ctrl+F поиск, Ctrl+Shift+O оглавление, Ctrl+P печать, Ctrl+Shift+P экспорт в PDF, Ctrl+C / Ctrl+Shift+C
копировать текстом или как Markdown, Shift+стрелки выделение, Ctrl+, настройки, Ctrl+E редактор,
Ctrl+Alt+←/→ ширина колонки.

Библиотеки рядом с exe подгружаются только когда нужны: fastmd-svg.dll для SVG, fastmd-tex.dll для формул,
fastmd-mermaid.dll для диаграмм, fastmd-preview.dll для панели просмотра и эскизов в Проводнике.

Exe пока не подписан: при первом запуске SmartScreen может предупредить («Подробнее» -> «Выполнить в любом случае»).
Лицензия: GNU GPL v3.0 (LICENSE.txt). Исходный код: https://github.com/beanbo/FastMD/tree/v$ver
"@
Set-Content -Path (Join-Path $stage 'README.txt') -Value $readme -Encoding utf8
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$zip.sha256" -Value "$hash  $(Split-Path $zip -Leaf)" -Encoding ascii
# the installer goes into the release too, with its own hash: that is what the updater downloads and checks
$setupSrc = Join-Path $PSScriptRoot 'build\Release\FastMD-Setup.exe'
if (Test-Path $setupSrc) {
    $setup = Join-Path $dist 'FastMD-Setup.exe'
    Copy-Item $setupSrc $setup -Force
    $setupHash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
    Set-Content -Path "$setup.sha256" -Value "$setupHash  FastMD-Setup.exe" -Encoding ascii
    Write-Host "packed $setup ($((Get-Item $setup).Length) bytes)"
    Write-Host "sha256 $setupHash"
}
Write-Host "packed $zip ($((Get-Item $zip).Length) bytes)"
Write-Host "sha256 $hash"
