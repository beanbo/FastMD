// Settings window: drawn with the document engine (DirectWrite → GDI DIB canvas, same palette and fonts), created only
// when opened. Every choice applies at once (the document re-lays out behind it), is saved, and other open FastMD
// windows are told to pick it up.
#include "app.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include "version.h"

namespace {
const wchar_t kClass[] = L"FastMD.Settings";
const wchar_t kRepoUrl[] = L"https://github.com/beanbo/FastMD";
const float kW = 680.f, kLabelX = 28.f, kCtlX = 236.f, kRowH = 48.f, kTop = 20.f, kCtlH = 32.f;
const int kSizes[] = {14, 15, 16, 17, 18, 20};

enum Row { ROW_THEME, ROW_FONT, ROW_SIZE, ROW_COLUMN, ROW_WRAP, ROW_SMOOTH, ROW_REMOTE, ROW_UPDATE, ROW_VERSION,
           ROW_LANG, ROW_EDITOR, ROW_ASSOC, ROW_COUNT };
const int kLinkId = 9000;

HWND g_wnd = nullptr;
Canvas* g_cv = nullptr;
float g_scale = 1.f;  // pixels per DIP (this window's DPI; the document zoom does not apply)
int g_pxW = 0, g_pxH = 0;
int g_hot = -1;
struct Hit { float l, t, r, b; int id; };
std::vector<Hit> g_hits;

float WindowH() { return kTop + (float)ROW_COUNT * kRowH + 64.f; }

std::wstring EditorLabel() {
    if (g.cfg.editor.empty()) return Tr(S_EDITOR_SYSTEM);
    for (const EditorInfo& e : DetectEditors())
        if (!_wcsicmp(e.exe.c_str(), g.cfg.editor.c_str())) return e.name;
    return FileNameOf(g.cfg.editor);
}

std::vector<std::wstring> Options(int row) {
    switch (row) {
    case ROW_THEME: return {Tr(S_THEME_SYSTEM), Tr(S_THEME_LIGHT), Tr(S_THEME_DARK)};
    case ROW_FONT: return {Tr(S_FONT_SEGOE), Tr(S_FONT_SITKA)};
    case ROW_SIZE: {
        std::vector<std::wstring> v;
        for (int s : kSizes) v.push_back(std::to_wstring(s));
        return v;
    }
    case ROW_COLUMN: return {Tr(S_COL_NARROW), Tr(S_COL_NORMAL), Tr(S_COL_WIDE), Tr(S_COL_FULL)};
    case ROW_WRAP: case ROW_SMOOTH: case ROW_UPDATE: return {Tr(S_OFF), Tr(S_ON)};
    case ROW_REMOTE: return {Tr(S_REMOTE_ALWAYS), Tr(S_REMOTE_ASK), Tr(S_REMOTE_NEVER)};
    case ROW_LANG: return {Tr(S_LANG_SYSTEM), L"Русский", L"English"};
    default: return {};
    }
}

int Selected(int row) {
    switch (row) {
    case ROW_THEME: return g.cfg.theme;
    case ROW_FONT: return g.cfg.font;
    case ROW_SIZE:
        for (int k = 0; k < (int)std::size(kSizes); k++) if (kSizes[k] == g.cfg.fontSize) return k;
        return -1;
    case ROW_COLUMN: return g.cfg.column;
    case ROW_WRAP: return g.cfg.wrapCode;
    case ROW_SMOOTH: return g.cfg.smoothScroll;
    case ROW_REMOTE: return g.cfg.remoteImages;
    case ROW_UPDATE: return g.cfg.updateCheck;
    case ROW_LANG: return g.cfg.language;
    default: return -1;
    }
}

const wchar_t* Label(int row) {
    static const StrId ids[ROW_COUNT] = {S_SET_THEME,  S_SET_FONT,   S_SET_SIZE,    S_SET_COLUMN,   S_SET_WRAP,
                                         S_SET_SMOOTH, S_SET_REMOTE, S_SET_UPDATE,  S_SET_VERSION,  S_SET_LANGUAGE,
                                         S_SET_EDITOR, S_SET_ASSOC};
    return Tr(ids[row]);
}

IDWriteTextLayout* Layout(const std::wstring& s, float maxW, float size = 13.f, bool semibold = false) {
    IDWriteTextLayout* L = UiLayout(s, maxW);
    if (!L) return nullptr;
    DWRITE_TEXT_RANGE all{0, (UINT32)s.size()};
    L->SetFontSize(size, all);
    if (semibold) L->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
    return L;
}

float TextW(IDWriteTextLayout* L) {
    DWRITE_TEXT_METRICS m{};
    L->GetMetrics(&m);
    return std::ceil(m.widthIncludingTrailingWhitespace);
}

void TextMid(IDWriteTextLayout* L, float x, float top, float h, uint8_t pal) {
    DWRITE_TEXT_METRICS m{};
    L->GetMetrics(&m);
    g_cv->Text(L, x, top + (h - m.height) * 0.5f, pal);
}

// a row of options: one rounded container, the selected segment filled with the accent colour
void Segmented(int row, float x, float y) {
    auto opts = Options(row);
    int sel = Selected(row);
    std::vector<IDWriteTextLayout*> ls;
    float total = 0;
    for (auto& o : opts) {
        ls.push_back(Layout(o, 400.f));
        total += (ls.back() ? TextW(ls.back()) : 20.f) + 26.f;
    }
    g_cv->FillRoundRect(x, y, x + total, y + kCtlH, 6.f, P_PANEL);
    g_cv->StrokeRoundRect(x, y, x + total, y + kCtlH, 6.f, 1.f, P_BORDER);
    float xx = x;
    for (size_t k = 0; k < opts.size(); k++) {
        float w = (ls[k] ? TextW(ls[k]) : 20.f) + 26.f;
        int id = row * 100 + (int)k;
        if ((int)k == sel) g_cv->FillRoundRect(xx + 2.f, y + 2.f, xx + w - 2.f, y + kCtlH - 2.f, 5.f, P_ACCENT);
        else if (g_hot == id) g_cv->FillRoundRect(xx + 2.f, y + 2.f, xx + w - 2.f, y + kCtlH - 2.f, 5.f, P_HOVER);
        if (ls[k]) {
            TextMid(ls[k], xx + 13.f, y, kCtlH, (int)k == sel ? P_ONACCENT : P_TEXT);
            ls[k]->Release();
        }
        g_hits.push_back(Hit{xx, y, xx + w, y + kCtlH, id});
        xx += w;
    }
}

void Button(int id, const std::wstring& text, float x, float y, bool dropdown) {
    IDWriteTextLayout* L = Layout(text, 360.f);
    float tw = L ? TextW(L) : 40.f, w = tw + (dropdown ? 52.f : 28.f);
    g_cv->FillRoundRect(x, y, x + w, y + kCtlH, 6.f, g_hot == id ? P_HOVER : P_PANEL);
    g_cv->StrokeRoundRect(x, y, x + w, y + kCtlH, 6.f, 1.f, P_BORDER);
    if (L) {
        TextMid(L, x + 14.f, y, kCtlH, P_TEXT);
        L->Release();
    }
    if (dropdown) {  // Segoe Fluent Icons: ChevronDown
        static const wchar_t kDown = 0xE70D;
        IDWriteTextLayout* il = nullptr;
        if (SUCCEEDED(g.dwf->CreateTextLayout(&kDown, 1, g.typo.uiIcon, 24.f, kCtlH, &il))) {
            il->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            il->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            il->SetFontSize(10.f, DWRITE_TEXT_RANGE{0, 1});
            g_cv->Text(il, x + w - 30.f, y, P_MUTED);
            il->Release();
        }
    }
    g_hits.push_back(Hit{x, y, x + w, y + kCtlH, id});
}

// The update row: one button that does the next step - check, update to the version found, restart into it - and a
// note beside it saying where things stand. While something runs, the button only says what.
void UpdateRow(float x, float y) {
    UpdateStatus st = UpdateGetStatus();
    std::wstring ver = UpdateVersion();
    wchar_t buf[160];
    std::wstring label, note;
    bool accent = false, busy = false;
    switch (st) {
    case US_CHECKING: label = Tr(S_UPD_CHECKING); busy = true; break;
    case US_DOWNLOADING: label = Tr(S_UPD_DOWNLOADING); busy = true; break;
    case US_INSTALLING: label = Tr(S_UPD_INSTALLING); busy = true; break;
    case US_INSTALLED:
        label = Tr(S_UPD_RESTART);
        accent = true;
        swprintf_s(buf, Tr(S_UPD_NOTE_INSTALLED_FMT), ver.c_str());
        note = buf;
        break;
    case US_AVAILABLE:
    case US_DOWNLOAD_FAILED:
        swprintf_s(buf, Tr(S_UPD_UPDATE_FMT), ver.c_str());
        label = buf;
        accent = true;
        if (st == US_DOWNLOAD_FAILED) note = Tr(S_UPD_NOTE_DL_FAILED);
        else {
            swprintf_s(buf, Tr(S_UPD_NOTE_CURRENT_FMT), FASTMD_VERSION_WSTR);
            note = buf;
        }
        break;
    case US_LATEST:
        label = Tr(S_UPD_CHECK);
        swprintf_s(buf, Tr(S_UPD_NOTE_LATEST_FMT), FASTMD_VERSION_WSTR);
        note = buf;
        break;
    case US_CHECK_FAILED: label = Tr(S_UPD_CHECK); note = Tr(S_UPD_NOTE_FAILED); break;
    default:
        label = Tr(S_UPD_CHECK);
        swprintf_s(buf, Tr(S_UPD_NOTE_CURRENT_FMT), FASTMD_VERSION_WSTR);
        note = buf;
        break;
    }
    int id = ROW_VERSION * 100;
    IDWriteTextLayout* L = Layout(label, 360.f, 13.f, accent);
    float w = (L ? TextW(L) : 60.f) + 28.f;
    if (accent) g_cv->FillRoundRect(x, y, x + w, y + kCtlH, 6.f, g_hot == id ? P_LINK : P_ACCENT);
    else {
        g_cv->FillRoundRect(x, y, x + w, y + kCtlH, 6.f, g_hot == id && !busy ? P_HOVER : P_PANEL);
        g_cv->StrokeRoundRect(x, y, x + w, y + kCtlH, 6.f, 1.f, P_BORDER);
    }
    if (L) {
        TextMid(L, x + 14.f, y, kCtlH, accent ? P_ONACCENT : busy ? P_MUTED : P_TEXT);
        L->Release();
    }
    if (!busy) g_hits.push_back(Hit{x, y, x + w, y + kCtlH, id});
    if (!note.empty()) {
        if (IDWriteTextLayout* N = Layout(note, kW - kLabelX - (x + w + 14.f), 12.5f)) {
            TextMid(N, x + w + 14.f, y, kCtlH, P_MUTED);
            N->Release();
        }
    }
}

void Paint() {
    if (!g_cv) return;
    g_hits.clear();
    g_cv->Begin();
    g_cv->Clear(P_BG);
    float y = kTop;
    for (int row = 0; row < ROW_COUNT; row++, y += kRowH) {
        if (IDWriteTextLayout* L = Layout(Label(row), kCtlX - kLabelX - 12.f, 13.5f)) {
            TextMid(L, kLabelX, y, kCtlH, P_TEXT);
            L->Release();
        }
        if (row == ROW_EDITOR) Button(row * 100, EditorLabel(), kCtlX, y, true);
        else if (row == ROW_ASSOC) Button(row * 100, Tr(S_SET_ASSOC_BTN), kCtlX, y, false);
        else if (row == ROW_VERSION) UpdateRow(kCtlX, y);
        else Segmented(row, kCtlX, y);
    }
    // footer: version, licence, repository
    g_cv->FillRect(kLabelX, y + 2.f, kW - kLabelX, y + 3.f, P_BORDER);
    wchar_t about[160];
    swprintf_s(about, Tr(S_SET_ABOUT), FASTMD_VERSION_WSTR);
    if (IDWriteTextLayout* L = Layout(about, 360.f, 12.5f)) {
        TextMid(L, kLabelX, y + 12.f, 28.f, P_MUTED);
        L->Release();
    }
    if (IDWriteTextLayout* L = Layout(L"github.com/beanbo/FastMD", 300.f, 12.5f)) {
        float w = TextW(L), x = kW - kLabelX - w;
        TextMid(L, x, y + 12.f, 28.f, P_LINK);
        if (g_hot == kLinkId) g_cv->FillRect(x, y + 33.f, x + w, y + 34.f, P_LINK);
        g_hits.push_back(Hit{x, y + 12.f, x + w, y + 40.f, kLinkId});
        L->Release();
    }
    g_cv->End();
}

int HitAt(float x, float y) {
    for (const Hit& h : g_hits)
        if (x >= h.l && x < h.r && y >= h.t && y < h.b) return h.id;
    return -1;
}

void EditorMenu(float x, float y) {
    auto eds = DetectEditors();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g.cfg.editor.empty() ? MF_CHECKED : 0), 1, Tr(S_EDITOR_SYSTEM));
    bool known = g.cfg.editor.empty();
    for (size_t k = 0; k < eds.size(); k++) {
        bool on = !_wcsicmp(eds[k].exe.c_str(), g.cfg.editor.c_str());
        known |= on;
        AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), 10 + (UINT)k, eds[k].name.c_str());
    }
    if (!known) AppendMenuW(m, MF_STRING | MF_CHECKED, 2, FileNameOf(g.cfg.editor).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, Tr(S_EDITOR_OTHER));
    POINT p{(LONG)std::lround(x * g_scale), (LONG)std::lround(y * g_scale)};
    ClientToScreen(g_wnd, &p);
    UINT id = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (!id || id == 2) return;
    if (id == 1) g.cfg.editor.clear();
    else if (id == 3) {
        std::wstring exe = PickExeDialog(g_wnd);
        if (exe.empty()) return;
        g.cfg.editor = exe;
    } else if (id >= 10 && id - 10 < eds.size()) g.cfg.editor = eds[id - 10].exe;
    ApplySettings(SC_OTHER, true);
}

