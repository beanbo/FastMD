// Win32 window, input, commands, context menu, settings, .md association, entry point.
#include "app.h"
#include <commdlg.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>

#define GET_X(lp) ((int)(short)LOWORD(lp))
#define GET_Y(lp) ((int)(short)HIWORD(lp))

static const wchar_t kClass[] = L"FastMD.Document";
static const wchar_t kRegKey[] = L"Software\\FastMD";
static const float kZoomSteps[] = {0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.f, 1.1f, 1.25f, 1.5f, 1.75f, 2.f, 2.5f, 3.f};

enum Cmd : UINT {
    CMD_COPY = 100, CMD_SELECT_ALL, CMD_OPEN, CMD_RELOAD, CMD_EDIT, CMD_FOLDER, CMD_FIND,
    CMD_THEME_SYSTEM, CMD_THEME_LIGHT, CMD_THEME_DARK, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET,
    CMD_BACK, CMD_FORWARD, CMD_COPY_LINK, CMD_ASSOCIATE,
};

void Invalidate() {
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------------------------------------ settings
static DWORD RegGetDword(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return def;
    return v;
}
static void RegSetDword(const wchar_t* name, DWORD v) { RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &v, sizeof(v)); }

static void LoadSettings() {
    g.cfg.theme = (ThemeMode)std::min<DWORD>(RegGetDword(L"Theme", TM_SYSTEM), TM_DARK);
    g.cfg.zoom = std::clamp(RegGetDword(L"ZoomPercent", 100), 50ul, 300ul) / 100.f;
    g.cfg.sizeW = (int)std::clamp(RegGetDword(L"Width", 1000), 400ul, 4000ul);
    g.cfg.sizeH = (int)std::clamp(RegGetDword(L"Height", 800), 300ul, 3000ul);
}

static void SaveSettings() {
    if (BenchActive()) return;
    RegSetDword(L"Theme", g.cfg.theme);
    RegSetDword(L"ZoomPercent", (DWORD)std::lround(g.cfg.zoom * 100));
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (g.hwnd && GetWindowPlacement(g.hwnd, &wp) && wp.showCmd == SW_SHOWNORMAL) {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        RegSetDword(L"Width", (DWORD)std::lround(rc.right * 96.f / g.dpi));
        RegSetDword(L"Height", (DWORD)std::lround(rc.bottom * 96.f / g.dpi));
    }
}

// ------------------------------------------------------------------------------------------------ theme
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
    Invalidate();
}

// ------------------------------------------------------------------------------------------------ scrolling / zoom
void ScrollTo(float y, bool animate) {
    g.targetY = std::clamp(y, 0.f, MaxScroll());
    if (!animate) { g.scrollY = g.targetY; g.animating = false; Invalidate(); }
    else g.animating = true;
}

static void Relayout() {
    ClearLayoutCache();
    WithAnchor([] {
        UpdateColumns();
        InitGeometry();
        RecomputeY();
    });
    g.gen++;
    g.jobsPending = 0;
    StartMeasure();
}

static void SetZoom(float z) {
    z = std::clamp(z, kZoomSteps[0], kZoomSteps[std::size(kZoomSteps) - 1]);
    if (std::fabs(z - g.cfg.zoom) < 0.001f) return;
    g.cfg.zoom = z;
    g.canvas->SetScale(Scale());
    Relayout();
    ShowToast(std::to_wstring((int)std::lround(z * 100)) + L" %", 900);
}

static void ZoomStep(int dir) {
    float z = g.cfg.zoom;
    if (dir > 0) { for (float s : kZoomSteps) if (s > z + 0.001f) { SetZoom(s); return; } }
    else { for (int i = (int)std::size(kZoomSteps) - 1; i >= 0; i--) if (kZoomSteps[i] < z - 0.001f) { SetZoom(kZoomSteps[i]); return; } }
}

// ------------------------------------------------------------------------------------------------ clipboard / shell
void CopyToClipboard(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), text.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

