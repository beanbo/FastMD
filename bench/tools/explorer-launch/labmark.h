/* FastMD startup lab: child-side timestamps through a named shared-memory block.
 * Uses kernel32 only (works in /NODEFAULTLIB builds). The launcher creates the block and the "done" event,
 * resets them before every run, takes t0 right before CreateProcessW/ShellExecuteExW, then reads the marks.
 *   lab_init()            first line of the entry point, records "main"
 *   lab_mark("name")      any breakdown mark (<= 31 chars)
 *   lab_note("text")      append free text (e.g. a module list)
 *   lab_done()            signal the launcher (it stops waiting)
 */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

#define LAB_SHM_NAME L"Local\\FastMDLabShm"
#define LAB_EVT_NAME L"Local\\FastMDLabDone"
#define LAB_MAX 256
#define LAB_NOTE 16384

typedef struct { char name[32]; unsigned long long t; } lab_mark_t;
typedef struct {
    volatile LONG count;
    DWORD pid;
    volatile LONG note_len;
    DWORD reserved;
    lab_mark_t m[LAB_MAX];
    char note[LAB_NOTE];
} lab_shm_t;

static lab_shm_t *g_lab;

static __inline unsigned long long lab_now(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static __inline void lab_mark_at(const char *name, unsigned long long t) {
    if (!g_lab) return;
    LONG i = InterlockedIncrement(&g_lab->count) - 1;
    if (i >= LAB_MAX) return;
    int k = 0;
    for (; name[k] && k < 31; k++) g_lab->m[i].name[k] = name[k];
    g_lab->m[i].name[k] = 0;
    g_lab->m[i].t = t;
}

static __inline void lab_mark(const char *name) { lab_mark_at(name, lab_now()); }

static __inline void lab_note(const char *s) {
    if (!g_lab) return;
    LONG n = g_lab->note_len;
    while (*s && n < LAB_NOTE - 1) g_lab->note[n++] = *s++;
    g_lab->note[n] = 0;
    g_lab->note_len = n;
}

/* Attach to the block; returns 1 if a launcher is listening. `t_main` = timestamp taken by the caller
 * as early as possible (pass 0 to take it here). */
static __inline int lab_init_at(unsigned long long t_main) {
    if (!t_main) t_main = lab_now();
    if (!g_lab) {
        HANDLE h = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, LAB_SHM_NAME);
        if (h) g_lab = (lab_shm_t *)MapViewOfFile(h, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(lab_shm_t));
    }
    if (!g_lab) return 0;
    g_lab->pid = GetCurrentProcessId();
    lab_mark_at("main", t_main);
    return 1;
}
static __inline int lab_init(void) { return lab_init_at(0); }

/* For long-lived (resident) processes: open-or-create the block (the launcher may start later) and keep it. */
static __inline int lab_attach(void) {
    if (g_lab) return 1;
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(lab_shm_t), LAB_SHM_NAME);
    if (h) g_lab = (lab_shm_t *)MapViewOfFile(h, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(lab_shm_t));
    return g_lab != NULL;
}

static __inline void lab_done(void) {
    if (GetModuleHandleW(L"windhawk.dll")) lab_note("[windhawk]");
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, LAB_EVT_NAME);
    if (e) { SetEvent(e); CloseHandle(e); }
}

/* Append the list of loaded modules to the note: "mods:a.dll,b.dll,..." (needs psapi via kernel32 K32*). */
static __inline void lab_note_modules(const char *prefix) {
    HMODULE mods[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    DWORD n = need / sizeof(HMODULE);
    if (n > 512) n = 512;
    lab_note(prefix);
    char nm[MAX_PATH];
    for (DWORD i = 0; i < n; i++) {
        if (K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], nm, MAX_PATH)) {
            lab_note(nm);
            lab_note(",");
        }
    }
    lab_note("\n");
}
