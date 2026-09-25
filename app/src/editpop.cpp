// Edit mode's source popups on the input thread, and the preview worker (docs/EDIT-MODE.md §9).
//
// A popup is a panel the canvas draws (editbar.cpp) with one or two real EDIT controls in it. The document window's UI
// thread runs without IME (a start-up measure), so the EDITs live on find's input thread, where IME, the caret, the
// selection and the clipboard work natively: each is a child of the document window inside a host of its own, which
// takes its notifications. The UI thread only posts to them (InputCall); they write their text into the popup buffer
// and post WM_APP_EDITINPUT back - never a SendMessage between the threads. Ctrl+Z / Ctrl+Y move through the popup's
// own history (EM_UNDO is never used: one level, and it knows nothing of what the document did).
//
// The preview worker renders the formula or the diagram a popup edits, at the screen's pixels, the latest job first:
// a keystroke's render replaces the one before it that has not begun. It loads the TeX and Mermaid libraries ahead of
// the first render when a popup or popover of theirs opens (R22) - a document that never touches them loads neither.
#include "app.h"
#include "formulas.h"
#include "svg.h"
#include <uxtheme.h>

// Nothing here runs more than once per key or message: nothing is inlined, for size (§1 principle 3; see editcore.cpp)
#pragma inline_depth(0)
namespace {
// ---- the popup buffer (§9.1): the fields' latest text, written by the input thread, read by the UI thread
struct Buffer { SRWLOCK lock = SRWLOCK_INIT; uint32_t seq = 0; std::wstring text[2]; HWND edit[2] = {}; };
Buffer g_buf;

// ---- the input thread's side
struct Snap { std::wstring text; DWORD s = 0, e = 0; };
const int kHist = 64;  // snapshots a field keeps (the oldest go); made with the field, not in the exe's data
struct Field { HWND host = nullptr, edit = nullptr; Snap* hist = nullptr; int n = 0, at = 0; DWORD last = 0; };
Field s_f[2];
int s_n = 0, s_tab = 0, s_fontPx = 13, s_lineH = 16;
bool s_multi = false, s_scroll = false, s_quiet = false, s_restoring = false;
HFONT s_font = nullptr;
HBRUSH s_brush = nullptr;
COLORREF s_fg = 0, s_bg = 0xFFFFFF;
WNDPROC s_editProc = nullptr;
const wchar_t kHost[] = L"FastMD.PopupField";

void PostEvent(WPARAM ev, LPARAM lp) { PostMessageW(g.hwnd, WM_APP_EDITINPUT, ev, lp); }

std::wstring TextOf(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring t(n + 1, L'\0');
    t.resize(GetWindowTextW(h, t.data(), n + 1));
    return t;
}
int FieldOf(HWND h) { return h && (h == s_f[1].edit || h == s_f[1].host) ? 1 : 0; }

// the field's text into the buffer (its lines "\n", as the core takes them), and the UI thread told
void Publish(int f) {
    std::wstring t = TextOf(s_f[f].edit), n;
    for (size_t i = 0; i < t.size(); i++)
        if (t[i] != L'\r') n += t[i];
        else if (i + 1 >= t.size() || t[i + 1] != L'\n') n += L'\n';
    AcquireSRWLockExclusive(&g_buf.lock);
    g_buf.text[f] = std::move(n);
    uint32_t seq = ++g_buf.seq;
    ReleaseSRWLockExclusive(&g_buf.lock);
    PostEvent(EI_TEXT, seq);
}

// The popup's history (UX-13): a snapshot of the text and the selection per burst of typing - a new one after a pause of
// 400 ms or where a word ended, else the latest one follows the typing. The first is the text the popup opened with.
void Remember(int f) {
    Field& x = s_f[f];
    Snap s;
    s.text = TextOf(x.edit);
    SendMessageW(x.edit, EM_GETSEL, (WPARAM)&s.s, (LPARAM)&s.e);
    const DWORD now = GetTickCount();
    const bool word = s.s > 0 && s.s <= s.text.size() && !iswalnum(s.text[s.s - 1]);
    if (x.at == 0 || now - x.last > 400 || word) {  // (what was undone goes)
        if (x.at + 1 == kHist) {
            for (int k = 1; k < kHist; k++) x.hist[k - 1] = std::move(x.hist[k]);
            x.at--;
        }
        x.at++;
    }
    x.hist[x.at] = std::move(s);
    x.n = x.at + 1;
    x.last = now;
}
void Restore(int f, int dir) {
    Field& x = s_f[f];
    if (dir < 0 ? x.at == 0 : x.at + 1 >= x.n) return;
    x.at += dir;
    s_restoring = true;  // (the text still goes to the document: it is a change like any other)
    SetWindowTextW(x.edit, x.hist[x.at].text.c_str());
    SendMessageW(x.edit, EM_SETSEL, x.hist[x.at].s, x.hist[x.at].e);
    SendMessageW(x.edit, EM_SCROLLCARET, 0, 0);
    s_restoring = false;
    x.last = 0;  // the next keystroke starts a snapshot of its own
}

void DeleteWordBack(HWND h) {
    DWORD s = 0, e = 0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (s == e) {
        std::wstring t = TextOf(h);
        DWORD k = std::min<DWORD>(s, (DWORD)t.size());
        while (k > 0 && iswspace(t[k - 1])) k--;
        while (k > 0 && !iswspace(t[k - 1])) k--;
        SendMessageW(h, EM_SETSEL, k, s);
    }
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"");
}

