// fastmd-edit-tests: edit mode's window-free core under test (docs/EDIT-MODE.md §14.1).
//
// It runs the golden cases of tests/edit/cases/*.txt and property sweeps over the corpus, on the very parser the app
// uses, with no window and no `g`. The exit code is the number of failures (0 = green).
//
//   pwsh -File app/tests/edit/run.ps1                     # build and run everything
//   fastmd-edit-tests.exe [--filter <text>] [--update] [--no-sweep] [--cases <dir>] [--corpus <dir>]...
//
// --update rewrites the `map:` blocks of the case files with what the parser produces now; a reviewer reads the diff.
//
// Case files. Text before the first case is a free comment. Every case starts with "=== <name> (<section>)", then
// fields, one per line, starting at column 0:
//   src:   the source; escapes \n \r \t \s (a space that would otherwise be trimmed) \0 \\ \uXXXX; ‸ marks a caret
//   tex:   on | off | both (default: both, on when the source holds '$'). Mermaid follows the same switch.
//   flags: noquote nolist nocrlf - skip the automatic variants
//   do:    caret - the ‸ of src is a source offset; TextOfSrc then SrcOfText(MAP_CARET) must land on want's ‸
//          or the operations, `;`-separated: type "x", BS, C-BS, Del, C-Del, Enter, S-Enter, C-Enter, Tab, S-Tab,
//          Paste("…"), Cut, Phantom (the caret into the phantom row), click(t,b[,c]) (the caret at a text position),
//          TaskToggle(n); src may hold a selection ⟦ … ⟧ (anchor first) instead of the ‸
//   want:  the expected source with its ‸ (or ⟦ … ⟧)
//   state: (operation cases) refused=<why> and atom=<none|blkN|N>: what the last operation left besides the source
//   phantom: (operation cases) none | after <b> | before <b> | break <b> [in] [style <n>] [depth <n>]: the phantom row
//          afterwards (default none)
//   new:   (diff cases) the source after an edit; with
//   diff:  p=<common prefix> q=<common suffix> of DiffBlocks(src, new)
//   map:   (map cases) the expected dump of the whole map, one record per line, indented by two spaces
// Automatic variants, unless flagged off: the same source with CRLF line ends (map cases: the map must be the LF map
// with every offset shifted by the CRs before it; caret cases: the carets move with the text), and the source wrapped
// in a quote ("> " on every line) and in a list item ("- ", then "  "), where map cases must pass MapSelfCheck and
// caret cases must land on the wrapped want. Every parse is checked by MapSelfCheck.
//
// Sweeps (bench/corpus/*.md, app/tests/*.md; large files strided), with TeX on and off: MapSelfCheck; at every caret
// stop SrcOfText in every mode gives an offset inside the block's lines and TextOfSrc(SrcOfText(t)) == t in both
// directions; TextOfSrc of any source offset is a caret stop (map_sweep.h, which the fuzzer runs too).
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "doc.h"
#include "editcore.h"
#include "editfile.h"
#include "map_sweep.h"
#include "../third_party/md4c/md4c.h"  // span type names in the dump

// The formula and diagram libraries are "there" or not per parse: a formula is a picture (an object atom) with them,
// code text without them, and the map has to be right both ways.
bool g_texOn = true;
bool TexAvailable() { return g_texOn; }
bool MermaidAvailable() { return g_texOn; }

