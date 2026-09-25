// Find: matching (case / whole word), the find bar (drawn by the document canvas), match ticks on the scrollbar, and
// the text box. The UI thread runs with ImmDisableIME(0) (TSF costs 12–35 ms at start-up), so the box is a real EDIT
// control on its own thread: a child window of the document window, created on the first Ctrl+F. IME composition
// (Chinese, Japanese, Korean), the caret, selection and the clipboard then work natively, and the start-up path
// stays IME-free. The box posts its text and the keys it does not handle back to the UI thread (never SendMessage:
// no cross-thread waits).
#include "app.h"
#include <commctrl.h>

namespace {
const wchar_t kInputClass[] = L"FastMD.FindInput";
enum : UINT { IM_SETUP = WM_APP + 1 /* lp = InputSetup* */, IM_SETTEXT /* lp = std::wstring* */, IM_FOCUS /* wp = select all */,
              IM_CALL /* wp = a function, lp = its argument: edit mode's popups (editpop.cpp) run on this thread too */ };

struct InputSetup {
    RECT rc;
    int fontPx;
    COLORREF text, bg;
    bool show;
    std::wstring cue, face;
};

// ---- UI thread
HWND g_host = nullptr, g_edit = nullptr;  // set once the input thread reported success
bool g_inputFailed = false;
bool g_fieldFocused = false;  // UI thread's view of the box focus (FI_FOCUS)

// ---- start-up hand-off: the state decides who owns the windows, the event only wakes the waiting UI thread
volatile LONG g_inputState = 0;  // 0 starting, 1 ready, 2 abandoned by the UI thread (timeout), 3 creation failed
HANDLE g_inputReady = nullptr;   // one per process, never closed (the input thread may signal it after a timeout)
HWND g_newHost = nullptr, g_newEdit = nullptr;  // written by the input thread before it sets the state to 1

// ---- input thread only
HWND s_host = nullptr, s_edit = nullptr;
WNDPROC g_editProc = nullptr;
HFONT g_font = nullptr;
HBRUSH g_brush = nullptr;
COLORREF g_textCol = 0, g_bgCol = 0xFFFFFF;
int g_fontPx = 0;
std::wstring g_face;
bool g_echoOff = false;

COLORREF Ref(uint32_t rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

void PostKey(WPARAM vk, int mods) { PostMessageW(g.hwnd, WM_APP_FINDINPUT, FI_KEY, MAKELPARAM(vk, mods)); }

void DeleteWordBack(HWND h) {
    DWORD s = 0, e = 0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (s == e) {
        int len = GetWindowTextLengthW(h);
        std::wstring t(len + 1, L'\0');
        GetWindowTextW(h, t.data(), len + 1);
        DWORD k = std::min<DWORD>(s, (DWORD)len);
        while (k > 0 && iswspace(t[k - 1])) k--;
        while (k > 0 && !iswspace(t[k - 1])) k--;
        SendMessageW(h, EM_SETSEL, k, s);
    }
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"");
}

LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        int mods = (ctrl ? KM_CTRL : 0) | (shift ? KM_SHIFT : 0) | (alt ? KM_ALT : 0);
        switch (wp) {
        case VK_RETURN: case VK_ESCAPE: case VK_F3: case VK_PRIOR: case VK_NEXT: case VK_UP: case VK_DOWN: case VK_F5:
        case VK_TAB:
            PostKey(wp, mods);
            return 0;
        case 'A': case 'F':
            if (ctrl && !alt) { CallWindowProcW(g_editProc, h, EM_SETSEL, 0, -1); return 0; }
            break;
        case VK_BACK:
            if (ctrl) { DeleteWordBack(h); return 0; }
            break;
        case VK_LEFT: case VK_RIGHT:
            if (ctrl && alt) { PostKey(wp, mods); return 0; }
            break;
        case 'W': case 'O': case 'E': case 'R': case VK_OEM_PLUS: case VK_OEM_MINUS: case VK_ADD: case VK_SUBTRACT:
        case '0': case VK_NUMPAD0: case VK_OEM_COMMA:
            if (ctrl) { PostKey(wp, mods); return 0; }
            break;
        }
        break;
    }
    case WM_SYSKEYDOWN:
        if (wp == 'C' || wp == 'W' || wp == VK_LEFT || wp == VK_RIGHT) {
            PostKey(wp, KM_ALT | (GetKeyState(VK_CONTROL) < 0 ? KM_CTRL : 0));
            return 0;
        }
        break;
    case WM_SYSCHAR: {
        wchar_t c = (wchar_t)towlower((wint_t)wp);
        if (c == L'c' || c == L'w') return 0;  // Alt+C / Alt+W: no menu beep
        break;
    }
    case WM_CHAR:
        // control characters the box does not use (Ctrl+A, Ctrl+Backspace …) would beep or insert boxes;
        // keep Backspace and Ctrl+C / V / X / Z
        if (wp < 32 && wp != 8 && wp != 3 && wp != 22 && wp != 24 && wp != 26) return 0;
        if (wp == 0x7F) return 0;
        break;
    case WM_SETFOCUS: PostMessageW(g.hwnd, WM_APP_FINDINPUT, FI_FOCUS, 1); break;
    case WM_KILLFOCUS: PostMessageW(g.hwnd, WM_APP_FINDINPUT, FI_FOCUS, 0); break;
    }
    return CallWindowProcW(g_editProc, h, m, wp, lp);
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case IM_SETUP: {
        auto* s = (InputSetup*)lp;
        if (s->fontPx != g_fontPx || s->face != g_face) {
            HFONT f = CreateFontW(-s->fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, s->face.c_str());
            if (f) {
                SendMessageW(s_edit, WM_SETFONT, (WPARAM)f, TRUE);
                if (g_font) DeleteObject(g_font);
                g_font = f;
                g_fontPx = s->fontPx;
                g_face = s->face;
            }
        }
        if (!g_brush || s->bg != g_bgCol || s->text != g_textCol) {
            if (g_brush) DeleteObject(g_brush);
            g_brush = CreateSolidBrush(s->bg);
            g_bgCol = s->bg;
            g_textCol = s->text;
            InvalidateRect(h, nullptr, TRUE);
            InvalidateRect(s_edit, nullptr, TRUE);
        }
        SendMessageW(s_edit, EM_SETCUEBANNER, TRUE, (LPARAM)s->cue.c_str());
        int w = s->rc.right - s->rc.left, hh = s->rc.bottom - s->rc.top;
        int eh = std::min(hh, (int)std::ceil(s->fontPx * 1.45f));
        SetWindowPos(h, HWND_TOP, s->rc.left, s->rc.top, w, hh, SWP_NOACTIVATE | (s->show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        MoveWindow(s_edit, 0, (hh - eh) / 2, w, eh, TRUE);
        SendMessageW(s_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        delete s;
        return 0;
    }
    case IM_SETTEXT: {
        auto* t = (std::wstring*)lp;
        g_echoOff = true;
        SetWindowTextW(s_edit, t->c_str());
        g_echoOff = false;
        SendMessageW(s_edit, EM_SETSEL, 0, -1);
        delete t;
        return 0;
    }
    case IM_FOCUS:
        SetFocus(s_edit);
        if (wp) SendMessageW(s_edit, EM_SETSEL, 0, -1);
        return 0;
    case IM_CALL:
        ((void (*)(void*))wp)((void*)lp);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && !g_echoOff) {
            int len = GetWindowTextLengthW(s_edit);
            auto* t = new std::wstring(len + 1, L'\0');
            t->resize(GetWindowTextW(s_edit, t->data(), len + 1));
            if (!PostMessageW(g.hwnd, WM_APP_FINDINPUT, FI_TEXT, (LPARAM)t)) delete t;
        }
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, g_textCol);
        SetBkColor((HDC)wp, g_bgCol);
        return (LRESULT)g_brush;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, g_brush);
        return 1;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

