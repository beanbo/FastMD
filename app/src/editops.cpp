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
    int32_t cell = -1;
    if (const Table* tb = TableOf(d, b)) {
        for (uint32_t k = 0; k < tb->rows * tb->cols && cell < 0; k++) {
            const Cell& cl = d.cells[tb->cellOff + k];
            if (t >= cl.textOff && t < cl.textOff + cl.textLen) cell = (int32_t)k;
        }
        if (cell < 0) return false;
    }
    TextPos p{t, b, cell};
    TRange rg;
    if (!RangeOf(d, p, &rg)) return false;
    bool has = false;
    ForSpans(d, p, rg, [&](const SpanSrc& sp) { has |= Gives(sp, fmt) && sp.tBeg <= t && t < sp.tEnd; });
    return has;
}

bool Verified(const Doc& a, const Doc& b, const EditResult& r) {
    if (!Kept(a, b, r.keep)) return false;
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
// text is all in the piece) - but not of those that go on beyond it
bool PieceSrc(const Doc& d, const TextPos& p, TRange rg, uint32_t t0, uint32_t t1, uint32_t* sA, uint32_t* sB) {
    SegSpan ss = SegsIn(d, p.block, rg);
    const SrcSeg* g0 = SegCovering(ss, t0);
    const SrcSeg* g1 = t1 > t0 ? SegCovering(ss, t1 - 1) : nullptr;
    if (!g0 || !g1) return false;
    *sA = g0->kind == SEG_PLAIN ? g0->s + (t0 - g0->t) : g0->s;
    *sB = g1->kind == SEG_PLAIN ? g1->s + (t1 - g1->t) : g1->s + g1->sLen;
    ForSpans(d, p, rg, [&](const SpanSrc& sp) {
        if ((sp.flags & SF_UNCLOSED) || sp.tBeg < t0 || sp.tEnd > t1 || sp.tBeg == sp.tEnd) return;
        *sA = std::min(*sA, sp.openBeg);
        *sB = std::max(*sB, sp.closeEnd);
    });
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
        auto whole = [&](const SpanSrc& sp) {
            if ((sp.flags & SF_UNCLOSED) || !Atomic(sp)) return false;
            if (fmt == FMT_CODE) return remove ? !Gives(sp, FMT_CODE) : !NotText(sp);
            return true;
        };
        for (bool grew = true; grew;) {
            grew = false;
            ForSpans(d, p, rg, [&](const SpanSrc& sp) {
                if (whole(sp) && sp.tBeg < t1 && sp.tEnd > t0 && (sp.tBeg < t0 || sp.tEnd > t1)) {
                    t0 = std::min(t0, sp.tBeg);
                    t1 = std::max(t1, sp.tEnd);
                    grew = true;
                }
            });
        }
        std::vector<uint32_t> cuts{t0, t1};
        std::vector<std::pair<uint32_t, uint32_t>> out_;  // left out of inline code
        ForSpans(d, p, rg, [&](const SpanSrc& sp) {
            if (sp.flags & SF_UNCLOSED) return;
            // every edge of the spans the format is taken off (one piece each), of what inline code leaves out; of the
            // other spans the edges of those that go on beyond the piece - a span inside it just nests in the new one
            bool every = (remove && Gives(sp, fmt)) || (fmt == FMT_CODE && !remove && NotText(sp));
            bool beyond = sp.tBeg < t0 || sp.tEnd > t1;
            if (!every && !(((sp.flags & SF_ENTERABLE) || Link(sp)) && beyond)) return;
            for (uint32_t e : {sp.tBeg, sp.tEnd})
                if (e > t0 && e < t1) cuts.push_back(e);
            if (fmt == FMT_CODE && !remove && NotText(sp)) out_.push_back({sp.tBeg, sp.tEnd});
        });
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
    if (!PieceSrc(d, p, rg, pc.t0, pc.t1, &sA, &sB)) return false;
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
    if (!Opens(Prev(src, sA), At(src, sA)) || !Closes(Prev(src, sB), At(src, sB))) {
        // out to the word's edges, if no span edge is in the way (`don⟦'t⟧` → `**don't**`)
        uint32_t t0 = pc.t0, t1 = pc.t1;
        while (t0 > rg.beg && WordChar(d.text[t0 - 1])) t0--;
        while (t1 < rg.end && WordChar(d.text[t1])) t1++;
        bool crosses = false;
        ForSpans(d, p, rg, [&](const SpanSrc& sp) {
            for (uint32_t e : {sp.tBeg, sp.tEnd}) crosses |= (e >= t0 && e < pc.t0) || (e > pc.t1 && e <= t1);
        });
        if (crosses || !PieceSrc(d, p, rg, t0, t1, &sA, &sB) || !Opens(Prev(src, sA), At(src, sA)) ||
            !Closes(Prev(src, sB), At(src, sB)))
            return false;
        pc.t0 = t0;
        pc.t1 = t1;
    }
    bool left = false, right = false;
    ForSpans(d, p, rg, [&](const SpanSrc& sp) {
        if (!Gives(sp, fmt) || (sp.flags & SF_UNCLOSED)) return;
        if (sp.tBeg >= pc.t0 && sp.tEnd <= pc.t1) {  // inside: its delimiters go, the new span covers it
            eds.push_back(Ed{sp.openBeg, sp.openEnd, L""});
            eds.push_back(Ed{sp.closeBeg, sp.closeEnd, L""});
        } else if (!left && sp.closeEnd == sA && Sub(src, sp.closeBeg, sp.closeEnd) == mark) {
            eds.push_back(Ed{sp.closeBeg, sp.closeEnd, L""});  // `**a**⟦b⟧` → `**ab**`
            left = true;
        } else if (!right && sp.openBeg == sB && Sub(src, sp.openBeg, sp.openEnd) == mark) {
            eds.push_back(Ed{sp.openBeg, sp.openEnd, L""});
            right = true;
        }
    });
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
    bool ok = true, any = false;
    ForSpans(d, p, rg, [&](const SpanSrc& S) {
        if (!ok || !Gives(S, fmt) || (S.flags & SF_UNCLOSED) || S.tBeg > pc.t0 || S.tEnd < pc.t1) return;
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
            return;
        }
        // a span between it and the piece that goes on beyond the piece would have to be cut too: not done
        ForSpans(d, p, rg, [&](const SpanSrc& J) {
            if (&J == &S || (J.flags & SF_UNCLOSED) || Atomic(J)) return;
            bool inS = J.openBeg >= S.openEnd && J.closeEnd <= S.closeBeg;
            bool holds = J.tBeg <= pc.t0 && J.tEnd >= pc.t1 && (J.tBeg < pc.t0 || J.tEnd > pc.t1);
            if (inS && holds) ok = false;
        });
        if (!ok) return;
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
        if (head && tail) return;  // the whole span: both delimiters go
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
    });
    if (!ok) return false;
    if (any) r.verify.push_back(EditResult::Expect{pc.t0, pc.t1, fmt, false});
    return true;
}
}  // namespace

