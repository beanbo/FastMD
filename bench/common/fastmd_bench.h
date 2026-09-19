/* FastMD bench protocol v1 helper for C / C++ prototypes (header-only).
 * Usage:
 *   fmb_init();                         // first line of main/wWinMain (records "main" mark)
 *   fmb_mark("parsed");                 // optional breakdown marks
 *   fmb_window_shown();                 // after the first paint of anything (optional)
 *   if (fmb_content_presented()) { ... // after EndDraw/Present of the document frame;
 *       PostQuitMessage(0); }           // calls DwmFlush, writes the result; returns 1 if the app should exit
 * Link: dwmapi.lib (and psapi is in kernel32 as K32GetProcessMemoryInfo).
 */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "dwmapi.lib")

#define FMB_MAX_MARKS 32

typedef struct {
    int active;
    int exit_after;
    int done;
    wchar_t out_path[MAX_PATH * 2];
    unsigned long long t_window;
    int mark_count;
    char mark_names[FMB_MAX_MARKS][32];
    unsigned long long mark_ticks[FMB_MAX_MARKS];
} fmb_state_t;

static fmb_state_t g_fmb;

static __inline unsigned long long fmb_now(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static __inline void fmb_mark(const char *name) {
    if (!g_fmb.active || g_fmb.mark_count >= FMB_MAX_MARKS) return;
    strncpy_s(g_fmb.mark_names[g_fmb.mark_count], 32, name, _TRUNCATE);
    g_fmb.mark_ticks[g_fmb.mark_count++] = fmb_now();
}

static __inline void fmb_init(void) {
    DWORD n = GetEnvironmentVariableW(L"FASTMD_BENCH_OUT", g_fmb.out_path, MAX_PATH * 2);
    g_fmb.active = (n > 0 && n < MAX_PATH * 2);
    if (!g_fmb.active) return;
    wchar_t buf[8] = {0};
    DWORD m = GetEnvironmentVariableW(L"FASTMD_BENCH_EXIT", buf, 8);
    g_fmb.exit_after = !(m > 0 && buf[0] == L'0');
    fmb_mark("main");
}

static __inline int fmb_is_bench(void) { return g_fmb.active; }

static __inline void fmb_window_shown(void) {
    if (g_fmb.active && !g_fmb.t_window) g_fmb.t_window = fmb_now();
}

/* Returns 1 if the app should now exit (bench mode, exit requested). Idempotent. */
static __inline int fmb_content_presented_ex(const char *notes) {
    if (!g_fmb.active || g_fmb.done) return 0;
    DwmFlush();
    unsigned long long t_content = fmb_now();
    g_fmb.done = 1;

    PROCESS_MEMORY_COUNTERS pmc;
    unsigned long long ws = 0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) ws = pmc.WorkingSetSize;

    wchar_t tmp[MAX_PATH * 2 + 8];
    swprintf_s(tmp, MAX_PATH * 2 + 8, L"%s.tmp", g_fmb.out_path);
    FILE *f = NULL;
    if (_wfopen_s(&f, tmp, L"wb") == 0 && f) {
        fprintf(f, "{\"protocol\":1,\"t_content\":%llu,", t_content);
        if (g_fmb.t_window) fprintf(f, "\"t_window\":%llu,", g_fmb.t_window);
        else fprintf(f, "\"t_window\":null,");
        fprintf(f, "\"ws_bytes\":%llu,\"marks\":{", ws);
        for (int i = 0; i < g_fmb.mark_count; i++)
            fprintf(f, "%s\"%s\":%llu", i ? "," : "", g_fmb.mark_names[i], g_fmb.mark_ticks[i]);
        fprintf(f, "},\"notes\":\"%s\"}", notes ? notes : "");
        fclose(f);
        MoveFileExW(tmp, g_fmb.out_path, MOVEFILE_REPLACE_EXISTING);
    }
    return g_fmb.exit_after;
}

static __inline int fmb_content_presented(void) { return fmb_content_presented_ex(""); }