namespace {
int g_failures = 0, g_checks = 0;
bool g_update = false;
std::string g_filter;

void Fail(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("[FAIL] %s\n", buf);
    g_failures++;
}
bool Check(bool ok, const char* fmt, ...) {
    g_checks++;
    if (ok) return true;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("[FAIL] %s\n", buf);
    g_failures++;
    return false;
}

// ------------------------------------------------------------------------------------------------ text helpers
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
bool ReadBytes(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    out.assign((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    bool ok = !out.size() || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    CloseHandle(f);
    return ok;
}
bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size();
    CloseHandle(f);
    return ok;
}
std::wstring ReadUtf8File(const std::wstring& path) {
    std::string b;
    if (!ReadBytes(path, b)) return L"";
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) b.erase(0, 3);
    return Wide(b);
}
std::vector<std::wstring> ListFiles(const std::wstring& dir, const wchar_t* pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// what a dump shows of a piece of source or text: control characters escaped, long pieces cut in the middle
std::string Esc(const std::wstring& s, size_t maxLen = 48) {
    std::wstring w;
    for (wchar_t c : s) {
        if (c == L'\n') w += L"\\n";
        else if (c == L'\r') w += L"\\r";
        else if (c == L'\t') w += L"\\t";
        else if (c == 0) w += L"\\0";
        else w += c;
    }
    if (w.size() > maxLen) w = w.substr(0, maxLen / 2) + L"…" + w.substr(w.size() - maxLen / 2);
    return Narrow(w);
}
std::string Slice(const std::wstring& src, uint32_t a, uint32_t b, size_t maxLen = 48) {
    a = std::min<uint32_t>(a, (uint32_t)src.size());
    b = std::min<uint32_t>(std::max(a, b), (uint32_t)src.size());
    return Esc(src.substr(a, b - a), maxLen);
}
std::string Fmt(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

// ------------------------------------------------------------------------------------------------ parsing
struct Parsed {
    Doc d;
    std::wstring src;
};
void ParseMap(Parsed& p, const std::wstring& src) {
    p.src = src;
    p.d = Doc{};
    p.d.baseDir = L"C:\\edit-tests\\";
    ParseOptions opt;
    opt.wantMap = true;
    ParseMarkdown(p.d, p.src.data(), p.src.size(), &opt);
}

// ------------------------------------------------------------------------------------------------ the map dump
const char* BlockKindName(const Block& b) {
    static const char* h[] = {"P", "H1", "H2", "H3", "H4", "H5", "H6"};
    switch (b.kind) {
    case BK_TEXT: return b.heading <= 6 ? h[b.heading] : "H?";
    case BK_CODE: return "CODE";
    case BK_HR: return "HR";
    case BK_TABLE: return "TABLE";
    case BK_IMAGE: return "IMAGE";
    }
    return "?";
}
std::string BlockFlags(uint16_t f) {
    static const struct { uint16_t bit; const char* name; } names[] = {
        {BS_RAW, "RAW"}, {BS_SETEXT, "SETEXT"}, {BS_ATX, "ATX"}, {BS_FENCED, "FENCED"}, {BS_UNCLOSED, "UNCLOSED"},
        {BS_SYNTH, "SYNTH"}, {BS_FOOTNOTE, "FOOTNOTE"}, {BS_EMPTYITEM, "EMPTYITEM"}, {BS_OBJECT, "OBJECT"},
        {BS_NOCONTENT, "NOCONTENT"}, {BS_RAWTEXT, "RAWTEXT"}, {BS_FRONT, "FRONT"}, {BS_HTML, "HTML"}};
    std::string s;
    for (auto& n : names)
        if (f & n.bit) s += (s.empty() ? "" : "|") + std::string(n.name);
    return s.empty() ? "-" : s;
}
const char* SegName(uint8_t k) {
    static const char* n[] = {"P", "TEXT", "OBJ", "SYNTH"};
    return k < 4 ? n[k] : "?";
}
std::string SpanType(uint8_t t) {
    switch (t) {
    case MD_SPAN_EM: return "EM";
    case MD_SPAN_STRONG: return "STRONG";
    case MD_SPAN_A: return "A";
    case MD_SPAN_IMG: return "IMG";
    case MD_SPAN_CODE: return "CODE";
    case MD_SPAN_DEL: return "DEL";
    case MD_SPAN_LATEXMATH: return "MATH";
    case MD_SPAN_LATEXMATH_DISPLAY: return "MATHD";
    case MD_SPAN_WIKILINK: return "WIKI";
    case MD_SPAN_U: return "U";
    case MD_SPAN_FOOTNOTE_REF: return "FNREF";
    case ST_HTML_B: return "<b>";
    case ST_HTML_I: return "<i>";
    case ST_HTML_CODE: return "<code>";
    case ST_HTML_S: return "<s>";
    case ST_HTML_KBD: return "<kbd>";
    case ST_HTML_SUP: return "<sup>";
    case ST_HTML_SUB: return "<sub>";
    case ST_HTML_A: return "<a>";
    }
    return Fmt("span%u", t);
}
std::string SpanFlagNames(uint8_t f) {
    std::string s;
    if (f & SF_AUTOLINK) s += " AUTO";
    if (f & SF_REF) s += " REF";
    if (f & SF_UNCLOSED) s += " UNCLOSED";
    if (f & SF_ENTERABLE) s += " ENTER";
    if (f & SF_UNDERSCORE) s += " _";
    return s;
}
std::string Off(uint32_t v, const std::function<uint32_t(uint32_t)>& tr) {
    return v == UINT32_MAX ? std::string("-") : std::to_string(tr(v));
}

// One line per record: blocks (with their segments, spans and, for tables, cells and rows), containers, pictures and
// the source order. `tr` maps every source offset (identity, or CRLF -> LF for the comparison of the two variants);
// `slices` adds the pieces of source the offsets point at, for a person reading the golden file.
std::vector<std::string> DumpMap(const Doc& d, const std::wstring& src, const std::function<uint32_t(uint32_t)>& tr,
                                 bool slices) {
    std::vector<std::string> out;
    for (uint32_t k = 0; k < d.blocks.size() && k < d.blockSrc.size(); k++) {
        const Block& b = d.blocks[k];
        const BlockSrc& bs = d.blockSrc[k];
        std::string line = Fmt("b%u %s %s", k, BlockKindName(b), BlockFlags(bs.flags).c_str());
        if (!(bs.flags & BS_SYNTH)) {
            line += " line=" + Off(bs.line, tr) + " beg=" + Off(bs.beg, tr) + " end=" + Off(bs.end, tr) +
                    " le=" + Off(bs.lineEnd, tr) + " outer=" + Off(bs.outerEnd, tr);
            if (bs.aux != UINT32_MAX) line += " aux=" + Off(bs.aux, tr);
        }
        if (bs.container >= 0) line += Fmt(" in=c%d", bs.container);
        if (bs.rawId >= 0) line += Fmt(" raw=b%d", bs.rawId);
        if (!(bs.flags & BS_SYNTH)) {
            std::wstring pre = ContPrefix(d, src, (int)k), blank = BlankPrefix(d, src, (int)k);
            if (!pre.empty()) line += " pre=\"" + Esc(pre) + "\"";
            if (blank != pre && !blank.empty()) line += " blank=\"" + Esc(blank) + "\"";
        }
        line += " \"" + Esc(d.text.substr(b.textOff, b.textLen), 40) + "\"";
        if (slices && !(bs.flags & BS_SYNTH)) {
            line += " «" + Slice(src, bs.line, bs.beg, 24) + "⟨" + Slice(src, bs.beg, bs.end) + "⟩" +
                    Slice(src, bs.end, bs.lineEnd, 24);
            if (bs.outerEnd > bs.lineEnd) line += "|" + Slice(src, bs.lineEnd, bs.outerEnd, 24);
            line += "»";
        }
        // text positions of the block that are not caret stops (strictly inside atoms, after synthesized text)
        if (b.kind != BK_TABLE && !(bs.flags & (BS_SYNTH | BS_OBJECT | BS_RAW))) {
            std::string no;
            int count = 0;
            for (uint32_t t = b.textOff; t <= b.textOff + b.textLen; t++)
                if (!CaretStop(d, TextPos{t, (int32_t)k, -1}) && count++ < 12) no += (no.empty() ? "" : ",") + std::to_string(t);
            if (!no.empty()) line += " nostop=" + no;
        }
        out.push_back(line);
        for (uint32_t i = bs.segOff; i < bs.segOff + bs.segCount && i < d.segs.size(); i++) {
            const SrcSeg& g = d.segs[i];
            std::string sl = Fmt("  s %u+%u %s ", g.t, g.tLen, SegName(g.kind)) + Off(g.s, tr) + ".." + Off(g.s + g.sLen, tr);
            if (g.flags & SEGF_SPLITTAB) sl += " SPLITTAB";
            if (slices) {
                sl += " «" + Slice(src, g.s, g.s + g.sLen) + "»";
                if (g.kind != SEG_PLAIN) sl += " \"" + Esc(d.text.substr(g.t, g.tLen)) + "\"";
            }
            out.push_back(sl);
        }
        for (uint32_t i = bs.spanOff; i < bs.spanOff + bs.spanCount && i < d.spans.size(); i++) {
            const SpanSrc& sp = d.spans[i];
            std::string sl = Fmt("  span %s t%u..%u o", SpanType(sp.type).c_str(), sp.tBeg, sp.tEnd) + Off(sp.openBeg, tr) +
                             ".." + Off(sp.openEnd, tr) + " c" + Off(sp.closeBeg, tr) + ".." + Off(sp.closeEnd, tr) +
                             SpanFlagNames(sp.flags);
            if (slices) sl += " «" + Slice(src, sp.openBeg, sp.openEnd) + "»«" + Slice(src, sp.closeBeg, sp.closeEnd) + "»";
            out.push_back(sl);
        }
        if (b.kind == BK_TABLE && b.aux < d.tables.size() && !(bs.flags & BS_RAW)) {
            const Table& tb = d.tables[b.aux];
            for (uint32_t c = 0; c < tb.rows * tb.cols && tb.cellOff + c < d.cellSrc.size(); c++) {
                const CellSrc& cs = d.cellSrc[tb.cellOff + c];
                const Cell& cell = d.cells[tb.cellOff + c];
                std::string sl = Fmt("  cell %u,%u t%u+%u ", c / tb.cols, c % tb.cols, cell.textOff, cell.textLen);
                if (cs.missing) sl += "missing@" + Off(cs.beg, tr);
                else sl += Off(cs.beg, tr) + ".." + Off(cs.end, tr) + (slices ? " «" + Slice(src, cs.beg, cs.end) + "»" : "");
                out.push_back(sl);
            }
            if (b.aux < d.tableSrc.size()) {
                const TableSrc& ts = d.tableSrc[b.aux];
                for (size_t r = 0; r < ts.rows.size(); r++) {
                    const RowSrc& row = ts.rows[r];
                    std::string sl = Fmt("  row %zu ", r) + Off(row.lineStart, tr) + "/" + Off(row.contentStart, tr) + ".." +
                                     Off(row.lineEnd, tr) + " pipes";
                    for (uint32_t p : row.pipes) sl += " " + Off(p, tr);
                    out.push_back(sl);
                }
            }
        }
    }
    static const char* ck[] = {"QUOTE", "ALERT", "ITEM", "FOOTNOTE"};
    for (size_t i = 0; i < d.containers.size(); i++) {
        const ContainerSrc& c = d.containers[i];
        std::string sl = Fmt("c%zu %s up=%d ", i, c.kind < 4 ? ck[c.kind] : "?", c.parent);
        sl += c.firstBlock == UINT32_MAX ? std::string("empty") : Fmt("b%u..%u", c.firstBlock, c.lastBlock);
        if (c.kind == CT_ITEM || c.kind == CT_FOOTNOTE) sl += " mark=" + Off(c.markOff, tr);
        if (c.kind == CT_ITEM) {
            sl += Fmt("+%u", c.markLen);
            if (c.bullet) sl += Fmt(" '%c'", (char)c.bullet);
            else sl += Fmt(" %u'%c'", c.number, c.delim ? (char)c.delim : '?');
            sl += Fmt(" col=%u", c.contentCol);
            if (c.taskOff != UINT32_MAX) sl += " task=" + Off(c.taskOff, tr);
            if (!c.tight) sl += " loose";
        }
        out.push_back(sl);
    }
    for (size_t i = 0; i < d.images.size(); i++) {
        const Image& im = d.images[i];
        if (im.outerBeg == UINT32_MAX) continue;
        std::string sl = Fmt("img%zu m%u outer=", i, im.mathKind) + Off(im.outerBeg, tr) + ".." + Off(im.outerEnd, tr) +
                         " src=" + Off(im.srcBeg, tr) + ".." + Off(im.srcEnd, tr) + " alt=" + Off(im.altBeg, tr) + ".." +
                         Off(im.altEnd, tr);
        if (slices && im.srcBeg != UINT32_MAX) sl += " «" + Slice(src, im.srcBeg, im.srcEnd) + "»";
        out.push_back(sl);
    }
    std::string order = "order";
    for (uint32_t k : d.blockOrder) order += Fmt(" %u", k);
    out.push_back(order);
    return out;
}

// ------------------------------------------------------------------------------------------------ variants
std::wstring ToCrlf(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\n') o += L'\r';
        o += c;
    }
    return o;
}
std::wstring WrapLines(const std::wstring& s, const wchar_t* first, const wchar_t* rest, const wchar_t* blank) {
    std::wstring o;
    size_t i = 0;
    bool firstLine = true;
    while (i < s.size()) {
        size_t e = s.find(L'\n', i);
        size_t end = e == std::wstring::npos ? s.size() : e;
        std::wstring line = s.substr(i, end - i);
        bool isBlank = line.find_first_not_of(L" \t\r") == std::wstring::npos;
        o += firstLine ? first : isBlank ? blank : rest;
        o += line;
        if (e != std::wstring::npos) o += L'\n';
        firstLine = false;
        i = e == std::wstring::npos ? s.size() : e + 1;
    }
    return o;
}
std::wstring WrapQuote(const std::wstring& s) { return WrapLines(s, L"> ", L"> ", L">"); }
std::wstring WrapList(const std::wstring& s) { return WrapLines(s, L"- ", L"  ", L""); }

// ------------------------------------------------------------------------------------------------ case files
struct Case {
    std::string name, file;
    int line = 0;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<std::string> map;
    bool hasMap = false;
    size_t mapFirst = 0, mapEnd = 0;  // the map's lines in the file, for --update
    const std::string* Get(const char* k) const {
        for (auto& f : fields)
            if (f.first == k) return &f.second;
        return nullptr;
    }
};

std::wstring Unescape(const std::string& raw) {
    std::wstring w = Wide(raw), o;
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] != L'\\' || i + 1 >= w.size()) {
            o += w[i];
            continue;
        }
        wchar_t c = w[++i];
        switch (c) {
        case L'n': o += L'\n'; break;
        case L'r': o += L'\r'; break;
        case L't': o += L'\t'; break;
        case L's': o += L' '; break;
        case L'0': o += L'\0'; break;
        case L'\\': o += L'\\'; break;
        case L'u':
            if (i + 4 < w.size()) {
                o += (wchar_t)wcstoul(w.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
            }
            break;
        default: o += L'\\'; o += c; break;
        }
    }
    return o;
}

std::vector<std::string> SplitLines(const std::string& bytes) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < bytes.size()) {
        size_t e = bytes.find('\n', i);
        size_t end = e == std::string::npos ? bytes.size() : e;
        std::string l = bytes.substr(i, end - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        i = e == std::string::npos ? bytes.size() : e + 1;
    }
    return lines;
}
std::string RTrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

std::vector<Case> ReadCases(const std::wstring& path, std::vector<std::string>& lines) {
    std::vector<Case> cases;
    std::string bytes;
    if (!ReadBytes(path, bytes)) {
        Fail("cannot read %s", Narrow(path).c_str());
        return cases;
    }
    lines = SplitLines(bytes);
    std::string file = Narrow(path.substr(path.find_last_of(L'\\') + 1));
    Case* cur = nullptr;
    bool inMap = false;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& l = lines[i];
        if (l.rfind("=== ", 0) == 0) {
            if (cur && inMap) cur->mapEnd = i;
            cases.push_back(Case{});
            cur = &cases.back();
            cur->name = RTrim(l.substr(4));
            cur->file = file;
            cur->line = (int)i + 1;
            inMap = false;
            continue;
        }
        if (!cur) continue;  // the file's header comment
        if (inMap && (l.rfind("  ", 0) == 0 || l.empty())) {
            if (!l.empty()) cur->map.push_back(RTrim(l.substr(2)));
            cur->mapEnd = i + 1;
            continue;
        }
        inMap = false;
        if (l.empty()) continue;
        size_t colon = l.find(':');
        if (colon == std::string::npos || colon > 8) {
            Fail("%s:%zu: not a field: %s", file.c_str(), i + 1, l.c_str());
            continue;
        }
        std::string key = l.substr(0, colon), value = l.substr(colon + 1);
        size_t p = value.find_first_not_of(" \t");
        value = p == std::string::npos ? "" : RTrim(value.substr(p));
        if (key == "map") {
            cur->hasMap = true;
            cur->mapFirst = cur->mapEnd = i + 1;
            inMap = true;
            continue;
        }
        cur->fields.emplace_back(key, value);
    }
    // a case's map runs to its last indented line (blank lines after it belong to the gap between cases)
    for (Case& c : cases)
        while (c.hasMap && c.mapEnd > c.mapFirst && lines[c.mapEnd - 1].empty()) c.mapEnd--;
    return cases;
}

