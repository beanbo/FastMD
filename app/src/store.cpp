// Persistent state: settings in HKCU\Software\FastMD (one key open, one value per setting), reading positions and the
// recent-documents list in %LOCALAPPDATA%\FastMD\positions.bin (shared by all FastMD windows: every write re-reads
// and merges under a named mutex), editor detection for Ctrl+E.
// Tests redirect both stores with FASTMD_REGKEY / FASTMD_DATA so they never touch the reader's own data.
#include "app.h"

namespace {
wchar_t g_regKey[160] = L"";
const size_t kMaxPositions = 200;

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)std::size(buf));
    return (n && n < std::size(buf)) ? std::wstring(buf) : std::wstring();
}

HKEY OpenKey(bool write) {
    HKEY k = nullptr;
    if (write) RegCreateKeyExW(HKEY_CURRENT_USER, RegKeyPath(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr);
    else RegOpenKeyExW(HKEY_CURRENT_USER, RegKeyPath(), 0, KEY_READ, &k);
    return k;
}
DWORD GetDword(HKEY k, const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v), type = 0;
    if (!k || RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&v, &sz) != ERROR_SUCCESS || type != REG_DWORD) return def;
    return v;
}
std::wstring GetString(HKEY k, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (!k || RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &sz) != ERROR_SUCCESS || type != REG_SZ) return L"";
    buf[sz / sizeof(wchar_t)] = 0;
    return buf;
}
void SetDword(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); }
void SetString(HKEY k, const wchar_t* name, const std::wstring& s) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)s.c_str(), (DWORD)((s.size() + 1) * sizeof(wchar_t)));
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

// ---- positions.bin: "FMDP", version 1, count, entries (length-prefixed UTF-16 strings, little-endian numbers)
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    template <class T> bool Get(T& v) {
        if ((size_t)(end - p) < sizeof(T)) return false;
        memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return true;
    }
    bool Str(std::wstring& s) {
        uint16_t n = 0;
        if (!Get(n) || (size_t)(end - p) < n * sizeof(wchar_t)) return false;
        s.assign((const wchar_t*)p, n);
        p += n * sizeof(wchar_t);
        return true;
    }
};
struct Writer {
    std::vector<uint8_t> b;
    template <class T> void Put(const T& v) { b.insert(b.end(), (const uint8_t*)&v, (const uint8_t*)&v + sizeof(T)); }
    void Str(const std::wstring& s) {
        uint16_t n = (uint16_t)std::min<size_t>(s.size(), 32767);
        Put(n);
        b.insert(b.end(), (const uint8_t*)s.data(), (const uint8_t*)(s.data() + n));
    }
};

bool ReadEntries(const std::wstring& file, std::vector<PosEntry>& out) {
    out.clear();
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::vector<uint8_t> buf(sz.QuadPart > 0 && sz.QuadPart < (8 << 20) ? (size_t)sz.QuadPart : 0);
    DWORD got = 0;
    bool ok = !buf.empty() && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got == buf.size();
    CloseHandle(f);
    if (!ok) return false;
    Reader r{buf.data(), buf.data() + buf.size()};
    uint32_t magic = 0, version = 0, count = 0;
    if (!r.Get(magic) || magic != 'PDMF' || !r.Get(version) || version != 1 || !r.Get(count)) return false;
    for (uint32_t i = 0; i < count && i < 4 * kMaxPositions; i++) {
        PosEntry e;
        if (!r.Str(e.path) || !r.Get(e.size) || !r.Get(e.mtime) || !r.Get(e.opened) || !r.Get(e.block) ||
            !r.Get(e.blockOff) || !r.Str(e.slug) || !r.Get(e.slugOff))
            break;
        if (!e.path.empty() && std::isfinite(e.blockOff) && std::isfinite(e.slugOff)) out.push_back(std::move(e));
    }
    return true;
}

bool WriteEntries(const std::wstring& dir, const std::vector<PosEntry>& list) {
    Writer w;
    w.Put((uint32_t)'PDMF');
    w.Put((uint32_t)1);
    w.Put((uint32_t)list.size());
    for (const PosEntry& e : list) {
        w.Str(e.path);
        w.Put(e.size);
        w.Put(e.mtime);
        w.Put(e.opened);
        w.Put(e.block);
        w.Put(e.blockOff);
        w.Str(e.slug);
        w.Put(e.slugOff);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring tmp = dir + L"positions.tmp", dst = dir + L"positions.bin";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, w.b.data(), (DWORD)w.b.size(), &put, nullptr) && put == w.b.size();
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void SortAndTrim(std::vector<PosEntry>& list) {
    std::stable_sort(list.begin(), list.end(), [](const PosEntry& a, const PosEntry& b) { return a.opened > b.opened; });
    if (list.size() > kMaxPositions) list.resize(kMaxPositions);
}

struct StoreLock {  // positions.bin is shared by every FastMD window
    HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\FastMD.positions");
    StoreLock() { if (m) WaitForSingleObject(m, 2000); }
    ~StoreLock() { if (m) { ReleaseMutex(m); CloseHandle(m); } }
};
}  // namespace