// ------------------------------------------------------------------------------------------------ pending formats (§7.3)
// the enterable spans a blank typed at text position p - their end - leaves behind: they stay "sticky" (§7.3)
uint16_t ec::StickyAt(const Doc& d, const TextPos& p) {
    TRange rg;
    if (!ValidBlock(d, p.block) || !RangeOf(d, p, &rg)) return 0;
    uint32_t hard = 0;  // a link, code or formula ending there too: a span inside it cannot be carried past its closer
    ForSpans(d, p, rg, [&](const SpanSrc& sp) {
        if (sp.tEnd == p.t && !(sp.flags & (SF_ENTERABLE | SF_UNCLOSED))) hard = std::max(hard, sp.closeEnd);
    });
    uint16_t bits = 0;
    ForSpans(d, p, rg, [&](const SpanSrc& sp) {
        if (sp.tEnd != p.t || sp.tBeg >= p.t || (sp.flags & SF_UNCLOSED) || sp.type >= 0x80 || sp.closeBeg < hard) return;
        for (uint16_t f : {FMT_BOLD, FMT_ITALIC, FMT_STRIKE})
            if (Gives(sp, f)) bits |= f;
    });
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
    const SpanSrc* O = nullptr;
    ForSpans(d, p, rg, [&](const SpanSrc& sp) {
        if (sp.flags & SF_UNCLOSED || sp.openEnd > s || s > sp.closeBeg) return;
        for (uint16_t f : kFmts)
            if ((off & f) && Gives(sp, f) && (!O || sp.openBeg < O->openBeg)) O = &sp;
    });
    if (O) {
        std::vector<const SpanSrc*> k;  // open at the caret inside O (O too), innermost first
        ForSpans(d, p, rg, [&](const SpanSrc& sp) {
            if (!(sp.flags & SF_UNCLOSED) && sp.openEnd <= s && s <= sp.closeBeg && sp.openBeg >= O->openBeg && sp.closeEnd <= O->closeEnd)
                k.push_back(&sp);
        });
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
            ForSpans(d, p, rg, [&](const SpanSrc& sp) {
                if (!Gives(sp, f) || (sp.flags & SF_UNCLOSED) || Sub(src, sp.closeBeg, sp.closeEnd) != Mark(f)) return;
                if (sp.closeEnd == s || (sticky && sp.closeEnd < s && AllBlank(src, sp.closeEnd, s))) X = &sp;
            });
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
    return r;
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
        ForSpans(d, p, rg, [&](const SpanSrc& sp) {
            if (Gives(sp, fmt) && !(sp.flags & SF_UNCLOSED) && sp.openEnd <= s && s <= sp.closeBeg && (!S || sp.openBeg > S->openBeg)) S = &sp;
        });
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
    if (to == 0 && bl.size() == 1 && d.blocks[bl[0]].heading && (d.blockSrc[bl[0]].flags & BS_ATX) && !d.blocks[bl[0]].textLen)
        return GoneToPhantom(c, st, bl[0], EK_FORMAT);  // an empty heading: nothing to keep as a paragraph
    std::vector<Ed> eds;
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
            EscapeAt(eds, Sub(src, bs.beg, bs.end), bs.beg, true);
        } else if (bs.flags & BS_SETEXT) {
            eds.push_back(Ed{bs.lineEnd, bs.outerEnd, L""});
        }
    }
    return Built(c, st, std::move(eds), EK_FORMAT);
}

