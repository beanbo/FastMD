// Edit mode's file half (docs/EDIT-MODE.md §10.1, §10.3-§10.5): the baseline, byte-exact encoding, the write and the
// recovery file around it.
//
// A reader must never damage what it shows (tasks.cpp:1-5), and an editor has more ways to. So the write keeps every
// byte outside the edited region as it was: the text is encoded again only from the first byte that differs to the
// last one, in the file's own code page, and the result is decoded back and compared with the text before a single
// byte of the file changes. The file is rewritten in place (its identity, its links, its permissions stay), and while
// that happens a recovery file holds the bytes that are being replaced, so an interrupted save can be undone.
//
// No `g`, no window: fastmd-edit-tests runs all of it, the faults of FASTMD_TEST_FAIL_WRITE included.
#include "editfile.h"

#include <cstdio>

namespace {
bool HighSur(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool LowSur(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
uint64_t U64(DWORD hi, DWORD lo) { return ((uint64_t)hi << 32) | lo; }
double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}

// ------------------------------------------------------------------------------------------------ faults
// FASTMD_TEST_FAIL_WRITE: one of these fails where it names, once (",always": every time); ",norollback" leaves a
// torn file and its recovery file behind, which is how the recovery strip is tested.
enum FaultKind : uint8_t { FK_NONE, FK_PARTIAL, FK_BUSY, FK_DENIED, FK_MISSING, FK_SHORT_READ, FK_FLUSH, FK_CLOSE,
                           FK_RECOVERY, FK_DISKFULL };
struct Fault { FaultKind kind = FK_NONE; uint64_t n = 0; bool always = false, norollback = false; };
Fault g_fault;
bool g_faultRead = false;

void ParseFault(const wchar_t* spec) {
    static const wchar_t* names[] = {L"", L"partial", L"busy", L"denied", L"missing", L"short_read", L"flush",
                                     L"close", L"recovery", L"diskfull"};
    g_fault = Fault();
    std::wstring v = spec ? spec : L"";
    for (size_t at = 0, k = 0; at <= v.size(); k++) {
        size_t e = v.find(L',', at);
        if (e == std::wstring::npos) e = v.size();
        std::wstring t = v.substr(at, e - at);
        if (k == 0) {
            size_t c = t.find(L':');
            for (int i = 1; i < (int)std::size(names); i++)
                if (t.compare(0, c, names[i]) == 0) g_fault.kind = (FaultKind)i;
            if (c != std::wstring::npos) g_fault.n = wcstoull(t.c_str() + c + 1, nullptr, 10);
        } else if (t == L"always") {
            g_fault.always = true;
        } else if (t == L"norollback") {
            g_fault.norollback = true;
        }
        at = e + 1;
    }
}

// true when the fault of this kind strikes now
bool Fires(FaultKind k) {
    if (!g_faultRead) {
        g_faultRead = true;
        wchar_t v[128];
        DWORD n = GetEnvironmentVariableW(L"FASTMD_TEST_FAIL_WRITE", v, (DWORD)std::size(v));
        if (n && n < std::size(v)) ParseFault(v);
    }
    if (g_fault.kind != k) return false;
    if (!g_fault.always) g_fault.kind = FK_NONE;
    return true;
}

// ------------------------------------------------------------------------------------------------ error classes
bool OnNetwork(const wchar_t* path) { return IsNetworkPath(path); }

bool FolderExists(const wchar_t* path) {
    std::wstring dir = DirOf(path);
    DWORD a = dir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// §10.4: what an error of the open means for the next step - wait and retry, ask, or give up
SaveState ErrorClass(DWORD e, const wchar_t* path) {
    switch (e) {
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_NETNAME_DELETED: case ERROR_BAD_NETPATH:
    case ERROR_UNEXP_NET_ERR: case ERROR_SEM_TIMEOUT:
    case ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING: case ERROR_CLOUD_FILE_IN_USE: case ERROR_CLOUD_FILE_UNSUCCESSFUL:
    case ERROR_CLOUD_FILE_REQUEST_ABORTED: case ERROR_CLOUD_FILE_REQUEST_CANCELED: case ERROR_CLOUD_FILE_REQUEST_TIMEOUT:
    case ERROR_CLOUD_FILE_PROVIDER_TERMINATED: case ERROR_CLOUD_FILE_NETWORK_UNAVAILABLE:
        return SS_BUSY;
    case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: return SS_DENIED;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND:
        // the file is gone from a folder that is there; a folder that is gone on a share is the share being away
        if (FolderExists(path)) return SS_MISSING;
        return OnNetwork(path) ? SS_BUSY : SS_MISSING;
    }
    return SS_FAILED;
}

// The whole file through an open handle, in 1 MB chunks. A short read, or a size that moved meanwhile, is UNKNOWN:
// never taken as the file's content (D5).
SaveState ReadAll(HANDLE f, std::string& bytes, BY_HANDLE_FILE_INFORMATION* info, DWORD* err, bool saving) {
    bytes.clear();
    if (!GetFileInformationByHandle(f, info)) { *err = GetLastError(); return SS_UNKNOWN; }
    uint64_t size = U64(info->nFileSizeHigh, info->nFileSizeLow);
    if (size > (1ull << 30)) { *err = ERROR_FILE_TOO_LARGE; return SS_FAILED; }  // the loader's limit
    bytes.resize((size_t)size);
    size_t got = 0;
    while (got < size) {
        DWORD n = 0, want = (DWORD)std::min<uint64_t>(1u << 20, size - got);
        if (!ReadFile(f, bytes.data() + got, want, &n, nullptr)) { *err = GetLastError(); break; }
        if (!n) break;
        got += n;
    }
    if (saving && got && Fires(FK_SHORT_READ)) got--;  // the fault is the save's read, not the baseline's
    LARGE_INTEGER now{};
    if (got != size || !GetFileSizeEx(f, &now) || (uint64_t)now.QuadPart != size) {
        bytes.resize(got);
        if (!*err) *err = ERROR_READ_FAULT;
        return SS_UNKNOWN;
    }
    return SS_SAVED;
}

void Identity(HANDLE f, const wchar_t* path, const BY_HANDLE_FILE_INFORMATION& fi, DiskState& d) {
    d.volume = fi.dwVolumeSerialNumber;
    d.index = U64(fi.nFileIndexHigh, fi.nFileIndexLow);
    d.mtime = fi.ftLastWriteTime;
    d.size = U64(fi.nFileSizeHigh, fi.nFileSizeLow);
    d.attributes = fi.dwFileAttributes;
    FILE_REMOTE_PROTOCOL_INFO rp{};
    d.remote = GetFileInformationByHandleEx(f, FileRemoteProtocolInfo, &rp, sizeof rp) || OnNetwork(path);
    d.cloud = (fi.dwFileAttributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_PINNED |
                                      FILE_ATTRIBUTE_UNPINNED)) != 0;
}

// Code pages whose encoder takes no flags (§10.1): UTF-8, GB18030, the symbol page and the stateful ones.
bool FlagsFree(UINT cp) {
    return cp == CP_UTF8 || cp == 54936 || cp == 42 || StatefulCodePage(cp);
}

// the byte count of text in cp - the same call the encoder makes, without the bytes
size_t EncodedLen(UINT cp, const wchar_t* p, size_t n) {
    if (!n) return 0;
    if (cp == 1200) return n * 2;
    bool nbf = !FlagsFree(cp);
    BOOL used = FALSE;
    int m = WideCharToMultiByte(cp, nbf ? WC_NO_BEST_FIT_CHARS : 0, p, (int)n, nullptr, 0, nullptr, nbf ? &used : nullptr);
    return m > 0 ? (size_t)m : SIZE_MAX;
}

// one character (or surrogate pair) survives the trip into cp and back
bool RoundTrips(UINT cp, const wchar_t* p, size_t n) {
    bool nbf = !FlagsFree(cp);
    BOOL used = FALSE;
    char buf[16];
    int m = WideCharToMultiByte(cp, nbf ? WC_NO_BEST_FIT_CHARS : 0, p, (int)n, buf, sizeof buf, nullptr, nbf ? &used : nullptr);
    if (m <= 0 || used) return false;
    wchar_t back[8];
    int k = MultiByteToWideChar(cp, 0, buf, m, back, 8);
    return k == (int)n && wmemcmp(back, p, n) == 0;
}

// the code page non-UTF-8 bytes of this file are read in: the one it was edited as, else the system's
UINT DecodeAcp(const DiskState& d) { return d.cp != CP_UTF8 && d.cp != 1200 ? d.cp : AnsiCodePage(); }

// the volumes' flush cost, measured on their first three flushes this session (§10.3 step 7)
struct VolFlush { uint32_t vol; int n; double ms[3]; };
VolFlush g_vols[16];
int g_volCount = 0;

VolFlush* VolumeCost(uint32_t vol) {
    for (int i = 0; i < g_volCount; i++)
        if (g_vols[i].vol == vol) return &g_vols[i];
    if (g_volCount == (int)std::size(g_vols)) return nullptr;
    g_vols[g_volCount] = VolFlush{vol, 0, {}};
    return &g_vols[g_volCount++];
}

int g_flushPolicy = 0;  // SetFlushPolicyForTests

// a local volume is flushed after every save while flushing it is cheap (the median of its first three flushes under
// 5 ms, and those three to find out); otherwise only at the flush points
bool FlushEverySave(uint32_t vol) {
    if (g_flushPolicy == 1) return false;
    VolFlush* v = VolumeCost(vol);
    if (!v || v->n < 3) return true;
    double a = v->ms[0], b = v->ms[1], c = v->ms[2];
    double median = std::max(std::min(a, b), std::min(std::max(a, b), c));
    return median < 5.0;
}

void RecordFlush(uint32_t vol, double ms) {
    VolFlush* v = VolumeCost(vol);
    if (v && v->n < 3) v->ms[v->n++] = ms;
}

// Strict UTF-8 over bytes that come in pieces - what MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS) takes: no
// overlong forms, no surrogates, nothing past U+10FFFF, no sequence cut off at the end.
struct Utf8Check {
    int need = 0;
    uint8_t lo = 0x80, hi = 0xBF;
    bool bad = false;
    void Feed(const char* p, uint64_t n) {
        for (uint64_t i = 0; i < n && !bad; i++) {
            uint8_t c = (uint8_t)p[i];
            if (need) {
                if (c < lo || c > hi) bad = true;
                lo = 0x80;
                hi = 0xBF;
                need--;
            } else if (c >= 0x80) {
                if (c >= 0xC2 && c <= 0xDF) need = 1;
                else if (c == 0xE0) { need = 2; lo = 0xA0; }
                else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) need = 2;
                else if (c == 0xED) { need = 2; hi = 0x9F; }
                else if (c == 0xF0) { need = 3; lo = 0x90; }
                else if (c >= 0xF1 && c <= 0xF3) need = 3;
                else if (c == 0xF4) { need = 3; hi = 0x8F; }
                else bad = true;
            }
        }
    }
    bool Valid() const { return !bad && !need; }
};

bool WriteAllAt(HANDLE f, uint64_t at, const char* p, size_t n, DWORD* err) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)at;
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) { *err = GetLastError(); return false; }
    for (size_t done = 0; done < n;) {
        DWORD put = 0, want = (DWORD)std::min<size_t>(1u << 20, n - done);
        if (!WriteFile(f, p + done, want, &put, nullptr) || put != want) {
            *err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            return false;
        }
        done += put;
    }
    return true;
}