// The keys a popup does not type: Esc cancels, Ctrl+Enter (a single-line field: Enter) is done, Ctrl+S and Ctrl+W are
// the document's; Tab types blanks in a source and goes to the other field of a picture's popup (§9.1)
LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    const int f = FieldOf(h);
    switch (m) {
    case WM_KEYDOWN: {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        switch (wp) {
        case VK_ESCAPE: PostEvent(EI_KEY, VK_ESCAPE); return 0;
        case VK_RETURN:
            if (!ctrl && s_multi) break;
            PostEvent(EI_KEY, MAKELPARAM(VK_RETURN, ctrl ? KM_CTRL : 0));
            return 0;
        case VK_TAB:
            if (s_tab) SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)std::wstring(s_tab, L' ').c_str());
            else if (s_n > 1 && s_f[1 - f].edit) {
                SetFocus(s_f[1 - f].edit);
                SendMessageW(s_f[1 - f].edit, EM_SETSEL, 0, -1);
            }
            return 0;
        case 'A': if (ctrl) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; } break;
        case 'Z': if (ctrl) { Restore(f, shift ? 1 : -1); return 0; } break;
        case 'Y': if (ctrl) { Restore(f, 1); return 0; } break;
        case VK_BACK: if (ctrl) { DeleteWordBack(h); return 0; } break;
        case 'S': case 'W': if (ctrl) { PostEvent(EI_KEY, MAKELPARAM(wp, KM_CTRL)); return 0; } break;
        }
        break;
    }
    case WM_CHAR:  // what the keys above took, and control characters, would beep or type boxes
        if (wp == L'\t' || wp == 10 || wp == 0x7F || (wp == L'\r' && !s_multi) ||
            (wp < 32 && wp != 8 && wp != 3 && wp != 22 && wp != 24 && wp != L'\r'))
            return 0;
        break;
    case WM_SETFOCUS: PostEvent(EI_FOCUS, 1 + f); break;
    case WM_KILLFOCUS: PostEvent(EI_FOCUS, 0); break;
    }
    return CallWindowProcW(s_editProc, h, m, wp, lp);
}

// A source's scrollbar only while its lines do not fit (an EDIT's own is there, greyed, from the first line on)
void FitScroll(int f) {
    HWND e = s_f[f].edit;
    RECT rc;
    if (!s_scroll || !e || !GetClientRect(s_f[f].host, &rc)) return;
    const int lines = (int)SendMessageW(e, EM_GETLINECOUNT, 0, 0);
    ShowScrollBar(e, SB_VERT, lines * s_lineH > rc.bottom - rc.top);
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && !s_quiet) {
            const int f = FieldOf(h);
            if (!s_restoring) Remember(f);
            FitScroll(f);
            Publish(f);
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:  // (a read-only field)
        SetTextColor((HDC)wp, s_fg);
        SetBkColor((HDC)wp, s_bg);
        return (LRESULT)s_brush;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, s_brush);
        return 1;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

