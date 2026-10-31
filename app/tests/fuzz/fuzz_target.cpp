// A libFuzzer target for everything that reads a document's bytes (plan 6.5): the decoder that turns a file into text
// (BOM sniffing, UTF-8 / UTF-16 / the ANSI code page, edit mode's byte-exact round trip), the Markdown parser in both of
// its modes, the source map and its round trips, and the HTML tag reader.
//
// fuzz.cpp mutates whole documents with its own loop and walks edit operations over them; this target lets libFuzzer's
// coverage guidance do the mutating, starting from the bytes of a file - so it also reaches the decoder, which fuzz.cpp
// never sees. The first byte of an input picks how the rest is read:
//   bit 0       the formula and diagram libraries count as present (a formula is a picture) or absent (code text)
//   bit 1       the rest is raw UTF-16 LE (lone surrogates and all) instead of a file's bytes
//   bits 2-3    the ANSI code page bytes that are not UTF-8 fall back to: 1252, 1251, 932 (DBCS) or 936
// A broken invariant aborts, so libFuzzer keeps the input as a crash.
//
// Windows, clang-cl (Visual Studio's "C++ Clang tools" component, or LLVM):
//   cmake -S app -B app\build\LibFuzzer -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
//         -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFASTMD_LIBFUZZER=ON -DFASTMD_RUST=OFF -DFASTMD_SETUP=OFF ^
//         -DFASTMD_TESTS=OFF -DFASTMD_PREVIEW=OFF
//   cmake --build app\build\LibFuzzer --target fastmd-libfuzzer
//   app\build\LibFuzzer\fastmd-libfuzzer.exe -max_total_time=600 -max_len=65536 -timeout=10 corpus-dir bench\corpus
// Without libFuzzer (mingw, MSVC): built with FASTMD_FUZZ_STANDALONE, the same target replays the files and folders it
// is given - the regression inputs in tests/fuzz/regress among them - or mutates them blindly (see the end of the file):
//   fastmd-fuzz-target.exe app\tests\fuzz\regress
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "doc.h"
#include "editcore.h"
#include "editfile.h"
#include "html.h"
#include "../edit/map_sweep.h"

static bool g_tex = false;
bool TexAvailable() { return g_tex; }
bool MermaidAvailable() { return g_tex; }

