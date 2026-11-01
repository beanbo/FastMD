// Micro-benchmarks of the window-free half of the program: the work that stands between the bytes of a file and the
// document model the layout draws (bench/PROTOCOL.md measures the whole launch; this measures the parts of it).
//
//   pwsh -File app/tests/bench/run.ps1                     # build (Release) and run everything
//   fastmd-bench.exe --corpus ..\..\bench\corpus           # or drive it directly
//   fastmd-bench.exe --filter deep --min-time 0.5          # only the cases whose name holds "deep"
//   fastmd-bench.exe --scaling                             # also the growth check (below)
//   fastmd-bench.exe --doc some.md                         # one document of your own instead (a slow fuzz input)
//
// Every case is a document: one of the corpus files (small / medium / large), or a generated one that stresses one
// thing - deep nesting, one very long line, thousands of links, tables, headings, footnotes, code blocks, HTML. For
// each document four operations are timed:
//   decode   bytes → UTF-16 (DecodeText: BOM sniffing, UTF-8 validation, the code page fallback)
//   parse    the reading parse (ParseMarkdown: md4c, inline HTML, highlighting, slugs, emoji)
//   map      the edit-mode parse (ParseMarkdown with the text <-> source map)
//   html     the HTML reader over the raw text (ParseHtmlTag tag by tag, as EmitHtmlBlock walks, and AppendHtmlText)
// Each operation is repeated until it has run for --min-time seconds (and at least 3 times); the median, the
// fastest run and the throughput are printed.
//
// --scaling builds every generated document at two sizes, n and 4n, and prints how the parse time grew with the size, as
// the exponent k in time ~ size^k: ~1 is linear, ~2 quadratic. Anything over --max-exponent (default 1.8) is reported
// and the exit code is 1, so the growth check can run in CI and catch an input shape that turned quadratic.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "doc.h"
#include "html.h"

// the formula and diagram libraries are DLLs of their own: the benchmark measures the parser as if they were present,
// the way a document with formulas is parsed in the app (a formula becomes a picture, not code text)
bool TexAvailable() { return true; }
bool MermaidAvailable() { return true; }

