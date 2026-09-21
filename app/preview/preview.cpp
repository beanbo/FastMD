// Explorer's preview pane, on the same engine as the reader (plan 5.2).
//
// An in-process COM server: Explorer hosts it in prevhost.exe, hands it a file and a window, and we draw the document
// into that window with exactly the code the reader uses - the same parser, the same layout, the same CPU canvas.
// Read-only and offline: no network (pictures from the web are not fetched here), no editing, no settings written.
//
// Registration is per-user, in HKCU, and is done by the reader itself (FastMD.exe --register-preview / --unregister-
// preview), so there is no regsvr32 and no administrator anywhere in the story.
#include "app.h"

#include <shlobj.h>
#include <thumbcache.h>
#include <shlwapi.h>

#include <new>

// {6F1A2C34-9B84-4B0E-9F6D-1C2E0A7B5D41} - FastMD preview handler
extern "C" const CLSID CLSID_FastMdPreview = {
    0x6f1a2c34, 0x9b84, 0x4b0e, {0x9f, 0x6d, 0x1c, 0x2e, 0x0a, 0x7b, 0x5d, 0x41}};
// {6F1A2C35-...} - FastMD thumbnail provider (plan 5.3): a class of its own, because the shell loads thumbnails
// somewhere else than preview panes
extern "C" const CLSID CLSID_FastMdThumb = {
    0x6f1a2c35, 0x9b84, 0x4b0e, {0x9f, 0x6d, 0x1c, 0x2e, 0x0a, 0x7b, 0x5d, 0x41}};

namespace {
HMODULE g_module = nullptr;
LONG g_objects = 0, g_locks = 0;
const wchar_t* kWndClass = L"FastMDPreviewView";
const wchar_t* kClsidText = L"{6F1A2C34-9B84-4B0E-9F6D-1C2E0A7B5D41}";
const wchar_t* kThumbClsidText = L"{6F1A2C35-9B84-4B0E-9F6D-1C2E0A7B5D41}";

std::wstring GuidText() { return kClsidText; }
std::wstring ThumbGuidText() { return kThumbClsidText; }

// ---------------------------------------------------------------------------------------- the drawing window
// The engine keeps one document in the global App state, which is exactly what a preview needs: one file at a time.
// Everything here runs on the thread Explorer calls us on.
struct View {
    HWND hwnd = nullptr;
    std::wstring path;
    bool loaded = false;
};
View* g_view = nullptr;

void Redraw() {
    if (!g_view || !g_view->hwnd) return;
    InvalidateRect(g_view->hwnd, nullptr, FALSE);
}

void LayoutForSize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    g.pxW = w;
    g.pxH = h;
    if (!g.canvas) {
        g.canvas = CreateGdiCanvas(g.dwf, w, h, Scale());
    } else {
        g.canvas->SetScale(Scale());  // a thumbnail draws the same layout at a much smaller scale than a pane
        g.canvas->Resize(w, h);
    }
    UpdateColumns();
    InitGeometry();
    RecomputeY();
}

void LoadDocument(const std::wstring& path, int w, int h) {
    g.path = path;
    g.doc = Doc();
    g.doc.baseDir = DirOf(path);
    g.src.clear();
    uint64_t t = 0;
    if (!ReadFileUtf16(path.c_str(), g.src, &t, &g.fileTime)) g.src = L"";
    ParseMarkdown(g.doc, g.src.data(), g.src.size());
    g.docSerial++;
    if (!g.dwf) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), (IUnknown**)&g.dwf);
        g.typo.Init(g.dwf);
    }
    g.scrollY = g.targetY = 0;
    LayoutForSize(w, h);
    InitialLayout();
    g.ready = true;
    g.firstFrame = false;
}

LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g.canvas && g.ready) {
            Render();
            BitBlt(hdc, 0, 0, g.pxW, g.pxH, g.canvas->DC(), 0, g.canvas->ViewportTop(), SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (g.ready) {
            LayoutForSize(LOWORD(lp), HIWORD(lp));
            g.offscreenValid = false;
            ForceFullRedraw();
            Redraw();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        float step = 3.f * 40.f * GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        g.scrollY = std::clamp(g.scrollY - step, 0.f, MaxScroll());
        g.targetY = g.scrollY;
        Redraw();
        return 0;
    }
    case WM_KEYDOWN: {
        float page = ViewH() - 56.f;
        float d = wp == VK_NEXT ? page : wp == VK_PRIOR ? -page : wp == VK_DOWN ? 56.f : wp == VK_UP ? -56.f : 0.f;
        if (wp == VK_HOME) g.scrollY = 0;
        else if (wp == VK_END) g.scrollY = MaxScroll();
        else if (d != 0) g.scrollY = std::clamp(g.scrollY + d, 0.f, MaxScroll());
        else return 0;
        g.targetY = g.scrollY;
        Redraw();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterViewClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ViewProc;
    wc.hInstance = g_module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);
}

// ---------------------------------------------------------------------------------------- the handler object
struct Handler final : IPreviewHandler, IInitializeWithFile, IObjectWithSite, IOleWindow, IPreviewHandlerVisuals {
    LONG ref = 1;
    HWND parent = nullptr;
    RECT rect{};
    IUnknown* site = nullptr;
    std::wstring file;
    View view;

    Handler() { InterlockedIncrement(&g_objects); }
    ~Handler() {
        Unload();
        if (site) site->Release();
        InterlockedDecrement(&g_objects);
    }

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPreviewHandler) *ppv = static_cast<IPreviewHandler*>(this);
        else if (riid == IID_IInitializeWithFile) *ppv = static_cast<IInitializeWithFile*>(this);
        else if (riid == IID_IObjectWithSite) *ppv = static_cast<IObjectWithSite*>(this);
        else if (riid == IID_IOleWindow) *ppv = static_cast<IOleWindow*>(this);
        else if (riid == IID_IPreviewHandlerVisuals) *ppv = static_cast<IPreviewHandlerVisuals*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }

    // ---- IInitializeWithFile: Explorer hands us the path, and nothing is read until DoPreview
    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR path, DWORD) override {
        if (!path) return E_INVALIDARG;
        file = path;
        return S_OK;
    }

    // ---- IPreviewHandler
    HRESULT STDMETHODCALLTYPE SetWindow(HWND hwnd, const RECT* r) override {
        parent = hwnd;
        if (r) rect = *r;
        if (view.hwnd) {
            SetParent(view.hwnd, parent);
            MoveWindow(view.hwnd, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRect(const RECT* r) override {
        if (!r) return E_INVALIDARG;
        rect = *r;
        if (view.hwnd)
            MoveWindow(view.hwnd, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DoPreview() override {
        if (file.empty() || !parent) return E_FAIL;
        RegisterViewClass();
        int w = rect.right - rect.left, h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0) { w = 600; h = 800; }
        if (!view.hwnd) {
            view.hwnd = CreateWindowExW(0, kWndClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, rect.left,
                                        rect.top, w, h, parent, nullptr, g_module, nullptr);
            if (!view.hwnd) return E_FAIL;
        }
        g_view = &view;
        g.hwnd = nullptr;  // the engine must not post to a window of its own here
        g.dpi = (float)GetDpiForWindow(view.hwnd);
        if (g.dpi < 72.f) g.dpi = 96.f;
        SetDarkPalette(SystemPrefersDark());
        LoadDocument(file, w, h);
        view.loaded = true;
        InvalidateRect(view.hwnd, nullptr, TRUE);
        UpdateWindow(view.hwnd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unload() override {
        if (view.hwnd) {
            DestroyWindow(view.hwnd);
            view.hwnd = nullptr;
        }
        if (g_view == &view) g_view = nullptr;
        ClearLayoutCache();
        g.doc = Doc();
        g.src.clear();
        g.ready = false;
        file.clear();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        if (view.hwnd) ::SetFocus(view.hwnd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryFocus(HWND* out) override {
        if (!out) return E_INVALIDARG;
        *out = GetFocus();
        return *out ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG* msg) override {
        IPreviewHandlerFrame* frame = nullptr;
        HRESULT hr = S_FALSE;
        if (site && SUCCEEDED(site->QueryInterface(IID_PPV_ARGS(&frame)))) {
            hr = frame->TranslateAccelerator(msg);
            frame->Release();
        }
        return hr;
    }

    // ---- IObjectWithSite
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* s) override {
        if (site) site->Release();
        site = s;
        if (site) site->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppv) override {
        if (!site) { *ppv = nullptr; return E_FAIL; }
        return site->QueryInterface(riid, ppv);
    }

    // ---- IOleWindow
    HRESULT STDMETHODCALLTYPE GetWindow(HWND* out) override {
        if (!out) return E_INVALIDARG;
        *out = view.hwnd;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

    // ---- IPreviewHandlerVisuals: the pane's own colours are ignored on purpose - the document keeps the GitHub
    // palette it has in the reader, light or dark according to the system theme.
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(COLORREF) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFont(const LOGFONTW*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetTextColor(COLORREF) override { return S_OK; }
};

// ------------------------------------------------------------------------------------ thumbnails (plan 5.3)
// The shell asks for a picture whose larger side is cx; a page is portrait, so it comes back cx tall and narrower
// than that. The document is laid out at the width the reader uses and drawn at the scale that makes it fit, which
// keeps the proportions of a real page instead of shrinking a screenshot.
struct Thumb final : IThumbnailProvider, IInitializeWithFile {
    LONG ref = 1;
    std::wstring file;

    Thumb() { InterlockedIncrement(&g_objects); }
    ~Thumb() { InterlockedDecrement(&g_objects); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IThumbnailProvider) *ppv = static_cast<IThumbnailProvider*>(this);
        else if (riid == IID_IInitializeWithFile) *ppv = static_cast<IInitializeWithFile*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR path, DWORD) override {
        if (!path) return E_INVALIDARG;
        file = path;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* out, WTS_ALPHATYPE* alpha) override {
        if (!out || !alpha || file.empty()) return E_INVALIDARG;
        *out = nullptr;
        *alpha = WTSAT_RGB;
        int h = (int)std::clamp<UINT>(cx, 32, 1024), w = std::max(32, (int)std::lround(h / 1.414));
        const float kPageDip = 700.f;  // the document is laid out for this width, then drawn to fit the picture
        g.hwnd = nullptr;
        g.dpi = 96.f * w / kPageDip;
        g.cfg.zoom = 1.f;
        SetDarkPalette(false);  // a thumbnail is a sheet of paper: light, whatever the theme is
        LoadDocument(file, w, h);
        if (!g.canvas) return E_FAIL;
        Render();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) return E_OUTOFMEMORY;
        HDC mem = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, h, g.canvas->DC(), 0, g.canvas->ViewportTop(), SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        GdiFlush();
        for (int i = 0; i < w * h; i++) ((uint32_t*)bits)[i] |= 0xff000000;  // opaque: the shell reads the alpha
        ClearLayoutCache();
        g.doc = Doc();
        g.src.clear();
        g.ready = false;
        *out = bmp;
        return S_OK;
    }
};

struct Factory final : IClassFactory {
    const CLSID* clsid = &CLSID_FastMdPreview;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // a static object: it outlives every caller
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        IUnknown* obj = nullptr;
        if (*clsid == CLSID_FastMdThumb) obj = static_cast<IThumbnailProvider*>(new (std::nothrow) Thumb());
        else obj = static_cast<IPreviewHandler*>(new (std::nothrow) Handler());
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_locks);
        else InterlockedDecrement(&g_locks);
        return S_OK;
    }
};
Factory g_factory;
Factory g_thumbFactory;

bool SetKey(const wchar_t* path, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
}  // namespace

// ------------------------------------------------------------------------------------------ COM entry points
extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid == CLSID_FastMdPreview) return g_factory.QueryInterface(riid, ppv);
    if (clsid == CLSID_FastMdThumb) {
        g_thumbFactory.clsid = &CLSID_FastMdThumb;
        return g_thumbFactory.QueryInterface(riid, ppv);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE;
}

// Per-user registration: the CLSID, the .md association's preview handler, and the list Windows shows in its own
// settings. FastMD.exe --register-preview calls this; nothing here needs administrator rights.
extern "C" HRESULT __stdcall DllRegisterServer() {
    wchar_t dll[MAX_PATH];
    if (!GetModuleFileNameW(g_module, dll, MAX_PATH)) return E_FAIL;
    std::wstring clsid = L"Software\\Classes\\CLSID\\" + GuidText();
    bool ok = SetKey(clsid.c_str(), nullptr, L"FastMD Preview Handler");
    ok &= SetKey((clsid + L"\\InprocServer32").c_str(), nullptr, dll);
    ok &= SetKey((clsid + L"\\InprocServer32").c_str(), L"ThreadingModel", L"Apartment");
    ok &= SetKey(clsid.c_str(), L"AppID", L"{534A1E02-D58F-44f0-B58B-36CBED287C7C}");  // prevhost, as Windows wants
    for (const wchar_t* ext : {L".md", L".markdown", L".mdown", L".mkd", L".mdx"}) {
        std::wstring key = std::wstring(L"Software\\Classes\\") + ext + L"\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
        ok &= SetKey(key.c_str(), nullptr, GuidText());
    }
    ok &= SetKey(L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", GuidText().c_str(),
                 L"FastMD Preview Handler");
    // thumbnails (plan 5.3): a class of its own, with no AppID - the shell hosts it where it sees fit
    std::wstring thumb = L"Software\\Classes\\CLSID\\" + ThumbGuidText();
    ok &= SetKey(thumb.c_str(), nullptr, L"FastMD Thumbnail Provider");
    ok &= SetKey((thumb + L"\\InprocServer32").c_str(), nullptr, dll);
    ok &= SetKey((thumb + L"\\InprocServer32").c_str(), L"ThreadingModel", L"Apartment");
    for (const wchar_t* ext : {L".md", L".markdown", L".mdown", L".mkd", L".mdx"}) {
        std::wstring key = std::wstring(L"Software\\Classes\\") + ext +
                           L"\\shellex\\{e357fccd-a995-4576-b01f-234630154e96}";
        ok &= SetKey(key.c_str(), nullptr, ThumbGuidText());
    }
    return ok ? S_OK : E_FAIL;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    std::wstring clsid = L"Software\\Classes\\CLSID\\" + GuidText();
    RegDeleteTreeW(HKEY_CURRENT_USER, clsid.c_str());
    for (const wchar_t* ext : {L".md", L".markdown", L".mdown", L".mkd", L".mdx"}) {
        std::wstring key = std::wstring(L"Software\\Classes\\") + ext + L"\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    }
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers",
                       GuidText().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\CLSID\\" + ThumbGuidText()).c_str());
    for (const wchar_t* ext : {L".md", L".markdown", L".mdown", L".mkd", L".mdx"}) {
        std::wstring key = std::wstring(L"Software\\Classes\\") + ext +
                           L"\\shellex\\{e357fccd-a995-4576-b01f-234630154e96}";
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    }
    return S_OK;
}