std::vector<int> TexModes(const Case& c, const std::wstring& src) {
    const std::string* t = c.Get("tex");
    if (t && *t == "on") return {1};
    if (t && *t == "off") return {0};
    if (t && *t == "both") return {1, 0};
    if (src.find(L'$') != std::wstring::npos) return {1};
    return {1, 0};
}
bool HasFlag(const Case& c, const char* f) {
    const std::string* v = c.Get("flags");
    return v && v->find(f) != std::string::npos;
}

// every offset of a CRLF source moved back to where it is in the LF source (the CRs before it removed)
std::function<uint32_t(uint32_t)> CrlfToLf(const std::wstring& crlf) {
    auto crs = std::make_shared<std::vector<uint32_t>>(crlf.size() + 1, 0);
    for (size_t i = 0; i < crlf.size(); i++) (*crs)[i + 1] = (*crs)[i] + (crlf[i] == L'\r');
    return [crs](uint32_t o) { return o < crs->size() ? o - (*crs)[o] : o; };
}

void SelfCheck(const Parsed& p, const std::string& what) {
    std::string why;
    bool ok = MapSelfCheck(p.d, p.src, &why);
    Check(ok, "%s: MapSelfCheck: %s", what.c_str(), why.c_str());
}

// ---- map cases
bool RunMapCase(Case& c, std::vector<std::string>& fileLines, bool& fileChanged) {
    const std::string* s = c.Get("src");
    if (!s) {
        Fail("%s:%d %s: no src", c.file.c_str(), c.line, c.name.c_str());
        return false;
    }
    std::wstring src = Unescape(*s);
    int before = g_failures;
    std::vector<std::string> first;
    for (int tex : TexModes(c, src)) {
        g_texOn = tex != 0;
        std::string what = c.name + (tex ? " [tex on]" : " [tex off]");
        Parsed p;
        ParseMap(p, src);
        SelfCheck(p, what);
        std::vector<std::string> got = DumpMap(p.d, p.src, [](uint32_t o) { return o; }, true);
        if (first.empty()) first = got;
        if (got != c.map) {
            if (g_update && tex == TexModes(c, src)[0]) {
                c.map = got;
                fileLines.erase(fileLines.begin() + c.mapFirst, fileLines.begin() + c.mapEnd);
                std::vector<std::string> indented;
                for (auto& l : got) indented.push_back("  " + l);
                fileLines.insert(fileLines.begin() + c.mapFirst, indented.begin(), indented.end());
                fileChanged = true;
                printf("[UPDATE] %s\n", what.c_str());
            } else {
                size_t k = 0;
                while (k < got.size() && k < c.map.size() && got[k] == c.map[k]) k++;
                Fail("%s:%d %s: the map differs at line %zu\n  want: %s\n  got:  %s", c.file.c_str(), c.line, what.c_str(),
                     k + 1, k < c.map.size() ? c.map[k].c_str() : "(end)", k < got.size() ? got[k].c_str() : "(end)");
            }
        } else {
            g_checks++;
        }
        // CRLF: the same map, every offset shifted by the CRs before it
        if (!HasFlag(c, "nocrlf")) {
            Parsed q;
            ParseMap(q, ToCrlf(src));
            SelfCheck(q, what + " [crlf]");
            auto lf = DumpMap(p.d, p.src, [](uint32_t o) { return o; }, false);
            auto cr = DumpMap(q.d, q.src, CrlfToLf(q.src), false);
            size_t k = 0;
            while (k < lf.size() && k < cr.size() && lf[k] == cr[k]) k++;
            Check(lf == cr, "%s [crlf]: the CRLF map is not the LF map at line %zu\n  lf:   %s\n  crlf: %s", what.c_str(), k + 1,
                  k < lf.size() ? lf[k].c_str() : "(end)", k < cr.size() ? cr[k].c_str() : "(end)");
        }
        if (!HasFlag(c, "noquote")) {
            Parsed q;
            ParseMap(q, WrapQuote(src));
            SelfCheck(q, what + " [quote]");
        }
        if (!HasFlag(c, "nolist")) {
            Parsed q;
            ParseMap(q, WrapList(src));
            SelfCheck(q, what + " [list]");
        }
    }
    return g_failures == before;
}

// ---- caret cases: TextOfSrc then SrcOfText(MAP_CARET) (§6.3)
const wchar_t kCaret = 0x2038;  // ‸
bool CaretAt(const std::wstring& marked, std::wstring* plain, uint32_t* at) {
    size_t k = marked.find(kCaret);
    if (k == std::wstring::npos || marked.find(kCaret, k + 1) != std::wstring::npos) return false;
    *plain = marked.substr(0, k) + marked.substr(k + 1);
    *at = (uint32_t)k;
    return true;
}
void CaretVariant(const Case& c, const std::string& what, const std::wstring& srcMarked, const std::wstring& wantMarked) {
    std::wstring src, want;
    uint32_t s0, s1;
    if (!CaretAt(srcMarked, &src, &s0) || !CaretAt(wantMarked, &want, &s1)) {
        Fail("%s: src and want need one ‸ each", what.c_str());
        return;
    }
    if (!Check(src == want, "%s: want is not the same source as src", what.c_str())) return;
    Parsed p;
    ParseMap(p, src);
    SelfCheck(p, what);
    TextPos t = TextOfSrc(p.d, p.src, s0, -1, nullptr);
    if (!Check(CaretStop(p.d, t), "%s: TextOfSrc(%u) = t%u b%d c%d is not a caret stop", what.c_str(), s0, t.t, t.block, t.cell))
        return;
    uint32_t got = SrcOfText(p.d, p.src, t, MAP_CARET);
    std::wstring shown = got <= src.size() ? src.substr(0, got) + kCaret + src.substr(got) : L"(none)";
    Check(got == s1, "%s: the caret maps to %u, not %u\n  want: %s\n  got:  %s", what.c_str(), got, s1,
          Esc(wantMarked, 200).c_str(), Esc(shown, 200).c_str());
}
void RunCaretCase(const Case& c) {
    const std::string* s = c.Get("src");
    const std::string* w = c.Get("want");
    if (!s || !w) {
        Fail("%s:%d %s: a caret case needs src and want", c.file.c_str(), c.line, c.name.c_str());
        return;
    }
    std::wstring src = Unescape(*s), want = Unescape(*w);
    for (int tex : TexModes(c, src)) {
        g_texOn = tex != 0;
        std::string what = c.name + (tex ? " [tex on]" : " [tex off]");
        CaretVariant(c, what, src, want);
        if (!HasFlag(c, "nocrlf")) CaretVariant(c, what + " [crlf]", ToCrlf(src), ToCrlf(want));
        if (!HasFlag(c, "noquote")) CaretVariant(c, what + " [quote]", WrapQuote(src), WrapQuote(want));
        if (!HasFlag(c, "nolist")) CaretVariant(c, what + " [list]", WrapList(src), WrapList(want));
    }
}

// ---- diff cases (§5.5 step 5)
void RunDiffCase(const Case& c) {
    const std::string* s = c.Get("src");
    const std::string* n = c.Get("new");
    const std::string* want = c.Get("diff");
    if (!s || !n || !want) {
        Fail("%s:%d %s: a diff case needs src, new and diff", c.file.c_str(), c.line, c.name.c_str());
        return;
    }
    for (int tex : TexModes(c, Unescape(*s))) {
        g_texOn = tex != 0;
        Parsed a, b;
        ParseMap(a, Unescape(*s));
        ParseMap(b, Unescape(*n));
        SelfCheck(a, c.name + " [old]");
        SelfCheck(b, c.name + " [new]");
        BlockDiff d = DiffBlocks(a.d, b.d);
        std::string got = Fmt("p=%u q=%u", d.p, d.q);
        Check(got == *want, "%s:%d %s%s: DiffBlocks gives %s, want %s", c.file.c_str(), c.line, c.name.c_str(),
              tex ? " [tex on]" : " [tex off]", got.c_str(), want->c_str());
    }
}

// ---- operation cases (§14.1): the operations on a caret (‸) or a selection (⟦ anchor … ⟧ focus), one after the
// other as `do:` lists them (type "…", BS, C-BS, Del, C-Del, TaskToggle(n)); each result is re-parsed and the caret
// normalised as the glue does (§6.4), and the typing check of §7.3 step 5 picks the fallback the glue would.
const wchar_t kSelA = 0x27E6, kSelB = 0x27E7;  // ⟦ ⟧
bool ParseMarked(const std::wstring& m, std::wstring* src, uint32_t* anchor, uint32_t* focus) {
    size_t c = m.find(kCaret), a = m.find(kSelA), b = m.find(kSelB);
    if (c != std::wstring::npos && a == std::wstring::npos && b == std::wstring::npos && m.find(kCaret, c + 1) == std::wstring::npos) {
        *src = m.substr(0, c) + m.substr(c + 1);
        *anchor = *focus = (uint32_t)c;
        return true;
    }
    if (c == std::wstring::npos && a != std::wstring::npos && b != std::wstring::npos && a < b) {
        *src = m.substr(0, a) + m.substr(a + 1, b - a - 1) + m.substr(b + 1);
        *anchor = (uint32_t)a;
        *focus = (uint32_t)(b - 1);
        return true;
    }
    return false;
}
std::wstring Marked(const std::wstring& src, uint32_t anchor, uint32_t focus) {
    if (anchor == focus) return src.substr(0, focus) + kCaret + src.substr(focus);
    uint32_t a = std::min(anchor, focus), b = std::max(anchor, focus);
    return src.substr(0, a) + kSelA + src.substr(a, b - a) + kSelB + src.substr(b);
}
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::vector<std::string> SplitOps(const std::string& s) {  // on ';' outside quotes
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && q && i + 1 < s.size()) { cur += s[i]; cur += s[++i]; continue; }
        if (s[i] == '"') q = !q;
        if (s[i] == ';' && !q) { out.push_back(Trim(cur)); cur.clear(); continue; }
        cur += s[i];
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    return out;
}

struct OpRun { std::wstring src; EditState st; std::string refused; int dir = 1; TextPos click{0, -1, -1}; };

bool InPhantom(const EditState& st) { return st.phantom.kind != PH_NONE && st.phantom.in; }

// a quoted argument: Paste("…")
bool QuotedArg(const std::string& op, const char* name, std::wstring* out) {
    size_t n = strlen(name);
    if (op.rfind(name, 0) != 0 || op.size() < n + 3 || op[n] != '(' || op[n + 1] != '"' || op.substr(op.size() - 2) != "\")")
        return false;
    *out = Unescape(op.substr(n + 2, op.size() - n - 4));
    return true;
}

