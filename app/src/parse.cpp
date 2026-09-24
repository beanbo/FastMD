// Markdown → flat block model. md4c is compiled with MD4C_USE_UTF16, so it parses the UTF-16 buffer directly and
// text callbacks point into it (no per-run UTF-8 → UTF-16 conversion).
// md4c is pinned at master 7fc1815a (2026-09-17), which parses GitHub alerts (admonitions) and footnotes itself.
// It carries one local patch (documented at the top of md4c.h): hooks that report where leaves, lines, breaks, span
// delimiters, cells and footnote definitions sit in the source. Only edit mode installs them (ParseOptions::wantMap),
// to build the text <-> source map of docs/EDIT-MODE.md §4; every other parse runs md4c exactly as upstream.
// Extras on top of it: YAML front matter (shown as a yaml code block), GitHub-style heading slugs for #anchors and
// the outline, footnote numbering into links that jump both ways, and our own alert parsing as a fallback.
#include "doc.h"
#ifndef MD4C_USE_UTF16
#define MD4C_USE_UTF16
#endif
#include "../third_party/md4c/md4c.h"
#include "emoji_table.h"
#include "html.h"
#include "formulas.h"
#include <cwchar>
#include <unordered_map>

// ------------------------------------------------------------------------------------------------ helpers
std::wstring GithubSlug(const wchar_t* s, size_t n) {
    // github-slugger: lower-case, keep letters / digits / marks / '_' / '-', spaces → '-', drop everything else
    std::wstring low = ToLower(std::wstring(s, n)), out;
    out.reserve(low.size());
    std::vector<WORD> t1(low.size() + 1), t3(low.size() + 1);
    if (!low.empty()) {
        GetStringTypeW(CT_CTYPE1, low.data(), (int)low.size(), t1.data());
        GetStringTypeW(CT_CTYPE3, low.data(), (int)low.size(), t3.data());
    }
    for (size_t i = 0; i < low.size(); i++) {
        wchar_t c = low[i];
        if (c == L' ') out.push_back(L'-');
        else if (c == L'-' || c == L'_') out.push_back(c);
        else if (c >= 0xD800 && c <= 0xDFFF) continue;  // astral (emoji etc.)
        else if ((t1[i] & (C1_ALPHA | C1_DIGIT)) || (t3[i] & C3_NONSPACING)) out.push_back(c);
    }
    return out;
}

uint32_t AlertColor(uint8_t alert) { return alert ? g_pal[P_ALERT_NOTE + alert - 1] : g_pal[P_BORDER]; }

// AppendEntity lives in html.cpp: the same decoder serves Markdown entities and HTML attributes.

// ------------------------------------------------------------------------------------------------ emoji
// GitHub shortcodes: :rocket: → 🚀. The names come from the gemoji database (app/tools/gen_emoji.py), sorted, so a
// candidate is looked up with a binary search. Unknown names stay as they were typed, and code is never touched.
static const wchar_t* EmojiFor(const wchar_t* name, size_t n, uint32_t* len) {
    size_t lo = 0, hi = std::size(kEmoji);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const EmojiEntry& e = kEmoji[mid];
        const char* s = kEmojiNames + e.name;
        size_t k = n < e.nameLen ? n : e.nameLen;
        int cmp = 0;
        for (size_t i = 0; i < k && cmp == 0; i++) cmp = (int)name[i] - (int)(unsigned char)s[i];
        if (cmp == 0 && n != e.nameLen) cmp = n < e.nameLen ? -1 : 1;
        if (cmp == 0) { *len = e.charsLen; return kEmojiChars + e.chars; }
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return nullptr;
}

// The emoji a shortcode starting at s[i] == ':' stands for (nullptr = none); *end = just past its closing colon.
static const wchar_t* EmojiAt(const MD_CHAR* s, MD_SIZE n, MD_SIZE i, uint32_t* len, MD_SIZE* end) {
    MD_SIZE j = i + 1;
    while (j < n && j - i <= 40) {
        wchar_t c = s[j];
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'+' || c == L'-')) break;
        j++;
    }
    *end = j + 1;
    return (j < n && j > i + 1 && s[j] == L':') ? EmojiFor(s + i + 1, j - i - 1, len) : nullptr;
}

// ------------------------------------------------------------------------------------------------ builder
namespace {
enum CtxType : uint8_t { C_QUOTE, C_UL, C_OL, C_LI };
struct Ctx {
    uint8_t type;
    bool tight;
    bool nested;       // list inside a list item
    uint32_t next;     // next ordered number
    uint32_t items;    // number of LIs seen
    int quote;         // index into Doc::quotes
};

struct AlertSpec { const wchar_t* tag; const wchar_t* title; wchar_t icon; };
const AlertSpec kAlerts[] = {
    {L"[!NOTE]", L"Note", 0xE946},       // Segoe Fluent Icons: Info
    {L"[!TIP]", L"Tip", 0xEA80},         // Lightbulb
    {L"[!IMPORTANT]", L"Important", 0xE171},
    {L"[!WARNING]", L"Warning", 0xE7BA},
    {L"[!CAUTION]", L"Caution", 0xEA39},
};

struct Builder {
    Doc& d;
    const wchar_t* srcBase = nullptr;  // the Markdown source, for the text → source map
    const wchar_t* srcEnd = nullptr;
    uint32_t mdBase = 0;               // where md4c's input starts in the source (after the front matter)
    std::vector<Ctx> stack;
    float indent = 0;
    int quoteDepth = 0, listDepth = 0;
    float pending = 0;      // collapsed margin waiting for the next leaf
    bool atStart = true;
    std::unordered_map<std::wstring, int> slugCount;

    // current leaf being collected
    bool collecting = false;
    uint8_t leafKind = BK_TEXT, leafHeading = 0;
    bool leafIsLiImplicit = false;
    uint32_t tStart = 0, rStart = 0;
    const MD_CHAR* codeLang = nullptr;
    uint32_t codeLangLen = 0;
    int bold = 0, italic = 0, code = 0, strike = 0, link = 0, img = 0;
    int kbd = 0, sup = 0, sub = 0;  // from HTML: <kbd>, <sup>, <sub>
    uint32_t curLink = 0;
    std::vector<uint32_t> linkStack;  // hrefs of the open <a> tags (they may nest with Markdown links)
    std::wstring htmlRaw;             // the raw text of an HTML block, processed when the block ends
    bool inHtmlBlock = false;
    uint8_t pendAlign = 0;            // <p align=center> etc. for the blocks that follow
    std::vector<uint8_t> alignStack;  // alignment of the open HTML containers
    uint16_t curDetails = 0;          // <details> group the blocks belong to (0 = none), kept across blocks
    bool inSummary = false;
    // <table> in HTML: cells are collected row by row, the table is emitted when it closes
    std::vector<std::vector<Cell>> htmlRows;
    bool inHtmlTable = false;
    uint32_t htmlCellStart = 0, htmlCellRun = 0;
    // <picture>: the <source> that matches the current theme wins over the plain <img>
    bool inPicture = false;
    std::wstring pictureSrc;
    // image-only paragraph detection
    int paraImages = 0;
    // formulas and diagrams (plan 4.1, 4.2): their source is gathered here and becomes a picture the renderer fills in
    int mathDepth = 0;
    bool mathDisplay = false;
    std::wstring mathSrc;
    int texOk = -1, mermaidOk = -1;  // is the library beside the exe? asked once per document
    bool paraOther = false;
    int paraImage = -1;
    std::wstring imgAlt;  // alt text of the picture being read (kept out of the document text)
    // pending list marker (attached to the next leaf)
    uint8_t pendMarker = MK_NONE, pendLevel = 0;
    uint32_t pendNumber = 0;
    uint32_t pendTask = 0;  // task item: source offset of the character between its brackets
    std::vector<std::wstring> pendAnchors;  // #targets for the next emitted block (footnote jumps)
    uint32_t fnId = 0;                      // footnote definition being collected
    // tables
    int tIndex = -1;
    uint32_t row = 0, col = 0;
    bool inCell = false;

    // ---- edit mode's text <-> source map (docs/EDIT-MODE.md §4.4). Idle unless `map`: a reading-mode parse takes
    // none of these paths, and md4c's hooks are not even installed then.
    bool map = false;
    int noSegs = 0;            // > 0: text appended now gets no segment (HTML-block text, front matter, tag atoms)
    struct Extent {
        bool set = false;
        MD_BLOCKTYPE type = MD_BLOCK_P;
        uint32_t beg = 0, end = 0;
        unsigned flags = 0;
        uint32_t fnDef = UINT32_MAX;  // a footnote definition: the '[' of "[^label]:"
    };
    Extent ext;                // the next leaf's extent (leaf_extent / footnote_extent); the Emit that uses it clears it
    bool brk = false, brkNow = false;  // a break extent waits for its static text / the text being added is that break
    uint32_t brkBeg = 0, brkEnd = 0;
    bool nulNow = false;       // the text being added stands for a NUL character of the source
    uint32_t srcCur = 0;       // just past the last thing md4c reported: where a NUL sits, where a leaf starts
    uint32_t segStart = 0, spanStart = 0;  // the first segment / span of the current leaf (or Markdown table)
    // the lines of the current code block
    uint32_t vFirst = UINT32_MAX, vLast = 0;  // the first content line's code-indentation start, the last one's end
    uint32_t vIndentLeft = 0;  // indentation spaces md4c has still to send for this line
    int32_t vNewline = -1;     // the "\n" atom whose source end arrives with the next line
    bool emptyItem = false;    // EndLeaf closes the leaf made up for an empty list item
    struct OpenSpan { uint32_t idx; bool pseudo; uint8_t cls; uint32_t images; };
    std::vector<OpenSpan> openSpans;  // spans of the current leaf / cell not closed yet (Doc::spans indices)
    std::vector<int32_t> contStack;   // open containers (Doc::containers indices)
    bool mdCell = false;       // inside a cell of a Markdown table (its text belongs to the table block)
    CellSrc cellExt{0, 0, true};
    Extent htmlExt;            // the HTML block being read
    uint32_t htmlFirst = 0;
    uint32_t fmBody = 0, fmLast = 0, fmOuter = 0;  // front matter: first body line, end of the last one, closing line end

    explicit Builder(Doc& doc) : d(doc) {}

    // ---- source lines (map mode). A line ends at \n, \r\n or a lone \r, as it does for md4c.
    uint32_t SrcLen() const { return (uint32_t)(srcEnd - srcBase); }
    uint32_t LineStart(uint32_t s) const {
        while (s > 0 && srcBase[s - 1] != L'\n' && srcBase[s - 1] != L'\r') s--;
        return s;
    }
    uint32_t LineEnd(uint32_t s) const {
        uint32_t n = SrcLen();
        while (s < n && srcBase[s] != L'\n' && srcBase[s] != L'\r') s++;
        return s;
    }
    uint32_t NextLine(uint32_t e) const {  // e at a line end: where the next line starts
        uint32_t n = SrcLen();
        if (e < n && srcBase[e] == L'\r') e++;
        if (e < n && srcBase[e] == L'\n') e++;
        return e;
    }
    static bool Blank(wchar_t c) { return c == L' ' || c == L'\t'; }

    // Text added now gets segments: a map is wanted, the text is not an HTML block's or a picture's alt text, and it
    // belongs to a leaf or to a Markdown table cell.
    bool SegsOn() const { return map && !noSegs && !img && !mathDepth && (collecting || mdCell); }

    void PushSeg(uint32_t t, uint32_t tLen, uint32_t s, uint32_t sLen, uint8_t kind, uint8_t flags = 0) {
        if (!tLen) return;
        d.segs.push_back(SrcSeg{t, tLen, s, sLen, kind, flags});
        if (kind != SEG_SYNTH) srcCur = s + sLen;
    }

    // md4c's hooks (every offset is relative to what md4c was given, which starts after the front matter)
    void OnLeafExtent(MD_BLOCKTYPE type, uint32_t beg, uint32_t end, unsigned flags) {
        if (collecting) EndLeaf();  // a paragraph a tight list swallowed ends where the next leaf begins
        ext = Extent{true, type, mdBase + beg, mdBase + end, flags, UINT32_MAX};
        srcCur = ext.beg;
    }
    void OnFootnoteExtent(uint32_t defBeg, uint32_t beg, uint32_t end) {
        if (collecting) EndLeaf();
        ext = Extent{true, MD_BLOCK_P, mdBase + beg, mdBase + end, 0, mdBase + defBeg};
        srcCur = ext.beg;
    }
    void OnBreakExtent(uint32_t beg, uint32_t end) {
        brk = true;
        brkBeg = mdBase + beg;
        brkEnd = mdBase + end;
    }
    void OnCellExtent(uint32_t beg, uint32_t end, int missing) {
        cellExt = CellSrc{mdBase + beg, mdBase + end, missing != 0};
    }

