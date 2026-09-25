// Edit mode's formatting commands and inserts (docs/EDIT-MODE.md §8, Phase 3a): bold, italic, strikethrough and inline
// code - on a selection, or pending for the next typed character -, paragraph and heading styles, lists, quotes, code
// blocks, the inserts (table, formula, diagram, picture, rule) and the table operations.
//
// Like the rest of the core every operation is a pure function of a mapped Doc and its source: no `g`, no window. It
// returns splices and the state after them; the glue applies them, re-parses and runs the check the result asks for
// (Verified: the reader's text is the same, and the formatting is where the command put it - else the step is taken
// back, §7.5 step 5). The helpers of editcore.cpp come through editcore_i.h.
#include "editcore_i.h"
#include "../third_party/md4c/md4c.h"

// The commands run once per key or click and build their splices out of many small string operations: nothing is
// inlined, as in editcore.cpp's operations (§1 principle 3); the compiler's own inlining is back at the end of the file.
#pragma inline_depth(0)
using namespace ec;

namespace {
const uint16_t kFmts[] = {FMT_BOLD, FMT_ITALIC, FMT_STRIKE, FMT_CODE};  // outermost first when several wrap one text

// does the span give that format - as Markdown, or as the HTML tag of its class?
bool Gives(const SpanSrc& sp, uint16_t fmt) {
    switch (fmt) {
    case FMT_BOLD: return sp.type == MD_SPAN_STRONG || sp.type == ST_HTML_B;
    case FMT_ITALIC: return sp.type == MD_SPAN_EM || sp.type == ST_HTML_I;
    case FMT_STRIKE: return sp.type == MD_SPAN_DEL || sp.type == ST_HTML_S;
    case FMT_CODE: return sp.type == MD_SPAN_CODE || sp.type == ST_HTML_CODE;
    case FMT_LINK: return sp.type == MD_SPAN_A || sp.type == ST_HTML_A;
    }
    return false;
}
// the delimiter the editor writes (§7.5 step 3: never `_`)
const wchar_t* Mark(uint16_t fmt) { return fmt == FMT_BOLD ? L"**" : fmt == FMT_ITALIC ? L"*" : L"~~"; }
bool Link(const SpanSrc& sp) { return (sp.type == MD_SPAN_A || sp.type == ST_HTML_A) && !(sp.flags & SF_AUTOLINK); }
// spans a format never cuts: code, formulas, pictures, footnote references, autolinks (a link's text is cut at its
// edges; emphasis nests)
bool Atomic(const SpanSrc& sp) { return !(sp.flags & SF_ENTERABLE) && !Link(sp); }
// what inline code leaves out of its text: it shows no text of its own
bool NotText(const SpanSrc& sp) {
    return sp.type == MD_SPAN_IMG || sp.type == MD_SPAN_LATEXMATH || sp.type == MD_SPAN_LATEXMATH_DISPLAY ||
           sp.type == MD_SPAN_FOOTNOTE_REF;
}

// ---- flanking (§7.5 step 4, CommonMark): an opener must be left-flanking, a closer right-flanking. 0 = a line's edge.
bool Punct(wchar_t c) {
    if (c < 0x80) return c > 0x20 && c < 0x7F && !iswalnum(c);
    WORD t = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &t);
    return (t & C1_PUNCT) != 0;
}
bool Ws(wchar_t c) { return !c || SpaceChar(c) || c == L'\r'; }
bool Opens(wchar_t before, wchar_t after) { return !Ws(after) && (!Punct(after) || Ws(before) || Punct(before)); }
bool Closes(wchar_t before, wchar_t after) { return !Ws(before) && (!Punct(before) || Ws(after) || Punct(after)); }
wchar_t At(const std::wstring& src, uint32_t i) { return i < src.size() && !EolChar(src[i]) ? src[i] : 0; }
wchar_t Prev(const std::wstring& src, uint32_t i) { return i > 0 && i <= src.size() && !EolChar(src[i - 1]) ? src[i - 1] : 0; }
bool Intraword(const std::wstring& src, uint32_t i) { return WordChar(Prev(src, i)) && WordChar(At(src, i)); }

// ---- edits on the source as it is, made into splices at the end
struct Ed { uint32_t a, b; std::wstring ins; };  // src[a, b) := ins
// back to front; edits at one offset in the order they were made; false when two overlap (nothing is applied then)
bool Splices(const std::wstring& src, std::vector<Ed> v, std::vector<Splice>& out) {
    Order(v, [](const Ed& x, const Ed& y) { return x.a > y.a; });  // (stable: equal offsets keep their order)
    out.clear();
    for (size_t i = 0; i < v.size();) {
        uint32_t a = v[i].a, b = v[i].b;
        std::wstring ins;
        size_t j = i;
        for (; j < v.size() && v[j].a == a; j++) {
            ins += v[j].ins;
            b = std::max(b, v[j].b);
        }
        if (b > src.size() || (!out.empty() && b > out.back().at)) return false;
        if (b > a || !ins.empty()) out.push_back(Replace(src, a, b, ins));
        i = j;
    }
    Coalesce(out);
    return true;
}
// The result of edits: the selection (or the caret) moves with the text - its start after text put in exactly there,
// its end before it
EditResult Built(const EditCtx& c, const EditState& st, std::vector<Ed> eds, EditKind kind) {
    EditResult r = Nothing(st, kind);
    if (!Splices(c.src, std::move(eds), r.splices)) return Refuse(st, "format");
    uint32_t lo = std::min(st.anchor, st.focus), hi = std::max(st.anchor, st.focus);
    uint32_t nlo = MapThrough(r.splices, lo, true), nhi = lo == hi ? nlo : MapThrough(r.splices, hi, false);
    r.after.anchor = st.anchor <= st.focus ? nlo : nhi;
    r.after.focus = st.anchor <= st.focus ? nhi : nlo;
    r.after.atom = -1;
    r.after.pendOn = r.after.pendOff = 0;
    r.after.phantom = Phantom{};
    return r;
}
// §7.4 for a line that will start at `at` (content): a backslash - or dropped indentation - as an edit
void EscapeAt(std::vector<Ed>& eds, const std::wstring& line, uint32_t at, bool first) {
    uint32_t drop;
    int k = Trigger(line, first, &drop);
    if (k >= 0) eds.push_back(Ed{at + (uint32_t)k, at + (uint32_t)k, L"\\"});
    else if (drop) eds.push_back(Ed{at, at + drop, L""});
}
// the Markdown-significant characters of text that was code, escaped (§8.2 step 4, §8.7)
std::wstring EscapeMd(const std::wstring& t, bool cell) {
    std::wstring o;
    for (wchar_t ch : t) {
        if (wcschr(L"\\*_`[]<&$~", ch) || (cell && ch == L'|')) o += L'\\';
        o += ch;
    }
    return o;
}
// a backtick run longer than any in the text, the text padded with a blank where it starts or ends with one (§7.5)
std::wstring Backticks(const std::wstring& t, size_t atLeast) {
    size_t best = 0;
    for (size_t i = 0; i < t.size();) {
        size_t k = i;
        while (k < t.size() && t[k] == L'`') k++;
        best = std::max(best, k - i);
        i = k > i ? k : i + 1;
    }
    return std::wstring(std::max(atLeast, best + 1), L'`');
}
std::wstring CodeSpan(const std::wstring& t, const std::wstring& run) {
    bool pad = !t.empty() && (t.front() == L'`' || t.back() == L'`');
    return run + (pad ? L" " : L"") + t + (pad ? L" " : L"") + run;
}

// ---- the ends of the selection (or the caret), and the blocks a command applies to
void Ends(const EditCtx& c, const EditState& st, TextPos* A, TextPos* B) {
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr), a = st.anchor == st.focus ? f : AnchorOf(c, st);
    *A = PosBefore(a, f) ? a : f;
    *B = PosBefore(a, f) ? f : a;
}
// The text blocks from the selection's first to its last (the caret's): not code, tables, objects, synthesized or
// folded blocks, footnotes or raw leaves. A block the selection only reaches at its very start is not touched.
std::vector<int32_t> TextBlocks(const EditCtx& c, const EditState& st) {
    const Doc& d = c.doc;
    TextPos A, B;
    Ends(c, st, &A, &B);
    std::vector<int32_t> v;
    if (!ValidBlock(d, A.block) || !ValidBlock(d, B.block)) return v;
    for (int32_t b = A.block; b <= B.block; b++) {
        if (b == B.block && b != A.block && B.t <= d.blocks[b].textOff) break;
        if (TextBlk(d, b) && !(d.blockSrc[b].flags & (BS_FOOTNOTE | BS_RAWTEXT)) && !Hidden(d, d.blocks[b])) v.push_back(b);
    }
    return v;
}
// the block an insert goes after (§8.8): the selected object's, the selection's last, the caret's (a table for a cell)
int32_t InsertAfterBlock(const EditCtx& c, const EditState& st) {
    if (st.atom >= 0) return AtomBlockOf(c.doc, st.atom);
    TextPos A, B;
    Ends(c, st, &A, &B);
    return B.block;
}
// the end of a block's last line, an HTML block drawn as several blocks included
uint32_t BlockEnd(const Doc& d, int32_t b) {
    const BlockSrc& bs = d.blockSrc[b];
    uint32_t end = bs.outerEnd;
    if ((bs.flags & BS_RAW) && bs.rawId >= 0)
        for (size_t k = b + 1; k < d.blocks.size() && d.blockSrc[k].rawId == bs.rawId; k++) end = std::max(end, d.blockSrc[k].outerEnd);
    return end;
}
// Where the containers of block b end on the line that starts at ls: the quote markers ('>' and a blank each) and the
// list items' indentation - on an item's own first line its marker. *quotes gets each quote's '>' (UINT32_MAX where a
// lazy line has none).
uint32_t WalkPrefix(const Doc& d, const std::wstring& src, int32_t b, uint32_t ls, std::vector<uint32_t>* quotes) {
    uint32_t le = LineEndOf(src, ls), p = ls, col = 0;
    auto step = [&]() { col = src[p] == L'\t' ? (col + 4) & ~3u : col + 1; p++; };
    for (const ContainerSrc* k : Chain(d, b)) {
        if (k->kind == CT_QUOTE || k->kind == CT_ALERT) {
            uint32_t q = p;
            while (q < le && q - p < 3 && src[q] == L' ') q++;
            if (q < le && src[q] == L'>') {
                while (p <= q) step();
                if (quotes) quotes->push_back(q);
                if (p < le && Blank(src[p])) step();
            } else if (quotes) {
                quotes->push_back(UINT32_MAX);
            }
        } else if (k->kind == CT_ITEM) {
            bool marker = k->markOff != UINT32_MAX && k->markOff >= p && k->markOff < le;  // the item's own first line
            while (p < le && col < k->contentCol && (marker || Blank(src[p]))) step();
        }
    }
    return p;
}
// the source lines of a block after its first (its continuation lines, a setext underline), by their starts
std::vector<uint32_t> MoreLines(const std::wstring& src, const BlockSrc& bs, uint32_t end) {
    std::vector<uint32_t> v;
    for (uint32_t l = SkipEol(src, LineEndOf(src, bs.line)); l <= end && l < src.size();) {
        if (l == LineEndOf(src, l) && l == end) break;
        v.push_back(l);
        uint32_t nx = SkipEol(src, LineEndOf(src, l));
        if (nx <= l) break;
        l = nx;
    }
    return v;
}

// A block's lines go and a phantom takes its place: an empty paragraph cannot be written (§6.7) - an emptied heading,
// a code block with nothing in it, a table that goes
EditResult GoneToPhantom(const EditCtx& c, const EditState& st, int32_t b, EditKind kind) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const BlockSrc& bs = d.blockSrc[b];
    uint32_t a = bs.line, e = BlockEnd(d, b);
    if (a > 0) {
        a = BackEol(src, a);
        uint32_t ls = LineStartOf(src, a);
        if (ls > 0 && BlankLine(src, ls, a)) a = BackEol(src, ls);  // and the blank line before it
    } else {
        e = SkipEol(src, e);
        uint32_t le = LineEndOf(src, e);
        if (e < src.size() && BlankLine(src, e, le)) e = SkipEol(src, le);
    }
    const int depth = (int)Chain(d, b).size();
    const int32_t P = PrevInSource(d, b), N = NextInSource(d, b);
    EditResult r;
    if (P >= 0) {
        r = NewPhantom(c, st, PH_AFTER, P, std::min(depth, (int)Chain(d, P).size()), BlockEnd(d, P), true);
    } else if (N >= 0) {
        uint32_t at;
        PrefixN(d, src, N, depth, &at);
        r = NewPhantom(c, st, PH_BEFORE, N, depth, at - (e - a), true);
    } else {
        r = Nothing(st, kind);
        r.after.focus = r.after.anchor = a;
    }
    r.splices.push_back(Replace(src, a, e, L""));
    r.kind = kind;
    r.after.pendOn = r.after.pendOff = 0;
    return r;
}

// A block of its own - the lines given, without prefixes - after the current block (§8.8): blank lines on both sides,
// every line with that block's continuation prefix, so it stays in the list item or quote; in a phantom the phantom
// becomes it. The caret goes to line cl, column cc; with `rule` a phantom after the new block takes it (UX-3).
EditResult InsertBlock(const EditCtx& c, const EditState& st, const std::vector<std::wstring>& lines, uint32_t cl, uint32_t cc,
                       bool rule) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    uint32_t at;
    std::wstring pre, bpre;
    int depth;
    bool before = false;
    const Phantom& ph = st.phantom;
    if (InPh(st) && ph.kind != PH_BREAK) {
        if (PhantomBlock(d, src, ph) < 0) return Refuse(st, "nowhere");
        at = std::min<uint32_t>(ph.anchorSrc, (uint32_t)src.size());
        pre = ph.prefix;
        bpre = ph.blankPrefix;
        depth = ph.depth;
        before = ph.kind == PH_BEFORE;
    } else {
        int32_t b = InPh(st) ? PhantomBlock(d, src, ph) : InsertAfterBlock(c, st);
        if (!ValidBlock(d, b) || (d.blockSrc[b].flags & BS_SYNTH)) return Refuse(st, "nowhere");
        at = BlockEnd(d, b);
        pre = ContPrefix(d, src, b);
        bpre = Strip(pre);
        depth = (int)Chain(d, b).size();
    }
    const std::wstring E = Eol(c, at);
    std::wstring body;
    uint32_t caret = 0;
    for (size_t k = 0; k < lines.size(); k++) {
        if (k) body += E;
        std::wstring p = before && !k ? L"" : lines[k].empty() && k != cl ? bpre : pre;  // (the caret's line: typed into)
        if (k == cl) caret = (uint32_t)(body.size() + p.size()) + cc;
        body += p + lines[k];
    }
    const uint32_t blockLen = (uint32_t)body.size();
    std::wstring ins;
    uint32_t off = 0;
    if (before) {
        ins = body + E + bpre + E + pre;
    } else {
        ins = E + bpre + E;
        off = (uint32_t)ins.size();
        ins += body;
        uint32_t nx = SkipEol(src, at);  // blank lines on both sides of the new block
        if (nx > at && nx < src.size() && !BlankLine(src, nx, LineEndOf(src, nx))) ins += E + bpre;
    }
    EditResult r = Nothing(st, EK_STRUCT);
    r.splices.push_back(Splice{at, L"", ins});
    r.after.focus = r.after.anchor = at + off + caret;
    r.after.atom = -1;
    r.after.pendOn = r.after.pendOff = 0;
    r.after.phantom = Phantom{};
    if (rule) {
        Phantom& nph = r.after.phantom;
        nph.kind = PH_AFTER;
        nph.anchorSrc = at + off + blockLen;
        nph.prefix = pre;
        nph.blankPrefix = bpre;
        nph.depth = (uint8_t)std::clamp(depth, 0, 255);
        nph.in = 1;
        r.after.focus = r.after.anchor = nph.anchorSrc;
    }
    return r;
}
}  // namespace