bool DoOp(OpRun& r, const std::string& op, const std::string& what) {
    Parsed p;
    ParseMap(p, r.src);
    const wchar_t* eol = r.src.find(L"\r\n") != std::wstring::npos ? L"\r\n" : L"\n";
    EditCtx c{p.d, p.src, eol, GraphemeLite, nullptr, 0};
    if (r.click.block >= 0) {  // a click said which block and cell (an empty cell shares its offset with the next one)
        c.focusPos = c.anchorPos = r.click;
        r.click = TextPos{0, -1, -1};
    }
    EditResult res;
    std::wstring typed, arg;
    if (op.rfind("type \"", 0) == 0 && op.size() >= 7 && op.back() == '"') {
        typed = Unescape(op.substr(6, op.size() - 7));
        res = OpType(c, r.st, typed);
    } else if (op == "BS" || op == "C-BS") {
        res = OpBackspace(c, r.st, op == "C-BS");
    } else if (op == "Del" || op == "C-Del") {
        res = OpDelete(c, r.st, op == "C-Del");
    } else if (op == "Enter" || op == "S-Enter" || op == "C-Enter") {
        res = OpEnter(c, r.st, op[0] == 'S' ? 1 : op[0] == 'C' ? 2 : 0);
    } else if (op == "Tab" || op == "S-Tab") {
        res = OpTab(c, r.st, op[0] == 'S');
    } else if (QuotedArg(op, "Paste", &arg)) {
        res = OpPaste(c, r.st, arg, false);
    } else if (op == "Cut") {
        res = OpDeleteSelection(c, r.st);
        res.kind = EK_CUT;
    } else if (op == "Phantom") {  // the caret goes into the phantom row (as ↑ / ↓ into it, §6.7)
        if (!Check(r.st.phantom.kind != PH_NONE, "%s: Phantom: there is none", what.c_str())) return false;
        r.st.phantom.in = 1;
        r.st.focus = r.st.anchor = r.st.phantom.anchorSrc;
        return true;
    } else if (op.rfind("click(", 0) == 0) {  // click(t,b[,c]): the caret at a text position
        int t = 0, b = -1, cl = -1;
        sscanf_s(op.c_str() + 6, "%d,%d,%d", &t, &b, &cl);
        r.click = TextPos{(uint32_t)t, b, cl};
        uint32_t s = SrcOfText(p.d, p.src, r.click, MAP_CARET);
        if (!Check(s != UINT32_MAX, "%s: %s maps nowhere", what.c_str(), op.c_str())) return false;
        r.st = EditState{};
        r.st.focus = r.st.anchor = s;
        return true;
    } else if (op.rfind("TaskToggle(", 0) == 0) {
        res = OpTaskToggle(c, r.st, atoi(op.c_str() + 11));
    } else {
        Fail("%s: unknown op \"%s\"", what.c_str(), op.c_str());
        return false;
    }
    if (!res.refused.empty()) {
        r.refused = res.refused;
        return true;
    }
    const std::wstring before = r.src;
    std::wstring s = r.src;
    std::string why;
    for (const Splice& sp : res.splices)
        Check(!SpliceSplits(s, sp.at, (uint32_t)sp.removed.size()), "%s %s: a splice cuts a pair or a CRLF", what.c_str(), op.c_str());
    if (!Check(ApplySplices(s, res.splices, false, &why), "%s %s: %s", what.c_str(), op.c_str(), why.c_str())) return false;
    // §7.3 step 5: an ordinary character must render as itself at the caret; else the fallbacks, in order. As the glue
    // does, not for blanks (OpType places them) nor after trailing blanks (they render as nothing until text follows).
    uint16_t tr = 0;
    uint32_t t0 = TextOfSrc(p.d, p.src, r.st.focus, 1, &tr).t;
    bool blanks = std::all_of(typed.begin(), typed.end(), [](wchar_t ch) { return ch == L' ' || ch == L'\t'; });
    if (!typed.empty() && !blanks && !tr && NeedsTypeCheck(typed) && res.splices.size() == 1 && res.splices[0].removed.empty() &&
        r.st.anchor == r.st.focus) {
        Parsed q;
        ParseMap(q, s);
        if (!TypedOk(p.d, t0, q.d, typed)) {
            for (const TypeCandidate& cand : TypeFallbacks(c, r.st, res.splices[0].at, typed)) {
                std::wstring s2 = r.src;
                s2.insert(cand.at, cand.text);
                Parsed q2;
                ParseMap(q2, s2);
                if (TypedOk(p.d, t0, q2.d, cand.rendered)) {
                    s = s2;
                    res.splices = {Splice{cand.at, L"", cand.text}};
                    res.after.focus = res.after.anchor = cand.caret;
                    break;
                }
            }
        }
    }
    std::wstring inv = s;
    Check(ApplySplices(inv, res.splices, true, &why) && inv == before, "%s %s: the inverse splices do not give the old source back",
          what.c_str(), op.c_str());
    Parsed n;
    ParseMap(n, s);
    // §7.5 step 5: a step that wrote delimiters back must render the old text around the change; else it is taken back
    if (res.keep.on && !Kept(p.d, n.d, res.keep)) {
        r.refused = "keep";
        return true;
    }
    r.src = s;
    r.st = res.after;
    r.dir = res.kind == EK_DEL_BACK ? -1 : 1;
    SelfCheck(n, what + " after " + op);
    uint16_t trail = 0;
    TextPos t = TextOfSrc(n.d, n.src, r.st.focus, r.dir, &trail);
    // a phantom lives while the caret is in it or in the block it stands next to (§6.7)
    if (r.st.phantom.kind != PH_NONE && !PhantomAlive(n.d, n.src, r.st, t.block)) r.st.phantom = Phantom{};
    if (InPhantom(r.st)) {
        Check(r.st.focus == r.st.phantom.anchorSrc, "%s %s: the caret %u is not at the phantom's anchor %u", what.c_str(),
              op.c_str(), r.st.focus, r.st.phantom.anchorSrc);
    } else if (r.st.atom < 0 && r.st.anchor == r.st.focus && !n.d.blocks.empty()) {  // (an emptied document has no stop)
        Check(CaretStop(n.d, t), "%s %s: the caret (t%u b%d) is not a stop", what.c_str(), op.c_str(), t.t, t.block);
        if (!trail) {  // normalised as the glue does: to where typed text would go
            uint32_t k = SrcOfText(n.d, n.src, t, MAP_CARET);
            if (k != UINT32_MAX) r.st.focus = r.st.anchor = k;
        }
    }
    return true;
}

// `phantom: none | after <b> | before <b> | break <b> [in] [style <n>] [depth <n>]`
std::string PhantomText(const Doc& d, const std::wstring& src, const Phantom& ph) {
    if (ph.kind == PH_NONE) return "none";
    static const char* kinds[] = {"none", "after", "before", "break"};
    std::string s = Fmt("%s %d", kinds[ph.kind], PhantomBlock(d, src, ph));
    if (ph.in) s += " in";
    if (ph.style) s += Fmt(" style %u", ph.style);
    return s;
}

void OpVariant(const Case& c, const std::string& what, const std::wstring& srcMarked, const std::wstring& wantMarked) {
    OpRun r;
    if (!ParseMarked(srcMarked, &r.src, &r.st.anchor, &r.st.focus)) {
        Fail("%s: src needs one ‸ or one ⟦…⟧", what.c_str());
        return;
    }
    for (const std::string& op : SplitOps(*c.Get("do"))) {
        if (!DoOp(r, op, what) || !r.refused.empty()) break;
    }
    const std::string* state = c.Get("state");
    if (state) {
        for (const std::string& kv : SplitOps(*state)) {
            if (kv.rfind("refused=", 0) == 0) Check(r.refused == kv.substr(8), "%s: refused \"%s\", want \"%s\"", what.c_str(),
                                                   r.refused.c_str(), kv.substr(8).c_str());
            else if (kv.rfind("atom=", 0) == 0) {
                std::string v = kv.substr(5);
                int32_t want = v == "none" ? -1 : v.rfind("blk", 0) == 0 ? (kAtomBlock | atoi(v.c_str() + 3)) : atoi(v.c_str() + 3);
                Check(r.st.atom == want, "%s: atom %d, want %d (%s)", what.c_str(), r.st.atom, want, v.c_str());
            }
        }
    } else {
        Check(r.refused.empty(), "%s: refused \"%s\"", what.c_str(), r.refused.c_str());
    }
    std::wstring got = Marked(r.src, r.st.anchor, r.st.focus);
    Check(got == wantMarked, "%s: the result differs\n  want: %s\n  got:  %s", what.c_str(), Esc(wantMarked, 200).c_str(),
          Esc(got, 200).c_str());
    Parsed fin;
    ParseMap(fin, r.src);
    const std::string* ph = c.Get("phantom");
    std::string gotPh = PhantomText(fin.d, fin.src, r.st.phantom), wantPh = ph ? *ph : "none";
    Check(gotPh == wantPh, "%s: phantom %s, want %s", what.c_str(), gotPh.c_str(), wantPh.c_str());
}
void RunOpCase(const Case& c) {
    const std::string* s = c.Get("src");
    const std::string* w = c.Get("want");
    if (!s || !w) {
        Fail("%s:%d %s: an op case needs src, do and want", c.file.c_str(), c.line, c.name.c_str());
        return;
    }
    std::wstring src = Unescape(*s), want = Unescape(*w);
    for (int tex : TexModes(c, src)) {
        g_texOn = tex != 0;
        std::string what = c.file + ":" + std::to_string(c.line) + " " + c.name + (tex ? " [tex on]" : " [tex off]");
        OpVariant(c, what, src, want);
        if (!HasFlag(c, "nocrlf")) OpVariant(c, what + " [crlf]", ToCrlf(src), ToCrlf(want));
        if (!HasFlag(c, "noquote")) OpVariant(c, what + " [quote]", WrapQuote(src), WrapQuote(want));
        if (!HasFlag(c, "nolist")) OpVariant(c, what + " [list]", WrapList(src), WrapList(want));
    }
}

