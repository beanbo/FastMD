// Everything that leaves the viewer: links (scheme allow-list), clipboard (text, images), the editor for Ctrl+E,
// Explorer, file dialogs, the .md association.
#include "app.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

// ------------------------------------------------------------------------------------------------ clipboard
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

static Image* DecodedImage(uint32_t bi, Image** entry) {
    if (bi >= g.doc.blocks.size() || g.doc.blocks[bi].kind != BK_IMAGE) return nullptr;
    Image& im0 = g.doc.images[g.doc.blocks[bi].aux];
    if (entry) *entry = &im0;
    Image& im = im0.canon >= 0 ? g.doc.images[im0.canon] : im0;
    bool ok = im.state.load() == 2 && im.pxW > 0 && im.pxH > 0 && im.px.size() >= (size_t)im.pxW * im.pxH;
    return ok ? &im : nullptr;
}

// 32-bit DIB of a picture block, composed over white: what both the clipboard and a drag hand out
HGLOBAL ImageAsDib(uint32_t bi) {
    Image* entry = nullptr;
    Image* im = DecodedImage(bi, &entry);
    if (!im) return nullptr;
    size_t w = (size_t)im->pxW, h = (size_t)im->pxH;  // the decoded buffer, not the header size
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + w * h * 4);
    if (!dib) return nullptr;
    auto* bih = (BITMAPINFOHEADER*)GlobalLock(dib);
    *bih = BITMAPINFOHEADER{sizeof(BITMAPINFOHEADER), (LONG)w, (LONG)h, 1, 32, BI_RGB, (DWORD)(w * h * 4), 0, 0, 0, 0};
    uint32_t* dst = (uint32_t*)(bih + 1);
    for (size_t y = 0; y < h; y++) {  // bottom-up rows
        const uint32_t* src = im->px.data() + (h - 1 - y) * w;
        for (size_t x = 0; x < w; x++) {
            uint32_t p = src[x], ia = 255 - (p >> 24);  // premultiplied BGRA over white
            uint32_t b = std::min<uint32_t>(255, (p & 0xff) + ia), gg = std::min<uint32_t>(255, ((p >> 8) & 0xff) + ia),
                     r = std::min<uint32_t>(255, ((p >> 16) & 0xff) + ia);
            dst[y * w + x] = 0xff000000u | r << 16 | gg << 8 | b;
        }
    }
    GlobalUnlock(dib);
    return dib;
}

// CF_DIB (opaque, composited over white: every app can paste it) + "PNG" with the original file bytes when the image
// is a PNG (Office, browsers and editors paste that one with its transparency)
bool CopyImageToClipboard(uint32_t bi) {
    HGLOBAL dib = ImageAsDib(bi);
    if (!dib) return false;
    Image* entry = nullptr;
    if (!DecodedImage(bi, &entry) || !entry) { GlobalFree(dib); return false; }
    HGLOBAL png = nullptr;
    if (EndsWithI(entry->path, L".png")) {
        HANDLE f = CreateFileW(entry->path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            GetFileSizeEx(f, &sz);
            if (sz.QuadPart > 0 && sz.QuadPart < (64 << 20) && (png = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)sz.QuadPart))) {
                DWORD got = 0;
                BOOL ok = ReadFile(f, GlobalLock(png), (DWORD)sz.QuadPart, &got, nullptr);
                GlobalUnlock(png);
                if (!ok || got != sz.QuadPart) { GlobalFree(png); png = nullptr; }
            }
            CloseHandle(f);
        }
    }
    if (!OpenClipboard(g.hwnd)) {
        GlobalFree(dib);
        if (png) GlobalFree(png);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, dib)) GlobalFree(dib);
    if (png && !SetClipboardData(RegisterClipboardFormatW(L"PNG"), png)) GlobalFree(png);
    CloseClipboard();
    return true;
}

