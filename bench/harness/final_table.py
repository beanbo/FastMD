"""Pivot the final serial measurement into report tables (markdown) + a compact JSON for the report page.

python bench/harness/final_table.py bench/results/final/main.json [bench/results/final/res-*.json ...]
"""
import json
import pathlib
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import bench  # noqa: E402

# family, human label, audit visual score (1-10) — scores from the independent audits (round 1/2)
META = {
    "baseline-win32": ("floor", "Эталон: пустое Win32+GDI окно (без markdown)", None),
    "cpp-d2d": ("native", "C++ Win32 + DirectWrite → GDI DIB (без GPU) + md4c", 9),
    "cpp-d2d-warp": ("native", "C++ Win32 + Direct2D на WARP (CPU) + md4c", 9),
    "cpp-d2d-gpu": ("native", "C++ Win32 + Direct2D на аппаратном D3D11 (RTX 5070)", 9),
    "cpp-d2d-resident": ("resident", "C++ D2D/DWrite, резидентный процесс (hand-off)", 9),
    "cpp-richedit-wl-mt": ("native", "C++ RichEdit windowless (GDI) + md4c→RTF", 7),
    "cpp-richedit": ("native", "C++ RichEdit (обычный контрол) + md4c→RTF", 7),
    "cpp-richedit-d2d": ("native", "C++ RichEdit windowless через D2D/DirectWrite", 7),
    "cpp-richedit-full": ("native", "C++ RichEdit, весь документ до первого кадра", 7),
    "litehtml-sciter-litehtml": ("html-lite", "litehtml + свой D2D/DWrite-контейнер (CPU)", 9),
    "litehtml-sciter-litehtml-gpu": ("html-lite", "litehtml + D2D HwndRenderTarget (GPU)", 9),
    "litehtml-sciter-sciter": ("html-lite", "Sciter.JS 6", 7),
    "blitz": ("html-lite", "Blitz (Rust: Stylo+Taffy+Parley) CPU-растр", 8),
    "blitz-full": ("html-lite", "Blitz, весь документ до первого кадра", 8),
    "blitz-gpu": ("html-lite", "Blitz на GPU (vello_hybrid/wgpu DX12)", 8),
    "rust-gpu-egui-softfirst": ("rust-ui", "egui (первый кадр на CPU, пропатченный winit)", 7),
    "rust-gpu-egui-softfirst-noime": ("rust-ui", "egui softfirst + без IME", 7),
    "rust-gpu-egui-wgpu": ("rust-ui", "egui на wgpu DX12 (чистый GPU)", 7),
    "rust-gpu-iced-tinyskia": ("rust-ui", "iced 0.14 (tiny-skia CPU)", 6.5),
    "qt": ("qt", "Qt 6.12 Widgets (статический, в DLL)", 8.5),
    "qt-dll": ("qt", "Qt 6.12 Widgets (официальные DLL)", 8.5),
    "qt-full": ("qt", "Qt 6.12, весь документ до первого кадра", 8.5),
    "avalonia": ("dotnet", "Avalonia 12 NativeAOT (DLL+лаунчер), Skia CPU", 8),
    "avalonia-exe": ("dotnet", "Avalonia 12 NativeAOT exe, Skia CPU", 8),
    "avalonia-gpu": ("dotnet", "Avalonia 12 NativeAOT, ANGLE/D3D11", 8),
    "avalonia-stock": ("dotnet", "Avalonia 12 по умолчанию (JIT, Fluent, ANGLE)", 8),
    "winui3": ("dotnet", "WinUI 3 (WinAppSDK 1.8) NativeAOT, XAML", 8),
    "winui3-r2r": ("dotnet", "WinUI 3 JIT+ReadyToRun", 8),
    "winui3-fluent": ("dotnet", "WinUI 3 NativeAOT + Fluent-ресурсы + Mica", 8),
    "winui3-webview2": ("web", "WinUI 3 + WebView2", 9),
    "wpf": ("dotnet", "WPF .NET 8 R2R + нативный лаунчер, все ручки", 8),
    "wpf-hw": ("dotnet", "WPF, GPU с первого кадра", 8),
    "wpf-apphost": ("dotnet", "WPF, штатный apphost", 8),
    "wpf-plain": ("dotnet", "WPF «как в учебнике» (JIT, Aero2)", 8),
    "cpp-webview2": ("web", "C++ Win32 + WebView2 (тюнинг)", 9),
    "cpp-webview2-default": ("web", "C++ Win32 + WebView2 (без тюнинга)", 9),
    "cpp-webview2-resident": ("resident", "WebView2, резидентный процесс (hand-off)", 9),
    "rust-webview": ("web", "Rust wry + WebView2", 9),
    "rust-webview-tao": ("web", "Rust wry + tao + WebView2", 9),
    "rust-webview-tauri": ("web", "Tauri 2 (минимальное приложение)", 9),
    "electron": ("web", "Electron 44 (тюнинг, упакован)", 9),
    "electron-gpu": ("web", "Electron 44 с GPU-процессом", 9),
    "electron-default": ("web", "Electron 44 «типичное приложение»", 9),
    "ext-vscode": ("external", "VS Code 1.113 (редактор, не превью; до окна с именем файла)", None),
    "ext-void": ("external", "Void (текущая ассоциация .md; онбординг поверх)", None),
    "ext-cursor": ("external", "Cursor 2.5 (экран логина поверх)", None),
    "ext-notepad": ("external", "Блокнот Windows 11 (синтаксис md, без рендера)", None),
}