void RunCaseFile(const std::wstring& path) {
    std::vector<std::string> lines;
    std::vector<Case> cases = ReadCases(path, lines);
    bool changed = false;
    // --update edits the lines from the bottom up, so the line numbers of the cases above stay right
    for (size_t i = cases.size(); i-- > 0;) {
        Case& c = cases[i];
        if (!g_filter.empty() && c.name.find(g_filter) == std::string::npos) continue;
        const std::string* op = c.Get("do");
        if (c.hasMap) RunMapCase(c, lines, changed);
        else if (op && *op == "caret") RunCaretCase(c);
        else if (c.Get("diff")) RunDiffCase(c);
        else if (op) RunOpCase(c);
        else Fail("%s:%d %s: no map, do or diff", c.file.c_str(), c.line, c.name.c_str());
    }
    if (changed) {
        std::string out;
        for (auto& l : lines) out += l + "\n";
        if (!WriteBytes(path, out)) Fail("cannot write %s", Narrow(path).c_str());
    }
    printf("  %s: %zu cases\n", Narrow(path.substr(path.find_last_of(L'\\') + 1)).c_str(), cases.size());
}

// ------------------------------------------------------------------------------------------------ sweeps (§14.1)
struct SweepStats { size_t stops = 0, offsets = 0; };

void SweepDoc(const std::string& name, const std::wstring& text, SweepStats& st) {
    Parsed p;
    ParseMap(p, text);
    std::string why;
    bool ok = MapSelfCheck(p.d, p.src, &why);
    if (!Check(ok, "%s: MapSelfCheck: %s", name.c_str(), why.c_str())) return;
    const uint32_t n = (uint32_t)p.src.size();
    int reported = 0;
    size_t stops = 0, offsets = 0;
    mapsweep::SweepMap(p.d, p.src, n > 400000 ? 61 : 1, n > 400000 ? 53 : 1, &stops, &offsets, [&](const std::string& m) {
        if (reported++ < 8) Fail("%s: %s", name.c_str(), m.c_str());
        else g_failures++;
        return true;
    });
    st.stops += stops;
    st.offsets += offsets;
    g_checks += (int)(stops * 6 + offsets);  // four modes and two round trips per stop, one look per offset
}

void Sweep(const std::vector<std::wstring>& dirs) {
    SweepStats st;
    size_t files = 0;
    for (const std::wstring& dir : dirs) {
        for (const std::wstring& f : ListFiles(dir, L"*.md")) {
            std::string name = Narrow(f.substr(f.find_last_of(L'\\') + 1));
            if (!g_filter.empty() && name.find(g_filter) == std::string::npos) continue;
            std::wstring text = ReadUtf8File(f);
            files++;
            for (int tex : {1, 0}) {
                g_texOn = tex != 0;
                SweepDoc(name + (tex ? " [tex on]" : " [tex off]"), text, st);
            }
        }
    }
    printf("  sweeps: %zu files, %zu caret stops, %zu source offsets\n", files, st.stops, st.offsets);
}

// ------------------------------------------------------------------------------------------------ small unit tests
void UnitTests() {
    struct { const wchar_t* src; uint32_t at; const wchar_t* want; } eol[] = {
        {L"a\r\nb", 0, L"\r\n"}, {L"a\r\nb", 1, L"\r\n"}, {L"a\r\nb", 2, L"\r\n"}, {L"a\r\nb", 3, L"(none)"},
        {L"a\nb", 0, L"\n"},     {L"a\rb", 1, L"\r"},     {L"", 0, L"(none)"},     {L"ab", 2, L"(none)"},
    };
    for (auto& e : eol) {
        const wchar_t* got = LineEol(e.src, e.at, L"(none)");
        Check(wcscmp(got, e.want) == 0, "LineEol(\"%s\", %u) = \"%s\", want \"%s\"", Esc(e.src).c_str(), e.at,
              Esc(got).c_str(), Esc(e.want).c_str());
    }
    // a Doc without a map answers nothing, and says so
    Doc plain;
    std::wstring src = L"# a\n";
    ParseMarkdown(plain, src.data(), src.size());
    std::string why;
    Check(!plain.hasMap && plain.segs.empty() && plain.blockSrc.empty(), "a reading-mode parse builds no map");
    Check(!plain.srcMap.empty(), "a reading-mode parse still builds srcMap");
    Check(!MapSelfCheck(plain, src, &why) && !why.empty(), "MapSelfCheck refuses a Doc without a map");
    Check(SrcOfText(plain, src, TextPos{0, 0, -1}, MAP_CARET) == UINT32_MAX, "SrcOfText without a map");
    Check(TextOfSrc(plain, src, 0, 1, nullptr).block == -1, "TextOfSrc without a map");
    // the map parse leaves srcMap alone (reading mode's copy-as-Markdown uses it; edit mode has the segments)
    Parsed p;
    ParseMap(p, src);
    Check(p.d.srcMap.empty() && p.d.hasMap, "a map parse builds the map and no srcMap");
    // reading mode: srcMap gets an entry after each emoji, so what follows maps exactly (§4.3)
    Doc e;
    std::wstring es = L"Flag :england: ok\n";
    ParseMarkdown(e, es.data(), es.size());
    uint32_t okT = (uint32_t)e.text.find(L"ok");
    bool exact = false;
    for (auto& m : e.srcMap)
        if (m.first <= okT) exact = m.second + (okT - m.first) == es.find(L"ok");
    Check(okT != UINT32_MAX && exact, "srcMap maps the text after an emoji exactly");

    // ---- the chords of edit mode (§2.7, §12.5): Ctrl+Alt is AltGr text, never a chord (T21)
    int altgr = 0;
    for (unsigned vk = 0; vk < 256; vk++)
        for (int sh = 0; sh < 2; sh++) altgr += EditChord(vk, true, sh != 0, true) != 0;
    Check(altgr == 0, "EditChord claims %d Ctrl+Alt chords", altgr);
    struct Ch { unsigned vk; bool ctrl, shift; unsigned want; } chords[] = {
        {'Z', 1, 0, 144}, {'Y', 1, 0, 145}, {'Z', 1, 1, 145}, {'S', 1, 0, 148}, {'B', 1, 0, 150}, {'I', 1, 0, 151},
        {'X', 1, 1, 152}, {VK_OEM_3, 1, 0, 153}, {'K', 1, 0, 154}, {'1', 1, 0, 157}, {'6', 1, 0, 162}, {'7', 1, 1, 164},
        {'8', 1, 1, 163}, {'9', 1, 1, 165}, {'Q', 1, 1, 166}, {'K', 1, 1, 167}, {'T', 1, 0, 169}, {'M', 1, 0, 170},
        {'M', 1, 1, 171}, {VK_RETURN, 1, 0, 175}, {'A', 1, 0, 101}, {'C', 1, 0, 100}, {VK_INSERT, 1, 0, 100},
        {'C', 1, 1, 137}, {'X', 1, 0, 146}, {'V', 1, 0, 147}, {VK_DELETE, 0, 1, 146}, {VK_INSERT, 0, 1, 147},
        {VK_F2, 0, 0, 141}, {VK_F5, 0, 0, 103}, {'E', 1, 0, 104}, {'E', 1, 1, 104}, {'A', 1, 1, 101}, {'R', 1, 0, 103},
        {'A', 0, 0, 0}, {VK_BACK, 0, 0, 0}, {VK_DELETE, 0, 0, 0}, {VK_RETURN, 0, 0, 0}, {'7', 1, 0, 0}, {'Y', 1, 1, 0},
    };
    for (auto& c : chords)
        Check(EditChord(c.vk, c.ctrl, c.shift, false) == c.want, "EditChord(%#x%s%s) = %u, want %u", c.vk, c.ctrl ? " ctrl" : "",
              c.shift ? " shift" : "", EditChord(c.vk, c.ctrl, c.shift, false), c.want);

    // ---- clusters (§6.2): what one Backspace / arrow takes whole
    struct Cl { const wchar_t* text; uint32_t pos; int dir; uint32_t want; } clusters[] = {
        {L"aéx", 1, 1, 3},                          // e + combining acute
        {L"aéx", 3, -1, 1},
        {L"a\U0001F600b", 1, 1, 3},                       // a surrogate pair
        {L"a\U0001F600b", 3, -1, 1},
        {L"\U0001F1F7\U0001F1FAx", 0, 1, 4},              // a flag: two regional indicators
        {L"\U0001F1F7\U0001F1FAx", 4, -1, 0},
        {L"\U0001F468‍\U0001F469‍\U0001F467!", 0, 1, 8},  // a ZWJ family
        {L"\U0001F468‍\U0001F469‍\U0001F467!", 8, -1, 0},
        {L"\U0001F44D\U0001F3FDok", 0, 1, 4},             // a skin tone
        {L"x❤️", 3, -1, 1},                     // a variation selector
    };
    for (auto& c : clusters)
        Check(GraphemeLite(c.text, c.pos, c.dir, nullptr) == c.want, "GraphemeLite(\"%s\", %u, %d) = %u, want %u",
              Esc(c.text).c_str(), c.pos, c.dir, GraphemeLite(c.text, c.pos, c.dir, nullptr), c.want);
}

