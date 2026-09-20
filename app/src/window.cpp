// Win32 window: input routing, keyboard shortcuts, commands, context menu, settings application, window placement,
// automation queries, entry point.
#include "app.h"
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>

#define GET_X(lp) ((int)(short)LOWORD(lp))
#define GET_Y(lp) ((int)(short)HIWORD(lp))

static const wchar_t kClass[] = L"FastMD.Document";
static const float kZoomSteps[] = {0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.f, 1.1f, 1.25f, 1.5f, 1.75f, 2.f, 2.5f, 3.f};
static UINT g_msgSettings = 0;      // registered "settings changed" message between FastMD windows
static bool g_findHadFocus = false;  // the find box had the keyboard when the window was deactivated

void Invalidate() {
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------------------------------------ window placement
// Placement of the last closed window (HKCU\Software\FastMD\Window, one binary value = one registry read).
struct SavedWindow {
    uint32_t version;     // 1
    RECT normal;          // restored (non-maximized) rect, workspace coordinates (GetWindowPlacement)
    int32_t clientW, clientH;  // client size at close (px) — the doc thread lays out for it before the window exists
    uint32_t dpi;         // DPI of the window at close
    uint32_t maximized;
};
static SavedWindow g_saved{};
static bool g_haveSaved = false;
static bool g_sizeFromArgs = false;  // --size=WxH (tests): ignore the saved placement

static void LoadPlacement() {
    g_haveSaved = RegReadBinary(L"Window", &g_saved, sizeof(g_saved)) && g_saved.version == 1 &&
                  g_saved.normal.right - g_saved.normal.left >= 200 && g_saved.normal.bottom - g_saved.normal.top >= 150 &&
                  g_saved.dpi >= 48 && g_saved.dpi <= 960;
}

static void SavePlacement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!g.hwnd || !GetWindowPlacement(g.hwnd, &wp)) return;
    SavedWindow s{};
    s.version = 1;
    s.normal = wp.rcNormalPosition;
    s.dpi = GetDpiForWindow(g.hwnd);
    s.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    if (wp.showCmd == SW_SHOWMINIMIZED && !s.maximized) {  // minimized: derive the client size from the normal rect
        RECT fr{0, 0, 0, 0};
        AdjustWindowRectExForDpi(&fr, WS_OVERLAPPEDWINDOW, FALSE, 0, s.dpi);
        s.clientW = (s.normal.right - s.normal.left) - (fr.right - fr.left);
        s.clientH = (s.normal.bottom - s.normal.top) - (fr.bottom - fr.top);
    } else {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        s.clientW = rc.right;
        s.clientH = rc.bottom;
    }
    RegWriteBinary(L"Window", &s, sizeof(s));
}

static void SaveAll() {  // window closes / session ends
    if (BenchActive()) return;
    g.cfg.tocOpen = g.tocOpen;
    SaveConfig(g.cfg, g.findQuery);
    SavePlacement();
    SaveReadingPosition();
}

// Where to create the window: the saved normal rect (workspace → screen coordinates), shifted diagonally while another
// FastMD window already sits exactly there (several documents opened at once cascade instead of stacking).
// Returns false when there is no usable saved placement (first run, or its monitor is gone).
static bool RestoredRect(RECT* out) {
    if (!g_haveSaved || g_sizeFromArgs) return false;
    RECT r = g_saved.normal;
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return false;
    OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);  // workspace → screen
    int step = MulDiv(28, (int)g_saved.dpi, 96);
    for (int k = 0; k < 12; k++) {
        bool taken = false;
        for (HWND h = FindWindowExW(nullptr, nullptr, kClass, nullptr); h && !taken; h = FindWindowExW(nullptr, h, kClass, nullptr)) {
            RECT o;
            taken = GetWindowRect(h, &o) && o.left == r.left && o.top == r.top;
        }
        if (!taken) break;
        OffsetRect(&r, step, step);
        if (r.right > mi.rcWork.right || r.bottom > mi.rcWork.bottom) {  // wrapped past the work area: back to its corner
            OffsetRect(&r, mi.rcWork.left - r.left + step * (k % 4), mi.rcWork.top - r.top + step * (k % 4));
        }
    }
    *out = r;
    return true;
}

// ------------------------------------------------------------------------------------------------ theme / settings
static bool WantDark() { return g.cfg.theme == TM_DARK || (g.cfg.theme == TM_SYSTEM && SystemPrefersDark()); }

