# Publishes a Release build to app\out\Release\FastMD.exe — the copy the .md association points to.
# Dev builds live in app\build\Release (build.ps1), so documents open in the published FastMD never lock the file the
# next build writes, and benchmarks never measure an exe whose image is already mapped by a running viewer.
# Usage: pwsh -File publish.ps1 [-NoBuild]
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (-not $NoBuild) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Config Release
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$src = Join-Path $PSScriptRoot 'build\Release\FastMD.exe'
$dstDir = Join-Path $PSScriptRoot 'out\Release'
$dst = Join-Path $dstDir 'FastMD.exe'
New-Item -ItemType Directory -Force $dstDir | Out-Null
# copies renamed by earlier publishes: delete the ones whose processes have exited
Get-ChildItem $dstDir -Filter 'FastMD.old-*.exe' | ForEach-Object { Remove-Item $_.FullName -ErrorAction SilentlyContinue }
try {
    Copy-Item $src $dst -Force -ErrorAction Stop
} catch {
    # the published exe is running (open documents): Windows lets you rename a running image, not overwrite it
    $old = Join-Path $dstDir ("FastMD.old-{0}.exe" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Move-Item $dst $old
    Copy-Item $src $dst
    Write-Host "running copy renamed to $(Split-Path $old -Leaf); open windows keep working, new ones start the new build"
}
Write-Host "published $dst ($((Get-Item $dst).Length) bytes)"