// ------------------------------------------------------------------------------------------------ the check (§7.5 step 5)
// Whether the character at text offset t has the format as the markup gives it - inside a span of it. (A table's
// header row is drawn bold without being bold: the toggles, their checks and the bar's states go by the markup.)
bool HasFormat(const Doc& d, uint32_t t, uint16_t fmt) {
    const std::vector<Block>& bl = d.blocks;
    if (!d.hasMap || d.blockSrc.size() != bl.size()) return false;
    auto it = std::upper_bound(bl.begin(), bl.end(), t, [](uint32_t v, const Block& b) { return v < b.textOff; });
    if (it == bl.begin()) return false;
    const int32_t b = (int32_t)(it - bl.begin()) - 1;
    const CellSrc* cs = nullptr;  // in a table: the spans opening in the cell that holds t
    if (const Table* tb = TableOf(d, b)) {
        for (uint32_t k = 0; k < tb->rows * tb->cols && !cs; k++) {
            const Cell& cl = d.cells[tb->cellOff + k];
            if (t >= cl.textOff && t < cl.textOff + cl.textLen && tb->cellOff + k < d.cellSrc.size()) cs = &d.cellSrc[tb->cellOff + k];
        }
        if (!cs) return false;
    }
    const BlockSrc& bs = d.blockSrc[b];
    for (uint32_t k = bs.spanOff; k < bs.spanOff + bs.spanCount && k < d.spans.size(); k++) {
        const SpanSrc& sp = d.spans[k];
        if (sp.tBeg <= t && t < sp.tEnd && Gives(sp, fmt) && (!cs || (sp.openBeg >= cs->beg && sp.openBeg <= cs->end))) return true;
    }
    return false;
}

std::vector<const SpanSrc*> ec::SpansIn(const Doc& d, const TextPos& p, TRange r) {
    std::vector<const SpanSrc*> v;
    ForSpans(d, p, r, [&](const SpanSrc& sp) { v.push_back(&sp); });
    return v;
}

namespace {
// the blocks a structural check counts: not the synthesized ones
std::vector<uint32_t> RealBlocks(const Doc& d) {
    std::vector<uint32_t> v;
    for (uint32_t i = 0; i < d.blocks.size(); i++)
        if (i >= d.blockSrc.size() || !(d.blockSrc[i].flags & BS_SYNTH)) v.push_back(i);
    return v;
}
bool SameShape(const Doc& a, uint32_t i, const Doc& b, uint32_t j) {
    const Block &x = a.blocks[i], &y = b.blocks[j];
    return x.kind == y.kind && x.heading == y.heading && x.textLen == y.textLen &&
           !a.text.compare(x.textOff, x.textLen, b.text, y.textOff, y.textLen);
}
// EditResult::Shape: the blocks outside the ones the step names are the same, and as many (Phase 4 notes)
bool Shaped(const Doc& a, const Doc& b, const EditResult::Shape& s) {
    if (!s.on) return true;
    const std::vector<uint32_t> ra = RealBlocks(a), rb = RealBlocks(b);
    if ((int64_t)rb.size() != (int64_t)ra.size() + s.delta) return false;
    size_t k0 = 0, k1 = 0;  // the first real block at b0, the first after b1
    while (k0 < ra.size() && (int32_t)ra[k0] < s.b0) k0++;
    k1 = k0;
    while (k1 < ra.size() && (int32_t)ra[k1] <= s.b1) k1++;
    if (s.b1 < s.b0) k0 = k1 = ra.size();  // every block
    for (size_t k = 0; k < k0; k++)
        if (!SameShape(a, ra[k], b, rb[k])) return false;
    for (size_t k = k1; k < ra.size(); k++)
        if (!SameShape(a, ra[k], b, rb[k + (size_t)((int64_t)rb.size() - (int64_t)ra.size())])) return false;
    if (s.same && s.b1 >= s.b0)
        for (size_t k = k0; k < k1; k++)
            if (!SameShape(a, ra[k], b, rb[k])) return false;
    return true;
}
}  // namespace

bool Verified(const Doc& a, const Doc& b, const EditResult& r) {
    if (!Kept(a, b, r.keep) || !Shaped(a, b, r.shape)) return false;
    for (const EditResult::Expect& e : r.verify) {
        if (e.tEnd > b.text.size()) return false;
        for (uint32_t t = e.tBeg; t < e.tEnd; t++) {
            wchar_t ch = b.text[t];
            if (SpaceChar(ch) || ch == 0xFFFC) continue;
            if (HasFormat(b, t, e.fmt) != e.present) return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ inline toggles (§8.2)
namespace {
struct Piece { int32_t block, cell; uint32_t t0, t1; };

// The source of text [t0, t1) of a block or cell, with the delimiters of the spans that lie wholly inside it (their
// text is all in the piece) - but not of those that go on beyond it, and for inline code not of a link: its text goes
// into code inside it, its address never into the code's text (Phase 4 notes)
bool PieceSrc(const Doc& d, const TextPos& p, TRange rg, uint32_t t0, uint32_t t1, uint32_t* sA, uint32_t* sB, bool code = false) {
    SegSpan ss = SegsIn(d, p.block, rg);
    const SrcSeg* g0 = SegCovering(ss, t0);
    const SrcSeg* g1 = t1 > t0 ? SegCovering(ss, t1 - 1) : nullptr;
    if (!g0 || !g1) return false;
    *sA = g0->kind == SEG_PLAIN ? g0->s + (t0 - g0->t) : g0->s;
    *sB = g1->kind == SEG_PLAIN ? g1->s + (t1 - g1->t) : g1->s + g1->sLen;
    for (const SpanSrc* sp : SpansIn(d, p, rg)) {
        if ((sp->flags & SF_UNCLOSED) || sp->tBeg < t0 || sp->tEnd > t1 || sp->tBeg == sp->tEnd || (code && Link(*sp))) continue;
        *sA = std::min(*sA, sp->openBeg);
        *sB = std::max(*sB, sp->closeEnd);
    }
    return *sA <= *sB;
}

// The selection in pieces (§8.2 step 1): per block and per cell; a piece grows over the spans it may not cut (for
// inline code only over those with text, and not over code when it is being removed) and is cut at the edges of every
// enterable span and link text inside it, so a new span nests with the ones there; inline code leaves out what shows
// no text of its own (a picture, a formula stays itself beside the code).
std::vector<Piece> Pieces(const EditCtx& c, const TextPos& A, const TextPos& B, uint16_t fmt, bool remove) {
    const Doc& d = c.doc;
    std::vector<Piece> out;
    auto add = [&](int32_t b, int32_t cell, uint32_t t0, uint32_t t1) {
        TextPos p{t0, b, cell};
        TRange rg;
        if (!RangeOf(d, p, &rg)) return;
        t0 = std::clamp(t0, rg.beg, rg.end);
        t1 = std::clamp(t1, rg.beg, rg.end);
        if (t1 <= t0) return;
        const std::vector<const SpanSrc*> spans = SpansIn(d, p, rg);
        for (bool grew = true; grew;) {
            grew = false;
            for (const SpanSrc* sp : spans) {
                bool whole = !(sp->flags & SF_UNCLOSED) && Atomic(*sp) &&
                             (fmt != FMT_CODE || (remove ? !Gives(*sp, FMT_CODE) : !NotText(*sp)));
                if (whole && sp->tBeg < t1 && sp->tEnd > t0 && (sp->tBeg < t0 || sp->tEnd > t1)) {
                    t0 = std::min(t0, sp->tBeg);
                    t1 = std::max(t1, sp->tEnd);
                    grew = true;
                }
            }
        }
        std::vector<uint32_t> cuts{t0, t1};
        std::vector<std::pair<uint32_t, uint32_t>> out_;  // left out of inline code
        for (const SpanSrc* sp : spans) {
            if (sp->flags & SF_UNCLOSED) continue;
            // every edge of the spans the format is taken off (one piece each), of what inline code leaves out; of the
            // other spans the edges of those that go on beyond the piece - a span inside it just nests in the new one
            // (inline code: every edge of a link's text too - the code goes inside the link, Phase 4 notes)
            bool every = (remove && Gives(*sp, fmt)) || (fmt == FMT_CODE && !remove && (NotText(*sp) || Link(*sp)));
            bool beyond = sp->tBeg < t0 || sp->tEnd > t1;
            if (!every && !(((sp->flags & SF_ENTERABLE) || Link(*sp)) && beyond)) continue;
            for (uint32_t e : {sp->tBeg, sp->tEnd})
                if (e > t0 && e < t1) cuts.push_back(e);
            if (fmt == FMT_CODE && !remove && NotText(*sp)) out_.push_back({sp->tBeg, sp->tEnd});
        }
        if (fmt == FMT_CODE && !remove) {  // a picture in a line without a span of its own (HTML <img>): left out too
            SegSpan ss = SegsIn(d, b, rg);
            for (const SrcSeg* g = ss.b; g < ss.e; g++)
                if (g->kind == SEG_OBJATOM && g->t >= t0 && g->t + g->tLen <= t1) {
                    for (uint32_t e : {g->t, g->t + g->tLen})
                        if (e > t0 && e < t1) cuts.push_back(e);
                    out_.push_back({g->t, g->t + g->tLen});
                }
        }
        Order(cuts, [](uint32_t x, uint32_t y) { return x < y; });
        for (size_t k = 0; k + 1 < cuts.size(); k++) {
            uint32_t x = cuts[k], y = cuts[k + 1];
            if (y <= x) continue;
            bool skip = false;
            for (auto& o : out_) skip |= o.first <= x && y <= o.second;
            if (!skip) out.push_back(Piece{b, cell, x, y});
        }
    };
    for (int32_t b = A.block; b <= B.block; b++) {
        if (!ValidBlock(d, b) || Hidden(d, d.blocks[b]) || (d.blockSrc[b].flags & (BS_RAWTEXT | BS_SYNTH))) continue;
        if ((d.blockSrc[b].flags & BS_FOOTNOTE) && A.block != B.block) continue;  // shown far from where it is written
        const Block& bl = d.blocks[b];
        if (const Table* tb = TableOf(d, b)) {
            int32_t n = (int32_t)(tb->rows * tb->cols);
            int32_t c0 = b == A.block && A.cell >= 0 ? A.cell : 0, c1 = b == B.block && B.cell >= 0 ? B.cell : n - 1;
            for (int32_t k = c0; k <= c1 && k < n; k++) {
                const Cell& cl = d.cells[tb->cellOff + k];
                add(b, k, b == A.block && k == c0 ? A.t : cl.textOff, b == B.block && k == c1 ? B.t : cl.textOff + cl.textLen);
            }
            continue;
        }
        if (!TextBlk(d, b)) continue;
        add(b, -1, b == A.block ? A.t : bl.textOff, b == B.block ? B.t : LastStop(d, b));
    }
    return out;
}

bool Ink(const Doc& d, uint32_t t) { return !Ws(d.text[t]) && d.text[t] != 0xFFFC; }

// §7.5 step 1: blanks stay outside the delimiters
void TrimPiece(const Doc& d, Piece& pc) {
    while (pc.t0 < pc.t1 && Ws(d.text[pc.t0])) pc.t0++;
    while (pc.t1 > pc.t0 && Ws(d.text[pc.t1 - 1])) pc.t1--;
}

// The rendered text of [t0, t1): what inline code shows (soft and hard breaks as blanks); a picture keeps its source
std::wstring Rendered(const Doc& d, const std::wstring& src, int32_t b, TRange rg, uint32_t t0, uint32_t t1) {
    std::wstring o;
    SegSpan ss = SegsIn(d, b, rg);
    for (const SrcSeg* g = ss.b; g < ss.e; g++) {
        uint32_t x = std::max(t0, g->t), y = std::min(t1, g->t + g->tLen);
        if (x >= y) continue;
        if (g->kind == SEG_OBJATOM) o += Sub(src, g->s, g->s + g->sLen);
        else if (g->kind != SEG_SYNTH)
            for (uint32_t t = x; t < y; t++) o += d.text[t] == L'\n' ? L' ' : d.text[t];
    }
    return o;
}

// Add the format to one piece (§8.2 step 3): whitespace stays outside (step 1), spans of the same kind inside it go,
// a span of the same kind that ends where it starts (starts where it ends) takes it in (step 2), and an edge that
// cannot open or close there moves out to the word's edge (step 4) - or the piece fails.
bool AddTo(const EditCtx& c, Piece pc, uint16_t fmt, std::vector<Ed>& eds, EditResult& r) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    TextPos p{pc.t0, pc.block, pc.cell};
    TRange rg;
    if (!RangeOf(d, p, &rg)) return false;
    TrimPiece(d, pc);
    if (pc.t0 >= pc.t1) return true;
    bool all = true;
    for (uint32_t t = pc.t0; t < pc.t1 && all; t++) all = !Ink(d, t) || HasFormat(d, t, fmt);
    if (all) return true;  // (a part of the selection that has it already)
    uint32_t sA, sB;
    if (!PieceSrc(d, p, rg, pc.t0, pc.t1, &sA, &sB, fmt == FMT_CODE)) return false;
    if (fmt == FMT_CODE) {
        std::wstring t = Rendered(d, src, pc.block, rg, pc.t0, pc.t1);
        if (pc.cell >= 0) {  // (a pipe ends a cell, in code too)
            std::wstring e;
            for (wchar_t ch : t) {
                if (ch == L'|') e += L'\\';
                e += ch;
            }
            t = e;
        }
        eds.push_back(Ed{sA, sB, CodeSpan(t, Backticks(t, 1))});
        r.verify.push_back(EditResult::Expect{pc.t0, pc.t1, fmt, true});
        return true;
    }
    const std::wstring mark = Mark(fmt);
    const std::vector<const SpanSrc*> spans = SpansIn(d, p, rg);
    if (!Opens(Prev(src, sA), At(src, sA)) || !Closes(Prev(src, sB), At(src, sB))) {
        // out to the word's edges, if no span edge is in the way (`don⟦'t⟧` → `**don't**`)
        uint32_t t0 = pc.t0, t1 = pc.t1;
        while (t0 > rg.beg && WordChar(d.text[t0 - 1])) t0--;
        while (t1 < rg.end && WordChar(d.text[t1])) t1++;
        bool crosses = false;
        for (const SpanSrc* sp : spans)
            for (uint32_t e : {sp->tBeg, sp->tEnd}) crosses |= (e >= t0 && e < pc.t0) || (e > pc.t1 && e <= t1);
        if (crosses || !PieceSrc(d, p, rg, t0, t1, &sA, &sB) || !Opens(Prev(src, sA), At(src, sA)) ||
            !Closes(Prev(src, sB), At(src, sB)))
            return false;
        pc.t0 = t0;
        pc.t1 = t1;
    }
    bool left = false, right = false;
    for (const SpanSrc* sp : spans) {
        if (!Gives(*sp, fmt) || (sp->flags & SF_UNCLOSED)) continue;
        if (sp->tBeg >= pc.t0 && sp->tEnd <= pc.t1) {  // inside: its delimiters go, the new span covers it
            eds.push_back(Ed{sp->openBeg, sp->openEnd, L""});
            eds.push_back(Ed{sp->closeBeg, sp->closeEnd, L""});
        } else if (!left && sp->closeEnd == sA && Sub(src, sp->closeBeg, sp->closeEnd) == mark) {
            eds.push_back(Ed{sp->closeBeg, sp->closeEnd, L""});  // `**a**⟦b⟧` → `**ab**`
            left = true;
        } else if (!right && sp->openBeg == sB && Sub(src, sp->openBeg, sp->openEnd) == mark) {
            eds.push_back(Ed{sp->openBeg, sp->openEnd, L""});
            right = true;
        }
    }
    if (!left) eds.push_back(Ed{sA, sA, mark});
    if (!right) eds.push_back(Ed{sB, sB, mark});
    r.verify.push_back(EditResult::Expect{pc.t0, pc.t1, fmt, true});
    return true;
}

// Remove the format from one piece (§8.2 step 3): the span that gives it is split around the piece - closed before it,
// opened again after it; at the span's edge only that delimiter moves; the whole span loses both. A `_` span whose new
// delimiter would stand inside a word is written with `*` (F9-2: `_ab⟦cd⟧ef_` → `*ab*cd*ef*`). Inline code is
// unwrapped: its text written back with the Markdown characters escaped.
bool RemoveFrom(const EditCtx& c, Piece pc, uint16_t fmt, std::vector<Ed>& eds, EditResult& r) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    TextPos p{pc.t0, pc.block, pc.cell};
    TRange rg;
    if (!RangeOf(d, p, &rg)) return false;
    TrimPiece(d, pc);
    if (pc.t0 >= pc.t1) return true;
    bool any = false;
    const std::vector<const SpanSrc*> spans = SpansIn(d, p, rg);
    for (const SpanSrc* sptr : spans) {
        const SpanSrc& S = *sptr;
        if (!Gives(S, fmt) || (S.flags & SF_UNCLOSED) || S.tBeg > pc.t0 || S.tEnd < pc.t1) continue;
        any = true;
        if (S.type == MD_SPAN_CODE) {  // `ab⟦cd⟧ef` → `ab`cd`ef`, cd escaped
            std::wstring run;
            for (uint32_t k = S.openBeg; k < S.openEnd && src[k] == L'`'; k++) run += L'`';
            std::wstring lt = d.text.substr(S.tBeg, pc.t0 - S.tBeg), mt = d.text.substr(pc.t0, pc.t1 - pc.t0),
                         rt = d.text.substr(pc.t1, S.tEnd - pc.t1);
            std::wstring ins = (lt.empty() ? L"" : CodeSpan(lt, run)) + EscapeMd(mt, pc.cell >= 0) + (rt.empty() ? L"" : CodeSpan(rt, run));
            if (S.tBeg == d.blocks[pc.block].textOff && pc.t0 == S.tBeg && pc.cell < 0) {  // §7.4 at a block's start
                uint32_t drop;
                int k = Trigger(ins, true, &drop);
                if (k >= 0) ins.insert(ins.begin() + k, L'\\');
            }
            eds.push_back(Ed{S.openBeg, S.closeEnd, ins});
            continue;
        }
        // a span between it and the piece that goes on beyond the piece would have to be cut too: not done
        for (const SpanSrc* J : spans) {
            if (J == sptr || (J->flags & SF_UNCLOSED) || Atomic(*J)) continue;
            bool inS = J->openBeg >= S.openEnd && J->closeEnd <= S.closeBeg;
            bool holds = J->tBeg <= pc.t0 && J->tEnd >= pc.t1 && (J->tBeg < pc.t0 || J->tEnd > pc.t1);
            if (inS && holds) return false;
        }
        std::wstring o = Sub(src, S.openBeg, S.openEnd), k = Sub(src, S.closeBeg, S.closeEnd);
        // where the delimiters go: the closer after the text before the piece, the opener before the text after it
        // (where only blanks are left beside the piece, they go plain with it)
        uint32_t cAt = UINT32_MAX, oAt = UINT32_MAX, x, y;
        Piece lp{pc.block, pc.cell, S.tBeg, pc.t0}, rp{pc.block, pc.cell, pc.t1, S.tEnd};
        TrimPiece(d, lp);
        TrimPiece(d, rp);
        if (lp.t0 < lp.t1 && PieceSrc(d, p, rg, lp.t0, lp.t1, &x, &y)) cAt = y;
        if (rp.t0 < rp.t1 && PieceSrc(d, p, rg, rp.t0, rp.t1, &x, &y)) oAt = x;
        const bool head = cAt == UINT32_MAX, tail = oAt == UINT32_MAX;
        eds.push_back(Ed{S.openBeg, S.openEnd, L""});
        eds.push_back(Ed{S.closeBeg, S.closeEnd, L""});
        if (head && tail) continue;  // the whole span: both delimiters go
        // F9-2: a `_` delimiter that would stand inside a word would not be one - the span is written with `*`
        if ((S.flags & SF_UNDERSCORE) && ((cAt != UINT32_MAX && Intraword(src, cAt)) || (oAt != UINT32_MAX && Intraword(src, oAt)))) {
            std::replace(o.begin(), o.end(), L'_', L'*');
            std::replace(k.begin(), k.end(), L'_', L'*');
        }
        if (!head) {  // the text before the piece keeps it: the opener stays (rewritten), a closer after that text
            eds.push_back(Ed{S.openBeg, S.openBeg, o});
            eds.push_back(Ed{cAt, cAt, k});
        }
        if (!tail) {  // and the text after it: an opener before that text, the closer stays
            eds.push_back(Ed{oAt, oAt, o});
            eds.push_back(Ed{S.closeEnd, S.closeEnd, k});
        }
    }
    if (any) r.verify.push_back(EditResult::Expect{pc.t0, pc.t1, fmt, false});
    return true;
}
}  // namespace

// ------------------------------------------------------------------------------------------------ pending formats (§7.3)
// the enterable spans a blank typed at text position p - their end - leaves behind: they stay "sticky" (§7.3)
uint16_t ec::StickyAt(const Doc& d, const TextPos& p, uint32_t caret) {
    TRange rg;
    if (!ValidBlock(d, p.block) || !RangeOf(d, p, &rg)) return 0;
    uint32_t hard = 0;  // a link, code or formula ending there too: a span inside it cannot be carried past its closer
    const std::vector<const SpanSrc*> spans = SpansIn(d, p, rg);
    for (const SpanSrc* sp : spans)
        if (sp->tEnd == p.t && !(sp->flags & (SF_ENTERABLE | SF_UNCLOSED))) hard = std::max(hard, sp->closeEnd);
    uint16_t bits = 0;
    for (const SpanSrc* sp : spans) {
        if (sp->tEnd != p.t || sp->tBeg >= p.t || (sp->flags & SF_UNCLOSED) || sp->type >= 0x80 || sp->closeBeg < hard) continue;
        // Only a span the caret was typing in, before its closer: one whose closer the reader typed (or stepped over)
        // is ended - the blank after `**bold**` is plain (Phase 4 notes)
        if (caret > sp->closeBeg) continue;
        for (uint16_t f : {FMT_BOLD, FMT_ITALIC, FMT_STRIKE})
            if (Gives(*sp, f)) bits |= f;
    }
    return bits;
}

uint16_t ClosedAt(const Doc& d, const std::wstring& src, uint32_t caret) {
    if (!d.hasMap) return 0;
    uint16_t tr = 0;
    TextPos p = TextOfSrc(d, src, caret, -1, &tr);
    TRange rg;
    if (!ValidBlock(d, p.block) || !RangeOf(d, p, &rg)) return 0;
    uint16_t bits = 0;
    for (const SpanSrc* sp : SpansIn(d, p, rg))
        if (sp->closeEnd == caret && sp->closeBeg < caret && !(sp->flags & SF_UNCLOSED) && (sp->flags & SF_ENTERABLE))
            for (uint16_t f : {FMT_BOLD, FMT_ITALIC, FMT_STRIKE})
                if (Gives(*sp, f)) bits |= f;
    return bits;
}

// Text typed with a pending format (§7.3, §8.2): the formats pending on wrap it (`**x‸**`) - or take it into a span of
// theirs that ends right before it (§7.5's merge; after a blank when sticky: `**bold** x` → `**bold x**`); the formats
// pending off split their span around it (`**ab**x‸**cd**`). A format whose delimiter cannot open or close there stays
// pending and the text goes in plain (`foo(**x**`, never `foo**(**`). refused "plain": nothing to do but type plain.
EditResult ec::TypePending(const EditCtx& c, const EditState& st, std::wstring_view text) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const std::wstring T(text);
    uint16_t on = st.pendOn & 0xFF & ~st.pendOff, off = st.pendOff;
    const bool sticky = (st.pendOn & FMT_STICKY) != 0;
    uint16_t tr = 0;
    TextPos p = FocusOf(c, st, &tr);
    TRange rg;
    if (T.empty() || !ValidBlock(d, p.block) || !RangeOf(d, p, &rg) || d.blocks[p.block].kind == BK_CODE) return Refuse(st, "plain");
    uint32_t s = std::min<uint32_t>(st.focus, (uint32_t)src.size());
    std::vector<Ed> eds;
    std::wstring pre, post;
    // off: out of the outermost span of those formats open at the caret, and of the spans nested in it there
    const std::vector<const SpanSrc*> spans = SpansIn(d, p, rg);
    const SpanSrc* O = nullptr;
    for (const SpanSrc* sp : spans) {
        if ((sp->flags & SF_UNCLOSED) || sp->openEnd > s || s > sp->closeBeg) continue;
        for (uint16_t f : kFmts)
            if ((off & f) && Gives(*sp, f) && (!O || sp->openBeg < O->openBeg)) O = sp;
    }
    if (O) {
        std::vector<const SpanSrc*> k;  // open at the caret inside O (O too), innermost first
        for (const SpanSrc* sp : spans)
            if (!(sp->flags & SF_UNCLOSED) && sp->openEnd <= s && s <= sp->closeBeg && sp->openBeg >= O->openBeg &&
                sp->closeEnd <= O->closeEnd)
                k.push_back(sp);
        Order(k, [](const SpanSrc* a, const SpanSrc* b) { return a->openBeg > b->openBeg; });
        bool atEnd = true, atStart = true;  // only closers up to O's end (only openers from its start): outside of it
        for (uint32_t x = s; x < O->closeBeg && atEnd; x++) {
            bool delim = false;
            for (const SpanSrc* sp : k) delim |= x >= sp->closeBeg && x < sp->closeEnd;
            atEnd = delim;
        }
        for (uint32_t x = O->openEnd; x < s && atStart; x++) {
            bool delim = false;
            for (const SpanSrc* sp : k) delim |= x >= sp->openBeg && x < sp->openEnd;
            atStart = delim;
        }
        if (atEnd) {
            s = O->closeEnd;
        } else if (atStart) {
            s = O->openBeg;
        } else {
            for (const SpanSrc* sp : k) pre += Sub(src, sp->closeBeg, sp->closeEnd);
            for (size_t i = k.size(); i-- > 0;) post += Sub(src, k[i]->openBeg, k[i]->openEnd);
        }
    }
    // on: wrapped, or taken into the span of that format that ends here (or, sticky, only blanks before here)
    std::wstring open, close, merged;
    uint16_t applied = 0, keep = 0;
    for (uint16_t f : kFmts) {
        if (!(on & f)) continue;
        const SpanSrc* X = nullptr;
        if (f != FMT_CODE && pre.empty())
            for (const SpanSrc* sp : spans) {
                if (!Gives(*sp, f) || (sp->flags & SF_UNCLOSED) || Sub(src, sp->closeBeg, sp->closeEnd) != Mark(f)) continue;
                if (sp->closeEnd == s || (sticky && sp->closeEnd < s && AllBlank(src, sp->closeEnd, s))) X = sp;
            }
        if (X) {
            eds.push_back(Ed{X->closeBeg, X->closeEnd, L""});
            merged = Sub(src, X->closeBeg, X->closeEnd) + merged;
            applied |= f;
            continue;
        }
        std::wstring m = f == FMT_CODE ? Backticks(T, 1) : std::wstring(Mark(f));
        wchar_t before = pre.empty() && open.empty() ? Prev(src, s) : L'*', after = post.empty() && close.empty() ? At(src, s) : L'*';
        if (f != FMT_CODE && (!Opens(before, T.front()) || !Closes(T.back(), after))) {
            keep |= f;
            continue;
        }
        open += m;
        close = m + close;
        applied |= f;
    }
    int64_t shift = 0;  // what the edits before the text (a closer taken away) move it by
    for (const Ed& e : eds)
        if (e.a < s && e.b <= s) shift += (int64_t)e.ins.size() - (int64_t)(e.b - e.a);
    std::wstring ins = pre + open + T + close + merged + post;
    eds.push_back(Ed{s, s, ins});
    EditResult r = Nothing(st, EK_TYPE);
    if (!Splices(src, std::move(eds), r.splices)) return OpTypePlain(c, st, text);
    r.after.focus = r.after.anchor = (uint32_t)(s + shift) + (uint32_t)(pre.size() + open.size() + T.size());
    // what could not be put on stays pending; what was taken off is done (outside its span the caret types plain on)
    r.after.pendOn = keep;
    r.after.pendOff = O ? 0 : off;
    r.after.phantom = Phantom{};
    // the check: the text is the old one with T at the caret - after the trailing blanks the caret stood in, which show
    // once text follows them -, T has what was put on and lacks what was taken off
    std::wstring lead;
    if (tr)
        for (uint32_t k = st.focus; k > 0 && Blank(src[k - 1]); k--) lead.insert(lead.begin(), src[k - 1]);
    const uint32_t t0 = p.t + (uint32_t)lead.size(), t1 = t0 + (uint32_t)T.size();
    r.keep.on = true;
    r.keep.t0 = r.keep.t1 = p.t;
    r.keep.text = lead + T;
    for (uint16_t f : kFmts) {
        if (applied & f) r.verify.push_back(EditResult::Expect{t0, t1, f, true});
        if (O && (off & f)) r.verify.push_back(EditResult::Expect{t0, t1, f, false});
    }
    return r;
}

