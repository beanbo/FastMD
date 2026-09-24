// FastMD — shared includes and small utilities.
// Architecture: one document window per process (every double-click = its own window, like Notepad).
// Start-up path (measured, see REPORT.md): CPU-only first frame (DirectWrite → GDI DIB), no GPU/COM/WinRT
// before the first frame, viewport-first layout, heavy work on worker threads.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite_3.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

template <class T> static inline void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// ------------------------------------------------------------------------------------------------ timing / bench
// Bench protocol (bench/PROTOCOL.md): active only when FASTMD_BENCH_OUT is set. Marks are thread-safe.
uint64_t NowTicks();                       // UTC FILETIME ticks (GetSystemTimePreciseAsFileTime)
void BenchInit();                          // first line of wWinMain
bool BenchActive();
void Mark(const char* name);
void MarkAt(const char* name, uint64_t ticks);
void BenchWindowShown();
bool BenchContentPresented(const char* notes);  // DwmFlush + write result; true = the app should exit now
void DebugLog(const char* fmt, ...);            // FASTMD_TRACE=1 → %TEMP%\fastmd-trace.txt
void DebugFlush();

// ------------------------------------------------------------------------------------------------ strings
std::wstring ToLower(const std::wstring& s);  // Unicode-aware (LCMapStringEx, invariant)
bool EndsWithI(const std::wstring& s, const wchar_t* suffix);
bool StartsWithI(const std::wstring& s, const wchar_t* prefix);
std::wstring FileNameOf(const std::wstring& path);
std::wstring DirOf(const std::wstring& path);  // with trailing backslash
// a path on another machine - UNC, or a drive letter mapped to a share - where a file call can block for a timeout
bool IsNetworkPath(const std::wstring& path);
