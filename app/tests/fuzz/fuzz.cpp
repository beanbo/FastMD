// Fuzzing the parts that read untrusted input (plan 6.5): the Markdown parser and the HTML tag reader.
//
// libFuzzer needs clang-cl, which is not installed here, so this is a small mutation loop of its own: it takes the
// documents in the corpus as seeds, mutates them (bit flips, cut, duplicate, splice, garbage runs), and parses the
// result. A build with /fsanitize=address turns any out-of-bounds read or use-after-free into an immediate failure.
//
//   pwsh -File app/tests/fuzz/run.ps1                 # build with ASan and run for a minute
//   fastmd-fuzz.exe --runs 100000 --seed 7            # or drive it directly
//   fastmd-fuzz.exe --file broken.md                  # parse one file and stop (for a case that already failed)
//
// The input being parsed is always in app/tests/out/fuzz/last.md, so a crash can be replayed with --file.
//
// Every input is also parsed the way edit mode parses (with the text <-> source map) and checked by MapSelfCheck
// (docs/EDIT-MODE.md §4.5, §14.2) and by the corpus sweep's round trips (tests/edit/map_sweep.h, strided to a few
// thousand positions per input). Half the inputs see the formula and diagram libraries as present, half as absent,
// and a third get CRLF line ends. A map that breaks an invariant stops the run with exit code 3 and leaves the input
// as out/fuzz/map-<n>-tex<0|1>.u16 (the exact UTF-16, lone surrogates and all) plus a readable .md copy:
//   fastmd-fuzz.exe --file out\fuzz\map-123-tex1.u16 --tex 1
// the standard headers come first: windows.h leaves macros behind that upset them
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "doc.h"
#include "editcore.h"
#include "html.h"
#include "../edit/map_sweep.h"

// The parser asks whether the formula and diagram libraries are there: a formula is then a picture (an object atom in
// the map) instead of code text. The fuzzer flips this per input so both paths of the map are exercised.
bool g_texOn = false;
bool TexAvailable() { return g_texOn; }
bool MermaidAvailable() { return g_texOn; }

namespace {
// Inputs are kept to this size: past a couple of hundred kilobytes a mutation finds nothing new, and with
// AddressSanitizer every extra kilobyte costs real time.
const size_t kMaxInput = 256u << 10;

std::vector<std::wstring> g_seeds;

std::wstring ReadUtf8File(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return L"";
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string bytes(n > 0 ? (size_t)n : 0, '\0');
    if (n > 0 && fread(bytes.data(), 1, (size_t)n, f) != (size_t)n) bytes.clear();
    fclose(f);
    int w = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring out(w > 0 ? w : 0, L'\0');
    if (w > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), out.data(), w);
    return out;
}

void WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string bytes(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), bytes.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr);
    CloseHandle(f);
}

void LoadSeeds(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.md").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring full = dir + L"\\" + fd.cFileName;
        char narrow[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, narrow, (int)std::size(narrow), nullptr, nullptr);
        std::wstring text = ReadUtf8File(narrow);
        if (!text.empty() && text.size() < kMaxInput) g_seeds.push_back(std::move(text));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// interesting characters to splice in: the ones our parser gives special meaning to (the NUL last: md4c replaces it)
const wchar_t kSpice[] = L"#*_`~[]()<>!|$\\\n\r\t \"'&;:^-+={}\x00A0\xFFFD\xD83D\xDE00\0";

std::wstring Mutate(std::mt19937& rng, const std::wstring& src) {
    std::wstring s = src;
    int rounds = 1 + (int)(rng() % 8);
    for (int r = 0; r < rounds && !s.empty(); r++) {
        size_t at = rng() % s.size();
        switch (rng() % 7) {
        case 0: s[at] = kSpice[rng() % (std::size(kSpice) - 1)]; break;                    // poke a special char in
        case 1: s[at] = (wchar_t)(rng() % 0x10000); break;                                 // any code unit at all
        case 2: s.erase(at, 1 + rng() % 64); break;                                        // cut a piece out
        case 3: s.insert(at, s.substr(at, std::min<size_t>(64, s.size() - at))); break;     // duplicate a piece
        case 4: {                                                                          // splice another seed in
            const std::wstring& other = g_seeds[rng() % g_seeds.size()];
            size_t from = rng() % other.size(), len = std::min<size_t>(other.size() - from, 1 + rng() % 256);
            s.insert(at, other, from, len);
            break;
        }
        case 5: s.resize(at); break;                                                       // truncate
        default: {                                                                         // a run of one character
            wchar_t c = kSpice[rng() % (std::size(kSpice) - 1)];
            s.insert(at, 1 + rng() % 200, c);
            break;
        }
        }
        if (s.size() > kMaxInput) s.resize(kMaxInput);  // bounded: a huge input only makes the run slower
    }
    return s;
}

bool g_trace = false;  // --file: say which stage is running, so a hang can be placed

std::wstring ToCrlf(const std::wstring& s) {  // every lone \n becomes \r\n
    std::wstring o;
    o.reserve(s.size() + s.size() / 16);
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) o += L'\r';
        o += s[i];
    }
    return o;
}

void WriteRawFile(const std::wstring& path, const std::wstring& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &wrote, nullptr);
    CloseHandle(f);
}
std::wstring ReadRawFile(const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (size >= sizeof(wchar_t)) ReadFile(f, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &got, nullptr);
    CloseHandle(f);
    return text;
}