// Typed text as it is, at the caret's own offset, the pending formats kept: where their delimiters could not stand
// (§7.3), and the glue's way out when the formatted text did not render as asked
EditResult OpTypePlain(const EditCtx& c, const EditState& st, std::wstring_view text) {
    EditResult r = Nothing(st, EK_TYPE);
    uint16_t tr = 0;
    TextPos p = FocusOf(c, st, &tr);
    std::wstring t(text);
    if (ValidBlock(c.doc, p.block) && TableOf(c.doc, p.block)) {  // (a pipe would end the cell)
        std::wstring e;
        for (wchar_t ch : t) {
            if (ch == L'|') e += L'\\';
            e += ch;
        }
        t = e;
    }
    uint32_t s = std::min<uint32_t>(st.focus, (uint32_t)c.src.size());
    r.splices.push_back(Splice{s, L"", t});
    r.after.focus = r.after.anchor = s + (uint32_t)t.size();
    r.after.pendOn &= ~FMT_STICKY;
    if (!(r.after.pendOn & 0xFF)) r.after.pendOn = 0;
    return Carry(std::move(r));  // (as OpType's: a phantom after the block moves with text typed at its end)
}

// §8.2: with a selection the format is added to every piece that lacks it - or, when every character has it already,
// taken off all of them; with a caret it is pending for the next typed character (in a span of it: pending off; at
// the span's end the caret steps over its closer), and a second toggle takes the pending bit back.
EditResult OpToggleInline(const EditCtx& c, const EditState& st, uint16_t fmt) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    if (st.atom >= 0 || !(fmt & (FMT_BOLD | FMT_ITALIC | FMT_STRIKE | FMT_CODE))) return Refuse(st, "context");
    if (st.anchor == st.focus || InPh(st)) {
        EditResult r = Nothing(st, EK_OTHER);
        EditState& a = r.after;
        if ((a.pendOn | a.pendOff) & fmt) {  // a second toggle takes it back
            a.pendOn &= ~fmt;
            a.pendOff &= ~fmt;
            if (!(a.pendOn & 0xFF)) a.pendOn = 0;
            return r;
        }
        a.pendOn &= ~FMT_STICKY;
        if (InPh(st)) {
            a.pendOn |= fmt;
            return r;
        }
        uint16_t tr = 0;
        TextPos p = FocusOf(c, st, &tr);
        TRange rg;
        if (!ValidBlock(d, p.block) || !RangeOf(d, p, &rg) || d.blocks[p.block].kind == BK_CODE) return Refuse(st, "context");
        const uint32_t s = st.focus;
        const SpanSrc* S = nullptr;  // the innermost span of the format around the caret
        for (const SpanSrc* sp : SpansIn(d, p, rg))
            if (Gives(*sp, fmt) && !(sp->flags & SF_UNCLOSED) && sp->openEnd <= s && s <= sp->closeBeg && (!S || sp->openBeg > S->openBeg))
                S = sp;
        if (!S) {
            a.pendOn |= fmt;
        } else {
            a.pendOff |= fmt;
            if (s == S->closeBeg && fmt != FMT_CODE) a.focus = a.anchor = S->closeEnd;  // at its end: past the closer
        }
        (void)src;
        return r;
    }
    TextPos A, B;
    Ends(c, st, &A, &B);
    // add, unless every character of the selection that shows has the format already
    bool all = true, ink = false;
    for (const Piece& pc : Pieces(c, A, B, fmt, false))
        for (uint32_t t = pc.t0; t < pc.t1; t++)
            if (Ink(d, t)) {
                ink = true;
                all &= HasFormat(d, t, fmt);
            }
    if (!ink) return Nothing(st, EK_OTHER);
    const bool remove = all;
    std::vector<Ed> eds;
    EditResult r;
    for (const Piece& pc : Pieces(c, A, B, fmt, remove))
        if (!(remove ? RemoveFrom(c, pc, fmt, eds, r) : AddTo(c, pc, fmt, eds, r))) return Refuse(st, "format");
    if (eds.empty()) return Nothing(st, EK_OTHER);
    std::vector<EditResult::Expect> verify = std::move(r.verify);
    r = Built(c, st, std::move(eds), EK_FORMAT);
    if (!r.refused.empty()) return r;
    r.verify = std::move(verify);
    // the text the reader sees stays the same (inline code shows a hard break as a blank) over the selection and the
    // words its edges moved out to; the selection is found again by its text
    r.keep.on = true;
    r.keep.t0 = A.t;
    r.keep.t1 = std::max(A.t, B.t);
    for (const EditResult::Expect& e : r.verify) {
        r.keep.t0 = std::min(r.keep.t0, e.tBeg);
        r.keep.t1 = std::max(r.keep.t1, e.tEnd);
    }
    r.keep.text = d.text.substr(r.keep.t0, r.keep.t1 - r.keep.t0);
    r.selA = A;
    r.selB = B;
    if (fmt == FMT_CODE && !remove)
        for (const EditResult::Expect& e : r.verify)
            for (uint32_t t = e.tBeg; t < e.tEnd; t++)
                if (r.keep.text[t - r.keep.t0] == L'\n') r.keep.text[t - r.keep.t0] = L' ';
    return r;
}

