// Checking for a new version, and installing it if the reader says yes (plan 5.6).
//
// Once a day, after the first frame, on a worker thread: one HTTPS request to the GitHub releases API. Nothing is
// sent about the reader - no identifiers, no telemetry - and nothing is downloaded until they agree. The installer
// that comes down is checked against the SHA-256 published beside it in the same release before it is ever run, and
// it is the same per-user installer as always: it replaces the files in %LOCALAPPDATA%\Programs\FastMD and leaves
// open windows alone.
//
// The whole thing can be switched off in the settings ("Проверять обновления"), and it never runs in bench mode.
#include "app.h"

#include "net.h"
#include "version.h"

#include <shellapi.h>

namespace {
const wchar_t* kApi = L"https://api.github.com/repos/beanbo/FastMD/releases/latest";
const uint64_t kDay = 24ull * 60 * 60 * 10'000'000;  // FILETIME ticks

struct Found {
    std::wstring version, url, shaUrl;
};
Found g_found;
std::wstring g_downloaded;  // the verified installer, waiting to be run
bool g_testOnly = false;    // UI test: download and check the hash, but never run what came down

// The address can be pointed elsewhere for a test (the UI test serves a release of its own from the loopback);
// nothing else in the program reads this variable.
std::wstring ApiUrl() {
    wchar_t env[1024];
    if (GetEnvironmentVariableW(L"FASTMD_UPDATE_URL", env, (DWORD)std::size(env))) return env;
    return kApi;
}

std::string Between(const std::string& s, const std::string& key, size_t from = 0, size_t* end = nullptr) {
    size_t k = s.find(key, from);
    if (k == std::string::npos) return "";
    size_t q1 = s.find('"', k + key.size());
    if (q1 == std::string::npos) return "";
    size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    if (end) *end = q2;
    return s.substr(q1 + 1, q2 - q1 - 1);
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// "0.3.1" > "0.2.0"? Anything that does not parse counts as not newer.
bool Newer(const std::wstring& a, const char* b) {
    int av[3] = {}, bv[3] = {};
    swscanf_s(a.c_str(), L"%d.%d.%d", &av[0], &av[1], &av[2]);
    sscanf_s(b, "%d.%d.%d", &bv[0], &bv[1], &bv[2]);
    for (int i = 0; i < 3; i++) {
        if (av[i] != bv[i]) return av[i] > bv[i];
    }
    return false;
}

DWORD WINAPI CheckThread(void*) {
    std::vector<uint8_t> body;
    if (!HttpGet(ApiUrl(), body, 1u << 20, false)) return 0;
    std::string json((const char*)body.data(), body.size());
    std::string tag = Between(json, "\"tag_name\"");
    if (tag.empty()) return 0;
    if (tag[0] == 'v' || tag[0] == 'V') tag.erase(0, 1);
    std::wstring version = Wide(tag);
    if (!Newer(version, FASTMD_VERSION_STR)) return 0;
    // the installer and the hash published next to it, both from this release
    Found f;
    f.version = version;
    for (size_t at = 0;;) {
        size_t end = 0;
        std::string url = Between(json, "\"browser_download_url\"", at, &end);
        if (url.empty()) break;
        at = end + 1;
        std::string lower = url;
        for (char& c : lower) c = (char)tolower((unsigned char)c);
        bool setup = lower.find("setup") != std::string::npos;
        if (setup && lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0) f.url = Wide(url);
        else if (setup && lower.size() > 7 && lower.compare(lower.size() - 7, 7, ".sha256") == 0) f.shaUrl = Wide(url);
    }
    if (f.url.empty() || g.closing) return 0;
    g_found = f;
    if (g.hwnd) PostMessageW(g.hwnd, WM_APP_UPDATE, 0, 0);
    return 0;
}

DWORD WINAPI DownloadThread(void*) {
    std::vector<uint8_t> exe, sha;
    if (!HttpGet(g_found.url, exe, 80u << 20, false) || exe.size() < 4096) return 0;
    std::string want;
    if (!g_found.shaUrl.empty() && HttpGet(g_found.shaUrl, sha, 4096, false)) {
        std::string text((const char*)sha.data(), sha.size());
        for (char& c : text) c = (char)tolower((unsigned char)c);
        size_t i = text.find_first_not_of(" \t\r\n");
        if (i != std::string::npos && text.size() - i >= 64) want = text.substr(i, 64);
    }
    std::string got = Sha256Hex(exe.data(), exe.size());
    if (want.empty() || want != got) {  // no hash, or not the file the release promised: stop
        if (g.hwnd) PostMessageW(g.hwnd, WM_APP_UPDATE, 2, 0);
        return 0;
    }
    std::wstring dir = DataDir() + L"update";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\FastMD-Setup.exe";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD wrote = 0;
    bool ok = WriteFile(f, exe.data(), (DWORD)exe.size(), &wrote, nullptr) && wrote == exe.size();
    CloseHandle(f);
    if (!ok) return 0;
    g_downloaded = path;
    if (g.hwnd) PostMessageW(g.hwnd, WM_APP_UPDATE, g_testOnly ? 3 : 1, 0);
    return 0;
}
}  // namespace

// after the first frame; does nothing more than once a day, and nothing at all when switched off
void UpdateCheckAsync() {
    if (!g.cfg.updateCheck || BenchActive() || g.closing) return;
    uint64_t now = NowTicks(), last = LastUpdateCheck();
    if (last && now - last < kDay) return;
    SetLastUpdateCheck(now);
    Spawn(CheckThread, nullptr, THREAD_PRIORITY_BELOW_NORMAL, 128 * 1024);
}

bool UpdateAvailable() { return !g_found.version.empty(); }
std::wstring UpdateVersion() { return g_found.version; }

// the menu item: ask, download, check the hash, run the installer
void UpdateInstall() {
    if (g_found.version.empty()) return;
    if (!g_downloaded.empty()) {  // already here and checked: just run it
        ShellExecuteW(g.hwnd, L"open", g_downloaded.c_str(), L"/S", nullptr, SW_SHOWNORMAL);
        ShowToast(Tr(S_UPDATE_INSTALLING), 3000);
        return;
    }
    wchar_t msg[1024];
    swprintf_s(msg, Tr(S_UPDATE_CONFIRM), g_found.version.c_str(), FASTMD_VERSION_WSTR);
    if (MessageBoxW(g.hwnd, msg, L"FastMD", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    ShowToast(Tr(S_UPDATE_DOWNLOADING), 4000);
    Spawn(DownloadThread, nullptr, THREAD_PRIORITY_BELOW_NORMAL, 256 * 1024);
}

// WM_APP_UPDATE: 0 = a newer version exists, 1 = the installer is here and checked, 2 = it failed, 3 = test fetch
void OnUpdateMessage(WPARAM what) {
    if (what == 0) ShowToast(Tr(S_UPDATE_FOUND) + g_found.version, 4000);
    else if (what == 1) UpdateInstall();  // runs the installer that has just been verified
    else if (what == 2) ShowToast(Tr(S_UPDATE_FAILED), 3000);
}

// automation only: fetch and verify the installer without running it (Q_UPDATE)
void UpdateFetchForTest() {
    if (g_found.url.empty()) return;
    g_testOnly = true;
    Spawn(DownloadThread, nullptr, THREAD_PRIORITY_BELOW_NORMAL, 256 * 1024);
}

// automation only: is the verified installer on disk?
bool UpdateFetched() { return !g_downloaded.empty(); }