// the EDIT inside its host: a single-line one centred, a multi-line one filling it
void Place(int f, const RECT& rc, bool show) {
    Field& x = s_f[f];
    if (!x.host) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(x.host, HWND_TOP, rc.left, rc.top, w, h, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    int eh = s_multi ? h : std::min(h, MulDiv(s_fontPx, 145, 100));
    MoveWindow(x.edit, 0, (h - eh) / 2, w, eh, TRUE);
    FitScroll(f);
}

void Style(const PopupFields& p) {
    HFONT f = CreateFontW(-p.fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, p.face.c_str());
    if (s_brush) DeleteObject(s_brush);
    s_brush = CreateSolidBrush(p.bg);
    s_fg = p.fg;
    s_bg = p.bg;
    s_fontPx = p.fontPx;
    for (Field& x : s_f) {
        if (!x.edit) continue;
        if (f) SendMessageW(x.edit, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(x.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        // dark scrollbars in the dark theme (uxtheme is delay-loaded: only a popup ever needs it)
        SetWindowTheme(x.edit, p.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        RedrawWindow(x.host, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    if (f) {
        if (s_font) DeleteObject(s_font);
        s_font = f;
        if (HDC dc = GetDC(nullptr)) {  // (a line's height, for the scrollbar)
            HGDIOBJ old = SelectObject(dc, f);
            TEXTMETRICW tm;
            if (GetTextMetricsW(dc, &tm)) s_lineH = std::max(1, (int)tm.tmHeight);
            SelectObject(dc, old);
            ReleaseDC(nullptr, dc);
        }
    }
}

void Close() {
    for (Field& x : s_f) {
        if (x.host) DestroyWindow(x.host);
        delete[] x.hist;
        x = Field{};
    }
    AcquireSRWLockExclusive(&g_buf.lock);
    g_buf.edit[0] = g_buf.edit[1] = nullptr;
    ReleaseSRWLockExclusive(&g_buf.lock);
    s_n = 0;
}

void Open(const PopupFields& p) {
    Close();
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = HostProc;
        wc.hInstance = g.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        wc.lpszClassName = kHost;
        registered = RegisterClassExW(&wc) != 0;
    }
    s_n = p.n;
    s_multi = p.multi;
    s_scroll = p.scroll;
    s_tab = p.tab;
    s_quiet = true;
    for (int f = 0; f < p.n; f++) {
        Field& x = s_f[f];
        x.host = CreateWindowExW(WS_EX_NOPARENTNOTIFY, kHost, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, g.hwnd, nullptr,
                                 g.inst, nullptr);
        DWORD style = WS_CHILD | WS_VISIBLE | ES_NOHIDESEL |
                      (p.multi ? ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL : ES_AUTOHSCROLL) | (p.scroll ? WS_VSCROLL : 0) |
                      (f == 1 && p.fixed1 ? ES_READONLY : 0);
        x.edit = x.host ? CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"EDIT", L"", style, 0, 0, 10, 10, x.host, (HMENU)(INT_PTR)(1 + f),
                                          g.inst, nullptr)
                        : nullptr;
        if (!x.edit) continue;
        s_editProc = (WNDPROC)SetWindowLongPtrW(x.edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
        // no limit: an EDIT takes 30,000 characters by default - WM_SETTEXT ignores it, but typing and paste stop there,
        // and a big HTML block or SVG would lose the pasted rest in the document (Phase 4 notes)
        SendMessageW(x.edit, EM_SETLIMITTEXT, 0, 0);
        std::wstring t;  // an EDIT breaks lines at CRLF
        for (wchar_t ch : p.text[f]) t += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch);
        SetWindowTextW(x.edit, t.c_str());
        x.hist = new Snap[kHist];
        x.hist[0].text = TextOf(x.edit);
        x.at = 0;
        x.n = 1;
    }
    Style(p);
    for (int f = 0; f < p.n; f++) Place(f, p.rc[f], p.show);
    s_quiet = false;
    AcquireSRWLockExclusive(&g_buf.lock);
    for (int f = 0; f < 2; f++) {
        g_buf.edit[f] = s_f[f].edit;
        g_buf.text[f] = f < p.n ? p.text[f] : std::wstring();
    }
    ReleaseSRWLockExclusive(&g_buf.lock);
    if (s_f[0].edit) {
        SetFocus(s_f[0].edit);
        int n = p.selectAll ? 0 : GetWindowTextLengthW(s_f[0].edit);  // all selected, or the caret at the end
        // - or, a source taller than its box, at its start: at the end it would show only its last lines (Phase 4)
        RECT rc;
        if (!p.selectAll && s_multi && GetClientRect(s_f[0].host, &rc) &&
            (int)SendMessageW(s_f[0].edit, EM_GETLINECOUNT, 0, 0) * s_lineH > rc.bottom - rc.top)
            n = 0;
        SendMessageW(s_f[0].edit, EM_SETSEL, n, p.selectAll ? -1 : n);
        SendMessageW(s_f[0].edit, EM_SCROLLCARET, 0, 0);
    }
}
}  // namespace

// ------------------------------------------------------------------------------------------------ the UI thread's side
bool PopupFieldsOpen(const PopupFields& f) {
    auto* p = new PopupFields(f);
    if (InputCall([](void* a) {
            auto* p = (PopupFields*)a;
            s_fontPx = p->fontPx;
            Open(*p);
            delete p;
        }, p))
        return true;
    delete p;
    return false;
}

void PopupFieldsMove(const RECT rc[2], bool show) {
    struct M { RECT rc[2]; bool show; };
    auto* m = new M;
    m->rc[0] = rc[0];
    m->rc[1] = rc[1];
    m->show = show;
    if (!InputCall([](void* a) {
            auto* m = (M*)a;
            for (int f = 0; f < s_n; f++) Place(f, m->rc[f], m->show);
            delete m;
        }, m))
        delete m;
}

void PopupFieldsStyle(const PopupFields& f) {
    auto* p = new PopupFields(f);
    if (!InputCall([](void* a) {
            auto* p = (PopupFields*)a;
            Style(*p);
            delete p;
        }, p))
        delete p;
}

void PopupFieldsSetText(int field, const std::wstring& text) {
    struct T { int f; std::wstring t; };
    auto* t = new T{field, text};
    if (!InputCall([](void* a) {
            auto* t = (T*)a;
            if (t->f < s_n && s_f[t->f].edit) {
                SetWindowTextW(s_f[t->f].edit, t->t.c_str());  // (a change like typing: history, buffer, the document)
                SetFocus(s_f[t->f].edit);
                SendMessageW(s_f[t->f].edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
            }
            delete t;
        }, t))
        delete t;
}

void PopupFieldsClose() { InputCall([](void*) { Close(); }, nullptr); }

HWND PopupFieldHwnd(int field) {
    AcquireSRWLockShared(&g_buf.lock);
    HWND h = field >= 0 && field < 2 ? g_buf.edit[field] : nullptr;
    ReleaseSRWLockShared(&g_buf.lock);
    return h;
}

uint32_t PopupFieldText(int field, std::wstring* text) {
    AcquireSRWLockShared(&g_buf.lock);
    if (text && field >= 0 && field < 2) *text = g_buf.text[field];
    uint32_t seq = g_buf.seq;
    ReleaseSRWLockShared(&g_buf.lock);
    return seq;
}

// ------------------------------------------------------------------------------------------------ the preview worker (§9.4)
namespace {
struct Job { uint32_t seq = 0, ctx = 0, loadGen = 0; std::wstring key; uint8_t kind = 0; std::string src; float fontPx = 16,
             scale = 1; uint32_t rgb = 0; bool dark = false; std::string good; };
SRWLOCK g_pl = SRWLOCK_INIT;
Job g_job;
bool g_has = false;
int g_warm = 0, g_warmAsked = 0;
HANDLE g_pe = nullptr;

// A formula or a diagram at the screen's pixels: the layout keeps its size in DIP, the canvas draws the pixels 1:1. One
// that does not render brings the last source of the popup that did, drawn for the context of now (a theme switched
// meanwhile): the result stays a failure, its pixels are shown outlined.
void Render(const Job& j, RenderResult& r) {
    r.key = j.key;
    r.ctx = j.ctx;
    r.loadGen = j.loadGen;
    r.math = true;
    r.keep = true;
    auto pix = std::make_shared<Pixels>();
    if (!RenderMath(j.kind, j.src, j.fontPx, j.rgb, j.dark, j.scale, *pix, r)) {
        pix = std::make_shared<Pixels>();
        if (j.good.empty() || !RenderMath(j.kind, j.good, j.fontPx, j.rgb, j.dark, j.scale, *pix, r)) return;
        r.ok = false;
    }
    pix->serial = NewPixelSerial();
    r.pix = std::move(pix);
}

DWORD WINAPI PreviewThread(void*) {
    for (;;) {
        WaitForSingleObject(g_pe, INFINITE);
        for (;;) {
            if (g.closing) return 0;
            AcquireSRWLockExclusive(&g_pl);
            const int warm = g_warm;
            const bool has = g_has;
            Job j = has ? std::move(g_job) : Job{};
            g_warm = 0;
            g_has = false;
            ReleaseSRWLockExclusive(&g_pl);
            std::vector<uint8_t> svg;
            float w, h, a;
            if (warm & 1) TexSvg("x", false, 16.f, 0, svg, &w, &h, &a);
            if ((warm & 2) && MermaidSvg("sequenceDiagram\nA->>B: x", false, svg)) SvgMeasure(svg.data(), svg.size(), &w, &h);
            if (!has) break;
            g.rendersStarted++;
            if (DWORD ms = TestSlowMs().preview) Sleep(ms);
            auto* res = new PreviewResult;
            res->seq = j.seq;
            Render(j, res->r);
            if (res->r.ok) res->good = std::move(j.src);
            if (g.closing || !PostMessageW(g.hwnd, WM_APP_PREVIEW, 0, (LPARAM)res)) delete res;
        }
    }
}

void Wake() {
    if (!g_pe) {  // started with the first job or warm-up of the process, and never waited for (detached)
        g_pe = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE th = g_pe ? CreateThread(nullptr, 0, PreviewThread, nullptr, 0, nullptr) : nullptr;
        if (th) CloseHandle(th);
    }
    if (g_pe) SetEvent(g_pe);
}
}  // namespace

bool PreviewRequest(uint32_t seq, const std::wstring& key, const Image& im, const std::string& good, const std::wstring& drop) {
    Job old;
    AcquireSRWLockExclusive(&g_pl);
    const bool displaced = g_has && g_job.key != key;  // (the same source again: the newer job simply takes its place)
    if (displaced) old = std::move(g_job);
    g_job = Job{seq, MathContext(), g.loadGen.load(), key, im.mathKind, im.math, (float)g.cfg.fontSize, Scale(), g_pal[P_TEXT],
                PaletteIsDark(), good};
    g_has = true;
    ReleaseSRWLockExclusive(&g_pl);
    Wake();
    // The one slot forgets the job it held: a keystroke's own last one may go, but another popup's (closed meanwhile)
    // is the last render of its formula - its table entry would stay pending for good (Phase 4 notes)
    if (!displaced || old.key == drop) return displaced;
    QueueMathRender(old.key, old.ctx, old.loadGen, old.kind, old.src, old.fontPx, old.rgb, old.dark);
    return false;
}

void PreviewWarm(int what) {
    what &= ~g_warmAsked;  // once per process each
    if (!what) return;
    g_warmAsked |= what;
    AcquireSRWLockExclusive(&g_pl);
    g_warm |= what;
    ReleaseSRWLockExclusive(&g_pl);
    Wake();
}

// Back to the compiler's own inlining for the templates instantiated at the end of the file (see editcore.cpp's end)
#pragma inline_depth()