// ------------------------------------------------------------------------------------------------ block style (§8.4)
namespace {
// the first text of a task item: its box stands before it, so neither a heading nor a quote can start there
bool TaskText(const Doc& d, int32_t b) {
    const int32_t it = ItemOf(d, b);
    return it >= 0 && d.containers[it].taskOff != UINT32_MAX;
}
// Block b becomes a paragraph: a blank line (its container prefix, stripped) between it and a paragraph line right
// above or below it, which would run into it - `text⏎===` would be a setext heading (Phase 4 notes)
void KeepApart(const EditCtx& c, int32_t b, uint32_t lastEnd, std::vector<Ed>& eds) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const std::wstring blank = Strip(ContPrefix(d, src, b));
    const uint32_t line = d.blockSrc[b].line, nx = SkipEol(src, lastEnd);
    if (RunsIntoAbove(d, src, b)) eds.push_back(Ed{line, line, blank + Eol(c, line)});
    if (RunsIntoBelow(d, src, lastEnd)) eds.push_back(Ed{nx, nx, blank + Eol(c, lastEnd)});
}
}  // namespace

// Paragraph ↔ heading n per text block; the same level again returns them to paragraphs. A paragraph's lines become
// one (soft breaks a blank, hard breaks <br>); ATX m → n changes the # run only; setext becomes ATX; back to a
// paragraph the markers go and §7.4 escapes what would read as block syntax. A one-line alert's tag goes onto its own
// line first. In a phantom: its style.
EditResult OpBlockStyle(const EditCtx& c, const EditState& st, int level) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    level = std::clamp(level, 0, 6);
    if (InPh(st)) {
        EditResult r = Nothing(st, EK_OTHER);
        r.after.phantom.style = (uint8_t)(st.phantom.style == level ? 0 : level);
        return r;
    }
    if (st.atom >= 0) return Refuse(st, "context");
    std::vector<int32_t> bl = TextBlocks(c, st);
    if (bl.empty()) return Refuse(st, "context");
    bool all = level > 0;
    for (int32_t b : bl) all &= d.blocks[b].heading == level;
    const int to = all ? 0 : level;
    if (to)  // a task item's box stands before its text: `- [x] ## Task` would be no heading (Phase 4 notes)
        for (int32_t b : bl)
            if (TaskText(d, b)) return Refuse(st, "context");
    if (to == 0 && bl.size() == 1 && d.blocks[bl[0]].heading && (d.blockSrc[bl[0]].flags & BS_ATX) && !d.blocks[bl[0]].textLen)
        return GoneToPhantom(c, st, bl[0], EK_FORMAT);  // an empty heading: nothing to keep as a paragraph
    std::vector<Ed> eds;
    uint32_t escAt = UINT32_MAX;  // a backslash put in at the caret: the caret stays before it
    for (int32_t b : bl) {
        const BlockSrc& bs = d.blockSrc[b];
        const Block& blk = d.blocks[b];
        if (blk.heading == to) continue;
        const std::wstring h(to, L'#');
        if (to) {
            if (bs.flags & BS_ATX) {  // only the # run changes
                uint32_t e = bs.beg;
                while (e > bs.line && Blank(src[e - 1])) e--;
                uint32_t a = e;
                while (a > bs.line && src[a - 1] == L'#') a--;
                eds.push_back(Ed{a, e, h});
                continue;
            }
            // the lines become one: soft breaks a blank, hard breaks <br> (F15)
            SegSpan ss = SegsIn(d, b, TRange{blk.textOff, blk.textOff + blk.textLen});
            for (const SrcSeg* g = ss.b; g < ss.e; g++)
                if (LineBrk(src, g)) eds.push_back(Ed{g->s, g->s + g->sLen, d.text[g->t] == L'\n' ? L"<br>" : L" "});
            if (bs.flags & BS_SETEXT) eds.push_back(Ed{bs.end, bs.outerEnd, L""});
            const ContainerSrc* ct = bs.container >= 0 ? &d.containers[bs.container] : nullptr;
            uint32_t ls = LineStartOf(src, bs.beg);
            if (ct && ct->kind == CT_ALERT && Sub(src, ls, bs.beg).find(L"[!") != std::wstring::npos) {
                uint32_t x = bs.beg;  // a one-line alert: its tag on a line of its own (F24)
                while (x > ls && Blank(src[x - 1])) x--;
                eds.push_back(Ed{x, bs.beg, std::wstring(Eol(c, bs.beg)) + ContPrefix(d, src, b)});
            }
            std::wstring lead = bs.beg == bs.end && (bs.beg == 0 || !Blank(src[bs.beg - 1])) ? L" " : L"";  // `-` + `#`
            eds.push_back(Ed{bs.beg, bs.beg, lead + h + (blk.textLen ? L" " : L"")});
        } else if (bs.flags & BS_ATX) {  // back to a paragraph
            uint32_t a = bs.beg;
            while (a > bs.line && Blank(src[a - 1])) a--;
            while (a > bs.line && src[a - 1] == L'#') a--;
            if (!AtxClose(src, bs).empty()) eds.push_back(Ed{bs.end, bs.lineEnd, L""});
            eds.push_back(Ed{a, bs.beg, L""});
            const size_t n = eds.size();
            EscapeAt(eds, Sub(src, bs.beg, bs.end), bs.beg, true);
            if (eds.size() > n && eds.back().a == eds.back().b && eds.back().a == st.focus) escAt = st.focus;
            KeepApart(c, b, bs.lineEnd, eds);
        } else if (bs.flags & BS_SETEXT) {
            eds.push_back(Ed{bs.lineEnd, bs.outerEnd, L""});
            KeepApart(c, b, bs.outerEnd, eds);
        }
    }
    EditResult r = Built(c, st, std::move(eds), EK_FORMAT);
    if (r.refused.empty() && escAt != UINT32_MAX && st.anchor == st.focus)  // (before the backslash, wherever it went)
        r.after.focus = r.after.anchor = MapThrough(r.splices, escAt, true) - 1;
    SetShape(r, bl.front(), bl.back(), 0, false);  // the same blocks: only their levels (and line breaks) change
    return r;
}

// ------------------------------------------------------------------------------------------------ lists (§8.5)
namespace {
// an item's kind as the bar shows it (Q_EDIT_ACTIVE): a task item is a task, whatever its marker
bool OfKind(const ContainerSrc& it, int kind) {
    bool task = it.taskOff != UINT32_MAX;
    return kind == 9 ? task : !task && (kind == 8 ? it.delim != 0 : it.bullet != 0);
}
bool SameList(const Doc& d, int32_t x, int32_t item) {
    const ContainerSrc& a = d.containers[x];
    const ContainerSrc& b = d.containers[item];
    return a.kind == CT_ITEM && a.parent == b.parent && a.bullet == b.bullet && a.delim == b.delim;
}
// the items of the list level an item is in, in order
std::vector<int32_t> ListRun(const Doc& d, int32_t item) {
    int32_t first = item;
    for (int guard = 0; guard < 100000; guard++) {
        int32_t P = PrevInSource(d, (int32_t)d.containers[first].firstBlock), prev = -1;
        for (int32_t k = P >= 0 ? d.blockSrc[P].container : -1; k >= 0; k = d.containers[k].parent)
            if (k != first && SameList(d, k, item)) { prev = k; break; }
        if (prev < 0) break;
        first = prev;
    }
    std::vector<int32_t> v{first};
    for (int32_t cur = first;;) {
        int32_t nb = NextInSource(d, LastOf(d, cur)), x = nb >= 0 ? ItemOf(d, nb) : -1;
        if (x < 0 || !SameList(d, x, item)) break;
        v.push_back(x);
        cur = x;
    }
    return v;
}
// an item's continuation lines and children move by delta columns, at column col: its marker got wider or narrower
void Reindent(const Doc& d, const std::wstring& src, int32_t item, uint32_t col, int delta, std::vector<Ed>& eds) {
    if (!delta) return;
    const ContainerSrc& it = d.containers[item];
    std::vector<Splice> v;
    uint32_t from = SkipEol(src, LineEndOf(src, it.markOff)), to = d.blockSrc[LastOf(d, item)].outerEnd;
    if (from > to) return;
    Shift(src, from, to, col, delta, v);
    for (const Splice& sp : v) eds.push_back(Ed{sp.at, sp.at + (uint32_t)sp.removed.size(), sp.inserted});
}
// The column an item's content starts at with another marker of width w in its place (the blanks after it the same; a
// tab still goes to its tab stop): what its continuation lines and children move by is the difference (Phase 4 notes)
uint32_t ContentWith(const std::wstring& src, const ContainerSrc& it, uint32_t w) {
    uint32_t c = Col(src, it.markOff) + w, k = it.markOff + it.markLen, blanks = 0;
    for (; k < src.size() && Blank(src[k]); k++) c = src[k] == L'\t' ? (c + 4) & ~3u : c + 1, blanks++;
    if (!blanks || k >= src.size() || EolChar(src[k]) || c > Col(src, it.markOff) + w + 4)
        return Col(src, it.markOff) + w + 1;  // an empty item, or indented code in it: one blank counts
    return c;
}
}  // namespace

// Bulleted (7), numbered (8), task (9) lists on the touched text blocks: on - a marker after each block's container
// prefix, continuation lines indented to its content, the blank lines between them gone (one tight list), after a list
// of that kind right before them: its next items; off - when every block is an item of that kind: the markers go (the
// task boxes only, for tasks), continuation lines and children lose the indentation, blank lines keep the paragraphs
// apart, §7.4; another kind - with a caret the whole list level, with a selection its items: bullet ↔ number
// (re-indented by the marker's width, a box kept), a box put in. In a phantom: its style.
EditResult OpList(const EditCtx& c, const EditState& st, int kind) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    if (kind < 7 || kind > 9) return Refuse(st, "context");
    if (InPh(st)) {
        EditResult r = Nothing(st, EK_OTHER);
        r.after.phantom.style = (uint8_t)(st.phantom.style == kind ? 0 : kind);
        return r;
    }
    if (st.atom >= 0) return Refuse(st, "context");
    std::vector<int32_t> bl = TextBlocks(c, st);
    if (bl.empty()) return Refuse(st, "context");
    std::vector<int32_t> items, plain;
    bool allKind = true;
    for (int32_t b : bl) {
        int32_t it = ItemAround(d, b);
        if (it < 0) {
            plain.push_back(b);
            allKind = false;
            continue;
        }
        if (std::find(items.begin(), items.end(), it) == items.end()) items.push_back(it);
        allKind &= OfKind(d.containers[it], kind);
    }
    // the outermost of them: an item's children go with it
    items.erase(std::remove_if(items.begin(), items.end(), [&](int32_t it) {
        for (int32_t k = d.containers[it].parent; k >= 0; k = d.containers[k].parent)
            if (std::find(items.begin(), items.end(), k) != items.end()) return true;
        return false;
    }), items.end());
    std::vector<Ed> eds;
    if (allKind) {  // off
        for (size_t i = 0; i < items.size(); i++) {
            const ContainerSrc& it = d.containers[items[i]];
            const int32_t fb = (int32_t)it.firstBlock;
            const BlockSrc& fs = d.blockSrc[fb];
            // the marker, its blanks and a task's box go - not a heading's #, which is the item's text (Phase 4 notes)
            uint32_t e = it.markOff + it.markLen;
            while (e < fs.beg && Blank(src[e])) e++;
            if (it.taskOff != UINT32_MAX && it.taskOff + 2 <= fs.beg) {
                e = it.taskOff + 2;
                while (e < fs.beg && Blank(src[e])) e++;
            }
            eds.push_back(Ed{it.markOff, std::min(e, fs.beg), L""});
            // The continuation lines and the children come out to the column the item's own content now stands in -
            // its parent item's content, or the marker's column at the top: markers at the item's content column
            // would be indented code or lazy text under the paragraph it became (Phase 4 notes)
            const uint32_t base = it.parent >= 0 && d.containers[it.parent].kind == CT_ITEM ? d.containers[it.parent].contentCol
                                                                                            : Col(src, it.markOff);
            if (it.contentCol > base) Reindent(d, src, items[i], base, -(int)(it.contentCol - base), eds);
            if (Para(d, fb)) EscapeAt(eds, Sub(src, fs.beg, LineEndOf(src, fs.beg)), fs.beg, true);
            // blank lines keep the paragraphs apart - from each other, from the items left before and after them, and
            // from a paragraph the list interrupted (`Intro:⏎- a`: Phase 4 notes)
            const std::wstring E = Eol(c, it.markOff), blank = Strip(PrefixN(d, src, fb, (int)Chain(d, fb).size() - 1, nullptr));
            const int32_t last = LastOf(d, items[i]), nx = NextInSource(d, last), pv = PrevInSource(d, fb);
            uint32_t le = d.blockSrc[last].outerEnd, nl = SkipEol(src, le);
            if (nx >= 0 && nl > le && nl < src.size() && !BlankLine(src, nl, LineEndOf(src, nl))) eds.push_back(Ed{nl, nl, blank + E});
            uint32_t ls = LineStartOf(src, it.markOff), pe = BackEol(src, ls), pl = LineStartOf(src, pe);
            if (i == 0 && pv >= 0 && ls > 0 && !BlankLine(src, pl, pe) && (ItemAround(d, pv) >= 0 || ParaLine(d, pl)))
                eds.push_back(Ed{ls, ls, blank + E});
        }
        EditResult r = Built(c, st, std::move(eds), EK_FORMAT);
        SetShape(r, 0, -1, 0, true);  // every block as it was: only the markers and the indentation change
        return r;
    }
    // another kind: with a caret its whole list level, with a selection the items it touches
    std::vector<int32_t> change;
    if (st.anchor == st.focus && !items.empty()) change = ListRun(d, items[0]);
    else change = items;
    uint32_t num = 1;
    for (int32_t x : change) {
        const ContainerSrc& it = d.containers[x];
        if (kind == 9) {  // a box put in
            if (it.taskOff == UINT32_MAX) eds.push_back(Ed{d.blockSrc[it.firstBlock].beg, d.blockSrc[it.firstBlock].beg, L"[ ] "});
            continue;
        }
        if (it.taskOff != UINT32_MAX && (kind == 7 ? it.bullet != 0 : it.delim != 0)) {  // a task of that marker: plain
            uint32_t e = it.taskOff + 2;
            if (e < src.size() && Blank(src[e])) e++;
            eds.push_back(Ed{it.taskOff - 1, e, L""});
            num++;
            continue;
        }
        if (OfKind(it, kind)) {
            num++;
            continue;
        }
        std::wstring m = kind == 8 ? std::to_wstring(num++) + L"." : L"-";
        eds.push_back(Ed{it.markOff, it.markOff + it.markLen, m});
        Reindent(d, src, x, Col(src, it.markOff),
                 (int)ContentWith(src, it, (uint32_t)m.size()) - (int)ContentWith(src, it, it.markLen), eds);
    }
    // on: the blocks in no list - one tight list of them (the blank lines between them go), or the next items of a list
    // of that kind right before the first of them
    wchar_t bullet = L'-', delim = L'.';
    uint32_t n = 1;
    for (size_t i = 0; i < plain.size(); i++) {
        const int32_t b = plain[i];
        const BlockSrc& bs = d.blockSrc[b];
        const int32_t P = i ? plain[i - 1] : PrevInSource(d, b);
        uint32_t pe = P >= 0 ? d.blockSrc[P].outerEnd : 0;
        bool join = i > 0 && d.blockSrc[P].container == bs.container;
        if (!i && P >= 0) {
            int32_t pit = ItemAround(d, P);
            while (pit >= 0 && d.containers[pit].parent != bs.container) pit = d.containers[pit].parent;
            if (pit >= 0 && d.containers[pit].kind == CT_ITEM && OfKind(d.containers[pit], kind)) {
                const ContainerSrc& pc = d.containers[pit];
                bullet = pc.bullet ? pc.bullet : L'-';
                delim = pc.delim ? pc.delim : L'.';
                n = pc.number + 1;
                pe = d.blockSrc[LastOf(d, pit)].outerEnd;
                join = pc.tight;  // (a loose list stays loose: the blank line keeps the item in it)
            }
        }
        bool blanks = join && pe < bs.line;
        for (uint32_t l = SkipEol(src, pe); l < bs.line && blanks;) {
            uint32_t le = LineEndOf(src, l);
            blanks = BlankLine(src, l, le);
            l = SkipEol(src, le) > l ? SkipEol(src, le) : l + 1;
        }
        if (blanks) eds.push_back(Ed{pe, bs.line, Eol(c, pe)});
        std::wstring m = kind == 8 ? std::to_wstring(n++) + delim : std::wstring(1, bullet);
        const uint32_t w = (uint32_t)m.size() + 1;
        m += kind == 9 ? L" [ ] " : L" ";
        uint32_t at = WalkPrefix(d, src, b, bs.line, nullptr);
        eds.push_back(Ed{at, at, m});
        for (uint32_t l : MoreLines(src, bs, bs.outerEnd)) {
            uint32_t x = WalkPrefix(d, src, b, l, nullptr);
            eds.push_back(Ed{x, x, std::wstring(w, L' ')});
        }
        // Indented code right after the new item (only blank lines between) would be the item's paragraph text now:
        // it moves into the item with it, still code (Phase 4 notes)
        const int32_t N = i + 1 == plain.size() ? NextInSource(d, b) : -1;
        if (N >= 0 && d.blocks[N].kind == BK_CODE && !(d.blockSrc[N].flags & BS_FENCED) && d.blockSrc[N].container == bs.container) {
            bool blanks = true;
            for (uint32_t l = SkipEol(src, bs.outerEnd); l < d.blockSrc[N].line && blanks;) {
                const uint32_t le = LineEndOf(src, l), nx = SkipEol(src, le);
                blanks = BlankLine(src, WalkPrefix(d, src, b, l, nullptr), le);
                l = nx > l ? nx : l + 1;
            }
            for (uint32_t l = d.blockSrc[N].line; blanks && l <= d.blockSrc[N].outerEnd && l < src.size();) {
                const uint32_t le = LineEndOf(src, l), nx = SkipEol(src, le), x = WalkPrefix(d, src, N, l, nullptr);
                if (!BlankLine(src, x, le)) eds.push_back(Ed{x, x, std::wstring(w, L' ')});
                if (nx <= l) break;
                l = nx;
            }
        }
    }
    EditResult r = Built(c, st, std::move(eds), EK_FORMAT);
    SetShape(r, 0, -1, 0, true);  // every block as it was: only markers and indentation (Phase 4 notes)
    return r;
}

