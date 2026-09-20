// WinHTTP download + a small file cache. No cookies, no credentials, no redirects to other schemes: a picture in a
// README must never turn into a way of tracking or authenticating the reader.
#include "net.h"
#include "app.h"
#include <winhttp.h>

namespace {
const DWORD kTimeoutMs = 10000;

uint64_t Fnv1a(const std::wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) {
        h ^= (uint64_t)(uint16_t)c;
        h *= 1099511628211ull;
    }
    return h;
}
}  // namespace

std::wstring CacheFileFor(const std::wstring& url) {
    wchar_t name[32];
    swprintf_s(name, L"%016llx.bin", (unsigned long long)Fnv1a(url));
    return DataDir() + L"cache\\" + name;
}

bool HttpGet(const std::wstring& url, std::vector<uint8_t>& out, size_t maxBytes, bool allowHttp) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host) - 1;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path) - 1;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring hostName = host;
    bool loopback = hostName == L"127.0.0.1" || hostName == L"localhost" || hostName == L"::1";
    // plain http only to this machine (a local preview server); everything else must be https
    if (!https && (uc.nScheme != INTERNET_SCHEME_HTTP || !(allowHttp || loopback))) return false;

    HINTERNET session = WinHttpOpen(L"FastMD", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (conn) {
        DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (req) {
            DWORD none = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
            WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &none, sizeof(none));
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &len, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    out.clear();
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) { ok = !out.empty(); break; }
                        if (out.size() + avail > maxBytes) { ok = false; break; }
                        size_t at = out.size();
                        out.resize(at + avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(req, out.data() + at, avail, &got)) { ok = false; break; }
                        out.resize(at + got);
                        if (g.closing) { ok = false; break; }
                    }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    if (!ok) out.clear();
    return ok;
}

void TrimHttpCache(uint64_t budget) {
    std::wstring dir = DataDir() + L"cache\\";
    struct Entry { std::wstring path; uint64_t size, used; };
    std::vector<Entry> files;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"*.bin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        uint64_t used = ((uint64_t)fd.ftLastAccessTime.dwHighDateTime << 32) | fd.ftLastAccessTime.dwLowDateTime;
        files.push_back(Entry{dir + fd.cFileName, size, used});
        total += size;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total <= budget) return;
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const Entry& e : files) {
        if (total <= budget) break;
        if (DeleteFileW(e.path.c_str())) total -= e.size;
    }
}