DWORD WINAPI InputThread(void*) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = kInputClass;
    RegisterClassExW(&wc);
    // a child of the document window: the input queues of the two threads are attached, the document window stays
    // active (caption colours), and focus moves between the box and the document like inside one window
    s_host = CreateWindowExW(WS_EX_NOPARENTNOTIFY, kInputClass, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, g.hwnd,
                             nullptr, g.inst, nullptr);
    if (s_host)
        s_edit = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                 s_host, (HMENU)1, g.inst, nullptr);
    if (s_edit) {
        g_editProc = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
        SendMessageW(s_edit, EM_SETLIMITTEXT, 256, 0);
    }
    bool ok = s_host && s_edit;
    if (ok) {
        g_newHost = s_host;
        g_newEdit = s_edit;
    }
    LONG prev = InterlockedCompareExchange(&g_inputState, ok ? 1 : 3, 0);
    SetEvent(g_inputReady);
    if (!ok || prev != 0) {  // failed, or the UI thread stopped waiting: nobody will ever use these windows
        if (s_host) DestroyWindow(s_host);
        return 0;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

bool EnsureInput() {
    if (g_host) return true;
    if (g_inputFailed || !g.hwnd) return false;
    if (!g_inputReady) g_inputReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = g_inputReady ? CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr) : nullptr;
    if (th) {
        // creating a cross-thread child may send messages to this thread: keep serving them while waiting
        DWORD until = GetTickCount() + 3000;
        for (;;) {
            DWORD left = until - GetTickCount();
            if ((int)left <= 0) break;
            DWORD r = MsgWaitForMultipleObjectsEx(1, &g_inputReady, left, QS_SENDMESSAGE, 0);
            if (r == WAIT_OBJECT_0 + 1) {
                MSG msg;
                PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
                continue;
            }
            break;
        }
        CloseHandle(th);
    }
    // still starting → abandoned: the thread cleans up after itself if it ever gets there
    if (InterlockedCompareExchange(&g_inputState, 2, 0) == 1) {
        g_host = g_newHost;
        g_edit = g_newEdit;
    } else {
        g_inputFailed = true;  // fall back to typing into the bar on the UI thread (no IME composition)
    }
    return g_host != nullptr;
}
void PushSetup(bool show) {
    if (!g_host) return;
    float l, t, r, b;
    FindPartRect(FP_TEXT, &l, &t, &r, &b);
    float s = Scale();
    auto* su = new InputSetup{};
    su->rc = RECT{(LONG)std::lround(l * s), (LONG)std::lround(t * s), (LONG)std::lround(r * s), (LONG)std::lround(b * s)};
    su->fontPx = (int)std::lround(13.f * s);
    su->text = Ref(g_pal[P_TEXT]);
    su->bg = Ref(g_pal[P_BG]);
    su->show = show;
    su->cue = Tr(S_FIND_PLACEHOLDER);
    su->face = g.typo.uiFamily;
    if (!PostMessageW(g_host, IM_SETUP, 0, (LPARAM)su)) delete su;
}

