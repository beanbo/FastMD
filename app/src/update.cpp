// Checking for a new version, and installing it if the reader says yes (plan 5.6).
//
// Once a day, after the first frame, on a worker thread: one HTTPS request to the GitHub releases API. Nothing is
// sent about the reader - no identifiers, no telemetry - and nothing is downloaded until they agree. The installer
// that comes down is checked against the SHA-256 published beside it in the same release before it is ever run, and
// it is the same per-user installer as always: it replaces the files in %LOCALAPPDATA%\Programs\FastMD and leaves
// open windows alone.
//
// A version once found is remembered (UpdateVersion in the registry), so every later window knows about it at once -
// a dot on the gear button, the button in the settings, the context menu - without asking GitHub again, until this
// FastMD is that version. A check that fails is tried again an hour later, not the next day. The settings window
// has a button that checks right away and then installs; after the installer is done, it restarts FastMD.
//
// The daily check can be switched off in the settings ("Проверять обновления"), and it never runs in bench mode.
#include "app.h"

#include "net.h"
#include "version.h"

#include <memory>
#include <shellapi.h>
#include <shlobj.h>

namespace {
const wchar_t* kApi = L"https://api.github.com/repos/beanbo/FastMD/releases/latest";
const uint64_t kHour = 60ull * 60 * 10'000'000;  // FILETIME ticks
const uint64_t kDay = 24 * kHour;
const uint64_t kRetry = kHour;                    // a check that failed is tried again after this

// WM_APP_UPDATE wParam
enum : WPARAM { UM_CHECKED = 0, UM_DOWNLOADED, UM_DOWNLOAD_FAILED, UM_TEST_FETCHED, UM_INSTALLED };

struct Found {
    std::wstring version, url, shaUrl;
};
// a check's answer, handed from the worker to the window thread (lParam of UM_CHECKED)
struct CheckResult {
    bool ok = false;  // GitHub answered with a release
    bool manual = false;
    Found found;      // version is empty when the release is not newer than this FastMD
};

Found g_found;              // UI thread only: the newer release, from a check or from the registry
UpdateStatus g_status = US_IDLE;
bool g_loaded = false;      // the remembered version has been read from the registry
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

// The updater's threads are not document workers: opening another document must never wait for a download or an
// installer (JoinWorkers waits for everything Spawn started), so they run detached and only ever post messages.
void Detached(LPTHREAD_START_ROUTINE fn, void* arg, SIZE_T stack) {
    HANDLE th = CreateThread(nullptr, stack, fn, arg, 0, nullptr);
    if (!th) return;
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(th);
}

void Post(WPARAM what, LPARAM lp = 0) {
    if (!g.hwnd || !PostMessageW(g.hwnd, WM_APP_UPDATE, what, lp)) {
        if (what == UM_CHECKED) delete (CheckResult*)lp;
    }
}

DWORD WINAPI CheckThread(void* arg) {
    auto* r = (CheckResult*)arg;
    std::vector<uint8_t> body;
    if (HttpGet(ApiUrl(), body, 1u << 20, false)) {
        std::string json((const char*)body.data(), body.size());
        std::string tag = Between(json, "\"tag_name\"");
        if (!tag.empty()) {
            r->ok = true;
            if (tag[0] == 'v' || tag[0] == 'V') tag.erase(0, 1);
            std::wstring version = Wide(tag);
            if (Newer(version, FASTMD_VERSION_STR)) {
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
                    else if (setup && lower.size() > 7 && lower.compare(lower.size() - 7, 7, ".sha256") == 0)
                        f.shaUrl = Wide(url);
                }
                if (!f.url.empty()) r->found = f;  // a release with nothing to install counts as nothing new
            }
        }
    }
    // Remembered here, not only by the window: one that is closed a second after it opened still leaves the answer
    // for the next (the window thread repeats these writes when the message arrives, which is harmless)
    if (!r->ok) SetLastUpdateCheck(NowTicks() - kDay + kRetry);
    else if (r->found.version.empty()) SaveFoundUpdate(L"", L"", L"");
    else SaveFoundUpdate(r->found.version, r->found.url, r->found.shaUrl);
    if (g.closing) {
        delete r;
        return 0;
    }
    Post(UM_CHECKED, (LPARAM)r);
    return 0;
}