void Pick(int id) {
    if (id == kLinkId) { ShellExecuteW(g_wnd, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL); return; }
    int row = id / 100, opt = id % 100;
    uint32_t changed = 0;
    switch (row) {
    case ROW_THEME: g.cfg.theme = (ThemeMode)opt; changed = SC_THEME; break;
    case ROW_FONT: g.cfg.font = (uint8_t)opt; changed = SC_TYPE; break;
    case ROW_SIZE: g.cfg.fontSize = kSizes[std::min(opt, (int)std::size(kSizes) - 1)]; changed = SC_TYPE; break;
    case ROW_COLUMN: g.cfg.column = (uint8_t)opt; changed = SC_COLUMN; break;
    case ROW_WRAP: g.cfg.wrapCode = opt == 1; changed = SC_TYPE; break;
    case ROW_SMOOTH: g.cfg.smoothScroll = opt == 1; changed = SC_OTHER; break;
    case ROW_REMOTE:
        g.cfg.remoteImages = (uint8_t)opt;
        if (opt == 0) LoadRemoteImages();  // switched to "always": fetch what this document is still missing
        changed = SC_OTHER;
        break;
    case ROW_UPDATE:
        g.cfg.updateCheck = opt == 1;
        ApplySettings(SC_OTHER, true);
        if (g.cfg.updateCheck) UpdateCheckAsync();  // switched on: ask now if a day has passed
        return;
    case ROW_VERSION:
        switch (UpdateGetStatus()) {
        case US_AVAILABLE: case US_DOWNLOAD_FAILED: UpdateInstall(false); break;  // the button named the version
        case US_INSTALLED: UpdateRestart(); break;
        default: UpdateCheckNow(); break;
        }
        return;
    case ROW_LANG: g.cfg.language = (uint8_t)opt; changed = SC_LANGUAGE; break;
    case ROW_EDITOR: {
        for (const Hit& h : g_hits)
            if (h.id == id) { EditorMenu(h.l, h.b + 2.f); break; }
        return;
    }
    case ROW_ASSOC: Command(CMD_ASSOCIATE); return;
    default: return;
    }
    ApplySettings(changed, true);
}