// ---- matching
bool IsWordChar(wchar_t c) {
    WORD t = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &t);
    return (t & (C1_ALPHA | C1_DIGIT)) || c == L'_';
}

bool WholeWordAt(size_t pos, size_t len) {
    const std::wstring& t = g.doc.text;
    return (pos == 0 || !IsWordChar(t[pos - 1])) && (pos + len >= t.size() || !IsWordChar(t[pos + len]));
}

float CountAreaW() {  // fixed width: the text box does not move while the count changes
    static float w = 0;
    static UiLang lang = UL_RU;
    if (w > 0 && lang == UiLanguage()) return w;
    lang = UiLanguage();
    w = 64.f;
    for (const wchar_t* s : {L"9999 / 9999", Tr(S_FIND_NONE)}) {
        if (IDWriteTextLayout* L = UiLayout(s, 400.f)) {
            DWRITE_TEXT_METRICS m{};
            L->GetMetrics(&m);
            w = std::max(w, std::ceil(m.width) + 4.f);
            L->Release();
        }
    }
    return w;
}
}  // namespace

// ------------------------------------------------------------------------------------------------ matching
static uint32_t CurrentMatch() {
    return (g.curMatch >= 0 && (size_t)g.curMatch < g.matches.size()) ? g.matches[g.curMatch] : UINT32_MAX;
}

