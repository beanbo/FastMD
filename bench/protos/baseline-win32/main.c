/* Calibration floor: the cheapest possible "window with text" — plain Win32 + GDI, no markdown.
 * Anything real can only be slower than this. Also the reference implementation of PROTOCOL.md for C. */
#include "../../common/fastmd_bench.h"
#include <shellapi.h>

static wchar_t g_title[512] = L"FastMD baseline";

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(WHITE_BRUSH));
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.left += 32; rc.top += 24;
        DrawTextW(dc, g_title, -1, &rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        EndPaint(hwnd, &ps);
        fmb_window_shown();
        if (fmb_content_presented_ex("GDI floor, no markdown")) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd;
    fmb_init();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        const wchar_t *name = wcsrchr(argv[1], L'\\');
        swprintf_s(g_title, 512, L"%s — FastMD (baseline-win32)", name ? name + 1 : argv[1]);
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"FastMDBaseline";
    RegisterClassW(&wc);

    UINT dpi = GetDpiForSystem();
    RECT r = {0, 0, MulDiv(1000, dpi, 96), MulDiv(800, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, g_title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