    // One content line of a code block, just before md4c sends its indentation, its text and its "\n". md4c re-emits
    // the code indentation (`indent` columns) as spaces from a static string, so the Builder maps it itself: it is the
    // last `indent` columns of the blanks before `beg` (tab stops of 4 from the line start). A space there is PLAIN, a
    // tab an atom as wide as it is, and a tab that is partly a container's indentation is split (SEGF_SPLITTAB). The
    // previous line's "\n" atom ends where this line's code indentation starts (line end + container prefix).
    void OnVerbatimLine(uint32_t beg, uint32_t end, unsigned indent) {
        if (inHtmlBlock || !collecting || leafKind != BK_CODE || noSegs) return;
        beg += mdBase;
        end += mdBase;
        uint32_t ls = LineStart(beg), ws = beg;
        while (ws > ls && Blank(srcBase[ws - 1])) ws--;
        auto next = [&](uint32_t c, uint32_t p) { return srcBase[p] == L'\t' ? (c + 4) & ~3u : c + 1; };
        uint32_t colW = 0;
        for (uint32_t p = ls; p < ws; p++) colW = next(colW, p);
        uint32_t colBeg = colW;
        for (uint32_t p = ws; p < beg; p++) colBeg = next(colBeg, p);
        uint32_t t = (uint32_t)d.text.size(), indentStart = beg;
        if (indent > colBeg - colW) {  // more indentation than blanks: md4c's arithmetic went somewhere we do not follow
            indentStart = ws;
            PushSeg(t, indent, ws, 0, SEG_SYNTH);
        } else if (indent) {
            uint32_t from = colBeg - indent, c0 = colW, plainT = 0, plainS = UINT32_MAX, plainN = 0;
            auto flushPlain = [&]() {
                if (plainN) PushSeg(plainT, plainN, plainS, plainN, SEG_PLAIN);
                plainN = 0;
            };
            for (uint32_t p = ws; p < beg; p++) {
                uint32_t c1 = next(c0, p);
                if (c1 > from) {
                    if (indentStart == beg) indentStart = p;
                    if (srcBase[p] == L' ') {
                        if (!plainN) { plainT = t; plainS = p; }
                        plainN++;
                        t++;
                    } else {  // a tab: one source character, as many text spaces as columns it covers here
                        flushPlain();
                        uint32_t w = c1 - std::max(c0, from);
                        PushSeg(t, w, p, 1, SEG_TEXTATOM, c0 < from ? SEGF_SPLITTAB : 0);
                        t += w;
                    }
                }
                c0 = c1;
            }
            flushPlain();
        }
        if (vNewline >= 0 && (size_t)vNewline < d.segs.size()) {
            SrcSeg& nl = d.segs[vNewline];
            nl.sLen = indentStart > nl.s ? indentStart - nl.s : 1;
        }
        vNewline = -1;
        vIndentLeft = indent;
        if (vFirst == UINT32_MAX) vFirst = indentStart;
        vLast = end;
        srcCur = beg;
    }

    // Span delimiters: the enter hook comes before md4c's enter callback, the leave hook after its leave callback, so
    // the text a span adds itself (a picture's U+FFFC, a footnote's "[n]", a formula's U+FFFC) lies between the two.
    void OnSpanExtent(MD_SPANTYPE type, int enter, uint32_t beg, uint32_t end) {
        beg += mdBase;
        end += mdBase;
        if (enter) EnsureLeaf();
        srcCur = end;
        if (!map || noSegs || img || !(collecting || mdCell)) return;  // alt text: its spans are not in the text
        uint32_t tNow = (uint32_t)d.text.size();
        if (enter) {
            uint8_t fl = 0;
            if (type == MD_SPAN_EM || type == MD_SPAN_STRONG) fl = SF_ENTERABLE | (srcBase[beg] == L'_' ? SF_UNDERSCORE : 0);
            else if (type == MD_SPAN_DEL) fl = SF_ENTERABLE;
            else if (type == MD_SPAN_A && (beg == end || srcBase[beg] == L'<')) fl = SF_AUTOLINK;
            d.spans.push_back(SpanSrc{-1, tNow, tNow, beg, end, end, end, (uint8_t)type, fl});
            openSpans.push_back(OpenSpan{(uint32_t)d.spans.size() - 1, false, (uint8_t)type, (uint32_t)d.images.size()});
            return;
        }
        size_t k = openSpans.size();  // the innermost open Markdown span of this type; pseudo-spans above it are cut
        while (k > 0 && (openSpans[k - 1].pseudo || openSpans[k - 1].cls != (uint8_t)type)) k--;
        if (k == 0) return;
        for (size_t m = openSpans.size(); m-- > k;) CutSpan(openSpans[m], beg, tNow);
        OpenSpan os = openSpans[k - 1];
        openSpans.resize(k - 1);
        SpanSrc& sp = d.spans[os.idx];
        sp.tEnd = tNow;
        sp.closeBeg = beg;
        sp.closeEnd = end;
        // md4c reports a footnote reference's whole "[^label]" on enter and on leave: it is all opener, and its closer
        // is empty at its end, so the delimiters stay in order (§4.5 #7)
        if (type == MD_SPAN_FOOTNOTE_REF) sp.closeBeg = sp.closeEnd = std::max(end, sp.openEnd);
        if ((type == MD_SPAN_A || type == MD_SPAN_IMG) && !(sp.flags & SF_AUTOLINK) &&
            !(end - beg >= 2 && srcBase[beg] == L']' && srcBase[beg + 1] == L'('))
            sp.flags |= SF_REF;
        if (tNow <= sp.tBeg) return;
        if (type == MD_SPAN_IMG) {  // the picture (and whatever its alt text drew) is one object atom
            PushSeg(sp.tBeg, tNow - sp.tBeg, sp.openBeg, end - sp.openBeg, SEG_OBJATOM);
            if (d.images.size() > os.images) {
                Image& im = d.images[os.images];
                im.outerBeg = sp.openBeg;
                im.outerEnd = end;
                im.altBeg = sp.openEnd;
                im.altEnd = beg;
                if (!(sp.flags & SF_REF)) DestinationOf(beg + 2, end, &im.srcBeg, &im.srcEnd);
            }
        } else if ((type == MD_SPAN_LATEXMATH || type == MD_SPAN_LATEXMATH_DISPLAY) && texOk == 1) {
            PushSeg(sp.tBeg, tNow - sp.tBeg, sp.openBeg, end - sp.openBeg, SEG_OBJATOM);
            if (d.images.size() > os.images) {
                Image& im = d.images.back();
                im.outerBeg = sp.openBeg;
                im.outerEnd = end;
                im.srcBeg = sp.openEnd;
                im.srcEnd = beg;
            }
        } else if (type == MD_SPAN_FOOTNOTE_REF) {  // "[n]": drawn from the number, typed as [^label]
            PushSeg(sp.tBeg, tNow - sp.tBeg, sp.openBeg, end - sp.openBeg, SEG_TEXTATOM);
        }
    }

    // A picture's destination in "](dest "title")": <…> or up to the first blank or unbalanced ')'.
    void DestinationOf(uint32_t p, uint32_t end, uint32_t* db, uint32_t* de) const {
        while (p < end && (Blank(srcBase[p]) || srcBase[p] == L'\r' || srcBase[p] == L'\n')) p++;
        if (p < end && srcBase[p] == L'<') {
            uint32_t q = p + 1;
            while (q < end && srcBase[q] != L'>') q++;
            *db = p + 1;
            *de = q;
            return;
        }
        uint32_t q = p;
        int depth = 0;
        while (q < end) {
            wchar_t c = srcBase[q];
            if (c == L'\\' && q + 1 < end) { q += 2; continue; }
            if (Blank(c) || c == L'\r' || c == L'\n') break;
            if (c == L'(') depth++;
            else if (c == L')' && depth-- == 0) break;
            q++;
        }
        *db = p;
        *de = std::min(q, end);
    }

    // A span that cannot close properly (an HTML tag pair cut by a Markdown span, or still open at the end of its
    // leaf) ends where it was cut, flagged unclosed.
    void CutSpan(const OpenSpan& os, uint32_t at, uint32_t tNow) {
        SpanSrc& sp = d.spans[os.idx];
        sp.tEnd = tNow;
        sp.closeBeg = sp.closeEnd = std::max(at, sp.openEnd);
        sp.flags |= SF_UNCLOSED;
    }
    void CutOpenSpans(uint32_t at) {
        uint32_t tNow = (uint32_t)d.text.size();
        for (size_t m = openSpans.size(); m-- > 0;) CutSpan(openSpans[m], at, tNow);
        openSpans.clear();
    }

    // Inline HTML formatting tags form pseudo-spans of their class (F25): an opener pushes one, a closer ends the
    // innermost open one of its class - unless a Markdown span opened after it, which would be cut in two.
    static int HtmlSpanClass(const std::wstring& n) {
        if (n == L"b" || n == L"strong") return ST_HTML_B;
        if (n == L"i" || n == L"em" || n == L"cite" || n == L"var") return ST_HTML_I;
        if (n == L"code" || n == L"tt" || n == L"samp") return ST_HTML_CODE;
        if (n == L"del" || n == L"s" || n == L"strike") return ST_HTML_S;
        if (n == L"kbd") return ST_HTML_KBD;
        if (n == L"sup") return ST_HTML_SUP;
        if (n == L"sub") return ST_HTML_SUB;
        if (n == L"a") return ST_HTML_A;
        return 0;
    }
    void PseudoSpan(const HtmlTag& tag, uint32_t beg, uint32_t end) {
        int cls = HtmlSpanClass(tag.name);
        if (!cls || tag.selfClose) return;
        uint32_t tNow = (uint32_t)d.text.size();
        if (!tag.closing) {
            uint8_t fl = (cls == ST_HTML_B || cls == ST_HTML_I || cls == ST_HTML_S || cls == ST_HTML_SUP ||
                          cls == ST_HTML_SUB) ? SF_ENTERABLE : 0;
            d.spans.push_back(SpanSrc{-1, tNow, tNow, beg, end, end, end, (uint8_t)cls, fl});
            openSpans.push_back(OpenSpan{(uint32_t)d.spans.size() - 1, true, (uint8_t)cls, 0});
            return;
        }
        for (size_t k = openSpans.size(); k-- > 0;) {
            if (!openSpans[k].pseudo) return;
            if (openSpans[k].cls != cls) continue;
            for (size_t m = openSpans.size(); m-- > k + 1;) CutSpan(openSpans[m], beg, tNow);
            SpanSrc& sp = d.spans[openSpans[k].idx];
            sp.tEnd = tNow;
            sp.closeBeg = beg;
            sp.closeEnd = end;
            openSpans.resize(k);
            return;
        }
    }

    // the value range of an HTML attribute inside a tag's source (for <img src=…>)
    static bool AttrValueRange(const wchar_t* s, uint32_t n, const wchar_t* name, uint32_t* vb, uint32_t* ve) {
        size_t ln = wcslen(name);
        for (uint32_t i = 1; i + ln < n; i++) {
            if (!iswspace(s[i - 1]) || _wcsnicmp(s + i, name, ln) != 0) continue;
            uint32_t j = i + (uint32_t)ln;
            while (j < n && iswspace(s[j])) j++;
            if (j >= n || s[j] != L'=') continue;
            j++;
            while (j < n && iswspace(s[j])) j++;
            if (j < n && (s[j] == L'"' || s[j] == L'\'')) {
                wchar_t q = s[j];
                uint32_t k = j + 1;
                while (k < n && s[k] != q) k++;
                if (k >= n) return false;
                *vb = j + 1;
                *ve = k;
                return true;
            }
            uint32_t k = j;
            while (k < n && !iswspace(s[k]) && s[k] != L'>' && s[k] != L'/') k++;
            *vb = j;
            *ve = k;
            return true;
        }
        return false;
    }

    // An inline HTML tag in map mode: what HtmlInline adds (a <br>'s "\n", an <img>'s U+FFFC) is one atom over the
    // tag's source; formatting tags become pseudo-spans.
    void MapHtmlTag(const HtmlTag& tag, const MD_CHAR* s, MD_SIZE n) {
        bool inSrc = srcBase && s >= srcBase && s < srcEnd;
        uint32_t so = inSrc ? (uint32_t)(s - srcBase) : srcCur;
        if (tag.name.empty()) {  // a comment
            if (inSrc) srcCur = so + n;
            return;
        }
        uint32_t t0 = (uint32_t)d.text.size(), i0 = (uint32_t)d.images.size();
        noSegs++;
        HtmlInline(tag);
        noSegs--;
        uint32_t len = (uint32_t)d.text.size() - t0;
        if (len && SegsOn()) {
            if (inSrc) PushSeg(t0, len, so, n, d.text[t0] == L'\xFFFC' ? SEG_OBJATOM : SEG_TEXTATOM);
            else PushSeg(t0, len, srcCur, 0, SEG_SYNTH);
        }
        if (inSrc && d.images.size() > i0) {
            Image& im = d.images.back();
            im.outerBeg = so;
            im.outerEnd = so + n;
            uint32_t vb, ve;
            if (AttrValueRange(s, n, L"src", &vb, &ve)) { im.srcBeg = so + vb; im.srcEnd = so + ve; }
        }
        if (inSrc && SegsOn()) PseudoSpan(tag, so, so + n);
        if (inSrc) srcCur = so + n;
    }

