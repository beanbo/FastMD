// Bench protocol hooks, trace log, string and file helpers.
#include "doc.h"
#include "bench_protocol.h"
#include <cstdarg>
#include <cstdio>

// ------------------------------------------------------------------------------------------------ bench
static SRWLOCK g_markLock = SRWLOCK_INIT;

uint64_t NowTicks() { return fmb_now(); }
void BenchInit() { fmb_init(); }
bool BenchActive() { return fmb_is_bench() != 0; }
void BenchWindowShown() { fmb_window_shown(); }
bool BenchContentPresented(const char* notes) { return fmb_content_presented_ex(notes) != 0; }

void MarkAt(const char* name, uint64_t t) {
    DebugLog("mark %s", name);
    if (!g_fmb.active) return;
    AcquireSRWLockExclusive(&g_markLock);
    if (g_fmb.mark_count < FMB_MAX_MARKS) {
        strncpy_s(g_fmb.mark_names[g_fmb.mark_count], 32, name, _TRUNCATE);
        g_fmb.mark_ticks[g_fmb.mark_count++] = t;
    }
    ReleaseSRWLockExclusive(&g_markLock);
}
void Mark(const char* name) { MarkAt(name, fmb_now()); }

// ------------------------------------------------------------------------------------------------ trace
namespace {
struct TraceEv { uint64_t t; DWORD tid; char what[120]; };
TraceEv g_ev[1024];
volatile LONG g_evN = 0;
int g_trace = -1;  // -1 = not checked yet
DWORD g_mainTid = 0;
}  // namespace

void DebugLog(const char* fmt, ...) {
    if (g_trace < 0) {
        wchar_t v[8] = {};
        g_trace = GetEnvironmentVariableW(L"FASTMD_TRACE", v, 8) > 0 && v[0] == L'1';
        g_mainTid = GetCurrentThreadId();
    }
    if (!g_trace) return;
    LONG i = InterlockedIncrement(&g_evN) - 1;
    if (i >= 1024) return;
    g_ev[i].t = fmb_now();
    g_ev[i].tid = GetCurrentThreadId();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ev[i].what, sizeof(g_ev[i].what), fmt, ap);
    va_end(ap);
}

void DebugFlush() {
    if (g_trace != 1) return;
    wchar_t p[MAX_PATH];
    GetTempPathW(MAX_PATH, p);
    wcscat_s(p, L"fastmd-trace.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, p, L"wb") || !f) return;
    FILETIME ct, et, kt, ut;
    GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut);
    uint64_t t0 = ((uint64_t)ct.dwHighDateTime << 32) | ct.dwLowDateTime;
    LONG n = g_evN < 1024 ? g_evN : 1024;
    for (LONG i = 0; i < n; i++)
        fprintf(f, "%8.2f  %s  %s\n", (g_ev[i].t - t0) / 10000.0, g_ev[i].tid == g_mainTid ? "UI" : "bg", g_ev[i].what);
    fclose(f);
}

// ------------------------------------------------------------------------------------------------ strings
std::wstring ToLower(const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring out(s.size(), L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), (int)s.size(), out.data(), (int)out.size(),
                          nullptr, nullptr, 0);
    if (n <= 0) return s;
    out.resize(n);
    return out;
}

bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring DirOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
}

// ------------------------------------------------------------------------------------------------ files
bool ReadFileUtf16(const wchar_t* path, std::wstring& out, uint64_t* ticksRead, FILETIME* writeTime) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    if (writeTime) GetFileTime(f, nullptr, nullptr, writeTime);
    if (sz.QuadPart > (1ll << 30)) { CloseHandle(f); return false; }
    DWORD n = (DWORD)sz.QuadPart, got = 0;
    char* buf = (char*)VirtualAlloc(nullptr, n + 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BOOL ok = buf && ReadFile(f, buf, n, &got, nullptr);
    CloseHandle(f);
    if (!ok) { if (buf) VirtualFree(buf, 0, MEM_RELEASE); return false; }
    if (ticksRead) *ticksRead = NowTicks();
    const char* p = buf;
    int len = (int)got;
    if (len >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) { p += 3; len -= 3; }
    if (len >= 2 && (uint8_t)p[0] == 0xFF && (uint8_t)p[1] == 0xFE) {  // UTF-16 LE file
        out.assign((const wchar_t*)(p + 2), (len - 2) / 2);
    } else {
        int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0);
        UINT cp = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (wn == 0 && len > 0) { cp = CP_ACP; flags = 0; wn = MultiByteToWideChar(cp, 0, p, len, nullptr, 0); }
        out.resize(wn);
        out.resize(MultiByteToWideChar(cp, flags, p, len, out.data(), wn));
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    return true;
}

bool GetFileStamp(const wchar_t* path, FILETIME* writeTime, uint64_t* size) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return false;
    if (writeTime) *writeTime = a.ftLastWriteTime;
    if (size) *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return true;
}