namespace {
using Clock = std::chrono::steady_clock;

double g_minTime = 0.3;   // seconds per operation and case
std::string g_filter;

std::string ReadBytes(const std::string& path) {
    std::string bytes;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return bytes;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes.resize(n > 0 ? (size_t)n : 0);
    if (n > 0 && fread(bytes.data(), 1, (size_t)n, f) != (size_t)n) bytes.clear();
    fclose(f);
    return bytes;
}

std::string Repeat(const std::string& s, size_t n) {
    std::string o;
    o.reserve(s.size() * n);
    for (size_t i = 0; i < n; i++) o += s;
    return o;
}

// ---- generated documents: each takes a size knob n and returns UTF-8
using Gen = std::string (*)(size_t n);

std::string GenDeepQuotes(size_t n) {     // > > > > … text   (n levels)
    return Repeat("> ", n) + "deep\n";
}
std::string GenDeepLists(size_t n) {      // a list nested n levels deep, one item per level
    std::string o;
    for (size_t i = 0; i < n; i++) o += std::string(i * 2, ' ') + "- item " + std::to_string(i) + "\n";
    return o;
}
std::string GenDeepEmphasis(size_t n) {   // ***…text…*** with n openers that never close in order
    return Repeat("*a _b ", n) + "text" + Repeat(" c_ d*", n / 2) + "\n";
}
std::string GenDeepBrackets(size_t n) {   // [[[[…]]]](url): the link-or-not search
    return Repeat("[", n) + "x" + Repeat("]", n) + "(u)\n";
}
std::string GenDeepHtml(size_t n) {       // <div><div>…</div></div> inline in a paragraph
    return "a " + Repeat("<span><b>", n) + "x" + Repeat("</b></span>", n) + "\n";
}
std::string GenLongLine(size_t n) {       // one paragraph line of n words
    std::string o;
    o.reserve(n * 6);
    for (size_t i = 0; i < n; i++) o += i % 17 ? "word " : "*em* ";
    return o + "\n";
}
std::string GenLongCodeLine(size_t n) {   // one line of n tokens in a highlighted code block
    std::string o = "```cpp\n";
    for (size_t i = 0; i < n; i++) o += "x" + std::to_string(i % 100) + " = \"s\" + 1; ";
    return o + "\n```\n";
}
std::string GenBackticks(size_t n) {      // `` ` `` runs of every length: the code span closer search
    std::string o;
    for (size_t i = 1; i <= n; i++) o += std::string(i % 64 + 1, '`') + " a ";
    return o + "\n";
}
std::string GenInlineLinks(size_t n) {    // n inline links in one paragraph
    std::string o;
    for (size_t i = 0; i < n; i++) o += "[link " + std::to_string(i) + "](https://example.com/p/" + std::to_string(i) + ") ";
    return o + "\n";
}
std::string GenRefLinks(size_t n) {       // n reference links and their definitions
    std::string o;
    for (size_t i = 0; i < n; i++) o += "see [ref " + std::to_string(i) + "][r" + std::to_string(i) + "]\n";
    o += "\n";
    for (size_t i = 0; i < n; i++) o += "[r" + std::to_string(i) + "]: https://example.com/" + std::to_string(i) + "\n";
    return o;
}
std::string GenAutolinks(size_t n) {      // bare URLs (GFM autolinks)
    std::string o;
    for (size_t i = 0; i < n; i++) o += "https://example.com/" + std::to_string(i) + " www.example.org ";
    return o + "\n";
}
std::string GenTables(size_t n) {         // n small tables
    std::string o;
    for (size_t i = 0; i < n; i++) o += "| a | b | c |\n|---|:-:|--:|\n| 1 | **2** | `3` |\n| x | y | z |\n\n";
    return o;
}
std::string GenWideTable(size_t n) {      // one table with n rows of 20 cells
    std::string row = "|" + Repeat(" cell |", 20) + "\n";
    return "|" + Repeat(" h |", 20) + "\n|" + Repeat("---|", 20) + "\n" + Repeat(row, n);
}
std::string GenHeadings(size_t n) {       // n headings, most of them with the same text (slug -1, -2, …)
    std::string o;
    for (size_t i = 0; i < n; i++) o += "## Section " + std::to_string(i % 10) + "\n\ntext\n\n";
    return o;
}
std::string GenFootnotes(size_t n) {      // n footnote references and definitions
    std::string o;
    for (size_t i = 0; i < n; i++) o += "text[^n" + std::to_string(i) + "]\n\n";
    for (size_t i = 0; i < n; i++) o += "[^n" + std::to_string(i) + "]: note " + std::to_string(i) + "\n";
    return o;
}
std::string GenCodeBlocks(size_t n) {     // n fenced blocks across several languages
    static const char* kLangs[] = {"cpp", "python", "js", "rust", "go", "html", "yaml", "sh", "json", "sql"};
    std::string o;
    for (size_t i = 0; i < n; i++)
        o += std::string("```") + kLangs[i % 10] + "\nif (x < 10) { return \"str\"; } // c\n<a href=\"x\">t</a>\n```\n\n";
    return o;
}
std::string GenHtmlBlocks(size_t n) {     // n HTML blocks of the kind READMEs use
    std::string o;
    for (size_t i = 0; i < n; i++)
        o += "<p align=\"center\"><img src=\"a.png\" width=\"100\"> <a href=\"#x\"><b>bold</b></a> &amp; &#169;</p>\n\n";
    return o;
}
std::string GenEmoji(size_t n) {          // emoji shortcodes, real and not
    return Repeat(":smile: :not_an_emoji: :+1: ", n) + "\n";
}
std::string GenMath(size_t n) {           // inline and display math
    return Repeat("text $x^2 + y_i$ and $$\\frac{a}{b}$$ ", n) + "\n";
}
std::string GenTasks(size_t n) {          // a long task list
    std::string o;
    for (size_t i = 0; i < n; i++) o += i % 2 ? "- [x] done\n" : "- [ ] todo\n";
    return o;
}
std::string GenUnclosedFences(size_t n) { // n fence openers of different lengths that never close
    std::string o;
    for (size_t i = 0; i < n; i++) o += std::string(3 + i % 7, i % 2 ? '`' : '~') + "x\ntext\n";
    return o;
}
std::string GenLtRun(size_t n) {          // '<' over and over: the inline HTML search
    return Repeat("< a", n) + "\n";
}

struct GenCase { const char* name; Gen gen; size_t n; };
// sizes picked so each document is roughly 100 KB - 1 MB: big enough to measure, small enough to run in seconds
const GenCase kGen[] = {
    {"deep-quotes", GenDeepQuotes, 2000},
    {"deep-lists", GenDeepLists, 300},
    {"deep-emphasis", GenDeepEmphasis, 20000},
    {"deep-brackets", GenDeepBrackets, 20000},
    {"deep-inline-html", GenDeepHtml, 10000},
    {"long-line", GenLongLine, 100000},
    {"long-code-line", GenLongCodeLine, 20000},
    {"backtick-runs", GenBackticks, 20000},
    {"inline-links", GenInlineLinks, 10000},
    {"ref-links", GenRefLinks, 10000},
    {"autolinks", GenAutolinks, 10000},
    {"tables", GenTables, 3000},
    {"wide-table", GenWideTable, 3000},
    {"headings", GenHeadings, 10000},
    {"footnotes", GenFootnotes, 5000},
    {"code-blocks", GenCodeBlocks, 3000},
    {"html-blocks", GenHtmlBlocks, 3000},
    {"emoji", GenEmoji, 10000},
    {"math", GenMath, 10000},
    {"task-list", GenTasks, 20000},
    {"unclosed-fences", GenUnclosedFences, 10000},
    {"lt-run", GenLtRun, 50000},
};

std::wstring Decode(const std::string& bytes) {
    std::wstring w;
    DecodeText(bytes.data(), (int)bytes.size(), w, nullptr);
    return w;
}

// runs f until min-time has passed (at least 3 times, at most 10000); returns the sorted times in seconds
std::vector<double> Time(const std::function<void()>& f, double minTime) {
    std::vector<double> t;
    double total = 0;
    while ((total < minTime || t.size() < 3) && t.size() < 10000) {
        auto a = Clock::now();
        f();
        double s = std::chrono::duration<double>(Clock::now() - a).count();
        t.push_back(s);
        total += s;
    }
    std::sort(t.begin(), t.end());
    return t;
}

double Median(const std::vector<double>& t) { return t[t.size() / 2]; }

void ParseRead(const std::wstring& w) {
    Doc d;
    d.baseDir = L"C:\\bench\\";
    ParseMarkdown(d, w.data(), w.size());
}
void ParseMap(const std::wstring& w) {
    Doc d;
    d.baseDir = L"C:\\bench\\";
    ParseOptions opt;
    opt.wantMap = true;
    ParseMarkdown(d, w.data(), w.size(), &opt);
}
void HtmlReader(const std::wstring& w) {
    // at every '<', stepping over what a tag consumed as EmitHtmlBlock does (a '<' at every character would make the
    // walk itself quadratic: an unclosed tag reads to the end of its block)
    for (size_t i = 0; i + 1 < w.size();) {
        HtmlTag tag;
        size_t used = w[i] == L'<' ? ParseHtmlTag(w.data() + i, w.size() - i, tag) : 0;
        i += used ? used : 1;
    }
    std::wstring out;
    AppendHtmlText(out, w.data(), w.size());
}

std::string Ms(double s) {
    char b[32];
    if (s < 1e-3) snprintf(b, sizeof b, "%.1f us", s * 1e6);
    else snprintf(b, sizeof b, "%.2f ms", s * 1e3);
    return b;
}

void RunCase(const std::string& name, const std::string& bytes) {
    if (!g_filter.empty() && name.find(g_filter) == std::string::npos) return;
    std::wstring w = Decode(bytes);
    struct Op { const char* name; std::function<void()> f; };
    Op ops[] = {
        {"decode", [&] { Decode(bytes); }},
        {"parse", [&] { ParseRead(w); }},
        {"map", [&] { ParseMap(w); }},
        {"html", [&] { HtmlReader(w); }},
    };
    for (const Op& op : ops) {
        std::vector<double> t = Time(op.f, g_minTime);
        double med = Median(t);
        printf("%-20s %-7s %9.1f KB %12s %12s %9.1f MB/s %6zu\n", name.c_str(), op.name, bytes.size() / 1024.0,
               Ms(med).c_str(), Ms(t.front()).c_str(), bytes.size() / med / 1e6, t.size());
        fflush(stdout);
    }
}

// parse time at n and at 4n, against the size of the two documents: the exponent k in time ~ size^k is ~1 for a linear
// cost and ~2 for a quadratic one (a nested list's size itself grows faster than its depth, so the time is compared
// with the bytes, not with n)
int Scaling(double maxExp) {
    int bad = 0;
    printf("\n%-20s %10s %10s %12s %12s %9s\n", "growth (parse+map)", "size(n)", "size(4n)", "t(n)", "t(4n)", "exponent");
    for (const GenCase& g : kGen) {
        if (!g_filter.empty() && std::string(g.name).find(g_filter) == std::string::npos) continue;
        size_t n = std::max<size_t>(g.n / 4, 8);
        std::wstring a = Decode(g.gen(n)), b = Decode(g.gen(n * 4));
        auto both = [](const std::wstring& w) { ParseRead(w); ParseMap(w); };
        double ta = Median(Time([&] { both(a); }, g_minTime)), tb = Median(Time([&] { both(b); }, g_minTime));
        double k = std::log(tb / ta) / std::log((double)b.size() / (double)a.size());
        bool over = k > maxExp;
        bad += over;
        printf("%-20s %9.0fK %9.0fK %12s %12s %9.2f%s\n", g.name, a.size() / 1024.0, b.size() / 1024.0, Ms(ta).c_str(),
               Ms(tb).c_str(), k, over ? "  <-- superlinear" : "");
        fflush(stdout);
    }
    return bad;
}
}  // namespace