bool SetLength(HANDLE f, uint64_t len, DWORD* err) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)len;
    if (SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && SetEndOfFile(f)) return true;
    *err = GetLastError();
    return false;
}

// ------------------------------------------------------------------------------------------------ recovery files
// FMDREC2: magic, the document's path, the version the file restores (length, stamp, code page, byte-order mark), the
// range [pb, pe) of its bytes that follow, RangeHash of its bytes before, after and in that range, FNV-1a-64 of all
// of them, the writer (pid, process start), a hash of all that; then a result block (the file's length and hash as the
// last save that used this recovery file left it, and their hash) that later saves rewrite in place; then the bytes. So a
// leftover can tell a torn file from a finished save, from a file that moved on since, and from another file that now
// has the same identity - and a header or tail that did not reach the disk is seen as such.
const char kRecMagic[8] = {'F', 'M', 'D', 'R', 'E', 'C', '2', 0};

template <class T> void Put(std::string& s, T v) { s.append((const char*)&v, sizeof v); }
template <class T> bool Get(const std::string& s, size_t& at, T& v) {
    if (at > s.size() || s.size() - at < sizeof v) return false;
    memcpy(&v, s.data() + at, sizeof v);
    at += sizeof v;
    return true;
}

// The hash of one range of bytes for a recovery file's checks (the bytes it holds, the ones before and after them):
// 8 bytes per step with a fold of the high half, several times faster than the byte-wise FNV-1a of the baseline -
// a tick at the top of a big file hashes the rest of it. Never chained across calls.
uint64_t RangeHash(const char* p, size_t n) {
    uint64_t h = 14695981039346656037ull ^ n;
    size_t i = 0;
    for (uint64_t w; i + 8 <= n; i += 8) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 32;
    }
    for (; i < n; i++) h = (h ^ (uint8_t)p[i]) * 1099511628211ull;
    return h;
}

uint64_t ProcessStart() {
    static const uint64_t t = [] {
        FILETIME c{}, x{}, k{}, u{};
        GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
        return U64(c.dwHighDateTime, c.dwLowDateTime);
    }();
    return t;
}
uint64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return U64(ft.dwHighDateTime, ft.dwLowDateTime);
}

std::string RecHeader(const RecoveryInfo& r) {
    std::string h(kRecMagic, sizeof kRecMagic);
    Put<uint32_t>(h, (uint32_t)r.path.size());
    h.append((const char*)r.path.data(), r.path.size() * sizeof(wchar_t));
    Put<uint64_t>(h, r.preSize);
    Put<uint64_t>(h, r.mtime);
    Put<uint32_t>(h, r.cp);
    Put<uint32_t>(h, (uint32_t)r.header.size());
    h += r.header;
    for (uint64_t v : {r.pb, r.pe, r.prefixHash, r.suffixHash, r.oldHash, r.tailHash}) Put<uint64_t>(h, v);
    Put<uint32_t>(h, r.pid);
    Put<uint64_t>(h, r.created);
    Put<uint64_t>(h, Fnv64(h.data(), h.size()));
    return h;
}
std::string RecResult(uint64_t len, uint64_t hash) {
    std::string s;
    Put<uint64_t>(s, len);
    Put<uint64_t>(s, hash);
    Put<uint64_t>(s, Fnv64(s.data(), s.size()));
    return s;
}

