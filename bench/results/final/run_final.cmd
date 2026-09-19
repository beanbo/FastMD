@echo off
rem Final serial measurement. Launched through explorer.exe (research/lab/startup/bin/launch.exe --mode explorer)
rem so that nothing inherits the Claude Desktop MSIX AppData/HKCU virtualization (research/05 section 11.1).
title FastMD final benchmark - do not close
cd /d C:\Main\Projects\FastMD
set PY="C:\Program Files\Python313\python.exe"
set OUT=bench\results\final
echo started %DATE% %TIME% > %OUT%\STARTED

%PY% bench\harness\bench.py run baseline-win32,cpp-d2d,cpp-d2d-warp,cpp-d2d-gpu,cpp-richedit-wl-mt,cpp-richedit,cpp-richedit-d2d,cpp-richedit-full,cpp-webview2,cpp-webview2-default,litehtml-sciter-litehtml,litehtml-sciter-litehtml-gpu,litehtml-sciter-sciter,blitz,blitz-full,blitz-gpu,rust-gpu-egui-softfirst,rust-gpu-egui-softfirst-noime,rust-gpu-egui-wgpu,rust-gpu-iced-tinyskia,rust-webview,rust-webview-tao,rust-webview-tauri,qt,qt-dll,qt-full,avalonia,avalonia-exe,avalonia-gpu,avalonia-stock,winui3,winui3-webview2,winui3-r2r,winui3-fluent,wpf,wpf-hw,wpf-apphost,wpf-plain,electron,electron-gpu,electron-default,ext-vscode,ext-void,ext-cursor,ext-notepad --doc small,medium,large --runs 12 --out %OUT%\main.json > %OUT%\main.log 2>&1

%PY% bench\harness\bench.py run cpp-d2d-resident --doc small,medium,large --runs 12 --order grouped --out %OUT%\res-d2d.json > %OUT%\res-d2d.log 2>&1
%PY% bench\harness\bench.py run cpp-webview2-resident --doc small,medium,large --runs 12 --order grouped --out %OUT%\res-wv2.json > %OUT%\res-wv2.log 2>&1

echo done %DATE% %TIME% > %OUT%\DONE
