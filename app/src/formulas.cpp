// Loading fastmd-tex.dll and fastmd-mermaid.dll on demand, and the calls we need from them. Like the SVG library,
// they are opened from worker threads only, after the first frame, and each is opened once.
#include "formulas.h"

#include "app.h"

namespace {
using TexFn = int(__cdecl*)(const uint8_t*, size_t, int, float, uint32_t, uint8_t**, size_t*, float*, float*, float*);
using TexFreeFn = void(__cdecl*)(uint8_t*, size_t);
using MerFn = int(__cdecl*)(const uint8_t*, size_t, int, uint8_t**, size_t*);
using MerFreeFn = void(__cdecl*)(uint8_t*, size_t);

std::wstring BesideExe(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L'\\');
    return (slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1)) + name;
}

// the file is there: asked while the document is still being parsed, so it must not load anything
bool FileExists(const wchar_t* name) {
    std::wstring p = BesideExe(name);
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct Lib {
    const wchar_t* file;
    SRWLOCK lock = SRWLOCK_INIT;
    int state = 0;  // 0 = not tried, 1 = ready, 2 = missing
    FARPROC render = nullptr, free = nullptr;
    std::atomic<int> cached{-1};  // FileExists, remembered (the parser may ask from two threads)

    bool Present() {
        int c = cached.load(std::memory_order_relaxed);
        if (c < 0) {
            c = FileExists(file) ? 1 : 0;
            cached.store(c, std::memory_order_relaxed);
        }
        return c == 1;
    }
    bool Ensure(const char* renderName, const char* freeName) {
        AcquireSRWLockExclusive(&lock);
        if (state == 0) {
            state = 2;
            std::wstring path = BesideExe(file);
            if (!path.empty()) {
                if (HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
                    render = GetProcAddress(h, renderName);
                    free = GetProcAddress(h, freeName);
                    if (render && free) state = 1;
                }
            }
        }
        bool ok = state == 1;
        ReleaseSRWLockExclusive(&lock);
        return ok;
    }
};

Lib g_tex{L"fastmd-tex.dll"};
Lib g_mer{L"fastmd-mermaid.dll"};
}  // namespace

bool TexAvailable() { return g_tex.Present(); }
bool MermaidAvailable() { return g_mer.Present(); }

bool TexSvg(const std::string& tex, bool display, float fontPx, uint32_t rgb, std::vector<uint8_t>& svg, float* w,
            float* h, float* ascent) {
    if (tex.empty() || tex.size() > (1u << 20) || !g_tex.Ensure("fastmd_tex_svg", "fastmd_tex_free")) return false;
    uint8_t* out = nullptr;
    size_t len = 0;
    float ww = 0, hh = 0, asc = 0;
    if (!((TexFn)g_tex.render)((const uint8_t*)tex.data(), tex.size(), display ? 1 : 0, fontPx, rgb, &out, &len, &ww,
                               &hh, &asc) ||
        !out || !len)
        return false;
    svg.assign(out, out + len);
    ((TexFreeFn)g_tex.free)(out, len);
    if (w) *w = ww;
    if (h) *h = hh;
    if (ascent) *ascent = asc;
    return true;
}

bool MermaidSvg(const std::string& src, bool dark, std::vector<uint8_t>& svg) {
    if (src.empty() || src.size() > (1u << 20) ||
        !g_mer.Ensure("fastmd_mermaid_svg", "fastmd_mermaid_free"))
        return false;
    uint8_t* out = nullptr;
    size_t len = 0;
    if (!((MerFn)g_mer.render)((const uint8_t*)src.data(), src.size(), dark ? 1 : 0, &out, &len) || !out || !len)
        return false;
    svg.assign(out, out + len);
    ((MerFreeFn)g_mer.free)(out, len);
    return true;
}