// ------------------------------------------------------------------------------------------------ quote (§8.6)
// On: every line of the touched blocks and the blank lines between them gets `> ` after its container prefix (a blank
// line `>`). Off (every block in a quote): the innermost quote they share loses one `>` (and the blank after it) on each
// of those lines; in the middle of the quote a blank line is kept on each side, and an alert loses its tag with it.
EditResult OpQuote(const EditCtx& c, const EditState& st) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    if (InPh(st)) {
        EditResult r = Nothing(st, EK_OTHER);
        r.after.phantom.style = (uint8_t)(st.phantom.style == 10 ? 0 : 10);
        return r;
    }
    if (st.atom >= 0) return Refuse(st, "context");
    std::vector<int32_t> bl = TextBlocks(c, st);
    if (bl.empty()) return Refuse(st, "context");
    // the innermost quote all of them are in (-1: none)
    int32_t q = -1;
    for (int32_t k = d.blockSrc[bl[0]].container; k >= 0 && q < 0; k = d.containers[k].parent) {
        if (d.containers[k].kind != CT_QUOTE && d.containers[k].kind != CT_ALERT) continue;
        bool shared = true;
        for (int32_t b : bl) shared &= InCont(d, b, k);
        if (shared) q = k;
    }
    // the lines: each block's, and the blank lines between two of them
    struct Line { uint32_t ls; int32_t b; bool blank; };
    std::vector<Line> lines;
    for (size_t i = 0; i < bl.size(); i++) {
        const BlockSrc& bs = d.blockSrc[bl[i]];
        if (i) {
            const BlockSrc& ps = d.blockSrc[bl[i - 1]];
            std::vector<Line> gap;
            bool blanks = true;
            for (uint32_t l = SkipEol(src, ps.outerEnd); l < bs.line && blanks;) {
                uint32_t le = LineEndOf(src, l);
                blanks = BlankLine(src, l, le);
                gap.push_back(Line{l, bl[i], true});
                l = SkipEol(src, le) > l ? SkipEol(src, le) : l + 1;
            }
            if (blanks)
                for (const Line& l : gap) lines.push_back(l);
        }
        lines.push_back(Line{bs.line, bl[i], false});
        for (uint32_t l : MoreLines(src, bs, bs.outerEnd)) lines.push_back(Line{l, bl[i], false});
    }
    std::vector<Ed> eds;
    if (q < 0) {  // on
        for (int32_t b : bl)  // a task item's box stands before its text: `- > [x] Task` loses the box (Phase 4 notes)
            if (TaskText(d, b)) return Refuse(st, "context");
        for (const Line& l : lines) {
            if (l.blank) {
                eds.push_back(Ed{l.ls, LineEndOf(src, l.ls), ContPrefix(d, src, l.b) + L">"});
                continue;
            }
            uint32_t at = WalkPrefix(d, src, l.b, l.ls, nullptr);
            // a lazy line (the containers' prefix left out) gets the whole prefix: `- > a⏎> b` would be two blocks
            const std::wstring pre = ContPrefix(d, src, l.b);
            if (l.ls != d.blockSrc[l.b].line && Col(src, at) < ColsOf(pre, 0, 0, (uint32_t)pre.size())) {
                eds.push_back(Ed{l.ls, at, pre + L"> "});
                continue;
            }
            eds.push_back(Ed{at, at, L"> "});
        }
        // A quote right above or below would take the new one in - and an alert there would become a plain quote with
        // its tag as text: a blank line keeps the two apart (Phase 4 notes)
        const std::wstring blank = Strip(ContPrefix(d, src, bl[0]));
        const uint32_t first = d.blockSrc[bl[0]].line, last = d.blockSrc[bl.back()].outerEnd, nx = SkipEol(src, last);
        auto quoted = [&](int32_t b, uint32_t l) {
            const uint32_t x = WalkPrefix(d, src, b, l, nullptr);
            return x < src.size() && src[x] == L'>';
        };
        if (first > 0 && quoted(bl[0], LineStartOf(src, BackEol(src, first)))) eds.push_back(Ed{first, first, blank + Eol(c, first)});
        if (nx > last && nx < src.size() && quoted(bl.back(), nx)) eds.push_back(Ed{nx, nx, blank + Eol(c, last)});
        EditResult r = Built(c, st, std::move(eds), EK_FORMAT);
        SetShape(r, 0, -1, 0, true);
        return r;
    }
    // off: which of the line's quote markers is q's
    size_t qi = 0;
    for (const ContainerSrc* k : Chain(d, bl[0])) {
        if (k == &d.containers[q]) break;
        qi += k->kind == CT_QUOTE || k->kind == CT_ALERT;
    }
    for (const Line& l : lines) {
        std::vector<uint32_t> marks;
        WalkPrefix(d, src, l.b, l.ls, &marks);
        if (qi >= marks.size() || marks[qi] == UINT32_MAX) continue;  // a lazy line
        uint32_t m = marks[qi], e = m + 1;
        if (e < src.size() && Blank(src[e])) e++;
        eds.push_back(Ed{m, e, L""});
    }
    const ContainerSrc& Q = d.containers[q];
    const BlockSrc& fs = d.blockSrc[bl[0]];
    const BlockSrc& ls = d.blockSrc[bl.back()];
    if (Q.kind == CT_ALERT) {  // the alert's tag goes with its quote level - when its first text is in the selection
        int32_t fb = (int32_t)Q.firstBlock;
        while (fb < (int32_t)Q.lastBlock && (d.blockSrc[fb].flags & BS_SYNTH)) fb++;
        if (fb == bl[0]) {
            uint32_t l0 = LineStartOf(src, fs.beg);
            size_t tag = Sub(src, l0, fs.beg).find(L"[!");
            if (tag != std::wstring::npos) eds.push_back(Ed{l0 + (uint32_t)tag, fs.beg, L""});
            else if (fs.line > 0) eds.push_back(Ed{LineStartOf(src, BackEol(src, fs.line)), fs.line, L""});
        }
    }
    // in the middle of the quote: a blank line of it on each side
    const std::wstring qblank = Strip(ContPrefix(d, src, bl[0]));
    if (fs.line > 0) {
        uint32_t pe = BackEol(src, fs.line), pl = LineStartOf(src, pe);
        if (pl >= d.blockSrc[Q.firstBlock].line && !BlankLine(src, pl, pe) && InCont(d, PrevInSource(d, bl[0]), q))
            eds.push_back(Ed{fs.line, fs.line, qblank + Eol(c, pe)});
    }
    int32_t nx = NextInSource(d, bl.back());
    uint32_t nl = SkipEol(src, ls.outerEnd);
    if (nx >= 0 && InCont(d, nx, q) && nl < src.size() && !BlankLine(src, nl, LineEndOf(src, nl)))
        eds.push_back(Ed{ls.outerEnd, ls.outerEnd, std::wstring(Eol(c, ls.outerEnd)) + qblank});
    EditResult r = Built(c, st, std::move(eds), EK_FORMAT);
    SetShape(r, 0, -1, 0, true);
    return r;
}

// ------------------------------------------------------------------------------------------------ code block (§8.7)
namespace {
// A text block as code lines: its source lines without their container prefixes - a heading without its markers and
// its closing sequence, a setext heading without its underline. The source, not the rendered text: link addresses,
// emphasis and pictures stay in the code as text, and a one-key toggle loses nothing (Phase 4 notes). *caretLine /
// *caretCol: where source offset s lands.
std::vector<std::wstring> CodeLines(const Doc& d, const std::wstring& src, int32_t b, uint32_t s, int* caretLine, uint32_t* caretCol) {
    const BlockSrc& bs = d.blockSrc[b];
    const uint32_t end = (bs.flags & BS_ATX) ? bs.end : bs.lineEnd;
    std::vector<std::wstring> v;
    for (uint32_t ls = bs.beg;;) {
        const uint32_t le = std::min(LineEndOf(src, ls), end);
        if (caretLine && *caretLine < 0 && s >= ls && s <= le) {
            *caretLine = (int)v.size();
            *caretCol = s - ls;
        }
        v.push_back(Sub(src, ls, le));
        const uint32_t nx = SkipEol(src, le);
        if (le >= end || nx <= le || nx > end) break;
        ls = WalkPrefix(d, src, b, nx, nullptr);
    }
    return v;
}
}  // namespace

// Text blocks → one fenced block of their rendered lines (a blank line between blocks); in a code block → unwrapped: a
// paragraph per non-empty line, escaped; an empty phantom → an empty fence pair; a table cell or an object → an empty
// fence after it.
EditResult OpCodeBlock(const EditCtx& c, const EditState& st) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const std::vector<std::wstring> empty{L"```", L"", L"```"};
    if (InPh(st) || st.atom >= 0) return InsertBlock(c, st, empty, 1, 0, false);
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr);
    if (!ValidBlock(d, f.block)) return Refuse(st, "context");
    if (TableOf(d, f.block)) return InsertBlock(c, st, empty, 1, 0, false);
    const Block& fb = d.blocks[f.block];
    const BlockSrc& fs = d.blockSrc[f.block];
    if (fb.kind == BK_CODE) {  // unwrapped
        std::vector<std::wstring> paras;
        size_t caretPara = 0;
        for (uint32_t t = fb.textOff; t <= fb.textOff + fb.textLen;) {
            uint32_t e = t;
            while (e < fb.textOff + fb.textLen && d.text[e] != L'\n') e++;
            std::wstring line = d.text.substr(t, e - t);
            if (!Strip(line).empty()) {
                if (f.t >= t) caretPara = paras.size();
                std::wstring esc = EscapeMd(line, false);
                uint32_t drop;
                int k = Trigger(esc, true, &drop);
                if (k >= 0) esc.insert(esc.begin() + k, L'\\');
                else if (drop) esc.erase(0, drop);
                paras.push_back(esc);
            }
            t = e + 1;
        }
        if (paras.empty()) return GoneToPhantom(c, st, f.block, EK_STRUCT);
        uint32_t at;
        PrefixN(d, src, f.block, -1, &at);
        const std::wstring E = Eol(c, at), sep = E + BlankPrefix(d, src, f.block) + E + ContPrefix(d, src, f.block);
        std::wstring ins;
        uint32_t caret = 0;
        for (size_t k = 0; k < paras.size(); k++) {
            if (k) ins += sep;
            if (k == caretPara) caret = (uint32_t)ins.size();
            ins += paras[k];
        }
        EditResult r = Nothing(st, EK_STRUCT);
        r.splices.push_back(Replace(src, at, fs.outerEnd, ins));
        r.after.focus = r.after.anchor = at + caret;
        r.after.pendOn = r.after.pendOff = 0;
        SetShape(r, f.block, f.block, (int32_t)paras.size() - 1, false);
        return Carry(std::move(r));
    }
    if (fs.flags & BS_FOOTNOTE) return Refuse(st, "context");
    std::vector<int32_t> bl = TextBlocks(c, st);
    if (bl.empty()) return Refuse(st, "context");
    for (int32_t b : bl)
        if (d.blockSrc[b].container != d.blockSrc[bl[0]].container) return Refuse(st, "format");  // one container only
    // Everything from the first block to the last goes into the fence: a table, code, an HTML block, a folded block or
    // a line no block owns (a reference definition, a comment) between them would be lost with it - refused then
    // (Phase 4 notes)
    const uint32_t from = d.blockSrc[bl[0]].line, to = d.blockSrc[bl.back()].outerEnd;
    for (uint32_t k : d.blockOrder)
        if (d.blockSrc[k].line >= from && d.blockSrc[k].line <= to && std::find(bl.begin(), bl.end(), (int32_t)k) == bl.end())
            return Refuse(st, "format");
    for (uint32_t l = from; l < to;) {
        const uint32_t le = LineEndOf(src, l), nx = SkipEol(src, le);
        if (!BlankLine(src, WalkPrefix(d, src, bl[0], l, nullptr), le) && !Owned(d, l)) return Refuse(st, "format");
        if (nx <= l) break;
        l = nx;
    }
    std::vector<std::wstring> lines;
    int caretLine = -1;
    uint32_t caretCol = 0;
    for (size_t i = 0; i < bl.size(); i++) {
        if (i) lines.emplace_back();
        int cl = -1;
        uint32_t cc = 0;
        std::vector<std::wstring> v = CodeLines(d, src, bl[i], st.focus, &cl, &cc);
        if (cl >= 0 && caretLine < 0) {
            caretLine = (int)lines.size() + cl;
            caretCol = cc;
        }
        for (std::wstring& l : v) lines.push_back(std::move(l));
    }
    std::wstring all;
    for (auto& l : lines) all += l + L"\n";
    const std::wstring fence = Backticks(all, 3);
    uint32_t at;
    PrefixN(d, src, bl[0], -1, &at);
    const std::wstring E = Eol(c, at), pre = ContPrefix(d, src, bl[0]), bpre = BlankPrefix(d, src, bl[0]);
    std::wstring ins = fence;
    uint32_t caret = 0;
    for (size_t k = 0; k < lines.size(); k++) {
        ins += E + (lines[k].empty() ? bpre : pre);
        if ((int)k == std::max(caretLine, 0)) caret = (uint32_t)ins.size() + std::min<uint32_t>(caretCol, (uint32_t)lines[k].size());
        ins += lines[k];
    }
    ins += E + pre + fence;
    EditResult r = Nothing(st, EK_STRUCT);
    r.splices.push_back(Replace(src, at, d.blockSrc[bl.back()].outerEnd, ins));
    r.after.focus = r.after.anchor = at + caret;
    r.after.atom = -1;
    r.after.pendOn = r.after.pendOff = 0;
    SetShape(r, bl.front(), bl.back(), 1 - (int32_t)bl.size(), false);  // they become one block; nothing else changes
    return Carry(std::move(r));  // (a phantom after them: after the fence - the final gate)
}

