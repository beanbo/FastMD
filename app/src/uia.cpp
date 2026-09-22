// Screen readers (plan 6.1): a UI Automation provider over the document.
//
// Narrator and NVDA see the window as a Document with a Text pattern, read it, move by characters, words, lines and
// paragraphs, jump between headings (the Style attribute carries the level), follow the selection and highlight what
// they read. Everything is answered straight from the model - the flat text buffer and the block table - so there is
// no second copy of the document to keep in step.
//
// uiautomationcore.dll is delay-loaded and the first WM_GETOBJECT for the root object only arrives when a screen
// reader is actually running, so a reader who never uses one never pays for this file.
#include "app.h"

#include <uiautomation.h>

namespace {
IRawElementProviderSimple* g_root = nullptr;

uint32_t Clamp(uint32_t pos) { return std::min<uint32_t>(pos, (uint32_t)g.doc.text.size()); }

// the text of a range, with the stand-ins for pictures and formulas replaced by what they stand for
std::wstring TextOf(uint32_t from, uint32_t to, int maxLen) {
    std::wstring out;
    from = Clamp(from);
    to = Clamp(to);
    for (uint32_t i = from; i < to; i++) {
        wchar_t c = g.doc.text[i];
        if (c == L'\xFFFC') continue;
        out.push_back(c);
        if (maxLen >= 0 && (int)out.size() >= maxLen) break;
    }
    return out;
}

BSTR Bstr(const std::wstring& s) { return SysAllocStringLen(s.c_str(), (UINT)s.size()); }

// ------------------------------------------------------------------------------------------------ text range
struct Range final : ITextRangeProvider {
    LONG ref = 1;
    uint32_t from = 0, to = 0;