static void ApplyWindowChrome(HWND h) {
    BOOL dark = PaletteIsDark();
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int backdrop = 2;  // DWMSBT_MAINWINDOW: Mica in the caption (the document keeps a solid background)
    DwmSetWindowAttribute(h, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
}

void ApplyTheme() {
    bool dark = WantDark();
    if (dark == PaletteIsDark()) return;
    SetDarkPalette(dark);
    if (g.hwnd) {
        ApplyWindowChrome(g.hwnd);
        SetClassLongPtrW(g.hwnd, GCLP_HBRBACKGROUND,
                         (LONG_PTR)CreateSolidBrush(RGB(g_pal[P_BG] >> 16, (g_pal[P_BG] >> 8) & 255, g_pal[P_BG] & 255)));
    }
    FindRelayoutInput();
    SettingsRefresh();
    Invalidate();
}

static UiLang ResolveLanguage() {
    return g.cfg.language == LANG_RU ? UL_RU : g.cfg.language == LANG_EN ? UL_EN : SystemUiLanguage();
}

static void TypographyOptions(Typography& t) {
    t.fontSet = g.cfg.font;
    t.textScale = g.cfg.fontSize / 16.f;
    t.wrapCode = g.cfg.wrapCode;
}

// Only measure jobs follow the layout generation; image decoding and a big file's full parse keep running (they use
// the document generation). While the full parse is pending the prefix is not measured: OnFullDoc measures the whole.
void Relayout() {
    ClearLayoutCache();
    WithAnchor([] {
        UpdateColumns();
        InitGeometry();
        RecomputeY();
    });
    g.gen++;
    g.jobsPending = 0;
    if (!g.fullPending) StartMeasure();
    Invalidate();
}

static void RebuildTypography() {
    g.typo.Release();
    TypographyOptions(g.typo);
    g.typo.Init(g.dwf);
    for (auto& kv : g.numLayouts) SafeRelease(kv.second);
    g.numLayouts.clear();
    Relayout();
}

static void BroadcastSettings() {
    if (!g_msgSettings) g_msgSettings = RegisterWindowMessageW(L"FastMD.SettingsChanged");
    for (HWND h = FindWindowExW(nullptr, nullptr, kClass, nullptr); h; h = FindWindowExW(nullptr, h, kClass, nullptr))
        if (h != g.hwnd) PostMessageW(h, g_msgSettings, 0, 0);
}

void ApplySettings(uint32_t changed, bool persist) {
    if (changed & SC_LANGUAGE) SetUiLanguage(ResolveLanguage());
    if (changed & SC_THEME) ApplyTheme();
    if (g.ready) {
        if (changed & SC_TYPE) RebuildTypography();
        else if (changed & SC_COLUMN) Relayout();
    }
    if (persist && !BenchActive()) {
        SaveConfig(g.cfg, g.findQuery);
        BroadcastSettings();
    }
    FindRelayoutInput();
    SettingsRefresh();
    Invalidate();
}

static void OnSettingsBroadcast() {  // another window changed the settings: take the shared ones (not zoom / outline)
    Config c = g.cfg;
    LoadConfig(c, nullptr);
    uint32_t changed = 0;  // redo only what changed: a theme click elsewhere must not re-lay out this document
    if (c.theme != g.cfg.theme) changed |= SC_THEME;
    if (c.font != g.cfg.font || c.fontSize != g.cfg.fontSize || c.wrapCode != g.cfg.wrapCode) changed |= SC_TYPE;
    if (c.column != g.cfg.column) changed |= SC_COLUMN;
    if (c.language != g.cfg.language) changed |= SC_LANGUAGE;
    if (c.smoothScroll != g.cfg.smoothScroll || c.editor != g.cfg.editor) changed |= SC_OTHER;
    g.cfg.theme = c.theme;
    g.cfg.column = c.column;
    g.cfg.wrapCode = c.wrapCode;
    g.cfg.font = c.font;
    g.cfg.fontSize = c.fontSize;
    g.cfg.smoothScroll = c.smoothScroll;
    g.cfg.language = c.language;
    g.cfg.editor = c.editor;
    if (changed) ApplySettings(changed, false);
}

// ------------------------------------------------------------------------------------------------ scrolling / zoom / column
void ScrollTo(float y, bool animate) {
    g.targetY = std::clamp(y, 0.f, MaxScroll());
    if (!animate || !g.cfg.smoothScroll) { g.scrollY = g.targetY; g.animating = false; Invalidate(); }
    else g.animating = true;
}

void UserScrollTo(float y, bool animate) {
    g.userMoved = true;
    g.restoreBlock = -1;
    ScrollTo(y, animate);
}

static void SetZoom(float z) {
    z = std::clamp(z, kZoomSteps[0], kZoomSteps[std::size(kZoomSteps) - 1]);
    if (std::fabs(z - g.cfg.zoom) < 0.001f) return;
    g.cfg.zoom = z;
    g.canvas->SetScale(Scale());
    Relayout();
    FindRelayoutInput();
    ShowToast(std::to_wstring((int)std::lround(z * 100)) + L" %", 900);
}

static void ZoomStep(int dir) {
    float z = g.cfg.zoom;
    if (dir > 0) { for (float s : kZoomSteps) if (s > z + 0.001f) { SetZoom(s); return; } }
    else { for (int i = (int)std::size(kZoomSteps) - 1; i >= 0; i--) if (kZoomSteps[i] < z - 0.001f) { SetZoom(kZoomSteps[i]); return; } }
}

static void SetColumn(int preset, bool toast) {
    preset = std::clamp(preset, (int)COL_NARROW, (int)COL_FULL);
    static const StrId names[] = {S_COL_NARROW, S_COL_NORMAL, S_COL_WIDE, S_COL_FULL};
    if (toast) ShowToast(std::wstring(Tr(S_COLUMN_TOAST)) + Tr(names[preset]), 1000);
    if (preset == g.cfg.column) return;
    g.cfg.column = (uint8_t)preset;
    ApplySettings(SC_COLUMN, true);
}

// ------------------------------------------------------------------------------------------------ commands
static void CopySelection() {
    std::wstring t = SelectionText();
    if (t.empty()) return;
    CopyToClipboard(t);
    ShowToast(Tr(S_COPIED), 900);
}

static int FirstVisibleImage() {  // automation: the image commands without a context menu target
    size_t n = g.doc.blocks.size();
    for (uint32_t i = FirstVisible(g.scrollY); i < n && g.Y[i] < g.scrollY + ViewH(); i++)
        if (g.doc.blocks[i].kind == BK_IMAGE) return (int)i;
    return -1;
}

void Command(UINT id) {
    int li = g.ctxLink >= 0 ? g.ctxLink : g.focusLink;
    switch (id) {
    case CMD_COPY: CopySelection(); break;
    case CMD_SELECT_ALL: SelectAll(); Invalidate(); break;
    case CMD_OPEN: OpenDialog(); break;
    case CMD_RELOAD: ReloadDocument(); ShowToast(Tr(S_RELOADED), 700); break;
    case CMD_EDIT: OpenInEditor(); break;
    case CMD_FOLDER: ShowInFolder(); break;
    case CMD_FIND: FindOpen(); break;
    case CMD_THEME_SYSTEM: case CMD_THEME_LIGHT: case CMD_THEME_DARK:
        g.cfg.theme = (ThemeMode)(id - CMD_THEME_SYSTEM);
        ApplySettings(SC_THEME, true);
        break;
    case CMD_ZOOM_IN: ZoomStep(1); break;
    case CMD_ZOOM_OUT: ZoomStep(-1); break;
    case CMD_ZOOM_RESET: SetZoom(1.f); break;
    case CMD_BACK: NavigateBack(); break;
    case CMD_FORWARD: NavigateForward(); break;
    case CMD_LINK_OPEN: OpenLink(li); break;
    case CMD_LINK_COPY:
        if (li >= 0 && (size_t)li < g.doc.links.size()) {
            CopyToClipboard(g.doc.links[li]);
            ShowToast(Tr(S_LINK_COPIED), 900);
        }
        break;
    case CMD_IMG_COPY: {
        int bi = g.ctxImage >= 0 ? g.ctxImage : FirstVisibleImage();
        if (bi >= 0 && CopyImageToClipboard((uint32_t)bi)) ShowToast(Tr(S_IMG_COPIED), 900);
        break;
    }
    case CMD_IMG_OPEN: {
        int bi = g.ctxImage >= 0 ? g.ctxImage : FirstVisibleImage();
        if (bi >= 0) OpenImageFile((uint32_t)bi);
        break;
    }
    case CMD_ASSOCIATE:
        if (RegisterAssociation(true)) ShowToast(Tr(S_ASSOC_OK), 5000);
        else ShowToast(Tr(S_ASSOC_FAIL), 3000);
        break;
    case CMD_TOC: if (TocAvailable()) TocSetOpen(!g.tocOpen); break;
    case CMD_COL_NARROW: case CMD_COL_NORMAL: case CMD_COL_WIDE: case CMD_COL_FULL: SetColumn(id - CMD_COL_NARROW, false); break;
    case CMD_COL_NARROWER: SetColumn(g.cfg.column - 1, true); break;
    case CMD_COL_WIDER: SetColumn(g.cfg.column + 1, true); break;
    case CMD_WRAP:
        g.cfg.wrapCode = !g.cfg.wrapCode;
        ApplySettings(SC_TYPE, true);
        ShowToast(Tr(g.cfg.wrapCode ? S_WRAP_ON : S_WRAP_OFF), 1000);
        break;
    case CMD_SETTINGS: SettingsOpen(); break;
    case CMD_FIND_CASE: if (!g.findOpen) FindOpen(); FindToggleCase(); break;
    case CMD_FIND_WORD: if (!g.findOpen) FindOpen(); FindToggleWord(); break;
    case CMD_FIND_NEXT: FindStep(1); break;
    case CMD_FIND_PREV: FindStep(-1); break;
    case CMD_FIND_CLOSE: FindClose(); break;
    case CMD_LINK_NEXT: FocusLinkStep(1); break;
    case CMD_LINK_PREV: FocusLinkStep(-1); break;
    }
}

static void ContextMenu(int sx, int sy, bool keyboard) {
    POINT cp{sx, sy};
    ScreenToClient(g.hwnd, &cp);
    float x = cp.x / Scale(), y = cp.y / Scale();
    g.ctxLink = keyboard ? g.focusLink : LinkAt(x, y);
    g.ctxImage = keyboard ? -1 : ImageAt(x, y);
    HMENU m = CreatePopupMenu(), theme = CreatePopupMenu(), zoom = CreatePopupMenu(), column = CreatePopupMenu();
    UINT hasDoc = g.path.empty() ? MF_GRAYED : 0;
    if (g.ctxLink >= 0) {
        AppendMenuW(m, MF_STRING, CMD_LINK_OPEN, Tr(S_LINK_OPEN));
        AppendMenuW(m, MF_STRING, CMD_LINK_COPY, Tr(S_LINK_COPY));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    if (g.ctxImage >= 0) {
        const Image& im0 = g.doc.images[g.doc.blocks[g.ctxImage].aux];
        const Image& im = im0.canon >= 0 ? g.doc.images[im0.canon] : im0;
        AppendMenuW(m, MF_STRING | (im.state.load() == 2 ? 0 : MF_GRAYED), CMD_IMG_COPY, Tr(S_IMG_COPY));
        AppendMenuW(m, MF_STRING | (im0.path.empty() ? MF_GRAYED : 0), CMD_IMG_OPEN, Tr(S_IMG_OPEN));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING | (HasSelection() ? 0 : MF_GRAYED), CMD_COPY, Tr(S_MENU_COPY));
    AppendMenuW(m, MF_STRING | hasDoc, CMD_SELECT_ALL, Tr(S_MENU_SELECT_ALL));
    AppendMenuW(m, MF_STRING | hasDoc, CMD_FIND, Tr(S_MENU_FIND));
    AppendMenuW(m, MF_STRING | (TocAvailable() ? 0 : MF_GRAYED) | (g.tocOpen ? MF_CHECKED : 0), CMD_TOC, Tr(S_MENU_TOC));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g.back.empty() ? MF_GRAYED : 0), CMD_BACK, Tr(S_MENU_BACK));
    AppendMenuW(m, MF_STRING | (g.fwd.empty() ? MF_GRAYED : 0), CMD_FORWARD, Tr(S_MENU_FORWARD));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_OPEN, Tr(S_MENU_OPEN));
    AppendMenuW(m, MF_STRING | hasDoc, CMD_RELOAD, Tr(S_MENU_RELOAD));
    AppendMenuW(m, MF_STRING | hasDoc, CMD_EDIT, Tr(S_MENU_EDIT));
    AppendMenuW(m, MF_STRING | hasDoc, CMD_FOLDER, Tr(S_MENU_FOLDER));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(theme, MF_STRING | (g.cfg.theme == TM_SYSTEM ? MF_CHECKED : 0), CMD_THEME_SYSTEM, Tr(S_THEME_SYSTEM));
    AppendMenuW(theme, MF_STRING | (g.cfg.theme == TM_LIGHT ? MF_CHECKED : 0), CMD_THEME_LIGHT, Tr(S_THEME_LIGHT));
    AppendMenuW(theme, MF_STRING | (g.cfg.theme == TM_DARK ? MF_CHECKED : 0), CMD_THEME_DARK, Tr(S_THEME_DARK));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)theme, Tr(S_MENU_THEME));
    AppendMenuW(zoom, MF_STRING, CMD_ZOOM_IN, Tr(S_ZOOM_IN));
    AppendMenuW(zoom, MF_STRING, CMD_ZOOM_OUT, Tr(S_ZOOM_OUT));
    AppendMenuW(zoom, MF_STRING, CMD_ZOOM_RESET, Tr(S_ZOOM_RESET));
    wchar_t zl[64];
    swprintf_s(zl, Tr(S_MENU_ZOOM_FMT), (int)std::lround(g.cfg.zoom * 100));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)zoom, zl);
    static const StrId cols[] = {S_COL_NARROW, S_COL_NORMAL, S_COL_WIDE, S_COL_FULL};
    for (int k = 0; k < 4; k++)
        AppendMenuW(column, MF_STRING | (g.cfg.column == k ? MF_CHECKED : 0), CMD_COL_NARROW + k, Tr(cols[k]));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)column, Tr(S_MENU_COLUMN));
    AppendMenuW(m, MF_STRING | (g.cfg.wrapCode ? MF_CHECKED : 0), CMD_WRAP, Tr(S_MENU_WRAP));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_SETTINGS, Tr(S_MENU_SETTINGS));
    AppendMenuW(m, MF_STRING, CMD_ASSOCIATE, Tr(S_MENU_ASSOCIATE));
    UINT id = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, g.hwnd, nullptr);
    DestroyMenu(m);  // destroys the submenus too
    if (id) Command(id);
    g.ctxLink = g.ctxImage = -1;
}