// one round: the Markdown model, the model with edit mode's map (checked), then the HTML tag reader over the same
// bytes. Returns false when the map breaks an invariant; *why says which.
bool ParseOnce(const std::wstring& text, std::string* why) {
    if (g_trace) { wprintf(L"  markdown...\n"); fflush(stdout); }
    Doc d;
    d.baseDir = L"C:\\fuzz\\";
    ParseMarkdown(d, text.data(), text.size());
    if (g_trace) { wprintf(L"  markdown with the map...\n"); fflush(stdout); }
    Doc m;
    m.baseDir = d.baseDir;
    ParseOptions opt;
    opt.wantMap = true;
    ParseMarkdown(m, text.data(), text.size(), &opt);
    // the map must not change the document's structure (its text may differ in one place by design: a code block
    // keeps its empty last lines, F4)
    bool ok = MapSelfCheck(m, text, why);
    if (ok && m.blocks.size() != d.blocks.size()) {
        *why = "the map parse has a different number of blocks than the reading parse";
        ok = false;
    }
    for (size_t k = 0; ok && k < m.blocks.size(); k++) {
        if (m.blocks[k].kind != d.blocks[k].kind || m.blocks[k].heading != d.blocks[k].heading) {
            *why = "the map parse has a different block " + std::to_string(k) + " than the reading parse";
            ok = false;
        }
    }
    // the round trips of the corpus sweep, over a few thousand stops and offsets of each input: a map can pass the
    // self-check and still send a caret somewhere else
    if (ok) {
        if (g_trace) { wprintf(L"  round trips...\n"); fflush(stdout); }
        uint32_t stride = 1 + (uint32_t)(text.size() / 4000);
        size_t stops = 0, offsets = 0;
        mapsweep::SweepMap(m, text, stride, stride, &stops, &offsets, [&](const std::string& msg) {
            *why = "round trip: " + msg;
            ok = false;
            return false;
        });
    }
    if (g_trace) { wprintf(L"  tags...\n"); fflush(stdout); }
    // the tag reader is normally fed by md4c; here it is fed the raw text, which is harsher
    for (size_t i = 0; i + 1 < text.size(); i++) {
        if (text[i] != L'<') continue;
        HtmlTag tag;
        ParseHtmlTag(text.data() + i, text.size() - i, tag);
    }
    if (g_trace) { wprintf(L"  entities...\n"); fflush(stdout); }
    std::wstring out;
    AppendHtmlText(out, text.data(), text.size());
    if (g_trace) { wprintf(L"  done\n"); fflush(stdout); }
    return ok;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    long runs = 20000;
    unsigned seed = (unsigned)GetTickCount();
    std::wstring single, corpus = L"..\\..\\..\\bench\\corpus";
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--runs" && i + 1 < argc) runs = wcstol(argv[++i], nullptr, 10);
        else if (a == L"--seed" && i + 1 < argc) seed = (unsigned)wcstoul(argv[++i], nullptr, 10);
        else if (a == L"--file" && i + 1 < argc) single = argv[++i];
        else if (a == L"--corpus" && i + 1 < argc) corpus = argv[++i];
        else if (a == L"--tex" && i + 1 < argc) g_texOn = wcstol(argv[++i], nullptr, 10) != 0;
    }
    if (!single.empty()) {
        g_trace = true;
        std::wstring text;
        if (single.size() > 4 && _wcsicmp(single.c_str() + single.size() - 4, L".u16") == 0) {
            text = ReadRawFile(single.c_str());
        } else {
            char narrow[MAX_PATH * 2];
            WideCharToMultiByte(CP_UTF8, 0, single.c_str(), -1, narrow, (int)std::size(narrow), nullptr, nullptr);
            text = ReadUtf8File(narrow);
        }
        std::string why;
        bool ok = ParseOnce(text, &why);
        wprintf(L"parsed %s (tex %d): %hs\n", single.c_str(), g_texOn ? 1 : 0, ok ? "map ok" : why.c_str());
        return ok ? 0 : 3;
    }
    LoadSeeds(corpus);
    LoadSeeds(corpus + L"\\..\\..\\app\\tests");
    if (g_seeds.empty()) {
        wprintf(L"no seeds found in %s\n", corpus.c_str());
        return 2;
    }
    CreateDirectoryW(L"out", nullptr);
    CreateDirectoryW(L"out\\fuzz", nullptr);
    wprintf(L"fuzzing: %zu seeds, %ld runs, seed %u\n", g_seeds.size(), runs, seed);
    std::mt19937 rng(seed);
    DWORD t0 = GetTickCount();
    for (long i = 0; i < runs; i++) {
        std::wstring text = Mutate(rng, g_seeds[rng() % g_seeds.size()]);
        g_texOn = (rng() & 1) != 0;
        if (rng() % 3 == 0) text = ToCrlf(text);
        // the input is written out first: if the parse takes the process down, the file is what reproduces it
        WriteUtf8File(L"out\\fuzz\\last.md", text);
        std::string why;
        if (!ParseOnce(text, &why)) {
            std::wstring name = L"out\\fuzz\\map-" + std::to_wstring(i) + L"-tex" + (g_texOn ? L"1" : L"0");
            WriteRawFile(name + L".u16", text);
            WriteUtf8File(name + L".md", text);
            wprintf(L"MapSelfCheck failed on run %ld: %hs\n  input: app\\tests\\%s.u16 (replay with --file ... --tex %d)\n",
                    i, why.c_str(), name.c_str(), g_texOn ? 1 : 0);
            return 3;
        }
        if ((i & 1023) == 1023) wprintf(L"  %ld runs, %.1f s\n", i + 1, (GetTickCount() - t0) / 1000.0);
    }
    wprintf(L"done: %ld runs in %.1f s, nothing fell over, every map checked\n", runs, (GetTickCount() - t0) / 1000.0);
    return 0;
}
