// Bench protocol hooks, trace log, string and file helpers.
#include "doc.h"
#include "bench_protocol.h"
#include <cstdarg>
#include <cstdio>

// ------------------------------------------------------------------------------------------------ bench
static SRWLOCK g_markLock = SRWLOCK_INIT;

uint64_t NowTicks() { return fmb_now(); }
void BenchInit() { fmb_init(); }
bool BenchActive() { return fmb_is_bench() != 0; }
void BenchWindowShown() { fmb_window_shown(); }
bool BenchContentPresented(const char* notes) { return fmb_content_presented_ex(notes) != 0; }

void MarkAt(const char* name, uint64_t t) {
    DebugLog("mark %s", name);
    if (!g_fmb.active) return;
    AcquireSRWLockExclusive(&g_markLock);
    if (g_fmb.mark_count < FMB_MAX_MARKS) {
        strncpy_s(g_fmb.mark_names[g_fmb.mark_count], 32, name, _TRUNCATE);
        g_fmb.mark_ticks[g_fmb.mark_count++] = t;
    }
    ReleaseSRWLockExclusive(&g_markLock);
}
void Mark(const char* name) { MarkAt(name, fmb_now()); }

// ------------------------------------------------------------------------------------------------ trace
namespace {
struct TraceEv { uint64_t t; DWORD tid; char what[120]; };
TraceEv g_ev[1024];
volatile LONG g_evN = 0;
int g_trace = -1;  // -1 = not checked yet
DWORD g_mainTid = 0;
}  // namespace

void DebugLog(const char* fmt, ...) {
    if (g_trace < 0) {
        wchar_t v[8] = {};
        g_trace = GetEnvironmentVariableW(L"FASTMD_TRACE", v, 8) > 0 && v[0] == L'1';
        g_mainTid = GetCurrentThreadId();
    }
    if (!g_trace) return;
    LONG i = InterlockedIncrement(&g_evN) - 1;
    if (i >= 1024) return;
    g_ev[i].t = fmb_now();
    g_ev[i].tid = GetCurrentThreadId();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ev[i].what, sizeof(g_ev[i].what), fmt, ap);
    va_end(ap);
}

void DebugFlush() {
    if (g_trace != 1) return;
    wchar_t p[MAX_PATH];
    GetTempPathW(MAX_PATH, p);
    wcscat_s(p, L"fastmd-trace.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, p, L"wb") || !f) return;
    FILETIME ct, et, kt, ut;
    GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut);
    uint64_t t0 = ((uint64_t)ct.dwHighDateTime << 32) | ct.dwLowDateTime;
    LONG n = g_evN < 1024 ? g_evN : 1024;
    for (LONG i = 0; i < n; i++)
        fprintf(f, "%8.2f  %s  %s\n", (g_ev[i].t - t0) / 10000.0, g_ev[i].tid == g_mainTid ? "UI" : "bg", g_ev[i].what);
    fclose(f);
}

// ------------------------------------------------------------------------------------------------ strings
std::wstring ToLower(const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring out(s.size(), L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), (int)s.size(), out.data(), (int)out.size(),
                          nullptr, nullptr, 0);
    if (n <= 0) return s;
    out.resize(n);
    return out;
}

bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring DirOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
}

// ------------------------------------------------------------------------------------------------ files
void DecodeText(const char* p, int len, std::wstring& out, TextEncoding* enc) {
    uint32_t header = 0;
    if (len >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {  // UTF-8 BOM
        p += 3;
        len -= 3;
        header = 3;
    }
    if (len >= 2 && (uint8_t)p[0] == 0xFF && (uint8_t)p[1] == 0xFE) {  // UTF-16 LE file
        out.assign((const wchar_t*)(p + 2), (len - 2) / 2);
        if (enc) *enc = TextEncoding{header + 2, 1200};
        return;
    }
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (wn == 0 && len > 0) { cp = CP_ACP; flags = 0; wn = MultiByteToWideChar(cp, 0, p, len, nullptr, 0); }
    out.resize(wn);
    out.resize(MultiByteToWideChar(cp, flags, p, len, out.data(), wn));
    if (enc) *enc = TextEncoding{header, cp};
}

bool ReadFileUtf16(const wchar_t* path, std::wstring& out, uint64_t* ticksRead, FILETIME* writeTime) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    if (writeTime) GetFileTime(f, nullptr, nullptr, writeTime);
    if (sz.QuadPart > (1ll << 30)) { CloseHandle(f); return false; }
    DWORD n = (DWORD)sz.QuadPart, got = 0;
    char* buf = (char*)VirtualAlloc(nullptr, n + 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BOOL ok = buf && ReadFile(f, buf, n, &got, nullptr);
    CloseHandle(f);
    if (!ok) { if (buf) VirtualFree(buf, 0, MEM_RELEASE); return false; }
    if (ticksRead) *ticksRead = NowTicks();
    DecodeText(buf, (int)got, out, nullptr);
    VirtualFree(buf, 0, MEM_RELEASE);
    return true;
}

bool ReadFileBytes(const wchar_t* path, std::vector<uint8_t>& out, size_t maxBytes) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && (uint64_t)sz.QuadPart <= maxBytes;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
        if (!ok) out.clear();
    }
    CloseHandle(f);
    return ok;
}

bool GetFileStamp(const wchar_t* path, FILETIME* writeTime, uint64_t* size) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return false;
    if (writeTime) *writeTime = a.ftLastWriteTime;
    if (size) *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return true;
}

// ------------------------------------------------------------------------------------------------ SHA-256
// FIPS 180-4, the short way: the updater checks a downloaded file against the hash published with the release, and
// that is the only place it is needed - so no bcrypt.dll on the start-up path and nothing to link.
namespace {
inline uint32_t Ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void Sha256Block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g2 = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g2);
        uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
        uint32_t S0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g2; g2 = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g2; h[7] += hh;
}
}  // namespace

std::string Sha256Hex(const uint8_t* data, size_t n) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = n / 64;
    for (size_t i = 0; i < full; i++) Sha256Block(h, data + i * 64);
    uint8_t tail[128] = {};
    size_t rest = n - full * 64;
    memcpy(tail, data + full * 64, rest);
    tail[rest] = 0x80;
    size_t len = rest + 1 <= 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[len - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < len; i += 64) Sha256Block(h, tail + i);
    static const char* hex = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; i++)
        for (int b = 0; b < 4; b++) {
            uint8_t v = (uint8_t)(h[i] >> (24 - 8 * b));
            out[i * 8 + b * 2] = hex[v >> 4];
            out[i * 8 + b * 2 + 1] = hex[v & 15];
        }
    return out;
}