static std::wstring UrlDecode(const std::wstring& s) {
    std::string bytes;
    std::wstring out;
    auto flush = [&] {
        if (bytes.empty()) return;
        int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), w.data(), n);
        out += w;
        bytes.clear();
    };
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
            bytes.push_back((char)wcstol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            flush();
            out.push_back(s[i]);
        }
    }
    flush();
    return out;
}

static bool Confirm(const std::wstring& what) {
    std::wstring msg = L"Документ хочет открыть:\n\n" + what + L"\n\nОткрыть?";
    return MessageBoxW(g.hwnd, msg.c_str(), L"FastMD", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

static bool IsMarkdownPath(const std::wstring& p) {
    for (const wchar_t* e : {L".md", L".markdown", L".mdown", L".mkd", L".mdx", L".txt"})
        if (EndsWithI(p, e)) return true;
    return false;
}

void OpenLink(int li) {
    if (li < 0 || (size_t)li >= g.doc.links.size()) return;
    std::wstring href = g.doc.links[li];
    while (!href.empty() && iswspace(href.back())) href.pop_back();
    if (href.empty()) return;
    if (href[0] == L'#') {  // in-document anchor
        int b = HeadingBlockBySlug(UrlDecode(href.substr(1)));
        if (b >= 0) ScrollToBlock((uint32_t)b, true);
        else ShowToast(L"Заголовок не найден: " + href);
        return;
    }
    // scheme?
    size_t colon = href.find(L':');
    size_t slash = href.find_first_of(L"/\\");
    if (colon != std::wstring::npos && colon > 1 && (slash == std::wstring::npos || colon < slash)) {
        std::wstring scheme = ToLower(href.substr(0, colon));
        if (scheme == L"http" || scheme == L"https" || scheme == L"mailto") {
            ShellExecuteW(g.hwnd, L"open", href.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (Confirm(href)) {  // file:, custom protocols: only with explicit consent
            ShellExecuteW(g.hwnd, L"open", href.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    if (href.rfind(L"\\\\", 0) == 0 || href.rfind(L"//", 0) == 0) {  // UNC / protocol-relative
        if (Confirm(href)) ShellExecuteW(g.hwnd, L"open", href.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    // relative / absolute local path, optional #fragment
    std::wstring frag;
    size_t hash = href.find(L'#');
    if (hash != std::wstring::npos) { frag = href.substr(hash + 1); href.resize(hash); }
    size_t q = href.find(L'?');
    if (q != std::wstring::npos) href.resize(q);
    std::wstring p = UrlDecode(href);
    for (auto& c : p) if (c == L'/') c = L'\\';
    std::wstring full = (p.size() > 1 && p[1] == L':') ? p : DirOf(g.path) + p;
    DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { ShowToast(L"Файл не найден: " + p); return; }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY) && IsMarkdownPath(full)) {
        OpenDocument(full, true, 0);
        if (!frag.empty()) {
            int b = HeadingBlockBySlug(UrlDecode(frag));
            if (b >= 0) ScrollToBlock((uint32_t)b, false);
        }
        return;
    }
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) || Confirm(full))
        ShellExecuteW(g.hwnd, L"open", full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void OpenInEditor() {
    if (g.path.empty()) return;
    if ((INT_PTR)ShellExecuteW(g.hwnd, L"edit", g.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32) return;
    std::wstring args = L"\"" + g.path + L"\"";
    ShellExecuteW(g.hwnd, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void ShowInFolder() {
    if (g.path.empty()) return;
    std::wstring args = L"/select,\"" + g.path + L"\"";
    ShellExecuteW(g.hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void OpenDialog() {
    wchar_t file[MAX_PATH * 4] = L"";
    std::wstring dir = DirOf(g.path);
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = g.hwnd;
    of.lpstrFilter = L"Markdown (*.md; *.markdown; *.mdx; *.txt)\0*.md;*.markdown;*.mdown;*.mkd;*.mdx;*.txt\0Все файлы\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = (DWORD)std::size(file);
    of.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&of)) OpenDocument(file, true, 0);
}

// .md association for the current user (no admin). Windows does not let apps set the default programmatically: we
// register a ProgId + capabilities and open Settings → Default apps → FastMD, where the user confirms.
static bool RegisterAssociation(bool openSettings) {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"%1\"";
    std::wstring icon = L"\"" + std::wstring(exe) + L"\",0";
    auto setStr = [](const wchar_t* key, const wchar_t* name, const std::wstring& v) {
        return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    bool ok = setStr(L"Software\\Classes\\FastMD.Markdown", nullptr, L"Markdown-документ");
    ok &= setStr(L"Software\\Classes\\FastMD.Markdown\\DefaultIcon", nullptr, icon);
    ok &= setStr(L"Software\\Classes\\FastMD.Markdown\\shell\\open\\command", nullptr, cmd);
    ok &= setStr(L"Software\\Classes\\Applications\\FastMD.exe\\shell\\open\\command", nullptr, cmd);
    ok &= setStr(L"Software\\FastMD\\Capabilities", L"ApplicationName", L"FastMD");
    ok &= setStr(L"Software\\FastMD\\Capabilities", L"ApplicationDescription", L"Мгновенный просмотр Markdown");
    for (const wchar_t* ext : {L".md", L".markdown", L".mdown", L".mkd", L".mdx"}) {
        ok &= setStr((std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids").c_str(), L"FastMD.Markdown", L"");
        ok &= setStr(L"Software\\FastMD\\Capabilities\\FileAssociations", ext, L"FastMD.Markdown");
        ok &= setStr(L"Software\\Classes\\Applications\\FastMD.exe\\SupportedTypes", ext, L"");
    }
    ok &= setStr(L"Software\\RegisteredApplications", L"FastMD", L"Software\\FastMD\\Capabilities");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (openSettings)
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=FastMD", nullptr, nullptr, SW_SHOWNORMAL);
    return ok;
}

static void UnregisterAssociation() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\FastMD.Markdown");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\FastMD.exe");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FastMD\\Capabilities");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"FastMD");
    for (const wchar_t* ext : {L".md", L".markdown", L".mdown", L".mkd", L".mdx"})
        RegDeleteKeyValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids").c_str(), L"FastMD.Markdown");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// ------------------------------------------------------------------------------------------------ commands
static void CopySelection() {
    std::wstring t = SelectionText();
    if (t.empty()) return;
    CopyToClipboard(t);
    ShowToast(L"Скопировано", 900);
}

static void OpenFind() {
    g.findOpen = true;
    if (HasSelection()) {  // seed with the selected text (single line)
        std::wstring s = SelectionText();
        if (!s.empty() && s.size() < 200 && s.find(L'\n') == std::wstring::npos) g.findQuery = s;
    }
    FindUpdate(false);
    Invalidate();
}

static void CloseFind() {
    g.findOpen = false;
    Invalidate();
}

static void Command(UINT id) {
    switch (id) {
    case CMD_COPY: CopySelection(); break;
    case CMD_SELECT_ALL: SelectAll(); Invalidate(); break;
    case CMD_OPEN: OpenDialog(); break;
    case CMD_RELOAD: ReloadDocument(); ShowToast(L"Обновлено", 700); break;
    case CMD_EDIT: OpenInEditor(); break;
    case CMD_FOLDER: ShowInFolder(); break;
    case CMD_FIND: OpenFind(); break;
    case CMD_THEME_SYSTEM: g.cfg.theme = TM_SYSTEM; ApplyTheme(); break;
    case CMD_THEME_LIGHT: g.cfg.theme = TM_LIGHT; ApplyTheme(); break;
    case CMD_THEME_DARK: g.cfg.theme = TM_DARK; ApplyTheme(); break;
    case CMD_ZOOM_IN: ZoomStep(1); break;
    case CMD_ZOOM_OUT: ZoomStep(-1); break;
    case CMD_ZOOM_RESET: SetZoom(1.f); break;
    case CMD_BACK: NavigateBack(); break;
    case CMD_FORWARD: NavigateForward(); break;
    case CMD_ASSOCIATE:
        if (RegisterAssociation(true)) ShowToast(L"FastMD зарегистрирован. Выберите его для .md в «Приложения по умолчанию»", 5000);
        else ShowToast(L"Не удалось записать ассоциацию в реестр", 3000);
        break;
    }
}

static void ContextMenu(int sx, int sy) {
    HMENU m = CreatePopupMenu(), theme = CreatePopupMenu(), zoom = CreatePopupMenu();
    UINT hasDoc = g.path.empty() ? MF_GRAYED : 0;
    AppendMenuW(m, MF_STRING | (HasSelection() ? 0 : MF_GRAYED), CMD_COPY, L"Копировать\tCtrl+C");
    AppendMenuW(m, MF_STRING | hasDoc, CMD_SELECT_ALL, L"Выделить всё\tCtrl+A");
    AppendMenuW(m, MF_STRING | hasDoc, CMD_FIND, L"Найти…\tCtrl+F");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g.back.empty() ? MF_GRAYED : 0), CMD_BACK, L"Назад\tAlt+←");
    AppendMenuW(m, MF_STRING | (g.fwd.empty() ? MF_GRAYED : 0), CMD_FORWARD, L"Вперёд\tAlt+→");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Открыть файл…\tCtrl+O");
    AppendMenuW(m, MF_STRING | hasDoc, CMD_RELOAD, L"Обновить\tF5");
    AppendMenuW(m, MF_STRING | hasDoc, CMD_EDIT, L"Открыть в редакторе\tCtrl+E");
    AppendMenuW(m, MF_STRING | hasDoc, CMD_FOLDER, L"Показать в папке");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(theme, MF_STRING | (g.cfg.theme == TM_SYSTEM ? MF_CHECKED : 0), CMD_THEME_SYSTEM, L"Как в системе");
    AppendMenuW(theme, MF_STRING | (g.cfg.theme == TM_LIGHT ? MF_CHECKED : 0), CMD_THEME_LIGHT, L"Светлая");
    AppendMenuW(theme, MF_STRING | (g.cfg.theme == TM_DARK ? MF_CHECKED : 0), CMD_THEME_DARK, L"Тёмная");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)theme, L"Тема");
    AppendMenuW(zoom, MF_STRING, CMD_ZOOM_IN, L"Увеличить\tCtrl++");
    AppendMenuW(zoom, MF_STRING, CMD_ZOOM_OUT, L"Уменьшить\tCtrl+−");
    AppendMenuW(zoom, MF_STRING, CMD_ZOOM_RESET, L"Сбросить (100 %)\tCtrl+0");
    std::wstring zl = L"Масштаб: " + std::to_wstring((int)std::lround(g.cfg.zoom * 100)) + L" %";
    AppendMenuW(m, MF_POPUP, (UINT_PTR)zoom, zl.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_ASSOCIATE, L"Открывать .md в FastMD…");
    UINT id = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, g.hwnd, nullptr);
    DestroyMenu(m);  // destroys the submenus too
    if (id) Command(id);
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
    g.offscreenValid = false;
    HDC dst = hdc ? hdc : GetDC(g.hwnd);
    BitBlt(dst, 0, 0, g.pxW, g.pxH, g.canvas->DC(), 0, 0, SRCCOPY);
    if (!hdc) ReleaseDC(g.hwnd, dst);
}

static void ScrollTest() {  // steady-state cost of a scrolling frame, logged to %TEMP%\fastmd-scroll.txt
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
    StartBackgroundWork();
    DragAcceptFiles(g.hwnd, TRUE);
    if (!g.path.empty()) SHAddToRecentDocs(SHARD_PATHW, g.path.c_str());
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

// ------------------------------------------------------------------------------------------------ input
static bool InScrollbar(float x) { return g.docH > ViewH() + 1 && x >= ViewW() - 14; }
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

static void OnMouseMove(int mx, int my, WPARAM keys) {
    float x = mx / Scale(), y = my / Scale();
    if (g.draggingThumb) {
        float th;
        ThumbTop(&th);
        float trackH = ViewH() - 4 - th;
        ScrollTo(trackH > 0 ? (y - g.dragGrab - 2) / trackH * MaxScroll() : 0, false);
        return;
    }
    if (g.selecting) {
        if (y < 0 || y > ViewH()) SetTimer(g.hwnd, TIMER_AUTOSCROLL, 16, nullptr);
        else KillTimer(g.hwnd, TIMER_AUTOSCROLL);
        UpdateSelectionTo(x, std::clamp(y, 0.f, ViewH()));
        return;
    }
    (void)keys;
    bool hot = InScrollbar(x);
    if (hot != g.hotScroll) { g.hotScroll = hot; Invalidate(); }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g.hwnd, 0};
    TrackMouseEvent(&tme);
    int link = hot ? -1 : LinkAt(x, y);
    bool onBtn = false;
    int code = hot ? -1 : CodeBlockAt(x, y, &onBtn);
    if (link != g.hoverLink || code != g.hoverCode || onBtn != g.hoverCopyBtn) {
        g.hoverLink = link;
        g.hoverCode = code;
        g.hoverCopyBtn = onBtn;
        Invalidate();
    }
    LPCWSTR cur = IDC_ARROW;
    if (link >= 0 || onBtn) cur = IDC_HAND;
    else if (!hot && !g.path.empty()) {
        uint32_t pos;
        bool inside = false;
        if (HitTestDoc(x, y, &pos, &inside) && inside) cur = IDC_IBEAM;
    }
    SetCursor(LoadCursorW(nullptr, cur));
}

static void OnLButtonDown(int mx, int my, WPARAM keys) {
    float x = mx / Scale(), y = my / Scale();
    SetCapture(g.hwnd);
    g.downX = mx;
    g.downY = my;
    if (g.path.empty()) return;
    if (InScrollbar(x)) {
        float th, tt = ThumbTop(&th);
        if (y >= tt && y < tt + th) { g.draggingThumb = true; g.dragGrab = y - tt; }
        else ScrollTo(g.targetY + (y < tt ? -1 : 1) * (ViewH() - 48), true);
        Invalidate();
        return;
    }
    bool onBtn = false;
    int code = CodeBlockAt(x, y, &onBtn);
    if (onBtn && code >= 0) {
        CopyToClipboard(BlockPlainText((uint32_t)code));
        ShowToast(L"Код скопирован", 900);
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

static bool OnFindKey(WPARAM vk, bool ctrl, bool shift) {
    switch (vk) {
    case VK_ESCAPE: CloseFind(); return true;
    case VK_RETURN: case VK_F3: FindStep(shift ? -1 : 1); return true;
    case VK_BACK:
        if (!g.findQuery.empty()) {
            if (ctrl) {
                size_t k = g.findQuery.size();
                while (k > 0 && iswspace(g.findQuery[k - 1])) k--;
                while (k > 0 && !iswspace(g.findQuery[k - 1])) k--;
                g.findQuery.resize(k);
            } else g.findQuery.pop_back();
            FindUpdate(false);
        }
        return true;
    case 'V':
        if (ctrl && OpenClipboard(g.hwnd)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* s = (const wchar_t*)GlobalLock(h)) {
                    std::wstring t = s;
                    GlobalUnlock(h);
                    for (auto& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
                    g.findQuery += t.substr(0, 256);
                }
            }
            CloseClipboard();
            FindUpdate(false);
            return true;
        }
        return false;
    }
    return false;
}

static bool OnKeyDown(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (g.findOpen && OnFindKey(vk, ctrl, shift)) return true;
    if (g.findOpen && vk == VK_SPACE) return true;  // typed into the find box (WM_CHAR), must not page down
    float vh = ViewH();
    if (ctrl) {
        switch (vk) {
        case 'C': case VK_INSERT: CopySelection(); return true;
        case 'A': if (!g.findOpen) { SelectAll(); Invalidate(); } return true;
        case 'F': OpenFind(); return true;
        case 'O': OpenDialog(); return true;
        case 'E': OpenInEditor(); return true;
        case 'R': Command(CMD_RELOAD); return true;
        case 'W': PostMessageW(g.hwnd, WM_CLOSE, 0, 0); return true;
        case VK_OEM_PLUS: case VK_ADD: ZoomStep(1); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT: ZoomStep(-1); return true;
        case '0': case VK_NUMPAD0: SetZoom(1.f); return true;
        case VK_HOME: ScrollTo(0, true); return true;
        case VK_END: ScrollTo(MaxScroll(), true); return true;
        }
        return false;
    }
    switch (vk) {
    case VK_DOWN: ScrollTo(g.targetY + 56, true); return true;
    case VK_UP: ScrollTo(g.targetY - 56, true); return true;
    case VK_NEXT: ScrollTo(g.targetY + vh - 56, true); return true;
    case VK_SPACE: ScrollTo(g.targetY + (shift ? -1 : 1) * (vh - 56), true); return true;
    case VK_PRIOR: ScrollTo(g.targetY - vh + 56, true); return true;
    case VK_HOME: ScrollTo(0, true); return true;
    case VK_END: ScrollTo(MaxScroll(), true); return true;
    case VK_F3: if (!g.findQuery.empty()) { g.findOpen = true; FindStep(shift ? -1 : 1); } else OpenFind(); return true;
    case VK_F5: Command(CMD_RELOAD); return true;
    case VK_BACK: NavigateBack(); return true;
    case VK_ESCAPE:
        if (HasSelection()) { g.selAnchor = g.selFocus; Invalidate(); }
        else PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
        return true;
    }
    return false;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
    case WM_MOUSEWHEEL: {
        if (!g.ready || g.firstFrame) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) { ZoomStep(delta > 0 ? 1 : -1); return 0; }
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        float step = lines == WHEEL_PAGESCROLL ? ViewH() - 56 : 38.f * std::max(1u, lines);
        ScrollTo(g.targetY - (float)delta / WHEEL_DELTA * step, true);
        return 0;
    }
    case WM_KEYDOWN:
        if (!g.ready || g.firstFrame) return 0;
        if (OnKeyDown(wp)) return 0;
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_LEFT) { NavigateBack(); return 0; }
        if (wp == VK_RIGHT) { NavigateForward(); return 0; }
        break;
    case WM_CHAR:
        if (g.findOpen && wp >= 32 && wp != 127) {
            g.findQuery.push_back((wchar_t)wp);
            FindUpdate(false);
            return 0;
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (!g.ready || g.firstFrame) return 0;
        OnLButtonDown(GET_X(lp), GET_Y(lp), wp);
        return 0;
    case WM_MOUSEMOVE:
        if (!g.ready || g.firstFrame) return 0;
        OnMouseMove(GET_X(lp), GET_Y(lp), wp);
        return 0;
    case WM_MOUSELEAVE:
        if (g.hotScroll || g.hoverLink >= 0 || g.hoverCode >= 0) {
            g.hotScroll = false;
            g.hoverLink = g.hoverCode = -1;
            g.hoverCopyBtn = false;
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
        int sx = GET_X(lp), sy = GET_Y(lp);
        if (sx == -1 && sy == -1) { POINT p{20, 20}; ClientToScreen(hwnd, &p); sx = p.x; sy = p.y; }
        ContextMenu(sx, sy);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
        break;
    case WM_DROPFILES: {
        wchar_t file[MAX_PATH * 4];
        if (DragQueryFileW((HDROP)wp, 0, file, (UINT)std::size(file))) OpenDocument(file, true, 0);
        DragFinish((HDROP)wp);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_TOAST) { KillTimer(hwnd, TIMER_TOAST); Invalidate(); }
        else if (wp == TIMER_RELOAD) { KillTimer(hwnd, TIMER_RELOAD); ReloadDocument(); }
        else if (wp == TIMER_AUTOSCROLL && g.selecting) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            float y = p.y / Scale();
            float d = y < 0 ? y : y > ViewH() ? y - ViewH() : 0;
            if (d != 0) {
                ScrollTo(g.scrollY + std::clamp(d * 0.5f, -60.f, 60.f), false);
                UpdateSelectionTo(p.x / Scale(), std::clamp(y, 0.f, ViewH()));
            }
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) ApplyTheme();
        return 0;
    case WM_COMMAND:  // menu ids; also lets tests and automation drive the viewer (tests/ui_smoke.py)
        if (g.ready && !g.firstFrame) Command(LOWORD(wp));
        return 0;
    case WM_APP_MEASURED: OnMeasured((MeasureJob*)lp); return 0;
    case WM_APP_FULLDOC: if (!g.firstFrame) OnFullDoc(); return 0;
    case WM_APP_IMAGES: OnImagesLoaded(); return 0;
    case WM_APP_FILECHANGED: SetTimer(hwnd, TIMER_RELOAD, 120, nullptr); return 0;  // debounce editor save bursts
    case WM_CLOSE:
        g.closing = true;
        SaveSettings();
        DestroyWindow(hwnd);
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
    if (!bench) LoadSettings();
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
        else if (a.rfind(L"--size=", 0) == 0) swscanf_s(a.c_str() + 7, L"%dx%d", &g.cfg.sizeW, &g.cfg.sizeH);
        else if (a.rfind(L"--scroll-test=", 0) == 0) g.cfg.scrollTest = _wtoi(a.c_str() + 14);
        else if (a.rfind(L"--", 0) != 0) path = a;
    }
    if (bench) { g.cfg.sizeW = 1000; g.cfg.sizeH = 800; }  // PROTOCOL.md §5: 1000×800 DIP
    SetDarkPalette(WantDark());

    // geometry guess (the doc thread lays out for it before the window exists)
    g.dpi = (float)GetDpiForSystem();
    g.pxW = MulDiv(g.cfg.sizeW, (int)g.dpi, 96);
    g.pxH = MulDiv(g.cfg.sizeH, (int)g.dpi, 96);
    if (!path.empty()) {
        wchar_t full[MAX_PATH * 4];
        g.path = GetFullPathNameW(path.c_str(), MAX_PATH * 4, full, nullptr) ? full : path;
    }
    g.docThread = CreateThread(nullptr, 0, StartupDocThread, nullptr, 0, nullptr);

    if (g.cfg.noIme) ImmDisableIME(0);  // UI thread only; the find box takes WM_CHAR (layouts work, IME composition not)
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    uint32_t bg = g_pal[P_BG];
    wc.hbrBackground = CreateSolidBrush(RGB(bg >> 16, (bg >> 8) & 255, bg & 255));
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    RECT r{0, 0, g.pxW, g.pxH};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)g.dpi);
    std::wstring title = WindowTitle();
    g.hwnd = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    Mark("window_created");
    if (g.cfg.noAnim) {
        BOOL on = TRUE;  // content appears at full opacity immediately instead of the ~200 ms open animation
        DwmSetWindowAttribute(g.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on));
    }
    ApplyWindowChrome(g.hwnd);
    ShowWindow(g.hwnd, show);
    Mark("shown");
    UpdateWindow(g.hwnd);
    return MessageLoop();
}