// ------------------------------------------------------------------------------------------------ paint
static void WaitReady() {
    if (g.ready) return;
    if (g.docThread) {
        WaitForSingleObject(g.docThread, INFINITE);
        CloseHandle(g.docThread);
        g.docThread = nullptr;
    }
    // the doc thread laid out for a guessed size / DPI; fix it if the real window differs
    UINT wdpi = GetDpiForWindow(g.hwnd);
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    if (rc.right != g.pxW || rc.bottom != g.pxH || (float)wdpi != g.dpi) {
        g.dpi = (float)wdpi;
        g.pxW = rc.right;
        g.pxH = rc.bottom;
        g.canvas->SetScale(Scale());
        g.canvas->Resize(g.pxW, g.pxH);
        g.offscreenValid = false;
        float tw = g.textW, ww = g.wideW;
        UpdateColumns();
        if (std::fabs(tw - g.textW) > 0.1f || std::fabs(ww - g.wideW) > 0.1f) {
            ClearLayoutCache();
            InitGeometry();
            RecomputeY();
        }
    }
    g.ready = true;
}

static void Present(HDC hdc) {
    if (!g.offscreenValid) Render();
    ScheduleImageScaling();  // a picture was drawn at a size we have no display-size copy of
    g.offscreenValid = false;
    HDC dst = hdc ? hdc : GetDC(g.hwnd);
    BitBlt(dst, 0, 0, g.pxW, g.pxH, g.canvas->DC(), 0, 0, SRCCOPY);
    if (!hdc) ReleaseDC(g.hwnd, dst);
}