DWORD WINAPI DownloadThread(void* arg) {
    Found* f = (Found*)arg;
    std::vector<uint8_t> exe, sha;
    bool ok = HttpGet(f->url, exe, 80u << 20, false) && exe.size() >= 4096;
    std::string want;
    if (ok && !f->shaUrl.empty() && HttpGet(f->shaUrl, sha, 4096, false)) {
        std::string text((const char*)sha.data(), sha.size());
        for (char& c : text) c = (char)tolower((unsigned char)c);
        size_t i = text.find_first_not_of(" \t\r\n");
        if (i != std::string::npos && text.size() - i >= 64) want = text.substr(i, 64);
    }
    delete f;
    // no hash, or not the file the release promised: stop
    if (!ok || want.empty() || want != Sha256Hex(exe.data(), exe.size())) {
        Post(UM_DOWNLOAD_FAILED);
        return 0;
    }
    std::wstring dir = DataDir() + L"update";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\FastMD-Setup.exe";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Post(UM_DOWNLOAD_FAILED);
        return 0;
    }
    DWORD wrote = 0;
    ok = WriteFile(h, exe.data(), (DWORD)exe.size(), &wrote, nullptr) && wrote == exe.size();
    CloseHandle(h);
    Post(ok ? (g_testOnly ? UM_TEST_FETCHED : UM_DOWNLOADED) : UM_DOWNLOAD_FAILED);
    return 0;
}

// the installer runs silently; this waits for it and reports how it ended (0 = installed)
DWORD WINAPI InstallWaitThread(void* process) {
    DWORD code = 1;
    if (WaitForSingleObject((HANDLE)process, 5 * 60 * 1000) != WAIT_OBJECT_0 || !GetExitCodeProcess((HANDLE)process, &code))
        code = 1;
    CloseHandle((HANDLE)process);
    Post(UM_INSTALLED, (LPARAM)code);
    return 0;
}

// the remembered release, once per window; forgotten as soon as this FastMD is that version (or newer)
void LoadRemembered() {
    if (g_loaded) return;
    g_loaded = true;
    Found f;
    LoadFoundUpdate(f.version, f.url, f.shaUrl);
    if (f.version.empty()) return;
    if (!Newer(f.version, FASTMD_VERSION_STR) || f.url.empty()) {
        SaveFoundUpdate(L"", L"", L"");
        return;
    }
    if (g_found.version.empty()) {
        g_found = f;
        if (g_status == US_IDLE || g_status == US_LATEST) g_status = US_AVAILABLE;
    }
}

void Changed() {  // the gear's dot and the settings' button follow the state
    SettingsRefresh();
    Invalidate();
}

void StartCheck(bool manual) {
    g_status = US_CHECKING;
    SetLastUpdateCheck(NowTicks());  // taken before asking: other windows opened meanwhile do not ask again
    Detached(CheckThread, new CheckResult{false, manual, {}}, 128 * 1024);
    Changed();
}

void StartDownload() {
    g_status = US_DOWNLOADING;
    Detached(DownloadThread, new Found(g_found), 256 * 1024);
    Changed();
}

void RunInstaller() {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = g.hwnd;
    sei.lpVerb = L"open";
    sei.lpFile = g_downloaded.c_str();
    sei.lpParameters = L"/S";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        g_status = US_DOWNLOAD_FAILED;
        ShowToast(Tr(S_UPDATE_FAILED), 3000);
        Changed();
        return;
    }
    g_status = US_INSTALLING;
    ShowToast(Tr(S_UPDATE_INSTALLING), 3000);
    Detached(InstallWaitThread, sei.hProcess, 64 * 1024);
    Changed();
}
}  // namespace

// after the first frame, and every hour while the window stays open (TIMER_UPDATE); asks GitHub no more than once a
// day, and not at all when switched off
void UpdateCheckAsync() {
    if (!g.cfg.updateCheck || BenchActive() || g.closing) return;
    if (g.hwnd) SetTimer(g.hwnd, TIMER_UPDATE, 60 * 60 * 1000, nullptr);  // a window left open for days asks again
    if (!g_loaded) {
        LoadRemembered();
        if (g_status == US_AVAILABLE) Changed();  // the dot on the gear, at once
    }
    if (g_status == US_CHECKING || g_status == US_DOWNLOADING || g_status == US_INSTALLING || g_status == US_INSTALLED)
        return;
    uint64_t now = NowTicks(), last = LastUpdateCheck();
    if (last && now >= last && now - last < kDay) return;
    StartCheck(false);
}

// the settings button: check now, whatever the time of the last check and the daily switch say
void UpdateCheckNow() {
    if (BenchActive() || g.closing) return;
    if (g_status == US_CHECKING || g_status == US_DOWNLOADING || g_status == US_INSTALLING) return;
    StartCheck(true);
}

bool UpdateAvailable() { return !g_found.version.empty() && g_status != US_INSTALLED; }
std::wstring UpdateVersion() { return g_found.version; }
UpdateStatus UpdateGetStatus() { return g_status; }