namespace {
[[noreturn]] void Fail(const char* what, const std::string& why) {
    fprintf(stderr, "fastmd fuzz target: %s: %s\n", what, why.c_str());
    fflush(stderr);
    abort();
}

void ParseBoth(const std::wstring& text) {
    Doc d;
    d.baseDir = L"C:\\fuzz\\";
    ParseMarkdown(d, text.data(), text.size());
    Doc m;
    m.baseDir = d.baseDir;
    ParseOptions opt;
    opt.wantMap = true;
    ParseMarkdown(m, text.data(), text.size(), &opt);
    std::string why;
    if (!MapSelfCheck(m, text, &why)) Fail("MapSelfCheck", why);
    if (m.blocks.size() != d.blocks.size()) Fail("map parse", "a different number of blocks than the reading parse");
    for (size_t k = 0; k < m.blocks.size(); k++)
        if (m.blocks[k].kind != d.blocks[k].kind || m.blocks[k].heading != d.blocks[k].heading)
            Fail("map parse", "block " + std::to_string(k) + " differs from the reading parse");
    uint32_t stride = 1 + (uint32_t)(text.size() / 2000);
    size_t stops = 0, offsets = 0;
    mapsweep::SweepMap(m, text, stride, stride, &stops, &offsets, [&](const std::string& msg) -> bool {
        Fail("round trip", msg);
    });
    // at every '<', stepping over what a tag consumed as EmitHtmlBlock does (a '<' at every character would make the
    // walk itself quadratic: an unclosed tag reads to the end of its block)
    for (size_t i = 0; i + 1 < text.size();) {
        HtmlTag tag;
        size_t used = text[i] == L'<' ? ParseHtmlTag(text.data() + i, text.size() - i, tag) : 0;
        i += used ? used : 1;
    }
    std::wstring out;
    AppendHtmlText(out, text.data(), text.size());
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > (256u << 10)) return -1;  // past a few hundred KB a mutation finds nothing new
    const uint8_t mode = data[0];
    data++;
    size--;
    g_tex = (mode & 1) != 0;
    std::wstring text;
    if (mode & 2) {
        // wchar_t is UTF-16 here (Windows); an odd last byte is dropped. memcpy: the bytes need not be aligned
        text.resize(size / 2);
        if (size >= 2) memcpy(text.data(), data, size / 2 * 2);
    } else {
        static const UINT kCp[] = {1252, 1251, 932, 936};
        const UINT acp = kCp[(mode >> 2) & 3];
        const std::string bytes(reinterpret_cast<const char*>(data), size);
        // the reading path: what the window shows
        DecodeTextCp(bytes.data(), (int)bytes.size(), acp, text, nullptr);
        // edit mode's entry check: when it says the file can be written back, the text must encode to the same bytes
        DiskState ds;
        if (DecodeDisk(bytes, acp, ds) == DR_OK) {
            std::string again;
            size_t bad = 0;
            const char* reason = nullptr;
            if (!EncodeText(ds.cp, ds.text.data(), ds.text.size(), again, &bad, &reason))
                Fail("round trip", std::string("DecodeDisk accepted text that does not encode: ") + (reason ? reason : "?"));
            if (ds.header + again != bytes) Fail("round trip", "DecodeDisk accepted bytes that do not come back");
            if (ds.length != bytes.size() || ds.hash != Fnv64(bytes.data(), bytes.size()))
                Fail("DecodeDisk", "the length or hash is not the bytes'");
        }
    }
    ParseBoth(text);
    return 0;
}

#ifdef FASTMD_FUZZ_STANDALONE
// Without libFuzzer: replays the files and folders it is given (each file in every mode the first byte can pick), or
// with --seconds, mutates their bytes blindly for that long - no coverage guidance, but every input reaches the
// decoder, which fuzz.cpp never feeds. The input about to run is always in out/fuzz/target-last.bin (run from
// app/tests); a hang (--timeout, default 30 s) stops the run with exit code 4.
//   fastmd-fuzz-target.exe app\tests\fuzz\regress                       # replay
//   fastmd-fuzz-target.exe --seconds 600 --seed 3 bench\corpus app\tests  # mutate for ten minutes
#include <windows.h>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

static std::atomic<uint64_t> g_started{0};

static bool ReadAll(const std::wstring& path, std::string& bytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    bytes.assign(size, '\0');
    if (size) ReadFile(f, bytes.data(), size, &got, nullptr);
    CloseHandle(f);
    bytes.resize(got);
    return true;
}

static void WriteAll(const wchar_t* path, const std::string& bytes) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr);
    CloseHandle(f);
}