static void ScrollTest() {  // steady-state cost of a scrolling frame, logged to %TEMP%\fastmd-scroll.txt
    for (DWORD start = GetTickCount(); GetTickCount() - start < 1200;) {  // let the background work settle first:
        MSG msg;                                                          // heights measured, images decoded and scaled
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(1);
    }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    std::vector<double> ms;
    for (int i = 0; i < g.cfg.scrollTest; i++) {
        g.scrollY += 13.f;
        if (g.scrollY > MaxScroll()) g.scrollY = 0;
        QueryPerformanceCounter(&t0);
        Present(nullptr);
        QueryPerformanceCounter(&t1);
        DwmFlush();
        ms.push_back((t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    }
    std::sort(ms.begin(), ms.end());
    wchar_t p[MAX_PATH];
    GetTempPathW(MAX_PATH, p);
    wcscat_s(p, L"fastmd-scroll.txt");
    FILE* fo = nullptr;
    if (!_wfopen_s(&fo, p, L"ab") && fo && !ms.empty()) {
        fprintf(fo, "%dx%d px blocks=%u render+blit p50=%.2f p95=%.2f max=%.2f ms\n", g.pxW, g.pxH,
                (unsigned)g.doc.blocks.size(), ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back());
        fclose(fo);
    }
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

static void AfterFirstFrame() {
    g.firstFrame = false;
    Invalidate();  // the icon buttons (settings, outline, its close icon) were left out of the first frame
    StartBackgroundWork();
    DragAcceptFiles(g.hwnd, TRUE);
    if (!g.path.empty()) SHAddToRecentDocs(SHARD_PATHW, g.path.c_str());
    if (!g_msgSettings) g_msgSettings = RegisterWindowMessageW(L"FastMD.SettingsChanged");
    LoadPositionsAsync();  // reading position of this document; marks it as recently opened
    DebugFlush();
    if (g.cfg.scrollTest) ScrollTest();
}

static void OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(g.hwnd, &ps);
    BenchWindowShown();
    WaitReady();
    Present(hdc);
    EndPaint(g.hwnd, &ps);
    if (g.firstFrame) {
        Mark("first_present");
        char notes[96];
        sprintf_s(notes, "dwrite-gdi blocks=%u laid_out=%u", (unsigned)g.doc.blocks.size(), g.cachedCount);
        if (BenchContentPresented(notes)) {  // bench mode: t_content written, exit
            g.closing = true;
            PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
            return;
        }
        AfterFirstFrame();
    }
}

static void Frame() {  // animation frame outside WM_PAINT, paced by DWM
    Present(nullptr);
    ValidateRect(g.hwnd, nullptr);
    DwmFlush();
}

// ------------------------------------------------------------------------------------------------ mouse
static bool InScrollbar(float x) { return !g.path.empty() && g.docH > ViewH() + 1 && x >= ViewW() - 14; }
static float ThumbTop(float* th) {
    float trackH = ViewH() - 4;
    *th = std::max(32.f, trackH * ViewH() / g.docH);
    return 2 + (trackH - *th) * (MaxScroll() > 0 ? g.scrollY / MaxScroll() : 0);
}

static void UpdateSelectionTo(float x, float y) {
    uint32_t pos;
    if (HitTestDoc(x, y, &pos, nullptr) && pos != g.selFocus) {
        g.selFocus = pos;
        Invalidate();
    }
}

static const wchar_t* FindTip(int part) {
    switch (part) {
    case FP_CASE: return Tr(S_FIND_CASE_TIP);
    case FP_WORD: return Tr(S_FIND_WORD_TIP);
    case FP_PREV: return Tr(S_FIND_PREV_TIP);
    case FP_NEXT: return Tr(S_FIND_NEXT_TIP);
    case FP_CLOSE: return Tr(S_FIND_CLOSE_TIP);
    default: return L"";
    }
}

static void OnMouseMove(int mx, int my) {
    float x = mx / Scale(), y = my / Scale();
    if (g.draggingThumb) {
        float th;
        ThumbTop(&th);
        float trackH = ViewH() - 4 - th;
        UserScrollTo(trackH > 0 ? (y - g.dragGrab - 2) / trackH * MaxScroll() : 0, false);
        return;
    }
    if (g.dragHBlock >= 0) {
        float l, t, r, b, tl, tr, vx, vw, cw;
        if (HScrollBarRect(g.dragHBlock, &l, &t, &r, &b, &tl, &tr) && HScrollInfo(g.dragHBlock, &vx, &vw, &cw)) {
            float track = (r - l) - (tr - tl);
            HScrollSet(g.dragHBlock, track > 0 ? (x - g.dragHGrab - l) / track * (cw - vw) : 0.f);
        }
        return;
    }
    if (g.selecting) {
        bool outside = y < 0 || y > ViewH();
        float vx, vw, cw;
        int hb = (int)BlockOfPos(g.selFocus);
        if (!outside && HScrollInfo(hb, &vx, &vw, &cw)) outside = x < vx || x > vx + vw;
        if (outside) SetTimer(g.hwnd, TIMER_AUTOSCROLL, 16, nullptr);
        else KillTimer(g.hwnd, TIMER_AUTOSCROLL);
        UpdateSelectionTo(x, std::clamp(y, 0.f, ViewH()));
        return;
    }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g.hwnd, 0};
    TrackMouseEvent(&tme);
    // overlays first: find bar, outline panel, outline button, settings button
    int fpart = FindPartAt(x, y);
    int tocItem = -1;
    bool inToc = fpart == FP_NONE && TocHit(x, y, &tocItem);
    bool tocBtn = fpart == FP_NONE && !inToc && TocButtonHit(x, y);
    bool gear = fpart == FP_NONE && !inToc && !tocBtn && SettingsButtonHit(x, y);
    bool overlay = fpart != FP_NONE || inToc || tocBtn || gear;
    int home = g.path.empty() ? HomeItemAt(x, y) : -1;
    bool hot = !overlay && InScrollbar(x);
    bool onHBar = false, onBtn = false;
    int hblock = (overlay || hot) ? -1 : HScrollBlockAt(x, y, &onHBar);
    int link = (overlay || hot) ? -1 : LinkAt(x, y);
    int code = (overlay || hot) ? -1 : CodeBlockAt(x, y, &onBtn);
    std::wstring tip = tocBtn ? std::wstring(Tr(S_TOC_BUTTON_TIP))
                     : gear   ? std::wstring(Tr(S_SETTINGS_BUTTON_TIP))
                              : std::wstring(FindTip(fpart));
    if (fpart != g.findHot || tocItem != g.tocHover || tocBtn != g.tocBtnHot || gear != g.settingsBtnHot ||
        hot != g.hotScroll || link != g.hoverLink || code != g.hoverCode || onBtn != g.hoverCopyBtn ||
        hblock != g.hoverHBlock || onHBar != g.hotHBar || home != g.recentHover || tip != g.tip) {
        g.findHot = fpart;
        g.tocHover = tocItem;
        g.tocBtnHot = tocBtn;
        g.settingsBtnHot = gear;
        g.hotScroll = hot;
        g.hoverLink = link;
        g.hoverCode = code;
        g.hoverCopyBtn = onBtn;
        g.hoverHBlock = hblock;
        g.hotHBar = onHBar;
        g.recentHover = home;
        g.tip = tip;
        Invalidate();
    }
    LPCWSTR cur = IDC_ARROW;
    if (link >= 0 || onBtn || tocBtn || gear || tocItem >= 0 || tocItem == -2 || home >= 0 ||
        (fpart != FP_NONE && fpart != FP_BAR && fpart != FP_FIELD && fpart != FP_COUNT))
        cur = IDC_HAND;
    else if (fpart == FP_FIELD) cur = IDC_IBEAM;
    else if (!overlay && !hot && !onHBar && !g.path.empty()) {
        uint32_t pos;
        bool inside = false;
        if (HitTestDoc(x, y, &pos, &inside) && inside) cur = IDC_IBEAM;
    }
    SetCursor(LoadCursorW(nullptr, cur));
}

static void OnLButtonDown(int mx, int my, WPARAM keys) {
    float x = mx / Scale(), y = my / Scale();
    g.downX = mx;
    g.downY = my;
    int fpart = FindPartAt(x, y);
    if (fpart != FP_NONE) { FindClick(fpart); return; }
    int item;
    if (TocHit(x, y, &item)) { TocClick(item); return; }
    if (TocButtonHit(x, y)) { TocSetOpen(true); return; }
    if (SettingsButtonHit(x, y)) { SettingsOpen(); return; }
    if (TocOverlayOpen()) { TocSetOpen(false); return; }  // a click beside the drawer closes it
    if (g.path.empty()) {
        int k = HomeItemAt(x, y);
        if (k >= 0) HomeOpen(k);
        return;
    }
    SetCapture(g.hwnd);
    if (FindInputFocused()) SetFocus(g.hwnd);  // the document takes the keyboard back (Ctrl+C copies its selection)
    if (g.focusLink >= 0) { g.focusLink = -1; Invalidate(); }
    if (InScrollbar(x)) {
        float th, tt = ThumbTop(&th);
        if (y >= tt && y < tt + th) { g.draggingThumb = true; g.dragGrab = y - tt; }
        else UserScrollTo(g.targetY + (y < tt ? -1 : 1) * (ViewH() - 48), true);
        g.userMoved = true;
        Invalidate();
        return;
    }
    bool onHBar = false;
    int hb = HScrollBlockAt(x, y, &onHBar);
    if (hb >= 0 && onHBar) {
        float l, t, r, b, tl, tr, vx, vw, cw;
        if (HScrollBarRect(hb, &l, &t, &r, &b, &tl, &tr) && HScrollInfo(hb, &vx, &vw, &cw)) {
            if (x >= tl && x <= tr) { g.dragHBlock = hb; g.dragHGrab = x - tl; }
            else HScrollSet(hb, HScrollOf(hb) + (x < tl ? -1.f : 1.f) * vw * 0.8f);
        }
        Invalidate();
        return;
    }
    bool onBtn = false;
    int code = CodeBlockAt(x, y, &onBtn);
    if (onBtn && code >= 0) {
        CopyToClipboard(BlockPlainText((uint32_t)code));
        ShowToast(Tr(S_CODE_COPIED), 900);
        return;
    }
    g.downOnLink = LinkAt(x, y) >= 0;
    DWORD now = GetMessageTime();
    bool isNear = std::abs(mx - g.lastClickPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                  std::abs(my - g.lastClickPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
    g.clickCount = (isNear && now - g.lastClickTime <= GetDoubleClickTime()) ? g.clickCount % 3 + 1 : 1;
    g.lastClickTime = now;
    g.lastClickPt = POINT{mx, my};
    uint32_t pos;
    if (!HitTestDoc(x, y, &pos, nullptr)) return;
    if (g.clickCount == 2) SelectWordAt(pos);
    else if (g.clickCount == 3) SelectBlockAt(pos);
    else {
        if (!(keys & MK_SHIFT)) g.selAnchor = pos;
        g.selFocus = pos;
        g.selecting = !g.downOnLink || (keys & MK_SHIFT);
    }
    if (g.clickCount == 1 && g.downOnLink) g.selecting = true;  // a drag from a link selects; a click opens it
    Invalidate();
}

static void OnLButtonUp(int mx, int my) {
    ReleaseCapture();
    KillTimer(g.hwnd, TIMER_AUTOSCROLL);
    if (g.draggingThumb) { g.draggingThumb = false; Invalidate(); return; }
    if (g.dragHBlock >= 0) { g.dragHBlock = -1; Invalidate(); return; }
    g.selecting = false;
    int dx = mx - g.downX, dy = my - g.downY;
    if (g.downOnLink && dx * dx + dy * dy < 16 && g.clickCount == 1) {
        int li = LinkAt(mx / Scale(), my / Scale());
        g.selAnchor = g.selFocus;  // a click is not a selection
        g.downOnLink = false;
        if (li >= 0) OpenLink(li);
        Invalidate();
        return;
    }
    g.downOnLink = false;
    Invalidate();
}

static float WheelStep() {
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    return lines == WHEEL_PAGESCROLL ? ViewH() - 56 : 38.f * std::max(1u, lines);
}

static void OnWheel(int delta, WORD keys, int sx, int sy, bool horizontal) {
    POINT p{sx, sy};
    ScreenToClient(g.hwnd, &p);
    float x = p.x / Scale(), y = p.y / Scale(), notches = (float)delta / WHEEL_DELTA;
    if (!horizontal && (keys & MK_CONTROL)) { ZoomStep(delta > 0 ? 1 : -1); return; }
    int item;
    if (!horizontal && TocHit(x, y, &item)) { TocWheel(-notches * 84.f); return; }
    if (horizontal || (keys & MK_SHIFT)) {
        bool bar;
        int hb = HScrollBlockAt(x, y, &bar);
        if (hb >= 0) {
            HScrollSet((uint32_t)hb, HScrollOf((uint32_t)hb) + (horizontal ? notches : -notches) * WheelStep());
            return;
        }
        if (horizontal) return;
    }
    if (g.path.empty()) return;
    UserScrollTo(g.targetY - notches * WheelStep(), true);
}

// ------------------------------------------------------------------------------------------------ keyboard
bool KeyCommand(WPARAM vk, bool ctrl, bool shift, bool alt) {
    float vh = ViewH();
    if (ctrl && alt) {
        if (vk == VK_LEFT) { Command(CMD_COL_NARROWER); return true; }
        if (vk == VK_RIGHT) { Command(CMD_COL_WIDER); return true; }
        return false;
    }
    if (alt) {
        if (vk == VK_LEFT) { NavigateBack(); return true; }
        if (vk == VK_RIGHT) { NavigateForward(); return true; }
        return false;
    }
    if (ctrl) {
        switch (vk) {
        case 'C': case VK_INSERT: CopySelection(); return true;
        case 'A': if (!g.findOpen) { SelectAll(); Invalidate(); } return true;
        case 'F': FindOpen(); return true;
        case 'O': Command(shift ? CMD_TOC : CMD_OPEN); return true;
        case 'E': OpenInEditor(); return true;
        case 'R': Command(CMD_RELOAD); return true;
        case 'W': PostMessageW(g.hwnd, WM_CLOSE, 0, 0); return true;
        case VK_OEM_PLUS: case VK_ADD: ZoomStep(1); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT: ZoomStep(-1); return true;
        case '0': case VK_NUMPAD0: SetZoom(1.f); return true;
        case VK_OEM_COMMA: SettingsOpen(); return true;
        case VK_HOME: UserScrollTo(0, true); return true;
        case VK_END: UserScrollTo(MaxScroll(), true); return true;
        }
        return false;
    }
    if (g.path.empty()) return vk == VK_F5;
    switch (vk) {
    case VK_DOWN: UserScrollTo(g.targetY + 56, true); return true;
    case VK_UP: UserScrollTo(g.targetY - 56, true); return true;
    case VK_NEXT: UserScrollTo(g.targetY + vh - 56, true); return true;
    case VK_PRIOR: UserScrollTo(g.targetY - vh + 56, true); return true;
    case VK_SPACE: UserScrollTo(g.targetY + (shift ? -1 : 1) * (vh - 56), true); return true;
    case VK_HOME: UserScrollTo(0, true); return true;
    case VK_END: UserScrollTo(MaxScroll(), true); return true;
    case VK_F3:
        if (!g.findOpen) FindOpen();
        else FindStep(shift ? -1 : 1);
        return true;
    case VK_F5: Command(CMD_RELOAD); return true;
    }
    return false;
}

static bool OnKeyDown(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (g.path.empty() && !ctrl && !alt && HomeKey(vk)) return true;
    if (!ctrl && !alt) {
        switch (vk) {
        case VK_ESCAPE:
            if (g.findOpen) FindClose();
            else if (TocOverlayOpen()) TocSetOpen(false);
            else if (g.focusLink >= 0) { g.focusLink = -1; Invalidate(); }
            else if (HasSelection()) { g.selAnchor = g.selFocus; Invalidate(); }
            else PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
            return true;
        case VK_TAB:
            if (!g.path.empty()) FocusLinkStep(shift ? -1 : 1);
            return true;
        case VK_RETURN:
            if (g.focusLink >= 0) { OpenLink(g.focusLink); return true; }
            if (g.findOpen) { FindStep(shift ? -1 : 1); return true; }
            return false;
        case VK_BACK:
            if (!g.findOpen) NavigateBack();  // with the find bar open, Backspace edits the query (WM_CHAR)
            return true;
        case VK_SPACE:
            if (g.findOpen) return true;  // typed into the find box (WM_CHAR), must not page down
            break;
        }
    }
    return KeyCommand(vk, ctrl, shift, alt);
}

// ------------------------------------------------------------------------------------------------ automation queries
static LRESULT Query(WPARAM q, LPARAM lp) {
    float s = Scale();
    size_t n = g.doc.blocks.size();
    switch (q) {
    case Q_SCROLLY: return std::lround(g.scrollY);
    case Q_TARGETY: return std::lround(g.targetY);
    case Q_DOCH: return std::lround(g.docH);
    case Q_TOC_OPEN: return g.tocOpen;
    case Q_TOC_DOCKED: return TocDocked();
    case Q_TOC_COUNT: TocSync(); return (LRESULT)g.toc.size();
    case Q_TOC_CURRENT: return TocCurrent();
    case Q_TOC_ITEM_Y: return std::lround(TocItemY((int)lp) * s);
    case Q_HSCROLL_BLOCK:
        for (uint32_t i = 0; i < n; i++) {
            float vx, vw, cw;
            if (HScrollInfo(i, &vx, &vw, &cw)) return i;
        }
        return -1;
    case Q_HSCROLL_X: return (lp >= 0 && (size_t)lp < n) ? std::lround(HScrollOf((uint32_t)lp)) : -1;
    case Q_FOCUS_LINK: return g.focusLink;
    case Q_MATCHES: return (LRESULT)g.matches.size();
    case Q_CUR_MATCH: return g.curMatch;
    case Q_TEXT_LEFT: return std::lround(TextLeft() * s);
    case Q_TEXT_W: return std::lround(g.textW * s);
    case Q_RECENT_COUNT: return (LRESULT)g.recentShown.size();
    case Q_FIND_EDIT: return (LRESULT)FindEditHwnd();
    case Q_SETTINGS_HWND: return (LRESULT)SettingsHwnd();
    case Q_FIND_OPEN: return g.findOpen;
    case Q_COLUMN: return g.cfg.column;
    case Q_FONT_SIZE: return g.cfg.fontSize;
    case Q_WRAP: return g.cfg.wrapCode;
    case Q_LANG: return UiLanguage();
    case Q_FIND_PART_X: {
        float l, t, r, b;
        FindPartRect((int)lp, &l, &t, &r, &b);
        return MAKELONG(std::lround((l + r) * 0.5f * s), std::lround((t + b) * 0.5f * s));
    }
    case Q_BLOCK_Y: return (lp >= 0 && (size_t)lp < n) ? std::lround((g.Y[lp] - g.scrollY) * s) : INT_MIN;
    case Q_RESTORED: return g.restored;
    case Q_THEME_DARK: return PaletteIsDark();
    case Q_SETTINGS_HIT: return SettingsHitCenter((int)lp);
    case Q_FONT_FAMILY_SITKA: return wcsncmp(g.typo.family[R_BODY], L"Sitka", 5) == 0;
    case Q_SETTINGS_BTN: {
        float l, t, r, b;
        if (!SettingsButtonRect(&l, &t, &r, &b)) return -1;
        return MAKELONG(std::lround((l + r) * 0.5f * s), std::lround((t + b) * 0.5f * s));
    }
    case Q_IMG_SCALED:
        for (const Image& im : g.doc.images)
            if (!im.sc.empty()) return im.scW.load();
        return 0;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------ window procedure
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgSettings && msg == g_msgSettings) {
        if (g.ready && !g.firstFrame) OnSettingsBroadcast();
        return 0;
    }
    switch (msg) {
    case WM_ERASEBKGND:
        if (!g.ready) { BenchWindowShown(); return DefWindowProcW(hwnd, msg, wp, lp); }  // theme-coloured class brush
        return 1;
    case WM_PAINT: OnPaint(); return 0;
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (!g.ready || (w == g.pxW && h == g.pxH) || w == 0 || h == 0) return 0;
        g.pxW = w;
        g.pxH = h;
        g.canvas->Resize(w, h);
        g.offscreenValid = false;
        float tw = g.textW, ww = g.wideW;
        UpdateColumns();
        if (std::fabs(tw - g.textW) > 0.1f || std::fabs(ww - g.wideW) > 0.1f) Relayout();
        g.scrollY = std::clamp(g.scrollY, 0.f, MaxScroll());
        g.targetY = std::clamp(g.targetY, 0.f, MaxScroll());
        FindRelayoutInput();
        Invalidate();
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = (float)HIWORD(wp);
        if (g.canvas) g.canvas->SetScale(Scale());
        RECT* r = (RECT*)lp;
        g.pxW = 0;  // force the WM_SIZE path (widths in DIP may change with rounding)
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = POINT{360, 240}; return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (!g.ready || g.firstFrame) return 0;
        OnWheel(GET_WHEEL_DELTA_WPARAM(wp), GET_KEYSTATE_WPARAM(wp), GET_X(lp), GET_Y(lp), msg == WM_MOUSEHWHEEL);
        return msg == WM_MOUSEHWHEEL ? TRUE : 0;
    case WM_KEYDOWN:
        if (!g.ready || g.firstFrame) return 0;
        if (OnKeyDown(wp)) return 0;
        break;
    case WM_SYSKEYDOWN: {
        if (!g.ready || g.firstFrame) break;
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (g.findOpen && !ctrl && wp == 'C') { FindToggleCase(); return 0; }
        if (g.findOpen && !ctrl && wp == 'W') { FindToggleWord(); return 0; }
        if (KeyCommand(wp, ctrl, shift, true)) return 0;
        break;
    }
    case WM_SYSCHAR:
        if (g.findOpen && (towlower((wint_t)wp) == L'c' || towlower((wint_t)wp) == L'w')) return 0;  // no menu beep
        break;
    case WM_CHAR:
        if (g.path.empty()) HomeChar((wchar_t)wp);
        else if (g.findOpen) FindTypeChar((wchar_t)wp);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) g_findHadFocus = FindInputFocused();
        else if (g.findOpen && g_findHadFocus) { FindFocusInput(); return 0; }
        break;
    case WM_LBUTTONDOWN:
        if (!g.ready || g.firstFrame) return 0;
        OnLButtonDown(GET_X(lp), GET_Y(lp), wp);
        return 0;
    case WM_MOUSEMOVE:
        if (!g.ready || g.firstFrame) return 0;
        OnMouseMove(GET_X(lp), GET_Y(lp));
        return 0;
    case WM_MOUSELEAVE:
        if (g.hotScroll || g.hoverLink >= 0 || g.hoverCode >= 0 || g.hoverHBlock >= 0 || g.findHot != -1 ||
            g.tocHover != -1 || g.tocBtnHot || g.settingsBtnHot || g.recentHover >= 0 || !g.tip.empty()) {
            g.hotScroll = g.tocBtnHot = g.settingsBtnHot = g.hotHBar = false;
            g.hoverLink = g.hoverCode = g.hoverHBlock = g.recentHover = -1;
            g.findHot = g.tocHover = -1;
            g.hoverCopyBtn = false;
            g.tip.clear();
            Invalidate();
        }
        return 0;
    case WM_LBUTTONUP:
        if (!g.ready || g.firstFrame) return 0;
        OnLButtonUp(GET_X(lp), GET_Y(lp));
        return 0;
    case WM_XBUTTONUP:
        if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) NavigateBack();
        else NavigateForward();
        return TRUE;
    case WM_CONTEXTMENU: {
        if (!g.ready || g.firstFrame) return 0;
        int sx = GET_X(lp), sy = GET_Y(lp);
        bool keyboard = sx == -1 && sy == -1;
        if (keyboard) { POINT p{20, 20}; ClientToScreen(hwnd, &p); sx = p.x; sy = p.y; }
        ContextMenu(sx, sy, keyboard);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd) return TRUE;  // set in WM_MOUSEMOVE (children: their own)
        break;
    case WM_DROPFILES: {
        wchar_t file[MAX_PATH * 4];
        if (DragQueryFileW((HDROP)wp, 0, file, (UINT)std::size(file))) OpenDocument(file, true, 0, true);
        DragFinish((HDROP)wp);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_TOAST) { KillTimer(hwnd, TIMER_TOAST); Invalidate(); }
        else if (wp == TIMER_HBAR) { KillTimer(hwnd, TIMER_HBAR); Invalidate(); }
        else if (wp == TIMER_RELOAD) { KillTimer(hwnd, TIMER_RELOAD); ReloadDocument(); }
        else if (wp == TIMER_AUTOSCROLL && g.selecting) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            float x = p.x / Scale(), y = p.y / Scale();
            float d = y < 0 ? y : y > ViewH() ? y - ViewH() : 0;
            if (d != 0) ScrollTo(g.scrollY + std::clamp(d * 0.5f, -60.f, 60.f), false);
            uint32_t hb = BlockOfPos(g.selFocus);  // selecting inside a wide block: scroll it sideways
            float vx, vw, cw;
            if (HScrollInfo(hb, &vx, &vw, &cw)) {
                float dx = x < vx ? x - vx : x > vx + vw ? x - (vx + vw) : 0;
                if (dx != 0) HScrollSet(hb, HScrollOf(hb) + std::clamp(dx * 0.5f, -40.f, 40.f));
            }
            UpdateSelectionTo(x, std::clamp(y, 0.f, ViewH()));
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) ApplyTheme();
        return 0;
    case WM_COMMAND:  // menu ids; also lets tests and automation drive the viewer (tests/ui_smoke.py)
        if (g.ready && !g.firstFrame) Command(LOWORD(wp));
        return 0;
    case WM_APP_QUERY: return (g.ready && !g.firstFrame) ? Query(wp, lp) : 0;
    case WM_APP_MEASURED: OnMeasured((MeasureJob*)lp); return 0;
    case WM_APP_FULLDOC: if (!g.firstFrame) OnFullDoc(); return 0;
    case WM_APP_IMAGES: OnImagesLoaded(); return 0;
    case WM_APP_SCALED: OnScaledImages((std::vector<ScaledImage>*)lp, (uint32_t)wp); return 0;
    case WM_APP_FILECHANGED: SetTimer(hwnd, TIMER_RELOAD, 120, nullptr); return 0;  // debounce editor save bursts
    case WM_APP_POSITIONS: OnPositionsLoaded((std::vector<PosEntry>*)lp); return 0;
    case WM_APP_FINDINPUT: if (g.ready) FindOnInput(wp, lp); return 0;
    case WM_CLOSE:
        g.closing = true;
        if (HWND s = SettingsHwnd()) DestroyWindow(s);
        SaveAll();
        DestroyWindow(hwnd);
        return 0;
    case WM_ENDSESSION:  // logoff / shutdown / restart: no WM_CLOSE is sent
        if (wp) SaveAll();
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------------------------------------ entry
static std::vector<std::wstring> SplitArgs(const wchar_t* cl) {  // CommandLineToArgvW without loading shell32
    std::vector<std::wstring> out;
    const wchar_t* p = cl;
    while (*p) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        std::wstring a;
        bool q = false;
        while (*p && (q || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'\\') {
                size_t bs = 0;
                while (*p == L'\\') { bs++; p++; }
                if (*p == L'"') { a.append(bs / 2, L'\\'); if (bs % 2) { a.push_back(L'"'); p++; } }
                else a.append(bs, L'\\');
                continue;
            }
            if (*p == L'"') { q = !q; p++; continue; }
            a.push_back(*p++);
        }
        out.push_back(a);
    }
    return out;
}

static int MessageLoop() {
    MSG msg;
    for (;;) {
        if (g.animating) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) goto done;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!g.animating || g.closing) continue;
            float d = g.targetY - g.scrollY;
            if (std::fabs(d) < 0.5f) { g.scrollY = g.targetY; g.animating = false; }
            else g.scrollY += d * 0.3f;
            Frame();
        } else {
            BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
done:
    g.closing = true;
    StopWatcher();
    ExitProcess(0);  // workers may still run (measure / images): no orderly teardown needed for a viewer
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    BenchInit();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g.inst = inst;

    std::wstring path;
    auto args = SplitArgs(GetCommandLineW());
    bool bench = BenchActive();
    if (!bench) {
        LoadConfig(g.cfg, &g.findQuery);
        LoadPlacement();
    }
    SetUiLanguage(ResolveLanguage());
    for (size_t i = 1; i < args.size(); i++) {
        const std::wstring& a = args[i];
        if (a == L"--register") return RegisterAssociation(true) ? 0 : 1;
        if (a == L"--unregister") { UnregisterAssociation(); return 0; }
        if (a.rfind(L"--id=", 0) == 0) wcsncpy_s(g.cfg.id, a.c_str() + 5, _TRUNCATE);
        else if (a == L"--light") g.cfg.theme = TM_LIGHT;
        else if (a == L"--dark") g.cfg.theme = TM_DARK;
        else if (a == L"--ime") g.cfg.noIme = false;
        else if (a == L"--anim") g.cfg.noAnim = false;
        else if (a.rfind(L"--zoom=", 0) == 0) g.cfg.zoom = std::clamp((float)_wtof(a.c_str() + 7) / 100.f, 0.5f, 3.f);
        else if (a.rfind(L"--size=", 0) == 0) {
            swscanf_s(a.c_str() + 7, L"%dx%d", &g.cfg.sizeW, &g.cfg.sizeH);
            g_sizeFromArgs = true;
        }
        else if (a.rfind(L"--scroll-test=", 0) == 0) g.cfg.scrollTest = _wtoi(a.c_str() + 14);
        else if (a.rfind(L"--", 0) != 0) path = a;
    }
    if (bench) { g.cfg.sizeW = 1000; g.cfg.sizeH = 800; }  // PROTOCOL.md §5: 1000×800 DIP
    SetDarkPalette(WantDark());
    TypographyOptions(g.typo);

    // geometry guess (the doc thread lays out for it before the window exists): the last closed window's client size
    bool restore = g_haveSaved && !g_sizeFromArgs && !bench;
    if (restore) {
        g.dpi = (float)g_saved.dpi;
        g.pxW = g_saved.clientW;
        g.pxH = g_saved.clientH;
    } else {
        g.dpi = (float)GetDpiForSystem();
        g.pxW = MulDiv(g.cfg.sizeW, (int)g.dpi, 96);
        g.pxH = MulDiv(g.cfg.sizeH, (int)g.dpi, 96);
    }
    g.tocOpen = g.cfg.tocOpen && TocWideEnough();  // the outline re-opens docked only (never as a drawer over the text)
    if (!path.empty()) {
        wchar_t full[MAX_PATH * 4];
        g.path = GetFullPathNameW(path.c_str(), MAX_PATH * 4, full, nullptr) ? full : path;
    }
    g.docThread = CreateThread(nullptr, 0, StartupDocThread, nullptr, 0, nullptr);

    if (g.cfg.noIme) ImmDisableIME(0);  // UI thread only: the find box runs on its own thread with IME
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    uint32_t bg = g_pal[P_BG];
    wc.hbrBackground = CreateSolidBrush(RGB(bg >> 16, (bg >> 8) & 255, bg & 255));
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    // position / size: the last closed window's placement (cascaded if another FastMD window sits there), created
    // directly at its final rect — no extra move/resize before the first frame
    RECT wr;
    bool placed = restore && RestoredRect(&wr);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w, h;
    if (placed) {
        x = wr.left;
        y = wr.top;
        w = wr.right - wr.left;
        h = wr.bottom - wr.top;
    } else {
        UINT sdpi = GetDpiForSystem();
        int cw = restore ? MulDiv(g_saved.clientW, (int)sdpi, (int)g_saved.dpi) : MulDiv(g.cfg.sizeW, (int)sdpi, 96);
        int ch = restore ? MulDiv(g_saved.clientH, (int)sdpi, (int)g_saved.dpi) : MulDiv(g.cfg.sizeH, (int)sdpi, 96);
        RECT r{0, 0, cw, ch};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, sdpi);
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
    std::wstring title = WindowTitle();
    g.hwnd = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr,
                             inst, nullptr);
    Mark("window_created");
    if (g.cfg.noAnim) {
        BOOL on = TRUE;  // content appears at full opacity immediately instead of the ~200 ms open animation
        DwmSetWindowAttribute(g.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on));
    }
    ApplyWindowChrome(g.hwnd);
    int cmd = show;
    bool plainShow = show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT || show == SW_SHOW;
    if (placed && g_saved.maximized && plainShow) cmd = SW_SHOWMAXIMIZED;
    ShowWindow(g.hwnd, cmd);
    Mark("shown");
    UpdateWindow(g.hwnd);
    return MessageLoop();
}