// The matches of the query in the current text. true = there are some and g.curMatch is set: the one at or after prev
// (keepCurrent), else the first at or below the top of the viewport.
static bool FindMatches(bool keepCurrent, uint32_t prev) {
    g.matches.clear();
    g.curMatch = -1;
    if (g.findQuery.empty() || g.doc.text.empty()) return false;
    const std::wstring* hay = &g.doc.text;
    std::wstring needle = g.findQuery;
    if (!g.cfg.findCase) {
        if (g.lowerText.size() != g.doc.text.size()) {
            g.lowerText = ToLower(g.doc.text);
            if (g.lowerText.size() != g.doc.text.size()) g.lowerText = g.doc.text;  // mapping changed length: exact match
        }
        hay = &g.lowerText;
        needle = ToLower(needle);
        if (needle.size() != g.findQuery.size()) needle = g.findQuery;
    }
    size_t pos = 0;
    while ((pos = hay->find(needle, pos)) != std::wstring::npos) {
        if (g.cfg.findWord && !WholeWordAt(pos, needle.size())) { pos++; continue; }
        g.matches.push_back((uint32_t)pos);
        pos += needle.size();
        if (g.matches.size() >= 100000) break;
    }
    if (g.matches.empty()) return false;
    if (keepCurrent && prev != UINT32_MAX) {
        auto it = std::lower_bound(g.matches.begin(), g.matches.end(), prev);
        g.curMatch = it == g.matches.end() ? 0 : (int)(it - g.matches.begin());
    } else {  // first match at or below the top of the viewport
        uint32_t top = g.doc.blocks.empty() ? 0 : g.doc.blocks[std::min<size_t>(FirstVisible(g.scrollY), g.doc.blocks.size() - 1)].textOff;
        auto it = std::lower_bound(g.matches.begin(), g.matches.end(), top);
        g.curMatch = it == g.matches.end() ? 0 : (int)(it - g.matches.begin());
    }
    return true;
}

void FindUpdate(bool keepCurrent) {
    if (FindMatches(keepCurrent, CurrentMatch())) FindStep(0);
    else Invalidate();
}

// An edit replaced the text [beg, oldEnd) with [beg, newEnd): the marks follow it, and the current match stays the one
// it was - shifted with the text after the edit, the first one after the edit's start when the edit took it - but the
// view stays where the reader is working (§12.6).
void FindRefresh(uint32_t beg, uint32_t oldEnd, uint32_t newEnd) {
    uint32_t prev = CurrentMatch();
    if (prev != UINT32_MAX && prev >= beg) prev = prev >= oldEnd ? prev - oldEnd + newEnd : beg;
    FindMatches(true, prev);
    Invalidate();
}

void FindStep(int dir) {
    if (g.matches.empty()) { Invalidate(); return; }
    g.userMoved = true;  // the reader went to a match: a late reading-position restore must not jump away
    int n = (int)g.matches.size();
    g.curMatch = ((g.curMatch < 0 ? 0 : g.curMatch) + dir + n) % n;
    RevealTextPos(g.matches[g.curMatch], false);
    Invalidate();
}

void FindToggleCase() {
    g.cfg.findCase = !g.cfg.findCase;
    FindUpdate(true);
}

void FindToggleWord() {
    g.cfg.findWord = !g.cfg.findWord;
    FindUpdate(true);
}

// ------------------------------------------------------------------------------------------------ open / close / input
void FindOpen() {
    g.findOpen = true;
    if (HasSelection()) {  // seed with the selected text (single line)
        std::wstring s = SelectionText();
        if (!s.empty() && s.size() < 200 && s.find(L'\n') == std::wstring::npos) g.findQuery = s;
    }
    if (EnsureInput()) {
        PushSetup(true);
        auto* t = new std::wstring(g.findQuery);
        if (!PostMessageW(g_host, IM_SETTEXT, 0, (LPARAM)t)) delete t;
        PostMessageW(g_host, IM_FOCUS, 1, 0);
    }
    FindUpdate(false);
    Invalidate();
}