// A new recovery file for r (its bytes in `tail`), created under a name nobody has used and flushed before the target
// changes (§10.5). An encrypted document gets an encrypted one (D22). Fills r's file, writer and offsets.
bool WriteRecovery(const std::wstring& recoveryDir, uint32_t vol, uint64_t index, bool encrypted, RecoveryInfo& r,
                   const std::string& tail, DWORD* err) {
    if (recoveryDir.empty()) { *err = ERROR_PATH_NOT_FOUND; return false; }
    std::wstring dir = recoveryDir;
    if (dir.back() != L'\\') dir += L'\\';
    CreateDirectoryW(DirOf(dir.substr(0, dir.size() - 1)).c_str(), nullptr);  // the data folder itself
    CreateDirectoryW(dir.c_str(), nullptr);
    if (Fires(FK_RECOVERY)) { *err = ERROR_WRITE_FAULT; return false; }
    r.pid = GetCurrentProcessId();
    r.created = ProcessStart();
    r.tailHash = RangeHash(tail.data(), tail.size());
    r.tailLen = tail.size();
    r.hasResult = true;
    std::string h = RecHeader(r);
    r.resultOff = h.size();
    h += RecResult(r.newLen, r.newHash);
    r.tailOff = h.size();
    // never over another file: a kept one may be the only copy of the bytes it holds
    static uint32_t seq = 0;
    HANDLE f = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 256 && f == INVALID_HANDLE_VALUE; tries++) {
        r.file = dir + RecoveryName(vol, index, r.pid, seq++);
        f = CreateFileW(r.file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        encrypted ? FILE_ATTRIBUTE_ENCRYPTED : FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (f == INVALID_HANDLE_VALUE) { *err = GetLastError(); return false; }
    bool ok = WriteAllAt(f, 0, h.data(), h.size(), err) && WriteAllAt(f, h.size(), tail.data(), tail.size(), err);
    if (ok && !FlushFileBuffers(f)) { *err = GetLastError(); ok = false; }
    if (!CloseHandle(f)) ok = false;
    if (!ok) DeleteFileW(r.file.c_str());
    r.written = NowFileTime();
    return ok;
}

// A later save used a recovery file held between flushes: what the target holds now goes into its result block. Not
// flushed - a stale one only means the file is later shown as torn or changed, never deleted unasked.
void WriteResult(RecoveryInfo& r, uint64_t len, uint64_t hash) {
    r.newLen = len;
    r.newHash = hash;
    r.hasResult = true;
    HANDLE f = CreateFileW(r.file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string s = RecResult(len, hash);
    DWORD e = 0;
    WriteAllAt(f, r.resultOff, s.data(), s.size(), &e);
    CloseHandle(f);
    r.written = NowFileTime();
}

// Another FastMD process that wrote a recovery file and still runs (the very process: pid and start time) deletes its
// own. This process's files are its own business, and a pid that was reused by now is not their writer.
bool LiveOtherWriter(DWORD pid, uint64_t created) {
    if (pid == GetCurrentProcessId() && created == ProcessStart()) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    FILETIME c{}, x{}, k{}, u{};
    bool live = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE && GetProcessTimes(h, &c, &x, &k, &u) &&
                U64(c.dwHighDateTime, c.dwLowDateTime) == created;
    wchar_t img[MAX_PATH * 2];
    DWORD n = (DWORD)std::size(img);
    if (live) live = QueryFullProcessImageNameW(h, 0, img, &n) && EndsWithI(img, L"\\FastMD.exe");
    CloseHandle(h);
    return live;
}

// the saved bytes of a recovery file, whole (their hash is checked by the caller)
bool ReadTail(const RecoveryInfo& r, std::string& tail) {
    if (r.tailLen > (1ull << 30)) return false;
    HANDLE h = CreateFileW(r.file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    tail.resize((size_t)r.tailLen);
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)r.tailOff;
    bool ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) != 0;
    for (size_t got = 0; ok && got < tail.size();) {
        DWORD n = 0, want = (DWORD)std::min<size_t>(1u << 20, tail.size() - got);
        ok = ReadFile(h, tail.data() + got, want, &n, nullptr) && n == want;
        got += n;
    }
    CloseHandle(h);
    return ok;
}

// what a restore would make of the file: bytes [0, pb), the saved ones, and for a save in place the rest as it is
uint64_t RestoredHash(const RecoveryInfo& r, const std::string& bytes, const std::string& tail) {
    uint64_t h = Fnv64(tail.data(), tail.size(), Fnv64(bytes.data(), (size_t)r.pb));
    if (r.pe < r.preSize) h = Fnv64(bytes.data() + r.pe, (size_t)(r.preSize - r.pe), h);
    return h;
}
}  // namespace

// ------------------------------------------------------------------------------------------------ baseline
void CountEols(const std::wstring& t, DiskState& d) {
    d.crlf = d.lf = d.cr = 0;
    for (size_t i = 0, n = t.size(); i < n; i++) {
        if (t[i] == L'\r') {
            if (i + 1 < n && t[i + 1] == L'\n') { d.crlf++; i++; }
            else d.cr++;
        } else if (t[i] == L'\n') {
            d.lf++;
        }
    }
}

const wchar_t* DiskEol(const DiskState& d) {
    if (!d.crlf && !d.lf && !d.cr) return L"\n";
    if (d.crlf >= d.lf && d.crlf >= d.cr) return L"\r\n";
    return d.lf >= d.cr ? L"\n" : L"\r";
}

uint64_t Fnv64(const void* p, size_t n, uint64_t h) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

bool StatefulCodePage(UINT cp) { return (cp >= 50220 && cp <= 50229) || (cp >= 57002 && cp <= 57011) || cp == 65000; }

bool EncodeText(UINT cp, const wchar_t* p, size_t n, std::string& out, size_t* bad, const char** reason) {
    out.clear();
    *bad = SIZE_MAX;
    *reason = "";
    if (!n) return true;
    if (cp == 1200) {  // UTF-16 LE holds every code unit, a lone surrogate too
        out.assign((const char*)p, n * sizeof(wchar_t));
        return true;
    }
    // A lone surrogate has no bytes in any other encoding; the encoder would put U+FFFD or '?' there without a word.
    for (size_t i = 0; i < n; i++) {
        if (HighSur(p[i]) && i + 1 < n && LowSur(p[i + 1])) { i++; continue; }
        if (HighSur(p[i]) || LowSur(p[i])) { *bad = i; *reason = "LONE_SURROGATE"; return false; }
    }
    bool nbf = !FlagsFree(cp);
    BOOL used = FALSE;
    DWORD flags = nbf ? WC_NO_BEST_FIT_CHARS : 0;
    int m = WideCharToMultiByte(cp, flags, p, (int)n, nullptr, 0, nullptr, nbf ? &used : nullptr);
    if (m > 0) {
        out.resize((size_t)m);
        m = WideCharToMultiByte(cp, flags, p, (int)n, out.data(), m, nullptr, nbf ? &used : nullptr);
    }
    if (m <= 0) { out.clear(); *reason = "ENCODER_ERROR"; return false; }
    out.resize((size_t)m);
    if (!used) return true;
    out.clear();
    *reason = "UNENCODABLE";
    for (size_t i = 0; i < n;) {  // which one it was
        size_t k = HighSur(p[i]) && i + 1 < n && LowSur(p[i + 1]) ? 2 : 1;
        if (!RoundTrips(cp, p + i, k)) { *bad = i; return false; }
        i += k;
    }
    *bad = 0;
    return false;
}