    // Segments for a chunk AppendText has just added: [start, start+len) of the text, from s[0..n).
    void MapChunk(uint32_t start, uint32_t len, const MD_CHAR* s, MD_SIZE n, bool inSrc, bool isEntity, bool raw) {
        if (!SegsOn()) return;
        if (inSrc) {
            uint32_t so = (uint32_t)(s - srcBase);
            // an entity is an atom - unless it is one nobody knows (&bogus;), which stays as typed: plain text
            if (isEntity) PushSeg(start, len, so, n, len == n && !d.text.compare(start, len, s, n) ? SEG_PLAIN : SEG_TEXTATOM);
            else if (raw) PushSeg(start, len, so, n, SEG_PLAIN);
            // other text got its segments from AppendWithEmoji (a backslash escape among them)
            return;
        }
        if (nulNow) {  // U+FFFD for a NUL: it sits where md4c stopped reporting
            uint32_t g = srcCur, e = std::min(SrcLen(), srcCur + 64);
            while (g < e && srcBase[g] != 0) g++;
            if (g < e) PushSeg(start, len, g, 1, SEG_TEXTATOM);
            else PushSeg(start, len, srcCur, 0, SEG_SYNTH);
            return;
        }
        if (brkNow) {  // a soft or hard break, or a line join inside a code span or formula
            PushSeg(start, len, brkBeg, brkEnd > brkBeg ? brkEnd - brkBeg : 1, SEG_TEXTATOM);
            brkNow = false;
            return;
        }
        if (collecting && leafKind == BK_CODE) {
            if (vIndentLeft) {  // the code indentation: its segments were made when the line was reported
                uint32_t k = std::min(len, vIndentLeft);
                vIndentLeft -= k;
                if (k < len) PushSeg(start + k, len - k, srcCur, 0, SEG_SYNTH);
                return;
            }
            if (len == 1 && d.text[start] == L'\n') {  // a line end: its source reaches into the next line
                vNewline = (int32_t)d.segs.size();
                PushSeg(start, 1, vLast, 1, SEG_TEXTATOM);
                return;
            }
        }
        PushSeg(start, len, srcCur, 0, SEG_SYNTH);  // text with no source we know of: never a caret stop
    }

    // is the one-character chunk at source offset so a backslash escape (after an odd run of backslashes)?
    bool IsEscaped(uint32_t so) const {
        uint32_t k = 0;
        while (so > k && srcBase[so - k - 1] == L'\\') k++;
        return k & 1;
    }

    // Normal text: :shortcodes: become emoji. Reading mode notes in srcMap where the text after each emoji came from;
    // map mode gives the text PLAIN segments and every emoji (or a backslash escape) an atom of its own.
    void AppendWithEmoji(const MD_CHAR* s, MD_SIZE n, bool inSrc) {
        std::wstring& t = d.text;
        bool segs = inSrc && SegsOn();
        uint32_t so = inSrc ? (uint32_t)(s - srcBase) : 0;
        if (segs && n == 1 && IsEscaped(so)) {
            PushSeg((uint32_t)t.size(), 1, so - 1, 2, SEG_TEXTATOM);
            t.push_back(s[0]);
            return;
        }
        MD_SIZE run = 0;
        uint32_t runT = (uint32_t)t.size();
        for (MD_SIZE i = 0; i < n;) {
            if (s[i] == L':') {
                uint32_t len = 0;
                MD_SIZE end = 0;
                if (const wchar_t* e = EmojiAt(s, n, i, &len, &end)) {
                    if (segs) {
                        PushSeg(runT, (uint32_t)(i - run), so + (uint32_t)run, (uint32_t)(i - run), SEG_PLAIN);
                        PushSeg((uint32_t)t.size(), len, so + (uint32_t)i, (uint32_t)(end - i), SEG_TEXTATOM);
                    }
                    t.append(e, len);
                    i = end;
                    run = i;
                    runT = (uint32_t)t.size();
                    // the text after it in this chunk maps from here (the shortcode is longer than the emoji)
                    if (inSrc && !map && i < n) d.srcMap.emplace_back(runT, so + (uint32_t)i);
                    continue;
                }
            }
            t.push_back(s[i++]);
        }
        if (segs) PushSeg(runT, (uint32_t)(n - run), so + (uint32_t)run, (uint32_t)(n - run), SEG_PLAIN);
    }

    void Margin(float m) { if (m > pending) pending = m; }

    void StartLeaf(uint8_t kind, uint8_t heading) {
        collecting = true;
        leafKind = kind;
        leafHeading = heading;
        tStart = (uint32_t)d.text.size();
        rStart = (uint32_t)d.runs.size();
        paraImages = 0;
        paraOther = false;
        paraImage = -1;
        if (map) {
            segStart = (uint32_t)d.segs.size();
            spanStart = (uint32_t)d.spans.size();
            openSpans.clear();
            vFirst = UINT32_MAX;
            vLast = 0;
            vIndentLeft = 0;
            vNewline = -1;
            if (ext.set) srcCur = ext.beg;
        }
    }

    // map mode: the block just emitted owns the segments and spans made since segFrom / spanFrom
    void ClaimMap(uint32_t segFrom, uint32_t spanFrom) {
        BlockSrc& bs = d.blockSrc.back();
        bs.segOff = segFrom;
        bs.segCount = (uint32_t)d.segs.size() - segFrom;
        bs.spanOff = spanFrom;
        bs.spanCount = (uint32_t)d.spans.size() - spanFrom;
        for (size_t k = spanFrom; k < d.spans.size(); k++) d.spans[k].block = (int32_t)d.blocks.size() - 1;
    }
    // ... or has none: an object atom, raw or synthesized
    void DropMap(uint32_t segFrom, uint32_t spanFrom) {
        if (d.segs.size() > segFrom) d.segs.resize(segFrom);
        if (d.spans.size() > spanFrom) d.spans.resize(spanFrom);
        BlockSrc& bs = d.blockSrc.back();
        bs.segOff = (uint32_t)d.segs.size();
        bs.segCount = 0;
        bs.spanOff = (uint32_t)d.spans.size();
        bs.spanCount = 0;
    }

    // The source record of the leaf just emitted, from its extent (§4.4's block table). Clears the extent.
    void LeafSrc() {
        BlockSrc& bs = d.blockSrc.back();
        Extent e = ext;
        ext.set = false;
        if (emptyItem && !(e.set && e.end > e.beg) && !contStack.empty() && d.containers[contStack.back()].kind == CT_ITEM) {
            // an empty list item: its insertion point is after the marker and its blanks (a task: after "[ ]" and one
            // blank), on the marker's line
            const ContainerSrc& c = d.containers[contStack.back()];
            uint32_t le = LineEnd(c.markOff), p = c.markOff + c.markLen;
            if (c.taskOff != UINT32_MAX) {
                p = c.taskOff + 2;
                if (p < le && Blank(srcBase[p])) p++;
            } else {
                while (p < le && Blank(srcBase[p])) p++;
            }
            p = std::min(p, le);
            bs.beg = bs.end = p;
            bs.line = LineStart(c.markOff);
            bs.lineEnd = bs.outerEnd = le;
            bs.flags = BS_EMPTYITEM;
            return;
        }
        if (!e.set) return;  // a leaf md4c said nothing about: stays synthesized
        bs.beg = e.beg;
        bs.end = e.end;
        bs.flags = 0;
        if (e.fnDef != UINT32_MAX) {  // footnote definition text: rendered at the end, written wherever it was
            bs.line = LineStart(e.fnDef);
            bs.lineEnd = bs.outerEnd = LineEnd(e.end);
            bs.aux = e.fnDef;
            bs.flags = BS_FOOTNOTE;
            return;
        }
        if (e.type == MD_BLOCK_CODE && (e.flags & MD_FASTMD_FENCED)) {
            bs.line = LineStart(e.beg);  // the opening fence's line
            bs.aux = LineEnd(e.beg);
            bs.flags = BS_FENCED;
            if (vFirst == UINT32_MAX) {  // ```⏎``` : no content line at all
                bs.beg = bs.end = bs.lineEnd = bs.aux;
                bs.flags |= BS_NOCONTENT;
            } else {
                bs.beg = vFirst;
                bs.end = bs.lineEnd = vLast;
            }
            if (e.flags & MD_FASTMD_CLOSED) {
                bs.outerEnd = LineEnd(NextLine(bs.lineEnd));
            } else {
                bs.outerEnd = bs.lineEnd;
                bs.flags |= BS_UNCLOSED;
            }
            return;
        }
        if (e.type == MD_BLOCK_CODE) {  // indented: from text offset 0 to the end of the last line md4c kept
            if (vFirst != UINT32_MAX) {
                bs.beg = vFirst;
                bs.end = vLast;
            }
            bs.line = LineStart(bs.beg);
            bs.lineEnd = bs.outerEnd = LineEnd(bs.end);
            return;
        }
        bs.line = LineStart(e.beg);
        bs.lineEnd = bs.outerEnd = LineEnd(e.end);
        if (e.type == MD_BLOCK_H) {
            if (e.flags & MD_FASTMD_SETEXT) {  // the underline md4c dropped: the next line, = or - after the prefix
                bs.flags = BS_SETEXT;
                uint32_t p = NextLine(bs.lineEnd), n = SrcLen();
                while (p < n && (Blank(srcBase[p]) || srcBase[p] == L'>')) p++;
                wchar_t u = p < n ? srcBase[p] : 0;
                if (u == L'=' || u == L'-') {
                    while (p < n && srcBase[p] == u) p++;
                    while (p < n && Blank(srcBase[p])) p++;
                    if (p == LineEnd(p)) bs.outerEnd = p;
                }
            } else {
                bs.flags = BS_ATX;
            }
        } else if (e.type == MD_BLOCK_HR) {
            bs.flags = BS_OBJECT;
        }
    }

    Block& Emit(uint8_t kind, float marginTop, float marginBottom) {
        Margin(marginTop);
        Block b{};
        b.kind = kind;
        b.gap = atStart ? 0.f : pending;
        atStart = false;
        pending = marginBottom;
        b.indent = indent;
        b.align = pendAlign;
        b.details = curDetails ? (uint16_t)(curDetails | (inSummary ? 0x8000 : 0)) : 0;
        b.muted = quoteDepth > 0;
        b.marker = pendMarker;
        b.listLevel = pendLevel;
        b.number = pendNumber;
        b.textOff = (uint32_t)d.text.size();  // every block owns a [textOff, textOff+textLen) range, in order
        if (pendMarker == MK_TASK_OPEN || pendMarker == MK_TASK_DONE)
            d.tasks.push_back(Task{(uint32_t)d.blocks.size(), pendTask});
        pendMarker = MK_NONE;
        d.blocks.push_back(b);
        for (std::wstring& a : pendAnchors) d.anchors.push_back(Anchor{std::move(a), (uint32_t)d.blocks.size() - 1});
        pendAnchors.clear();
        if (map) {  // synthesized until its caller says where it came from
            BlockSrc bs;
            bs.flags = BS_SYNTH;
            bs.segOff = (uint32_t)d.segs.size();
            bs.spanOff = (uint32_t)d.spans.size();
            bs.container = contStack.empty() ? -1 : contStack.back();
            d.blockSrc.push_back(bs);
        }
        return d.blocks.back();
    }

    // the title line of a GitHub alert: icon + name, its own block in the alert's colour
    void EmitAlertTitle(uint8_t alert) {
        if (!alert || alert > std::size(kAlerts)) return;
        const AlertSpec& spec = kAlerts[alert - 1];
        uint32_t ts = (uint32_t)d.text.size(), rs = (uint32_t)d.runs.size();
        d.text.push_back(spec.icon);
        d.text += L"  ";
        d.text += spec.title;
        uint32_t T = (uint32_t)d.text.size() - ts;
        uint8_t pal = (uint8_t)(P_ALERT_NOTE + alert - 1);
        d.runs.push_back(Run{ts, 1, F_ICON, pal, 0, 0});
        d.runs.push_back(Run{ts + 1, T - 1, F_BOLD, pal, 0, 0});
        Block& tb = Emit(BK_TEXT, 0, 8);
        tb.alertTitle = alert;
        tb.textOff = ts;
        tb.textLen = T;
        tb.runOff = rs;
        tb.runCount = 2;
    }