void OpenImageFile(uint32_t bi) {
    if (bi >= g.doc.blocks.size() || g.doc.blocks[bi].kind != BK_IMAGE) return;
    const Image& im = g.doc.images[g.doc.blocks[bi].aux];
    if (!im.path.empty()) ShellExecuteW(g.hwnd, L"open", im.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ------------------------------------------------------------------------------------------------ links
std::wstring UrlDecode(const std::wstring& s) {
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
    wchar_t msg[2048];
    swprintf_s(msg, Tr(S_CONFIRM_OPEN), what.substr(0, 1500).c_str());
    return MessageBoxW(g.hwnd, msg, L"FastMD", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
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
    g.userMoved = true;
    if (href[0] == L'#') {  // in-document anchor
        int b = HeadingBlockBySlug(UrlDecode(href.substr(1)));
        if (b >= 0) ScrollToBlock((uint32_t)b, true);
        else ShowToast(Tr(S_HEADING_NOT_FOUND) + href);
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
    if (attr == INVALID_FILE_ATTRIBUTES) { ShowToast(Tr(S_FILE_NOT_FOUND) + p); return; }
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

// ------------------------------------------------------------------------------------------------ editor / Explorer
void OpenInEditor() {
    if (g.path.empty()) return;
    if (!g.cfg.editor.empty()) {  // the editor chosen in the settings
        std::wstring cmd = L"\"" + g.cfg.editor + L"\" \"" + g.path + L"\"";
        std::wstring dir = DirOf(g.path);
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(g.cfg.editor.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            ShowToast(Tr(S_EDITOR_FAIL), 2500);
        }
        return;
    }
    if ((INT_PTR)ShellExecuteW(g.hwnd, L"edit", g.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32) return;
    std::wstring args = L"\"" + g.path + L"\"";
    ShellExecuteW(g.hwnd, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void ShowInFolder() {
    if (g.path.empty()) return;
    std::wstring args = L"/select,\"" + g.path + L"\"";
    ShellExecuteW(g.hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static std::wstring Filter(std::initializer_list<const wchar_t*> parts) {  // "a\0b\0c\0d\0\0"
    std::wstring f;
    for (const wchar_t* p : parts) { f += p; f.push_back(L'\0'); }
    return f;
}

void OpenDialog() {
    wchar_t file[MAX_PATH * 4] = L"";
    std::wstring dir = DirOf(g.path);
    std::wstring filter = Filter({Tr(S_FILTER_MD), L"*.md;*.markdown;*.mdown;*.mkd;*.mdx;*.txt", Tr(S_FILTER_ALL), L"*.*"});
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = g.hwnd;
    of.lpstrFilter = filter.c_str();
    of.lpstrFile = file;
    of.nMaxFile = (DWORD)std::size(file);
    of.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&of)) OpenDocument(file, true, 0, true);
}

// where to write the exported PDF: beside the document, named after it
std::wstring SavePdfDialog() {
    wchar_t preset[MAX_PATH * 4];
    if (GetEnvironmentVariableW(L"FASTMD_PDF_OUT", preset, (DWORD)std::size(preset)))
        return preset;  // UI tests (and scripts) say where the file goes instead of answering a dialog
    std::wstring name = FileNameOf(g.path);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    name += L".pdf";
    wchar_t file[MAX_PATH * 4] = L"";
    wcsncpy_s(file, name.c_str(), _TRUNCATE);
    std::wstring dir = DirOf(g.path);
    std::wstring filter = Filter({Tr(S_FILTER_PDF), L"*.pdf", Tr(S_FILTER_ALL), L"*.*"});
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = g.hwnd;
    of.lpstrFilter = filter.c_str();
    of.lpstrFile = file;
    of.nMaxFile = (DWORD)std::size(file);
    of.lpstrDefExt = L"pdf";
    of.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOREADONLYRETURN;
    return GetSaveFileNameW(&of) ? std::wstring(file) : std::wstring();
}

std::wstring PickExeDialog(HWND owner) {
    wchar_t file[MAX_PATH * 4] = L"";
    std::wstring filter = Filter({Tr(S_FILTER_EXE), L"*.exe", Tr(S_FILTER_ALL), L"*.*"});
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = owner;
    of.lpstrFilter = filter.c_str();
    of.lpstrFile = file;
    of.nMaxFile = (DWORD)std::size(file);
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    return GetOpenFileNameW(&of) ? std::wstring(file) : std::wstring();
}

// ------------------------------------------------------------------------------------------------ association
// .md association for the current user (no admin). Windows does not let apps set the default programmatically: we
// register a ProgId + capabilities and open Settings → Default apps → FastMD, where the user confirms.
bool RegisterAssociation(bool openSettings) {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"%1\"";
    std::wstring icon = L"\"" + std::wstring(exe) + L"\",0";
    auto setStr = [](const wchar_t* key, const wchar_t* name, const std::wstring& v) {
        return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    bool ok = setStr(L"Software\\Classes\\FastMD.Markdown", nullptr, Tr(S_PROGID_NAME));
    ok &= setStr(L"Software\\Classes\\FastMD.Markdown\\DefaultIcon", nullptr, icon);
    ok &= setStr(L"Software\\Classes\\FastMD.Markdown\\shell\\open\\command", nullptr, cmd);
    ok &= setStr(L"Software\\Classes\\Applications\\FastMD.exe\\shell\\open\\command", nullptr, cmd);
    ok &= setStr(L"Software\\FastMD\\Capabilities", L"ApplicationName", L"FastMD");
    ok &= setStr(L"Software\\FastMD\\Capabilities", L"ApplicationDescription", Tr(S_APP_DESCRIPTION));
    for (const wchar_t* ext : {L".md", L".markdown", L".mdown", L".mkd", L".mdx"}) {
        ok &= setStr((std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids").c_str(), L"FastMD.Markdown", L"");
        ok &= setStr(L"Software\\FastMD\\Capabilities\\FileAssociations", ext, L"FastMD.Markdown");
        ok &= setStr(L"Software\\Classes\\Applications\\FastMD.exe\\SupportedTypes", ext, L"");
    }
    ok &= setStr(L"Software\\RegisteredApplications", L"FastMD", L"Software\\FastMD\\Capabilities");
    PreviewRegister(true);  // the preview pane and the thumbnails come with the association (plan 5.2, 5.3)
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (openSettings)
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=FastMD", nullptr, nullptr, SW_SHOWNORMAL);
    return ok;
}

void UnregisterAssociation() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\FastMD.Markdown");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\FastMD.exe");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FastMD\\Capabilities");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"FastMD");
    for (const wchar_t* ext : {L".md", L".markdown", L".mdown", L".mkd", L".mdx"})
        RegDeleteKeyValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids").c_str(), L"FastMD.Markdown");
    PreviewRegister(false);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