// ------------------------------------------------------------------------------------------------ undo (§11)
void UndoTests() {
    UndoStack u;
    std::wstring src;
    std::string why;
    auto step = [](EditKind k, uint32_t at, const std::wstring& removed, const std::wstring& inserted, uint32_t focusBefore,
                   uint32_t focusAfter) {
        EditStep s;
        s.kind = k;
        s.splices.push_back(Splice{at, removed, inserted});
        s.before.focus = s.before.anchor = focusBefore;
        s.after.focus = s.after.anchor = focusAfter;
        return s;
    };
    auto type = [&](const std::wstring& text, uint64_t& t, uint64_t dt) {
        for (wchar_t c : text) {
            uint32_t at = (uint32_t)src.size();
            src += c;
            u.Push(step(EK_TYPE, at, L"", std::wstring(1, c), at, at + 1), t += dt);
        }
    };
    auto undo = [&] {
        bool ok = u.PeekUndo() && ApplySplices(src, u.PeekUndo()->splices, true, &why);
        if (ok) u.DidUndo();
        return ok;
    };
    auto redo = [&] {
        bool ok = u.PeekRedo() && ApplySplices(src, u.PeekRedo()->splices, false, &why);
        if (ok) u.DidRedo();
        return ok;
    };
    uint64_t t = 10000;
    type(L"Новый мир", t, 120);  // T19: a word after a blank is a step of its own
    Check(u.Depth() == 2, "undo: «Новый мир» typed in one go is 2 steps, got %zu", u.Depth());
    Check(undo() && src == L"Новый ", "undo: one undo leaves «Новый », got \"%s\"", Esc(src).c_str());
    Check(u.Depth() == 1 && u.RedoDepth() == 1, "undo: depths after one undo %zu/%zu", u.Depth(), u.RedoDepth());
    Check(redo() && src == L"Новый мир", "undo: redo gives the text back");
    Check(undo() && undo() && src.empty() && !undo(), "undo: two undos empty the text, a third does nothing");
    Check(redo() && redo() && src == L"Новый мир" && !redo(), "undo: two redos give it all back");

    u.Clear();
    src.clear();
    type(L"ab", t, 1499);  // < 1.5 s apart: one step
    Check(u.Depth() == 1, "undo: typing 1499 ms apart coalesces");
    type(L"c", t, 1500);   // 1.5 s: a new one
    Check(u.Depth() == 2, "undo: a pause of 1.5 s starts a new step, depth %zu", u.Depth());
    u.BreakCoalescing();   // a click, a command, a caret move in between
    type(L"d", t, 10);
    Check(u.Depth() == 3, "undo: BreakCoalescing starts a new step");
    uint32_t at = (uint32_t)src.size();  // the caret went elsewhere: typing there is a new step too
    src.insert(0, L"x");
    u.Push(step(EK_TYPE, 0, L"", L"x", 0, 1), t += 10);
    Check(u.Depth() == 4 && at == 4, "undo: typing where the last step did not leave the caret starts a new step");
    u.Push(step(EK_STRUCT, 1, L"", L"\n", 1, 2), t += 10);
    src.insert(1, L"\n");
    u.Push(step(EK_STRUCT, 2, L"", L"\n", 2, 3), t += 10);
    src.insert(2, L"\n");
    Check(u.Depth() == 6, "undo: structural steps never coalesce");
    Check(undo() && undo() && src == L"xabcd", "undo: structural steps undone one by one, got \"%s\"", Esc(src).c_str());
    Check(u.RedoDepth() == 2, "undo: two steps to redo");
    type(L"e", t, 10);
    Check(u.RedoDepth() == 0, "undo: a new step clears the redo stack");

    // Backspace and Delete coalesce into one splice each; undo puts the characters back where they were
    u.Clear();
    src = L"0123456789";
    for (uint32_t k = 0; k < 3; k++) {  // Backspace at 6, 5, 4
        uint32_t p = 6 - k - 1;
        std::wstring r = src.substr(p, 1);
        src.erase(p, 1);
        u.Push(step(EK_DEL_BACK, p, r, L"", p + 1, p), t += 50);
    }
    Check(u.Depth() == 1 && u.PeekUndo()->splices.size() == 1 && u.PeekUndo()->splices[0].removed == L"345",
          "undo: three Backspaces are one step of one splice");
    for (int k = 0; k < 2; k++) {  // Delete at 3, twice (a different kind: its own step)
        std::wstring r = src.substr(3, 1);
        src.erase(3, 1);
        u.Push(step(EK_DEL_FWD, 3, r, L"", 3, 3), t += 50);
    }
    Check(u.Depth() == 2 && u.PeekUndo()->splices.size() == 1 && u.PeekUndo()->splices[0].removed == L"67",
          "undo: two Deletes are one step of one splice");
    Check(undo() && src == L"0126789" && undo() && src == L"0123456789", "undo: deletions undone, got \"%s\"",
          Esc(src).c_str());

    // the history is bounded: the oldest steps go
    u.Clear();
    src.clear();
    for (int k = 0; k < 1005; k++) {
        u.BreakCoalescing();
        u.Push(step(EK_TYPE, (uint32_t)src.size(), L"", L"a", (uint32_t)src.size(), (uint32_t)src.size() + 1), t += 10);
        src += L'a';
    }
    Check(u.Depth() == UndoStack::kMaxSteps, "undo: at most %zu steps, got %zu", UndoStack::kMaxSteps, u.Depth());

    // ApplySplices verifies before it changes anything: all or nothing
    std::wstring s = L"hello world";
    std::vector<Splice> sp = {{0, L"hello", L"HELLO"}, {6, L"world", L"there"}};
    Check(ApplySplices(s, sp, false, &why) && s == L"HELLO there", "ApplySplices forward");
    Check(ApplySplices(s, sp, true, &why) && s == L"hello world", "ApplySplices inverse restores the text");
    std::vector<Splice> bad = {{0, L"hello", L"HELLO"}, {6, L"earth", L"there"}};
    why.clear();
    Check(!ApplySplices(s, bad, false, &why) && s == L"hello world" && !why.empty(),
          "ApplySplices refuses a splice whose text is not there and leaves the text as it was");
    // a splice may not cut a surrogate pair or a CRLF in two (§7.1)
    std::wstring e = L"a\r\nb\U0001F600c";
    Check(SpliceSplits(e, 2, 0) && SpliceSplits(e, 1, 1) && !SpliceSplits(e, 1, 2) && SpliceSplits(e, 5, 0) &&
              !SpliceSplits(e, 4, 2) && !SpliceSplits(e, 0, 0),
          "SpliceSplits: CRLF and surrogate pair boundaries");
}