    // Map mode, one-line alert: the tag's text [tStart, tagEnd) became the title. Its segments and the spans opened in
    // it go; a PLAIN segment across its end is cut there; everything after moves with the text. The content then
    // starts at the first source character left (alertBeg).
    uint32_t alertBeg = UINT32_MAX;
    void AlertMap(uint32_t tagEnd, int32_t shift) {
        std::vector<SrcSeg> kept;
        for (size_t i = segStart; i < d.segs.size(); i++) {
            SrcSeg sg = d.segs[i];
            if (sg.t + sg.tLen <= tagEnd) continue;
            if (sg.t < tagEnd) {
                uint32_t cut = tagEnd - sg.t;
                if (sg.kind == SEG_PLAIN) {
                    sg.s += cut;
                    sg.sLen -= cut;
                } else {  // an atom cannot be split: what is left of it has no source
                    sg.s += sg.sLen;
                    sg.sLen = 0;
                    sg.kind = SEG_SYNTH;
                }
                sg.t += cut;
                sg.tLen -= cut;
            }
            sg.t = (uint32_t)((int32_t)sg.t + shift);
            if (alertBeg == UINT32_MAX && sg.kind != SEG_SYNTH) alertBeg = sg.s;
            kept.push_back(sg);
        }
        d.segs.resize(segStart);
        d.segs.insert(d.segs.end(), kept.begin(), kept.end());
        std::vector<SpanSrc> spansKept;
        for (size_t i = spanStart; i < d.spans.size(); i++) {
            SpanSrc sp = d.spans[i];
            if (sp.tBeg < tagEnd) continue;
            sp.tBeg = (uint32_t)((int32_t)sp.tBeg + shift);
            sp.tEnd = (uint32_t)((int32_t)sp.tEnd + shift);
            spansKept.push_back(sp);
        }
        d.spans.resize(spanStart);
        d.spans.insert(d.spans.end(), spansKept.begin(), spansKept.end());
    }

    // The first paragraph of a blockquote starting with [!NOTE] etc. turns the quote into a GitHub alert: the tag is
    // replaced by a title line (icon + name) that becomes its own block, in document order.
    bool TryAlert(uint32_t& tEnd) {
        if (leafKind != BK_TEXT || leafHeading || stack.empty() || stack.back().type != C_QUOTE) return false;
        QuoteSpan& q = d.quotes[stack.back().quote];
        if (q.first != d.blocks.size() || q.alert) return false;  // not the quote's first leaf
        for (size_t a = 0; a < 5; a++) {
            size_t tl = wcslen(kAlerts[a].tag);
            if (tEnd - tStart < tl || _wcsnicmp(d.text.data() + tStart, kAlerts[a].tag, tl) != 0) continue;
            size_t k = tl;
            if (tStart + k < tEnd && !iswspace(d.text[tStart + k])) return false;  // "[!NOTE]x" is not an alert
            while (tStart + k < tEnd && iswspace(d.text[tStart + k])) k++;
            uint8_t alert = (uint8_t)(a + 1);
            q.alert = alert;
            std::wstring title;
            title.push_back(kAlerts[a].icon);
            title += L"  ";
            title += kAlerts[a].title;
            uint32_t T = (uint32_t)title.size();
            int32_t shift = (int32_t)T - (int32_t)k;
            d.text.replace(tStart, k, title);
            for (size_t r = rStart; r < d.runs.size(); r++) d.runs[r].start += shift;
            if (map) AlertMap(tStart + (uint32_t)k, shift);
            uint8_t pal = (uint8_t)(P_ALERT_NOTE + a);
            Run icon{tStart, 1, F_ICON, pal, 0, 0}, name{tStart + 1, T - 1, F_BOLD, pal, 0, 0};
            d.runs.insert(d.runs.begin() + rStart, {icon, name});
            Block& tb = Emit(BK_TEXT, 0, 8);
            tb.alertTitle = alert;
            tb.textOff = tStart;
            tb.textLen = T;
            tb.runOff = rStart;
            tb.runCount = 2;
            if (map) {  // the title has no source; the quote around it is an alert now
                d.blockSrc.back().segOff = segStart;
                d.blockSrc.back().spanOff = spanStart;
                if (!contStack.empty() && d.containers[contStack.back()].kind == CT_QUOTE)
                    d.containers[contStack.back()].kind = CT_ALERT;
            }
            tEnd = (uint32_t)d.text.size();
            tStart += T;
            rStart += 2;
            return true;
        }
        return false;
    }

    void EndLeaf() {
        if (!collecting) return;
        collecting = false;
        uint32_t tEnd = (uint32_t)d.text.size();
        bool mermaid = leafKind == BK_CODE && codeLang && codeLangLen == 7 && _wcsnicmp(codeLang, L"mermaid", 7) == 0 &&
                       MermaidOk();
        if (leafKind == BK_CODE) {
            if (map && !noSegs && !mermaid) {
                // edit mode: only the last line's own "\n" goes, so empty last lines keep their caret positions
                if (tEnd > tStart && d.text[tEnd - 1] == L'\n') tEnd--;
            } else {
                while (tEnd > tStart && (d.text[tEnd - 1] == L'\n' || d.text[tEnd - 1] == L'\r')) tEnd--;
            }
            d.text.resize(tEnd);
            if (map) {  // the segments of what was trimmed go with it
                while (d.segs.size() > segStart && d.segs.back().t >= tEnd) d.segs.pop_back();
                if (vNewline >= (int32_t)d.segs.size()) vNewline = -1;
            }
        }
        if (map) {  // HTML tag pairs still open end with the leaf
            uint32_t contentEnd = ext.set ? ext.end : srcCur;
            if (emptyItem || leafKind == BK_CODE) contentEnd = std::max(contentEnd, srcCur);
            CutOpenSpans(contentEnd);
        }
        if (leafKind == BK_TEXT && !leafHeading && paraImages == 1 && !paraOther && paraImage >= 0) {
            // paragraph that is just a picture → picture block, with the alt text for its placeholder
            d.runs.resize(rStart);
            d.text.resize(tStart);
            const std::wstring& alt = d.images[paraImage].alt;
            d.text += alt;
            Block& b = Emit(BK_IMAGE, 0, 16);
            b.aux = (uint32_t)paraImage;
            b.textOff = tStart;
            b.textLen = (uint32_t)alt.size();
            if (map) {  // an object atom: its source is edited in a popup, its text has no segments
                DropMap(segStart, spanStart);
                LeafSrc();
                d.blockSrc.back().flags |= BS_OBJECT;
            }
            return;
        }
        alertBeg = UINT32_MAX;
        if (TryAlert(tEnd) && tEnd == tStart) {  // alert whose content starts in the next paragraph
            leafIsLiImplicit = false;
            if (map) {
                d.segs.resize(std::min<size_t>(d.segs.size(), segStart));
                d.spans.resize(std::min<size_t>(d.spans.size(), spanStart));
                ext.set = false;
            }
            return;
        }
        float mt = 0, mb = 16;
        if (leafHeading) { mt = 24; mb = 16; }
        else if (leafIsLiImplicit) { mt = 0; mb = 0; }
        else if (leafKind == BK_TEXT && !stack.empty() && stack.back().type == C_LI) { mt = 16; }  // li > p
        Block& b = Emit(leafKind, mt, mb);
        b.heading = leafHeading;
        if (leafHeading == 6) b.muted = 1;
        b.textOff = tStart;
        b.textLen = tEnd - tStart;
        b.runOff = rStart;
        b.runCount = (uint32_t)d.runs.size() - rStart;
        uint32_t extBeg = ext.beg;  // a fenced block's fence character (LeafSrc clears the extent)
        if (map) {
            bool empty = emptyItem;
            LeafSrc();
            BlockSrc& bs = d.blockSrc.back();
            if (alertBeg != UINT32_MAX && alertBeg > bs.beg && alertBeg <= bs.end) {  // the content after the tag
                bs.beg = alertBeg;
                bs.line = LineStart(alertBeg);
            }
            if ((bs.flags & BS_SYNTH) || (empty && (bs.flags & BS_EMPTYITEM))) DropMap(segStart, spanStart);
            else ClaimMap(segStart, spanStart);
        }
        if (leafKind == BK_CODE) {
            uint8_t lang = 0;
            // a Mermaid fence is a diagram, not code: the source stays as the placeholder until it is drawn
            if (mermaid) {
                int idx = AddMathImage(std::wstring(d.text.data() + tStart, tEnd - tStart), 3);
                d.runs.resize(rStart);
                b.kind = BK_IMAGE;
                b.aux = (uint32_t)idx;
                b.runOff = rStart;
                b.runCount = 0;
                if (map) {  // an object atom: the diagram's source is its content lines
                    DropMap(segStart, spanStart);
                    BlockSrc& bs = d.blockSrc.back();
                    bs.flags |= BS_OBJECT;
                    Image& im = d.images[idx];
                    im.outerBeg = (bs.flags & BS_FENCED) ? extBeg : bs.beg;
                    im.outerEnd = bs.outerEnd;
                    im.srcBeg = bs.beg;
                    im.srcEnd = bs.end;
                }
                return;
            }
            Highlight(d, codeLang, codeLangLen, tStart, tEnd - tStart, lang);
            b.runCount = (uint32_t)d.runs.size() - rStart;
            b.lang = lang;
            if (codeLang && codeLangLen) {  // the name as typed, shown in the corner of the block
                std::wstring name(codeLang, codeLangLen);
                size_t sp = name.find_first_of(L" \t{");  // ```js title="x"
                if (sp != std::wstring::npos) name.resize(sp);
                if (!name.empty() && name.size() <= 24) {
                    d.langNames.push_back(name);
                    b.aux = (uint32_t)d.langNames.size();
                }
            }
        }
        if (leafHeading) {
            std::wstring slug = GithubSlug(d.text.data() + tStart, tEnd - tStart);
            int& cnt = slugCount[slug];
            if (cnt) slug += L"-" + std::to_wstring(cnt);
            cnt++;
            d.headings.push_back(Heading{(uint32_t)d.blocks.size() - 1, leafHeading, std::move(slug)});
        }
        leafIsLiImplicit = false;
    }

    // implicit text leaf: md4c sends the text of tight list items and of footnote definitions without a P block
    void EnsureLeaf() {
        if (collecting || inCell) return;
        if (fnId || (!stack.empty() && stack.back().type == C_LI)) {
            StartLeaf(BK_TEXT, 0);
            leafIsLiImplicit = true;
        }
    }

    void AppendText(const MD_CHAR* s, MD_SIZE n, bool isEntity) {
        EnsureLeaf();
        if (!collecting && !inCell) return;
        if (img) {  // alt text belongs to the picture, not to the line it stands in
            if (isEntity) AppendEntity(imgAlt, s, n);
            else imgAlt.append(s, n);
            return;
        }
        uint32_t start = (uint32_t)d.text.size();
        bool inSrc = srcBase && s >= srcBase && s < srcEnd;
        if (inSrc && !map)  // remember where this chunk came from in the source (reading mode: "copy as Markdown")
            d.srcMap.emplace_back(start, (uint32_t)(s - srcBase));
        bool raw = code || (leafKind == BK_CODE && collecting);  // code keeps :colons: and every character as typed
        if (isEntity) AppendEntity(d.text, s, n);
        else if (raw) d.text.append(s, n);
        else AppendWithEmoji(s, n, inSrc);
        uint32_t len = (uint32_t)d.text.size() - start;
        if (map) MapChunk(start, len, s, n, inSrc, isEntity, raw);
        if (!len) return;
        if (img) return;  // alt text: not styled as a run, but kept (shown if not an image block)
        if (!paraOther) {
            for (uint32_t i = start; i < start + len; i++)
                if (d.text[i] != L' ' && d.text[i] != L'\n' && d.text[i] != L'\t') { paraOther = true; break; }
        }
        uint16_t fl = (bold ? F_BOLD : 0) | (italic ? F_ITALIC : 0) | (code ? F_CODE : 0) | (strike ? F_STRIKE : 0) |
                      (link ? F_LINK : 0) | (kbd ? F_KBD : 0) | (sup ? F_SUP : 0) | (sub ? F_SUB : 0);
        if (leafKind == BK_CODE && collecting) fl = 0;
        if (!fl) return;
        uint8_t color = (fl & F_LINK) ? P_LINK : (fl & F_STRIKE) ? P_MUTED : P_DEFAULT;
        uint32_t lnk = (fl & F_LINK) ? curLink : 0;
        if (!d.runs.empty() && d.runs.size() > rStart) {
            Run& r = d.runs.back();
            if (r.start + r.len == start && r.flags == fl && r.color == color && r.link == lnk) {
                r.len += len;
                return;
            }
        }
        d.runs.push_back(Run{start, len, fl, color, 0, lnk});
    }

