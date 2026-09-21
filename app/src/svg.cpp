// Loading fastmd-svg.dll on demand, and the few calls we need from it. Every function here is called from worker
// threads only; loading is guarded so two threads cannot race on it.
#include "svg.h"
#include "app.h"

namespace {
using MeasureFn = int(__cdecl*)(const void*, int, float*, float*);
using RenderFn = int(__cdecl*)(const void*, int, int, int, void*);
using AddFontFn = int(__cdecl*)(const char*, int, int, const wchar_t*);

SRWLOCK g_lock = SRWLOCK_INIT;
int g_state = 0;  // 0 = not tried, 1 = ready, 2 = missing
MeasureFn g_measure = nullptr;
RenderFn g_render = nullptr;

void RegisterFonts(AddFontFn add) {
    wchar_t dir[MAX_PATH];
    if (!GetWindowsDirectoryW(dir, MAX_PATH)) return;
    std::wstring fonts = std::wstring(dir) + L"\\Fonts\\";
    struct { const char* family; int bold; const wchar_t* file; } faces[] = {
        {"", 0, L"segoeui.ttf"},   {"", 1, L"segoeuib.ttf"},
        {"Verdana", 0, L"verdana.ttf"}, {"Verdana", 1, L"verdanab.ttf"},   // shields.io badges ask for Verdana
        {"Arial", 0, L"arial.ttf"}, {"Arial", 1, L"arialbd.ttf"},
        {"Segoe UI", 0, L"segoeui.ttf"}, {"Segoe UI", 1, L"segoeuib.ttf"},
        {"Courier New", 0, L"cour.ttf"},
    };
    for (const auto& f : faces) add(f.family, f.bold, 0, (fonts + f.file).c_str());
}

bool Ensure() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_state == 0) {
        g_state = 2;
        // beside our own binary: in the preview handler the process is prevhost.exe, the DLL is ours
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&Ensure, &self);
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            std::wstring dll(path, n);
            size_t slash = dll.find_last_of(L'\\');
            dll = (slash == std::wstring::npos ? L"" : dll.substr(0, slash + 1)) + L"fastmd-svg.dll";
            if (HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
                g_measure = (MeasureFn)GetProcAddress(h, "FastMdSvgMeasure");
                g_render = (RenderFn)GetProcAddress(h, "FastMdSvgRender");
                auto add = (AddFontFn)GetProcAddress(h, "FastMdSvgAddFont");
                if (g_measure && g_render) {
                    if (add) RegisterFonts(add);
                    g_state = 1;
                }
            }
        }
    }
    bool ok = g_state == 1;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}
}  // namespace

bool IsSvgData(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n && i < 512; i++) {  // "<svg" within the first bytes, past any xml declaration or BOM
        if (data[i] == '<' && i + 4 <= n && memcmp(data + i, "<svg", 4) == 0) return true;
        if (data[i] == '<' && i + 5 <= n && memcmp(data + i, "<?xml", 5) != 0 && memcmp(data + i, "<!--", 4) != 0 &&
            memcmp(data + i, "<!DOC", 5) != 0)
            return false;
    }
    return false;
}

bool SvgMeasure(const uint8_t* data, size_t n, float* w, float* h) {
    return Ensure() && g_measure(data, (int)n, w, h) != 0;
}

bool SvgRender(const uint8_t* data, size_t n, int w, int h, std::vector<uint32_t>& out) {
    if (!Ensure() || w <= 0 || h <= 0 || (int64_t)w * h > (64 << 20)) return false;
    out.assign((size_t)w * h, 0u);
    return g_render(data, (int)n, w, h, out.data()) != 0;
}
