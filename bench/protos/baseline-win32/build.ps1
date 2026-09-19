$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force out | Out-Null
& "$PSScriptRoot\..\..\tools\msvc.cmd" cl /nologo /utf-8 /O2 /MT /W3 /DUNICODE /D_UNICODE main.c /Fo:out\ /Fe:out\baseline.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib dwmapi.lib
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Remove-Item out\*.obj -ErrorAction SilentlyContinue
