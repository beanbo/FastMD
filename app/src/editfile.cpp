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
bool OnNetwork(const wchar_t* path) {
    if (path[0] == L'\\' && path[1] == L'\\') return true;  // UNC
    if (!path[0] || path[1] != L':') return false;
    wchar_t root[4] = {path[0], L':', L'\\', 0};
    return GetDriveTypeW(root) == DRIVE_REMOTE;
}

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

// a local volume is flushed after every save while flushing it is cheap (the median of its first three flushes under
// 5 ms, and those three to find out); otherwise only at the flush points
bool FlushEverySave(uint32_t vol) {
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
const char kRecMagic[8] = {'F', 'M', 'D', 'R', 'E', 'C', '1', 0};

template <class T> void Put(std::string& s, T v) { s.append((const char*)&v, sizeof v); }
template <class T> bool Get(const std::string& s, size_t& at, T& v) {
    if (s.size() - at < sizeof v || at > s.size()) return false;
    memcpy(&v, s.data() + at, sizeof v);
    at += sizeof v;
    return true;
}

// the old bytes from pb on, with what it takes to put them back, safe on disk before the target changes (§10.5)
bool WriteRecovery(const SaveRequest& rq, const DiskState& base, const BY_HANDLE_FILE_INFORMATION& fi,
                   const std::string& bytes, uint64_t pb, std::wstring& file, DWORD* err) {
    if (rq.recoveryDir.empty()) { *err = ERROR_PATH_NOT_FOUND; return false; }
    std::wstring dir = rq.recoveryDir;
    if (dir.back() != L'\\') dir += L'\\';
    CreateDirectoryW(DirOf(dir.substr(0, dir.size() - 1)).c_str(), nullptr);  // the data folder itself
    CreateDirectoryW(dir.c_str(), nullptr);
    file = dir + RecoveryName(fi.dwVolumeSerialNumber, U64(fi.nFileIndexHigh, fi.nFileIndexLow), GetCurrentProcessId());
    if (Fires(FK_RECOVERY)) { *err = ERROR_WRITE_FAULT; return false; }
    std::string h(kRecMagic, sizeof kRecMagic);
    size_t pathLen = wcslen(rq.path);
    Put<uint32_t>(h, (uint32_t)pathLen);
    h.append((const char*)rq.path, pathLen * sizeof(wchar_t));
    Put<uint64_t>(h, bytes.size());
    Put<uint64_t>(h, U64(fi.ftLastWriteTime.dwHighDateTime, fi.ftLastWriteTime.dwLowDateTime));
    Put<uint32_t>(h, base.cp);
    Put<uint32_t>(h, (uint32_t)base.header.size());
    h += base.header;
    Put<uint64_t>(h, pb);
    // an encrypted document gets an encrypted copy (D22)
    DWORD attr = (fi.dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED) ? FILE_ATTRIBUTE_ENCRYPTED : FILE_ATTRIBUTE_NORMAL;
    HANDLE r = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, attr, nullptr);
    if (r == INVALID_HANDLE_VALUE) { *err = GetLastError(); return false; }
    bool ok = WriteAllAt(r, 0, h.data(), h.size(), err) &&
              WriteAllAt(r, h.size(), bytes.data() + pb, bytes.size() - (size_t)pb, err);
    if (ok && !FlushFileBuffers(r)) { *err = GetLastError(); ok = false; }
    if (!CloseHandle(r)) ok = false;
    if (!ok) DeleteFileW(file.c_str());
    return ok;
}