SaveState ReadDisk(const wchar_t* path, std::string& bytes, DiskState* d, DWORD* err) {
    *err = 0;
    bytes.clear();
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return ErrorClass(*err, path);
    }
    BY_HANDLE_FILE_INFORMATION fi{};
    SaveState st = ReadAll(f, bytes, &fi, err, false);
    if (st == SS_SAVED && d) Identity(f, path, fi, *d);
    CloseHandle(f);
    return st;
}

DiskRefusal DecodeDisk(const std::string& bytes, UINT acp, DiskState& out) {
    const char* p = bytes.data();
    size_t n = bytes.size();
    out.length = n;
    out.hash = Fnv64(p, n);
    out.text.clear();
    out.header.clear();
    if (n >= 2 && (uint8_t)p[0] == 0xFE && (uint8_t)p[1] == 0xFF) {
        out.cp = 1201;
        return DR_UTF16BE;
    }
    TextEncoding enc;
    DecodeTextCp(p, (int)n, acp, out.text, &enc);
    out.cp = enc.cp;
    out.header.assign(p, enc.header);
    CountEols(out.text, out);
    bool utf16 = enc.cp == 1200;
    if (!utf16 && memchr(p, 0, n)) return DR_BINARY;       // NUL bytes outside UTF-16: not a text file (D14)
    if (utf16 && (n - enc.header) % 2) return DR_UTF16ODD;  // the last code unit is cut in half
    if (StatefulCodePage(enc.cp)) return DR_STATEFUL;
    // Byte for byte: what the editor would write for this very text is exactly what is there. A lossy decode (mojibake,
    // a stray byte) fails here, and edit mode is refused instead of "converting" the file (D1).
    std::string round = out.header, body;
    size_t bad;
    const char* why;
    if (!EncodeText(enc.cp, out.text.data(), out.text.size(), body, &bad, &why)) return DR_LOSSY;
    round += body;
    return round.size() == n && memcmp(round.data(), p, n) == 0 ? DR_OK : DR_LOSSY;
}