def main() -> None:
    files = [pathlib.Path(p) for p in sys.argv[1:]]
    runs = []
    for f in files:
        runs += json.loads(f.read_text(encoding="utf-8"))["runs"]
    variants = bench.load_variants()
    rows = bench.summarize(runs, variants)
    by = {}
    for r in rows:
        by.setdefault(r["variant"], {})[r["doc"]] = r
    order = sorted(by, key=lambda v: (by[v].get("medium", {}).get("median_ms") or 1e9))
    out = []
    print("| # | вариант | стек | small | medium | large | p90 med | окно med | CPU мс | commit МБ | дистр. МБ | визуал |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for i, v in enumerate(order, 1):
        d = by[v]
        fam, label, vis = META.get(v, ("?", v, None))
        m = d.get("medium", {})
        cell = lambda doc: "" if d.get(doc, {}).get("median_ms") is None else f"{d[doc]['median_ms']:.0f}"  # noqa: E731
        print(f"| {i} | `{v}` | {label} | {cell('small')} | **{cell('medium')}** | {cell('large')} | "
              f"{m.get('p90_ms') or ''} | {m.get('window_ms') or m.get('observed_ms') or ''} | {m.get('cpu_ms') or ''} | "
              f"{m.get('peak_commit_mb') or ''} | {m.get('dist_mb') or ''} | {vis or ''} |")
        out.append({"id": v, "family": fam, "label": label, "visual": vis,
                    **{f"{doc}_{k}": d.get(doc, {}).get(k) for doc in ("small", "medium", "large")
                       for k in ("median_ms", "p90_ms", "min_ms", "first_ms", "window_ms", "observed_ms", "cpu_ms",
                                 "peak_commit_mb", "ws_mb", "processes", "runs", "failed")},
                    "dist_mb": m.get("dist_mb"), "errors": sorted({e for doc in d.values() for e in doc["errors"]})})
    errs = [(o["id"], o["errors"]) for o in out if o["errors"]]
    for vid, e in errs:
        print(f"  ! {vid}: {e}")
    # marks breakdown (medians over medium runs) for the report's "where time goes" section
    marks = {}
    for r in runs:
        if r.get("ok") and r["doc"] == "medium" and r.get("marks_ms"):
            for k, val in r["marks_ms"].items():
                if val is not None:
                    marks.setdefault(r["variant"], {}).setdefault(k, []).append(val)
    mk = {v: {k: round(statistics.median(xs), 1) for k, xs in ks.items()} for v, ks in marks.items()}
    launch = {}
    for r in runs:
        if r.get("ok") and r["doc"] == "medium" and r.get("launch_overhead_ms") is not None:
            launch.setdefault(r["variant"], []).append(r["launch_overhead_ms"])
    lo = {v: round(statistics.median(xs), 1) for v, xs in launch.items()}
    dst = files[0].parent / "final_summary.json"
    dst.write_text(json.dumps({"rows": out, "marks_medium": mk, "launch_overhead_medium": lo}, ensure_ascii=False,
                              indent=1), encoding="utf-8")
    print(f"\nsaved {dst}")


if __name__ == "__main__":
    main()