void FindClose() {
    g.findOpen = false;
    g.findHot = -1;
    if (g_host) {
        PushSetup(false);
        SetFocus(g.hwnd);
    }
    Invalidate();
}

void FindRelayoutInput() {
    if (g_host) PushSetup(g.findOpen && !g.barSliding);  // hidden while edit mode's bar slides past it (R18)
}

bool FindInputFocused() { return g_host && g_fieldFocused; }

bool InputCall(void (*fn)(void*), void* arg) {
    return EnsureInput() && PostMessageW(g_host, IM_CALL, (WPARAM)fn, (LPARAM)arg);
}

void FindFocusInput() {
    if (g_host) PostMessageW(g_host, IM_FOCUS, 0, 0);
}

HWND FindEditHwnd() { return g_edit; }

bool FindTypeChar(wchar_t c) {
    if (!g.findOpen || (c < 32 && c != 8) || c == 127) return false;
    if (g_host) {  // typed while the document had the focus: continue in the box
        PostMessageW(g_host, IM_FOCUS, 0, 0);
        PostMessageW(g_edit, WM_CHAR, c, 0);
        return true;
    }
    if (c == 8) {
        if (g.findQuery.empty()) return true;
        g.findQuery.pop_back();
    } else {
        g.findQuery.push_back(c);
    }
    FindUpdate(false);
    return true;
}

void FindOnInput(WPARAM ev, LPARAM lp) {
    switch (ev) {
    case FI_TEXT: {
        auto* t = (std::wstring*)lp;
        if (g.findOpen && *t != g.findQuery) {
            g.findQuery = *t;
            FindUpdate(false);
        }
        delete t;
        break;
    }
    case FI_FOCUS:
        g_fieldFocused = lp != 0;
        Invalidate();
        break;
    case FI_KEY: {
        WPARAM vk = LOWORD(lp);
        int mods = HIWORD(lp);
        bool ctrl = mods & KM_CTRL, shift = mods & KM_SHIFT, alt = mods & KM_ALT;
        if (vk == VK_RETURN || vk == VK_F3) FindStep(shift ? -1 : 1);
        else if (vk == VK_ESCAPE) FindClose();
        else if (vk == VK_TAB) SetFocus(g.hwnd);  // keyboard to the document (Tab again: links)
        else if (alt && !ctrl && vk == 'C') FindToggleCase();
        else if (alt && !ctrl && vk == 'W') FindToggleWord();
        else KeyCommand(vk, ctrl, shift, alt);
        break;
    }
    }
}

// ------------------------------------------------------------------------------------------------ bar
void FindPartRect(int part, float* l, float* t, float* r, float* b) {
    // under edit mode's bar and a strip, when they are there (§12.1)
    float w = std::max(260.f, std::min(480.f, ViewW() - 24.f)), h = 40.f, x = std::max(4.f, ViewW() - w - 20.f);
    float y = 12.f + EditInset() + g.stripH;
    float bs = 28.f, by = y + 6.f;
    float closeL = x + w - 6.f - bs, nextL = closeL - 2.f - bs, prevL = nextL - 2.f - bs;
    float countW = CountAreaW(), countL = prevL - 6.f - countW;
    float fieldL = x + 6.f, fieldR = std::max(fieldL + 80.f, countL - 6.f);
    float wordL = fieldR - 3.f - 26.f, caseL = wordL - 2.f - 26.f;
    auto set = [&](float a, float bb, float c, float d) { *l = a; *t = bb; *r = c; *b = d; };
    switch (part) {
    case FP_FIELD: set(fieldL, by, fieldR, by + bs); break;
    case FP_TEXT: set(fieldL + 8.f, by + 2.f, caseL - 4.f, by + bs - 2.f); break;
    case FP_CASE: set(caseL, by + 3.f, caseL + 26.f, by + bs - 3.f); break;
    case FP_WORD: set(wordL, by + 3.f, wordL + 26.f, by + bs - 3.f); break;
    case FP_COUNT: set(countL, by, countL + countW, by + bs); break;
    case FP_PREV: set(prevL, by, prevL + bs, by + bs); break;
    case FP_NEXT: set(nextL, by, nextL + bs, by + bs); break;
    case FP_CLOSE: set(closeL, by, closeL + bs, by + bs); break;
    default: set(x, y, x + w, y + h); break;
    }
}