// ------------------------------------------------------------------------------------------------ lists (§8.5)
namespace {
bool OfKind(const ContainerSrc& it, int kind) {
    return kind == 9 ? it.taskOff != UINT32_MAX : kind == 8 ? it.delim != 0 : it.bullet != 0;
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
// an item's continuation lines and children move by delta columns: its marker got wider or narrower
void Reindent(const Doc& d, const std::wstring& src, int32_t item, int delta, std::vector<Ed>& eds) {
    if (!delta) return;
    const ContainerSrc& it = d.containers[item];
    std::vector<Splice> v;
    uint32_t from = SkipEol(src, LineEndOf(src, it.markOff)), to = d.blockSrc[LastOf(d, item)].outerEnd;
    if (from > to) return;
    Shift(src, from, to, Col(src, it.markOff), delta, v);
    for (const Splice& sp : v) eds.push_back(Ed{sp.at, sp.at + (uint32_t)sp.removed.size(), sp.inserted});
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
            if (kind == 9) {  // the box only
                uint32_t e = it.taskOff + 2;
                if (e < src.size() && Blank(src[e])) e++;
                eds.push_back(Ed{it.taskOff - 1, e, L""});
                continue;
            }
            eds.push_back(Ed{it.markOff, fs.beg, L""});
            Reindent(d, src, items[i], -(int)(it.contentCol - Col(src, it.markOff)), eds);
            if (Para(d, fb)) EscapeAt(eds, Sub(src, fs.beg, LineEndOf(src, fs.beg)), fs.beg, true);
            // blank lines keep the paragraphs apart - from each other and from the items left before and after them
            const std::wstring E = Eol(c, it.markOff), blank = Strip(PrefixN(d, src, fb, (int)Chain(d, fb).size() - 1, nullptr));
            const int32_t last = LastOf(d, items[i]), nx = NextInSource(d, last), pv = PrevInSource(d, fb);
            uint32_t le = d.blockSrc[last].outerEnd, nl = SkipEol(src, le);
            if (nx >= 0 && nl < src.size() && !BlankLine(src, nl, LineEndOf(src, nl))) eds.push_back(Ed{le, le, E + blank});
            if (i == 0 && pv >= 0 && ItemAround(d, pv) >= 0) {
                uint32_t ls = LineStartOf(src, it.markOff), pe = BackEol(src, ls);
                if (ls > 0 && !BlankLine(src, LineStartOf(src, pe), pe)) eds.push_back(Ed{ls, ls, blank + E});
            }
        }
        return Built(c, st, std::move(eds), EK_FORMAT);
    }
    // another kind: with a caret its whole list level, with a selection the items it touches
    std::vector<int32_t> change;
    if (st.anchor == st.focus && !items.empty()) change = ListRun(d, items[0]);
    else change = items;
    uint32_t num = 1;
    for (int32_t x : change) {
        const ContainerSrc& it = d.containers[x];
        if (kind == 9) {
            if (it.taskOff == UINT32_MAX) eds.push_back(Ed{d.blockSrc[it.firstBlock].beg, d.blockSrc[it.firstBlock].beg, L"[ ] "});
            continue;
        }
        if (OfKind(it, kind)) {
            num++;
            continue;
        }
        std::wstring m = kind == 8 ? std::to_wstring(num++) + L"." : L"-";
        eds.push_back(Ed{it.markOff, it.markOff + it.markLen, m});
        Reindent(d, src, x, (int)m.size() - (int)it.markLen, eds);
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
    }
    return Built(c, st, std::move(eds), EK_FORMAT);
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
            if (blanks) lines.insert(lines.end(), gap.begin(), gap.end());
        }
        lines.push_back(Line{bs.line, bl[i], false});
        for (uint32_t l : MoreLines(src, bs, bs.outerEnd)) lines.push_back(Line{l, bl[i], false});
    }
    std::vector<Ed> eds;
    if (q < 0) {  // on
        for (const Line& l : lines) {
            if (l.blank) {
                eds.push_back(Ed{l.ls, LineEndOf(src, l.ls), ContPrefix(d, src, l.b) + L">"});
                continue;
            }
            uint32_t at = WalkPrefix(d, src, l.b, l.ls, nullptr);
            eds.push_back(Ed{at, at, L"> "});
        }
        return Built(c, st, std::move(eds), EK_FORMAT);
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
    return Built(c, st, std::move(eds), EK_FORMAT);
}

