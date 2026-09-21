// Dragging things out of the window (plan 3.5): the selection, a link, a picture — each in the formats the program
// on the other end expects. Text carries its formatting, a link is offered as a URL, a picture as a bitmap and, when
// it is a file on this disk, as that file.
//
// COM lives only here: OLE is initialised on the first drag, never on the start-up path (it costs 10-20 ms), and the
// data object is handed to the shell, which keeps it alive until the drop is done.
#include "app.h"

#include <shlobj.h>

namespace {
// a data object over a handful of HGLOBALs — everything a viewer ever needs to hand out
struct DataObject final : IDataObject {
    LONG ref = 1;
    std::vector<FORMATETC> fmt;
    std::vector<STGMEDIUM> med;

    ~DataObject() {
        for (STGMEDIUM& m : med) ReleaseStgMedium(&m);
    }
    void Add(UINT cf, HGLOBAL h) {
        if (!cf || !h) return;
        fmt.push_back(FORMATETC{(CLIPFORMAT)cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        STGMEDIUM m{};
        m.tymed = TYMED_HGLOBAL;
        m.hGlobal = h;
        med.push_back(m);
    }
    int Find(const FORMATETC* f) const {
        for (size_t i = 0; i < fmt.size(); i++)
            if (f && fmt[i].cfFormat == f->cfFormat && (f->tymed & TYMED_HGLOBAL) && f->dwAspect == DVASPECT_CONTENT)
                return (int)i;
        return -1;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* out) override {
        int i = Find(f);
        if (i < 0) return DV_E_FORMATETC;
        SIZE_T bytes = GlobalSize(med[i].hGlobal);
        HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, bytes);  // the receiver owns what it gets
        if (!copy) return E_OUTOFMEMORY;
        void* src = GlobalLock(med[i].hGlobal);
        void* dst = GlobalLock(copy);
        if (src && dst) memcpy(dst, src, bytes);
        GlobalUnlock(copy);
        GlobalUnlock(med[i].hGlobal);
        *out = STGMEDIUM{};
        out->tymed = TYMED_HGLOBAL;
        out->hGlobal = copy;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override { return Find(f) >= 0 ? S_OK : DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
        if (dir != DATADIR_GET || fmt.empty()) return E_NOTIMPL;
        return SHCreateStdEnumFmtEtc((UINT)fmt.size(), fmt.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

struct DropSource final : IDropSource {
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

HGLOBAL Bytes(const void* p, size_t n) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!h) return nullptr;
    void* dst = GlobalLock(h);
    if (!dst) { GlobalFree(h); return nullptr; }
    memcpy(dst, p, n);
    GlobalUnlock(h);
    return h;
}
HGLOBAL Wide(const std::wstring& s) { return Bytes(s.c_str(), (s.size() + 1) * sizeof(wchar_t)); }
HGLOBAL Narrow(const std::string& s) { return Bytes(s.c_str(), s.size() + 1); }

HGLOBAL DropFiles(const std::wstring& path) {  // CF_HDROP with one file: Explorer and most editors take this
    size_t chars = path.size() + 2;            // double-null terminated list
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return nullptr;
    auto* df = (DROPFILES*)GlobalLock(h);
    if (!df) { GlobalFree(h); return nullptr; }
    memset(df, 0, bytes);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy((char*)df + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(h);
    return h;
}

UINT CF(const wchar_t* name) {
    return RegisterClipboardFormatW(name);
}

// kind: DRAG_TEXT / DRAG_LINK / DRAG_IMAGE; arg = link index or image block
DataObject* Build(int kind, int arg) {
    auto* d = new DataObject();
    if (kind == DRAG_TEXT) {
        std::wstring text;
        std::string html, rtf;
        SelectionRichFormats(text, html, rtf);
        if (text.empty()) { d->Release(); return nullptr; }
        d->Add(CF_UNICODETEXT, Wide(text));
        d->Add(CF(L"HTML Format"), Narrow(html));
        d->Add(CF(L"Rich Text Format"), Narrow(rtf));
    } else if (kind == DRAG_LINK) {
        if (arg < 0 || (size_t)arg >= g.doc.links.size()) { d->Release(); return nullptr; }
        const std::wstring& url = g.doc.links[arg];
        d->Add(CF_UNICODETEXT, Wide(url));
        d->Add(CF(CFSTR_INETURLW), Wide(url));  // browsers and bookmark bars ask for this one
    } else if (kind == DRAG_IMAGE) {
        if (arg < 0 || (size_t)arg >= g.doc.blocks.size() || g.doc.blocks[arg].kind != BK_IMAGE) {
            d->Release();
            return nullptr;
        }
        d->Add(CF_DIB, ImageAsDib((uint32_t)arg));
        const Image& im = g.doc.images[g.doc.blocks[arg].aux];
        const Image& src = im.canon >= 0 ? g.doc.images[im.canon] : im;
        if (!src.path.empty()) d->Add(CF_HDROP, DropFiles(src.path));
    }
    if (d->fmt.empty()) { d->Release(); return nullptr; }
    return d;
}

bool g_oleReady = false;
}  // namespace

// What a drag of this kind would carry, as a bitmask (automation / UI tests): the drag loop itself is the shell's.
uint32_t DragFormats(int kind, int arg) {
    DataObject* d = Build(kind, arg);
    if (!d) return 0;
    uint32_t bits = 0;
    const UINT html = CF(L"HTML Format"), rtf = CF(L"Rich Text Format"), url = CF(CFSTR_INETURLW);
    for (const FORMATETC& f : d->fmt) {
        if (f.cfFormat == CF_UNICODETEXT) bits |= DF_TEXT;
        else if (f.cfFormat == html) bits |= DF_HTML;
        else if (f.cfFormat == rtf) bits |= DF_RTF;
        else if (f.cfFormat == url) bits |= DF_URL;
        else if (f.cfFormat == CF_DIB) bits |= DF_DIB;
        else if (f.cfFormat == CF_HDROP) bits |= DF_FILE;
    }
    d->Release();
    return bits;
}

// Runs the shell's drag loop. It ends when the button goes up or Esc is pressed; nothing is ever moved or deleted
// here, so the effect the target reports does not matter to us.
void StartDrag(int kind, int arg) {
    if (kind < 0) return;
    if (!g_oleReady) {  // once per process, and never before the first frame
        HRESULT hr = OleInitialize(nullptr);
        if (FAILED(hr)) return;  // the thread is already in another apartment: no drag
        g_oleReady = true;
    }
    DataObject* data = Build(kind, arg);
    if (!data) return;
    auto* src = new DropSource();
    DWORD effect = 0;
    DoDragDrop(data, src, DROPEFFECT_COPY | (kind == DRAG_LINK ? DROPEFFECT_LINK : 0), &effect);
    src->Release();
    data->Release();
    g.selecting = false;
    g.dragKind = -1;
    Invalidate();
}