// The code language (§8.7): the opening fence's info string; an indented block becomes a fenced one
EditResult OpCodeLang(const EditCtx& c, const EditState& st, std::wstring_view info) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr);
    if (!ValidBlock(d, f.block) || d.blocks[f.block].kind != BK_CODE) return Refuse(st, "context");
    const BlockSrc& bs = d.blockSrc[f.block];
    std::wstring lang(info);
    for (wchar_t& ch : lang)
        if (EolChar(ch) || ch == L'`') ch = L' ';
    lang = Strip(lang);
    while (!lang.empty() && Blank(lang.front())) lang.erase(0, 1);
    EditResult r = Nothing(st, EK_FORMAT);
    if (bs.flags & BS_FENCED) {
        uint32_t p = WalkPrefix(d, src, f.block, bs.line, nullptr), le = LineEndOf(src, bs.line);
        while (p < le && Blank(src[p])) p++;
        wchar_t fc = p < le ? src[p] : L'`';
        while (p < le && src[p] == fc) p++;
        r.splices.push_back(Replace(src, p, le, lang));
    } else {  // indented: fenced, its four columns of indentation gone
        uint32_t at;
        PrefixN(d, src, f.block, -1, &at);
        const std::wstring E = Eol(c, at), pre = ContPrefix(d, src, f.block);
        std::wstring code = d.text.substr(d.blocks[f.block].textOff, d.blocks[f.block].textLen), body;
        const std::wstring fence = Backticks(code, 3);
        body = fence + lang;
        for (size_t i = 0, e; i <= code.size(); i = e + 1) {
            e = code.find(L'\n', i);
            if (e == std::wstring::npos) e = code.size();
            std::wstring l = code.substr(i, e - i);
            body += E + (l.empty() ? Strip(pre) : pre) + l;
        }
        body += E + pre + fence;
        r.splices.push_back(Replace(src, at, bs.outerEnd, body));
    }
    r.after.focus = MapThrough(r.splices, st.focus, true);
    r.after.anchor = MapThrough(r.splices, st.anchor, true);
    return Carry(std::move(r), true);  // (a phantom after the block stays after its closing fence - the final gate)
}

// ------------------------------------------------------------------------------------------------ inserts (§8.8)
EditResult OpInsertTable(const EditCtx& c, const EditState& st, int rows, int cols) {
    rows = std::clamp(rows, 1, 64);
    cols = std::clamp(cols, 1, 15);
    std::wstring row = L"|", delim = L"|";
    for (int k = 0; k < cols; k++) {
        row += L"  |";
        delim += L" --- |";
    }
    std::vector<std::wstring> lines{row, delim};
    for (int k = 1; k < rows; k++) lines.push_back(row);
    return InsertBlock(c, st, lines, 0, 2, false);  // the caret in the first header cell
}

// An inline formula: the selection (in one block) as its TeX, else `x`; a blank apart from a letter or digit beside it
// (F9-4). A formula block: `$$`, `x`, `$$` after the block.
EditResult OpInsertFormula(const EditCtx& c, const EditState& st, bool block) {
    const Doc& d = c.doc;
    if (block) return InsertBlock(c, st, {L"$$", L"x", L"$$"}, 1, 0, false);
    if (st.atom >= 0) return Refuse(st, "context");
    if (InPh(st)) return Insert(c, st, L"$x$", false, EK_STRUCT);
    TextPos A, B;
    Ends(c, st, &A, &B);
    TRange rg;
    if (!ValidBlock(d, A.block) || !RangeOf(d, A, &rg) || d.blocks[A.block].kind == BK_CODE) return Refuse(st, "context");
    std::wstring tex = L"x";
    if (st.anchor != st.focus) {
        if (A.block != B.block || A.cell != B.cell) return Refuse(st, "format");
        tex = Rendered(d, c.src, A.block, rg, A.t, B.t);
        while (!tex.empty() && Ws(tex.back())) tex.pop_back();
        while (!tex.empty() && Ws(tex.front())) tex.erase(0, 1);
        if (tex.empty()) tex = L"x";
        // the text as TeX: a `$` would end the formula (\$), and in a cell a `|` the cell (\vert, Phase 4 notes)
        std::wstring o;
        for (size_t i = 0; i < tex.size(); i++)
            o += tex[i] == L'$' ? L"\\$" : tex[i] == L'|' && A.cell >= 0 ? (i + 1 < tex.size() && iswalpha(tex[i + 1]) ? L"\\vert " : L"\\vert")
                                                                           : std::wstring(1, tex[i]);
        tex = o;
    }
    std::wstring t = L"$" + tex + L"$";
    const uint32_t s0 = SrcOfText(d, c.src, A, st.anchor != st.focus ? MAP_OUTER_START : MAP_CARET);
    if (A.t > rg.beg && iswalnum(d.text[A.t - 1])) t = L" " + t;
    else if (s0 != UINT32_MAX && s0 > 0 && s0 <= c.src.size() && c.src[s0 - 1] == L'\\') t = L" " + t;  // (`\$` is no formula)
    if (B.t < rg.end && iswalnum(d.text[B.t])) t += L" ";
    if (st.anchor != st.focus) return Carry(CutRange(c, st, t, EK_STRUCT));
    return Carry(Insert(c, st, t, false, EK_STRUCT));
}

EditResult OpInsertDiagram(const EditCtx& c, const EditState& st, int tmpl) {
    // §8.8's templates, language-neutral, indented by four (a class's members and a mind map's branches deeper)
    static const wchar_t* const kT[9] = {
        L"flowchart TD\n    A[Start] --> B{Choice}\n    B -->|Yes| C[Do it]\n    B -->|No| D[End]",
        L"sequenceDiagram\n    Alice->>Bob: Hello\n    Bob-->>Alice: Hi",
        L"classDiagram\n    class Animal {\n        +name\n        +speak()\n    }\n    Animal <|-- Cat",
        L"stateDiagram-v2\n    [*] --> Idle\n    Idle --> Running : start\n    Running --> Idle : stop\n    Running --> [*]",
        L"erDiagram\n    CUSTOMER ||--o{ ORDER : places\n    ORDER ||--|{ ITEM : contains",
        L"gantt\n    title Plan\n    dateFormat YYYY-MM-DD\n    section Work\n    Task A :a1, 2026-01-01, 7d\n    Task B :after a1, 5d",
        L"pie title Share\n    \"A\" : 60\n    \"B\" : 40",
        L"mindmap\n    root((Idea))\n      Branch A\n      Branch B",
        L"timeline\n    title History\n    2025 : Start\n    2026 : Growth",
    };
    std::vector<std::wstring> lines{L"```mermaid"};
    for (const wchar_t* p = kT[std::clamp(tmpl, 0, 8)]; *p;) {
        const wchar_t* e = wcschr(p, L'\n');
        lines.emplace_back(p, e ? e - p : wcslen(p));
        p = e ? e + 1 : p + wcslen(p);
    }
    lines.push_back(L"```");
    return InsertBlock(c, st, lines, 1, 0, false);
}

// A picture: in the line at the caret, or a paragraph of its own in a phantom, after code or an object. Several
// (dropped or pasted files): dest and alt hold one per line, and they go in one after the other, blank-separated.
EditResult OpInsertImage(const EditCtx& c, const EditState& st, std::wstring_view dest, std::wstring_view alt) {
    const Doc& d = c.doc;
    std::wstring t;
    for (size_t i = 0, j = 0; i <= dest.size();) {
        size_t e = std::min(dest.find(L'\n', i), dest.size()), ae = std::min(alt.find(L'\n', j), alt.size());
        std::wstring a;
        for (wchar_t ch : alt.substr(std::min(j, alt.size()), ae - std::min(j, alt.size()))) {
            if (ch == L'[' || ch == L']' || ch == L'\\') a += L'\\';
            a += EolChar(ch) ? L' ' : ch;
        }
        t += (t.empty() ? L"![" : L" ![") + a + L"](" + std::wstring(dest.substr(i, e - i)) + L")";
        i = e + 1;
        j = ae + 1;
    }
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr);
    if (!InPh(st) && (st.atom >= 0 || !ValidBlock(d, f.block) || d.blocks[f.block].kind == BK_CODE))
        return InsertBlock(c, st, {t}, 0, (uint32_t)t.size(), false);
    // At a paragraph's end a picture is a paragraph of its own after it: glued to the text it would be a picture in
    // its last line, as tall as the picture (Phase 4 notes). In the middle of a line it stays in the line.
    if (!InPh(st) && st.anchor == st.focus && Para(d, f.block) && f.cell < 0 && d.blocks[f.block].textLen &&
        f.t >= LastStop(d, f.block))
        return InsertBlock(c, st, {t}, 0, (uint32_t)t.size(), false);
    return Carry(Insert(c, st, t, false, EK_STRUCT));
}

// A rule after the block, and a new paragraph after the rule with the caret in it (UX-3)
EditResult OpInsertHr(const EditCtx& c, const EditState& st) { return InsertBlock(c, st, {L"---"}, 0, 3, true); }

// ------------------------------------------------------------------------------------------------ tables (§8.9)
// Rows above / below, columns left / right of the caret's, the row or the column deleted, the column aligned, the
// table deleted - on the rows' pipes (tableSrc). The caret stays in its cell, or the nearest one.
EditResult OpTable(const EditCtx& c, const EditState& st, int op) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr);
    const Table* tb = ValidBlock(d, f.block) ? TableOf(d, f.block) : nullptr;
    if (!tb || f.cell < 0 || d.blocks[f.block].aux >= d.tableSrc.size()) return Refuse(st, "context");
    const TableSrc& ts = d.tableSrc[d.blocks[f.block].aux];
    const uint32_t cols = tb->cols, r0 = (uint32_t)f.cell / cols, c0 = (uint32_t)f.cell % cols;
    if (ts.rows.size() != tb->rows + 1) return Refuse(st, "table");
    auto rowOf = [&](uint32_t r) -> const RowSrc& { return ts.rows[r ? r + 1 : 0]; };
    const std::wstring pre = ContPrefix(d, src, f.block);
    std::wstring blank = L"|";
    for (uint32_t k = 0; k < cols; k++) blank += L"  |";
    std::vector<Ed> eds;
    // a row line's pipes: the one before cell k and the one after it (UINT32_MAX none), its cell count
    struct Pipes { bool lead, trail; uint32_t n, first, end; const std::vector<uint32_t>* v; };
    auto pipesOf = [&](const RowSrc& rs) {
        Pipes p{false, false, 0, rs.contentStart, rs.lineEnd, &rs.pipes};
        while (p.first < rs.lineEnd && Blank(src[p.first])) p.first++;
        while (p.end > p.first && Blank(src[p.end - 1])) p.end--;
        p.lead = !rs.pipes.empty() && rs.pipes.front() == p.first;
        p.trail = !rs.pipes.empty() && rs.pipes.back() + 1 == p.end && (rs.pipes.size() > 1 || !p.lead);
        p.n = (uint32_t)rs.pipes.size() + 1 - p.lead - p.trail;
        return p;
    };
    auto before = [&](const Pipes& p, uint32_t k) -> uint32_t {
        if (p.lead) return (*p.v)[k];
        return k ? (*p.v)[k - 1] : UINT32_MAX;
    };
    auto after = [&](const Pipes& p, uint32_t k) -> uint32_t {
        size_t i = p.lead ? k + 1 : k;
        return i < p.v->size() ? (*p.v)[i] : UINT32_MAX;
    };
    EditResult r = Nothing(st, EK_STRUCT);
    switch (op) {
    case 0:  // a row above (never above the header)
        if (!r0) return Refuse(st, "table");
        eds.push_back(Ed{rowOf(r0).lineStart, rowOf(r0).lineStart, pre + blank + Eol(c, rowOf(r0).lineStart)});
        break;
    case 1: {  // a row below (under the header: after the delimiter row)
        const RowSrc& rs = r0 ? rowOf(r0) : ts.rows[1];
        eds.push_back(Ed{rs.lineEnd, rs.lineEnd, std::wstring(Eol(c, rs.lineEnd)) + pre + blank});
        break;
    }
    case 2: case 3:  // a column left / right of the caret's
        for (size_t i = 0; i < ts.rows.size(); i++) {
            Pipes p = pipesOf(ts.rows[i]);
            const bool dl = i == 1;
            if (c0 >= p.n) continue;  // too short to have that cell
            uint32_t at = op == 2 ? before(p, c0) : after(p, c0);
            if (at != UINT32_MAX) eds.push_back(Ed{at + 1, at + 1, dl ? L" --- |" : L"  |"});
            else if (op == 2) eds.push_back(Ed{p.first, p.first, dl ? L"| --- |" : L"|  |"});
            else eds.push_back(Ed{p.end, p.end, dl ? L" | --- |" : L" |  |"});
        }
        break;
    case 4: {  // the row (never the header)
        if (!r0) return Refuse(st, "table");
        const RowSrc& rs = rowOf(r0);
        uint32_t e = SkipEol(src, rs.lineEnd);
        if (e > rs.lineEnd) eds.push_back(Ed{rs.lineStart, e, L""});
        else eds.push_back(Ed{BackEol(src, rs.lineStart), rs.lineEnd, L""});
        break;
    }
    case 5:  // the column: its cells and one pipe beside each (a table of one column goes whole)
        if (cols <= 1) return OpTable(c, st, 9);
        for (size_t i = 0; i < ts.rows.size(); i++) {
            Pipes p = pipesOf(ts.rows[i]);
            if (c0 >= p.n) continue;
            uint32_t b = before(p, c0), a = after(p, c0);
            bool last = c0 + 1 == p.n;
            if (a != UINT32_MAX && (!last || b == UINT32_MAX)) eds.push_back(Ed{b == UINT32_MAX ? p.first : b + 1, a + 1, L""});
            else if (b != UINT32_MAX) eds.push_back(Ed{b, a == UINT32_MAX ? p.end : a, L""});
            // one column left of rows without outer pipes: they get them - a row with no pipe is no table row
            // (`a | b⏎--|--` would become a setext heading, Phase 4 notes)
            if (cols == 2 && !p.lead && !p.trail) {
                eds.push_back(Ed{p.first, p.first, L"| "});
                eds.push_back(Ed{p.end, p.end, L" |"});
            }
        }
        break;
    case 6: case 7: case 8: {  // the column's alignment in the delimiter row
        Pipes p = pipesOf(ts.rows[1]);
        if (c0 >= p.n) return Refuse(st, "table");
        uint32_t b = before(p, c0), a = after(p, c0), x = b == UINT32_MAX ? p.first : b + 1, y = a == UINT32_MAX ? p.end : a;
        while (x < y && Blank(src[x])) x++;
        while (y > x && Blank(src[y - 1])) y--;
        const bool L = op == 6 || op == 7, R = op == 7 || op == 8;
        size_t w = y - x, dashes = std::max<size_t>(3, w > (size_t)(L + R) ? w - L - R : 0);
        eds.push_back(Ed{x, y, (L ? L":" : L"") + std::wstring(dashes, L'-') + (R ? L":" : L"")});
        break;
    }
    case 9: {  // the whole table; the caret to the end of the block before, or a phantom where it was
        const BlockSrc& bs = d.blockSrc[f.block];
        int32_t P = PrevInSource(d, f.block);
        if (P < 0 || !TextBlk(d, P)) return GoneToPhantom(c, st, f.block, EK_STRUCT);
        EditResult del = DropLines(c, st, f.block, bs.line, SkipEol(src, bs.outerEnd));
        del.kind = EK_STRUCT;
        uint32_t s = SrcOfText(d, src, BlockEdge(d, P, true), MAP_CARET);
        if (s != UINT32_MAX) del.after.focus = del.after.anchor = s;
        return del;
    }
    default: return Refuse(st, "context");
    }
    (void)r;
    EditResult res = Built(c, st, std::move(eds), EK_STRUCT);
    if (!res.refused.empty()) return res;
    // the caret stays in its cell: a column put in beside it does not take it along
    res.after.focus = res.after.anchor = MapThrough(res.splices, st.focus, false);
    int32_t to = -1;  // or it goes to the nearest one: the same column of the row that took the deleted one's place, the
    bool end = false; // end of the cell left of the deleted column (its start when it was the first)
    if (op == 4) to = (int32_t)((r0 + 1 < tb->rows ? r0 + 1 : r0 - 1) * cols + c0);
    if (op == 5) {
        to = (int32_t)(r0 * cols + (c0 ? c0 - 1 : c0 + 1));
        end = c0 > 0;
    }
    if (to >= 0) {
        const Cell& cl = d.cells[tb->cellOff + to];
        uint32_t s = SrcOfText(d, src, TextPos{end ? cl.textOff + cl.textLen : cl.textOff, f.block, to}, MAP_CARET);
        if (s != UINT32_MAX) res.after.focus = res.after.anchor = MapThrough(res.splices, s, !end);
    }
    return res;
}

