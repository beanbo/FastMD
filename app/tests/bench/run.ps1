# Builds fastmd-bench (tests/bench/bench.cpp) in the Release tree and runs it over the corpus and the generated
# documents. Arguments after the switches go to fastmd-bench as they are.
# Usage: pwsh -File app/tests/bench/run.ps1 [-Scaling] [-Filter <text>] [-MinTime <seconds>]
param(
    [switch]$Scaling,
    [string]$Filter = '',
    [double]$MinTime = 0.3
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
& $msvc cmake --build $build --target fastmd-bench
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$benchArgs = @('--corpus', (Join-Path $app '..\bench\corpus'), '--min-time', $MinTime)
if ($Filter) { $benchArgs += '--filter', $Filter }
if ($Scaling) { $benchArgs += '--scaling' }
& (Join-Path $build 'fastmd-bench.exe') @benchArgs @args
if ($LASTEXITCODE -ne 0) { throw "fastmd-bench: exit code $LASTEXITCODE (a superlinear case, see above)" }