// ------------------------------------------------------------------------------------------------ code block (§8.7)
namespace {
// A text block as code lines: its rendered text, a line per source line (soft and hard breaks end a line); a picture
// or formula in it and a footnote reference keep their source. *caretLine / *caretCol: where text offset t lands.
std::vector<std::wstring> CodeLines(const Doc& d, const std::wstring& src, int32_t b, uint32_t t, int* caretLine, uint32_t* caretCol) {
    const Block& bl = d.blocks[b];
    std::vector<std::wstring> v(1);
    SegSpan ss = SegsIn(d, b, TRange{bl.textOff, bl.textOff + bl.textLen});
    for (const SrcSeg* g = ss.b; g < ss.e; g++) {
        if (t >= g->t && t <= g->t + g->tLen && caretLine && *caretLine < 0) {
            *caretLine = (int)v.size() - 1;
            *caretCol = (uint32_t)v.back().size() + (g->kind == SEG_PLAIN ? t - g->t : 0);
        }
        if (LineBrk(src, g)) v.emplace_back();
        else if (g->kind == SEG_OBJATOM || (g->kind == SEG_TEXTATOM && g->sLen > 1 && src[g->s] == L'[' && src[g->s + 1] == L'^'))
            v.back() += Sub(src, g->s, g->s + g->sLen);
        else if (g->kind != SEG_SYNTH) v.back() += d.text.substr(g->t, g->tLen);
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
        return r;
    }
    if (fs.flags & BS_FOOTNOTE) return Refuse(st, "context");
    std::vector<int32_t> bl = TextBlocks(c, st);
    if (bl.empty()) return Refuse(st, "context");
    for (int32_t b : bl)
        if (d.blockSrc[b].container != d.blockSrc[bl[0]].container) return Refuse(st, "format");  // one container only
    std::vector<std::wstring> lines;
    int caretLine = -1;
    uint32_t caretCol = 0;
    for (size_t i = 0; i < bl.size(); i++) {
        if (i) lines.emplace_back();
        int cl = -1;
        uint32_t cc = 0;
        std::vector<std::wstring> v = CodeLines(d, src, bl[i], bl[i] == f.block ? f.t : UINT32_MAX, &cl, &cc);
        if (cl >= 0 && caretLine < 0) {
            caretLine = (int)lines.size() + cl;
            caretCol = cc;
        }
        lines.insert(lines.end(), v.begin(), v.end());
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
    return r;
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
    return r;
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
    }
    std::wstring t = L"$" + tex + L"$";
    if (A.t > rg.beg && iswalnum(d.text[A.t - 1])) t = L" " + t;
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

// A picture: in the line at the caret, or a paragraph of its own in a phantom, after code or an object
EditResult OpInsertImage(const EditCtx& c, const EditState& st, std::wstring_view dest, std::wstring_view alt) {
    const Doc& d = c.doc;
    std::wstring a;
    for (wchar_t ch : alt) {
        if (ch == L'[' || ch == L']' || ch == L'\\') a += L'\\';
        a += EolChar(ch) ? L' ' : ch;
    }
    const std::wstring t = L"![" + a + L"](" + std::wstring(dest) + L")";
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr);
    if (!InPh(st) && (st.atom >= 0 || !ValidBlock(d, f.block) || d.blocks[f.block].kind == BK_CODE))
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

// Back to the compiler's own inlining for the templates instantiated at the end of the file (see editcore.cpp's end)
#pragma inline_depth()