bool IsLiveFastMd(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool live = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    wchar_t img[MAX_PATH * 2];
    DWORD n = (DWORD)std::size(img);
    if (live) live = QueryFullProcessImageNameW(h, 0, img, &n) && EndsWithI(img, L"\\FastMD.exe");
    CloseHandle(h);
    return live;
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
    auto give = [&](SaveState st, const char* why) {  // nothing has changed yet
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        r.state = st;
        r.reason = why;
        r.error = e;
        return r;
    };

    // 2. What is there now. The baseline's bytes: go on. Other bytes of the same text (re-encoded outside): their code
    // page, header and line ends from now on, if they can be written back byte for byte (D13). Other text: CONFLICT.
    std::string bytes;
    BY_HANDLE_FILE_INFORMATION fi{};
    SaveState st = ReadAll(f, bytes, &fi, &e, true);
    if (st != SS_SAVED) return give(st, "read");
    const DiskState* base = rq.disk;
    if (bytes.size() != base->length || Fnv64(bytes.data(), bytes.size()) != base->hash) {
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
    }
    const std::wstring& old = base->text;
    const UINT cp = base->cp;

    // 3. The bytes that change: from the first differing character to the last, moved outwards so that neither end
    // splits a surrogate pair or a CRLF; the rest of the file keeps its bytes (D1).
    size_t on = old.size(), nn = text.size(), lim = std::min(on, nn), p = 0, s = 0;
    while (p < lim && old[p] == text[p]) p++;
    while (s < lim - p && old[on - 1 - s] == text[nn - 1 - s]) s++;
    auto splits = [](const std::wstring& t, size_t i) {
        return i > 0 && i < t.size() && ((HighSur(t[i - 1]) && LowSur(t[i])) || (t[i - 1] == L'\r' && t[i] == L'\n'));
    };
    while (p > 0 && (splits(old, p) || splits(text, p))) p--;
    while (s > 0 && (splits(old, on - s) || splits(text, nn - s))) s--;
    // the baseline is byte-exact, so its text up to p is exactly the bytes up to pb (and the same for the suffix)
    size_t pl = EncodedLen(cp, old.data(), p), sb = EncodedLen(cp, old.data() + on - s, s);
    size_t pb = pl == SIZE_MAX ? SIZE_MAX : base->header.size() + pl;
    if (pb > bytes.size() || sb > bytes.size() - pb) return give(SS_FAILED, "ENCODER_ERROR");
    std::string middle;
    size_t bad = SIZE_MAX;
    const char* why = "";
    if (!EncodeText(cp, text.data() + p, nn - s - p, middle, &bad, &why)) {
        r.bad = bad == SIZE_MAX ? UINT32_MAX : (uint32_t)(p + bad);
        return give(strcmp(why, "ENCODER_ERROR") ? SS_UNENCODABLE : SS_FAILED, why);
    }
    // A file without a byte-order mark must not start to look like one: the next read would take it for the mark.
    if (base->header.empty() && cp != 1200) {
        std::string head = bytes.substr(0, std::min<size_t>(pb, 3)) + middle.substr(0, 3);
        if (head.size() < 3) head += bytes.substr(bytes.size() - sb, 3);
        auto starts = [&](const char* m, size_t k) { return head.size() >= k && memcmp(head.data(), m, k) == 0; };
        if ((cp == CP_UTF8 && nn && text[0] == 0xFEFF) || starts("\xEF\xBB\xBF", 3) || starts("\xFF\xFE", 2) ||
            starts("\xFE\xFF", 2)) {
            r.bad = 0;
            return give(SS_UNENCODABLE, "BOM_LOOKALIKE");
        }
    }
    const uint64_t oldLen = bytes.size(), newLen = pb + middle.size() + sb;
    const char* suffix = bytes.data() + (oldLen - sb);
    // The proof: the new file reads back as exactly this text - all of it below a million characters (and always under
    // FASTMD_EDIT_SELFCHECK), the changed window with whole characters on both sides above.
    uint64_t hash = Fnv64(suffix, sb, Fnv64(middle.data(), middle.size(), Fnv64(bytes.data(), pb)));
    std::wstring back;
    if (rq.fullProof || nn < (1u << 20)) {
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
            if (cp != CP_UTF8 || at >= newLen) return false;
            uint8_t c = at < pb ? (uint8_t)bytes[at] : at < pb + middle.size() ? (uint8_t)middle[at - pb] : (uint8_t)suffix[at - pb - middle.size()];
            return (c & 0xC0) == 0x80;
        };
        if (back.compare(0, std::wstring::npos, text, p, nn - s - p) != 0 || cont(pb) || cont(pb + middle.size())) {
            r.bad = (uint32_t)p;
            return give(SS_UNENCODABLE, "UNENCODABLE");
        }
    }

    // 4. The recovery file: the bytes about to be replaced, on disk before the first byte of the target changes.
    std::wstring rec;
    if (!WriteRecovery(rq, *base, fi, bytes, pb, rec, &e)) return give(SS_FAILED, "recovery");

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
        if (restored) DeleteFileW(rec.c_str());
        else r.recoveryKept = rec;
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
        if (e) {
            if (!changed) {
                DeleteFileW(rec.c_str());
                return give(SS_FAILED, "disk full");
            }
            return fail("disk full", e);
        }
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
    if (flushFault || rq.flushPoint || base->remote || base->cloud || FlushEverySave(fi.dwVolumeSerialNumber)) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        bool ok = !flushFault && FlushFileBuffers(f);
        QueryPerformanceCounter(&t1);
        if (!ok) return fail("flush", flushFault ? ERROR_WRITE_FAULT : GetLastError());
        r.flushMs = Ms(t0, t1);
        if (!base->remote && !base->cloud) RecordFlush(fi.dwVolumeSerialNumber, r.flushMs);
    }
    // 8. The new stamp through the same handle, before it is closed (NTFS moves the write time at WriteFile).
    BY_HANDLE_FILE_INFORMATION after{};
    if (!GetFileInformationByHandle(f, &after)) return fail("stamp", GetLastError());
    bool closed = CloseHandle(f) != 0;
    f = INVALID_HANDLE_VALUE;
    if (Fires(FK_CLOSE)) closed = false;
    if (!closed) return fail("close", ERROR_WRITE_FAULT);

    // 10. Saved: the recovery file goes, and the snapshot is what the disk holds.
    DeleteFileW(rec.c_str());
    DiskState d;
    d.valid = true;
    d.text = text;
    d.cp = cp;
    d.header = base->header;
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
std::wstring RecoveryName(uint32_t volume, uint64_t index, DWORD pid) {
    wchar_t b[64];
    swprintf_s(b, L"%08x-%016llx-%lu.rec", volume, (unsigned long long)index, (unsigned long)pid);
    return b;
}

