// What happens when something goes badly wrong (plan 6.4).
//
// An unhandled exception writes a minidump into %LOCALAPPDATA%\FastMD\crashes and nothing else: no network, no
// automatic sending, no dialog inside the broken process. The next start notices the file and offers to open the
// folder, once. Installing the handler is a single call with no DLL behind it, so the start-up path pays nothing;
// dbghelp.dll is loaded only at the moment of a crash, when speed no longer matters.
#include "app.h"

#include <dbghelp.h>
#include <shellapi.h>

namespace {
using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
wchar_t g_dir[MAX_PATH] = L"";  // %LOCALAPPDATA%\FastMD\crashes\, filled before the handler can ever run
LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;

void Stamp(wchar_t* out, size_t n) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf_s(out, n, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// Deliberately small and allocation-free: the heap may be the thing that broke.
LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0 && g_dir[0]) {
        CreateDirectoryW(g_dir, nullptr);
        wchar_t stamp[32], path[MAX_PATH];
        Stamp(stamp, std::size(stamp));
        swprintf_s(path, L"%sFastMD-%s-%lu.dmp", g_dir, stamp, GetCurrentProcessId());
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            if (HMODULE dbg = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
                if (auto write = (WriteDumpFn)GetProcAddress(dbg, "MiniDumpWriteDump")) {
                    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
                    // small dump: the stacks and enough memory around them to read a trace, no document text
                    auto type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                                MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
                    write(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep ? &mei : nullptr, nullptr, nullptr);
                }
            }
            CloseHandle(f);
        }
    }
    return g_prev ? g_prev(ep) : EXCEPTION_EXECUTE_HANDLER;
}

// the newest dump in the folder, and how many there are
bool NewestDump(std::wstring& path, uint64_t& written, int* count) {
    *count = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((std::wstring(g_dir) + L"*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    written = 0;
    do {
        (*count)++;
        uint64_t t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (t > written) {
            written = t;
            path = std::wstring(g_dir) + fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return written != 0;
}
}  // namespace

// wWinMain, before anything else: one call, no library behind it
void CrashHandlerInstall() {
    std::wstring dir = DataDir() + L"crashes\\";
    wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE);
    g_prev = SetUnhandledExceptionFilter(OnCrash);
}

// After the first frame: if the previous run left a dump, say so once and offer the folder. Old dumps are pruned so
// the folder cannot grow without end.
void CrashReportIfAny() {
    if (!g_dir[0]) return;
    std::wstring newest;
    uint64_t written = 0;
    int count = 0;
    if (!NewestDump(newest, written, &count)) return;
    if (count > 10) {  // keep the ten newest
        std::vector<std::pair<uint64_t, std::wstring>> all;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((std::wstring(g_dir) + L"*.dmp").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                uint64_t t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                all.emplace_back(t, std::wstring(g_dir) + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (size_t i = 10; i < all.size(); i++) DeleteFileW(all[i].second.c_str());
    }
    if (written <= LastCrashSeen()) return;  // already offered for this one
    SetLastCrashSeen(written);
    wchar_t msg[1024];
    swprintf_s(msg, Tr(S_CRASH_FOUND), newest.c_str());
    ModalScope modal;
    if (MessageBoxW(g.hwnd, msg, L"FastMD", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
        ShellExecuteW(g.hwnd, L"open", L"explorer.exe", (L"/select,\"" + newest + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