int main(int argc, char** argv) {
    std::string corpus = "../../bench/corpus", doc;
    bool scaling = false, onlyScaling = false;
    double maxExp = 1.8;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--corpus" && i + 1 < argc) corpus = argv[++i];
        else if (a == "--filter" && i + 1 < argc) g_filter = argv[++i];
        else if (a == "--min-time" && i + 1 < argc) g_minTime = atof(argv[++i]);
        else if (a == "--doc" && i + 1 < argc) doc = argv[++i];
        else if (a == "--scaling") scaling = true;
        else if (a == "--only-scaling") scaling = onlyScaling = true;
        else if (a == "--max-exponent" && i + 1 < argc) maxExp = atof(argv[++i]);
        else {
            fprintf(stderr, "usage: fastmd-bench [--corpus dir | --doc file] [--filter text] [--min-time s] [--scaling | --only-scaling] "
                            "[--max-exponent k]\n");
            return 2;
        }
    }
    if (!doc.empty()) {
        std::string bytes = ReadBytes(doc);
        printf("%-20s %-7s %12s %12s %12s %14s %6s\n", "case", "op", "size", "median", "min", "throughput", "runs");
        RunCase(doc.substr(doc.find_last_of("/\\") + 1), bytes);
        return 0;
    }
    if (!onlyScaling) {
        printf("%-20s %-7s %12s %12s %12s %14s %6s\n", "case", "op", "size", "median", "min", "throughput", "runs");
        for (const char* f : {"small.md", "medium.md", "large.md"}) {
            std::string bytes = ReadBytes(corpus + "/" + f);
            if (bytes.empty()) {
                fprintf(stderr, "cannot read %s/%s (--corpus)\n", corpus.c_str(), f);
                return 2;
            }
            RunCase(std::string("corpus/") + f, bytes);
        }
        for (const GenCase& g : kGen) {
            if (!g_filter.empty() && std::string(g.name).find(g_filter) == std::string::npos) continue;
            RunCase(g.name, g.gen(g.n));
        }
    }
    return scaling && Scaling(maxExp) ? 1 : 0;
}
