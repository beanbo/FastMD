@echo off
rem Runs the given command inside an x64 MSVC developer environment: the newest Visual Studio 2022+ or Build Tools
rem with the C++ workload, found via vswhere (developed with VS 2022 Community, MSVC 14.44, SDK 10.0.26100).
rem Example: msvc.cmd cl /nologo /O2 main.c /link user32.lib
rem          msvc.cmd cmake -G Ninja -S . -B build   (cmake + ninja ship with VS)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "VSDIR="
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo msvc.cmd: no Visual Studio 2022+ with the "Desktop development with C++" workload found 1>&2
  exit /b 1
)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo >nul
if errorlevel 1 exit /b 1
%*
