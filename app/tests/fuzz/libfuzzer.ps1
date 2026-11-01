# Builds the libFuzzer target (tests/fuzz/fuzz_target.cpp) with clang-cl, libFuzzer and AddressSanitizer, and runs it.
# clang-cl comes with Visual Studio's "C++ Clang tools for Windows" component. It is called directly rather than through
# CMake: CMake links clang-cl targets with lld-link itself, which leaves out the fuzzer and sanitizer runtimes that
# clang-cl adds when it drives the link.
# Usage: pwsh -File app/tests/fuzz/libfuzzer.ps1 [-Seconds 300] [-BuildOnly] [libFuzzer flags...]
#   the corpus grows in app/tests/out/libfuzzer/corpus, crashes land in app/tests/out/libfuzzer/
param(
    [int]$Seconds = 300,
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
$app = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent   # …\app
$env:VSLANG = '1033'
$msvc = Join-Path $app 'tools\msvc.cmd'
$out = Join-Path $app 'build\LibFuzzer'
# (paths handed to the tools end in '/', not '\': a trailing backslash before a closing quote escapes the quote)
New-Item -ItemType Directory -Force (Join-Path $out 'generated') | Out-Null

# version.h, as CMake would generate it
$ver = [regex]::Match((Get-Content (Join-Path $app 'CMakeLists.txt') -Raw), 'project\(FastMD VERSION ([0-9]+)\.([0-9]+)\.([0-9]+)')
$h = (Get-Content (Join-Path $app 'src\version.h.in') -Raw).
    Replace('@PROJECT_VERSION_MAJOR@', $ver.Groups[1].Value).Replace('@PROJECT_VERSION_MINOR@', $ver.Groups[2].Value).
    Replace('@PROJECT_VERSION_PATCH@', $ver.Groups[3].Value).
    Replace('@PROJECT_VERSION@', "$($ver.Groups[1].Value).$($ver.Groups[2].Value).$($ver.Groups[3].Value)")
Set-Content (Join-Path $out 'generated\version.h') $h -Encoding utf8

$defs = '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS', '/DMD4C_USE_UTF16',
        '/DFASTMD_NO_APP'
$common = @('/nologo', '/O1', '/Zi', '/MT', '/EHsc', '/utf-8', '-fsanitize=fuzzer,address') + $defs
$md4c = Join-Path $out 'md4c.obj'
& $msvc clang-cl @common /c (Join-Path $app 'third_party\md4c\md4c.c') "/Fo$md4c"
if ($LASTEXITCODE -ne 0) { throw 'md4c.c failed to compile' }
$sources = 'tests\fuzz\fuzz_target.cpp', 'src\parse.cpp', 'src\html.cpp', 'src\highlight.cpp', 'src\util.cpp',
           'src\theme.cpp', 'src\editcore.cpp', 'src\editops.cpp', 'src\editfile.cpp' | ForEach-Object { Join-Path $app $_ }
$exe = Join-Path $out 'fastmd-libfuzzer.exe'
& $msvc clang-cl @common /std:c++20 /Zc:__cplusplus "/I$(Join-Path $out 'generated')" "/I$(Join-Path $app 'src')" `
    "/Fo$($out -replace '\\', '/')/" "/Fe$exe" @sources $md4c /link user32.lib advapi32.lib shlwapi.lib dwmapi.lib
if ($LASTEXITCODE -ne 0) { throw 'fastmd-libfuzzer failed to build' }
Write-Host "built $exe"
if ($BuildOnly) { return }

# the AddressSanitizer runtime DLL lives with the compiler, not on PATH
$rt = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\Llvm\x64\lib\clang\*\lib\windows\clang_rt.asan_dynamic-x86_64.dll',
                    'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll',
                    'C:\Program Files\LLVM\lib\clang\*\lib\windows\clang_rt.asan_dynamic-x86_64.dll' `
    -ErrorAction SilentlyContinue | Select-Object -First 1
if ($rt) { $env:PATH = "$($rt.DirectoryName);$env:PATH" }

# libFuzzer's own runtime is built against an older MSVC standard library than the one this target compiles with, and
# the two disagree about container annotations: ASan then reports a "container-overflow" inside libFuzzer's mutator
# (fuzzer::MutationDispatcher) after a few dozen inputs, with no frame of ours on the stack. Only that check is off;
# heap, stack and use-after-free errors are still caught.
$env:ASAN_OPTIONS = 'detect_container_overflow=0'

$work = Join-Path $app 'tests\out\libfuzzer'
$corpus = Join-Path $work 'corpus'
New-Item -ItemType Directory -Force $corpus | Out-Null
# seeds: the corpus documents (large.md is too big to mutate usefully), the test documents and the regression inputs
[string[]]$seeds = @(
    (Join-Path $app '..\bench\corpus\small.md'),
    (Join-Path $app '..\bench\corpus\medium.md')
) + @(Get-ChildItem -Path (Join-Path $app 'tests\*.md'), (Join-Path $app 'tests\fuzz\*.md'), (Join-Path $app 'tests\fuzz\regress\*') -File |
    ForEach-Object { $_.FullName })
Copy-Item $seeds $corpus -Force
& $exe "-max_total_time=$Seconds" -max_len=65536 -timeout=20 -rss_limit_mb=4096 "-artifact_prefix=$($work -replace '\\', '/')/" `
    -print_final_stats=1 @args $corpus
if ($LASTEXITCODE -ne 0) { throw "libFuzzer stopped with code $LASTEXITCODE - the input is in $work" }