// ------------------------------------------------------------------------------------------------ the write (§10.3)
SaveResult SaveSource(const SaveRequest& rq) {
    SaveResult r;
    const std::wstring& text = *rq.text;
    DWORD e = 0;
    // 1. Nobody else may write while the file is checked and changed; reading it stays allowed.
    HANDLE f = INVALID_HANDLE_VALUE;
    if (Fires(FK_BUSY)) e = ERROR_SHARING_VIOLATION;
    else if (Fires(FK_DENIED)) e = ERROR_ACCESS_DENIED;
    else if (Fires(FK_MISSING)) e = ERROR_FILE_NOT_FOUND;
    else {
        f = CreateFileW(rq.path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) e = GetLastError();
    }
    if (f == INVALID_HANDLE_VALUE) {
        r.error = e;
        r.state = ErrorClass(e, rq.path);
        r.reason = "open";
        return r;
    }
    // A recovery file this process holds between flushes (the version the last flush left on disk) is handed back
    // unchanged by every way out before step 4.
    const RecoveryInfo* pend = rq.pending && !rq.pending->file.empty() ? rq.pending : nullptr;
    auto give = [&](SaveState st, const char* why) {  // nothing has changed yet
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        r.state = st;
        r.reason = why;
        r.error = e;
        if (pend) r.pending = *pend;
        return r;
    };

    // 2. What is there now. The baseline's bytes: go on. Other bytes of the same text (re-encoded outside): their code
    // page, header and line ends from now on, if they can be written back byte for byte (D13). Other text: CONFLICT.
    std::string bytes;
    BY_HANDLE_FILE_INFORMATION fi{};
    SaveState st = ReadAll(f, bytes, &fi, &e, true);
    if (st != SS_SAVED) return give(st, "read");
    const DiskState* base = rq.disk;
    const uint64_t curHash = Fnv64(bytes.data(), bytes.size());
    if (bytes.size() != base->length || curHash != base->hash) {
        DiskState now;
        DiskRefusal dr = DecodeDisk(bytes, DecodeAcp(*base), now);
        if (now.text != base->text) return give(SS_CONFLICT, "conflict");
        if (dr != DR_OK) return give(SS_FAILED, "encoding changed");
        bool ascii = now.header.empty() && std::all_of(bytes.begin(), bytes.end(), [](char c) { return (uint8_t)c < 0x80; });
        if (ascii && base->cp != 1200) now.cp = base->cp;  // pure ASCII keeps the code page it was edited in (D13)
        Identity(f, rq.path, fi, now);
        now.valid = true;
        r.adopted = true;
        r.disk = std::move(now);
        base = &r.disk;
        // Written outside meanwhile: a recovery file held for the last flush describes bytes that are not there
        // any more (the whole file was rewritten), so it goes.
        if (pend) DeleteFileW(pend->file.c_str());
        pend = nullptr;
    }
    const std::wstring& old = base->text;
    // A whole rewrite may change the encoding (the encoding strip's "Save as UTF-8"): its code page and mark from here on.
    const bool whole = rq.whole;
    const UINT cp = whole && rq.toCp ? rq.toCp : base->cp;
    const std::string hdr = whole && rq.toCp ? rq.toHeader : base->header;

    // 3. The bytes that change: from the first differing character to the last, moved outwards so that neither end
    // splits a surrogate pair or a CRLF; the rest of the file keeps its bytes (D1). A whole rewrite changes them all.
    size_t on = old.size(), nn = text.size(), lim = std::min(on, nn), p = 0, s = 0;
    size_t pb = 0, sb = 0;
    if (!whole) {
        while (p < lim && old[p] == text[p]) p++;
        while (s < lim - p && old[on - 1 - s] == text[nn - 1 - s]) s++;
        auto splits = [](const std::wstring& t, size_t i) {
            return i > 0 && i < t.size() && ((HighSur(t[i - 1]) && LowSur(t[i])) || (t[i - 1] == L'\r' && t[i] == L'\n'));
        };
        while (p > 0 && (splits(old, p) || splits(text, p))) p--;
        while (s > 0 && (splits(old, on - s) || splits(text, nn - s))) s--;
        // The baseline is byte-exact, so its text up to p is exactly the bytes up to pb (and the same for the suffix).
        // Only the shorter side is encoded to count its bytes: the longer one is what the file's length leaves (a tick
        // at the top of a big file would otherwise encode all of it once more).
        const size_t body = bytes.size() - std::min(bytes.size(), base->header.size());
        size_t mid = EncodedLen(cp, old.data() + p, on - s - p), pl = SIZE_MAX;
        sb = SIZE_MAX;
        if (p <= s) pl = EncodedLen(cp, old.data(), p);
        else sb = EncodedLen(cp, old.data() + on - s, s);
        size_t known = p <= s ? pl : sb;
        if (mid != SIZE_MAX && known != SIZE_MAX && known + mid <= body) (p <= s ? sb : pl) = body - known - mid;
        pb = pl == SIZE_MAX ? SIZE_MAX : base->header.size() + pl;
        if (pb > bytes.size() || sb > bytes.size() - pb) return give(SS_FAILED, "ENCODER_ERROR");
    }
    std::string middle = whole ? hdr : std::string();
    {
        std::string enc;
        size_t bad = SIZE_MAX;
        const char* why = "";
        if (!EncodeText(cp, text.data() + p, nn - s - p, enc, &bad, &why)) {
            r.bad = bad == SIZE_MAX ? UINT32_MAX : (uint32_t)(p + bad);
            return give(strcmp(why, "ENCODER_ERROR") ? SS_UNENCODABLE : SS_FAILED, why);
        }
        middle += enc;
    }
    const uint64_t oldLen = bytes.size(), newLen = pb + middle.size() + sb;
    const char* suffix = bytes.data() + (oldLen - sb);
    auto outAt = [&](uint64_t at) -> uint8_t {  // byte `at` of the file as this save would leave it
        return at < pb ? (uint8_t)bytes[at] : at < pb + middle.size() ? (uint8_t)middle[at - pb] : (uint8_t)suffix[at - pb - middle.size()];
    };
    // The text after the byte-order mark (or at the start, without one) must not start to look like one: the next read
    // would take it for the mark. Without a mark that is EF BB BF, FF FE or FE FF; after a UTF-8 mark (an ANSI file
    // can have one) the read looks for FF FE once more.
    if (cp != 1200) {
        const uint64_t hs = hdr.size();
        uint8_t head[3] = {};
        uint64_t k = 0;
        for (; k < 3 && hs + k < newLen; k++) head[k] = outAt(hs + k);
        auto starts = [&](const char* m, uint64_t n) { return k >= n && memcmp(head, m, (size_t)n) == 0; };
        bool look = hs ? starts("\xFF\xFE", 2)
                       : (cp == CP_UTF8 && nn && text[0] == 0xFEFF) || starts("\xEF\xBB\xBF", 3) || starts("\xFF\xFE", 2) ||
                             starts("\xFE\xFF", 2);
        if (look) {
            r.bad = 0;
            return give(SS_UNENCODABLE, "BOM_LOOKALIKE");
        }
    }
    // The proof: the new file reads back as exactly this text - all of it below a million characters (and always under
    // FASTMD_EDIT_SELFCHECK), the changed window with whole characters on both sides above.
    uint64_t hash = Fnv64(suffix, sb, Fnv64(middle.data(), middle.size(), Fnv64(bytes.data(), pb)));
    std::wstring back;
    if (rq.fullProof || nn < (1u << 20) || whole) {
        std::string all;
        all.reserve((size_t)newLen);
        all.append(bytes, 0, pb);
        all += middle;
        all.append(suffix, sb);
        DecodeTextCp(all.data(), (int)all.size(), DecodeAcp(*base), back, nullptr);
        if (back != text) {
            size_t k = 0;
            while (k < back.size() && k < nn && back[k] == text[k]) k++;
            r.bad = (uint32_t)k;
            return give(SS_UNENCODABLE, "UNENCODABLE");
        }
    } else {
        if (cp == 1200) back.assign((const wchar_t*)middle.data(), middle.size() / 2);
        else {
            int k = MultiByteToWideChar(cp, 0, middle.data(), (int)middle.size(), nullptr, 0);
            back.resize(k > 0 ? k : 0);
            if (k > 0) MultiByteToWideChar(cp, 0, middle.data(), (int)middle.size(), back.data(), k);
        }
        auto cont = [&](uint64_t at) {  // a UTF-8 continuation byte where a character must start
            return cp == CP_UTF8 && at < newLen && (outAt(at) & 0xC0) == 0x80;
        };
        bool same = back.compare(0, std::wstring::npos, text, p, nn - s - p) == 0 && !cont(pb) && !cont(pb + middle.size());
        // An ANSI file must not become valid UTF-8 (or its bytes are read as UTF-8 next time: "Ã©" turns into "é").
        // Only bytes of 0x80 and up can make or break a UTF-8 sequence, so the whole file is looked at only when the
        // edit removes, adds or borders one.
        if (same && cp != CP_UTF8) {
            bool high = std::any_of(middle.begin(), middle.end(), [](char c) { return (uint8_t)c >= 0x80; }) ||
                        std::any_of(bytes.begin() + pb, bytes.begin() + (oldLen - sb), [](char c) { return (uint8_t)c >= 0x80; }) ||
                        (pb > 0 && (uint8_t)bytes[pb - 1] >= 0x80) || (sb > 0 && (uint8_t)suffix[0] >= 0x80);
            if (high) {
                Utf8Check u;
                const uint64_t hs = hdr.size();
                u.Feed(bytes.data() + hs, pb - hs);
                u.Feed(middle.data(), middle.size());
                u.Feed(suffix, sb);
                same = !u.Valid();
            }
        }
        if (!same) {
            r.bad = (uint32_t)p;
            return give(SS_UNENCODABLE, "UNENCODABLE");
        }
    }

    // 4. The recovery file: the bytes about to be replaced, on disk before the first byte of the target changes. A save
    // that keeps the length holds only the bytes it replaces ([pb, pe)); one that does not, everything from pb on.
    // While saves go unflushed (a slow local volume between flush points), one recovery file stands for the last
    // flushed version: a save inside the range it holds needs nothing new, one outside it gets a wider one, pieced
    // together from it and the bytes around (which have not changed since that flush), and the old one goes only once
    // the new one is safe.
    const bool inPlace = newLen == oldLen;
    const uint64_t pe = inPlace ? oldLen - sb : oldLen;
    if (pend && (pend->pb > oldLen || pend->pe > pend->preSize || (pend->pe < pend->preSize && oldLen != pend->preSize))) {
        DeleteFileW(pend->file.c_str());  // not the file it was kept for: it cannot restore anything here
        pend = nullptr;
    }
    RecoveryInfo rec;
    bool fresh = false;
    if (pend && pb >= pend->pb && (pend->pe == pend->preSize || (inPlace && pe <= pend->pe))) {
        rec = *pend;
    } else {
        std::string tail;
        if (pend) {
            std::string held;
            if (!ReadTail(*pend, held) || RangeHash(held.data(), held.size()) != pend->tailHash) return give(SS_FAILED, "recovery");
            const uint64_t lo = std::min<uint64_t>(pb, pend->pb);
            const uint64_t hi = pend->pe == pend->preSize || !inPlace ? pend->preSize : std::max(pend->pe, pe);
            tail.assign(bytes, (size_t)lo, (size_t)(pend->pb - lo));
            tail += held;
            if (hi > pend->pe) tail.append(bytes, (size_t)pend->pe, (size_t)(hi - pend->pe));
            rec.preSize = pend->preSize;
            rec.mtime = pend->mtime;
            rec.oldHash = pend->oldHash;
            rec.cp = pend->cp;
            rec.header = pend->header;
            rec.pb = lo;
            rec.pe = hi;
        } else {
            tail.assign(bytes, pb, (size_t)(pe - pb));
            rec.preSize = oldLen;
            rec.mtime = U64(fi.ftLastWriteTime.dwHighDateTime, fi.ftLastWriteTime.dwLowDateTime);
            rec.oldHash = curHash;
            rec.cp = base->cp;
            rec.header = base->header;
            rec.pb = pb;
            rec.pe = pe;
        }
        rec.prefixHash = RangeHash(bytes.data(), (size_t)rec.pb);
        rec.suffixHash = RangeHash(bytes.data() + rec.pe, (size_t)(rec.preSize - rec.pe));
        rec.path = rq.path;
        rec.newLen = newLen;  // what the file will hold once this save is through
        rec.newHash = hash;
        if (!WriteRecovery(rq.recoveryDir, fi.dwVolumeSerialNumber, U64(fi.nFileIndexHigh, fi.nFileIndexLow),
                           (fi.dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0, rec, tail, &e))
            return give(SS_FAILED, "recovery");
        fresh = true;
    }
    // After this save: the target holds `len` bytes of `h` (`torn`: neither the old nor the new ones), flushed or not.
    // The recovery files that are not needed any more go; the one still needed is kept - as the file of a torn save, or
    // as the one standing for the last flush.
    auto settle = [&](bool torn, bool flushed, uint64_t len, uint64_t h) {
        if (fresh && pend) DeleteFileW(pend->file.c_str());  // the fresh one holds all it held
        if (torn) {
            r.recoveryKept = rec.file;
        } else if (flushed || (!pend && len == oldLen && h == curHash && fresh)) {
            DeleteFileW(rec.file.c_str());  // on disk for good, or back to a flushed version
        } else {
            if (!fresh || len != rec.newLen || h != rec.newHash) WriteResult(rec, len, h);
            r.pending = rec;
        }
    };

    bool changed = false;  // bytes (or the length) of the target have changed: a failure from here on is rolled back
    auto fail = [&](const char* what, DWORD err) {
        r.state = SS_FAILED;
        r.reason = what;
        r.error = err;
        bool restored = !changed;
        if (changed && !g_fault.norollback) {
            // 9. The old bytes go back at pb and the old length is restored; if that fails too, the recovery file
            // stays - it is then the only copy of them.
            HANDLE h = f != INVALID_HANDLE_VALUE ? f
                                                 : CreateFileW(rq.path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                                               FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD e2 = 0;
            restored = h != INVALID_HANDLE_VALUE && WriteAllAt(h, pb, bytes.data() + pb, (size_t)(oldLen - pb), &e2) &&
                       SetLength(h, oldLen, &e2) && FlushFileBuffers(h);
            if (h != INVALID_HANDLE_VALUE && h != f) CloseHandle(h);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        f = INVALID_HANDLE_VALUE;
        settle(!restored, false, oldLen, curHash);
        return r;
    };

    // 5. A file that grows gets its new length first: a full disk fails here, before any content byte has changed.
    e = 0;
    if (newLen > oldLen) {
        if (Fires(FK_DISKFULL)) {
            e = ERROR_DISK_FULL;
        } else if (SetLength(f, newLen, &e)) {
            e = 0;
            changed = true;
        }
        if (e) return fail("disk full", e);  // nothing changed yet: only the recovery files are put right
    }
    // 6. The new bytes from pb on, then the length. An edit that keeps the byte length leaves the suffix alone: it is
    // already there.
    std::string tail = std::move(middle);
    if (newLen != oldLen) tail.append(suffix, sb);
    size_t put = tail.size();
    bool partial = Fires(FK_PARTIAL);
    if (partial) put = (size_t)std::min<uint64_t>(g_fault.n, tail.size());
    changed = true;
    if (!WriteAllAt(f, pb, tail.data(), put, &e)) return fail("write", e);
    if (partial) return fail("write", ERROR_WRITE_FAULT);
    if (!SetLength(f, newLen, &e)) return fail("write", e);
    // 7. Flushed always on remote and cloud files, at the flush points, and on every save while the volume flushes
    // cheaply.
    bool flushFault = Fires(FK_FLUSH);
    bool flushed = false;
    if (flushFault || rq.flushPoint || base->remote || base->cloud || FlushEverySave(fi.dwVolumeSerialNumber)) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        bool ok = !flushFault && FlushFileBuffers(f);
        QueryPerformanceCounter(&t1);
        if (!ok) return fail("flush", flushFault ? ERROR_WRITE_FAULT : GetLastError());
        r.flushMs = Ms(t0, t1);
        if (!base->remote && !base->cloud) RecordFlush(fi.dwVolumeSerialNumber, r.flushMs);
        flushed = true;
    }
    // 8. The new stamp through the same handle, before it is closed (NTFS moves the write time at WriteFile).
    BY_HANDLE_FILE_INFORMATION after{};
    if (!GetFileInformationByHandle(f, &after)) return fail("stamp", GetLastError());
    bool closed = CloseHandle(f) != 0;
    f = INVALID_HANDLE_VALUE;
    if (Fires(FK_CLOSE)) closed = false;
    if (!closed) return fail("close", ERROR_WRITE_FAULT);

    // 10. Saved: the recovery file goes once the bytes are flushed (until then it stays, standing for the last flush),
    // and the snapshot is what the disk holds.
    settle(false, flushed, newLen, hash);
    DiskState d;
    d.valid = true;
    d.text = text;
    d.cp = cp;
    d.header = hdr;
    d.length = newLen;
    d.hash = hash;
    d.volume = after.dwVolumeSerialNumber;
    d.index = U64(after.nFileIndexHigh, after.nFileIndexLow);
    d.mtime = after.ftLastWriteTime;
    d.size = U64(after.nFileSizeHigh, after.nFileSizeLow);
    d.attributes = after.dwFileAttributes;
    d.remote = base->remote;
    d.cloud = base->cloud;
    CountEols(text, d);
    r.disk = std::move(d);
    r.state = SS_SAVED;
    r.reason = "";
    return r;
}

// ------------------------------------------------------------------------------------------------ recovery files
std::wstring RecoveryName(uint32_t volume, uint64_t index, DWORD pid, uint32_t n) {
    wchar_t b[80];
    swprintf_s(b, L"%08x-%016llx-%lu-%u.rec", volume, (unsigned long long)index, (unsigned long)pid, n);
    return b;
}

bool ReadRecovery(const std::wstring& file, RecoveryInfo& out) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    FILETIME wt{};
    std::string head(70000, '\0');  // the longest path fits
    DWORD got = 0;
    bool ok = GetFileSizeEx(h, &size) && GetFileTime(h, nullptr, nullptr, &wt) &&
              ReadFile(h, head.data(), (DWORD)head.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    head.resize(got);
    size_t at = sizeof kRecMagic;
    uint32_t pathLen = 0, hdrLen = 0;
    if (head.size() < at || memcmp(head.data(), kRecMagic, at) != 0 || !Get(head, at, pathLen) || pathLen > 32768 ||
        head.size() - at < pathLen * sizeof(wchar_t))
        return false;
    out.path.assign((const wchar_t*)(head.data() + at), pathLen);
    at += pathLen * sizeof(wchar_t);
    if (!Get(head, at, out.preSize) || !Get(head, at, out.mtime) || !Get(head, at, out.cp) || !Get(head, at, hdrLen) ||
        hdrLen > 8 || head.size() - at < hdrLen)
        return false;
    out.header.assign(head.data() + at, hdrLen);
    at += hdrLen;
    uint32_t pid = 0;
    uint64_t headHash = 0, len = 0, hash = 0, resHash = 0;
    if (!Get(head, at, out.pb) || !Get(head, at, out.pe) || !Get(head, at, out.prefixHash) || !Get(head, at, out.suffixHash) ||
        !Get(head, at, out.oldHash) || !Get(head, at, out.tailHash) || !Get(head, at, pid) || !Get(head, at, out.created))
        return false;
    size_t headEnd = at;
    if (!Get(head, at, headHash) || headHash != Fnv64(head.data(), headEnd)) return false;  // a header that is not whole
    out.pid = pid;
    out.resultOff = at;
    if (!Get(head, at, len) || !Get(head, at, hash) || !Get(head, at, resHash)) return false;
    out.hasResult = resHash == Fnv64(head.data() + out.resultOff, 16);
    out.newLen = len;
    out.newHash = hash;
    out.file = file;
    out.tailOff = at;
    out.tailLen = (uint64_t)size.QuadPart - at;
    out.written = U64(wt.dwHighDateTime, wt.dwLowDateTime);
    if (!(out.pb <= out.pe && out.pe <= out.preSize && out.tailLen == out.pe - out.pb)) return false;
    // the saved bytes are what was written (a power cut in the flush can leave zeros past a valid length)
    std::string tail;
    return ReadTail(out, tail) && RangeHash(tail.data(), tail.size()) == out.tailHash;
}

std::vector<RecoveryInfo> FindRecovery(const std::wstring& dir, uint32_t volume, uint64_t index, const std::wstring& path) {
    std::vector<RecoveryInfo> out;
    std::wstring d = dir;
    if (d.empty()) return out;
    if (d.back() != L'\\') d += L'\\';
    wchar_t pat[64];
    swprintf_s(pat, L"%08x-%016llx-*.rec", volume, (unsigned long long)index);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((d + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        RecoveryInfo ri;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ReadRecovery(d + fd.cFileName, ri)) continue;
        // The identity alone is not enough: on FAT a file made later in the same place gets the same index.
        if (CompareStringOrdinal(ri.path.c_str(), (int)ri.path.size(), path.c_str(), (int)path.size(), TRUE) != CSTR_EQUAL)
            continue;
        if (LiveOtherWriter(ri.pid, ri.created)) continue;
        size_t k = out.size();  // oldest first (there is hardly ever more than one)
        out.push_back(std::move(ri));
        for (; k > 0 && out[k - 1].written > out[k].written; k--) std::swap(out[k - 1], out[k]);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

RecoveryVerdict ClassifyRecovery(const RecoveryInfo& r, const std::string& bytes, uint64_t mtime) {
    const uint64_t len = bytes.size();
    const uint64_t all = Fnv64(bytes.data(), bytes.size());
    if (r.hasResult && len == r.newLen && all == r.newHash) return RV_DONE;  // the save went through
    if (len == r.preSize && all == r.oldHash) return RV_UNTOUCHED;           // it never wrote a byte (or was rolled back)
    bool prefix = len >= r.pb && RangeHash(bytes.data(), (size_t)r.pb) == r.prefixHash;
    bool suffix = r.pe == r.preSize ||
                  (len == r.preSize && RangeHash(bytes.data() + r.pe, (size_t)(r.preSize - r.pe)) == r.suffixHash);
    // not written since the recovery file was (FAT keeps write times to 2 s)
    bool quiet = mtime <= r.written + 20000000ull;
    return prefix && suffix && quiet ? RV_TORN : RV_CHANGED;
}

bool RecoveryRestore(const RecoveryInfo& r, const wchar_t* target, const std::wstring& recoveryDir, DWORD* err) {
    *err = 0;
    std::string tail;
    if (!ReadTail(r, tail) || RangeHash(tail.data(), tail.size()) != r.tailHash) { *err = ERROR_INVALID_DATA; return false; }
    HANDLE f = CreateFileW(target, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = GetLastError(); return false; }
    // Nobody can write it now: it must still be the torn file the recovery file is for - its bytes around the saved
    // range that version's own, and not written since. Anything else would lose what was written after.
    std::string bytes;
    BY_HANDLE_FILE_INFORMATION fi{};
    if (ReadAll(f, bytes, &fi, err, false) != SS_SAVED ||
        ClassifyRecovery(r, bytes, U64(fi.ftLastWriteTime.dwHighDateTime, fi.ftLastWriteTime.dwLowDateTime)) != RV_TORN) {
        CloseHandle(f);
        if (!*err) *err = ERROR_INVALID_DATA;
        return false;
    }
    // what it replaces goes to a recovery file of its own first: an interrupted restore can be undone as well
    RecoveryInfo own;
    own.path = target;
    own.preSize = bytes.size();
    own.mtime = U64(fi.ftLastWriteTime.dwHighDateTime, fi.ftLastWriteTime.dwLowDateTime);
    own.cp = r.cp;
    own.header = r.header;
    own.pb = r.pb;
    own.pe = bytes.size();
    own.prefixHash = RangeHash(bytes.data(), (size_t)r.pb);
    own.suffixHash = RangeHash(nullptr, 0);
    own.oldHash = Fnv64(bytes.data(), bytes.size());
    own.newLen = r.preSize;
    own.newHash = RestoredHash(r, bytes, tail);
    std::string ownTail = bytes.substr((size_t)r.pb);
    bool ok = WriteRecovery(recoveryDir, fi.dwVolumeSerialNumber, U64(fi.nFileIndexHigh, fi.nFileIndexLow),
                            (fi.dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0, own, ownTail, err) &&
              WriteAllAt(f, r.pb, tail.data(), tail.size(), err) && SetLength(f, r.preSize, err);
    if (ok && !FlushFileBuffers(f)) { *err = GetLastError(); ok = false; }
    CloseHandle(f);
    if (ok) DeleteFileW(own.file.c_str());
    return ok;
}

bool RecoveryRebuild(const RecoveryInfo& r, const wchar_t* current, const wchar_t* out) {
    std::string tail, bytes;
    DWORD e = 0;
    if (!ReadTail(r, tail) || RangeHash(tail.data(), tail.size()) != r.tailHash ||
        ReadDisk(current, bytes, nullptr, &e) != SS_SAVED || bytes.size() < r.pb)
        return false;
    std::string rest = r.pe < r.preSize && bytes.size() >= r.preSize ? bytes.substr((size_t)r.pe, (size_t)(r.preSize - r.pe)) : "";
    bytes.resize((size_t)r.pb);
    bytes += tail;
    bytes += rest;
    HANDLE f = CreateFileW(out, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = WriteAllAt(f, 0, bytes.data(), bytes.size(), &e);
    CloseHandle(f);
    return ok;
}

bool RecoveryFlushPending(const RecoveryInfo& r, const wchar_t* target) {
    HANDLE f = CreateFileW(target, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = f != INVALID_HANDLE_VALUE && FlushFileBuffers(f);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (ok) DeleteFileW(r.file.c_str());
    return ok;
}

// ------------------------------------------------------------------------------------------------ around edit mode
SaveState WriteProbe(const wchar_t* path, DWORD* err) {
    *err = 0;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        SaveState st = ErrorClass(*err, path);
        return st == SS_DENIED ? SS_READONLY : st;
    }
    CloseHandle(f);
    return SS_SAVED;
}

bool WriteNewFile(const wchar_t* path, const std::string& bytes, DWORD* err, bool replace) {
    *err = 0;
    std::wstring tmp = std::wstring(path) + L".fastmd-tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = GetLastError(); return false; }
    bool ok = WriteAllAt(f, 0, bytes.data(), bytes.size(), err);
    if (ok && !FlushFileBuffers(f)) { *err = GetLastError(); ok = false; }
    CloseHandle(f);
    if (ok && !MoveFileExW(tmp.c_str(), path, (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH)) {
        *err = GetLastError();
        ok = false;
    }
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

namespace {
const char kJrnMagic[8] = {'F', 'M', 'D', 'J', 'R', 'N', '1', 0};

std::wstring JournalFileName(uint32_t volume, uint64_t index, DWORD pid) {
    wchar_t b[80];
    swprintf_s(b, L"%08x-%016llx-%lu.unsaved", volume, (unsigned long long)index, (unsigned long)pid);
    return b;
}
}  // namespace

uint64_t TextHash(const std::wstring& t) { return Fnv64(t.data(), t.size() * sizeof(wchar_t)); }

bool WriteJournal(const std::wstring& dir, uint32_t volume, uint64_t index, bool encrypted, const std::wstring& path,
                  uint64_t diskHash, UINT cp, const std::wstring& text, std::wstring* file) {
    if (dir.empty()) return false;
    std::wstring d = dir;
    if (d.back() != L'\\') d += L'\\';
    CreateDirectoryW(DirOf(d.substr(0, d.size() - 1)).c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
    std::string h(kJrnMagic, sizeof kJrnMagic);
    Put<uint32_t>(h, (uint32_t)path.size());
    h.append((const char*)path.data(), path.size() * sizeof(wchar_t));
    Put<uint64_t>(h, NowFileTime());
    Put<uint64_t>(h, diskHash);
    Put<uint32_t>(h, cp);
    Put<uint32_t>(h, GetCurrentProcessId());
    Put<uint64_t>(h, ProcessStart());
    Put<uint64_t>(h, (uint64_t)text.size());
    Put<uint64_t>(h, TextHash(text));
    Put<uint64_t>(h, Fnv64(h.data(), h.size()));
    std::wstring dst = d + JournalFileName(volume, index, GetCurrentProcessId()), tmp = dst + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           encrypted ? FILE_ATTRIBUTE_ENCRYPTED : FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD e = 0;
    bool ok = WriteAllAt(f, 0, h.data(), h.size(), &e) &&
              WriteAllAt(f, h.size(), (const char*)text.data(), text.size() * sizeof(wchar_t), &e) && FlushFileBuffers(f);
    CloseHandle(f);
    ok = ok && MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp.c_str());
    else if (file) *file = dst;
    return ok;
}

static bool ReadJournal(const std::wstring& file, JournalInfo& out) {
    std::string bytes;
    DWORD e = 0;
    if (ReadDisk(file.c_str(), bytes, nullptr, &e) != SS_SAVED) return false;
    size_t at = sizeof kJrnMagic;
    uint32_t pathLen = 0, cp = 0, pid = 0;
    uint64_t len = 0, hash = 0, headHash = 0;
    if (bytes.size() < at || memcmp(bytes.data(), kJrnMagic, at) != 0 || !Get(bytes, at, pathLen) || pathLen > 32768 ||
        bytes.size() - at < pathLen * sizeof(wchar_t))
        return false;
    out.path.assign((const wchar_t*)(bytes.data() + at), pathLen);
    at += pathLen * sizeof(wchar_t);
    if (!Get(bytes, at, out.time) || !Get(bytes, at, out.diskHash) || !Get(bytes, at, cp) || !Get(bytes, at, pid) ||
        !Get(bytes, at, out.created) || !Get(bytes, at, len) || !Get(bytes, at, hash))
        return false;
    size_t headEnd = at;
    if (!Get(bytes, at, headHash) || headHash != Fnv64(bytes.data(), headEnd)) return false;
    if ((bytes.size() - at) / sizeof(wchar_t) != len || (bytes.size() - at) % sizeof(wchar_t)) return false;
    out.text.assign((const wchar_t*)(bytes.data() + at), (size_t)len);
    if (TextHash(out.text) != hash) return false;  // a journal cut short is no copy of anything
    out.cp = cp;
    out.pid = pid;
    out.file = file;
    return true;
}

std::vector<JournalInfo> FindJournals(const std::wstring& dir, uint32_t volume, uint64_t index, const std::wstring& path) {
    std::vector<JournalInfo> out;
    std::wstring d = dir;
    if (d.empty()) return out;
    if (d.back() != L'\\') d += L'\\';
    wchar_t pat[64];
    swprintf_s(pat, L"%08x-%016llx-*.unsaved", volume, (unsigned long long)index);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((d + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        JournalInfo j;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ReadJournal(d + fd.cFileName, j)) continue;
        if (CompareStringOrdinal(j.path.c_str(), (int)j.path.size(), path.c_str(), (int)path.size(), TRUE) != CSTR_EQUAL)
            continue;
        if (LiveOtherWriter(j.pid, j.created)) continue;  // another window still edits it: its own business
        size_t k = out.size();  // newest first
        out.push_back(std::move(j));
        for (; k > 0 && out[k - 1].time < out[k].time; k--) std::swap(out[k - 1], out[k]);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::wstring RecoveryPrefix(uint32_t volume, uint64_t index) {
    wchar_t b[48];
    swprintf_s(b, L"%08x-%016llx-", volume, (unsigned long long)index);
    return b;
}

void PurgeRecovery(const std::wstring& dir, uint32_t days, const std::wstring& keep) {
    std::wstring d = dir;
    if (d.empty()) return;
    if (d.back() != L'\\') d += L'\\';
    const uint64_t limit = NowFileTime() - (uint64_t)days * 24 * 3600 * 10000000ull;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((d + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!keep.empty() && !_wcsnicmp(fd.cFileName, keep.c_str(), keep.size())) continue;
        bool ours = EndsWithI(fd.cFileName, L".rec") || EndsWithI(fd.cFileName, L".unsaved") || EndsWithI(fd.cFileName, L".theirs");
        if (ours && U64(fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime) < limit)
            DeleteFileW((d + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool WriteTheirs(const std::wstring& dir, uint32_t volume, uint64_t index, const std::string& bytes, std::wstring* file) {
    if (dir.empty()) return false;
    std::wstring d = dir;
    if (d.back() != L'\\') d += L'\\';
    CreateDirectoryW(DirOf(d.substr(0, d.size() - 1)).c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
    wchar_t b[80];
    swprintf_s(b, L"%08x-%016llx-%lu.theirs", volume, (unsigned long long)index, GetCurrentProcessId());
    std::wstring f = d + b;
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD e = 0;
    bool ok = WriteAllAt(h, 0, bytes.data(), bytes.size(), &e) && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) DeleteFileW(f.c_str());
    else if (file) *file = f;
    return ok;
}

std::wstring EditMutexName(uint32_t volume, uint64_t index) {
    wchar_t b[96];
    swprintf_s(b, L"Local\\FastMD.edit.%08x-%016llx", volume, (unsigned long long)index);
    return b;
}

void SetFailWriteForTests(const wchar_t* spec) {
    g_faultRead = true;
    ParseFault(spec);
}

void SetFlushPolicyForTests(int policy) { g_flushPolicy = policy; }