    // --------------------------------------------------------------------------------------- HTML (plan 2.2)
    bool TexOk() {
        if (texOk < 0) texOk = TexAvailable() ? 1 : 0;
        return texOk == 1;
    }
    bool MermaidOk() {
        if (mermaidOk < 0) mermaidOk = MermaidAvailable() ? 1 : 0;
        return mermaidOk == 1;
    }

    // A formula or a diagram: a picture whose source is text. The size here is only a guess to leave room in the
    // first frame - the real one arrives with the drawing (loader.cpp) and the document re-flows, as it does for a
    // picture from the network. kind: 1 = formula in the line, 2 = formula of its own, 3 = Mermaid diagram.
    int AddMathImage(const std::wstring& src, uint8_t kind) {
        Image image;
        int n = WideCharToMultiByte(CP_UTF8, 0, src.data(), (int)src.size(), nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            image.math.resize(n);
            WideCharToMultiByte(CP_UTF8, 0, src.data(), (int)src.size(), image.math.data(), n, nullptr, nullptr);
        }
        image.mathKind = kind;
        image.alt = src;
        const float fs = 16.f;  // the default text size: the parser knows nothing about settings, and this is a guess
        if (kind == 3) {
            size_t lines = 1 + (size_t)std::count(src.begin(), src.end(), (wchar_t)10);
            image.w = 440;
            image.h = (int)std::max(180.f, std::min(720.f, lines * fs * 1.5f + 32.f));
        } else {
            float chars = (float)std::min<size_t>(src.size(), 160);
            image.w = (int)std::clamp(chars * fs * 0.42f, fs, 900.f);
            image.h = (int)(kind == 2 ? fs * 2.4f : fs * 1.3f);
        }
        d.themed = true;  // the colour of a formula follows the theme, so a switch re-reads the document
        d.images.push_back(std::move(image));
        return (int)d.images.size() - 1;
    }

    // the gathered formula source becomes a picture in the line (or a block of its own, if it stands alone)
    void EmitMath() {
        if (mathSrc.empty()) return;
        int idx = AddMathImage(mathSrc, mathDisplay ? 2 : 1);
        mathSrc.clear();
        if (!collecting && !inCell) return;
        uint32_t start = (uint32_t)d.text.size();
        d.text.push_back(L'\xFFFC');
        uint16_t fl = (uint16_t)(F_IMAGE | (link ? F_LINK : 0));
        d.runs.push_back(Run{start, 1, fl, P_DEFAULT, 0, link ? curLink : 0, (uint32_t)idx});
        if (mathDisplay) {  // a formula on its own line: the paragraph around it becomes a picture block
            paraImage = idx;
            paraImages++;
        } else {
            paraOther = true;
        }
    }