int FindPartAt(float x, float y) {
    if (!g.findOpen) return FP_NONE;
    for (int p : {FP_CASE, FP_WORD, FP_PREV, FP_NEXT, FP_CLOSE, FP_FIELD, FP_BAR}) {
        float l, t, r, b;
        FindPartRect(p, &l, &t, &r, &b);
        if (x >= l && x < r && y >= t && y < b) return p;
    }
    return FP_NONE;
}

void FindClick(int part) {
    switch (part) {
    case FP_CASE: FindToggleCase(); break;
    case FP_WORD: FindToggleWord(); break;
    case FP_PREV: FindStep(-1); break;
    case FP_NEXT: FindStep(1); break;
    case FP_CLOSE: FindClose(); break;
    case FP_FIELD: case FP_TEXT: FindFocusInput(); break;
    default: break;
    }
}

static void DrawButtonBg(int part, bool active) {
    float l, t, r, b;
    FindPartRect(part, &l, &t, &r, &b);
    if (active) {
        g.canvas->FillRoundRect(l, t, r, b, 4.f, P_CURRENT);
        g.canvas->StrokeRoundRect(l, t, r, b, 4.f, 1.f, P_ACCENT);
    } else if (g.findHot == part) {
        g.canvas->FillRoundRect(l, t, r, b, 4.f, P_HOVER);
    }
}

static void DrawCentered(int part, const wchar_t* s, UINT32 n, IDWriteTextFormat* fmt, float size, uint8_t pal) {
    float l, t, r, b;
    FindPartRect(part, &l, &t, &r, &b);
    IDWriteTextLayout* L = nullptr;
    if (FAILED(g.dwf->CreateTextLayout(s, n, fmt, r - l, b - t, &L))) return;
    L->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    L->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    L->SetFontSize(size, DWRITE_TEXT_RANGE{0, n});
    g.canvas->Text(L, l, t, pal);
    L->Release();
}