// the menu item and the settings button: ask (unless the button already said which version), download, check the
// hash, run the installer
void UpdateInstall(bool ask) {
    if (g_found.version.empty()) return;
    if (g_status == US_DOWNLOADING || g_status == US_INSTALLING) return;
    if (g_status == US_INSTALLED) {
        UpdateRestart();
        return;
    }
    if (!g_downloaded.empty()) {  // already here and checked: just run it
        RunInstaller();
        return;
    }
    if (ask) {
        wchar_t msg[1024];
        swprintf_s(msg, Tr(S_UPDATE_CONFIRM), g_found.version.c_str(), FASTMD_VERSION_WSTR);
        HWND owner = SettingsHwnd() ? SettingsHwnd() : g.hwnd;
        if (MessageBoxW(owner, msg, L"FastMD", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    ShowToast(Tr(S_UPDATE_DOWNLOADING), 4000);
    StartDownload();
}

// FastMD again, as the version just installed, with this document at this place (the reading position is saved
// first, so the new window restores it); this window closes the ordinary way
void UpdateRestart() {
    PWSTR local = nullptr;
    std::wstring exe;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local)
        exe = std::wstring(local) + L"\\Programs\\FastMD\\FastMD.exe";
    CoTaskMemFree(local);
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShowToast(Tr(S_UPDATE_FAILED), 3000);
        return;
    }
    if (!PrepareToClose()) return;  // unsaved edits that could not be written: stay
    SaveReadingPosition();
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!g.path.empty()) cmd += L" \"" + g.path + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        ShowToast(Tr(S_UPDATE_FAILED), 3000);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

void OnUpdateMessage(WPARAM what, LPARAM lp) {
    switch (what) {
    case UM_CHECKED: {
        std::unique_ptr<CheckResult> r((CheckResult*)lp);
        if (!r) return;
        // the reader may have started the remembered version's download while this check ran: that goes on
        bool busy = g_status == US_DOWNLOADING || g_status == US_INSTALLING || g_status == US_INSTALLED;
        if (!r->ok) {
            // GitHub could not be reached: try again in an hour, not tomorrow; a newer version known from before stays
            SetLastUpdateCheck(NowTicks() - kDay + kRetry);
            if (!busy) g_status = g_found.version.empty() ? US_CHECK_FAILED : US_AVAILABLE;
            if (r->manual) ShowToast(Tr(S_UPDATE_CHECK_FAILED), 3000);
        } else if (r->found.version.empty()) {
            SaveFoundUpdate(L"", L"", L"");
            if (!busy) {
                g_found = Found{};
                g_status = US_LATEST;
            }
            if (r->manual) ShowToast(Tr(S_UPDATE_LATEST), 2500);
        } else if (!busy) {
            bool fresh = r->found.version != g_found.version;
            g_found = r->found;
            g_status = US_AVAILABLE;
            SaveFoundUpdate(g_found.version, g_found.url, g_found.shaUrl);
            if (fresh || r->manual) ShowToast(Tr(S_UPDATE_FOUND) + g_found.version + Tr(S_UPDATE_FOUND_HINT), 6000);
        }
        Changed();
        break;
    }
    case UM_DOWNLOADED:
        g_downloaded = DataDir() + L"update\\FastMD-Setup.exe";
        RunInstaller();  // the installer that has just been verified
        break;
    case UM_TEST_FETCHED:
        g_downloaded = DataDir() + L"update\\FastMD-Setup.exe";
        g_status = US_AVAILABLE;
        Changed();
        break;
    case UM_DOWNLOAD_FAILED:
        g_status = US_DOWNLOAD_FAILED;
        ShowToast(Tr(S_UPDATE_FAILED), 3000);
        Changed();
        break;
    case UM_INSTALLED: {
        if (lp != 0) {  // the installer failed or was stopped
            g_status = US_DOWNLOAD_FAILED;
            g_downloaded.clear();
            ShowToast(Tr(S_UPDATE_INSTALL_FAILED), 4000);
            Changed();
            break;
        }
        g_status = US_INSTALLED;
        Changed();
        wchar_t msg[512];
        swprintf_s(msg, Tr(S_UPDATE_RESTART_ASK), g_found.version.c_str());
        HWND owner = SettingsHwnd() ? SettingsHwnd() : g.hwnd;
        if (MessageBoxW(owner, msg, L"FastMD", MB_YESNO | MB_ICONINFORMATION) == IDYES) UpdateRestart();
        break;
    }
    }
}

// automation only: fetch and verify the installer without running it (Q_UPDATE)
void UpdateFetchForTest() {
    if (g_found.url.empty()) return;
    g_testOnly = true;
    g_status = US_DOWNLOADING;
    Detached(DownloadThread, new Found(g_found), 256 * 1024);
}

// automation only: is the verified installer on disk?
bool UpdateFetched() { return !g_downloaded.empty(); }