// ------------------------------------------------------------------------------------------------ links (§8.3)
namespace {
// the Markdown link or autolink whose text holds text position p, its edges included (the innermost)
const SpanSrc* LinkSpan(const Doc& d, const TextPos& p) {
    TRange rg;
    if (!ValidBlock(d, p.block) || !RangeOf(d, p, &rg)) return nullptr;
    const SpanSrc* best = nullptr;
    for (const SpanSrc* sp : SpansIn(d, p, rg))
        if (sp->type == MD_SPAN_A && !(sp->flags & SF_UNCLOSED) && sp->tBeg <= p.t && p.t <= sp->tEnd &&
            (!best || sp->tBeg >= best->tBeg))
            best = sp;
    return best;
}
// The destination in `(dest "title")` from p on (up to end): its token, <…> included
bool DestToken(const std::wstring& src, uint32_t p, uint32_t end, uint32_t* tb, uint32_t* te) {
    while (p < end && (Blank(src[p]) || EolChar(src[p]))) p++;
    *tb = p;
    if (p < end && src[p] == L'<') {
        while (++p < end && src[p] != L'>' && !EolChar(src[p])) {}
        *te = p + 1;
        return p < end && src[p] == L'>';
    }
    for (int depth = 0; p < end; p++) {
        wchar_t ch = src[p];
        if (ch == L'\\' && p + 1 < end) p++;
        else if (Blank(ch) || EolChar(ch) || (ch == L')' && depth-- == 0)) break;
        else if (ch == L'(') depth++;
    }
    *te = p;
    return true;
}
std::wstring Unwrap(const std::wstring& src, uint32_t tb, uint32_t te) {
    return te >= tb + 2 && src[tb] == L'<' && src[te - 1] == L'>' ? Sub(src, tb + 1, te - 1) : Sub(src, tb, te);
}
// An address as a destination: <…> when it holds blanks, '<' or '>', or parentheses that do not balance - or, a picture's
// path, any (§8.8) - with '<' and '>' percent-encoded inside; bare otherwise
std::wstring DestOf(std::wstring_view url, bool path) {
    bool angle = false;
    int depth = 0;
    std::wstring in;
    for (size_t i = 0; i < url.size(); i++) {
        const wchar_t ch = url[i];
        if (EolChar(ch)) continue;
        angle |= Blank(ch) || ch == L'<' || ch == L'>' || (path && (ch == L'(' || ch == L')'));
        if (ch == L'(') depth++;
        if (ch == L')' && --depth < 0) angle = true;
        in += ch == L'<' ? L"%3C" : ch == L'>' ? L"%3E" : std::wstring(1, ch);
        // a backslash before punctuation, or at the end, would escape it (`dir\(1)` lost its `\`, `C:\dir\` its `)`):
        // written twice it stays itself (Phase 4 notes)
        if (ch == L'\\' && (i + 1 == url.size() || (url[i + 1] < 0x80 && Punct(url[i + 1])))) in += L'\\';
    }
    return angle || depth ? L"<" + in + L">" : in;
}
// labels match without regard to case and runs of blanks (CommonMark)
std::wstring LabelKey(std::wstring_view l) {
    std::wstring k;
    for (wchar_t ch : l) {
        if (SpaceChar(ch) || EolChar(ch)) {
            if (!k.empty() && k.back() != L' ') k += L' ';
        } else {
            k += (wchar_t)towlower(ch);
        }
    }
    while (!k.empty() && k.back() == L' ') k.pop_back();
    return k;
}
// The definition `[label]: dest` of a label (on a line of its own, after quote markers and up to three blanks): the
// destination's token - on the same line or the next
bool Definition(const std::wstring& src, std::wstring_view label, uint32_t* tb, uint32_t* te) {
    const std::wstring key = LabelKey(label);
    for (uint32_t l = 0; l < src.size();) {
        uint32_t le = LineEndOf(src, l), p = l, q;
        while (p < le && (Blank(src[p]) || src[p] == L'>')) p++;
        for (q = p + 1; p < le && src[p] == L'[' && q < le && src[q] != L']'; q++) q += src[q] == L'\\';
        if (p < le && src[p] == L'[' && q + 1 < le && src[q] == L']' && src[q + 1] == L':' && LabelKey(Sub(src, p + 1, q)) == key) {
            uint32_t e = q + 2;
            while (e < le && Blank(src[e])) e++;
            if (e == le) e = SkipEol(src, le);  // (the destination on the next line)
            return DestToken(src, e, LineEndOf(src, e), tb, te) && *te > *tb;
        }
        uint32_t nx = SkipEol(src, le);
        if (nx <= l) break;
        l = nx;
    }
    return false;
}
// the label of a reference link or picture: `[t][label]`, else its text (`[t][]`, `[t]`)
std::wstring RefLabel(const std::wstring& src, const SpanSrc& sp) {
    std::wstring close = Sub(src, sp.closeBeg, sp.closeEnd);
    return close.size() > 3 && close[1] == L'[' ? close.substr(2, close.size() - 3) : Sub(src, sp.openEnd, sp.closeBeg);
}
std::wstring EscapeBrackets(std::wstring_view t) {
    std::wstring o;
    for (wchar_t ch : t) {
        if (ch == L'[' || ch == L']') o += L'\\';
        if (!EolChar(ch)) o += ch;
    }
    return o;
}
}  // namespace

bool LinkOfCaret(const EditCtx& c, const EditState& st, std::wstring* dest, std::wstring* label, bool* autolink, TextPos* t0,
                 TextPos* t1) {
    TextPos A, B;
    Ends(c, st, &A, &B);
    const SpanSrc* sp = st.atom < 0 && !InPh(st) ? LinkSpan(c.doc, A) : nullptr;
    if (!sp) return false;
    const std::wstring& src = c.src;
    uint32_t tb, te;
    if (t0) *t0 = TextPos{sp->tBeg, A.block, A.cell};
    if (t1) *t1 = TextPos{sp->tEnd, A.block, A.cell};
    label->clear();
    dest->clear();
    *autolink = (sp->flags & SF_AUTOLINK) != 0;
    if (*autolink) *dest = Sub(src, sp->openEnd, sp->closeBeg);
    else if (!(sp->flags & SF_REF)) { if (DestToken(src, sp->closeBeg + 2, sp->closeEnd - 1, &tb, &te)) *dest = Unwrap(src, tb, te); }
    else if (Definition(src, *label = RefLabel(src, *sp), &tb, &te)) *dest = Unwrap(src, tb, te);
    return true;
}

// Ctrl+K's link (§8.3): a selection in one block becomes `[text](url)` - links inside it lose their delimiters, spans it
// cuts in two are taken in whole, brackets of the text are escaped; with no selection the link at the caret gets the
// address (a reference link's definition once the notice was confirmed; an autolink becomes a link of its own), else
// `[url](url)` is put in at the caret.
EditResult OpLink(const EditCtx& c, const EditState& st, std::wstring_view url, bool confirmRefDef) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    std::wstring u(url);
    while (!u.empty() && Ws(u.back())) u.pop_back();
    while (!u.empty() && Ws(u.front())) u.erase(0, 1);
    if (u.empty() || st.atom >= 0) return Refuse(st, "link");
    const std::wstring dest = DestOf(u, false);
    TextPos A, B;
    Ends(c, st, &A, &B);
    if (st.anchor == st.focus || InPh(st)) {
        const SpanSrc* sp = InPh(st) ? nullptr : LinkSpan(d, A);
        if (!sp) {
            EditResult r = Carry(Insert(c, st, L"[" + EscapeBrackets(u) + L"](" + dest + L")", false, EK_FORMAT));
            // right after a `!` the link would be a picture: that `!` is escaped (Phase 4 notes)
            const uint32_t s = r.splices.empty() ? 0 : r.splices.back().at;
            if (r.refused.empty() && s > 0 && s <= src.size() && src[s - 1] == L'!' && (s < 2 || src[s - 2] != L'\\')) {
                r.splices.push_back(Splice{s - 1, L"", L"\\"});
                r.after.focus++;
                r.after.anchor++;
            }
            return r;
        }
        uint32_t tb, te;
        std::vector<Ed> eds;
        if (sp->flags & SF_AUTOLINK) {  // its text stays, and gets the address; the caret stays in it
            eds.push_back(Ed{sp->openBeg, sp->closeEnd, L"[" + Sub(src, sp->openEnd, sp->closeBeg) + L"](" + dest + L")"});
            EditResult r = Built(c, st, std::move(eds), EK_FORMAT);
            r.after.focus = r.after.anchor = sp->openBeg + 1 + std::clamp(st.focus, sp->openEnd, sp->closeBeg) - sp->openEnd;
            return r;
        } else if (sp->flags & SF_REF) {  // the definition, for every link that uses it: only once the notice was seen
            if (!confirmRefDef) return Refuse(st, "refdef");
            if (!Definition(src, RefLabel(src, *sp), &tb, &te)) return Refuse(st, "link");
            eds.push_back(Ed{tb, te, dest});
        } else {
            if (!DestToken(src, sp->closeBeg + 2, sp->closeEnd - 1, &tb, &te)) return Refuse(st, "link");
            eds.push_back(Ed{tb, te, dest});
        }
        return Built(c, st, std::move(eds), EK_FORMAT);
    }
    TRange rg;
    if (A.block != B.block || A.cell != B.cell || !RangeOf(d, A, &rg) || d.blocks[A.block].kind == BK_CODE)
        return Refuse(st, "format");
    while (A.t < B.t && Ws(d.text[A.t])) A.t++;  // blanks stay outside, as outside a format (§7.5 step 1)
    while (B.t > A.t && Ws(d.text[B.t - 1])) B.t--;
    if (A.t == B.t) return Refuse(st, "link");
    uint32_t sA = SrcOfText(d, src, A, MAP_OUTER_START), sB = SrcOfText(d, src, B, MAP_OUTER_END), tA = A.t, tB = B.t;
    if (sA == UINT32_MAX || sB == UINT32_MAX || sB <= sA) return Refuse(st, "format");
    const std::vector<const SpanSrc*> spans = SpansIn(d, A, rg);
    for (bool grew = true; grew;) {  // a span half in and half out comes in whole, and so does a link around the text
        grew = false;
        for (const SpanSrc* sp : spans) {
            bool meets = !(sp->flags & SF_UNCLOSED) && sp->openBeg < sB && sp->closeEnd > sA;
            bool whole = sp->openBeg >= sA && sp->closeEnd <= sB, around = sp->openBeg < sA && sp->closeEnd > sB;
            if (meets && !whole && (sp->type == MD_SPAN_A || !around)) {
                sA = std::min(sA, sp->openBeg);
                sB = std::max(sB, sp->closeEnd);
                tA = std::min(tA, sp->tBeg);
                tB = std::max(tB, sp->tEnd);
                grew = true;
            }
        }
    }
    std::vector<Ed> eds;
    eds.push_back(Ed{sA, sA, L"["});
    for (const SpanSrc* sp : spans)  // the links inside lose their delimiters: links do not nest
        if (sp->type == MD_SPAN_A && !(sp->flags & SF_UNCLOSED) && sp->openBeg >= sA && sp->closeEnd <= sB) {
            eds.push_back(Ed{sp->openBeg, sp->openEnd, L""});
            eds.push_back(Ed{sp->closeBeg, sp->closeEnd, L""});
        }
    SegSpan ss = SegsIn(d, A.block, rg);  // a bracket of the text itself would end the link's text
    for (const SrcSeg* g = ss.b; g < ss.e; g++)
        for (uint32_t k = std::max(g->s, sA); g->kind == SEG_PLAIN && k < std::min(g->s + g->sLen, sB); k++)
            if (src[k] == L'[' || src[k] == L']') eds.push_back(Ed{k, k, L"\\"});
    eds.push_back(Ed{sB, sB, L"](" + dest + L")"});
    EditResult r = Built(c, st, std::move(eds), EK_FORMAT);
    if (!r.refused.empty()) return r;
    r.verify.push_back(EditResult::Expect{tA, tB, FMT_LINK, true});
    r.keep = EditResult::Keep{true, tA, tB, d.text.substr(tA, tB - tA)};
    r.selA = A;
    r.selB = B;
    return r;
}

// The link at the caret loses its delimiters, its text stays (a definition stays too); an autolink stays text by an
// escape where it would link again: `<http://x>` → `http\://x`, `www.x.com` → `www\.x.com`, `a@b.c` → `a\@b.c`
EditResult OpLinkRemove(const EditCtx& c, const EditState& st) {
    const std::wstring& src = c.src;
    uint16_t tr = 0;
    const SpanSrc* sp = st.atom < 0 && !InPh(st) ? LinkSpan(c.doc, FocusOf(c, st, &tr)) : nullptr;
    if (!sp) return Refuse(st, "link");
    std::vector<Ed> eds;
    eds.push_back(Ed{sp->openBeg, sp->openEnd, L""});
    eds.push_back(Ed{sp->closeBeg, sp->closeEnd, L""});
    if (sp->flags & SF_AUTOLINK) {
        std::wstring t = Sub(src, sp->openEnd, sp->closeBeg);
        size_t k = t.find(L"://");
        if (k == std::wstring::npos) k = t.find(L'@');
        if (k == std::wstring::npos && t.size() > 4 && !_wcsnicmp(t.c_str(), L"www.", 4)) k = 3;
        if (k == std::wstring::npos) return Refuse(st, "link");
        eds.push_back(Ed{sp->openEnd + (uint32_t)k, sp->openEnd + (uint32_t)k, L"\\"});
    } else if (sp->block >= 0 && Para(c.doc, sp->block)) {
        // the text now starts its line: §7.4, so `[# x](u)` becomes `\# x`, not a heading (Phase 4 notes)
        bool first;
        if (LineContent(c.doc, src, sp->block, sp->tBeg, &first) == sp->openBeg)
            EscapeAt(eds, Sub(src, sp->openEnd, sp->closeBeg) + Sub(src, sp->closeEnd, LineEndOf(src, sp->closeEnd)), sp->openEnd, first);
    }
    EditResult r = Built(c, st, std::move(eds), EK_FORMAT);
    if (!r.refused.empty()) return r;
    r.verify.push_back(EditResult::Expect{sp->tBeg, sp->tEnd, FMT_LINK, false});
    r.keep = EditResult::Keep{true, sp->tBeg, sp->tEnd, c.doc.text.substr(sp->tBeg, sp->tEnd - sp->tBeg)};
    return r;
}