    Range(uint32_t a, uint32_t b) : from(std::min(a, b)), to(std::max(a, b)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ITextRangeProvider)) {
            *ppv = static_cast<ITextRangeProvider*>(this);
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

    // the boundaries of one unit around a position
    static bool UnitRange(TextUnit unit, uint32_t pos, uint32_t* a, uint32_t* b) {
        switch (unit) {
        case TextUnit_Character:
            *a = pos;
            *b = TextStep(pos, 1);
            return true;
        case TextUnit_Word: {
            // A screen reader's "word" carries the space after it, and a run of spaces belongs to the word before -
            // otherwise moving word by word reads out a blank between every pair of words.
            if (!WordRange(pos, a, b)) return false;
            const std::wstring& t = g.doc.text;
            if (*b > *a && iswspace(t[*a])) {  // landed inside the spaces: take the word that follows them
                uint32_t wa = 0, wb = 0;
                if (*b < t.size() && WordRange(*b, &wa, &wb)) *b = wb;
                return true;
            }
            uint32_t e = *b;
            while (e < t.size() && (t[e] == L' ' || t[e] == L'\t')) e++;
            *b = e;
            return true;
        }
        case TextUnit_Line: return LineRange(pos, a, b);
        case TextUnit_Paragraph:
        case TextUnit_Format: return ParagraphRange(pos, a, b);
        case TextUnit_Page:
        case TextUnit_Document:
        default:
            *a = 0;
            *b = (uint32_t)g.doc.text.size();
            return true;
        }
    }

    // one step of `unit` from pos; returns the new position, or pos itself when there is nowhere left to go
    static uint32_t Step(TextUnit unit, uint32_t pos, int dir) {
        if (unit == TextUnit_Character) return TextStep(pos, dir);
        uint32_t a = 0, b = 0;
        if (!UnitRange(unit, pos, &a, &b)) return pos;
        if (dir > 0) return b >= (uint32_t)g.doc.text.size() ? pos : b;  // the last unit has no next one
        if (a == 0) return pos;                                          // nor the first one a previous
        uint32_t pa = 0, pb = 0;
        return UnitRange(unit, TextStep(a, -1), &pa, &pb) ? pa : pos;
    }

    HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider** out) override {
        *out = new Range(from, to);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider* other, BOOL* same) override {
        auto* o = static_cast<Range*>(other);
        *same = o && o->from == from && o->to == to;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CompareEndpoints(TextPatternRangeEndpoint mine, ITextRangeProvider* other,
                                               TextPatternRangeEndpoint theirs, int* result) override {
        auto* o = static_cast<Range*>(other);
        if (!o) return E_INVALIDARG;
        uint32_t a = mine == TextPatternRangeEndpoint_Start ? from : to;
        uint32_t b = theirs == TextPatternRangeEndpoint_Start ? o->from : o->to;
        *result = a < b ? -1 : a > b ? 1 : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit unit) override {
        uint32_t a = 0, b = 0;
        if (UnitRange(unit, from, &a, &b)) {
            from = a;
            to = b;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID, VARIANT, BOOL, ITextRangeProvider** out) override {
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindText(BSTR text, BOOL backward, BOOL ignoreCase,
                                       ITextRangeProvider** out) override {
        *out = nullptr;
        if (!text) return E_INVALIDARG;
        std::wstring needle(text, SysStringLen(text)), hay = g.doc.text.substr(from, to - from);
        if (needle.empty()) return S_OK;
        if (ignoreCase) {
            needle = ToLower(needle);
            hay = ToLower(hay);
        }
        size_t at = backward ? hay.rfind(needle) : hay.find(needle);
        if (at == std::wstring::npos) return S_OK;
        *out = new Range(from + (uint32_t)at, from + (uint32_t)(at + needle.size()));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID id, VARIANT* value) override {
        VariantInit(value);
        if (id == UIA_StyleIdAttributeId) {  // how a screen reader finds headings
            int level = HeadingLevelAt(from);
            value->vt = VT_I4;
            value->lVal = level >= 1 && level <= 6 ? StyleId_Heading1 + (level - 1) : StyleId_Normal;
            return S_OK;
        }
        if (id == UIA_IsReadOnlyAttributeId) {
            value->vt = VT_BOOL;
            value->boolVal = VARIANT_TRUE;
            return S_OK;
        }
        value->vt = VT_UNKNOWN;
        return UiaGetReservedNotSupportedValue((IUnknown**)&value->punkVal);
    }
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY** out) override {
        std::vector<double> rects;
        RangeScreenRects(from, to, rects);
        *out = SafeArrayCreateVector(VT_R8, 0, (ULONG)rects.size());
        if (!*out) return E_OUTOFMEMORY;
        for (LONG i = 0; i < (LONG)rects.size(); i++) SafeArrayPutElement(*out, &i, &rects[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(IRawElementProviderSimple** out) override {
        *out = g_root;
        if (g_root) g_root->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetText(int maxLength, BSTR* out) override {
        *out = Bstr(TextOf(from, to, maxLength));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Move(TextUnit unit, int count, int* moved) override {
        *moved = 0;
        uint32_t at = from;
        for (int i = 0; i < std::abs(count); i++) {
            uint32_t next = Step(unit, at, count > 0 ? 1 : -1);
            if (next == at) break;
            at = next;
            (*moved) += count > 0 ? 1 : -1;
        }
        uint32_t a = 0, b = 0;
        if (UnitRange(unit, at, &a, &b)) {
            from = a;
            to = b;
        } else {
            from = to = at;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(TextPatternRangeEndpoint mine, ITextRangeProvider* other,
                                                  TextPatternRangeEndpoint theirs) override {
        auto* o = static_cast<Range*>(other);
        if (!o) return E_INVALIDARG;
        uint32_t v = theirs == TextPatternRangeEndpoint_Start ? o->from : o->to;
        if (mine == TextPatternRangeEndpoint_Start) from = std::min(v, to);
        else to = std::max(v, from);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(TextPatternRangeEndpoint endpoint, TextUnit unit, int count,
                                                 int* moved) override {
        *moved = 0;
        uint32_t at = endpoint == TextPatternRangeEndpoint_Start ? from : to;
        for (int i = 0; i < std::abs(count); i++) {
            uint32_t next = Step(unit, at, count > 0 ? 1 : -1);
            if (next == at) break;
            at = next;
            (*moved) += count > 0 ? 1 : -1;
        }
        if (endpoint == TextPatternRangeEndpoint_Start) {
            from = at;
            to = std::max(to, from);
        } else {
            to = at;
            from = std::min(from, to);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE ScrollIntoView(BOOL) override {
        RevealTextPos(from, false);
        Invalidate();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Select() override {
        g.selAnchor = from;
        g.selFocus = to;
        Invalidate();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY** out) override {
        *out = SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
        return S_OK;
    }
};

// ------------------------------------------------------------------------------------------- the document itself
struct DocumentProvider final : IRawElementProviderSimple, IRawElementProviderFragment, IRawElementProviderFragmentRoot,
                                ITextProvider {
    LONG ref = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IRawElementProviderSimple))
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment))
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot))
            *ppv = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (riid == __uuidof(ITextProvider))
            *ppv = static_cast<ITextProvider*>(this);
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

    // ---- IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
        // no UseComThreading: the window thread has no COM apartment until after the first frame (it costs 10-20 ms
        // at start-up), and UI Automation is happy to call a plain server-side provider on its own thread
        *out = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** out) override {
        *out = nullptr;
        if (id == UIA_TextPatternId) {
            *out = static_cast<ITextProvider*>(this);
            AddRef();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* value) override {
        VariantInit(value);
        switch (id) {
        case UIA_ControlTypePropertyId:
            value->vt = VT_I4;
            value->lVal = UIA_DocumentControlTypeId;
            return S_OK;
        case UIA_NamePropertyId:
            value->vt = VT_BSTR;
            value->bstrVal = Bstr(g.path.empty() ? L"FastMD" : FileNameOf(g.path));
            return S_OK;
        case UIA_IsContentElementPropertyId:
        case UIA_IsControlElementPropertyId:
        case UIA_IsKeyboardFocusablePropertyId:
        case UIA_HasKeyboardFocusPropertyId:
            value->vt = VT_BOOL;
            value->boolVal = (id != UIA_HasKeyboardFocusPropertyId || ::GetFocus() == g.hwnd) ? VARIANT_TRUE
                                                                                            : VARIANT_FALSE;
            return S_OK;
        case UIA_IsEnabledPropertyId:
            value->vt = VT_BOOL;
            value->boolVal = VARIANT_TRUE;
            return S_OK;
        case UIA_AutomationIdPropertyId:
            value->vt = VT_BSTR;
            value->bstrVal = Bstr(L"FastMDDocument");
            return S_OK;
        case UIA_LocalizedControlTypePropertyId:
            value->vt = VT_BSTR;
            value->bstrVal = Bstr(UiLanguage() == UL_RU ? L"документ" : L"document");
            return S_OK;
        }
        return S_OK;  // VT_EMPTY: the framework falls back to the window's own value
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        return UiaHostProviderFromHwnd(g.hwnd, out);
    }

    // ---- IRawElementProviderFragment: one element, no children
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection, IRawElementProviderFragment** out) override {
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
        int id[] = {UiaAppendRuntimeId, 1};
        *out = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!*out) return E_OUTOFMEMORY;
        for (LONG i = 0; i < 2; i++) SafeArrayPutElement(*out, &i, &id[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
        RECT r{};
        GetWindowRect(g.hwnd, &r);
        *out = UiaRect{(double)r.left, (double)r.top, (double)(r.right - r.left), (double)(r.bottom - r.top)};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        ::SetFocus(g.hwnd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        AddRef();
        return S_OK;
    }

    // ---- IRawElementProviderFragmentRoot
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double, double, IRawElementProviderFragment** out) override {
        *out = static_cast<IRawElementProviderFragment*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
        *out = nullptr;
        return S_OK;
    }

    // ---- ITextProvider
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** out) override {
        *out = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
        if (!*out) return E_OUTOFMEMORY;
        ITextRangeProvider* r = new Range(std::min(g.selAnchor, g.selFocus), std::max(g.selAnchor, g.selFocus));
        LONG i = 0;
        SafeArrayPutElement(*out, &i, r);
        r->Release();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVisibleRanges(SAFEARRAY** out) override {
        uint32_t a = 0, b = (uint32_t)g.doc.text.size();
        if (!g.doc.blocks.empty()) {
            uint32_t first = FirstVisible(g.scrollY);
            a = g.doc.blocks[std::min<size_t>(first, g.doc.blocks.size() - 1)].textOff;
            uint32_t last = FirstVisible(g.scrollY + ViewH());
            const Block& lb = g.doc.blocks[std::min<size_t>(last, g.doc.blocks.size() - 1)];
            b = lb.textOff + lb.textLen;
        }
        *out = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
        if (!*out) return E_OUTOFMEMORY;
        ITextRangeProvider* r = new Range(a, b);
        LONG i = 0;
        SafeArrayPutElement(*out, &i, r);
        r->Release();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RangeFromChild(IRawElementProviderSimple*, ITextRangeProvider** out) override {
        *out = new Range(0, (uint32_t)g.doc.text.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RangeFromPoint(UiaPoint point, ITextRangeProvider** out) override {
        POINT p{(LONG)point.x, (LONG)point.y};
        ScreenToClient(g.hwnd, &p);
        uint32_t pos = 0;
        float s = Scale();
        HitTestDoc(p.x / s, p.y / s, &pos, nullptr);
        *out = new Range(pos, pos);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_DocumentRange(ITextRangeProvider** out) override {
        *out = new Range(0, (uint32_t)g.doc.text.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SupportedTextSelection(SupportedTextSelection* out) override {
        *out = SupportedTextSelection_Single;
        return S_OK;
    }
};
}  // namespace

// WM_GETOBJECT: a screen reader is asking for the document. This is the first time anything in this file runs, and
// the first time uiautomationcore.dll is loaded.
LRESULT UiaHandleGetObject(WPARAM wp, LPARAM lp) {
    if ((DWORD)lp != (DWORD)UiaRootObjectId || !g.hwnd) return 0;
    if (!g_root) g_root = new DocumentProvider();
    return UiaReturnRawElementProvider(g.hwnd, wp, lp, g_root);
}

// the document changed under the reader: tell whoever is listening to read it again
void UiaDocumentChanged() {
    if (g_root && UiaClientsAreListening()) UiaRaiseAutomationEvent(g_root, UIA_Text_TextChangedEventId);
}

// the selection moved (mouse, keyboard, find): a screen reader follows it
void UiaSelectionChanged() {
    if (g_root && UiaClientsAreListening()) UiaRaiseAutomationEvent(g_root, UIA_Text_TextSelectionChangedEventId);
}

void UiaShutdown() {
    if (!g_root) return;
    UiaDisconnectProvider(g_root);
    g_root->Release();
    g_root = nullptr;
}
