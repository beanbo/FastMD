# Builds fastmd-edit-tests (edit mode's window-free core, docs/EDIT-MODE.md §14.1) in the Release tree and runs it.
# Usage: pwsh -File app/tests/edit/run.ps1 [-Update] [-Filter <text>] [-NoSweep]
#   -Update rewrites the map: blocks of the golden cases with what the parser produces now (review the diff!)
param(
    [switch]$Update,
    [string]$Filter = '',
    [switch]$NoSweep
)
$ErrorActionPreference = 'Stop'
$app = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent   # …\app
$env:VSLANG = '1033'
$msvc = Join-Path $app 'tools\msvc.cmd'
$build = Join-Path $app 'build\Release'
if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    & $msvc cmake -S $app -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
}
& $msvc cmake --build $build --target fastmd-edit-tests
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$exe = Join-Path $build 'fastmd-edit-tests.exe'
$testArgs = @()
if ($Update) { $testArgs += '--update' }
if ($Filter) { $testArgs += '--filter', $Filter }
if ($NoSweep) { $testArgs += '--no-sweep' }
& $exe @testArgs
$code = $LASTEXITCODE
if ($code -ne 0) { throw "edit tests: $code failure(s)" }
Write-Host 'edit tests ok'