// when the updater last asked GitHub (FILETIME ticks): once a day is enough
uint64_t LastUpdateCheck() {
    HKEY k = OpenKey(false);
    uint64_t lo = GetDword(k, L"UpdateSeenLo", 0), hi = GetDword(k, L"UpdateSeenHi", 0);
    if (k) RegCloseKey(k);
    return (hi << 32) | lo;
}

void SetLastUpdateCheck(uint64_t t) {
    HKEY k = OpenKey(true);
    if (!k) return;
    SetDword(k, L"UpdateSeenLo", (DWORD)(t & 0xFFFFFFFF));
    SetDword(k, L"UpdateSeenHi", (DWORD)(t >> 32));
    RegCloseKey(k);
}

// the newest release a check found, with its installer and hash: later windows show it without asking GitHub again
void LoadFoundUpdate(std::wstring& version, std::wstring& url, std::wstring& shaUrl) {
    HKEY k = OpenKey(false);
    version = GetString(k, L"UpdateVersion");
    url = GetString(k, L"UpdateUrl");
    shaUrl = GetString(k, L"UpdateShaUrl");
    if (k) RegCloseKey(k);
}

void SaveFoundUpdate(const std::wstring& version, const std::wstring& url, const std::wstring& shaUrl) {
    HKEY k = OpenKey(true);
    if (!k) return;
    if (version.empty()) {
        RegDeleteValueW(k, L"UpdateVersion");
        RegDeleteValueW(k, L"UpdateUrl");
        RegDeleteValueW(k, L"UpdateShaUrl");
    } else {
        SetString(k, L"UpdateVersion", version);
        SetString(k, L"UpdateUrl", url);
        SetString(k, L"UpdateShaUrl", shaUrl);
    }
    RegCloseKey(k);
}

// the newest crash dump the reader has already been offered (FILETIME ticks): so it is offered once, not every start
uint64_t LastCrashSeen() {
    HKEY k = OpenKey(false);
    uint64_t lo = GetDword(k, L"CrashSeenLo", 0), hi = GetDword(k, L"CrashSeenHi", 0);
    if (k) RegCloseKey(k);
    return (hi << 32) | lo;
}

void SetLastCrashSeen(uint64_t t) {
    HKEY k = OpenKey(true);
    if (!k) return;
    SetDword(k, L"CrashSeenLo", (DWORD)(t & 0xFFFFFFFF));
    SetDword(k, L"CrashSeenHi", (DWORD)(t >> 32));
    RegCloseKey(k);
}

// ------------------------------------------------------------------------------------------------ locations
const wchar_t* RegKeyPath() {
    if (!g_regKey[0]) {
        DWORD n = GetEnvironmentVariableW(L"FASTMD_REGKEY", g_regKey, (DWORD)std::size(g_regKey));
        if (!n || n >= std::size(g_regKey)) wcscpy_s(g_regKey, L"Software\\FastMD");
    }
    return g_regKey;
}

std::wstring DataDir() {
    std::wstring d = Env(L"FASTMD_DATA");
    if (d.empty()) {
        std::wstring la = Env(L"LOCALAPPDATA");
        if (la.empty()) return L"";
        d = la + L"\\FastMD";
    }
    if (d.back() != L'\\') d += L'\\';
    return d;
}