    int AddImage(const std::wstring& src, int w, int h) {
        Image image;
        bool remote = src.find(L"://") != std::wstring::npos || src.rfind(L"data:", 0) == 0;
        if (remote && src.rfind(L"data:", 0) != 0) image.url = src;  // fetched after the first frame (loader.cpp)
        if (!remote && !src.empty()) {
            std::wstring p;
            for (size_t i = 0; i < src.size(); i++) {  // %xx decoding + slashes
                wchar_t c = src[i];
                if (c == L'%' && i + 2 < src.size() && iswxdigit(src[i + 1]) && iswxdigit(src[i + 2])) {
                    p.push_back((wchar_t)wcstol(src.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else {
                    p.push_back(c == L'/' ? L'\\' : c);
                }
            }
            image.path = (p.size() > 1 && p[1] == L':') ? p : d.baseDir + p;
        }
        image.attrW = w > 0 ? w : 0;  // HTML width / height: honoured like a browser does
        image.attrH = h > 0 ? h : 0;
        d.images.push_back(std::move(image));
        return (int)d.images.size() - 1;
    }

    // tags that only change the look of the text around them
    void HtmlInline(const HtmlTag& t) {
        auto adj = [&](int& c) {
            if (t.closing) { if (c > 0) c--; }
            else if (!t.selfClose) c++;
        };
        const std::wstring& n = t.name;
        if (n == L"br") { AppendText(L"\n", 1, false); return; }
        if (n == L"picture") {
            inPicture = !t.closing;
            pictureSrc.clear();
            return;
        }
        if (n == L"source" && inPicture) {  // <source media="(prefers-color-scheme: dark)" srcset="…">
            const std::wstring* media = t.Attr(L"media");
            const std::wstring* set = t.Attr(L"srcset");
            if (!set || set->empty()) return;
            std::wstring m = media ? ToLower(*media) : L"";
            bool wantDark = m.find(L"dark") != std::wstring::npos;
            bool wantLight = m.find(L"light") != std::wstring::npos;
            if ((wantDark && PaletteIsDark()) || (wantLight && !PaletteIsDark()) || (!wantDark && !wantLight)) {
                std::wstring first = set->substr(0, set->find_first_of(L" ,"));
                if (!first.empty() && (pictureSrc.empty() || wantDark || wantLight)) pictureSrc = first;
            }
            d.themed = true;  // the picture depends on the theme: a theme switch re-reads the document
            return;
        }
        if (n == L"img") {  // a picture inside the line, like a browser: badges, icons, logos
            const std::wstring* src = t.Attr(L"src");
            if (t.closing) return;
            std::wstring chosen = inPicture && !pictureSrc.empty() ? pictureSrc : (src ? *src : std::wstring());
            if (chosen.empty()) return;
            EnsureLeaf();
            if (!collecting && !inCell) StartLeaf(BK_TEXT, 0);
            int idx = AddImage(chosen, t.AttrInt(L"width"), t.AttrInt(L"height"));
            uint32_t start = (uint32_t)d.text.size();
            d.text.push_back(L'\xFFFC');  // object replacement character: the line box holds the picture
            uint16_t fl = (uint16_t)(F_IMAGE | (link ? F_LINK : 0));
            d.runs.push_back(Run{start, 1, fl, P_DEFAULT, 0, link ? curLink : 0, (uint32_t)idx});
            paraOther = true;
            return;
        }
        if (n == L"b" || n == L"strong") adj(bold);
        else if (n == L"i" || n == L"em" || n == L"cite" || n == L"var") adj(italic);
        else if (n == L"code" || n == L"tt" || n == L"samp") adj(code);
        else if (n == L"del" || n == L"s" || n == L"strike") adj(strike);
        else if (n == L"kbd") adj(kbd);
        else if (n == L"sup") adj(sup);
        else if (n == L"sub") adj(sub);
        else if (n == L"a") {
            if (t.closing) {
                if (link > 0) link--;
                if (!linkStack.empty()) linkStack.pop_back();
                curLink = linkStack.empty() ? 0 : linkStack.back();
            } else {
                if (const std::wstring* id = t.Attr(L"name")) pendAnchors.push_back(*id);
                else if (const std::wstring* id2 = t.Attr(L"id")) pendAnchors.push_back(*id2);
                if (const std::wstring* href = t.Attr(L"href")) {
                    d.links.push_back(*href);
                    curLink = (uint32_t)d.links.size() - 1;
                    linkStack.push_back(curLink);
                    link++;
                }
            }
        }
    }

    uint8_t HtmlAlign(const HtmlTag& t) {
        if (t.name == L"center") return 1;
        if (const std::wstring* a = t.Attr(L"align")) {
            std::wstring v = ToLower(*a);
            return v == L"center" ? 1 : v == L"right" ? 2 : 0;
        }
        if (const std::wstring* st = t.Attr(L"style")) {
            std::wstring v = ToLower(*st);
            if (v.find(L"center") != std::wstring::npos) return 1;
            if (v.find(L"right") != std::wstring::npos) return 2;
        }
        return 0;
    }

    void PushAlign(uint8_t a) {
        alignStack.push_back(a ? a : pendAlign);  // a container without align keeps what it is inside
        pendAlign = alignStack.back();
    }
    void PopAlign() {
        if (!alignStack.empty()) alignStack.pop_back();
        pendAlign = alignStack.empty() ? 0 : alignStack.back();
    }

    void EndHtmlLeaf() {  // HTML leaves keep no trailing space from the source layout
        if (collecting) {
            while (d.text.size() > tStart && d.text.back() == L' ') {
                d.text.pop_back();
                if (d.runs.size() > rStart) {
                    Run& r = d.runs.back();
                    if (r.start + r.len > d.text.size()) r.len = (uint32_t)(d.text.size() - r.start);
                }
            }
        }
        EndLeaf();
    }

    static bool IsHtmlContainer(const std::wstring& n) {
        static const wchar_t* kNames[] = {L"p",      L"div",   L"center",  L"section", L"article", L"summary",
                                          L"figure", L"figcaption", L"blockquote", L"ul", L"ol", L"li",
                                          L"tr",     L"td",    L"th",      L"table",   L"tbody",   L"thead",
                                          L"details", L"body",    L"html",    L"main", L"header",
                                          L"footer", L"nav",   L"dl",      L"dt",      L"dd"};
        for (const wchar_t* k : kNames)
            if (n == k) return true;
        return false;
    }

    void EmitHtmlTable() {
        inHtmlTable = false;
        inCell = false;
        size_t cols = 0;
        for (auto& r : htmlRows) cols = std::max(cols, r.size());
        if (!cols) { htmlRows.clear(); return; }
        Table tb{};
        tb.cols = (uint32_t)cols;
        tb.rows = (uint32_t)htmlRows.size();
        tb.cellOff = (uint32_t)d.cells.size();
        tb.alignOff = (uint32_t)d.aligns.size();
        d.cells.resize(d.cells.size() + cols * htmlRows.size(), Cell{0, 0, 0, 0});
        d.aligns.resize(d.aligns.size() + cols, 0);
        uint32_t first = UINT32_MAX, last = 0;
        for (size_t r = 0; r < htmlRows.size(); r++) {
            for (size_t c = 0; c < htmlRows[r].size(); c++) {
                const Cell& cell = htmlRows[r][c];
                d.cells[tb.cellOff + r * cols + c] = cell;
                first = std::min(first, cell.textOff);
                last = std::max(last, cell.textOff + cell.textLen);
            }
        }
        for (size_t k = 0; k < cols * htmlRows.size(); k++) {  // rows shorter than the widest: empty cells at the end
            Cell& c = d.cells[tb.cellOff + k];
            if (!c.textLen && !c.textOff) { c.textOff = last; c.runOff = (uint32_t)d.runs.size(); }
        }
        d.tables.push_back(tb);
        Block& b = Emit(BK_TABLE, 0, 16);
        b.aux = (uint32_t)d.tables.size() - 1;
        b.textOff = first == UINT32_MAX ? (uint32_t)d.text.size() : first;
        b.textLen = last > b.textOff ? last - b.textOff : 0;
        htmlRows.clear();
    }

    void HtmlBlockTag(const HtmlTag& t) {
        const std::wstring& n = t.name;
        if (n.size() == 2 && n[0] == L'h' && n[1] >= L'1' && n[1] <= L'6') {  // <h2>…</h2> is a heading like ##
            EndHtmlLeaf();
            if (t.closing) {
                PopAlign();
            } else {
                PushAlign(HtmlAlign(t));
                StartLeaf(BK_TEXT, (uint8_t)(n[1] - L'0'));
            }
            return;
        }
        if (n == L"table") {
            EndHtmlLeaf();
            if (t.closing) EmitHtmlTable();
            else { htmlRows.clear(); inHtmlTable = true; inCell = false; }
            return;
        }
        if (inHtmlTable && (n == L"tr" || n == L"td" || n == L"th")) {
            if (n == L"tr") {
                if (!t.closing) htmlRows.emplace_back();
                return;
            }
            if (t.closing) {  // finish the cell: its text range and runs become a table cell
                if (!inCell) return;
                inCell = false;
                uint32_t end = (uint32_t)d.text.size();
                while (end > htmlCellStart && d.text[end - 1] == L' ') end--;
                d.text.resize(end);
                if (htmlRows.empty()) htmlRows.emplace_back();
                htmlRows.back().push_back(Cell{htmlCellStart, end - htmlCellStart, htmlCellRun,
                                               (uint32_t)d.runs.size() - htmlCellRun});
                bold = italic = code = strike = kbd = sup = sub = 0;
                link = 0;
                linkStack.clear();
                return;
            }
            if (htmlRows.empty()) htmlRows.emplace_back();
            inCell = true;  // AppendText puts the text and its runs straight into the cell's range
            htmlCellStart = (uint32_t)d.text.size();
            htmlCellRun = (uint32_t)d.runs.size();
            if (n == L"th") bold++;
            return;
        }
        if (n == L"details") {  // a block folded away until its summary is clicked
            EndHtmlLeaf();
            if (t.closing) {
                curDetails = 0;
                inSummary = false;
                PopAlign();
            } else {
                d.detailsOpen.push_back(t.Attr(L"open") ? 1 : 0);
                curDetails = (uint16_t)d.detailsOpen.size();
                PushAlign(HtmlAlign(t));
            }
            return;
        }
        if (n == L"summary") {
            EndHtmlLeaf();
            inSummary = !t.closing;
            if (t.closing) PopAlign();
            else PushAlign(HtmlAlign(t));
            return;
        }
        if (IsHtmlContainer(n)) {
            EndHtmlLeaf();
            if (t.closing) PopAlign();
            else if (!t.selfClose) PushAlign(HtmlAlign(t));
            return;
        }
        if (n == L"hr") { EndHtmlLeaf(); Emit(BK_HR, 8, 24); return; }
        HtmlInline(t);
    }

    // tags whose contents are never shown, let alone run
    static bool IsHtmlHidden(const std::wstring& n) {
        return n == L"script" || n == L"style" || n == L"iframe" || n == L"noscript" || n == L"template" ||
               n == L"object" || n == L"embed" || n == L"svg" || n == L"head";
    }

    // one HTML block: walk the tags in order, keeping the text and dropping what we do not know
    void EmitHtmlBlock() {
        uint8_t saved = pendAlign;
        size_t alignDepth = alignStack.size();
        const std::wstring& h = htmlRaw;
        for (size_t i = 0; i < h.size();) {
            if (h[i] == L'<') {
                HtmlTag t;
                size_t used = ParseHtmlTag(h.data() + i, h.size() - i, t);
                if (used) {
                    i += used;
                    if (t.name.empty()) continue;  // comment
                    if (!t.closing && !t.selfClose && IsHtmlHidden(t.name)) {
                        std::wstring close = L"</" + t.name;
                        size_t end = h.find(close, i);
                        i = end == std::wstring::npos ? h.size() : end + close.size();
                        while (i < h.size() && h[i] != L'>') i++;
                        if (i < h.size()) i++;
                        continue;
                    }
                    HtmlBlockTag(t);
                    continue;
                }
            }
            // A '<' that is not the start of a tag (an unfinished one at the end of the block, say) is text like any
            // other character - and stepping over it is what keeps this loop moving. Found by fuzzing: without the
            // step, "<p align=center>\n<i>text<" spun forever.
            size_t j = i + (h[i] == L'<' ? 1 : 0);
            while (j < h.size() && h[j] != L'<') j++;
            std::wstring txt;
            AppendHtmlText(txt, h.data() + i, j - i);
            i = j;
            std::wstring out;  // HTML folds every run of spaces and line breaks into one space
            for (wchar_t c : txt) {
                bool space = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
                if (!space) out.push_back(c);
                else if (!out.empty() && out.back() != L' ') out.push_back(L' ');
                else if (out.empty() && collecting && d.text.size() > tStart && d.text.back() != L' ') out.push_back(L' ');
            }
            if (out.empty() || (!collecting && !inCell && out == L" ")) continue;
            if (!collecting && !inCell) {
                if (out.front() == L' ') out.erase(0, 1);
                if (out.empty()) continue;
                StartLeaf(BK_TEXT, 0);
            }
            AppendText(out.data(), (MD_SIZE)out.size(), false);
        }
        EndHtmlLeaf();
        while (alignStack.size() > alignDepth) alignStack.pop_back();
        pendAlign = saved;
        bold = italic = code = strike = kbd = sup = sub = 0;  // an unclosed tag must not leak into the next block
        link = 0;
        linkStack.clear();
    }

    // --------------------------------------------------------------------------------------- containers (map mode)
    void PushContainer(ContainerSrc c) {
        c.parent = contStack.empty() ? -1 : contStack.back();
        c.firstBlock = (uint32_t)d.blocks.size();
        d.containers.push_back(c);
        contStack.push_back((int32_t)d.containers.size() - 1);
    }
    void PopContainer() {
        if (contStack.empty()) return;
        ContainerSrc& c = d.containers[contStack.back()];
        if (d.blocks.size() > c.firstBlock) {
            c.lastBlock = (uint32_t)d.blocks.size() - 1;
        } else {  // held no block
            c.firstBlock = UINT32_MAX;
            c.lastBlock = 0;
        }
        contStack.pop_back();
    }
    // A list item: md4c (patched) hands over its marker's offset, or for a task item the offset of the mark between
    // the brackets - then the marker is found by walking back from '[' over the blanks.
    void PushItem(const MD_BLOCK_LI_DETAIL* li, bool tight) {
        ContainerSrc c;
        c.kind = CT_ITEM;
        c.tight = tight;
        uint32_t n = SrcLen(), off = mdBase + (uint32_t)li->task_mark_offset, m = off;
        if (li->is_task && off >= 2 && off < n) {
            c.taskOff = off;
            m = off - 2;  // before the '['
            while (m > 0 && Blank(srcBase[m])) m--;
            if (srcBase[m] == L'.' || srcBase[m] == L')') {  // an ordered marker: back over its digits
                uint32_t q = m;
                while (q > 0 && srcBase[q - 1] >= L'0' && srcBase[q - 1] <= L'9') q--;
                m = q;
            }
        }
        if (m < n) {
            c.markOff = m;
            wchar_t ch = srcBase[m];
            if (ch == L'-' || ch == L'+' || ch == L'*') {
                c.bullet = ch;
                c.markLen = 1;
            } else {
                uint32_t q = m, num = 0;
                while (q < n && srcBase[q] >= L'0' && srcBase[q] <= L'9' && q - m < 9) num = num * 10 + (srcBase[q++] - L'0');
                c.number = num;
                c.delim = q < n ? srcBase[q] : 0;
                c.markLen = (uint8_t)(q - m + 1);
            }
            // the content column: the marker's column + its length + the blanks after it (1-4; five or more, or
            // none before the line end, count as one) - md4c's own rule
            uint32_t ls = LineStart(m), col = 0;
            for (uint32_t p = ls; p < m; p++) col = srcBase[p] == L'\t' ? (col + 4) & ~3u : col + 1;
            col += c.markLen;
            uint32_t p = m + c.markLen, cb = col;
            while (p < n && Blank(srcBase[p])) { cb = srcBase[p] == L'\t' ? (cb + 4) & ~3u : cb + 1; p++; }
            uint32_t blanks = cb - col;
            bool eol = p >= n || srcBase[p] == L'\n' || srcBase[p] == L'\r';
            c.contentCol = (uint16_t)(col + ((eol || blanks >= 5 || blanks == 0) ? 1 : blanks));
        }
        PushContainer(c);
    }

    // --------------------------------------------------------------------------------------- blocks
    int Enter(MD_BLOCKTYPE t, void* det) {
        switch (t) {
        case MD_BLOCK_DOC: break;
        case MD_BLOCK_QUOTE:
        case MD_BLOCK_ADMONITION: {  // md4c reports > [!NOTE] … as an admonition; a plain quote has no alert
            EndLeaf();
            uint8_t alert = AL_NONE;
            if (t == MD_BLOCK_ADMONITION) {
                const MD_ATTRIBUTE& ty = ((MD_BLOCK_ADMONITION_DETAIL*)det)->type;
                for (size_t a = 0; a < std::size(kAlerts); a++)
                    if (ty.size == wcslen(kAlerts[a].title) && _wcsnicmp(ty.text, kAlerts[a].title, ty.size) == 0)
                        alert = (uint8_t)(a + 1);
            }
            QuoteSpan q{indent, (uint32_t)d.blocks.size(), UINT32_MAX, alert};
            d.quotes.push_back(q);
            stack.push_back(Ctx{C_QUOTE, false, false, 0, 0, (int)d.quotes.size() - 1});
            indent += 20.f;  // 4 px bar + 16 px padding (GitHub: padding 0 1em, border-left .25em)
            quoteDepth++;
            if (map) {
                ContainerSrc c;
                c.kind = t == MD_BLOCK_ADMONITION ? CT_ALERT : CT_QUOTE;
                PushContainer(c);
            }
            EmitAlertTitle(alert);
            break;
        }
        // footnotes: the definitions md4c collected at the end of the document, numbered and linked back
        case MD_BLOCK_FOOTNOTE_DEF_SECTION: EndLeaf(); Emit(BK_HR, 24, 16); break;
        case MD_BLOCK_FOOTNOTE_DEF: {
            EndLeaf();
            fnId = ((MD_BLOCK_FOOTNOTE_DEF_DETAIL*)det)->id;
            pendMarker = MK_NUMBER;
            pendNumber = fnId;
            pendLevel = 1;
            pendAnchors.push_back(L"fn-" + std::to_wstring(fnId));
            indent += 32.f;
            if (map) {
                ContainerSrc c;
                c.kind = CT_FOOTNOTE;
                c.markOff = ext.fnDef;
                PushContainer(c);
            }
            break;
        }
        case MD_BLOCK_UL:
        case MD_BLOCK_OL: {
            EndLeaf();
            bool nested = !stack.empty() && stack.back().type == C_LI;
            Ctx c{};
            c.type = t == MD_BLOCK_UL ? C_UL : C_OL;
            c.nested = nested;
            if (t == MD_BLOCK_UL) c.tight = ((MD_BLOCK_UL_DETAIL*)det)->is_tight != 0;
            else { auto* o = (MD_BLOCK_OL_DETAIL*)det; c.tight = o->is_tight != 0; c.next = o->start; }
            stack.push_back(c);
            indent += 32.f;  // padding-left: 2em
            listDepth++;
            break;
        }
        case MD_BLOCK_LI: {
            EndLeaf();
            auto* li = (MD_BLOCK_LI_DETAIL*)det;
            Ctx& list = stack.back();
            if (list.items++ > 0) Margin(4);  // li + li { margin-top: .25em }
            pendLevel = (uint8_t)listDepth;
            if (li->is_task) {
                pendMarker = (li->task_mark == L' ') ? MK_TASK_OPEN : MK_TASK_DONE;
                pendTask = mdBase + (uint32_t)li->task_mark_offset;
            } else if (list.type == C_OL) { pendMarker = MK_NUMBER; pendNumber = list.next++; }
            else pendMarker = MK_BULLET;
            stack.push_back(Ctx{C_LI, list.tight, false, 0, 0, -1});
            if (map) PushItem(li, list.tight);
            break;
        }
        case MD_BLOCK_HR:
            EndLeaf();
            Emit(BK_HR, 8, 24);
            if (map) LeafSrc();  // an object atom with no text
            break;
        case MD_BLOCK_H: EndLeaf(); StartLeaf(BK_TEXT, (uint8_t)((MD_BLOCK_H_DETAIL*)det)->level); break;
        case MD_BLOCK_CODE: {
            EndLeaf();
            auto* c = (MD_BLOCK_CODE_DETAIL*)det;
            codeLang = c->lang.text;
            codeLangLen = c->lang.size;
            StartLeaf(BK_CODE, 0);
            break;
        }
        case MD_BLOCK_HTML:  // gathered, then walked at leave
            EndLeaf();
            htmlRaw.clear();
            inHtmlBlock = true;
            if (map) {  // every block the walk makes shares this record (an object atom edited as source)
                htmlExt = ext;
                ext.set = false;
                htmlFirst = (uint32_t)d.blocks.size();
            }
            break;
        case MD_BLOCK_P: EndLeaf(); StartLeaf(BK_TEXT, 0); break;
        case MD_BLOCK_TABLE: {
            EndLeaf();
            auto* td = (MD_BLOCK_TABLE_DETAIL*)det;
            Table tb{};
            tb.cols = td->col_count;
            tb.rows = td->head_row_count + td->body_row_count;
            tb.cellOff = (uint32_t)d.cells.size();
            tb.alignOff = (uint32_t)d.aligns.size();
            d.cells.resize(d.cells.size() + (size_t)tb.cols * tb.rows, Cell{0, 0, 0, 0});
            d.aligns.resize(d.aligns.size() + tb.cols, 0);
            d.tables.push_back(tb);
            tIndex = (int)d.tables.size() - 1;
            row = 0;
            col = 0;
            if (map) {  // the cells' segments and spans belong to the table block
                segStart = (uint32_t)d.segs.size();
                spanStart = (uint32_t)d.spans.size();
                d.cellSrc.resize(d.cells.size(), CellSrc{0, 0, true});
                d.tableSrc.resize(d.tables.size());
            }
            break;
        }
        case MD_BLOCK_THEAD: case MD_BLOCK_TBODY: break;
        case MD_BLOCK_TR: col = 0; break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            inCell = true;
            tStart = (uint32_t)d.text.size();
            rStart = (uint32_t)d.runs.size();
            if (row == 0 && tIndex >= 0) {
                Table& tb = d.tables[tIndex];
                if (col < tb.cols) d.aligns[tb.alignOff + col] = (uint8_t)((MD_BLOCK_TD_DETAIL*)det)->align;
            }
            if (map && tIndex >= 0) {
                mdCell = true;
                openSpans.clear();
                srcCur = cellExt.missing ? srcCur : cellExt.beg;
            }
            break;
        }
        default: break;
        }
        return 0;
    }

    int Leave(MD_BLOCKTYPE t, void*) {
        switch (t) {
        case MD_BLOCK_QUOTE:
        case MD_BLOCK_ADMONITION: {
            EndLeaf();
            Ctx c = stack.back();
            stack.pop_back();
            QuoteSpan& q = d.quotes[c.quote];
            q.last = d.blocks.size() > q.first ? (uint32_t)d.blocks.size() - 1 : UINT32_MAX;
            if (q.alert && q.last != UINT32_MAX)  // alerts keep the normal text colour (plain quotes are muted)
                for (uint32_t k = q.first; k <= q.last; k++) d.blocks[k].muted = quoteDepth > 1;
            indent -= 20.f;
            quoteDepth--;
            Margin(16);
            if (map) PopContainer();
            break;
        }
        case MD_BLOCK_UL:
        case MD_BLOCK_OL: {
            EndLeaf();
            bool nested = stack.back().nested;
            stack.pop_back();
            indent -= 32.f;
            listDepth--;
            if (!nested) Margin(16);
            break;
        }
        case MD_BLOCK_LI: {
            EndLeaf();
            if (pendMarker != MK_NONE) {  // empty item: still show its marker
                StartLeaf(BK_TEXT, 0);
                leafIsLiImplicit = true;
                emptyItem = true;
                EndLeaf();
                emptyItem = false;
            }
            stack.pop_back();
            if (map) PopContainer();
            break;
        }
        case MD_BLOCK_H: case MD_BLOCK_CODE: case MD_BLOCK_P: EndLeaf(); break;
        case MD_BLOCK_FOOTNOTE_DEF: {
            EndLeaf();
            indent -= 32.f;
            // back to the place that referenced it: an arrow appended to the definition's last line
            if (fnId && !d.blocks.empty()) {
                Block& b = d.blocks.back();
                if (b.kind == BK_TEXT && b.textOff + b.textLen == d.text.size()) {
                    uint32_t start = (uint32_t)d.text.size();
                    d.text += L" \x21A9";  // ↩
                    d.links.push_back(L"#fnref-" + std::to_wstring(fnId));
                    d.runs.push_back(Run{start + 1, 1, F_LINK, P_LINK, 0, (uint32_t)d.links.size() - 1});
                    b.textLen += 2;
                    b.runCount = (uint32_t)d.runs.size() - b.runOff;
                    if (map) {  // synthesized text: it has no source and is never a caret stop
                        BlockSrc& bs = d.blockSrc.back();
                        if (!(bs.flags & (BS_SYNTH | BS_RAW | BS_OBJECT)) && bs.segOff + bs.segCount == d.segs.size()) {
                            d.segs.push_back(SrcSeg{start, 2, bs.end, 0, SEG_SYNTH, 0});
                            bs.segCount++;
                        }
                    }
                }
            }
            fnId = 0;
            Margin(4);
            if (map) {
                PopContainer();
                ext.set = false;  // a definition with no text leaves its extent unused
            }
            break;
        }
        case MD_BLOCK_HTML:
            inHtmlBlock = false;
            noSegs++;
            EmitHtmlBlock();
            noSegs--;
            if (map) {
                BlockSrc hs;
                if (htmlExt.set) {
                    hs.beg = htmlExt.beg;
                    hs.end = htmlExt.end;
                    hs.line = LineStart(hs.beg);
                    hs.lineEnd = hs.outerEnd = LineEnd(hs.end);
                    hs.flags = BS_RAW | BS_HTML;
                } else {
                    hs.flags = BS_SYNTH;
                }
                hs.rawId = (int32_t)htmlFirst;
                hs.segOff = (uint32_t)d.segs.size();
                hs.spanOff = (uint32_t)d.spans.size();
                for (size_t k = htmlFirst; k < d.blockSrc.size(); k++) {
                    hs.container = d.blockSrc[k].container;
                    d.blockSrc[k] = hs;
                }
                htmlExt.set = false;
            }
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            inCell = false;
            if (tIndex >= 0) {
                Table& tb = d.tables[tIndex];
                if (row < tb.rows && col < tb.cols) {
                    uint32_t tEnd = (uint32_t)d.text.size();
                    if (t == MD_BLOCK_TH) {  // header cells: bold over the whole cell
                        d.runs.resize(rStart);
                        if (tEnd > tStart) d.runs.push_back(Run{tStart, tEnd - tStart, F_BOLD, P_DEFAULT, 0, 0});
                    }
                    d.cells[tb.cellOff + row * tb.cols + col] =
                        Cell{tStart, tEnd - tStart, rStart, (uint32_t)d.runs.size() - rStart};
                    if (map) {
                        CutOpenSpans(cellExt.missing ? srcCur : cellExt.end);
                        d.cellSrc[tb.cellOff + row * tb.cols + col] = cellExt;
                    }
                }
                mdCell = false;
                cellExt = CellSrc{0, 0, true};
            }
            col++;
            break;
        }
        case MD_BLOCK_TR: row++; break;
        case MD_BLOCK_TABLE: {
            const Table& tb = d.tables[tIndex];
            uint32_t first = (uint32_t)d.text.size(), last = first;
            if (tb.rows * tb.cols) {
                first = d.cells[tb.cellOff].textOff;
                const Cell& lc = d.cells[tb.cellOff + tb.rows * tb.cols - 1];
                last = std::max(first, lc.textOff + lc.textLen);
            }
            Block& b = Emit(BK_TABLE, 0, 16);
            b.aux = (uint32_t)tIndex;
            b.textOff = first;
            b.textLen = last - first;
            if (map) {
                LeafSrc();
                ClaimMap(segStart, spanStart);
                TableRows(tIndex);
            }
            tIndex = -1;
            break;
        }
        default: break;
        }
        return 0;
    }

    // Map mode: each row line of a Markdown table - where it starts, where its content starts (after the container
    // prefix), where it ends and where its pipes are. md4c reports cells only, so a row is found from its first present
    // cell; the delimiter row, which md4c never reports, is the line after the header, split on '|'. A cell a short
    // row lacks gets its row's end as its place.
    void TableRows(int ti) {
        const Table& tb = d.tables[ti];
        TableSrc& ts = d.tableSrc[ti];
        ts.rows.clear();
        uint32_t prevEnd = d.blockSrc.back().beg;
        auto rowOf = [&](uint32_t r) {
            RowSrc rs{UINT32_MAX, 0, 0, {}};
            for (uint32_t c = 0; c < tb.cols; c++) {
                const CellSrc& cs = d.cellSrc[tb.cellOff + r * tb.cols + c];
                if (cs.missing) continue;
                if (rs.lineStart == UINT32_MAX) {
                    rs.lineStart = LineStart(cs.beg);
                    rs.lineEnd = LineEnd(cs.beg);
                    uint32_t p = cs.beg;
                    while (p > rs.lineStart && Blank(srcBase[p - 1])) p--;
                    if (p > rs.lineStart && srcBase[p - 1] == L'|') {  // the leading pipe
                        rs.contentStart = p - 1;
                        rs.pipes.push_back(p - 1);
                    } else {
                        rs.contentStart = cs.beg;
                    }
                }
                uint32_t q = cs.end;  // the separator (or trailing pipe) after the cell
                while (q < rs.lineEnd && Blank(srcBase[q])) q++;
                if (q < rs.lineEnd && srcBase[q] == L'|' && (rs.pipes.empty() || rs.pipes.back() < q)) rs.pipes.push_back(q);
            }
            if (rs.lineStart == UINT32_MAX) {  // no cell to find the line by: the line after the previous row
                rs.lineStart = rs.contentStart = NextLine(prevEnd);
                rs.lineEnd = LineEnd(rs.lineStart);
            }
            for (uint32_t c = 0; c < tb.cols; c++) {
                CellSrc& cs = d.cellSrc[tb.cellOff + r * tb.cols + c];
                if (cs.missing) cs.beg = cs.end = rs.lineEnd;
            }
            prevEnd = rs.lineEnd;
            ts.rows.push_back(std::move(rs));
        };
        if (!tb.rows) return;
        rowOf(0);
        RowSrc delim{NextLine(ts.rows[0].lineEnd), 0, 0, {}};
        delim.lineEnd = LineEnd(delim.lineStart);
        delim.contentStart = delim.lineStart;
        while (delim.contentStart < delim.lineEnd && (Blank(srcBase[delim.contentStart]) || srcBase[delim.contentStart] == L'>'))
            delim.contentStart++;
        for (uint32_t p = delim.contentStart; p < delim.lineEnd; p++)
            if (srcBase[p] == L'|') delim.pipes.push_back(p);
        prevEnd = delim.lineEnd;
        ts.rows.push_back(std::move(delim));
        for (uint32_t r = 1; r < tb.rows; r++) rowOf(r);
    }

    int SpanEnter(MD_SPANTYPE t, void* det) {
        EnsureLeaf();
        switch (t) {
        case MD_SPAN_EM: italic++; break;
        case MD_SPAN_STRONG: bold++; break;
        case MD_SPAN_CODE: code++; break;
        case MD_SPAN_DEL: strike++; break;
        // a formula is typeset by fastmd-tex.dll; without the library it stays as its own source, in code type
        case MD_SPAN_LATEXMATH: case MD_SPAN_LATEXMATH_DISPLAY:
            if (TexOk()) {
                mathDepth++;
                mathDisplay = t == MD_SPAN_LATEXMATH_DISPLAY;
                mathSrc.clear();
            } else {
                code++;
            }
            break;
        case MD_SPAN_A: {
            auto* a = (MD_SPAN_A_DETAIL*)det;
            d.links.emplace_back(a->href.text, a->href.size);
            curLink = (uint32_t)d.links.size() - 1;
            link++;
            break;
        }
        case MD_SPAN_FOOTNOTE_REF: {  // self-contained: md4c sends no text for it, the number is ours to draw
            if (!collecting && !inCell) break;
            auto* f = (MD_SPAN_FOOTNOTE_REF_DETAIL*)det;
            std::wstring tag = L"[" + std::to_wstring(f->id) + L"]";
            uint32_t start = (uint32_t)d.text.size();
            d.text += tag;
            d.links.push_back(L"#fn-" + std::to_wstring(f->id));
            d.runs.push_back(Run{start, (uint32_t)tag.size(), F_LINK, P_LINK, 0, (uint32_t)d.links.size() - 1});
            if (f->ref_id == 1) pendAnchors.push_back(L"fnref-" + std::to_wstring(f->id));
            paraOther = true;
            break;
        }
        case MD_SPAN_IMG: {
            auto* im = (MD_SPAN_IMG_DETAIL*)det;
            paraImage = AddImage(std::wstring(im->src.text, im->src.size), 0, 0);
            paraImages++;
            img++;
            imgAlt.clear();
            // a picture inside the line (badges: [![alt](badge)](link)); a paragraph holding nothing else still
            // becomes a picture block, and then this run is dropped
            if (collecting || inCell) {
                uint32_t start = (uint32_t)d.text.size();
                d.text.push_back(L'\xFFFC');
                uint16_t fl = (uint16_t)(F_IMAGE | (link ? F_LINK : 0));
                d.runs.push_back(Run{start, 1, fl, P_DEFAULT, 0, link ? curLink : 0, (uint32_t)paraImage});
            }
            break;
        }
        default: break;
        }
        return 0;
    }

    int SpanLeave(MD_SPANTYPE t, void*) {
        switch (t) {
        case MD_SPAN_EM: italic--; break;
        case MD_SPAN_STRONG: bold--; break;
        case MD_SPAN_CODE: code--; break;
        case MD_SPAN_DEL: strike--; break;
        case MD_SPAN_LATEXMATH: case MD_SPAN_LATEXMATH_DISPLAY:
            if (texOk == 1) {
                if (mathDepth > 0 && --mathDepth == 0) EmitMath();
            } else {
                code--;
            }
            break;
        case MD_SPAN_A: link--; break;
        case MD_SPAN_IMG:
            img--;
            if (paraImage >= 0 && (size_t)paraImage < d.images.size()) d.images[paraImage].alt = imgAlt;
            imgAlt.clear();
            break;
        default: break;
        }
        return 0;
    }

    int OnText(MD_TEXTTYPE t, const MD_CHAR* s, MD_SIZE n) {
        brkNow = brk;  // map mode: a break extent belongs to the static text md4c sends right after it, or to nothing
        brk = false;
        if (mathDepth > 0) {  // inside a formula: every character belongs to its source, not to the document text
            mathSrc.append(s, n);
            return 0;
        }
        switch (t) {
        case MD_TEXT_NORMAL: case MD_TEXT_CODE: case MD_TEXT_LATEXMATH: AppendText(s, n, false); break;
        case MD_TEXT_NULLCHAR:
            nulNow = true;
            AppendText(L"\xFFFD", 1, false);
            nulNow = false;
            break;
        case MD_TEXT_BR: AppendText(L"\n", 1, false); break;
        case MD_TEXT_SOFTBR: AppendText(L" ", 1, false); break;
        case MD_TEXT_ENTITY: AppendText(s, n, true); break;
        case MD_TEXT_HTML: {
            if (inHtmlBlock) { htmlRaw.append(s, n); break; }  // a block: kept whole, walked when it ends
            HtmlTag tag;                                       // inline: one tag per callback, text comes as normal
            if (n && s[0] == L'<' && ParseHtmlTag(s, n, tag)) {
                if (map) MapHtmlTag(tag, s, n);
                else if (!tag.name.empty()) HtmlInline(tag);
                break;
            }
            AppendText(s, n, false);
            break;
        }
        default: break;
        }
        brkNow = false;
        return 0;
    }

    // Simple front matter ("key: value" lines and nothing else) becomes a two-column table of properties; anything
    // nested — indentation, list items, keys without a value — is left to the YAML block. Returns false if not simple.
    bool FrontMatterTable(const wchar_t* s, size_t n) {
        struct Prop { std::wstring key, value; uint32_t keyOff, valueOff; };  // offsets: where they are in the source
        std::vector<Prop> props;
        for (size_t i = 0; i < n;) {
            size_t le = i;
            while (le < n && s[le] != L'\n') le++;
            size_t end = le;
            while (end > i && (s[end - 1] == L'\r' || s[end - 1] == L' ' || s[end - 1] == L'\t')) end--;
            if (end == i) { i = le + 1; continue; }        // blank line
            if (s[i] == L' ' || s[i] == L'\t') return false;  // nested block
            if (s[i] == L'#') { i = le + 1; continue; }    // comment
            size_t colon = i;
            while (colon < end && s[colon] != L':') colon++;
            if (colon >= end || colon == i) return false;
            for (size_t k = i; k < colon; k++) {
                wchar_t c = s[k];
                if (!(iswalnum(c) || c == L'_' || c == L'-' || c == L'.' || c == L' ')) return false;
            }
            size_t vs = colon + 1;
            while (vs < end && (s[vs] == L' ' || s[vs] == L'\t')) vs++;
            if (vs >= end) return false;  // "key:" alone starts a nested value
            std::wstring value(s + vs, end - vs);
            uint32_t valueOff = (uint32_t)(s + vs - srcBase);
            if (value.size() > 1 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front()) {
                value = value.substr(1, value.size() - 2);
                valueOff++;
            }
            props.push_back(Prop{std::wstring(s + i, colon - i), std::move(value), (uint32_t)(s + i - srcBase), valueOff});
            if (props.size() > 40) return false;
            i = le + 1;
        }
        if (props.empty()) return false;
        Table tb{};
        tb.cols = 2;
        tb.rows = (uint32_t)props.size();
        tb.cellOff = (uint32_t)d.cells.size();
        tb.alignOff = (uint32_t)d.aligns.size();
        d.cells.resize(d.cells.size() + 2 * props.size(), Cell{0, 0, 0, 0});
        d.aligns.resize(d.aligns.size() + 2, 0);
        d.tables.push_back(tb);
        uint32_t first = (uint32_t)d.text.size();
        for (size_t r = 0; r < props.size(); r++) {
            uint32_t ks = (uint32_t)d.text.size(), kr = (uint32_t)d.runs.size();
            d.text += props[r].key;
            uint32_t klen = (uint32_t)d.text.size() - ks;
            d.runs.push_back(Run{ks, klen, F_BOLD, P_DEFAULT, 0, 0});
            d.cells[tb.cellOff + r * 2] = Cell{ks, klen, kr, 1};
            uint32_t vs2 = (uint32_t)d.text.size();
            d.text += props[r].value;
            d.cells[tb.cellOff + r * 2 + 1] = Cell{vs2, (uint32_t)d.text.size() - vs2, (uint32_t)d.runs.size(), 0};
            if (!map) {  // "copy as Markdown" of a selection in the table gives back its property lines
                d.srcMap.emplace_back(ks, props[r].keyOff);
                d.srcMap.emplace_back(vs2, props[r].valueOff);
            }
        }
        Block& b = Emit(BK_TABLE, 0, 16);
        b.aux = (uint32_t)d.tables.size() - 1;
        b.textOff = first;
        b.textLen = (uint32_t)d.text.size() - first;
        return true;
    }

    // YAML front matter at the very top: "---" … "---" / "..." → property table or yaml code block. Returns chars consumed.
    size_t FrontMatter(const wchar_t* s, size_t n) {
        auto lineEnd = [&](size_t i) { while (i < n && s[i] != L'\n') i++; return i; };
        auto isDelim = [&](size_t ls, size_t le, bool closing) {
            while (le > ls && (s[le - 1] == L'\r' || s[le - 1] == L' ' || s[le - 1] == L'\t')) le--;
            if (le - ls != 3) return false;
            return wcsncmp(s + ls, L"---", 3) == 0 || (closing && wcsncmp(s + ls, L"...", 3) == 0);
        };
        size_t first = lineEnd(0);
        if (first >= n || !isDelim(0, first, false)) return 0;
        size_t i = first + 1, bodyStart = i;
        for (int lines = 0; i < n && lines < 400; lines++) {
            size_t le = lineEnd(i);
            if (isDelim(i, le, true)) {
                if (i == bodyStart) return 0;  // "---\n---" is two thematic breaks, not front matter
                // map mode: the body (first line .. end of the last one) and the closing line, before their line ends
                fmBody = (uint32_t)bodyStart;
                fmLast = (uint32_t)(i - 1);
                if (fmLast > fmBody && s[fmLast - 1] == L'\r') fmLast--;
                fmOuter = (uint32_t)le;
                if (fmOuter > i && s[fmOuter - 1] == L'\r') fmOuter--;
                if (FrontMatterTable(s + bodyStart, i - bodyStart)) return le < n ? le + 1 : n;
                StartLeaf(BK_CODE, 0);
                for (size_t k = bodyStart; k < i; k++) {
                    if (!map && (k == bodyStart || s[k - 1] == L'\n'))  // "copy as Markdown": one entry per line
                        d.srcMap.emplace_back((uint32_t)d.text.size(), (uint32_t)k);
                    if (s[k] != L'\r') d.text.push_back(s[k]);
                }
                static const wchar_t kYaml[] = L"yaml";
                codeLang = kYaml;
                codeLangLen = 4;
                EndLeaf();
                d.blocks.back().muted = 1;
                return le < n ? le + 1 : n;
            }
            i = le + 1;
        }
        return 0;
    }
};

int cbEnter(MD_BLOCKTYPE t, void* det, void* u) { return ((Builder*)u)->Enter(t, det); }
int cbLeave(MD_BLOCKTYPE t, void* det, void* u) { return ((Builder*)u)->Leave(t, det); }
int cbSpanEnter(MD_SPANTYPE t, void* det, void* u) { return ((Builder*)u)->SpanEnter(t, det); }
int cbSpanLeave(MD_SPANTYPE t, void* det, void* u) { return ((Builder*)u)->SpanLeave(t, det); }
int cbText(MD_TEXTTYPE t, const MD_CHAR* s, MD_SIZE n, void* u) { return ((Builder*)u)->OnText(t, s, n); }

// the md4c patch's hooks (edit mode only)
void hkLeaf(MD_BLOCKTYPE t, MD_OFFSET beg, MD_OFFSET end, unsigned flags, void* u) {
    ((Builder*)u)->OnLeafExtent(t, beg, end, flags);
}
void hkVerbatim(MD_OFFSET beg, MD_OFFSET end, unsigned indent, void* u) { ((Builder*)u)->OnVerbatimLine(beg, end, indent); }
void hkBreak(MD_TEXTTYPE, MD_OFFSET beg, MD_OFFSET end, void* u) { ((Builder*)u)->OnBreakExtent(beg, end); }
void hkSpan(MD_SPANTYPE t, int enter, MD_OFFSET beg, MD_OFFSET end, void* u) {
    ((Builder*)u)->OnSpanExtent(t, enter, beg, end);
}
void hkCell(MD_OFFSET beg, MD_OFFSET end, int missing, void* u) { ((Builder*)u)->OnCellExtent(beg, end, missing); }
void hkFootnote(MD_OFFSET defBeg, MD_OFFSET beg, MD_OFFSET end, void* u) {
    ((Builder*)u)->OnFootnoteExtent(defBeg, beg, end);
}
const MD_FASTMD_HOOKS kHooks = {hkLeaf, hkVerbatim, hkBreak, hkSpan, hkCell, hkFootnote};
}  // namespace

bool ParseMarkdown(Doc& d, const wchar_t* src, size_t n, const ParseOptions* opt) {
    d.text.reserve(n);
    d.runs.reserve(n / 24 + 16);
    d.blocks.reserve(n / 60 + 16);
    Builder b(d);
    b.srcBase = src;
    b.srcEnd = src + n;
    // Edit mode's map (§4.3). Masks and raw-text leaves (§6.9) are accepted here already; they take effect in the
    // phase that brings raw-while-typing.
    b.map = opt && opt->wantMap;
    if (b.map) {
        d.blockSrc.reserve(n / 60 + 16);
        d.segs.reserve(n / 16 + 16);
        b.noSegs++;  // front matter is one raw object atom
    }
    size_t skip = b.FrontMatter(src, n);
    b.mdBase = (uint32_t)skip;
    if (b.map) {
        b.noSegs--;
        for (size_t k = 0; k < d.blockSrc.size(); k++) {  // the property table or the yaml block, edited as source
            BlockSrc& bs = d.blockSrc[k];
            bs.beg = b.fmBody;
            bs.end = b.fmLast;
            bs.line = 0;
            bs.lineEnd = b.fmLast;
            bs.outerEnd = b.fmOuter;
            bs.flags = BS_RAW | BS_FRONT;
            bs.rawId = 0;
            bs.segOff = bs.segCount = bs.spanOff = bs.spanCount = 0;
        }
        d.segs.clear();
        d.spans.clear();
        d.cellSrc.resize(d.cells.size(), CellSrc{0, 0, true});
        d.tableSrc.resize(d.tables.size());
    }
    MD_PARSER p{};
    p.abi_version = 0;
    p.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;  // $…$ and $$…$$ as on GitHub
    p.enter_block = cbEnter;
    p.leave_block = cbLeave;
    p.enter_span = cbSpanEnter;
    p.leave_span = cbSpanLeave;
    p.text = cbText;
    p.fastmd = b.map ? &kHooks : nullptr;
    int rc = md_parse(src + skip, (MD_SIZE)(n - skip), &p, &b);
    b.EndLeaf();
    for (auto& q : d.quotes)  // empty quotes are marked with first = UINT32_MAX and skipped when drawing
        if (q.last == UINT32_MAX || q.last < q.first || q.first >= d.blocks.size()) q.first = q.last = UINT32_MAX;
    if (b.map) {
        while (!b.contStack.empty()) b.PopContainer();  // an aborted parse
        // HTML-table and front-matter cells have no source of their own; cellSrc / tableSrc stay parallel
        d.cellSrc.resize(d.cells.size(), CellSrc{0, 0, true});
        d.tableSrc.resize(d.tables.size());
        d.blockOrder.reserve(d.blocks.size());
        for (uint32_t k = 0; k < d.blockSrc.size(); k++)
            if (!(d.blockSrc[k].flags & BS_SYNTH)) d.blockOrder.push_back(k);
        std::stable_sort(d.blockOrder.begin(), d.blockOrder.end(),
                         [&](uint32_t a, uint32_t c) { return d.blockSrc[a].line < d.blockSrc[c].line; });
        d.hasMap = true;
    }
    return rc == 0;
}

size_t FindPrefixCut(const wchar_t* s, size_t n, size_t minChars) {
    bool inFence = false, prevBlank = true;
    wchar_t fenceCh = 0;
    size_t fenceLen = 0, i = 0;
    while (i < n) {
        size_t ls = i;
        while (i < n && s[i] != L'\n') i++;
        size_t le = i;
        if (i < n) i++;
        size_t k = ls;
        int ind = 0;
        while (k < le && s[k] == L' ' && ind < 4) { k++; ind++; }
        bool blank = true;
        for (size_t j = ls; j < le; j++)
            if (s[j] != L' ' && s[j] != L'\t' && s[j] != L'\r') { blank = false; break; }
        if (ind < 4 && k + 2 < le && (s[k] == L'`' || s[k] == L'~') && s[k + 1] == s[k] && s[k + 2] == s[k]) {
            size_t cnt = 0;
            while (k + cnt < le && s[k + cnt] == s[k]) cnt++;
            if (!inFence) { inFence = true; fenceCh = s[k]; fenceLen = cnt; }
            else if (s[k] == fenceCh && cnt >= fenceLen) inFence = false;
        } else if (!inFence && ls >= minChars && prevBlank && ind == 0 && le > ls && s[ls] == L'#') {
            return ls;
        }
        prevBlank = blank;
    }
    return n;
}