void Chrome() {
    BOOL dark = PaletteIsDark();
    DwmSetWindowAttribute(g_wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    SetWindowTextW(g_wnd, Tr(S_SETTINGS_TITLE));
}

void Resize(UINT dpi) {
    g_scale = dpi / 96.f;
    RECT r{0, 0, (LONG)std::lround(kW * g_scale), (LONG)std::lround(WindowH() * g_scale)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    SetWindowPos(g_wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Paint();
        if (g_cv) BitBlt(dc, 0, 0, g_pxW, g_pxH, g_cv->DC(), 0, g_cv->ViewportTop(), SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        g_pxW = LOWORD(lp);
        g_pxH = HIWORD(lp);
        if (g_cv && g_pxW && g_pxH) g_cv->Resize(g_pxW, g_pxH);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        g_scale = HIWORD(wp) / 96.f;
        if (g_cv) g_cv->SetScale(g_scale);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int hot = HitAt(GET_X_LPARAM(lp) / g_scale, GET_Y_LPARAM(lp) / g_scale);
        if (hot != g_hot) { g_hot = hot; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        SetCursor(LoadCursorW(nullptr, hot >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hot != -1) { g_hot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE;
        break;
    case WM_LBUTTONDOWN: {
        int id = HitAt(GET_X_LPARAM(lp) / g_scale, GET_Y_LPARAM(lp) / g_scale);
        if (id >= 0) Pick(id);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
        break;
    case WM_DESTROY:
        delete g_cv;
        g_cv = nullptr;
        g_wnd = nullptr;
        g_hot = -1;
        if (g.hwnd) SetForegroundWindow(g.hwnd);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

HWND SettingsHwnd() { return g_wnd; }

LRESULT SettingsHitCenter(int id) {  // tests: client pixel centre of a control (row * 100 + option)
    for (const Hit& h : g_hits)
        if (h.id == id) return MAKELONG(std::lround((h.l + h.r) * 0.5f * g_scale), std::lround((h.t + h.b) * 0.5f * g_scale));
    return -1;
}

void SettingsOpen() {
    if (g_wnd) { SetForegroundWindow(g_wnd); return; }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = g.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    UINT dpi = GetDpiForWindow(g.hwnd);
    float s = dpi / 96.f;
    RECT r{0, 0, (LONG)std::lround(kW * s), (LONG)std::lround(WindowH() * s)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = owner.left + ((owner.right - owner.left) - w) / 2, y = owner.top + std::max(0, (int)((owner.bottom - owner.top) - h) / 3);
    g_wnd = CreateWindowExW(0, kClass, Tr(S_SETTINGS_TITLE), WS_CAPTION | WS_SYSMENU, x, y, w, h, g.hwnd, nullptr, g.inst, nullptr);
    if (!g_wnd) return;
    g_scale = GetDpiForWindow(g_wnd) / 96.f;
    if (g_scale != s) Resize(GetDpiForWindow(g_wnd));
    RECT cr;
    GetClientRect(g_wnd, &cr);
    g_pxW = cr.right;
    g_pxH = cr.bottom;
    g_cv = CreateGdiCanvas(g.dwf, std::max(1, g_pxW), std::max(1, g_pxH), g_scale);
    Chrome();
    ShowWindow(g_wnd, SW_SHOW);
}

void SettingsRefresh() {
    if (!g_wnd) return;
    Chrome();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

// ------------------------------------------------------------------------------------------------ gear button
// The document window's way in besides Ctrl+, and the menu: a gear in the top-right corner, the same size as the
// outline button in the top-left one, placed just left of the scrollbar's 14 DIP hit zone so the glyph stays in the
// column's side padding. An icon-font glyph, so it waits for the second frame; the find bar takes that corner while open.
bool SettingsButtonRect(float* l, float* t, float* r, float* b) {
    const float kBtn = 30.f, kTop = 8.f, kRight = 14.f;
    if (g.firstFrame || g.findOpen) return false;
    *r = ViewW() - kRight;
    *l = *r - kBtn;
    *t = kTop;
    *b = kTop + kBtn;
    return true;
}

bool SettingsButtonHit(float x, float y) {
    float l, t, r, b;
    return SettingsButtonRect(&l, &t, &r, &b) && x >= l && x < r && y >= t && y < b;
}

void DrawSettingsButton() {
    float l, t, r, b;
    if (!SettingsButtonRect(&l, &t, &r, &b)) return;
    bool hot = g.settingsBtnHot;
    if (hot) g.canvas->FillRoundRect(l, t, r, b, 6.f, P_HOVER);
    DrawIcon(0xE713, l, t, r - l, 15.f, hot ? P_TEXT : P_MUTED);  // Segoe Fluent Icons: Settings
    // a newer version is known: a dot on the gear until it is installed (the tooltip names the version)
    if (UpdateAvailable()) g.canvas->FillCircle(r - 7.f, t + 7.f, 3.5f, P_ACCENT);
}
