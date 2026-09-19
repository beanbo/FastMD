@echo off
rem Runs the given command inside an x64 MSVC developer environment (VS 2022 Community, MSVC 14.44, SDK 10.0.26100).
rem Example: bench\tools\msvc.cmd cl /nologo /O2 main.c /link user32.lib
rem          bench\tools\msvc.cmd cmake -G Ninja -S . -B build   (cmake + ninja ship with VS)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo >nul
if errorlevel 1 exit /b 1
%*