bool ReadRecovery(const std::wstring& file, RecoveryInfo& out) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string head(4096, '\0');
    DWORD got = 0;
    bool ok = GetFileSizeEx(h, &size) && ReadFile(h, head.data(), (DWORD)head.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    head.resize(got);
    size_t at = sizeof kRecMagic;
    uint32_t pathLen = 0, hdrLen = 0;
    if (head.size() < at || memcmp(head.data(), kRecMagic, at) != 0 || !Get(head, at, pathLen) ||
        pathLen > 1024 || head.size() - at < pathLen * sizeof(wchar_t))
        return false;
    out.path.assign((const wchar_t*)(head.data() + at), pathLen);
    at += pathLen * sizeof(wchar_t);
    if (!Get(head, at, out.preSize) || !Get(head, at, out.mtime) || !Get(head, at, out.cp) || !Get(head, at, hdrLen) ||
        hdrLen > 8 || head.size() - at < hdrLen)
        return false;
    out.header.assign(head.data() + at, hdrLen);
    at += hdrLen;
    if (!Get(head, at, out.pb)) return false;
    out.file = file;
    out.tailOff = at;
    out.tailLen = (uint64_t)size.QuadPart - at;
    // the name says which process wrote it
    size_t dash = file.find_last_of(L'-'), dot = file.find_last_of(L'.');
    out.pid = dash != std::wstring::npos && dot > dash ? (DWORD)wcstoul(file.c_str() + dash + 1, nullptr, 10) : 0;
    return out.pb + out.tailLen == out.preSize;
}

std::vector<RecoveryInfo> FindRecovery(const std::wstring& dir, uint32_t volume, uint64_t index) {
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
        if (IsLiveFastMd(ri.pid)) continue;  // still saving, or it deletes its own
        ri.written = U64(fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime);
        size_t k = out.size();  // oldest first (there is hardly ever more than one)
        out.push_back(std::move(ri));
        for (; k > 0 && out[k - 1].written > out[k].written; k--) std::swap(out[k - 1], out[k]);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

namespace {
bool ReadTail(const RecoveryInfo& r, std::string& tail) {
    HANDLE h = CreateFileW(r.file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE || r.tailLen > (1ull << 30)) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return false;
    }
    tail.resize((size_t)r.tailLen);
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)r.tailOff;
    DWORD got = 0;
    bool ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) &&
              (tail.empty() || (ReadFile(h, tail.data(), (DWORD)tail.size(), &got, nullptr) && got == tail.size()));
    CloseHandle(h);
    return ok;
}
}  // namespace

bool RecoveryRestore(const RecoveryInfo& r, const wchar_t* target, DWORD* err) {
    *err = 0;
    std::string tail;
    if (!ReadTail(r, tail)) { *err = GetLastError() ? GetLastError() : ERROR_READ_FAULT; return false; }
    HANDLE f = CreateFileW(target, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = GetLastError(); return false; }
    LARGE_INTEGER size{};
    // the bytes before pb are the file's own: a file now shorter than that is not the one the copy belongs to
    bool ok = GetFileSizeEx(f, &size) && (uint64_t)size.QuadPart >= r.pb;
    if (!ok) *err = ERROR_INVALID_DATA;
    ok = ok && WriteAllAt(f, r.pb, tail.data(), tail.size(), err) && SetLength(f, r.pb + tail.size(), err);
    if (ok && !FlushFileBuffers(f)) { *err = GetLastError(); ok = false; }
    CloseHandle(f);
    return ok;
}

bool RecoveryRebuild(const RecoveryInfo& r, const wchar_t* current, const wchar_t* out) {
    std::string tail, bytes;
    DWORD e = 0;
    if (!ReadTail(r, tail) || ReadDisk(current, bytes, nullptr, &e) != SS_SAVED || bytes.size() < r.pb) return false;
    bytes.resize((size_t)r.pb);
    bytes += tail;
    HANDLE f = CreateFileW(out, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = WriteAllAt(f, 0, bytes.data(), bytes.size(), &e);
    CloseHandle(f);
    return ok;
}

void SetFailWriteForTests(const wchar_t* spec) {
    g_faultRead = true;
    ParseFault(spec);
}