static void Collect(const std::wstring& p, std::vector<std::wstring>& files) {
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) { files.push_back(p); return; }
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(p + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void Run(const std::string& input) {
    WriteAll(L"out\\fuzz\\target-last.bin", input);
    g_started = GetTickCount64();
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    g_started = 0;
}

// byte mutations: the ones that matter to a decoder (BOMs, high bytes, cut sequences, NULs) and to a parser
static std::string Mutate(std::mt19937& rng, const std::vector<std::string>& seeds) {
    static const char* kBom[] = {"\xEF\xBB\xBF", "\xFF\xFE", "\xFE\xFF", "\x2B\x2F\x76"};
    static const char kSpice[] = "#*_`~[]()<>!|$\\\n\r\t \"'&;:^-+={}\x80\xBF\xC0\xE2\xF0\xFF";
    const std::string& src = seeds[rng() % seeds.size()];
    size_t from = src.empty() ? 0 : rng() % src.size();
    std::string s = src.substr(from, 1 + rng() % 8192);
    std::string out(1, (char)(rng() & 0xFF));  // the mode byte
    int rounds = 1 + (int)(rng() % 8);
    for (int r = 0; r < rounds; r++) {
        size_t at = s.empty() ? 0 : rng() % (s.size() + 1);
        switch (rng() % 8) {
        case 0: if (!s.empty() && at < s.size()) s[at] = (char)(rng() & 0xFF); break;
        case 1: if (!s.empty() && at < s.size()) s[at] = kSpice[rng() % (sizeof kSpice - 1)]; break;
        case 2: s.erase(at, 1 + rng() % 64); break;
        case 3: s.insert(0, kBom[rng() % 4]); break;
        case 4: {
            const std::string& o = seeds[rng() % seeds.size()];
            if (!o.empty()) { size_t f = rng() % o.size(); s.insert(at, o, f, 1 + rng() % 256); }
            break;
        }
        case 5: s.insert(at, 1 + rng() % 4, '\0'); break;
        case 6: {  // a short piece many times over
            std::string piece = s.substr(at, 1 + rng() % 12), run;
            for (size_t k = 0, n = 16 + rng() % 4096; k < n && run.size() < (64u << 10); k++) run += piece;
            s.insert(at, run);
            break;
        }
        default: s.resize(at); break;
        }
        if (s.size() > (128u << 10)) s.resize(128u << 10);
    }
    return out + s;
}

int wmain(int argc, wchar_t** argv) {
    double seconds = 0, timeout = 30;
    unsigned seed = (unsigned)GetTickCount();
    std::vector<std::wstring> files;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--seconds" && i + 1 < argc) seconds = wcstod(argv[++i], nullptr);
        else if (a == L"--seed" && i + 1 < argc) seed = (unsigned)wcstoul(argv[++i], nullptr, 10);
        else if (a == L"--timeout" && i + 1 < argc) timeout = wcstod(argv[++i], nullptr);
        else Collect(a, files);
    }
    CreateDirectoryW(L"out", nullptr);
    CreateDirectoryW(L"out\\fuzz", nullptr);
    std::thread([timeout] {
        for (;;) {
            Sleep(200);
            uint64_t t = g_started.load();
            if (t && GetTickCount64() - t > (uint64_t)(timeout * 1000)) {
                CopyFileW(L"out\\fuzz\\target-last.bin", L"out\\fuzz\\target-hang.bin", FALSE);
                wprintf(L"an input has taken over %.0f s: a hang - it is out\\fuzz\\target-hang.bin\n", timeout);
                fflush(stdout);
                ExitProcess(4);
            }
        }
    }).detach();
    std::vector<std::string> seeds;
    for (const std::wstring& f : files) {
        std::string bytes;
        if (!ReadAll(f, bytes)) { fwprintf(stderr, L"cannot read %ls\n", f.c_str()); return 1; }
        if (seconds > 0) { seeds.push_back(std::move(bytes)); continue; }
        wprintf(L"  %ls (%zu bytes)\n", f.c_str(), bytes.size());
        fflush(stdout);
        // a regression input is a document, not a libFuzzer input: it runs in every mode the first byte can pick
        for (int mode = 0; mode < 16; mode++) Run(std::string(1, (char)mode) + bytes);
    }
    if (seconds <= 0) {
        wprintf(L"%zu inputs replayed in 16 modes each\n", files.size());
        return 0;
    }
    if (seeds.empty()) { fwprintf(stderr, L"no seed files\n"); return 2; }
    std::mt19937 rng(seed);
    const uint64_t t0 = GetTickCount64();
    long runs = 0;
    while (GetTickCount64() - t0 < (uint64_t)(seconds * 1000)) {
        Run(Mutate(rng, seeds));
        if ((++runs & 4095) == 0) wprintf(L"  %ld runs, %.0f s\n", runs, (GetTickCount64() - t0) / 1000.0);
    }
    wprintf(L"done: %ld runs in %.0f s (seed %u), nothing fell over\n", runs, (GetTickCount64() - t0) / 1000.0, seed);
    return 0;
}
#endif