uint64_t FileTimeU64(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

// ------------------------------------------------------------------------------------------------ settings
void LoadConfig(Config& c, std::wstring* findQuery) {
    HKEY k = OpenKey(false);
    c.theme = (ThemeMode)std::min<DWORD>(GetDword(k, L"Theme", TM_SYSTEM), TM_DARK);
    c.zoom = std::clamp(GetDword(k, L"ZoomPercent", 100), 50ul, 300ul) / 100.f;
    c.column = (uint8_t)std::min<DWORD>(GetDword(k, L"Column", COL_NORMAL), COL_FULL);
    c.wrapCode = GetDword(k, L"WrapCode", 0) != 0;
    c.tocOpen = GetDword(k, L"Outline", 0) != 0;
    c.font = (uint8_t)std::min<DWORD>(GetDword(k, L"Font", FONT_SEGOE), FONT_SITKA);
    c.fontSize = (int)std::clamp(GetDword(k, L"FontSize", 16), 12ul, 24ul);
    c.smoothScroll = GetDword(k, L"SmoothScroll", 1) != 0;
    c.language = (uint8_t)std::min<DWORD>(GetDword(k, L"Language", LANG_AUTO), LANG_EN);
    c.remoteImages = (uint8_t)std::min<DWORD>(GetDword(k, L"RemoteImages", 0), 2);
    c.updateCheck = GetDword(k, L"UpdateCheck", 1) != 0;
    c.editor = GetString(k, L"Editor");
    c.findCase = GetDword(k, L"FindCase", 0) != 0;
    c.findWord = GetDword(k, L"FindWord", 0) != 0;
    if (findQuery) *findQuery = GetString(k, L"FindQuery");
    c.sizeW = (int)std::clamp(GetDword(k, L"Width", 1000), 400ul, 4000ul);  // v0.1: client size only
    c.sizeH = (int)std::clamp(GetDword(k, L"Height", 800), 300ul, 3000ul);
    if (k) RegCloseKey(k);
}

void SaveConfig(const Config& c, const std::wstring& findQuery) {
    HKEY k = OpenKey(true);
    if (!k) return;
    SetDword(k, L"Theme", c.theme);
    SetDword(k, L"ZoomPercent", (DWORD)std::lround(c.zoom * 100));
    SetDword(k, L"Column", c.column);
    SetDword(k, L"WrapCode", c.wrapCode);
    SetDword(k, L"Outline", c.tocOpen);
    SetDword(k, L"Font", c.font);
    SetDword(k, L"FontSize", (DWORD)c.fontSize);
    SetDword(k, L"SmoothScroll", c.smoothScroll);
    SetDword(k, L"Language", c.language);
    SetDword(k, L"RemoteImages", c.remoteImages);
    SetDword(k, L"UpdateCheck", c.updateCheck);
    SetString(k, L"Editor", c.editor);
    SetDword(k, L"FindCase", c.findCase);
    SetDword(k, L"FindWord", c.findWord);
    SetString(k, L"FindQuery", findQuery.substr(0, 256));
    RegCloseKey(k);
}

bool RegReadBinary(const wchar_t* name, void* data, DWORD size) {
    DWORD sz = size;
    return RegGetValueW(HKEY_CURRENT_USER, RegKeyPath(), name, RRF_RT_REG_BINARY, nullptr, data, &sz) == ERROR_SUCCESS &&
           sz == size;
}

void RegWriteBinary(const wchar_t* name, const void* data, DWORD size) {
    RegSetKeyValueW(HKEY_CURRENT_USER, RegKeyPath(), name, REG_BINARY, data, size);
}

// ------------------------------------------------------------------------------------------------ positions
bool PositionsLoad(std::vector<PosEntry>& out) {
    std::wstring dir = DataDir();
    if (dir.empty()) return false;
    StoreLock lock;
    bool ok = ReadEntries(dir + L"positions.bin", out);
    SortAndTrim(out);
    return ok;
}

void PositionsSave(const PosEntry& e, bool keepPosition) {
    std::wstring dir = DataDir();
    if (dir.empty() || e.path.empty()) return;
    StoreLock lock;
    std::vector<PosEntry> list;
    ReadEntries(dir + L"positions.bin", list);
    auto it = std::find_if(list.begin(), list.end(), [&](const PosEntry& o) { return SamePath(o.path, e.path); });
    if (it == list.end()) list.push_back(e);
    else if (keepPosition) it->opened = std::max(it->opened, e.opened);
    else {
        uint64_t opened = it->opened;
        *it = e;
        it->opened = std::max(opened, e.opened);
    }
    SortAndTrim(list);
    WriteEntries(dir, list);
}

void PositionsRemove(const std::wstring& path) {
    std::wstring dir = DataDir();
    if (dir.empty()) return;
    StoreLock lock;
    std::vector<PosEntry> list;
    if (!ReadEntries(dir + L"positions.bin", list)) return;
    size_t n = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const PosEntry& o) { return SamePath(o.path, path); }), list.end());
    if (list.size() != n) WriteEntries(dir, list);
}

// ------------------------------------------------------------------------------------------------ editors
std::vector<EditorInfo> DetectEditors() {
    std::wstring la = Env(L"LOCALAPPDATA"), pf = Env(L"ProgramFiles"), pf86 = Env(L"ProgramFiles(x86)"), sys = Env(L"SystemRoot");
    struct Cand { const wchar_t* name; std::wstring paths[2]; };
    const Cand cands[] = {
        {L"VS Code", {la + L"\\Programs\\Microsoft VS Code\\Code.exe", pf + L"\\Microsoft VS Code\\Code.exe"}},
        {L"Cursor", {la + L"\\Programs\\cursor\\Cursor.exe", pf + L"\\Cursor\\Cursor.exe"}},
        {L"Void", {la + L"\\Programs\\Void\\Void.exe", pf + L"\\Void\\Void.exe"}},
        {L"Notepad++", {pf + L"\\Notepad++\\notepad++.exe", pf86 + L"\\Notepad++\\notepad++.exe"}},
        {L"Sublime Text", {pf + L"\\Sublime Text\\sublime_text.exe", pf + L"\\Sublime Text 3\\sublime_text.exe"}},
    };
    std::vector<EditorInfo> out;
    for (const Cand& c : cands)
        for (const std::wstring& p : c.paths) {
            DWORD a = p.size() > 12 ? GetFileAttributesW(p.c_str()) : INVALID_FILE_ATTRIBUTES;
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) { out.push_back({c.name, p}); break; }
        }
    if (!sys.empty()) out.push_back({Tr(S_EDITOR_NOTEPAD), sys + L"\\System32\\notepad.exe"});
    return out;
}