// ------------------------------------------------------------------------------------------------ editfile (§10)
std::string EncodeAs(UINT cp, const std::wstring& t) {
    if (cp == 1200) return std::string((const char*)t.data(), t.size() * 2);
    int n = WideCharToMultiByte(cp, 0, t.data(), (int)t.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(cp, 0, t.data(), (int)t.size(), s.data(), n, nullptr, nullptr);
    return s;
}

struct FileFixture {
    std::wstring dir, rec, path;
    FileFixture() {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        dir = std::wstring(tmp) + L"fastmd-edit-tests-" + std::to_wstring(GetCurrentProcessId()) + L"\\";
        rec = dir + L"recovery\\";
        path = dir + L"doc.md";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    ~FileFixture() {
        for (const std::wstring& f : ListFiles(rec.substr(0, rec.size() - 1), L"*")) DeleteFileW(f.c_str());
        RemoveDirectoryW(rec.c_str());
        DeleteFileW(path.c_str());
        RemoveDirectoryW(dir.c_str());
    }
    size_t RecoveryFiles() { return ListFiles(rec.substr(0, rec.size() - 1), L"*.rec").size(); }
    // the file holds `bytes`; the baseline from it, read and checked as edit mode takes it
    DiskRefusal Baseline(const std::string& bytes, UINT acp, DiskState& d) {
        WriteBytes(path, bytes);
        std::string b;
        DWORD e = 0;
        d = DiskState();
        if (ReadDisk(path.c_str(), b, &d, &e) != SS_SAVED) return DR_BINARY;
        DiskRefusal r = DecodeDisk(b, acp, d);
        d.valid = r == DR_OK;
        return r;
    }
    SaveResult Save(const std::wstring& text, const DiskState& d, bool flushPoint = false,
                    const RecoveryInfo* pending = nullptr, bool fullProof = true) {
        SaveRequest rq;
        rq.path = path.c_str();
        rq.text = &text;
        rq.disk = &d;
        rq.recoveryDir = rec;
        rq.flushPoint = flushPoint;
        rq.fullProof = fullProof;
        rq.pending = pending;
        return SaveSource(rq);
    }
    std::vector<std::wstring> RecoveryList() { return ListFiles(rec.substr(0, rec.size() - 1), L"*.rec"); }
    std::string Bytes() {
        std::string b;
        ReadBytes(path, b);
        return b;
    }
};

const char* StateName(SaveState s) {
    static const char* n[] = {"SAVED", "PENDING", "SAVING", "BUSY", "DENIED", "READONLY", "MISSING", "CONFLICT",
                              "UNENCODABLE", "FAILED", "UNKNOWN", "OFF"};
    return s < std::size(n) ? n[s] : "?";
}

uint64_t FileTimeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
    return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}
void SetFileTimeOf(const std::wstring& path, uint64_t t) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

// Recovery files (§10.3 steps 4 and 9, §10.5, and the review of Phase 1): what they hold, when they go, and that a
// leftover is put back only over the torn file it was made for.
void RecoveryTests(FileFixture& fx) {
    DiskState d;
    const std::wstring v0 = L"# Title\n\nline one of the original\nline two of the original\nline three\n\n- [ ] task\n";
    const std::string orig = EncodeAs(CP_UTF8, v0);
    const size_t box = v0.find(L"[ ]") + 1;
    std::wstring ticked = v0;
    ticked[box] = L'x';
    for (const std::wstring& f : fx.RecoveryList()) DeleteFileW(f.c_str());

    // A save that keeps the length holds only the bytes it replaces; one that never wrote a byte leaves a file that
    // is seen as untouched (and would be deleted without a word).
    fx.Baseline(orig, 1252, d);
    SetFailWriteForTests(L"partial:0,norollback");
    SaveResult r = fx.Save(ticked, d);
    SetFailWriteForTests(L"");
    RecoveryInfo ri;
    bool read = !r.recoveryKept.empty() && ReadRecovery(r.recoveryKept, ri);
    Check(read && ri.pb == box && ri.pe == box + 1 && ri.tailLen == 1 && ri.preSize == orig.size(),
          "recovery: a tick keeps one byte, [%llu,%llu) of %llu", ri.pb, ri.pe, ri.preSize);
    Check(read && ClassifyRecovery(ri, fx.Bytes(), FileTimeOf(fx.path)) == RV_UNTOUCHED,
          "recovery: a save that wrote nothing leaves an untouched file");
    DeleteFileW(r.recoveryKept.c_str());

    // Unflushed saves (a slow volume between flush points): one recovery file stands for the last flushed version. A
    // save inside its range adds nothing, one outside widens it; a crash leaves a file that shows the save went
    // through; a torn save among them restores the flushed version; the flush point removes it.
    SetFlushPolicyForTests(1);
    fx.Baseline(orig, 1252, d);
    r = fx.Save(ticked, d);
    RecoveryInfo pend = r.pending;
    Check(r.state == SS_SAVED && !pend.file.empty() && fx.RecoveryList().size() == 1 && r.flushMs < 0,
          "pending: an unflushed tick keeps its recovery file (%s, %zu files)", StateName(r.state), fx.RecoveryList().size());
    d = r.disk;
    r = fx.Save(v0, d, false, &pend);  // untick: the same byte
    Check(r.state == SS_SAVED && r.pending.file == pend.file && fx.RecoveryList().size() == 1,
          "pending: a save inside its range reuses it");
    pend = r.pending;
    d = r.disk;
    std::wstring grown = L"# A longer title\n" + v0.substr(v0.find(L'\n') + 1);
    r = fx.Save(grown, d, false, &pend);  // before its range, and longer: a wider one, to the end
    Check(r.state == SS_SAVED && !r.pending.file.empty() && r.pending.file != pend.file && fx.RecoveryList().size() == 1 &&
              r.pending.pb == 2 && r.pending.pe == r.pending.preSize && r.pending.preSize == orig.size(),
          "pending: a save outside its range widens it (pb %llu, pe %llu, %zu files)", r.pending.pb, r.pending.pe,
          fx.RecoveryList().size());
    pend = r.pending;
    d = r.disk;
    std::wstring copy = fx.dir + L"copy.md";
    std::string cb;
    Check(RecoveryRebuild(pend, fx.path.c_str(), copy.c_str()) && ReadBytes(copy, cb) && cb == orig,
          "pending: it rebuilds the flushed version");
    DeleteFileW(copy.c_str());
    std::vector<RecoveryInfo> found = FindRecovery(fx.rec, d.volume, d.index, fx.path);
    Check(found.size() == 1 && ClassifyRecovery(found[0], fx.Bytes(), FileTimeOf(fx.path)) == RV_DONE,
          "pending: after a crash it shows the last save went through");
    SetFailWriteForTests(L"partial:3,norollback");
    r = fx.Save(grown + L"and more at the end\n", d, false, &pend);
    SetFailWriteForTests(L"");
    found = FindRecovery(fx.rec, d.volume, d.index, fx.path);
    DWORD e = 0;
    Check(r.state == SS_FAILED && r.recoveryKept == pend.file && r.pending.file.empty() && found.size() == 1 &&
              ClassifyRecovery(found[0], fx.Bytes(), FileTimeOf(fx.path)) == RV_TORN &&
              RecoveryRestore(found[0], fx.path.c_str(), fx.rec, &e) && fx.Bytes() == orig,
          "pending: a torn save among unflushed ones restores the last flushed version");
    for (const std::wstring& f : fx.RecoveryList()) DeleteFileW(f.c_str());
    fx.Baseline(orig, 1252, d);
    r = fx.Save(ticked, d);
    pend = r.pending;
    Check(RecoveryFlushPending(pend, fx.path.c_str()) && fx.RecoveryList().empty(), "pending: the flush point removes it");
    SetFlushPolicyForTests(0);

    // A leftover is put back only over the torn file it was made for (the reviewer's harness case): written again by
    // another program since, or only touched later than the save, it is "changed" - Restore refuses, the file stays.
    fx.Baseline(orig, 1252, d);
    std::wstring shrunk = v0;
    shrunk.erase(v0.find(L"line one"), 9);
    SetFailWriteForTests(L"partial:4,norollback");
    r = fx.Save(shrunk, d);
    SetFailWriteForTests(L"");
    std::string torn = fx.Bytes();
    const std::string other = "INSERTED BY ANOTHER EDITOR\n" + orig + "new closing line\n";
    WriteBytes(fx.path, other);
    found = FindRecovery(fx.rec, d.volume, d.index, fx.path);
    Check(found.size() == 1 && ClassifyRecovery(found[0], other, FileTimeOf(fx.path)) == RV_CHANGED &&
              !RecoveryRestore(found[0], fx.path.c_str(), fx.rec, &e) && e == ERROR_INVALID_DATA && fx.Bytes() == other,
          "restore: a file rewritten since is not overwritten (%lu)", e);
    WriteBytes(fx.path, torn);
    SetFileTimeOf(fx.path, FileTimeOf(fx.path) + 600000000ull);  // a minute later than the recovery file
    Check(found.size() == 1 && ClassifyRecovery(found[0], torn, FileTimeOf(fx.path)) == RV_CHANGED &&
              !RecoveryRestore(found[0], fx.path.c_str(), fx.rec, &e) && fx.Bytes() == torn,
          "restore: a torn file written after the recovery file is not overwritten either");
    // another file under the same identity (FAT reuses the index of a deleted file): not its recovery file at all
    Check(FindRecovery(fx.rec, d.volume, d.index, fx.dir + L"todo.md").empty(), "recovery: another path, not listed");

    // A kept recovery file is never written over: the next save from the torn file gets one of its own.
    fx.Baseline(torn, 1252, d);
    size_t before = fx.RecoveryList().size();
    r = fx.Save(d.text + L"x", d);
    RecoveryInfo still;
    Check(r.state == SS_SAVED && fx.RecoveryList().size() == before && found.size() == 1 && ReadRecovery(found[0].file, still) &&
              still.tailHash == found[0].tailHash,
          "recovery: a later save leaves the kept file as it was (%s)", StateName(r.state));
    for (const std::wstring& f : fx.RecoveryList()) DeleteFileW(f.c_str());

    // Above a million characters the proof decodes only the changed window: an ANSI file must still not turn into
    // valid UTF-8 ("Ã©" plus one lone "é": deleting the "é" would make the next read take it for UTF-8).
    SetAnsiCodePageForTests(1252);
    std::wstring big = L"Ã© and é\n" + std::wstring(1100000, L'a') + L"\n";
    std::string bigBytes = EncodeAs(1252, big);
    Check(fx.Baseline(bigBytes, 1252, d) == DR_OK && d.cp == 1252, "ansi: the big 1252 file is taken, cp %u", d.cp);
    std::wstring noE = big;
    noE.erase(noE.find(L'é', 3), 1);
    r = fx.Save(noE, d, false, nullptr, false);
    Check(r.state == SS_UNENCODABLE && fx.Bytes() == bigBytes, "ansi: > 1M characters, becoming UTF-8 is refused (%s %s)",
          StateName(r.state), r.reason);
    std::wstring plainEdit = big;
    plainEdit.insert(20, L"b");
    r = fx.Save(plainEdit, d, false, nullptr, false);
    Check(r.state == SS_SAVED && fx.Bytes() == EncodeAs(1252, plainEdit), "ansi: an ASCII edit is saved (%s)", StateName(r.state));
    // an ANSI file that starts with a UTF-8 byte-order mark must not get FF FE after it (the read would see UTF-16)
    std::string bomAnsi = "\xEF\xBB\xBF" "caf\xE9\n";
    Check(fx.Baseline(bomAnsi, 1252, d) == DR_OK && d.cp == 1252 && d.header == "\xEF\xBB\xBF", "ansi: BOM + 1252 is taken");
    r = fx.Save(L"\xFF\xFE" + d.text, d);
    Check(r.state == SS_UNENCODABLE && !strcmp(r.reason, "BOM_LOOKALIKE") && fx.Bytes() == bomAnsi,
          "ansi: \"ÿþ\" after its UTF-8 mark → BOM_LOOKALIKE (%s %s)", StateName(r.state), r.reason);
    SetAnsiCodePageForTests(GetACP());
}

void EditFileTests() {
    FileFixture fx;
    DiskState d;
    const std::wstring ru = L"# Привет\n\nТекст и ещё строка.\n";
    // ---- the byte-exact entry check (§10.1, D1, D14)
    Check(fx.Baseline(EncodeAs(1251, ru), 65001, d) == DR_LOSSY, "entry: a CP1251 file under FASTMD_ACP=65001 is refused");
    Check(fx.Baseline(EncodeAs(1251, ru), 1251, d) == DR_OK && d.cp == 1251 && d.text == ru,
          "entry: the same file under 1251 is taken, cp %u", d.cp);
    Check(fx.Baseline(std::string("abc\x81\n", 5), 932, d) == DR_LOSSY, "entry: a stray byte under 932 is refused");
    Check(fx.Baseline(std::string("a\0b\n", 4), 1252, d) == DR_BINARY, "entry: NUL bytes outside UTF-16 are refused");
    Check(fx.Baseline(std::string("\xFE\xFF\0a", 4), 1252, d) == DR_UTF16BE, "entry: UTF-16 BE is refused");
    Check(fx.Baseline(std::string("\xFF\xFE" "a\0b", 5), 1252, d) == DR_UTF16ODD, "entry: UTF-16 LE of odd length is refused");
    Check(fx.Baseline(std::string("\xA4\xA2\xA4\xA4\n", 5), 50220, d) == DR_STATEFUL, "entry: a stateful code page is refused");
    Check(fx.Baseline(std::string("\xEF\xBB\xBF" "a\r\nb\r\nc\n", 11), 1252, d) == DR_OK && d.header == "\xEF\xBB\xBF" &&
              d.cp == CP_UTF8 && d.crlf == 2 && d.lf == 1 && !wcscmp(DiskEol(d), L"\r\n"),
          "entry: UTF-8 with BOM, line ends counted (2 CRLF, 1 LF → CRLF)");
    Check(fx.Baseline("plain ascii\n", 1251, d) == DR_OK && d.cp == CP_UTF8, "entry: pure ASCII reads as UTF-8");

    // ---- splice-local output == the full encode, in every encoding (§10.3 step 3)
    struct Enc { const char* name; UINT cp; std::string header; const wchar_t* eol; };
    const Enc encs[] = {{"utf8", CP_UTF8, "", L"\n"},
                        {"utf8-bom-crlf", CP_UTF8, "\xEF\xBB\xBF", L"\r\n"},
                        {"utf16", 1200, "\xFF\xFE", L"\r\n"},
                        {"cp1251", 1251, "", L"\n"}};
    const std::wstring base = L"# Заголовок\n\nПервый абзац с текстом.\n\n- [ ] задача\n- [x] вторая\n\nПоследняя строка.\n";
    struct Edit { const wchar_t* what; size_t at, len; const wchar_t* text; };
    const Edit edits[] = {{L"tick", 0, 1, L"x"},       {L"insert", 20, 0, L"новое "}, {L"delete", 14, 6, L""},
                          {L"start", 0, 1, L"##"},    {L"end", SIZE_MAX, 0, L"Хвост."}, {L"grow", 30, 0, L"очень длинная вставка "},
                          {L"shrink", 5, 40, L"-"},   {L"line end", 11, 0, L"\n\nновая строка"}};
    for (const Enc& en : encs) {
        std::wstring text0 = base;
        if (wcscmp(en.eol, L"\n")) {  // CRLF files
            std::wstring t;
            for (wchar_t c : text0) { if (c == L'\n') t += L'\r'; t += c; }
            text0 = t;
        }
        SetAnsiCodePageForTests(en.cp == 1251 ? 1251 : GetACP());
        std::wstring cur = text0;
        if (!Check(fx.Baseline(en.header + EncodeAs(en.cp, cur), en.cp == 1251 ? 1251 : GetACP(), d) == DR_OK,
                   "save %s: baseline", en.name))
            continue;
        for (const Edit& ed : edits) {
            std::wstring next = cur;
            size_t at = ed.at == SIZE_MAX ? next.size() : std::min(ed.at, next.size());
            if (!wcscmp(ed.what, L"tick")) at = next.find(L"[ ]") + 1;
            while (at > 0 && at < next.size() && next[at - 1] == L'\r' && next[at] == L'\n') at++;
            size_t len = std::min(ed.len, next.size() - at);
            if (at + len < next.size() && at + len > 0 && next[at + len - 1] == L'\r' && next[at + len] == L'\n') len++;
            next.replace(at, len, ed.text);
            SaveResult r = fx.Save(next, d);
            std::string want = en.header + EncodeAs(en.cp, next), got = fx.Bytes();
            if (!Check(r.state == SS_SAVED && got == want, "save %s, %s: state %s (%s), %zu bytes vs the full encode's %zu",
                       en.name, Narrow(ed.what).c_str(), StateName(r.state), r.reason, got.size(), want.size()))
                break;
            Check(r.disk.valid && r.disk.text == next && r.disk.length == got.size() &&
                      r.disk.hash == Fnv64(got.data(), got.size()) && r.disk.cp == en.cp && r.disk.header == en.header,
                  "save %s, %s: the new baseline is what the file holds", en.name, Narrow(ed.what).c_str());
            Check(fx.RecoveryFiles() == 0, "save %s, %s: no recovery file left after a save", en.name, Narrow(ed.what).c_str());
            d = r.disk;
            cur = next;
        }
    }
    SetAnsiCodePageForTests(GetACP());

    // ---- what cannot be saved: nothing is written, the reason says why (§10.3, D12)
    std::string orig = EncodeAs(CP_UTF8, ru);
    fx.Baseline(orig, 1252, d);
    std::wstring lone = ru;
    lone.insert(3, 1, (wchar_t)0xD83D);
    SaveResult r = fx.Save(lone, d);
    Check(r.state == SS_UNENCODABLE && !strcmp(r.reason, "LONE_SURROGATE") && r.bad == 3 && fx.Bytes() == orig,
          "save: a lone surrogate in UTF-8 → LONE_SURROGATE at 3, file untouched (%s %s %u)", StateName(r.state), r.reason, r.bad);
    r = fx.Save(L"\xFEFF" + ru, d);
    Check(r.state == SS_UNENCODABLE && !strcmp(r.reason, "BOM_LOOKALIKE") && fx.Bytes() == orig,
          "save: U+FEFF at the start of a UTF-8 file without a BOM → BOM_LOOKALIKE (%s %s)", StateName(r.state), r.reason);
    SetAnsiCodePageForTests(1251);
    std::string orig1251 = EncodeAs(1251, ru);
    fx.Baseline(orig1251, 1251, d);
    r = fx.Save(L"яю" + ru, d);  // FF FE: the next read would take it for UTF-16
    Check(r.state == SS_UNENCODABLE && !strcmp(r.reason, "BOM_LOOKALIKE") && fx.Bytes() == orig1251,
          "save: 1251 bytes starting FF FE → BOM_LOOKALIKE (%s %s)", StateName(r.state), r.reason);
    std::wstring check = ru;
    check.insert(5, L"\x2713");  // ✓ has no byte in 1251
    r = fx.Save(check, d);
    Check(r.state == SS_UNENCODABLE && !strcmp(r.reason, "UNENCODABLE") && r.bad == 5 && fx.Bytes() == orig1251,
          "save: ✓ in a 1251 file → UNENCODABLE at 5 (%s %s %u)", StateName(r.state), r.reason, r.bad);
    SetAnsiCodePageForTests(GetACP());

    // ---- the file moved on: other text is a conflict; the same text in other bytes is adopted (D13)
    fx.Baseline(orig, 1252, d);
    WriteBytes(fx.path, orig + "снаружи\n");
    std::wstring mine = ru + L"моё\n";
    r = fx.Save(mine, d);
    Check(r.state == SS_CONFLICT && fx.Bytes() == orig + "снаружи\n", "save: a file changed outside → CONFLICT, untouched");
    WriteBytes(fx.path, "\xEF\xBB\xBF" + orig);  // re-encoded outside: a BOM now, the same text
    r = fx.Save(mine, d);
    Check(r.state == SS_SAVED && r.adopted && fx.Bytes() == "\xEF\xBB\xBF" + EncodeAs(CP_UTF8, mine) && r.disk.header == "\xEF\xBB\xBF",
          "save: the same text re-encoded outside is adopted (its BOM kept) and saved (%s)", StateName(r.state));

    // ---- every fault leaves the original bytes, and nothing behind (§13.5, D24)
    const wchar_t* faults[] = {L"partial:3", L"busy", L"denied", L"missing", L"short_read", L"flush", L"close",
                               L"recovery", L"diskfull"};
    const SaveState expect[] = {SS_FAILED, SS_BUSY, SS_DENIED, SS_MISSING, SS_UNKNOWN, SS_FAILED, SS_FAILED, SS_FAILED,
                                SS_FAILED};
    std::wstring grown = ru + L"Дописано в конце, файл растёт.\n";
    for (size_t k = 0; k < std::size(faults); k++) {
        fx.Baseline(orig, 1252, d);
        SetFailWriteForTests(faults[k]);
        r = fx.Save(grown, d);
        Check(r.state == expect[k] && fx.Bytes() == orig && fx.RecoveryFiles() == 0 && r.recoveryKept.empty(),
              "fault %s: %s (want %s), original bytes %s, recovery files %zu", Narrow(faults[k]).c_str(),
              StateName(r.state), StateName(expect[k]), fx.Bytes() == orig ? "kept" : "CHANGED", fx.RecoveryFiles());
        r = fx.Save(grown, d);  // once by default: the next save goes through
        Check(r.state == SS_SAVED && fx.Bytes() == EncodeAs(CP_UTF8, grown), "fault %s: the next save succeeds (%s)",
              Narrow(faults[k]).c_str(), StateName(r.state));
    }
    // ",norollback": the torn file stays and so does the recovery file, which restores the original
    fx.Baseline(orig, 1252, d);
    SetFailWriteForTests(L"partial:10,norollback");
    r = fx.Save(grown, d);
    std::string torn = fx.Bytes();
    Check(r.state == SS_FAILED && torn != orig && !r.recoveryKept.empty() && fx.RecoveryFiles() == 1,
          "fault partial:10,norollback: a torn file and its recovery file (%s)", StateName(r.state));
    std::vector<RecoveryInfo> found = FindRecovery(fx.rec, d.volume, d.index, fx.path);
    Check(found.size() == 1 && found[0].path == fx.path && found[0].preSize == orig.size() && found[0].pid == GetCurrentProcessId(),
          "recovery: found for the file's identity, %zu", found.size());
    if (found.size() == 1) {
        Check(ClassifyRecovery(found[0], torn, FileTimeOf(fx.path)) == RV_TORN, "recovery: the torn file is seen as torn");
        std::wstring copy = fx.dir + L"copy.md";
        Check(RecoveryRebuild(found[0], fx.path.c_str(), copy.c_str()), "recovery: the copy is rebuilt");
        std::string cb;
        ReadBytes(copy, cb);
        Check(cb == orig, "recovery: the rebuilt copy is the file before the save");
        DeleteFileW(copy.c_str());
        DWORD e = 0;
        Check(RecoveryRestore(found[0], fx.path.c_str(), fx.rec, &e) && fx.Bytes() == orig, "recovery: restore gives the original bytes");
        Check(fx.RecoveryFiles() == 1, "recovery: the restore's own recovery file is gone after it, %zu left", fx.RecoveryFiles());
        DeleteFileW(found[0].file.c_str());
    }
    SetFailWriteForTests(L"");
    RecoveryTests(fx);

    // ---- the local flush cost (§10.3 step 7): every save at a flush point flushes; the median goes into the report
    fx.Baseline(orig, 1252, d);
    std::vector<double> ms;
    std::wstring t = ru;
    for (int k = 0; k < 9; k++) {
        t += L"строка\n";
        r = fx.Save(t, d, true);
        if (r.state != SS_SAVED) break;
        d = r.disk;
        if (r.flushMs >= 0) ms.push_back(r.flushMs);
    }
    std::sort(ms.begin(), ms.end());
    Check(ms.size() == 9, "flush: every save at a flush point flushes (%zu)", ms.size());
    if (!ms.empty()) printf("  local flush (temp folder): median %.2f ms, max %.2f ms over %zu saves\n", ms[ms.size() / 2], ms.back(), ms.size());
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring app = Wide(FASTMD_APP_DIR);
    for (wchar_t& c : app)
        if (c == L'/') c = L'\\';
    std::wstring cases = app + L"\\tests\\edit\\cases";
    std::vector<std::wstring> corpus = {app + L"\\..\\bench\\corpus", app + L"\\tests"};
    bool sweep = true, corpusGiven = false;
    std::wstring dump;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--dump" && i + 1 < argc) dump = argv[++i];  // print the map of one file (.u16: raw UTF-16) and stop
        else if (a == L"--tex" && i + 1 < argc) g_texOn = wcstol(argv[++i], nullptr, 10) != 0;
        else if (a == L"--update") g_update = true;
        else if (a == L"--no-sweep") sweep = false;
        else if (a == L"--filter" && i + 1 < argc) g_filter = Narrow(argv[++i]);
        else if (a == L"--cases" && i + 1 < argc) cases = argv[++i];
        else if (a == L"--corpus" && i + 1 < argc) {
            if (!corpusGiven) corpus.clear();
            corpusGiven = true;
            corpus.push_back(argv[++i]);
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    if (!dump.empty()) {  // a failing fuzz input, say: fastmd-edit-tests --dump out\fuzz\map-1-tex1.u16 --tex 1
        std::wstring text;
        if (dump.size() > 4 && _wcsicmp(dump.c_str() + dump.size() - 4, L".u16") == 0) {
            std::string raw;
            ReadBytes(dump, raw);
            text.assign((const wchar_t*)raw.data(), raw.size() / sizeof(wchar_t));
        } else {
            text = ReadUtf8File(dump);
        }
        printf("source: \"%s\"\n", Esc(text, 4000).c_str());
        Parsed p;
        ParseMap(p, text);
        for (const std::string& l : DumpMap(p.d, p.src, [](uint32_t o) { return o; }, true)) printf("%s\n", l.c_str());
        std::string why;
        bool ok = MapSelfCheck(p.d, p.src, &why);
        printf("MapSelfCheck: %s\n", ok ? "ok" : why.c_str());
        return ok ? 0 : 1;
    }
    DWORD t0 = GetTickCount();
    printf("fastmd-edit-tests\n");
    UnitTests();
    UndoTests();
    EditFileTests();
    for (const std::wstring& f : ListFiles(cases, L"*.txt")) RunCaseFile(f);
    if (sweep) Sweep(corpus);
    printf("%s: %d checks, %d failures, %.2f s\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures,
           (GetTickCount() - t0) / 1000.0);
    return g_failures;
}