void DrawFindBar() {
    float l, t, r, b;
    FindPartRect(FP_BAR, &l, &t, &r, &b);
    g.canvas->FillRoundRect(l, t, r, b, 8.f, P_OVERLAY_BG);
    g.canvas->StrokeRoundRect(l, t, r, b, 8.f, 1.f, P_OVERLAY_BORDER);
    // the text box (the EDIT child draws the text itself; this is its frame)
    FindPartRect(FP_FIELD, &l, &t, &r, &b);
    bool focused = FindInputFocused();
    g.canvas->FillRoundRect(l, t, r, b, 5.f, P_BG);
    g.canvas->StrokeRoundRect(l, t, r, b, 5.f, focused ? 1.5f : 1.f, focused ? P_ACCENT : P_BORDER);
    if (!g_host) {  // fallback: typed on the UI thread, drawn here
        FindPartRect(FP_TEXT, &l, &t, &r, &b);
        bool empty = g.findQuery.empty();
        if (IDWriteTextLayout* ql = UiLayout(empty ? std::wstring(Tr(S_FIND_PLACEHOLDER)) : g.findQuery, 10000.f)) {
            DWRITE_TEXT_METRICS m{};
            ql->GetMetrics(&m);
            float maxW = r - l, shift = empty ? 0.f : std::max(0.f, m.width - maxW + 2.f);
            float ty = t + (b - t - m.height) * 0.5f;
            g.canvas->PushClip(l, t, r, b);
            g.canvas->Text(ql, l - shift, ty, empty ? P_MUTED : P_TEXT);
            g.canvas->PopClip();
            float cx = empty ? l : l - shift + m.widthIncludingTrailingWhitespace + 1.f;
            g.canvas->FillRect(cx, ty + 1.f, cx + 1.2f, ty + m.height - 1.f, P_TEXT);
            ql->Release();
        }
    }
    // toggles: Aa (match case), ab̲ (whole word)
    DrawButtonBg(FP_CASE, g.cfg.findCase);
    DrawCentered(FP_CASE, L"Aa", 2, g.typo.ui, 13.f, g.cfg.findCase ? P_ACCENT : P_MUTED);
    DrawButtonBg(FP_WORD, g.cfg.findWord);
    DrawCentered(FP_WORD, L"ab", 2, g.typo.ui, 13.f, g.cfg.findWord ? P_ACCENT : P_MUTED);
    FindPartRect(FP_WORD, &l, &t, &r, &b);
    float cx = (l + r) * 0.5f;
    g.canvas->FillRect(cx - 7.f, b - 5.f, cx + 7.f, b - 4.f, g.cfg.findWord ? P_ACCENT : P_MUTED);
    // count
    std::wstring count;
    if (!g.findQuery.empty())
        count = g.matches.empty() ? std::wstring(Tr(S_FIND_NONE))
                                  : std::to_wstring(g.curMatch + 1) + L" / " + std::to_wstring(g.matches.size());
    if (!count.empty()) {
        FindPartRect(FP_COUNT, &l, &t, &r, &b);
        if (IDWriteTextLayout* cl = UiLayout(count, r - l)) {
            cl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            DWRITE_TEXT_METRICS m{};
            cl->GetMetrics(&m);
            g.canvas->Text(cl, l, t + (b - t - m.height) * 0.5f, g.matches.empty() ? P_ALERT_CAUTION : P_MUTED);
            cl->Release();
        }
    }
    // buttons (Segoe Fluent Icons: ChevronUp, ChevronDown, Cancel)
    static const wchar_t kUp = 0xE70E, kDown = 0xE70D, kClose = 0xE711;
    uint8_t arrows = g.matches.empty() ? P_BORDER : P_OVERLAY_TEXT;
    DrawButtonBg(FP_PREV, false);
    DrawCentered(FP_PREV, &kUp, 1, g.typo.uiIcon, 12.f, arrows);
    DrawButtonBg(FP_NEXT, false);
    DrawCentered(FP_NEXT, &kDown, 1, g.typo.uiIcon, 12.f, arrows);
    DrawButtonBg(FP_CLOSE, false);
    DrawCentered(FP_CLOSE, &kClose, 1, g.typo.uiIcon, 11.f, P_OVERLAY_TEXT);
}

// ticks for every match on the scrollbar track (one per pixel row), the current one on top
void DrawFindMarks(float x0, float x1) {
    size_t nb = g.doc.blocks.size();
    if (!nb || g.docH <= 0 || g.matches.empty()) return;
    float trackT = ScrollTrackTop(), trackH = ViewH() - 2.f - trackT, s = Scale();
    auto yOf = [&](uint32_t pos, size_t& bi) {
        while (bi + 1 < nb && g.doc.blocks[bi + 1].textOff <= pos) bi++;
        const Block& b = g.doc.blocks[bi];
        float frac = b.textLen ? std::clamp((float)(pos - b.textOff) / b.textLen, 0.f, 1.f) : 0.f;
        return trackT + trackH * ((g.Y[bi] + g.H[bi] * frac) / g.docH);
    };
    size_t bi = 0;
    int lastRow = INT_MIN;
    for (size_t k = 0; k < g.matches.size(); k++) {
        float y = yOf(g.matches[k], bi);
        int row = (int)(y * s);
        if (row == lastRow) continue;
        lastRow = row;
        g.canvas->FillRect(x0, y - 1.f, x1, y + 1.f, P_MARK);
    }
    if (g.curMatch >= 0 && (size_t)g.curMatch < g.matches.size()) {
        size_t b0 = 0;
        float y = yOf(g.matches[g.curMatch], b0);
        g.canvas->FillRect(x0, y - 1.5f, x1, y + 1.5f, P_FIND_CUR);
    }
}
