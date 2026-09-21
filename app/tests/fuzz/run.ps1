# Builds the fuzz harness with AddressSanitizer and runs it (plan 6.5).
# Usage: pwsh -File app/tests/fuzz/run.ps1 [-Runs 50000] [-Seed 1] [-NoAsan]
param(
    [int]$Runs = 20000,
    [int]$Seed = 0,
    [switch]$NoAsan
)
$ErrorActionPreference = 'Stop'
$app = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent   # …\app
$env:VSLANG = '1033'
$msvc = Join-Path $app 'tools\msvc.cmd'
$build = Join-Path $app 'build\Fuzz'
$asan = if ($NoAsan) { 'OFF' } else { 'ON' }
& $msvc cmake -S $app -B $build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFASTMD_FUZZ=ON -DFASTMD_ASAN=$asan `
    -DFASTMD_RUST=OFF -DFASTMD_SETUP=OFF
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
& $msvc cmake --build $build --target fastmd-fuzz
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$exe = Join-Path $build 'fastmd-fuzz.exe'
# AddressSanitizer brings its own runtime DLL, which lives with the compiler and is not on PATH
if (-not $NoAsan) {
    $pattern = 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'
    $rt = Get-ChildItem $pattern -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($rt) { $env:PATH = "$($rt.DirectoryName);$env:PATH" }
}
if ($Seed -eq 0) { $Seed = Get-Random -Minimum 1 -Maximum 100000 }
Push-Location (Join-Path $app 'tests')
try {
    # run it directly, not through the MSVC wrapper: that one changes the working directory, and the harness writes
    # the input it is about to parse next to the other test output
    & $exe --runs $Runs --seed $Seed --corpus (Join-Path $app '..\bench\corpus')
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($code -ne 0) { throw "fuzzing stopped with code $code - the input that did it is app\tests\out\fuzz\last.md" }
Write-Host "fuzz ok: $Runs runs, seed $Seed$(if ($NoAsan) { '' } else { ', with AddressSanitizer' })"
