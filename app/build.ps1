# Builds FastMD with CMake + Ninja (both ship with Visual Studio 2022) in an x64 MSVC environment.
# Usage: pwsh -File build.ps1 [-Config Release|Debug] [-Clean]
param(
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033'  # English compiler messages
Set-Location $PSScriptRoot
$msvc = Join-Path $PSScriptRoot 'tools\msvc.cmd'
$build = "build\$Config"
if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }
if (-not (Test-Path 'res\fastmd.ico')) { python tools\make_icon.py }
& $msvc cmake -S . -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
& $msvc cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
$exe = Join-Path $PSScriptRoot "build\$Config\FastMD.exe"
Write-Host "built $exe ($((Get-Item $exe).Length) bytes)"