// ------------------------------------------------------------------------------------------------ the private format (§7.11)
// What FastMD puts on the clipboard for itself: the selection's source, balanced - the openers of the spans its start
// cuts in front, the closers of those its end cuts behind (`**br⟦own** fox⟧` → `**own** fox`); blocks it covers whole
// with their markers, and every line without the prefixes of the containers both ends share. Line ends: the file's.
std::wstring BalancedSlice(const EditCtx& c, const EditState& st) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    TextPos A, B;
    Ends(c, st, &A, &B);
    TRange ra;
    if (st.anchor == st.focus || st.atom >= 0 || InPh(st) || !ValidBlock(d, A.block) || !ValidBlock(d, B.block) ||
        !RangeOf(d, A, &ra))
        return L"";
    uint32_t sA = SrcOfText(d, src, A, MAP_OUTER_START), sB = SrcOfText(d, src, B, MAP_OUTER_END);
    if (sA == UINT32_MAX || sB == UINT32_MAX || sB <= sA) return L"";
    const std::vector<const ContainerSrc*> ca = Chain(d, A.block), cb = Chain(d, B.block);
    size_t common = 0;
    while (common < ca.size() && common < cb.size() && ca[common] == cb[common]) common++;
    const bool multi = A.block != B.block;
    if (multi && A.t == ra.beg && A.cell < 0) PrefixN(d, src, A.block, (int)common, &sA);  // the first block whole
    const std::wstring pc = PrefixN(d, src, A.block, (int)common, nullptr);
    std::wstring head, tail;
    auto cut = [&](const TextPos& p, bool start, bool end) {
        TRange rg;
        if (!RangeOf(d, p, &rg)) return;
        for (const SpanSrc* sp : SpansIn(d, p, rg)) {
            if (sp->flags & SF_UNCLOSED) continue;
            std::wstring close = Sub(src, sp->closeBeg, sp->closeEnd);
            if ((sp->flags & SF_REF) && (close == L"]" || close == L"][]")) close = L"][" + RefLabel(src, *sp) + L"]";
            if (start && sp->openEnd <= sA && sp->closeBeg >= sA) head += Sub(src, sp->openBeg, sp->openEnd);
            if (end && sp->openEnd <= sB && sp->closeBeg >= sB && (sp->openBeg >= sA || !multi)) tail.insert(0, close);
        }
    };
    cut(A, true, !multi);
    if (multi) cut(B, false, true);
    std::wstring out;
    for (uint32_t i = sA; i < sB;) {
        uint32_t le = std::min(LineEndOf(src, i), sB);
        out += Sub(src, i, le);
        if (le >= sB) break;
        uint32_t nx = std::min(SkipEol(src, le), sB);
        out += c.eol;
        for (size_t k = 0; nx < sB && k < pc.size() && src[nx] == pc[k]; k++) nx++;  // the shared containers' prefix
        i = nx;
    }
    // blanks at the slice's edges stay outside the delimiters it adds (`** c**` would not be bold, §7.5 step 1)
    size_t lead = 0, trail = out.size();
    while (!head.empty() && lead < out.size() && (out[lead] == L' ' || out[lead] == L'\t')) lead++;
    if (lead == out.size()) return out;
    while (!tail.empty() && trail > lead && (out[trail - 1] == L' ' || out[trail - 1] == L'\t')) trail--;
    return out.substr(0, lead) + head + out.substr(lead, trail - lead) + tail + out.substr(trail);
}

// ------------------------------------------------------------------------------------------------ source popups (§9)
namespace {
// a field's source as the popup shows it: its lines without the prefix they share, joined by "\n"
std::wstring Shown(const std::wstring& src, uint32_t a, uint32_t b, const std::wstring& pre) {
    std::wstring o;
    for (uint32_t i = a; i < b;) {
        uint32_t le = std::min(LineEndOf(src, i), b);
        o += Sub(src, i, le);
        if (le >= b) break;
        o += L'\n';
        i = std::min(SkipEol(src, le), b);
        for (size_t k = 0; i < b && k < pre.size() && src[i] == pre[k]; k++) i++;
    }
    return o;
}
// a fence's run of backticks or tildes at p (its length)
uint8_t RunOf(const std::wstring& src, uint32_t p, wchar_t ch) {
    uint32_t q = p;
    while (q < src.size() && src[q] == ch) q++;
    return (uint8_t)std::min<uint32_t>(q - p, 255);
}
}  // namespace

bool BindAtom(const Doc& d, const std::wstring& src, int32_t atom, AtomBinding* out) {
    AtomBinding& b = *out;  // (a fresh one: the callers' own)
    const int32_t blk = AtomBlockOf(d, atom);
    if (!ValidBlock(d, blk)) return false;
    const BlockSrc& bs = d.blockSrc[blk];
    auto one = [&](PopupKind k, uint32_t x, uint32_t y) {
        b.kind = k;
        b.fields = 1;
        b.beg[0] = x;
        b.end[0] = y;
    };
    const Image* im = nullptr;
    if (atom & kAtomBlock) {
        if (bs.flags & BS_RAW) {  // an HTML block (all the blocks drawn from it), front matter: its lines
            one(bs.flags & BS_FRONT ? PK_FRONT : PK_HTML, bs.beg, bs.end);
            b.outerBeg = bs.line;
            b.outerEnd = BlockEnd(d, blk);
        } else if (d.blocks[blk].kind == BK_IMAGE && d.blocks[blk].aux < d.images.size()) {
            im = &d.images[d.blocks[blk].aux];
        } else {
            return false;  // a rule: nothing to edit
        }
    } else if ((uint32_t)atom < d.images.size()) {
        im = &d.images[atom];
    }
    if (im) {
        if (im->outerBeg == UINT32_MAX || im->outerEnd > src.size()) return false;
        b.outerBeg = im->outerBeg;
        b.outerEnd = im->outerEnd;
        if (im->mathKind && im->srcBeg == UINT32_MAX) return false;
        if (im->mathKind == 1) {
            one(PK_FORMULA, im->srcBeg, im->srcEnd);
        } else if (im->mathKind == 2) {  // between `$$` and `$$`: without the line ends and prefixes around the TeX
            uint32_t x = im->srcBeg, y = im->srcEnd, ls = LineStartOf(src, y);
            if (x < y && EolChar(src[x])) {  // (the TeX from the next line on, after its container prefix)
                const std::wstring pre = ContPrefix(d, src, blk);
                x = SkipEol(src, x);
                for (size_t k = 0; x < y && k < pre.size() && src[x] == pre[k]; k++) x++;
            }
            if (ls > x && BlankLine(src, ls, y)) y = BackEol(src, ls);  // (the closing `$$` on a line of its own)
            one(PK_FORMULA_BLOCK, x, std::max(x, y));
        } else if (im->mathKind == 3) {  // the content lines; the fences, to make them longer when the text needs it
            one(PK_DIAGRAM, im->srcBeg, im->srcEnd);
            if (im->outerBeg < src.size() && (src[im->outerBeg] == L'`' || src[im->outerBeg] == L'~')) {
                b.fenceCh = src[im->outerBeg];
                b.fence[0] = im->outerBeg;
                b.fenceLen = RunOf(src, im->outerBeg, b.fenceCh);
                if (!(bs.flags & BS_UNCLOSED)) {
                    uint32_t e = bs.outerEnd;
                    while (e > b.end[0] && Blank(src[e - 1])) e--;
                    uint32_t q = e;
                    while (q > b.end[0] && src[q - 1] == b.fenceCh) q--;
                    if (q < e) {
                        b.fence[1] = q;
                        b.closeLen = (uint8_t)std::min<uint32_t>(e - q, 255);
                    }
                }
            }
        } else if (src[im->outerBeg] == L'<') {  // an HTML <img>: its tag
            one(PK_HTML, im->outerBeg, im->outerEnd);
        } else {  // a picture: its alt text and its destination, with its <…>
            if (im->altBeg == UINT32_MAX) return false;
            b.kind = PK_IMAGE;
            b.fields = 2;
            b.beg[0] = im->altBeg;
            b.end[0] = im->altEnd;
            if (im->srcBeg != UINT32_MAX) {
                b.beg[1] = im->srcBeg;
                b.end[1] = im->srcEnd;
                if (b.beg[1] > 0 && src[b.beg[1] - 1] == L'<' && b.end[1] < src.size() && src[b.end[1]] == L'>') b.beg[1]--, b.end[1]++;
            } else {  // a reference picture: its definition's address is shown, not edited here
                b.fixed1 = true;
                b.beg[1] = b.end[1] = im->outerEnd;
                for (uint32_t k = bs.spanOff; k < bs.spanOff + bs.spanCount && k < d.spans.size(); k++) {
                    const SpanSrc& sp = d.spans[k];
                    uint32_t tb, te;
                    if (sp.type == MD_SPAN_IMG && sp.openBeg == im->outerBeg && Definition(src, b.label = RefLabel(src, sp), &tb, &te))
                        b.text[1] = Unwrap(src, tb, te);
                }
            }
        }
    }
    b.prefix = ContPrefix(d, src, blk);
    b.cell = TableOf(d, blk) != nullptr;
    b.text[0] = Shown(src, b.beg[0], b.end[0], b.prefix);
    if (b.kind == PK_IMAGE && !b.fixed1) b.text[1] = Unwrap(src, b.beg[1], b.end[1]);
    return b.kind != PK_NONE;
}

bool AtomSplice(const std::wstring& src, AtomBinding& b, int f, std::wstring_view text, const wchar_t* eol, Splice* out,
                std::string* why) {
    if (f < 0 || f >= b.fields || (f == 1 && b.fixed1) || b.beg[f] > b.end[f] || b.end[f] > src.size()) {
        *why = "field";
        return false;
    }
    const std::wstring E = LineEol(src, b.beg[f], eol);
    const bool oneLine = b.kind == PK_FORMULA || b.kind == PK_IMAGE;
    std::wstring ins, line;
    uint32_t longest = 0;  // the longest fence-like run a line of a diagram starts with
    for (size_t i = 0, k = 0; i <= text.size(); i++) {
        wchar_t ch = i < text.size() ? text[i] : L'\n';
        if (ch == L'|' && b.cell) {
            // A pipe ends the cell (§7.10) - in a formula, an alt text or a path too: TeX's \vert (md4c would keep the
            // backslash of \|, and TeX draws \| as ‖), \| in alt text, %7C in a path, &#124; in HTML (Phase 4 notes)
            if (b.kind == PK_FORMULA) line += i + 1 < text.size() && iswalpha(text[i + 1]) ? L"\\vert " : L"\\vert";
            else if (b.kind == PK_IMAGE) line += f ? L"%7C" : L"\\|";
            else line += L"&#124;";
            continue;
        }
        if (ch != L'\r' && ch != L'\n') {
            line += (b.kind == PK_IMAGE && f == 0 && (ch == L'[' || ch == L']') && (line.empty() || line.back() != L'\\'))
                        ? std::wstring{L'\\', ch} : std::wstring(1, ch);
            continue;
        }
        if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') i++;
        if (b.kind == PK_FRONT && (Strip(line) == L"---" || Strip(line) == L"...")) {
            *why = "front";
            return false;
        }
        if (b.kind == PK_DIAGRAM && b.fenceCh) {
            size_t p = 0, q;
            while (p < line.size() && p < 3 && line[p] == L' ') p++;
            for (q = p; q < line.size() && line[q] == b.fenceCh;) q++;
            if (q > p && AllBlank(line, (uint32_t)q, (uint32_t)line.size())) longest = std::max<uint32_t>(longest, (uint32_t)(q - p));
        }
        if (k++) ins += oneLine ? std::wstring(L" ") : E + (line.empty() ? Strip(b.prefix) : b.prefix);
        ins += line;
        line.clear();
    }
    if (b.kind == PK_IMAGE && f == 1) ins = DestOf(ins, true);
    if (b.kind == PK_IMAGE && f == 0) {  // a backslash at the alt text's end would escape its `]` (Phase 4 notes)
        size_t n = 0;
        while (n < ins.size() && ins[ins.size() - 1 - n] == L'\\') n++;
        if (n % 2) ins += L'\\';
    }
    const uint32_t at = b.beg[f], oldEnd = b.end[f];
    const int64_t delta = (int64_t)ins.size() - (oldEnd - at);
    if (b.kind == PK_DIAGRAM && b.fence[0] != UINT32_MAX && longest >= b.fenceLen) {
        // a line of the text would close the fence: both fences get longer than any such line (§9.2)
        const std::wstring run(longest + 1, b.fenceCh);
        const bool closed = b.fence[1] != UINT32_MAX;
        const uint32_t o = b.fence[0], ce = closed ? b.fence[1] + b.closeLen : oldEnd;
        std::wstring all = run + Sub(src, o + b.fenceLen, at) + ins;
        if (closed) all += Sub(src, oldEnd, b.fence[1]) + run;
        *out = Replace(src, o, ce, all);
        const int grow = (int)run.size() - b.fenceLen, closeGrow = closed ? (int)run.size() - b.closeLen : 0;
        b.beg[f] = at + grow;
        b.end[f] = b.beg[f] + (uint32_t)ins.size();
        if (closed) b.fence[1] = b.end[f] + (b.fence[1] - oldEnd);
        b.outerEnd = (uint32_t)((int64_t)b.outerEnd + delta + grow + closeGrow);
        b.fenceLen = b.closeLen = (uint8_t)run.size();
        return true;
    }
    *out = Replace(src, at, oldEnd, ins);
    b.end[f] = at + (uint32_t)ins.size();
    if (f == 0 && b.fields > 1) {
        b.beg[1] = (uint32_t)(b.beg[1] + delta);
        b.end[1] = (uint32_t)(b.end[1] + delta);
    }
    if (b.fence[1] != UINT32_MAX && b.fence[1] >= oldEnd) b.fence[1] = (uint32_t)(b.fence[1] + delta);
    b.outerEnd = (uint32_t)((int64_t)b.outerEnd + delta);
    return true;
}

// the popup's change as an operation (the goldens' AtomSource): the atom bound afresh, the field's text put in
EditResult OpAtomSource(const EditCtx& c, const EditState& st, int atom, int field, std::wstring_view text) {
    AtomBinding b;
    Splice sp;
    std::string why;
    if (!BindAtom(c.doc, c.src, atom, &b)) return Refuse(st, "atom");
    if (!AtomSplice(c.src, b, field, text, c.eol, &sp, &why)) return Refuse(st, why.c_str());
    EditResult r = Nothing(st, EK_POPUP);
    if (sp.removed != sp.inserted) r.splices.push_back(std::move(sp));
    r.after.focus = MapThrough(r.splices, st.focus, false);
    r.after.anchor = MapThrough(r.splices, st.anchor, false);
    return Carry(std::move(r), true);  // (a phantom stays beside its block: the final gate's fuzz walk)
}

// ------------------------------------------------------------------------------------------------ picture files (§8.8)
std::wstring PictureDest(const std::wstring& docDir, const std::wstring& file) {
    auto parts = [](const std::wstring& p) {
        std::vector<std::wstring> v;
        for (size_t i = 0; i < p.size();) {
            size_t e = p.find_first_of(L"\\/", i);
            if (e == std::wstring::npos) e = p.size();
            if (e > i) v.push_back(p.substr(i, e - i));
            i = e + 1;
        }
        return v;
    };
    auto same = [](const std::wstring& x, const std::wstring& y) {
        return CompareStringOrdinal(x.c_str(), (int)x.size(), y.c_str(), (int)y.size(), TRUE) == CSTR_EQUAL;
    };
    const std::vector<std::wstring> a = parts(docDir), f = parts(file);
    const bool unc = file.rfind(L"\\\\", 0) == 0, sameUnc = unc == (docDir.rfind(L"\\\\", 0) == 0);
    const size_t root = unc ? 2 : 1;  // a drive, or a share: \\server\share
    size_t k = 0;
    while (k < a.size() && k + 1 < f.size() && same(a[k], f[k])) k++;
    std::wstring rel;
    const bool here = sameUnc && k >= root && a.size() >= root;
    if (here) {
        for (size_t i = k; i < a.size(); i++) rel += L"../";
    } else {
        k = 0;  // another volume: the whole path, always in <…> (`<D:/pics/x.png>`)
        if (unc) rel = L"//";
    }
    for (size_t i = k; i < f.size(); i++) rel += (i > k ? L"/" : L"") + f[i];
    std::wstring o;
    for (wchar_t ch : rel) o += ch == L'%' ? std::wstring(L"%25") : std::wstring(1, ch);
    o = DestOf(o, true);
    return here || o[0] == L'<' ? o : L"<" + o + L">";
}

// Back to the compiler's own inlining for the templates instantiated at the end of the file (see editcore.cpp's end)
#pragma inline_depth()
