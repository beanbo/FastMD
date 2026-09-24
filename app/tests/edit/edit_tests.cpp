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
//   want:  the expected source with its ‸
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
// directions; TextOfSrc of any source offset is a caret stop.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "doc.h"
#include "editcore.h"
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
    const Doc& d = p.d;
    const uint32_t n = (uint32_t)p.src.size();
    const uint32_t stride = n > 400000 ? 61 : 1;
    int reported = 0;
    auto report = [&](const char* fmt, auto... args) {
        if (reported++ < 8) Fail(fmt, args...);
        else g_failures++;
    };
    uint32_t tick = 0;
    for (uint32_t k = 0; k < d.blocks.size(); k++) {
        const Block& b = d.blocks[k];
        const BlockSrc& bs = d.blockSrc[k];
        if (bs.flags & BS_SYNTH) continue;
        std::vector<std::pair<int32_t, std::pair<uint32_t, uint32_t>>> ranges;  // cell, [beg, end]
        if (bs.flags & (BS_OBJECT | BS_RAW)) {
            ranges.push_back({-1, {b.textOff, b.textOff}});
        } else if (b.kind == BK_TABLE && b.aux < d.tables.size()) {
            const Table& tb = d.tables[b.aux];
            for (uint32_t c = 0; c < tb.rows * tb.cols; c++) {
                const Cell& cell = d.cells[tb.cellOff + c];
                ranges.push_back({(int32_t)c, {cell.textOff, cell.textOff + cell.textLen}});
            }
        } else {
            ranges.push_back({-1, {b.textOff, b.textOff + b.textLen}});
        }
        for (auto& r : ranges) {
            for (uint32_t t = r.second.first; t <= r.second.second; t++) {
                if (stride > 1 && (tick++ % stride) != 0) continue;
                TextPos pos{t, (int32_t)k, r.first};
                if (!CaretStop(d, pos)) continue;
                st.stops++;
                g_checks += 6;  // four modes, two round trips
                for (MapMode m : {MAP_CARET, MAP_OUTER_START, MAP_OUTER_END, MAP_INNER_START}) {
                    uint32_t s = SrcOfText(d, p.src, pos, m);
                    if (s == UINT32_MAX || s > n || s < bs.line || s > bs.outerEnd)
                        report("%s: b%u t%u c%d mode %d: SrcOfText = %d outside the block's lines [%u,%u]", name.c_str(), k,
                               t, r.first, (int)m, (int)s, bs.line, bs.outerEnd);
                }
                uint32_t s = SrcOfText(d, p.src, pos, MAP_CARET);
                for (int dir : {-1, 1}) {
                    TextPos back = TextOfSrc(d, p.src, s, dir, nullptr);
                    int32_t wantBlock = (bs.flags & BS_RAW) && bs.rawId >= 0 ? bs.rawId : (int32_t)k;
                    bool same = back.t == (bs.flags & BS_RAW ? d.blocks[wantBlock].textOff : t) && back.block == wantBlock;
                    if (same && back.cell != r.first) {
                        // a cell a short row lacks shares its place with the end of the cell before it
                        const Table& tb = d.tables[b.aux];
                        bool missing = r.first >= 0 && d.cellSrc[tb.cellOff + r.first].missing;
                        same = missing;
                    }
                    if (!same)
                        report("%s: b%u t%u c%d -> s%u -> t%u b%d c%d (dir %d): the round trip does not come back", name.c_str(),
                               k, t, r.first, s, back.t, back.block, back.cell, dir);
                }
            }
        }
    }
    // any source offset maps to a caret stop (or a folded block)
    const uint32_t sStride = n > 400000 ? 53 : 1;
    for (uint32_t s = 0; s <= n; s += sStride) {
        for (int dir : {-1, 1}) {
            TextPos t = TextOfSrc(d, p.src, s, dir, nullptr);
            st.offsets++;
            g_checks++;
            if (t.block < 0) {
                if (!d.blockOrder.empty()) report("%s: TextOfSrc(%u, %d) found no block", name.c_str(), s, dir);
                continue;
            }
            const Block& b = d.blocks[t.block];
            bool hidden = b.details && !(b.details & 0x8000) && (uint32_t)(b.details & 0x7FFF) - 1 < d.detailsOpen.size() &&
                          !d.detailsOpen[(b.details & 0x7FFF) - 1];
            if (!hidden && !CaretStop(d, t))
                report("%s: TextOfSrc(%u, %d) = t%u b%d c%d is not a caret stop", name.c_str(), s, dir, t.t, t.block, t.cell);
        }
    }
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
    for (const std::wstring& f : ListFiles(cases, L"*.txt")) RunCaseFile(f);
    if (sweep) Sweep(corpus);
    printf("%s: %d checks, %d failures, %.2f s\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures,
           (GetTickCount() - t0) / 1000.0);
    return g_failures;
}
