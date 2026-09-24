// Edit mode's core, the map half (docs/EDIT-MODE.md §4.5, §5.5, §6.2-§6.4, §7.2): where a caret may stand, which
// source offset a text position stands for and back, the map's self-check, block diffing, line ends and the prefixes
// a new line inside containers needs.
//
// Everything here is a pure function of a Doc parsed with ParseOptions::wantMap and the source it was parsed from:
// no `g`, no window, no Win32 object. The unit tests (tests/edit) and the fuzzer call it directly.
#include "editcore.h"
#include "../third_party/md4c/md4c.h"  // the span types SpanSrc::type holds

#include <cstdarg>
#include <cstdio>

namespace {
bool Blank(wchar_t c) { return c == L' ' || c == L'\t'; }
bool EolChar(wchar_t c) { return c == L'\n' || c == L'\r'; }
bool HighSur(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool LowSur(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

uint32_t LineStartOf(const std::wstring& src, uint32_t s) {
    s = std::min<uint32_t>(s, (uint32_t)src.size());
    while (s > 0 && !EolChar(src[s - 1])) s--;
    return s;
}
uint32_t LineEndOf(const std::wstring& src, uint32_t s) {
    uint32_t n = (uint32_t)src.size();
    while (s < n && !EolChar(src[s])) s++;
    return s;
}
// columns of src[from, to) when the line starts at ls (tab stops of 4, as md4c counts them)
uint32_t ColsOf(const std::wstring& src, uint32_t ls, uint32_t from, uint32_t to) {
    uint32_t c = 0, c0 = 0;
    for (uint32_t p = ls; p < to && p < src.size(); p++) {
        if (p == from) c0 = c;
        c = src[p] == L'\t' ? (c + 4) & ~3u : c + 1;
    }
    if (from >= to) return 0;
    return c - c0;
}
bool AllBlank(const std::wstring& src, uint32_t from, uint32_t to) {
    if (to > src.size()) return false;
    for (uint32_t p = from; p < to; p++)
        if (!Blank(src[p])) return false;
    return true;
}

// the same rule as view.cpp's BlockHidden, on a Doc that is not g.doc
bool Hidden(const Doc& d, const Block& b) {
    if (!b.details || (b.details & 0x8000)) return false;
    uint32_t gi = (uint32_t)(b.details & 0x7FFF) - 1;
    return gi < d.detailsOpen.size() && !d.detailsOpen[gi];
}
bool AtomBlock(const BlockSrc& bs) { return (bs.flags & (BS_OBJECT | BS_RAW)) != 0; }
bool ValidBlock(const Doc& d, int32_t b) {
    return d.hasMap && b >= 0 && (size_t)b < d.blocks.size() && d.blockSrc.size() == d.blocks.size();
}

// A text range a position lives in: its block's text, or in a table the text of its cell.
struct TRange { uint32_t beg, end; };
const Table* TableOf(const Doc& d, int32_t block) {
    const Block& b = d.blocks[block];
    if (b.kind != BK_TABLE || b.aux >= d.tables.size()) return nullptr;
    return &d.tables[b.aux];
}
bool RangeOf(const Doc& d, const TextPos& p, TRange* r) {
    const Block& b = d.blocks[p.block];
    if (const Table* tb = TableOf(d, p.block)) {
        uint32_t n = tb->rows * tb->cols;
        if (p.cell < 0 || (uint32_t)p.cell >= n || tb->cellOff + n > d.cells.size()) return false;
        const Cell& c = d.cells[tb->cellOff + p.cell];
        *r = TRange{c.textOff, c.textOff + c.textLen};
        return true;
    }
    if (p.cell >= 0) return false;
    *r = TRange{b.textOff, b.textOff + b.textLen};
    return true;
}

// the segments of a block that lie in [r.beg, r.end): its whole slice, or the part a cell owns
struct SegSpan { const SrcSeg* b; const SrcSeg* e; };
SegSpan SegsIn(const Doc& d, int32_t block, TRange r) {
    const BlockSrc& bs = d.blockSrc[block];
    if (bs.segOff + bs.segCount > d.segs.size() || !bs.segCount) return SegSpan{nullptr, nullptr};
    const SrcSeg* b = d.segs.data() + bs.segOff;
    const SrcSeg* e = b + bs.segCount;
    b = std::lower_bound(b, e, r.beg, [](const SrcSeg& s, uint32_t t) { return s.t < t; });
    e = std::lower_bound(b, e, r.end, [](const SrcSeg& s, uint32_t t) { return s.t < t; });
    return SegSpan{b, e};
}
// the spans of a position's block inside [r.beg, r.end] (all of them outside tables). In a table the text range is not
// enough: a cell whose text is empty (`| <kbd> | c |`) shares its offset with the next cell's start, so a span must
// also open inside the position's own cell.
template <class F> void ForSpans(const Doc& d, const TextPos& p, TRange r, F f) {
    const BlockSrc& bs = d.blockSrc[p.block];
    const CellSrc* cs = nullptr;
    if (const Table* tb = TableOf(d, p.block); tb && p.cell >= 0 && tb->cellOff + (uint32_t)p.cell < d.cellSrc.size())
        cs = &d.cellSrc[tb->cellOff + p.cell];
    for (uint32_t k = bs.spanOff; k < bs.spanOff + bs.spanCount && k < d.spans.size(); k++) {
        const SpanSrc& sp = d.spans[k];
        if (sp.tBeg >= r.beg && sp.tEnd <= r.end && (!cs || (sp.openBeg >= cs->beg && sp.openBeg <= cs->end))) f(sp);
    }
}

// Where text typed into an empty block or cell goes (§6.3.1). The op that uses it adds the blank a marker or a pipe
// needs; this is only the offset.
uint32_t InsertionPoint(const Doc& d, const std::wstring& src, const TextPos& p) {
    const BlockSrc& bs = d.blockSrc[p.block];
    if (const Table* tb = TableOf(d, p.block)) {
        uint32_t idx = tb->cellOff + (uint32_t)p.cell;
        if (idx >= d.cellSrc.size()) return bs.beg;
        const CellSrc& cs = d.cellSrc[idx];
        if (cs.missing) return cs.beg;  // the end of its row (§7.10 completes the row)
        uint32_t q = cs.beg;            // an empty cell sits on its closing pipe: go back to the opening one
        while (q > bs.line && Blank(src[q - 1])) q--;
        if (q > bs.line && src[q - 1] == L'|') {
            if (q < cs.beg && Blank(src[q])) q++;  // right after the one blank that follows the pipe
            return q;
        }
        return cs.beg;
    }
    return bs.beg;  // empty heading, empty item, empty fenced block: their records already point there
}

// the first segment of the run of synthesized ones that ends with *g (only the start of such a run is a caret stop)
const SrcSeg* SynthRunStart(const SrcSeg* first, const SrcSeg* g) {
    while (g > first && (g - 1)->kind == SEG_SYNTH && (g - 1)->t + (g - 1)->tLen == g->t) g--;
    return g;
}

// the last caret stop of a block (before a synthesized tail such as the footnote arrow)
uint32_t LastStop(const Doc& d, int32_t block) {
    const Block& b = d.blocks[block];
    const BlockSrc& bs = d.blockSrc[block];
    uint32_t end = b.textOff + b.textLen;
    if (bs.segCount && bs.segOff + bs.segCount <= d.segs.size()) {
        const SrcSeg* first = d.segs.data() + bs.segOff;
        const SrcSeg* last = first + bs.segCount - 1;
        if (last->kind == SEG_SYNTH && last->t + last->tLen == end) return SynthRunStart(first, last)->t;
    }
    return end;
}

TextPos BlockEdge(const Doc& d, int32_t block, bool atEnd) {
    const Block& b = d.blocks[block];
    const BlockSrc& bs = d.blockSrc[block];
    if (AtomBlock(bs)) {
        int32_t k = (bs.flags & BS_RAW) && bs.rawId >= 0 ? bs.rawId : block;
        return TextPos{d.blocks[k].textOff, k, -1};
    }
    if (const Table* tb = TableOf(d, block)) {
        uint32_t n = tb->rows * tb->cols;
        if (!n) return TextPos{b.textOff, block, -1};
        int32_t c = atEnd ? (int32_t)n - 1 : 0;
        const Cell& cell = d.cells[tb->cellOff + c];
        return TextPos{atEnd ? cell.textOff + cell.textLen : cell.textOff, block, c};
    }
    return TextPos{atEnd ? LastStop(d, block) : b.textOff, block, -1};
}

// Source -> text inside one run of segments (a block's, or a cell's). zoneEnd: the source offset where trailing
// blanks after the last segment stop counting (the line end, or a cell's separator pipe).
TextPos InSegs(const Doc& d, const std::wstring& src, int32_t block, int32_t cell, SegSpan ss, TRange r, uint32_t s,
               int dir, uint16_t* trailCols) {
    if (ss.b == ss.e) return TextPos{r.beg, block, cell};
    const SrcSeg* it = std::upper_bound(ss.b, ss.e, s, [](uint32_t v, const SrcSeg& g) { return v < g.s; });
    if (it == ss.b) return TextPos{ss.b->t, block, cell};  // before the first segment: a prefix, marker or opener
    const SrcSeg& pv = *(it - 1);
    const SrcSeg* nx = it != ss.e ? it : nullptr;
    uint32_t pvEnd = pv.s + pv.sLen;
    if (s < pvEnd) {
        if (pv.kind == SEG_PLAIN) {
            uint32_t t = pv.t + (s - pv.s);
            if (t > pv.t && HighSur(d.text[t - 1]) && LowSur(d.text[t])) t += dir < 0 ? -1 : 1;  // never mid-pair
            return TextPos{t, block, cell};
        }
        if (s == pv.s) return TextPos{pv.t, block, cell};
        // The blanks a soft break starts with are the trailing blanks of its line (§6.5). A hard break's two blanks are
        // the break itself: the caret stands at its edges, never inside (§6.6).
        if (pv.kind == SEG_TEXTATOM && pv.tLen == 1 && d.text[pv.t] == L' ' && AllBlank(src, pv.s, s)) {
            if (trailCols) *trailCols = (uint16_t)std::min<uint32_t>(0xFFFF, ColsOf(src, LineStartOf(src, pv.s), pv.s, s));
            return TextPos{pv.t, block, cell};
        }
        return TextPos{dir < 0 ? pv.t : pv.t + pv.tLen, block, cell};  // inside an atom: its nearer edge that way
    }
    if (pv.kind == SEG_SYNTH) return TextPos{SynthRunStart(ss.b, it - 1)->t, block, cell};  // never past its start
    uint32_t after = pv.t + pv.tLen;
    // Trailing blanks: after the last segment of a line - and after the closers of the spans that end with it (a link's
    // `](u)`, a code span's backtick) - before the line's end (or the cell's pipe).
    uint32_t runBeg = pvEnd;
    ForSpans(d, TextPos{after, block, cell}, r, [&](const SpanSrc& sp) {
        if (sp.tEnd == after && !(sp.flags & SF_UNCLOSED) && sp.closeBeg >= pvEnd) runBeg = std::max(runBeg, sp.closeEnd);
    });
    uint32_t le = LineEndOf(src, runBeg);
    if (s > runBeg && s <= le && AllBlank(src, runBeg, s) && (!nx || nx->s > le || nx->kind == SEG_SYNTH)) {
        if (trailCols) *trailCols = (uint16_t)std::min<uint32_t>(0xFFFF, ColsOf(src, LineStartOf(src, runBeg), runBeg, s));
        return TextPos{after, block, cell};
    }
    if (!nx || nx->kind == SEG_SYNTH || dir < 0) return TextPos{after, block, cell};
    return TextPos{nx->t, block, cell};
}

// the separator pipe after a cell (or the row's end when there is none)
uint32_t PipeAfter(const std::wstring& src, uint32_t from, uint32_t le) {
    uint32_t q = from;
    while (q < le && src[q] != L'|') {
        if (src[q] == L'\\' && q + 1 < le) q++;
        q++;
    }
    return q;
}

// A table: the row line that holds s, then the cell whose zone does - from just after the previous separator pipe to
// its own separator. Leading blanks and the leading pipe go to the cell's start, trailing blanks up to the pipe to its
// end (§6.5), and a row's end after its last present cell to the first cell the row lacks.
TextPos InTable(const Doc& d, const std::wstring& src, int32_t block, const Table& tb, uint32_t s, int dir,
                uint16_t* trailCols) {
    uint32_t n = tb.rows * tb.cols;
    if (!n || tb.cellOff + n > d.cellSrc.size()) return TextPos{d.blocks[block].textOff, block, -1};
    auto cellPos = [&](uint32_t c, bool end) {
        const Cell& cell = d.cells[tb.cellOff + c];
        return TextPos{end ? cell.textOff + cell.textLen : cell.textOff, block, (int32_t)c};
    };
    const TableSrc* ts = d.blocks[block].aux < d.tableSrc.size() ? &d.tableSrc[d.blocks[block].aux] : nullptr;
    for (uint32_t r = 0; r < tb.rows; r++) {
        uint32_t ri = r == 0 ? 0 : r + 1;  // tableSrc rows: header, delimiter, body...
        if (!ts || ri >= ts->rows.size()) break;
        const RowSrc& row = ts->rows[ri];
        uint32_t first = r * tb.cols;
        if (s < row.lineStart) {  // between two rows (a line end, a prefix, the delimiter row): the side dir asks for
            if (dir < 0 && r > 0) return cellPos(first - 1, true);
            return cellPos(first, false);
        }
        if (s > row.lineEnd) continue;
        for (uint32_t c = 0; c < tb.cols; c++) {
            const CellSrc& cs = d.cellSrc[tb.cellOff + first + c];
            if (cs.missing) return cellPos(first + c, false);  // past every present cell: the first one missing
            bool last = c + 1 == tb.cols || d.cellSrc[tb.cellOff + first + c + 1].missing;
            uint32_t pipe = PipeAfter(src, cs.end, row.lineEnd);
            if (s > pipe && !last) continue;  // a later cell's zone
            if (s <= cs.beg) return cellPos(first + c, false);
            if (s <= cs.end) {
                const Cell& cell = d.cells[tb.cellOff + first + c];
                TRange cr{cell.textOff, cell.textOff + cell.textLen};
                return InSegs(d, src, block, (int32_t)(first + c), SegsIn(d, block, cr), cr, s, dir, trailCols);
            }
            if (s <= pipe && AllBlank(src, cs.end, s)) {  // trailing blanks: they run up to the separator pipe
                if (trailCols) *trailCols = (uint16_t)std::min<uint32_t>(0xFFFF, ColsOf(src, row.lineStart, cs.end, s));
                return cellPos(first + c, true);
            }
            if (s > pipe && c + 1 < tb.cols) return cellPos(first + c + 1, false);  // after the last pipe: a missing cell
            return cellPos(first + c, true);
        }
        return cellPos(first + tb.cols - 1, true);
    }
    return cellPos(n - 1, true);
}

TextPos InBlock(const Doc& d, const std::wstring& src, int32_t block, uint32_t s, int dir, uint16_t* trailCols) {
    const BlockSrc& bs = d.blockSrc[block];
    if (AtomBlock(bs)) return BlockEdge(d, block, false);
    if (const Table* tb = TableOf(d, block)) return InTable(d, src, block, *tb, s, dir, trailCols);
    const Block& b = d.blocks[block];
    TRange r{b.textOff, b.textOff + b.textLen};
    SegSpan ss = SegsIn(d, block, r);
    if (ss.b == ss.e) return TextPos{b.textOff, block, -1};
    return InSegs(d, src, block, -1, ss, r, s, dir, trailCols);
}

// ---- self-check helpers
struct Why {
    std::string* out;
    bool Fail(const char* fmt, ...) {
        if (out) {
            char buf[512];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, sizeof buf, fmt, ap);
            va_end(ap);
            *out = buf;
        }
        return false;
    }
};
const char* KindName(uint8_t k) {
    static const char* n[] = {"PLAIN", "TEXTATOM", "OBJATOM", "SYNTH"};
    return k < 4 ? n[k] : "?";
}

std::wstring PictureKey(const Image& im) {
    if (im.mathKind) return L"m" + std::to_wstring(im.mathKind) + L":" + im.alt;
    if (!im.url.empty()) return L"u:" + im.url;
    return L"p:" + im.path;
}

bool SameRuns(const Doc& a, uint32_t ra, uint32_t na, uint32_t ta, const Doc& b, uint32_t rb, uint32_t nb, uint32_t tb) {
    if (na != nb || ra + na > a.runs.size() || rb + nb > b.runs.size()) return false;
    for (uint32_t k = 0; k < na; k++) {
        const Run& x = a.runs[ra + k];
        const Run& y = b.runs[rb + k];
        if (x.start - ta != y.start - tb || x.len != y.len || x.flags != y.flags || x.color != y.color) return false;
        if (x.flags & F_LINK) {
            bool okx = x.link < a.links.size(), oky = y.link < b.links.size();
            if (okx != oky || (okx && a.links[x.link] != b.links[y.link])) return false;
        }
        if (x.flags & F_IMAGE) {
            bool okx = x.image < a.images.size(), oky = y.image < b.images.size();
            if (okx != oky || (okx && PictureKey(a.images[x.image]) != PictureKey(b.images[y.image]))) return false;
        }
    }
    return true;
}
bool SameText(const Doc& a, uint32_t ta, uint32_t la, const Doc& b, uint32_t tb, uint32_t lb) {
    return la == lb && ta + la <= a.text.size() && tb + lb <= b.text.size() &&
           a.text.compare(ta, la, b.text, tb, lb) == 0;
}

// §5.5 step 5: the two blocks would lay out the same (their gap and list number aside)
bool SameBlock(const Doc& a, uint32_t i, const Doc& b, uint32_t j) {
    const Block& x = a.blocks[i];
    const Block& y = b.blocks[j];
    if (x.kind != y.kind || x.heading != y.heading || x.marker != y.marker || x.listLevel != y.listLevel ||
        x.muted != y.muted || x.lang != y.lang || x.alertTitle != y.alertTitle || x.align != y.align ||
        x.indent != y.indent || x.details != y.details)
        return false;
    if (!SameText(a, x.textOff, x.textLen, b, y.textOff, y.textLen)) return false;
    if (!SameRuns(a, x.runOff, x.runCount, x.textOff, b, y.runOff, y.runCount, y.textOff)) return false;
    if (x.kind == BK_CODE) {
        bool hx = x.aux && x.aux <= a.langNames.size(), hy = y.aux && y.aux <= b.langNames.size();
        if (hx != hy || (hx && a.langNames[x.aux - 1] != b.langNames[y.aux - 1])) return false;
    }
    if (x.kind == BK_IMAGE) {
        if (x.aux >= a.images.size() || y.aux >= b.images.size()) return x.aux >= a.images.size() && y.aux >= b.images.size();
        const Image& p = a.images[x.aux];
        const Image& q = b.images[y.aux];
        if (PictureKey(p) != PictureKey(q) || p.w != q.w || p.h != q.h || p.attrW != q.attrW || p.attrH != q.attrH)
            return false;
    }
    if (x.kind == BK_TABLE) {
        if (x.aux >= a.tables.size() || y.aux >= b.tables.size()) return x.aux >= a.tables.size() && y.aux >= b.tables.size();
        const Table& p = a.tables[x.aux];
        const Table& q = b.tables[y.aux];
        if (p.cols != q.cols || p.rows != q.rows) return false;
        for (uint32_t c = 0; c < p.cols; c++)
            if (a.aligns[p.alignOff + c] != b.aligns[q.alignOff + c]) return false;
        for (uint32_t k = 0; k < p.rows * p.cols; k++) {
            const Cell& cp = a.cells[p.cellOff + k];
            const Cell& cq = b.cells[q.cellOff + k];
            if (!SameText(a, cp.textOff, cp.textLen, b, cq.textOff, cq.textLen) ||
                !SameRuns(a, cp.runOff, cp.runCount, cp.textOff, b, cq.runOff, cq.runCount, cq.textOff))
                return false;
        }
    }
    return true;
}

// the container chain of a block, outermost first
std::vector<const ContainerSrc*> Chain(const Doc& d, int block) {
    std::vector<const ContainerSrc*> chain;
    int32_t c = d.blockSrc[block].container;
    for (int guard = 0; c >= 0 && (size_t)c < d.containers.size() && guard < 256; guard++) {
        chain.push_back(&d.containers[c]);
        c = d.containers[c].parent;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}
}  // namespace

// ------------------------------------------------------------------------------------------------ caret stops (§6.2)
bool CaretStop(const Doc& d, const TextPos& p) {
    if (!ValidBlock(d, p.block)) return false;
    const Block& b = d.blocks[p.block];
    const BlockSrc& bs = d.blockSrc[p.block];
    if (Hidden(d, b) || (bs.flags & BS_SYNTH)) return false;
    if (AtomBlock(bs)) return p.t == b.textOff && p.cell < 0;  // an object atom is one stop
    TRange r;
    if (!RangeOf(d, p, &r) || p.t < r.beg || p.t > r.end) return false;
    if (p.t > r.beg && p.t < r.end && HighSur(d.text[p.t - 1]) && LowSur(d.text[p.t])) return false;
    SegSpan ss = SegsIn(d, p.block, r);
    if (ss.b == ss.e) return true;
    const SrcSeg* it = std::upper_bound(ss.b, ss.e, p.t, [](uint32_t t, const SrcSeg& g) { return t < g.t + g.tLen; });
    if (it != ss.e && it->t < p.t && it->kind != SEG_PLAIN) return false;  // strictly inside an atom or synthesized text
    // at the end of synthesized text that nothing real follows (the footnote's back arrow)
    if (it != ss.b && (it - 1)->kind == SEG_SYNTH && (it - 1)->t + (it - 1)->tLen == p.t &&
        !(it != ss.e && it->t == p.t && it->kind != SEG_SYNTH))
        return false;
    return true;
}

// ------------------------------------------------------------------------------------------------ text -> source (§6.3)
uint32_t SrcOfText(const Doc& d, const std::wstring& src, const TextPos& p, MapMode mode) {
    if (!ValidBlock(d, p.block)) return UINT32_MAX;
    const BlockSrc& bs = d.blockSrc[p.block];
    if (bs.flags & BS_SYNTH) return UINT32_MAX;
    if (AtomBlock(bs)) return mode == MAP_OUTER_END ? bs.outerEnd : bs.line;
    TRange r;
    if (!RangeOf(d, p, &r)) return UINT32_MAX;
    uint32_t t = std::clamp(p.t, r.beg, r.end);
    SegSpan ss = SegsIn(d, p.block, r);
    const SrcSeg* it = ss.b == ss.e ? nullptr
                                    : std::upper_bound(ss.b, ss.e, t, [](uint32_t v, const SrcSeg& g) { return v < g.t + g.tLen; });
    if (it && it != ss.e && it->t < t) {  // strictly inside a segment
        if (it->kind == SEG_PLAIN) return it->s + (t - it->t);
        return mode == MAP_OUTER_END ? it->s + it->sLen : it->s;  // inside an atom: callers snap first
    }
    const SrcSeg* R = it && it != ss.e && it->t == t ? it : nullptr;
    const SrcSeg* L = it && it != ss.b && (it - 1)->t + (it - 1)->tLen == t ? it - 1 : nullptr;
    if (L && R && L->kind == SEG_SYNTH) L = nullptr;  // synthesized text has no source to continue: R's start counts
    uint32_t lEnd = L ? L->s + L->sLen : 0;
    switch (mode) {
    case MAP_CARET:
        if (L) {
            // Typing continues the formatting of the character before the caret - but never inside a link, code or
            // formula from its right edge: past the closer of the outermost such span that ends here (F12).
            const SpanSrc* out = nullptr;
            ForSpans(d, p, r, [&](const SpanSrc& sp) {
                if (sp.tBeg < t && sp.tEnd == t && !(sp.flags & (SF_ENTERABLE | SF_UNCLOSED)) &&
                    (!out || sp.closeEnd > out->closeEnd))
                    out = &sp;
            });
            return out ? std::max(lEnd, out->closeEnd) : lEnd;
        }
        if (R) {  // block or cell start: inside enterable openers, outside the outermost non-enterable one
            const SpanSrc* out = nullptr;
            ForSpans(d, p, r, [&](const SpanSrc& sp) {
                if (sp.tBeg == t && !(sp.flags & SF_ENTERABLE) && (!out || sp.openBeg < out->openBeg)) out = &sp;
            });
            return out ? std::min(R->s, out->openBeg) : R->s;
        }
        return InsertionPoint(d, src, TextPos{t, p.block, p.cell});
    case MAP_OUTER_START: {
        uint32_t s = R ? R->s : L ? lEnd : InsertionPoint(d, src, TextPos{t, p.block, p.cell});
        ForSpans(d, p, r, [&](const SpanSrc& sp) {
            if (sp.tBeg == t) s = std::min(s, sp.openBeg);
        });
        return s;
    }
    case MAP_OUTER_END: {
        uint32_t s = L ? lEnd : R ? R->s : InsertionPoint(d, src, TextPos{t, p.block, p.cell});
        ForSpans(d, p, r, [&](const SpanSrc& sp) {
            if (sp.tEnd == t && !(sp.flags & SF_UNCLOSED)) s = std::max(s, sp.closeEnd);
        });
        return s;
    }
    case MAP_INNER_START: {
        if (R) return R->s;
        uint32_t s = L ? lEnd : InsertionPoint(d, src, TextPos{t, p.block, p.cell});
        ForSpans(d, p, r, [&](const SpanSrc& sp) {
            if (sp.tBeg == t) s = std::max(s, sp.openEnd);
        });
        return s;
    }
    }
    return UINT32_MAX;
}

// ------------------------------------------------------------------------------------------------ source -> text (§6.4)
TextPos TextOfSrc(const Doc& d, const std::wstring& src, uint32_t s, int dir, uint16_t* trailCols) {
    if (trailCols) *trailCols = 0;
    if (!d.hasMap || d.blockOrder.empty() || d.blockSrc.size() != d.blocks.size()) return TextPos{0, -1, -1};
    const std::vector<uint32_t>& ord = d.blockOrder;
    // the last block (in source order) whose first line starts at or before s; footnote definitions are in there in
    // source order too, so this never assumes text order
    auto it = std::upper_bound(ord.begin(), ord.end(), s, [&](uint32_t v, uint32_t b) { return v < d.blockSrc[b].line; });
    if (it != ord.begin()) {
        int32_t b = (int32_t)*(it - 1);
        if (s <= d.blockSrc[b].outerEnd) return InBlock(d, src, b, s, dir, trailCols);
    }
    // in no block (a blank line, a reference definition): the nearest block boundary in the direction asked
    if ((dir < 0 || it == ord.end()) && it != ord.begin()) return BlockEdge(d, (int32_t)*(it - 1), true);
    if (it != ord.end()) return BlockEdge(d, (int32_t)*it, false);
    return TextPos{0, -1, -1};
}

// ------------------------------------------------------------------------------------------------ self-check (§4.5)
bool MapSelfCheck(const Doc& d, const std::wstring& src, std::string* why) {
    Why w{why};
    if (why) why->clear();
    if (!d.hasMap) return w.Fail("the document has no map");
    const uint32_t n = (uint32_t)src.size(), nb = (uint32_t)d.blocks.size();
    if (d.blockSrc.size() != nb) return w.Fail("blockSrc has %zu records for %u blocks", d.blockSrc.size(), nb);
    if (d.cellSrc.size() != d.cells.size()) return w.Fail("cellSrc %zu vs cells %zu", d.cellSrc.size(), d.cells.size());
    if (d.tableSrc.size() != d.tables.size()) return w.Fail("tableSrc %zu vs tables %zu", d.tableSrc.size(), d.tables.size());

    // 1. segments sorted by t and not overlapping; every one belongs to exactly one block's slice
    for (size_t i = 0; i < d.segs.size(); i++) {
        const SrcSeg& g = d.segs[i];
        if (!g.tLen || g.t + g.tLen > d.text.size()) return w.Fail("seg %zu: text range [%u,+%u) out of the text", i, g.t, g.tLen);
        if (i && d.segs[i - 1].t + d.segs[i - 1].tLen > g.t) return w.Fail("seg %zu: overlaps or precedes seg %zu in the text", i, i - 1);
        if (g.kind > SEG_SYNTH) return w.Fail("seg %zu: kind %u", i, g.kind);
        if (g.s > n || g.sLen > n - g.s) return w.Fail("seg %zu: source range [%u,+%u) out of the source", i, g.s, g.sLen);
    }
    uint32_t claimed = 0, sliceEnd = 0;
    for (uint32_t k = 0; k < nb; k++) {
        const BlockSrc& bs = d.blockSrc[k];
        if (bs.segOff + bs.segCount > d.segs.size()) return w.Fail("block %u: segment slice out of range", k);
        if (bs.spanOff + bs.spanCount > d.spans.size()) return w.Fail("block %u: span slice out of range", k);
        if (bs.segCount) {
            if (bs.segOff < sliceEnd) return w.Fail("block %u: its segments overlap an earlier block's", k);
            if (bs.segOff != sliceEnd) return w.Fail("block %u: segments %u..%u belong to no block", k, sliceEnd, bs.segOff);
            sliceEnd = bs.segOff + bs.segCount;
            claimed += bs.segCount;
        }
    }
    if (claimed != d.segs.size()) return w.Fail("%zu segments belong to no block", d.segs.size() - claimed);

    for (uint32_t k = 0; k < nb; k++) {
        const Block& b = d.blocks[k];
        const BlockSrc& bs = d.blockSrc[k];
        if (bs.flags & BS_SYNTH) {
            if (bs.segCount || bs.spanCount) return w.Fail("block %u: synthesized but has segments or spans", k);
            continue;
        }
        // 6. the record's own order
        if (!(bs.line <= bs.beg && bs.beg <= bs.end && bs.end <= bs.lineEnd && bs.lineEnd <= bs.outerEnd && bs.outerEnd <= n))
            return w.Fail("block %u: line %u beg %u end %u lineEnd %u outerEnd %u (source %u)", k, bs.line, bs.beg, bs.end,
                          bs.lineEnd, bs.outerEnd, n);
        // a record's lines are whole lines: it starts at a line start and its line ends are line ends
        if ((bs.line && !EolChar(src[bs.line - 1])) || (bs.lineEnd < n && !EolChar(src[bs.lineEnd])) ||
            (bs.outerEnd < n && !EolChar(src[bs.outerEnd])))
            return w.Fail("block %u: line %u, lineEnd %u or outerEnd %u is not on a line boundary", k, bs.line, bs.lineEnd,
                          bs.outerEnd);
        if (bs.container >= (int32_t)d.containers.size()) return w.Fail("block %u: container %d", k, bs.container);
        if ((bs.flags & BS_RAW) && (bs.rawId < 0 || bs.rawId > (int32_t)k)) return w.Fail("block %u: rawId %d", k, bs.rawId);
        const uint32_t tEnd = b.textOff + b.textLen;
        const bool atom = AtomBlock(bs);
        if (atom && (bs.segCount || bs.spanCount)) return w.Fail("block %u: an object atom with segments or spans", k);
        // 1, 2, 3, 5: each segment inside the block (and its cell), PLAIN equal to its source, atoms inside the lines
        const Table* tb = TableOf(d, (int32_t)k);
        uint32_t prevSEnd = 0, cover = b.textOff, cell = 0;
        std::vector<const SrcSeg*> real;  // the segments with a source, in source order (#5), for the span check below
        for (uint32_t i = bs.segOff; i < bs.segOff + bs.segCount; i++) {
            const SrcSeg& g = d.segs[i];
            if (g.t < b.textOff || g.t + g.tLen > tEnd) return w.Fail("block %u: seg %u [%u,+%u) outside the block's text [%u,%u)", k, i, g.t, g.tLen, b.textOff, tEnd);
            if (tb) {
                uint32_t cells = tb->rows * tb->cols;
                while (cell < cells && d.cells[tb->cellOff + cell].textOff + d.cells[tb->cellOff + cell].textLen <= g.t) cell++;
                if (cell >= cells) return w.Fail("block %u: seg %u in no cell", k, i);
                const Cell& c = d.cells[tb->cellOff + cell];
                if (g.t < c.textOff || g.t + g.tLen > c.textOff + c.textLen) return w.Fail("block %u: seg %u crosses cell %u", k, i, cell);
                // ... and its source inside that cell's
                const CellSrc& cs = d.cellSrc[tb->cellOff + cell];
                if (g.kind != SEG_SYNTH && !(bs.flags & BS_RAW) && (cs.missing || g.s < cs.beg || g.s + g.sLen > cs.end))
                    return w.Fail("block %u: seg %u source [%u,+%u) outside cell %u's [%u,%u]", k, i, g.s, g.sLen, cell, cs.beg, cs.end);
            }
            // every segment with a source lies in its block's lines, plain text too: a record that belongs to another
            // block (or none) is caught even where its text happens to equal some other part of the source
            if (g.kind != SEG_SYNTH && (g.s < bs.line || g.s + g.sLen > bs.outerEnd))
                return w.Fail("block %u: %s seg %u source [%u,+%u) outside the block's lines [%u,%u]", k, KindName(g.kind), i, g.s, g.sLen, bs.line, bs.outerEnd);
            if (g.kind == SEG_PLAIN) {
                if (g.tLen != g.sLen) return w.Fail("block %u: PLAIN seg %u has tLen %u, sLen %u", k, i, g.tLen, g.sLen);
                if (src.compare(g.s, g.sLen, d.text, g.t, g.tLen) != 0) return w.Fail("block %u: PLAIN seg %u text != source at t %u, s %u", k, i, g.t, g.s);
            } else if (g.kind == SEG_SYNTH) {
                if (g.sLen) return w.Fail("block %u: SYNTH seg %u has source length %u", k, i, g.sLen);
            } else {
                if (!g.sLen) return w.Fail("block %u: %s seg %u has no source", k, KindName(g.kind), i);
                if (g.kind == SEG_TEXTATOM && g.tLen == 1 && g.sLen == 1 && d.text[g.t] == 0xFFFD && src[g.s] != 0 && src[g.s] != 0xFFFD)
                    return w.Fail("block %u: seg %u stands for a NUL but the source holds U+%04X", k, i, src[g.s]);
                // an escape atom ("\*") stands for the character it escapes; a backslash hard break ("\" + a line end)
                // looks the same size but stands for "\n"
                if (g.kind == SEG_TEXTATOM && g.tLen == 1 && g.sLen == 2 && src[g.s] == L'\\' && !EolChar(src[g.s + 1]) &&
                    src[g.s + 1] != d.text[g.t])
                    return w.Fail("block %u: escape seg %u does not escape its character", k, i);
            }
            if (g.kind != SEG_SYNTH) {
                if (g.s < prevSEnd) return w.Fail("block %u: seg %u source %u goes back before %u", k, i, g.s, prevSEnd);
                prevSEnd = g.s + g.sLen;
                real.push_back(&g);
            }
            // 4. coverage: the segments tile the text (cells are contiguous, so a table's text too)
            if (g.t != cover) return w.Fail("block %u: text [%u,%u) is covered by no segment", k, cover, g.t);
            cover = g.t + g.tLen;
        }
        if (!atom && cover != tEnd && !(bs.segCount == 0 && b.textLen == 0))
            return w.Fail("block %u: text [%u,%u) is covered by no segment", k, cover, tEnd);
        // 7. spans: delimiters in order, nested, inside the block
        for (uint32_t i = bs.spanOff; i < bs.spanOff + bs.spanCount; i++) {
            const SpanSrc& a = d.spans[i];
            if (a.block != (int32_t)k) return w.Fail("span %u: block %d, listed under %u", i, a.block, k);
            bool open = (a.flags & SF_UNCLOSED) != 0;
            if (!(a.openBeg <= a.openEnd && a.openEnd <= a.closeBeg && (open || a.closeBeg <= a.closeEnd)))
                return w.Fail("span %u: open [%u,%u) close [%u,%u)", i, a.openBeg, a.openEnd, a.closeBeg, a.closeEnd);
            if (a.openBeg < bs.line || a.closeEnd > bs.outerEnd) return w.Fail("span %u: [%u,%u) outside block %u's lines", i, a.openBeg, a.closeEnd, k);
            if (!(b.textOff <= a.tBeg && a.tBeg <= a.tEnd && a.tEnd <= tEnd)) return w.Fail("span %u: text [%u,%u) outside block %u", i, a.tBeg, a.tEnd, k);
            for (uint32_t j = bs.spanOff; j < i; j++) {
                const SpanSrc& c = d.spans[j];
                auto partial = [](uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
                    return (a0 < b0 && b0 < a1 && a1 < b1) || (b0 < a0 && a0 < b1 && b1 < a1);
                };
                if (partial(a.tBeg, a.tEnd, c.tBeg, c.tEnd)) return w.Fail("spans %u and %u overlap in the text", j, i);
                if (partial(a.openBeg, a.closeEnd, c.openBeg, c.closeEnd)) return w.Fail("spans %u and %u overlap in the source", j, i);
            }
            // No text stands for a piece of a delimiter: a segment reaching into an opener or a closer is wrong, unless
            // it is the atom that stands for the whole span (a picture, a formula, a footnote reference).
            auto intoDelim = [&](uint32_t db, uint32_t de) -> const SrcSeg* {
                if (db >= de) return nullptr;
                auto it = std::lower_bound(real.begin(), real.end(), db,
                                           [](const SrcSeg* g, uint32_t v) { return g->s + g->sLen <= v; });
                if (it == real.end() || (*it)->s >= de) return nullptr;
                const SrcSeg* g = *it;
                return g->s <= a.openBeg && g->s + g->sLen >= (open ? a.closeBeg : a.closeEnd) ? nullptr : g;
            };
            const SrcSeg* bad = intoDelim(a.openBeg, a.openEnd);
            if (!bad && !open) bad = intoDelim(a.closeBeg, a.closeEnd);
            if (bad)
                return w.Fail("span %u (delimiters [%u,%u) [%u,%u)): seg at source [%u,+%u) reaches into a delimiter", i,
                              a.openBeg, a.openEnd, a.closeBeg, a.closeEnd, bad->s, bad->sLen);
        }
        // 8. cells inside their rows, pipes that are pipes (HTML and front-matter tables are raw: no cell sources)
        if (tb && !(bs.flags & BS_RAW) && d.blocks[k].aux < d.tableSrc.size()) {
            const TableSrc& ts = d.tableSrc[d.blocks[k].aux];
            if (tb->rows && ts.rows.size() != tb->rows + 1) return w.Fail("table of block %u: %zu row records for %u rows", k, ts.rows.size(), tb->rows);
            for (size_t ri = 0; ri < ts.rows.size(); ri++) {
                const RowSrc& row = ts.rows[ri];
                if (!(row.lineStart <= row.contentStart && row.contentStart <= row.lineEnd && row.lineEnd <= n))
                    return w.Fail("table of block %u, row %zu: %u %u %u", k, ri, row.lineStart, row.contentStart, row.lineEnd);
                for (uint32_t p : row.pipes)
                    if (p >= n || src[p] != L'|') return w.Fail("table of block %u, row %zu: pipe at %u is not a pipe", k, ri, p);
            }
            for (uint32_t r = 0; r < tb->rows && !ts.rows.empty(); r++) {
                const RowSrc& row = ts.rows[r == 0 ? 0 : r + 1];
                for (uint32_t c = 0; c < tb->cols; c++) {
                    const CellSrc& cs = d.cellSrc[tb->cellOff + r * tb->cols + c];
                    if (!cs.missing && !(row.contentStart <= cs.beg && cs.beg <= cs.end && cs.end <= row.lineEnd))
                        return w.Fail("table of block %u: cell %u,%u [%u,%u] outside its row [%u,%u]", k, r, c, cs.beg, cs.end, row.contentStart, row.lineEnd);
                }
            }
        }
    }

    // 6. blockOrder: every block with a source record once, sorted by line; the normal flow never goes back
    {
        std::vector<uint8_t> seen(nb, 0);
        uint32_t prev = 0;
        for (size_t i = 0; i < d.blockOrder.size(); i++) {
            uint32_t k = d.blockOrder[i];
            if (k >= nb || seen[k]++) return w.Fail("blockOrder[%zu] = %u: out of range or repeated", i, k);
            if (d.blockSrc[k].flags & BS_SYNTH) return w.Fail("blockOrder holds synthesized block %u", k);
            if (d.blockSrc[k].line < prev) return w.Fail("blockOrder is not sorted at %zu", i);
            prev = d.blockSrc[k].line;
            // Two blocks never claim the same source line, or TextOfSrc could not tell whose it is - except the
            // blocks one HTML block became, which share its record.
            if (i) {
                const BlockSrc& a = d.blockSrc[d.blockOrder[i - 1]];
                const BlockSrc& c = d.blockSrc[k];
                bool shared = (a.flags & BS_RAW) && (c.flags & BS_RAW) && a.rawId >= 0 && a.rawId == c.rawId;
                if (!shared && c.line <= a.outerEnd)
                    return w.Fail("blocks %u and %u share source lines ([%u,%u] and [%u,%u])", d.blockOrder[i - 1], k,
                                  a.line, a.outerEnd, c.line, c.outerEnd);
            }
        }
        for (uint32_t k = 0; k < nb; k++)
            if (!seen[k] && !(d.blockSrc[k].flags & BS_SYNTH)) return w.Fail("block %u is missing from blockOrder", k);
        uint32_t line = 0;
        for (uint32_t k = 0; k < nb; k++) {
            const BlockSrc& bs = d.blockSrc[k];
            if (bs.flags & (BS_SYNTH | BS_FOOTNOTE)) continue;
            if (bs.line < line) return w.Fail("block %u starts at line %u, before the block ahead of it (%u)", k, bs.line, line);
            line = bs.line;
        }
    }

    // 9. containers: markers and task marks where they say they are
    std::vector<uint32_t> marks;
    for (size_t i = 0; i < d.containers.size(); i++) {
        const ContainerSrc& c = d.containers[i];
        if (c.parent >= (int32_t)i) return w.Fail("container %zu: parent %d", i, c.parent);
        if (c.firstBlock != UINT32_MAX && (c.firstBlock > c.lastBlock || c.lastBlock >= nb))
            return w.Fail("container %zu: blocks %u..%u", i, c.firstBlock, c.lastBlock);
        if (c.kind == CT_ITEM) {
            if (c.markOff >= n) return w.Fail("item %zu: marker at %u", i, c.markOff);
            wchar_t m = src[c.markOff];
            if (!(m == L'-' || m == L'+' || m == L'*' || (m >= L'0' && m <= L'9'))) return w.Fail("item %zu: marker '%lc'", i, m);
            // the whole marker: one bullet character, or digits and '.' / ')'; then a blank or the line end
            uint32_t me = c.markOff + c.markLen;
            bool bullet = m == L'-' || m == L'+' || m == L'*';
            bool shape = c.markLen >= 1 && me <= n && (bullet ? c.markLen == 1 : (src[me - 1] == L'.' || src[me - 1] == L')'));
            for (uint32_t p = c.markOff; shape && !bullet && p + 1 < me; p++) shape = src[p] >= L'0' && src[p] <= L'9';
            if (!shape || (me < n && !Blank(src[me]) && !EolChar(src[me])))
                return w.Fail("item %zu: [%u,+%u) is not a list marker", i, c.markOff, c.markLen);
            marks.push_back(c.markOff);
            if (c.firstBlock != UINT32_MAX && !(d.blockSrc[c.firstBlock].flags & BS_SYNTH) &&
                c.markOff >= d.blockSrc[c.firstBlock].beg)
                return w.Fail("item %zu: marker at %u is not before its first block's content at %u", i, c.markOff,
                              d.blockSrc[c.firstBlock].beg);
            if (c.taskOff != UINT32_MAX) {
                if (c.taskOff < 1 || c.taskOff + 1 >= n) return w.Fail("item %zu: task mark at %u", i, c.taskOff);
                wchar_t t = src[c.taskOff];
                if (!(t == L' ' || t == L'x' || t == L'X') || src[c.taskOff - 1] != L'[' || src[c.taskOff + 1] != L']')
                    return w.Fail("item %zu: no [ ] around the task mark at %u", i, c.taskOff);
            }
        }
    }
    // every item has a marker of its own (a nested "- - a" has two)
    std::vector<bool> marked(n + 1, false);
    for (uint32_t m : marks) {
        if (marked[m]) return w.Fail("two items have the same marker at %u", m);
        marked[m] = true;
    }

    // 10. pictures, formulas and diagrams: their ranges nest and lie in the lines of the block that shows them
    std::vector<int32_t> blockOfImage(d.images.size(), -1);
    for (uint32_t k = 0; k < nb; k++) {
        const Block& b = d.blocks[k];
        if (b.kind == BK_IMAGE && b.aux < d.images.size()) blockOfImage[b.aux] = (int32_t)k;
        auto runs = [&](uint32_t off, uint32_t cnt) {
            for (uint32_t r = off; r < off + cnt && r < d.runs.size(); r++)
                if ((d.runs[r].flags & F_IMAGE) && d.runs[r].image < d.images.size() && blockOfImage[d.runs[r].image] < 0)
                    blockOfImage[d.runs[r].image] = (int32_t)k;
        };
        runs(b.runOff, b.runCount);
        if (const Table* tb = TableOf(d, (int32_t)k))
            for (uint32_t c = 0; c < tb->rows * tb->cols; c++) runs(d.cells[tb->cellOff + c].runOff, d.cells[tb->cellOff + c].runCount);
    }
    for (size_t i = 0; i < d.images.size(); i++) {
        const Image& im = d.images[i];
        if (im.outerBeg == UINT32_MAX) continue;
        if (im.outerEnd == UINT32_MAX || im.outerBeg > im.outerEnd || im.outerEnd > n) return w.Fail("image %zu: outer [%u,%u)", i, im.outerBeg, im.outerEnd);
        if (im.srcBeg != UINT32_MAX && !(im.outerBeg <= im.srcBeg && im.srcBeg <= im.srcEnd && im.srcEnd <= im.outerEnd))
            return w.Fail("image %zu: source [%u,%u) outside [%u,%u)", i, im.srcBeg, im.srcEnd, im.outerBeg, im.outerEnd);
        if (im.altBeg != UINT32_MAX && !(im.outerBeg <= im.altBeg && im.altBeg <= im.altEnd && im.altEnd <= im.outerEnd))
            return w.Fail("image %zu: alt [%u,%u) outside [%u,%u)", i, im.altBeg, im.altEnd, im.outerBeg, im.outerEnd);
        int32_t k = blockOfImage[i];
        if (k < 0) continue;  // not shown (a picture inside another picture's alt text)
        const BlockSrc& bs = d.blockSrc[k];
        if (!(bs.flags & BS_SYNTH) && (im.outerBeg < bs.line || im.outerEnd > bs.outerEnd))
            return w.Fail("image %zu: [%u,%u) outside the lines [%u,%u] of block %d", i, im.outerBeg, im.outerEnd, bs.line, bs.outerEnd, k);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ block diff (§5.5)
BlockDiff DiffBlocks(const Doc& oldD, const Doc& newD) {
    uint32_t na = (uint32_t)oldD.blocks.size(), nb = (uint32_t)newD.blocks.size(), m = std::min(na, nb);
    uint32_t p = 0, q = 0;
    while (p < m && SameBlock(oldD, p, newD, p)) p++;
    while (q < m - p && SameBlock(oldD, na - 1 - q, newD, nb - 1 - q)) q++;
    return BlockDiff{p, q};
}

// ------------------------------------------------------------------------------------------------ prefixes (§7.2)
// What a new continuation line of block b starts with inside its n outermost containers (n < 0: all of them): for each
// container outer -> inner, a quote's ">" (and the blank after it when b's own first line has one there), a list item's
// indentation up to its content column; a footnote adds nothing. *at: where those containers' markers end on b's first
// line (a phantom before b stands there, §6.7).
static std::wstring PrefixN(const Doc& d, const std::wstring& src, int block, int n, uint32_t* at) {
    std::wstring out;
    if (at) *at = 0;
    if (!ValidBlock(d, block)) return out;
    const BlockSrc& bs = d.blockSrc[block];
    if (bs.flags & BS_SYNTH) return out;
    // p walks b's first line, col is p's column there: an item's content column is counted from the line start, with
    // every '>' and blank before it, so the spaces it needs are what is left after the quote markers written so far
    uint32_t le = LineEndOf(src, bs.line), p = bs.line, col = 0;
    auto step = [&]() { col = src[p] == L'\t' ? (col + 4) & ~3u : col + 1; p++; };
    int k = 0;
    for (const ContainerSrc* c : Chain(d, block)) {
        if (n >= 0 && k++ >= n) break;
        if (c->kind == CT_QUOTE || c->kind == CT_ALERT) {
            uint32_t q = p;
            while (q < le && q - p < 3 && src[q] == L' ') q++;  // up to three spaces before the '>'
            if (q < le && src[q] == L'>') {
                while (p <= q) step();
                out += L'>';
                if (p < le && Blank(src[p])) {
                    out += L' ';
                    step();
                }
            } else {  // the line does not show the marker (a lazy line): the usual form
                out += L"> ";
                col += 2;
            }
        } else if (c->kind == CT_ITEM) {
            if (c->contentCol > col) out.append(c->contentCol - col, L' ');
            while (p < le && col < c->contentCol) step();  // over the marker and its blanks (or the indentation)
            col = std::max<uint32_t>(col, c->contentCol);
        }
    }
    if (at) *at = p;
    return out;
}
std::wstring ContPrefix(const Doc& d, const std::wstring& src, int block) { return PrefixN(d, src, block, -1, nullptr); }

std::wstring BlankPrefix(const Doc& d, const std::wstring& src, int block) {
    std::wstring p = ContPrefix(d, src, block);
    while (!p.empty() && Blank(p.back())) p.pop_back();
    return p;
}

// the ending of the source line that holds s; the last line has none and gets the fallback (the file's own EOL)
const wchar_t* LineEol(const std::wstring& src, uint32_t s, const wchar_t* fallback) {
    uint32_t n = (uint32_t)src.size();
    if (s < n && s > 0 && src[s] == L'\n' && src[s - 1] == L'\r') return L"\r\n";  // between the two halves of a CRLF
    uint32_t e = LineEndOf(src, s);
    if (e >= n) return fallback;
    if (src[e] == L'\n') return L"\n";
    return e + 1 < n && src[e + 1] == L'\n' ? L"\r\n" : L"\r";
}

// ------------------------------------------------------------------------------------------------ splices (§7.1, §11)
bool SpliceSplits(const std::wstring& src, uint32_t at, uint32_t len) {
    auto cut = [&](size_t i) {
        return i > 0 && i < src.size() && ((HighSur(src[i - 1]) && LowSur(src[i])) || (src[i - 1] == L'\r' && src[i] == L'\n'));
    };
    return cut(at) || cut((size_t)at + len);
}

bool ApplySplices(std::wstring& src, const std::vector<Splice>& sps, bool inverse, std::string* why) {
    const size_t n = sps.size();
    auto nth = [&](size_t k) -> const Splice& { return sps[inverse ? n - 1 - k : k]; };
    for (size_t k = 0; k < n; k++) {
        const Splice& s = nth(k);
        const std::wstring& there = inverse ? s.inserted : s.removed;
        if (s.at > src.size() || src.size() - s.at < there.size() || src.compare(s.at, there.size(), there) != 0) {
            for (size_t j = k; j-- > 0;) {  // put back what was applied: all or nothing
                const Splice& t = nth(j);
                src.replace(t.at, (inverse ? t.removed : t.inserted).size(), inverse ? t.inserted : t.removed);
            }
            if (why) {
                char b[96];
                snprintf(b, sizeof b, "splice %zu of %zu at %u: the text it replaces is not there", k + 1, n, s.at);
                *why = b;
            }
            return false;
        }
        src.replace(s.at, there.size(), inverse ? s.removed : s.inserted);
    }
    return true;
}

namespace {
size_t StepBytes(const EditStep& s) {
    size_t b = sizeof(EditStep);
    for (const Splice& sp : s.splices) b += sizeof(Splice) + (sp.removed.size() + sp.inserted.size()) * sizeof(wchar_t);
    return b;
}
}  // namespace

void UndoStack::Push(EditStep st, uint64_t nowMs) {
    if (!st.t0) st.t0 = nowMs;
    st.t1 = nowMs;
    for (const EditStep& r : redo_) bytes_ -= StepBytes(r);  // a new step ends the redo history
    redo_.clear();
    EditStep* top = undo_.empty() ? nullptr : &undo_.back();
    bool merge = top && !broken_ && top->kind == st.kind &&
                 (st.kind == EK_TYPE || st.kind == EK_DEL_BACK || st.kind == EK_DEL_FWD) &&
                 top->after.focus == st.before.focus && nowMs >= top->t1 && nowMs - top->t1 < 1500;
    if (merge && st.kind == EK_TYPE && !top->splices.empty() && !top->splices.back().inserted.empty() &&
        !st.splices.empty() && !st.splices[0].inserted.empty()) {
        // a word typed after a blank is a step of its own: one undo takes back a word, not the whole sentence
        if (Blank(top->splices.back().inserted.back()) && !Blank(st.splices[0].inserted[0])) merge = false;
    }
    if (!merge) {
        bytes_ += StepBytes(st);
        undo_.push_back(std::move(st));
    } else {
        bytes_ -= StepBytes(*top);
        for (Splice& sp : st.splices) {
            Splice* last = top->splices.empty() ? nullptr : &top->splices.back();
            if (last && sp.removed.empty() && last->removed.empty() && sp.at == last->at + last->inserted.size()) {
                last->inserted += sp.inserted;  // typing on where the last character went
            } else if (last && sp.inserted.empty() && last->inserted.empty() && sp.at + sp.removed.size() == last->at) {
                last->at = sp.at;  // Backspace on from where the last one stopped
                last->removed.insert(0, sp.removed);
            } else if (last && sp.inserted.empty() && last->inserted.empty() && sp.at == last->at) {
                last->removed += sp.removed;  // Delete on at the same place
            } else {
                top->splices.push_back(std::move(sp));
            }
        }
        top->after = st.after;
        top->t1 = nowMs;
        bytes_ += StepBytes(*top);
    }
    broken_ = false;
    Trim();
}

void UndoStack::Trim() {
    size_t drop = 0;
    while (drop < undo_.size() && (undo_.size() - drop > kMaxSteps || bytes_ > kMaxBytes)) bytes_ -= StepBytes(undo_[drop++]);
    if (drop) undo_.erase(undo_.begin(), undo_.begin() + drop);
}

void UndoStack::DidUndo() {
    if (undo_.empty()) return;
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    broken_ = true;
}

void UndoStack::DidRedo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    broken_ = true;
}

void UndoStack::Clear() {
    undo_.clear();
    redo_.clear();
    bytes_ = 0;
    broken_ = true;
}

// ------------------------------------------------------------------------------------------------ keyboard (§2.7, §12.5)
// The chords of edit mode, as command ids (app.h's Cmd; the numbers are frozen, §13.1). Ctrl+Alt is AltGr on many
// layouts - Polish ż, German € - so it is never a chord here: that text reaches WM_CHAR.
unsigned EditChord(unsigned vk, bool ctrl, bool shift, bool alt) {
    enum : unsigned { C_COPY = 100, C_SELECT_ALL = 101, C_RELOAD = 103, C_EDIT = 104, C_COPY_MD = 137, C_TOGGLE = 141,
                      C_UNDO = 144, C_REDO = 145, C_CUT = 146, C_PASTE = 147, C_SAVE = 148, C_BOLD = 150, C_ITALIC = 151,
                      C_STRIKE = 152, C_CODE = 153, C_LINK = 154, C_H1 = 157, C_BULLET = 163, C_NUMBER = 164, C_TASK = 165,
                      C_QUOTE = 166, C_CODEBLOCK = 167, C_TABLE = 169, C_FORMULA = 170, C_FORMULA_BLOCK = 171,
                      C_NEW_PARAGRAPH = 175 };
    if (alt) return 0;
    if (!ctrl) {
        if (vk == VK_F2 && !shift) return C_TOGGLE;
        if (vk == VK_F5 && !shift) return C_RELOAD;
        if (shift && vk == VK_DELETE) return C_CUT;
        if (shift && vk == VK_INSERT) return C_PASTE;
        return 0;
    }
    if (vk >= '1' && vk <= '6' && !shift) return C_H1 + (vk - '1');
    switch (vk) {
    case 'Z': return shift ? C_REDO : C_UNDO;
    case 'Y': return shift ? 0 : C_REDO;
    case 'S': return shift ? 0 : C_SAVE;
    case 'A': return C_SELECT_ALL;  // with Shift too: reading mode's Ctrl+A ignores it, and would select its own way
    case 'C': return shift ? C_COPY_MD : C_COPY;
    case VK_INSERT: return shift ? 0 : C_COPY;
    case 'X': return shift ? C_STRIKE : C_CUT;
    case 'V': return shift ? 0 : C_PASTE;
    case 'B': return shift ? 0 : C_BOLD;
    case 'I': return shift ? 0 : C_ITALIC;
    case VK_OEM_3: return shift ? 0 : C_CODE;
    case 'K': return shift ? C_CODEBLOCK : C_LINK;
    case '7': return shift ? C_NUMBER : 0;
    case '8': return shift ? C_BULLET : 0;
    case '9': return shift ? C_TASK : 0;
    case 'Q': return shift ? C_QUOTE : 0;
    case 'T': return shift ? 0 : C_TABLE;
    case 'M': return shift ? C_FORMULA_BLOCK : C_FORMULA;
    case VK_RETURN: return shift ? 0 : C_NEW_PARAGRAPH;
    case 'E': return C_EDIT;  // with Shift too: the external editor only after the flush (D19)
    case 'R': return shift ? 0 : C_RELOAD;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------ clusters (§6.2)
namespace {
uint32_t CpAt(const std::wstring& t, uint32_t i, uint32_t* len) {
    if (i + 1 < t.size() && HighSur(t[i]) && LowSur(t[i + 1])) {
        *len = 2;
        return 0x10000 + (((uint32_t)t[i] - 0xD800) << 10) + ((uint32_t)t[i + 1] - 0xDC00);
    }
    *len = 1;
    return t[i];
}
// what joins the cluster before it: combining marks, variation selectors, emoji skin tones and tag characters
bool Extends(uint32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF) ||
           (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x1F3FB && c <= 0x1F3FF) ||
           (c >= 0xE0020 && c <= 0xE007F) || (c >= 0xE0100 && c <= 0xE01EF);
}
bool Regional(uint32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }
uint32_t ClusterEnd(const std::wstring& t, uint32_t pos) {
    uint32_t n = (uint32_t)t.size(), len;
    if (pos >= n) return n;
    uint32_t cp = CpAt(t, pos, &len), p = pos + len;
    if (Regional(cp) && p < n) {  // a flag is two regional indicators
        uint32_t l2;
        if (Regional(CpAt(t, p, &l2))) p += l2;
    }
    while (p < n) {
        uint32_t l2, c2 = CpAt(t, p, &l2);
        if (Extends(c2)) { p += l2; continue; }
        if (c2 == 0x200D) {  // a zero-width joiner takes the next character with it
            p += l2;
            if (p < n) { CpAt(t, p, &l2); p += l2; }
            continue;
        }
        break;
    }
    return p;
}
}  // namespace

uint32_t GraphemeLite(const std::wstring& t, uint32_t pos, int dir, void*) {
    uint32_t n = (uint32_t)t.size();
    if (dir > 0) return ClusterEnd(t, std::min(pos, n));
    if (pos == 0) return 0;
    // Backwards the clusters are found from a place that surely starts one - a character below U+0300 that no joiner
    // precedes - and walked forwards (at most 128 units back; a longer run of joined characters is stepped by pairs).
    uint32_t s0 = pos - 1;
    for (uint32_t k = 0; s0 > 0 && k < 128; k++, s0--)
        if (t[s0] < 0x300 && t[s0 - 1] != 0x200D) break;
    uint32_t last = s0, p = s0;
    while (p < pos) {
        last = p;
        p = ClusterEnd(t, p);
    }
    return last;
}

uint32_t ClusterStep(const EditCtx& c, uint32_t pos, int dir, uint32_t lo, uint32_t hi) {
    uint32_t r = c.clusters ? c.clusters(c.doc.text, pos, dir, c.clusterCtx) : GraphemeLite(c.doc.text, pos, dir, nullptr);
    if (dir > 0 && r <= pos) r = pos + 1;
    if (dir < 0 && r >= pos) r = pos ? pos - 1 : 0;
    return std::clamp(r, lo, hi);
}

// ------------------------------------------------------------------------------------------------ atoms (§6.2)
bool IsAtomBlock(const Doc& d, int32_t b) {
    return ValidBlock(d, b) && AtomBlock(d.blockSrc[b]) && !(d.blockSrc[b].flags & BS_SYNTH);
}
int32_t AtomOfBlock(const Doc& d, int32_t b) {
    if (!IsAtomBlock(d, b)) return -1;
    const BlockSrc& bs = d.blockSrc[b];
    return kAtomBlock | ((bs.flags & BS_RAW) && bs.rawId >= 0 ? bs.rawId : b);
}
int32_t AtomBlockOf(const Doc& d, int32_t atom) {
    if (atom < 0) return -1;
    if (atom & kAtomBlock) {
        int32_t b = atom & ~kAtomBlock;
        return (size_t)b < d.blocks.size() ? b : -1;
    }
    for (uint32_t i = 0; i < d.blocks.size(); i++) {  // the block (or table) that holds the picture in its line
        const Block& b = d.blocks[i];
        auto has = [&](uint32_t off, uint32_t n) {
            for (uint32_t k = 0; k < n && off + k < d.runs.size(); k++)
                if ((d.runs[off + k].flags & F_IMAGE) && d.runs[off + k].image == (uint32_t)atom) return true;
            return false;
        };
        if (has(b.runOff, b.runCount)) return (int32_t)i;
        if (b.kind == BK_TABLE && b.aux < d.tables.size()) {
            const Table& tb = d.tables[b.aux];
            for (uint32_t k = 0; k < tb.rows * tb.cols; k++)
                if (has(d.cells[tb.cellOff + k].runOff, d.cells[tb.cellOff + k].runCount)) return (int32_t)i;
        }
    }
    return -1;
}

// ------------------------------------------------------------------------------------------------ typing (§7.3)
// From here on the operations: each runs once per key, command or paste, never in a loop over the document, and they
// build their splices out of many small string operations. Inlining those costs about 30 KB of the exe together with the
// glue's (§1 principle 3), so nothing is inlined here; the map queries above keep their inlining, they are measured (§5.8).
#pragma inline_depth(0)
namespace {
// a few elements put in order: an insertion sort (std::sort's code is kilobytes for every place it is used)
template <class T, class Less> void Order(std::vector<T>& v, Less less) {
    for (size_t i = 1; i < v.size(); i++)
        for (size_t k = i; k > 0 && less(v[k], v[k - 1]); k--) std::swap(v[k], v[k - 1]);
}
EditResult Nothing(const EditState& st, EditKind k) {
    EditResult r;
    r.after = st;
    r.kind = k;
    return r;
}
EditResult Refuse(const EditState& st, const char* why) {
    EditResult r = Nothing(st, EK_OTHER);
    r.refused = why;
    return r;
}
TextPos FocusOf(const EditCtx& c, const EditState& st, uint16_t* trail) {
    if (c.focusPos.block >= 0) {
        *trail = c.trail;
        return c.focusPos;
    }
    return TextOfSrc(c.doc, c.src, st.focus, 1, trail);
}
TextPos AnchorOf(const EditCtx& c, const EditState& st) {
    if (c.anchorPos.block >= 0) return c.anchorPos;
    uint16_t tr = 0;
    return TextOfSrc(c.doc, c.src, st.anchor, 1, &tr);
}
bool LineEndAt(const std::wstring& src, uint32_t s) { return s >= src.size() || EolChar(src[s]); }
// a run of blanks from s up to a line end (or, in a table, up to a cell's pipe): trailing blanks (§6.5)
bool TrailingRun(const std::wstring& src, uint32_t s, bool cell) {
    uint32_t n = (uint32_t)src.size(), p = s;
    if (p >= n || !Blank(src[p])) return false;
    while (p < n && Blank(src[p])) p++;
    return p >= n || EolChar(src[p]) || (cell && src[p] == L'|');
}
bool WordChar(wchar_t c) {
    WORD t = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &t);
    return (t & (C1_ALPHA | C1_DIGIT)) || c == L'_' || HighSur(c) || LowSur(c);
}
bool SpaceChar(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == 0xA0 || c == 0x3000; }
// Windows' word rules: back over blanks, then over a word or a run of punctuation (Ctrl+Backspace, Ctrl+←)
uint32_t WordBack(const std::wstring& t, uint32_t p, uint32_t lo) {
    while (p > lo && SpaceChar(t[p - 1])) p--;
    if (p > lo && WordChar(t[p - 1])) while (p > lo && WordChar(t[p - 1])) p--;
    else while (p > lo && !WordChar(t[p - 1]) && !SpaceChar(t[p - 1])) p--;
    return p;
}
// forwards over a word or a run of punctuation, then over the blanks after it (Ctrl+Delete, Ctrl+→)
uint32_t WordFwd(const std::wstring& t, uint32_t p, uint32_t hi) {
    if (p < hi && WordChar(t[p])) while (p < hi && WordChar(t[p])) p++;
    else while (p < hi && !WordChar(t[p]) && !SpaceChar(t[p])) p++;
    while (p < hi && SpaceChar(t[p])) p++;
    return p;
}
// the segment that covers text offset t (t inside it or at its start)
const SrcSeg* SegCovering(SegSpan ss, uint32_t t) {
    if (ss.b == ss.e) return nullptr;
    const SrcSeg* it = std::upper_bound(ss.b, ss.e, t, [](uint32_t v, const SrcSeg& g) { return v < g.t; });
    if (it == ss.b) return nullptr;
    --it;
    return t < it->t + it->tLen ? it : nullptr;
}
const SrcSeg* SegEndingAt(SegSpan ss, uint32_t t) {
    const SrcSeg* g = t ? SegCovering(ss, t - 1) : nullptr;
    return g && g->t + g->tLen == t ? g : nullptr;
}
const SrcSeg* SegStartingAt(SegSpan ss, uint32_t t) {
    const SrcSeg* g = SegCovering(ss, t);
    return g && g->t == t ? g : nullptr;
}
// a soft break (or the blank that joins the lines of a code span): an atom drawn as one blank over a line end
bool SoftBreak(const Doc& d, const std::wstring& src, const SrcSeg* g) {
    if (!g || g->kind != SEG_TEXTATOM || g->tLen != 1 || d.text[g->t] != L' ') return false;
    for (uint32_t k = g->s; k < g->s + g->sLen && k < src.size(); k++)
        if (EolChar(src[k])) return true;
    return false;
}
// the text the typed characters show as, and the ones that are Markdown and render as something else
bool Significant(wchar_t c) { return c && wcschr(L"\\`*_~$[]()!<>#|=+-:&", c) != nullptr; }

// the formatting of the character at text offset t (0 = none, or outside any run)
uint16_t FlagsAt(const Doc& d, uint32_t t) {
    const std::vector<Block>& bl = d.blocks;
    auto it = std::upper_bound(bl.begin(), bl.end(), t, [](uint32_t v, const Block& b) { return v < b.textOff; });
    if (it == bl.begin()) return 0;
    const Block& b = *(it - 1);
    auto in = [&](uint32_t off, uint32_t n) -> int {
        for (uint32_t k = 0; k < n && off + k < d.runs.size(); k++) {
            const Run& r = d.runs[off + k];
            if (t >= r.start && t < r.start + r.len) return r.flags;
        }
        return -1;
    };
    int f = in(b.runOff, b.runCount);
    if (f < 0 && b.kind == BK_TABLE && b.aux < d.tables.size()) {
        const Table& tb = d.tables[b.aux];
        for (uint32_t k = 0; k < tb.rows * tb.cols && f < 0; k++) {
            const Cell& c = d.cells[tb.cellOff + k];
            if (t >= c.textOff && t < c.textOff + c.textLen) f = in(c.runOff, c.runCount);
        }
    }
    return f < 0 ? 0 : (uint16_t)f;
}

// the block before / after b in the source (footnote definitions stand where they are written)
int32_t PrevInSource(const Doc& d, int32_t b) {
    const BlockSrc& bs = d.blockSrc[b];
    int32_t best = -1;
    for (uint32_t k : d.blockOrder) {
        if (d.blockSrc[k].line >= bs.line) break;
        best = (int32_t)k;
    }
    if (best >= 0 && (d.blockSrc[best].flags & BS_RAW) && d.blockSrc[best].rawId >= 0) best = d.blockSrc[best].rawId;
    return best;
}
int32_t NextInSource(const Doc& d, int32_t b) {
    const BlockSrc& bs = d.blockSrc[b];
    for (uint32_t k : d.blockOrder)
        if (d.blockSrc[k].line > bs.outerEnd) return (int32_t)k;
    return -1;
}
// the picture a SEG_OBJATOM stands for
int32_t ImageOfSeg(const Doc& d, int32_t block, const SrcSeg& g) {
    const Block& b = d.blocks[block];
    auto find = [&](uint32_t off, uint32_t n) -> int32_t {
        for (uint32_t k = 0; k < n && off + k < d.runs.size(); k++) {
            const Run& r = d.runs[off + k];
            if ((r.flags & F_IMAGE) && r.start == g.t) return (int32_t)r.image;
        }
        return -1;
    };
    int32_t im = find(b.runOff, b.runCount);
    if (im < 0 && b.kind == BK_TABLE && b.aux < d.tables.size()) {
        const Table& tb = d.tables[b.aux];
        for (uint32_t k = 0; k < tb.rows * tb.cols && im < 0; k++)
            im = find(d.cells[tb.cellOff + k].runOff, d.cells[tb.cellOff + k].runCount);
    }
    return im;
}
// a line that holds nothing but blanks and quote markers
bool BlankLine(const std::wstring& src, uint32_t ls, uint32_t le) {
    for (uint32_t k = ls; k < le; k++)
        if (!Blank(src[k]) && src[k] != L'>') return false;
    return true;
}
uint32_t SkipEol(const std::wstring& src, uint32_t s) {
    if (s < src.size() && src[s] == L'\r') s++;
    if (s < src.size() && src[s] == L'\n') s++;
    return s;
}

// ---- the structure of Phase 2b: phantoms, splits, joins, prefixes, escapes (§6.7, §7.4-§7.11)
bool InPh(const EditState& st) { return st.phantom.kind != PH_NONE && st.phantom.in; }
const wchar_t* Eol(const EditCtx& c, uint32_t s) { return LineEol(c.src, s, c.eol); }
std::wstring Strip(std::wstring p) {
    while (!p.empty() && Blank(p.back())) p.pop_back();
    return p;
}
std::wstring Sub(const std::wstring& s, uint32_t a, uint32_t b) {
    return b > a && a < s.size() ? s.substr(a, b - a) : std::wstring();
}
Splice Replace(const std::wstring& src, uint32_t a, uint32_t b, std::wstring ins) { return Splice{a, Sub(src, a, b), std::move(ins)}; }
bool PosBefore(const TextPos& a, const TextPos& b) {
    if (a.block != b.block) return a.block < b.block;
    if (a.cell != b.cell) return a.cell < b.cell;
    return a.t < b.t;
}
// a line break of the text that is one in the source too: a soft or a hard break (a <br> tag has no line end)
bool LineBrk(const std::wstring& src, const SrcSeg* g) {
    if (!g || g->kind != SEG_TEXTATOM) return false;
    for (uint32_t k = g->s; k < g->s + g->sLen && k < src.size(); k++)
        if (EolChar(src[k])) return true;
    return false;
}
// where a break's line end starts: its blanks or its backslash come before, the next line's prefix after
uint32_t BrkEol(const std::wstring& src, const SrcSeg& g) {
    uint32_t k = g.s;
    while (k < g.s + g.sLen && k < src.size() && !EolChar(src[k])) k++;
    return k;
}
// a block of text: joins, splits and §7.4 are about these (not code, tables, atoms or synthesized blocks)
bool TextBlk(const Doc& d, int32_t b) {
    return ValidBlock(d, b) && d.blocks[b].kind == BK_TEXT && !(d.blockSrc[b].flags & (BS_SYNTH | BS_OBJECT | BS_RAW));
}
// a paragraph (a list item's text too): where a line's start could turn into block syntax by accident (§7.4)
bool Para(const Doc& d, int32_t b) {
    return TextBlk(d, b) && !d.blocks[b].heading && !(d.blockSrc[b].flags & (BS_FOOTNOTE | BS_RAWTEXT));
}
// the list item whose first block b is, -1 none
int32_t ItemOf(const Doc& d, int32_t b) {
    int32_t ci = d.blockSrc[b].container;
    return ci >= 0 && (size_t)ci < d.containers.size() && d.containers[ci].kind == CT_ITEM &&
                   d.containers[ci].firstBlock == (uint32_t)b ? ci : -1;
}
// the innermost list item around b, -1 none
int32_t ItemAround(const Doc& d, int32_t b) {
    for (int32_t ci = d.blockSrc[b].container; ci >= 0 && (size_t)ci < d.containers.size(); ci = d.containers[ci].parent)
        if (d.containers[ci].kind == CT_ITEM) return ci;
    return -1;
}
bool InCont(const Doc& d, int32_t b, int32_t ci) {
    for (int32_t k = ValidBlock(d, b) ? d.blockSrc[b].container : -1; k >= 0 && (size_t)k < d.containers.size();
         k = d.containers[k].parent)
        if (k == ci) return true;
    return false;
}
uint32_t Col(const std::wstring& src, uint32_t s) {
    uint32_t ls = LineStartOf(src, s);
    return ColsOf(src, ls, ls, s);
}
// an ATX heading's closing sequence as written (" ##"); none when only blanks follow its text
std::wstring AtxClose(const std::wstring& src, const BlockSrc& bs) {
    if (!(bs.flags & BS_ATX)) return std::wstring();
    std::wstring s = Strip(Sub(src, bs.end, bs.lineEnd));
    return s.find(L'#') != std::wstring::npos ? s : std::wstring();
}
// is the line starting at l owned by a block (lines nobody owns - reference and unreferenced footnote definitions,
// comments, hidden HTML - are invisible: joins and cuts keep them, §7.7, §7.9)
bool Owned(const Doc& d, uint32_t l) {
    const std::vector<uint32_t>& ord = d.blockOrder;
    auto it = std::upper_bound(ord.begin(), ord.end(), l, [&](uint32_t v, uint32_t b) { return v < d.blockSrc[b].line; });
    return it != ord.begin() && l <= d.blockSrc[*(it - 1)].outerEnd;
}
EditResult SelectAtom(const Doc& d, const EditState& st, int32_t atom);
// the caret alone, onto a block's first or last stop (an object atom: selected)
EditResult Goto(const EditCtx& c, const EditState& st, int32_t b, bool end) {
    if (IsAtomBlock(c.doc, b)) return SelectAtom(c.doc, st, AtomOfBlock(c.doc, b));
    EditResult r = Nothing(st, EK_OTHER);
    uint32_t s = SrcOfText(c.doc, c.src, BlockEdge(c.doc, b, end), MAP_CARET);
    if (s != UINT32_MAX) r.after.focus = r.after.anchor = s;
    r.after.atom = -1;
    return r;
}
// the prefix of the new block a heading, list or quote style gives a phantom (§6.7; the styles are Phase 3a's)
std::wstring StylePrefix(uint8_t style) {
    if (style >= 1 && style <= 6) return std::wstring(style, L'#') + L" ";
    return style == 7 ? L"- " : style == 8 ? L"1. " : style == 9 ? L"- [ ] " : style == 10 ? L"> " : L"";
}
// a phantom next to block b, inside its `depth` outermost containers
EditResult NewPhantom(const EditCtx& c, const EditState& st, PhantomKind k, int32_t b, int depth, uint32_t at, bool in) {
    EditResult r = Nothing(st, EK_OTHER);
    Phantom& ph = r.after.phantom;
    ph = Phantom{};
    ph.kind = k;
    ph.anchorSrc = at;
    ph.anchorBlock = b;
    ph.depth = (uint8_t)std::clamp(depth, 0, 255);
    ph.prefix = PrefixN(c.doc, c.src, b, depth, nullptr);
    ph.blankPrefix = Strip(ph.prefix);
    ph.in = in;
    r.after.atom = -1;
    if (in) r.after.focus = r.after.anchor = at;
    return r;
}
// a new paragraph after block b at its own level, the caret in it (Ctrl+Enter, Enter on a rule, an emptied phantom)
EditResult After(const EditCtx& c, const EditState& st, int32_t b) {
    const Doc& d = c.doc;
    const BlockSrc& bs = d.blockSrc[b];
    uint32_t end = bs.outerEnd;
    if ((bs.flags & BS_RAW) && bs.rawId >= 0)  // an HTML block drawn as several: after all of them
        for (size_t k = b + 1; k < d.blocks.size() && d.blockSrc[k].rawId == bs.rawId; k++) end = std::max(end, d.blockSrc[k].outerEnd);
    return NewPhantom(c, st, PH_AFTER, b, (int)Chain(d, b).size(), end, true);
}

// ---- §7.4: accidental block syntax at the start of a line
bool HtmlStart(const std::wstring& l, size_t i, bool first) {
    // the tags that start an HTML block (CommonMark's kinds 1 and 6)
    static const wchar_t kTags[] = L"|address|article|aside|base|basefont|blockquote|body|caption|center|col|colgroup|dd|"
                                   L"details|dialog|dir|div|dl|dt|fieldset|figcaption|figure|footer|form|frame|frameset|h1|"
                                   L"h2|h3|h4|h5|h6|head|header|hr|html|iframe|legend|li|link|main|menu|menuitem|nav|"
                                   L"noframes|ol|optgroup|option|p|param|pre|script|search|section|style|summary|table|"
                                   L"tbody|td|textarea|tfoot|th|thead|title|tr|track|ul|";
    size_t n = l.size(), k = i + 1;
    if (k >= n) return false;
    if (l[k] == L'!' || l[k] == L'?') return true;  // a comment, a declaration, CDATA, a processing instruction
    if (l[k] == L'/') k++;
    size_t b = k;
    while (k < n && k - b < 16 && iswalnum(l[k])) k++;
    if (k == b || !iswalpha(l[b])) return false;
    std::wstring name = L"|";
    for (size_t j = b; j < k; j++) name += (wchar_t)towlower(l[j]);
    name += L"|";
    bool ends = k >= n || Blank(l[k]) || l[k] == L'>' || (l[k] == L'/' && k + 1 < n && l[k + 1] == L'>');
    if (ends && wcsstr(kTags, name.c_str())) return true;
    if (!first) return false;
    // kind 7: one whole tag alone on its line starts a block too (it cannot interrupt a paragraph)
    wchar_t q = 0;
    for (; k < n; k++) {
        if (q) { if (l[k] == q) q = 0; }
        else if (l[k] == L'"' || l[k] == L'\'') q = l[k];
        else if (l[k] == L'>') break;
    }
    if (k >= n) return false;
    while (++k < n)
        if (!Blank(l[k])) return false;
    return true;
}
// Where a backslash keeps line l (from its content start to its end) from being read as block syntax, -1 none. first:
// the line starts a block (else it goes on a paragraph, where fewer things can interrupt it). *drop: blanks to remove
// instead (four columns of them would make a new block indented code).
int Trigger(const std::wstring& l, bool first, uint32_t* drop) {
    size_t n = l.size(), i = 0;
    *drop = 0;
    if (first) {
        uint32_t cols = 0;
        while (i < n && Blank(l[i])) cols = l[i++] == L'\t' ? (cols + 4) & ~3u : cols + 1;
        if (cols >= 4) {
            *drop = (uint32_t)i;
            return -1;
        }
    }
    if (i >= n) return -1;
    wchar_t ch = l[i];
    auto run = [&](wchar_t x) { size_t k = i; while (k < n && l[k] == x) k++; return k - i; };
    auto blankTo = [&](size_t k) { while (k < n && Blank(l[k])) k++; return k >= n; };  // only blanks from k on
    auto endOrBlank = [&](size_t k) { return k >= n || Blank(l[k]); };
    switch (ch) {
    case L'#': { size_t r = run(L'#'); return r <= 6 && endOrBlank(i + r) ? (int)i : -1; }
    case L'>': return (int)i;
    case L'`': case L'~': return run(ch) >= 3 ? (int)i : -1;
    case L'<': return HtmlStart(l, i, first) ? (int)i : -1;
    case L'[': return first && l.find(L"]:", i + 1) != std::wstring::npos ? (int)i : -1;
    case L'=': return !first && blankTo(i + run(L'=')) ? (int)i : -1;  // a setext underline under a paragraph line
    case L'-': case L'+': case L'*': case L'_': {
        if (ch != L'_' && endOrBlank(i + 1)) return (int)i;  // a bullet
        if (ch == L'+') return -1;
        size_t cnt = 0, k = i;  // a rule: three or more of one character, blanks between
        for (; k < n && (l[k] == ch || Blank(l[k])); k++) cnt += l[k] == ch;
        if (k >= n && cnt >= 3) return (int)i;
        return !first && ch == L'-' && blankTo(i + run(L'-')) ? (int)i : -1;
    }
    }
    if (ch >= L'0' && ch <= L'9') {  // an ordered list marker
        size_t k = i;
        while (k < n && k - i < 9 && l[k] >= L'0' && l[k] <= L'9') k++;
        if (k < n && (l[k] == L'.' || l[k] == L')') && endOrBlank(k + 1)) return (int)k;
    }
    return -1;
}
// §7.4 after an operation: l is the line it changed, as it will be, from its content start (at, in the source after the
// operation's splices); a backslash - or dropping indentation - is appended to r's splices, and the caret moves with it
void Escape(EditResult& r, const std::wstring& l, uint32_t at, bool first) {
    uint32_t drop;
    int k = Trigger(l, first, &drop);
    if (k < 0 && !drop) return;
    Splice sp = drop ? Splice{at, l.substr(0, drop), L""} : Splice{at + (uint32_t)k, L"", L"\\"};
    for (uint32_t* o : {&r.after.focus, &r.after.anchor}) {
        if (*o <= sp.at) continue;
        *o = drop ? std::max(sp.at, *o - std::min(*o - sp.at, drop)) : *o + 1;
    }
    r.splices.push_back(std::move(sp));
}
// the content start of the line text offset t of paragraph b is on - after the line break before it, else the block's
// content start (*first)
uint32_t LineContent(const Doc& d, const std::wstring& src, int32_t b, uint32_t t, bool* first) {
    const Block& bl = d.blocks[b];
    SegSpan ss = SegsIn(d, b, TRange{bl.textOff, bl.textOff + bl.textLen});
    for (const SrcSeg* g = ss.e; g > ss.b;) {
        --g;
        if (g->t + g->tLen <= t && LineBrk(src, g)) {
            *first = false;
            return g->s + g->sLen;
        }
    }
    *first = true;
    return d.blockSrc[b].beg;
}

// ---- §7.5 / §7.6 / §7.9: the delimiters an operation writes back where it cuts spans
struct Delim { uint32_t at; uint8_t type; std::wstring text; };
struct Bal { std::vector<Delim> close, open; bool torn = false; };
// The spans of p's block (or cell) that removing source [sA, sB) cuts: one whose opener stays before the cut and whose
// closer goes (with `reopen`: or stays after it) is closed at the cut - innermost first; one whose opener goes (or,
// reopen, stays before it) and whose closer stays after it is opened again after the cut - outermost first. A link
// closes with its whole closer (`](u)`; a shortcut reference with its label, so both halves still resolve). torn: the
// cut goes through a delimiter, or would split an autolink (`<http://a` + `b>`).
Bal Balance(const EditCtx& c, const TextPos& p, uint32_t sA, uint32_t sB, bool reopen) {
    Bal k;
    const std::wstring& src = c.src;
    TRange rg;
    if (!RangeOf(c.doc, p, &rg)) return k;
    ForSpans(c.doc, p, rg, [&](const SpanSrc& sp) {
        if (sp.flags & SF_UNCLOSED) return;
        auto through = [&](uint32_t x, uint32_t y) { return (x < sA && sA < y) || (x < sB && sB < y); };
        if (through(sp.openBeg, sp.openEnd) || through(sp.closeBeg, sp.closeEnd)) {
            k.torn = true;
            return;
        }
        bool oBefore = sp.openEnd <= sA, oIn = !oBefore && sp.openBeg >= sA && sp.openEnd <= sB;
        bool cAfter = sp.closeBeg >= sB, cIn = !cAfter && sp.closeBeg >= sA && sp.closeEnd <= sB;
        bool surround = oBefore && cAfter && reopen;
        if (surround && (sp.flags & SF_AUTOLINK) && sp.openEnd > sp.openBeg) k.torn = true;
        std::wstring close = Sub(src, sp.closeBeg, sp.closeEnd);
        if ((sp.type == MD_SPAN_A || sp.type == MD_SPAN_IMG) && (sp.flags & SF_REF) && (close == L"]" || close == L"][]"))
            close = L"][" + Sub(src, sp.openEnd, sp.closeBeg) + L"]";
        if ((oBefore && cIn) || surround) k.close.push_back(Delim{sp.openBeg, sp.type, close});
        if ((oIn && cAfter) || surround) k.open.push_back(Delim{sp.openBeg, sp.type, Sub(src, sp.openBeg, sp.openEnd)});
    });
    Order(k.close, [](const Delim& a, const Delim& b) { return a.at > b.at; });
    Order(k.open, [](const Delim& a, const Delim& b) { return a.at < b.at; });
    return k;
}
std::wstring Joined(const std::vector<Delim>& v) {
    std::wstring s;
    for (const Delim& x : v) s += x.text;
    return s;
}
// A closer and an opener of one Markdown span type that would meet in what a cut writes back are both left out: the
// two spans become one (never `****`, §7.5 step 2).
void Cancel(Bal& k) {
    while (!k.close.empty() && !k.open.empty() && k.close.back().type == k.open.front().type &&
           k.close.back().type < 0x80 && k.close.back().text == k.open.front().text) {
        k.close.pop_back();
        k.open.erase(k.open.begin());
    }
}

// ---- §7.7 / §7.9: joining a text block into the one before it
// Text block B's rest - from gapEnd on - becomes part of text block P's last line; the source [gapBeg, gapEnd) goes
// (from P's content end to B's content start, or a selection's cut) and gapText takes its place, the caret at caretIn
// in it. B's line breaks get P's prefix (into an ATX heading: blanks and <br>, F15), B's heading decoration goes, P's
// (an ATX closing sequence, a setext underline) moves after the joined text, and so do the lines between them that no
// block owns (reference definitions, comments, F6). Spans of one kind that meet at the join become one (§7.5).
EditResult JoinBlocks(const EditCtx& c, const EditState& st, int32_t P, int32_t B, uint32_t gapBeg, uint32_t gapEnd,
                      std::wstring gapText, uint32_t caretIn, EditKind kind) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const BlockSrc& ps = d.blockSrc[P];
    const BlockSrc& qs = d.blockSrc[B];
    EditResult r = Nothing(st, kind);
    const bool atx = (ps.flags & BS_ATX) != 0;
    const std::wstring pre = ContPrefix(d, src, P);
    std::wstring tail = atx ? AtxClose(src, ps) : std::wstring(), inv;
    if (ps.flags & BS_SETEXT) tail = Sub(src, ps.lineEnd, ps.outerEnd);
    for (uint32_t l = SkipEol(src, ps.outerEnd); l < qs.line && l < src.size();) {
        uint32_t le = LineEndOf(src, l), nx = SkipEol(src, le);
        if (l >= gapBeg && le <= gapEnd && !BlankLine(src, l, le) && !Owned(d, l)) inv += Sub(src, l, nx);
        l = nx > l ? nx : l + 1;
    }
    while (!inv.empty() && EolChar(inv.back())) inv.pop_back();
    if (!inv.empty()) tail += std::wstring(Eol(c, qs.end)) + Strip(pre) + Eol(c, qs.end) + inv;
    // (a cut that ends past B's text - after its closers - leaves no rest: what follows goes right after the cut)
    uint32_t tailAt = std::max(qs.end, gapEnd);
    uint32_t tailEnd = std::max(tailAt, (qs.flags & BS_SETEXT) ? qs.outerEnd : (qs.flags & BS_ATX) ? qs.lineEnd : qs.end);
    if (!tail.empty() || tailEnd > tailAt) r.splices.push_back(Replace(src, tailAt, tailEnd, tail));
    const std::wstring qpre = ContPrefix(d, src, B);
    const Block& qb = d.blocks[B];
    SegSpan ss = SegsIn(d, B, TRange{qb.textOff, qb.textOff + qb.textLen});
    for (const SrcSeg* g = ss.e; g > ss.b;) {
        --g;
        if (g->s < gapEnd || !LineBrk(src, g)) continue;
        if (atx) {
            r.splices.push_back(Replace(src, g->s, g->s + g->sLen, d.text[g->t] == L'\n' ? L"<br>" : L" "));
        } else if (qpre != pre) {
            uint32_t e = BrkEol(src, *g);
            r.splices.push_back(Replace(src, e, g->s + g->sLen, std::wstring(Eol(c, e)) + pre));
        }
    }
    if (gapText.empty()) {
        const SpanSrc *x = nullptr, *y = nullptr;
        ForSpans(d, TextPos{d.blocks[P].textOff + d.blocks[P].textLen, P, -1}, TRange{d.blocks[P].textOff, d.blocks[P].textOff + d.blocks[P].textLen},
                 [&](const SpanSrc& sp) {
                     if ((sp.flags & SF_ENTERABLE) && !(sp.flags & SF_UNCLOSED) && sp.closeEnd == gapBeg && (!x || sp.closeBeg > x->closeBeg)) x = &sp;
                 });
        ForSpans(d, TextPos{qb.textOff, B, -1}, TRange{qb.textOff, qb.textOff + qb.textLen}, [&](const SpanSrc& sp) {
            if ((sp.flags & SF_ENTERABLE) && !(sp.flags & SF_UNCLOSED) && sp.openBeg == gapEnd && (!y || sp.openEnd < y->openEnd)) y = &sp;
        });
        if (x && y && x->type == y->type && x->type < 0x80 &&
            Sub(src, x->closeBeg, x->closeEnd) == Sub(src, y->openBeg, y->openEnd)) {
            gapBeg = x->closeBeg;
            gapEnd = y->openEnd;
        }
        if (atx && ps.beg == ps.end) gapText = L" ";  // into an empty heading: its marker keeps its blank
    }
    r.splices.push_back(Replace(src, gapBeg, gapEnd, gapText));
    r.after.focus = r.after.anchor = gapBeg + std::min<uint32_t>(caretIn, (uint32_t)gapText.size());
    r.after.atom = -1;
    return r;
}

// The lines [a, e) of a block that goes whole (an object atom, an empty code block, a table), and of the blank lines
// around it exactly those that keep its neighbours apart: a blank line on one side only stays; with blank lines on both
// sides the one after goes; at the document's end the one before goes; with text right above and right below, the lines
// become one blank line (`abc⏎⏎---⏎def` → `abc⏎⏎def`, §7.7). The caret goes where they were.
EditResult DropLines(const EditCtx& c, const EditState& st, int32_t b, uint32_t a, uint32_t e) {
    const std::wstring& src = c.src;
    EditResult r = Nothing(st, EK_OTHER);
    r.after.atom = -1;
    uint32_t pe = a;  // the end of the line before
    if (pe > 0 && src[pe - 1] == L'\n') pe--;
    if (pe > 0 && src[pe - 1] == L'\r') pe--;
    const uint32_t ps = LineStartOf(src, pe), le = LineEndOf(src, e);
    const bool before = a > 0, blankBefore = before && BlankLine(src, ps, pe);
    const bool after = e < src.size(), blankAfter = after && BlankLine(src, e, le);
    std::wstring put;
    if (!after) {
        if (blankBefore) a = ps;  // the end of the document: the blank line before goes too
    } else if (blankAfter) {
        if (!before || blankBefore) e = SkipEol(src, le);  // one blank line after it, unless that one now separates
    } else if (before && !blankBefore) {
        put = BlankPrefix(c.doc, src, b) + LineEol(src, a, c.eol);  // text right above and right below: kept apart
    }
    if (e < a || e > src.size()) return Refuse(st, "atom");
    r.splices.push_back(Replace(src, a, e, put));
    r.after.focus = r.after.anchor = a;
    return r;
}

// A deletion of [s0, s1) (the text [t0, t1) of one block or cell). A span it empties loses its delimiters with it
// (`****`, `[]()`, a code span's backticks, §7.7); a setext heading it empties becomes an empty ATX heading of its
// level, never a lone underline, which is a rule (F17).
EditResult Deletion(const EditCtx& c, const EditState& st, const TextPos& p, TRange rg, uint32_t t0, uint32_t t1,
                    uint32_t s0, uint32_t s1, EditKind kind) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    EditResult r = Nothing(st, kind);
    const BlockSrc& bs = d.blockSrc[p.block];
    if (p.cell < 0 && (bs.flags & BS_SETEXT) && t0 <= rg.beg && t1 >= rg.end && bs.outerEnd > bs.beg) {
        std::wstring hashes(std::max<uint8_t>(1, d.blocks[p.block].heading), L'#');
        r.splices.push_back(Splice{bs.beg, src.substr(bs.beg, bs.outerEnd - bs.beg), hashes});
        r.after.focus = r.after.anchor = bs.beg + (uint32_t)hashes.size();
        return r;
    }
    uint32_t a = s0, b = s1;
    for (bool grew = true; grew;) {
        grew = false;
        ForSpans(d, p, rg, [&](const SpanSrc& sp) {
            if ((sp.flags & SF_UNCLOSED) || sp.tBeg < t0 || sp.tEnd > t1) return;
            if (sp.openEnd >= a && sp.closeBeg <= b && (sp.openBeg < a || sp.closeEnd > b)) {
                a = std::min(a, sp.openBeg);
                b = std::max(b, sp.closeEnd);
                grew = true;
            }
        });
    }
    if (b <= a) return r;
    // blanks the deletion leaves at the start of the content would be stripped there anyway (§7.9): they go with it
    if (t0 <= rg.beg && d.blocks[p.block].kind != BK_CODE) {
        uint32_t start = SrcOfText(d, src, TextPos{rg.beg, p.block, p.cell}, MAP_OUTER_START);
        if (a <= start)
            while (b < src.size() && Blank(src[b])) b++;
    }
    // §7.5: two spans of one kind that now meet become one (`*a* ‸*b*` → `*ab*`); a span whose text now ends (starts)
    // with blanks gets its closer (opener) moved in front of (behind) them (`**bold x‸**` → `**bold** `)
    std::wstring ins;
    uint32_t caret = UINT32_MAX;
    const SpanSrc *x = nullptr, *y = nullptr, *cl = nullptr, *op = nullptr;
    ForSpans(d, p, rg, [&](const SpanSrc& sp) {
        if (!(sp.flags & SF_ENTERABLE) || (sp.flags & SF_UNCLOSED)) return;
        if (sp.closeEnd == a && sp.closeBeg < a && (!x || sp.closeBeg > x->closeBeg)) x = &sp;
        if (sp.openBeg == b && sp.openEnd > b && (!y || sp.openEnd < y->openEnd)) y = &sp;
        if (sp.closeBeg == b && sp.openEnd < a) cl = &sp;
        if (sp.openEnd == a && sp.closeBeg > b) op = &sp;
    });
    if (x && y && x->type == y->type && x->type < 0x80 && Sub(src, x->closeBeg, x->closeEnd) == Sub(src, y->openBeg, y->openEnd)) {
        a = x->closeBeg;
        b = y->openEnd;
    } else if (cl && Blank(src[a - 1])) {
        uint32_t ws = a;
        while (ws > cl->openEnd && Blank(src[ws - 1])) ws--;
        if (ws > cl->openEnd) {
            ins = Sub(src, cl->closeBeg, cl->closeEnd) + Sub(src, ws, a);
            a = ws;
            b = cl->closeEnd;
            caret = a + (uint32_t)ins.size();
        }
    } else if (op && b < src.size() && Blank(src[b])) {
        uint32_t we = b;
        while (we < op->closeBeg && Blank(src[we])) we++;
        if (we < op->closeBeg) {
            ins = Sub(src, b, we) + Sub(src, op->openBeg, op->openEnd);
            b = we;
            a = op->openBeg;
            caret = a + (uint32_t)ins.size();
        }
    }
    r.splices.push_back(Splice{a, src.substr(a, b - a), ins});
    r.after.focus = r.after.anchor = caret != UINT32_MAX ? caret : a;
    // §7.4: the paragraph line the deletion changed must not turn into block syntax
    if (p.cell < 0 && Para(d, p.block)) {
        bool first;
        uint32_t cs = LineContent(d, src, p.block, t0, &first);
        if (cs <= a) Escape(r, Sub(src, cs, a) + ins + Sub(src, b, LineEndOf(src, b)), cs, first);
    }
    return r;
}

// The selection's cut inside one block or cell (§7.9, the part Phase 2a needs). It runs from before every opener at its
// start to after every closer at its end (MAP_OUTER_START / MAP_OUTER_END), so a span whose whole text is selected goes
// with both of its delimiters; a span it cuts in two gets the delimiter it lost written back at the cut - the closer of
// one that began before it, the opener of one that goes on after it - so formatting never leaks. For typing over the
// selection, `wrapOpen` / `wrapClose` are the delimiters of the enterable spans that held its first character and go
// with it: the typed text takes their place (`**⟦bold⟧**` + `x` → `**x**`). Several blocks are Phase 2b's.
struct SelCut {
    bool ok = false;
    uint32_t a = 0, b = 0;
    std::wstring closers, openers, wrapOpen, wrapClose, openersRest;  // rest: the openers not in wrapOpen
    TextPos A, B;
    TRange rg{};
};
SelCut CutSelection(const EditCtx& c, const EditState& st) {
    SelCut cut;
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr), an = AnchorOf(c, st);
    if (f.block != an.block || f.cell != an.cell || !ValidBlock(d, f.block)) return cut;
    const BlockSrc& bs = d.blockSrc[f.block];
    if (AtomBlock(bs) || (bs.flags & BS_SYNTH)) return cut;
    cut.A = f.t <= an.t ? f : an;
    cut.B = f.t <= an.t ? an : f;
    if (!RangeOf(d, cut.A, &cut.rg)) return cut;
    cut.a = SrcOfText(d, src, cut.A, MAP_OUTER_START);
    cut.b = SrcOfText(d, src, cut.B, MAP_OUTER_END);
    if (cut.a == UINT32_MAX || cut.b == UINT32_MAX || cut.a > cut.b || cut.b > src.size()) return cut;
    std::vector<std::pair<uint32_t, std::wstring>> closers, openers, wrapOpen, wrapClose, rest;
    bool torn = false;
    ForSpans(d, cut.A, cut.rg, [&](const SpanSrc& sp) {
        bool unclosed = (sp.flags & SF_UNCLOSED) != 0;
        auto straddles = [&](uint32_t x, uint32_t y) { return (x < cut.a && y > cut.a) || (x < cut.b && y > cut.b); };
        if (straddles(sp.openBeg, sp.openEnd) || (!unclosed && straddles(sp.closeBeg, sp.closeEnd))) torn = true;
        bool oIn = sp.openEnd > sp.openBeg && sp.openBeg >= cut.a && sp.openEnd <= cut.b;
        bool cIn = !unclosed && sp.closeEnd > sp.closeBeg && sp.closeBeg >= cut.a && sp.closeEnd <= cut.b;
        if (!oIn && !cIn) return;
        std::wstring o = src.substr(sp.openBeg, sp.openEnd - sp.openBeg);
        std::wstring k = cIn ? src.substr(sp.closeBeg, sp.closeEnd - sp.closeBeg) : std::wstring();
        bool holds = (sp.flags & SF_ENTERABLE) && sp.tBeg <= cut.A.t && cut.A.t < sp.tEnd;  // the first character
        if (holds && oIn) wrapOpen.push_back({sp.openBeg, o});
        if (holds && oIn && cIn) wrapClose.push_back({sp.closeBeg, k});
        if (oIn && !cIn) openers.push_back({sp.openBeg, o});
        if (oIn && !cIn && !holds) rest.push_back({sp.openBeg, o});
        if (cIn && !oIn) closers.push_back({sp.closeBeg, k});
    });
    if (torn) return cut;
    auto join = [](std::vector<std::pair<uint32_t, std::wstring>>& v, std::wstring& out) {
        Order(v, [](const auto& x, const auto& y) { return x.first < y.first; });
        for (auto& k : v) out += k.second;
    };
    join(closers, cut.closers);
    join(openers, cut.openers);
    join(wrapOpen, cut.wrapOpen);
    join(wrapClose, cut.wrapClose);
    join(rest, cut.openersRest);
    cut.ok = true;
    return cut;
}
}  // namespace

bool NeedsTypeCheck(std::wstring_view text) {
    for (wchar_t ch : text)
        if (Significant(ch)) return false;
    return !text.empty();
}

bool TypedOk(const Doc& a, uint32_t t, const Doc& b, std::wstring_view rendered) {
    size_t n = rendered.size();
    if (t > a.text.size() || b.text.size() != a.text.size() + n) return false;
    if (a.text.compare(0, t, b.text, 0, t) != 0 || b.text.compare(t, n, rendered.data(), n) != 0 ||
        a.text.compare(t, std::wstring::npos, b.text, t + n, std::wstring::npos) != 0)
        return false;
    if (t > 0 && FlagsAt(a, t - 1) != FlagsAt(b, t - 1)) return false;
    if (t < a.text.size() && FlagsAt(a, t) != FlagsAt(b, (uint32_t)(t + n))) return false;
    return true;
}

std::vector<TypeCandidate> TypeFallbacks(const EditCtx& c, const EditState& st, uint32_t s, std::wstring_view text) {
    std::vector<TypeCandidate> out;
    uint16_t tr = 0;
    TextPos p = FocusOf(c, st, &tr);
    TRange rg;
    if (!ValidBlock(c.doc, p.block) || !RangeOf(c.doc, p, &rg)) return out;
    const std::wstring t(text);
    const uint32_t n = (uint32_t)t.size();
    // past the closers that begin at s, one after the other (`**API‸**s` + `.` → `**API**.s`) ...
    for (uint32_t at = s, more = 1; more;) {
        more = 0;
        ForSpans(c.doc, p, rg, [&](const SpanSrc& sp) {
            if (!more && !(sp.flags & SF_UNCLOSED) && sp.closeBeg == at && sp.closeEnd > at) { at = sp.closeEnd; more = 1; }
        });
        if (more) out.push_back(TypeCandidate{at, t, at + n, t});
    }
    // ... before the openers that end at s
    for (uint32_t at = s, more = 1; more;) {
        more = 0;
        ForSpans(c.doc, p, rg, [&](const SpanSrc& sp) {
            if (!more && sp.openEnd == at && sp.openBeg < at) { at = sp.openBeg; more = 1; }
        });
        if (more) out.push_back(TypeCandidate{at, t, at + n, t});
    }
    // beside a formula's dollar: a separating blank (`the $E$‸ is` + `x` → `the $E$ x is`, F9-4)
    if (s > 0 && c.src[s - 1] == L'$') out.push_back(TypeCandidate{s, L" " + t, s + 1 + n, L" " + t});
    if (s < c.src.size() && c.src[s] == L'$') out.push_back(TypeCandidate{s, t + L" ", s + n, t + L" "});
    return out;
}

namespace {
EditResult CutRange(const EditCtx& c, const EditState& st, std::wstring ins, EditKind kind);

// A phantom the caret is not in stays next to its block through an operation's splices, as the caret does (§6.7): one
// after a block moves with text typed at its end, one before it does not.
EditResult Carry(EditResult r) {
    Phantom& ph = r.after.phantom;
    if (ph.kind != PH_NONE && !ph.in && r.refused.empty()) ph.anchorSrc = MapThrough(r.splices, ph.anchorSrc, ph.kind != PH_BEFORE);
    return r;
}

// The first text typed or pasted into a phantom makes it real (§6.7): one splice at its anchor.
EditResult Materialise(const EditCtx& c, const EditState& st, std::wstring_view text, bool typing, EditKind kind) {
    const Phantom& ph = st.phantom;
    const std::wstring& src = c.src;
    EditResult r = Nothing(st, kind);
    if (typing && std::all_of(text.begin(), text.end(), Blank)) return r;  // a blank at a new block's start (§6.5)
    int32_t ab = PhantomBlock(c.doc, src, ph);
    if (ab < 0) return Refuse(st, "nowhere");
    uint32_t at = std::min<uint32_t>(ph.anchorSrc, (uint32_t)src.size()), caret;
    const std::wstring E = Eol(c, at), t = StylePrefix(ph.style) + std::wstring(text);
    std::wstring ins;
    if (ph.kind == PH_AFTER) {
        ins = E + ph.blankPrefix + E + ph.prefix + t;
        caret = at + (uint32_t)ins.size();
        uint32_t nx = SkipEol(src, at);  // blank lines on both sides of the new block
        if (nx > at && nx < src.size() && !BlankLine(src, nx, LineEndOf(src, nx))) ins += E + ph.blankPrefix;
    } else if (ph.kind == PH_BEFORE) {
        ins = t;
        caret = at + (uint32_t)ins.size();
        ins += E + ph.blankPrefix + E + ph.prefix;
    } else {  // a pending hard break at the block's end (an ATX heading can only hold a <br>)
        ins = (c.doc.blockSrc[ab].flags & BS_ATX) ? std::wstring(L"<br>") : L"\\" + E + ph.prefix;
        ins += text;
        caret = at + (uint32_t)ins.size();
    }
    r.splices.push_back(Splice{at, L"", ins});
    r.after.focus = r.after.anchor = caret;
    r.after.phantom = Phantom{};
    // the line a hard break starts goes on the paragraph: what is typed there is checked as on any such line (§7.4)
    if (ph.kind == PH_BREAK && typing && Para(c.doc, ab) && !(c.doc.blockSrc[ab].flags & BS_ATX))
        Escape(r, std::wstring(text) + Sub(src, at, LineEndOf(src, at)), caret - (uint32_t)text.size(), false);
    return r;
}

// Text put in at the caret: typed (§7.3, with its context transforms) or pasted (as it is: OpPaste made its lines fit
// the block, §7.11). Over a selection it replaces it (§7.9).
EditResult Insert(const EditCtx& c, const EditState& st, std::wstring_view text, bool typing, EditKind kind) {
    EditResult r = Nothing(st, kind);
    if (text.empty()) return r;
    if (st.atom >= 0) return Refuse(st, "atom");  // typing never goes into the text next to a selected atom (§2.10)
    if (InPh(st)) return Materialise(c, st, text, typing, kind);
    if (st.anchor != st.focus) return CutRange(c, st, std::wstring(text), kind);
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const bool blank = typing && std::all_of(text.begin(), text.end(), Blank);
    uint16_t trail = 0;
    TextPos p = FocusOf(c, st, &trail);
    if (!ValidBlock(d, p.block)) {
        // Nothing left to stand in (the last character of the only block went): the text starts the document again,
        // after whatever invisible lines remain (a reference definition, a comment), as a paragraph of its own.
        if (!d.hasMap || !d.blocks.empty()) return Refuse(st, "nowhere");
        if (blank) return r;
        uint32_t end = (uint32_t)src.size();
        while (end > 0 && (Blank(src[end - 1]) || EolChar(src[end - 1]))) end--;
        std::wstring ins = end ? std::wstring(LineEol(src, end, c.eol)) + LineEol(src, end, c.eol) + std::wstring(text)
                               : std::wstring(text);
        r.splices.push_back(Splice{end, L"", ins});
        r.after.focus = r.after.anchor = end + (uint32_t)ins.size();
        return r;
    }
    const BlockSrc& bs = d.blockSrc[p.block];
    if (bs.flags & (BS_SYNTH | BS_OBJECT | BS_RAW)) return Refuse(st, "atom");
    TRange rg;
    if (!RangeOf(d, p, &rg)) return Refuse(st, "nowhere");
    const Table* tb = TableOf(d, p.block);
    const bool missing = tb && tb->cellOff + (uint32_t)p.cell < d.cellSrc.size() && d.cellSrc[tb->cellOff + p.cell].missing;
    const bool code = d.blocks[p.block].kind == BK_CODE;
    uint32_t s = trail ? st.focus : SrcOfText(d, src, p, MAP_CARET);
    if (s == UINT32_MAX || s > src.size()) return Refuse(st, "nowhere");
    if (!code && blank) {
        SegSpan ss = SegsIn(d, p.block, rg);
        const SrcSeg* R = SegStartingAt(ss, p.t);
        if (!trail) {
            // A blank where it would be stripped (or four of them would make indented code) is not written (§6.5); at
            // a soft break the break already shows as that blank: at its left edge the caret goes over it (§6.6).
            if (p.t == rg.beg) return r;
            if (SoftBreak(d, src, SegEndingAt(ss, p.t))) return r;
            // At the end of a span the blank goes after its closers: `**bold **` is no bold at all, and the next word
            // after the blank is plain in Markdown anyway (§7.3's sticky end; the merge that makes it bold is 3a's).
            s = SrcOfText(d, src, p, MAP_OUTER_END);
        }
        // A caret in the blanks a soft break starts with (§6.5): one more there would make two blanks before the line
        // end, a hard break. It goes over the break instead, as from its left edge (§6.6).
        if (SoftBreak(d, src, R) && s >= R->s && s < R->s + R->sLen) {
            r.after.focus = r.after.anchor = R->s + R->sLen;
            r.kind = EK_OTHER;
            return r;
        }
    }
    std::wstring ins(text), pre, post;
    // one there would be a hard break (§6.6) - or, typed right before a backslash break, turn it into an escape
    if (typing && !code && text == L"\\" && (LineEndAt(src, s) || (src[s] == L'\\' && LineEndAt(src, s + 1)))) ins = L"\\\\";
    if (tb) {  // a pipe would end the cell
        std::wstring e;
        for (wchar_t ch : ins) {
            if (ch == L'|') e += L'\\';
            e += ch;
        }
        ins = e;
    }
    if (missing) {
        // A cell the row lacks (§7.10): the row is completed first - its trailing pipe, then an empty cell for every
        // missing one before this - and the text goes into a cell of its own: `| x |` + y in the second → `| x | y |`.
        const TableSrc& ts = d.tableSrc[d.blocks[p.block].aux];
        uint32_t row = (uint32_t)p.cell / tb->cols, col = (uint32_t)p.cell % tb->cols, k = col;
        const RowSrc& rs = ts.rows[row ? row + 1 : 0];
        uint32_t e = rs.lineEnd;
        while (e > rs.contentStart && Blank(src[e - 1])) e--;
        std::wstring fill = e == rs.contentStart || src[e - 1] != L'|' || (e >= 2 && src[e - 2] == L'\\') ? L" |" : L"";
        while (k > 0 && d.cellSrc[tb->cellOff + row * tb->cols + k - 1].missing) k--;
        for (; k < col; k++) fill += L"  |";
        fill += L" ";
        r.splices.push_back(Splice{e, L"", fill + ins + L" |"});
        r.after.focus = r.after.anchor = e + (uint32_t)(fill.size() + ins.size());
        return r;
    }
    if (rg.beg == rg.end) {  // the first character of an empty heading, item, footnote, cell or fence (§6.3.1)
        if (!tb && (bs.flags & (BS_ATX | BS_EMPTYITEM)) && (s == 0 || !Blank(src[s - 1]))) pre = L" ";
        if (!tb && (bs.flags & BS_ATX) && s < src.size() && !Blank(src[s]) && !EolChar(src[s])) post = L" ";
        if (!tb && (bs.flags & BS_NOCONTENT)) pre = std::wstring(LineEol(src, s, c.eol)) + ContPrefix(d, src, p.block);
        if (tb && s < src.size() && src[s] == L'|') post = L" ";
    }
    r.splices.push_back(Splice{s, L"", pre + ins + post});
    r.after.focus = r.after.anchor = s + (uint32_t)(pre.size() + ins.size());
    // §7.4: a line that goes on a paragraph must not turn into block syntax as it is typed (`The value⏎- is high`);
    // the first line of a block is where Markdown typed at the visible start is taken as such (Principle 4)
    if (typing && !tb && Para(d, p.block)) {
        bool first;
        uint32_t cs = LineContent(d, src, p.block, p.t, &first);
        if (!first && cs <= s) Escape(r, Sub(src, cs, s) + pre + ins + post + Sub(src, s, LineEndOf(src, s)), cs, false);
    }
    return r;
}
}  // namespace

EditResult OpType(const EditCtx& c, const EditState& st, std::wstring_view text) {
    return Carry(Insert(c, st, text, true, EK_TYPE));
}

// ------------------------------------------------------------------------------------------------ deleting (§7.7)
namespace {
// where a picture in a line stands in the text: its block, its cell, its offset
bool ImagePos(const Doc& d, int32_t image, TextPos* p) {
    int32_t b = AtomBlockOf(d, image);
    if (b < 0) return false;
    auto find = [&](uint32_t off, uint32_t n, int32_t cell) {
        for (uint32_t k = 0; k < n && off + k < d.runs.size(); k++)
            if ((d.runs[off + k].flags & F_IMAGE) && d.runs[off + k].image == (uint32_t)image) {
                *p = TextPos{d.runs[off + k].start, b, cell};
                return true;
            }
        return false;
    };
    const Block& bl = d.blocks[b];
    if (find(bl.runOff, bl.runCount, -1)) return true;
    if (const Table* tb = TableOf(d, b))
        for (uint32_t k = 0; k < tb->rows * tb->cols; k++)
            if (find(d.cells[tb->cellOff + k].runOff, d.cells[tb->cellOff + k].runCount, (int32_t)k)) return true;
    return false;
}
// The first Backspace or Delete next to an object atom selects it (§7.7). The caret stands on the atom - at the start of
// its source - so the arrows and Esc go on from there, not from where the key was pressed.
EditResult SelectAtom(const Doc& d, const EditState& st, int32_t atom) {
    EditResult r = Nothing(st, EK_OTHER);
    if (atom < 0 || (!(atom & kAtomBlock) && (size_t)atom >= d.images.size())) return r;
    r.after.atom = atom;
    r.after.focus = r.after.anchor = atom & kAtomBlock ? d.blockSrc[atom & ~kAtomBlock].line : d.images[atom].outerBeg;
    return r;
}
bool TagAt(const std::wstring& src, uint32_t k, const wchar_t* tag) {
    size_t n = wcslen(tag);
    return k + n <= src.size() && !_wcsnicmp(src.c_str() + k, tag, n);
}

// The second Backspace or Delete on a selected object atom (§2.10). One in a line goes with its whole source (`![…](…)`,
// `$…$`, `<img …>`) and with the spans it was all of (`**![i](a.png)**`, a linked badge: §7.7's emptied spans). A block
// goes with its lines - a <details> summary with its whole group up to the closing tag - and of the blank lines around
// it exactly what keeps its neighbours apart: `abc⏎⏎---⏎def` loses the rule, and stays two paragraphs.
EditResult DeleteAtom(const EditCtx& c, const EditState& st) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    if (!(st.atom & kAtomBlock)) {
        TextPos p{0, -1, -1};
        TRange rg;
        if ((size_t)st.atom >= d.images.size() || !ImagePos(d, st.atom, &p) || !RangeOf(d, p, &rg)) return Refuse(st, "atom");
        const Image& im = d.images[st.atom];
        if (im.outerBeg >= im.outerEnd || im.outerEnd > src.size()) return Refuse(st, "atom");
        EditResult del = Deletion(c, st, p, rg, p.t, p.t + 1, im.outerBeg, im.outerEnd, EK_OTHER);
        del.after.atom = -1;
        return del;
    }
    EditResult r = Nothing(st, EK_OTHER);
    r.after.atom = -1;
    int32_t b = st.atom & ~kAtomBlock;
    if (!ValidBlock(d, b)) return r;
    const BlockSrc& bs = d.blockSrc[b];
    uint32_t a = bs.line, end = bs.outerEnd;
    for (size_t k = b + 1; k < d.blocks.size() && (d.blockSrc[k].flags & BS_RAW) && d.blockSrc[k].rawId == b; k++)
        end = std::max(end, d.blockSrc[k].outerEnd);  // an HTML block drawn as several
    uint32_t e = SkipEol(src, end);
    if (uint16_t det = d.blocks[b].details; det & 0x8000) {
        // a <summary>: its <details> goes whole - the blocks of the group, then the line with the closing tag
        bool closed = false;
        for (uint32_t k = a; k < e && !closed; k++) closed = TagAt(src, k, L"</details");
        if (!closed) {
            for (size_t k = b + 1; k < d.blocks.size() && (d.blocks[k].details & 0x7FFF) == (det & 0x7FFF); k++)
                e = std::max(e, SkipEol(src, d.blockSrc[k].outerEnd));
            for (uint32_t k = a; k < e && !closed; k++) closed = TagAt(src, k, L"</details");
            while (!closed && e < src.size() && BlankLine(src, e, LineEndOf(src, e))) e = SkipEol(src, LineEndOf(src, e));
            uint32_t k = e;
            while (k < src.size() && (Blank(src[k]) || src[k] == L'>')) k++;
            if (!closed && !TagAt(src, k, L"</details")) return Refuse(st, "atom");  // nothing sure to take: nothing
            if (!closed) e = SkipEol(src, LineEndOf(src, e));
        }
    }
    return DropLines(c, st, b, a, e);
}

// ---- lists (§7.6-§7.8)
uint32_t BackEol(const std::wstring& src, uint32_t x) {  // from a line's start back over the line end before it
    if (x > 0 && src[x - 1] == L'\n') x--;
    if (x > 0 && src[x - 1] == L'\r') x--;
    return x;
}
// the marker a new item of item's list gets: its own kind and spacing, the next number, an unticked box (F16)
std::wstring NewMarker(const Doc& d, const std::wstring& src, int32_t item, bool next) {
    const ContainerSrc& it = d.containers[item];
    std::wstring m = it.delim ? std::to_wstring(it.number + (next ? 1 : 0)) + it.delim : std::wstring(1, it.bullet ? it.bullet : L'-');
    uint32_t p = it.markOff + it.markLen, n = (uint32_t)src.size();
    std::wstring gap;
    while (p < n && Blank(src[p]) && gap.size() < 5) gap += src[p++];
    if (gap.empty() || gap.size() > 4) gap = L" ";  // none (an empty item) or five and more count as one
    m += gap;
    if (it.taskOff != UINT32_MAX) m += L"[ ] ";
    return m;
}
// the item in block b's containers that is item's sibling: the same parent, the same kind of list (-1 none)
int32_t SiblingOf(const Doc& d, int32_t b, int32_t item) {
    const ContainerSrc& it = d.containers[item];
    for (int32_t k = ValidBlock(d, b) ? d.blockSrc[b].container : -1; k >= 0 && (size_t)k < d.containers.size();
         k = d.containers[k].parent) {
        const ContainerSrc& x = d.containers[k];
        if (k != item && x.kind == CT_ITEM && x.parent == it.parent && x.bullet == it.bullet && x.delim == it.delim) return k;
    }
    return -1;
}
// the last block of an item and all in it
int32_t LastOf(const Doc& d, int32_t item) {
    const ContainerSrc& it = d.containers[item];
    int32_t b = it.lastBlock >= it.firstBlock ? (int32_t)it.lastBlock : (int32_t)it.firstBlock;
    while (b > (int32_t)it.firstBlock && (d.blockSrc[b].flags & BS_SYNTH)) b--;
    return b;
}
// every non-blank line of [a, e] shifted by delta columns at column col: blanks put in there, or taken out from there
void Shift(const std::wstring& src, uint32_t a, uint32_t e, uint32_t col, int delta, std::vector<Splice>& out) {
    for (uint32_t l = a; l <= e && l < src.size();) {
        uint32_t le = LineEndOf(src, l);
        if (!BlankLine(src, l, le)) {
            uint32_t p = l, c = 0;
            while (p < le && c < col && (Blank(src[p]) || src[p] == L'>')) c = src[p++] == L'\t' ? (c + 4) & ~3u : c + 1;
            if (delta > 0) {
                out.push_back(Splice{p, L"", std::wstring((size_t)delta, L' ')});
            } else {
                uint32_t q = p, cc = c;
                while (q < le && Blank(src[q]) && cc < c + (uint32_t)-delta) cc = src[q++] == L'\t' ? (cc + 4) & ~3u : cc + 1;
                if (q > p) out.push_back(Replace(src, p, q, L""));
            }
        }
        uint32_t nx = SkipEol(src, le);
        if (nx <= le) break;
        l = nx;
    }
}
void SortDown(std::vector<Splice>& v) {  // back to front, so each splice's offset holds; at one offset the removal first
    Order(v, [](const Splice& a, const Splice& b) {
        return a.at > b.at || (a.at == b.at && a.removed.size() > b.removed.size());
    });
}
// Sorted splices that touch - one ending where the one applied before it begins - become one: applied one at a time,
// the first could bring a lone CR and a lone LF together for a moment, a CRLF the second then cuts (found by the edit
// walk of the fuzzer)
void Coalesce(std::vector<Splice>& v) {
    for (size_t k = 1; k < v.size();) {
        Splice& lo = v[k];
        if (lo.at + lo.removed.size() != v[k - 1].at) {
            k++;
            continue;
        }
        lo.removed += v[k - 1].removed;
        lo.inserted += v[k - 1].inserted;
        v.erase(v.begin() + (ptrdiff_t)(k - 1));
    }
}
// Tab on an item: nested under the item before it in its list (F16): its lines and its children's gain that item's
// content column; the first item of a new nested ordered list is numbered 1
bool NestItem(const EditCtx& c, int32_t item, std::vector<Splice>& out) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const ContainerSrc& it = d.containers[item];
    int32_t P = PrevInSource(d, (int32_t)it.firstBlock), sib = P >= 0 ? SiblingOf(d, P, item) : -1;
    if (sib < 0) return false;
    uint32_t col = Col(src, it.markOff);
    int delta = (int)d.containers[sib].contentCol - (int)col;
    if (delta <= 0) return false;
    Shift(src, LineStartOf(src, it.markOff), d.blockSrc[LastOf(d, item)].outerEnd, col, delta, out);
    bool kids = false;
    for (const ContainerSrc& k : d.containers) kids |= k.kind == CT_ITEM && k.parent == sib;
    if (it.delim && !kids && it.number != 1) out.push_back(Replace(src, it.markOff, it.markOff + it.markLen - 1, L"1"));
    return true;
}
// Shift+Tab on a nested item: one level out; the items after it in its list become its children, numbered from 1
bool OutdentItem(const EditCtx& c, int32_t item, std::vector<Splice>& out) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const ContainerSrc& it = d.containers[item];
    if (it.parent < 0 || d.containers[it.parent].kind != CT_ITEM) return false;
    uint32_t col = Col(src, it.markOff), pcol = Col(src, d.containers[it.parent].markOff);
    int delta = (int)col - (int)pcol;
    if (delta <= 0) return false;
    Shift(src, LineStartOf(src, it.markOff), d.blockSrc[LastOf(d, item)].outerEnd, pcol, -delta, out);
    uint32_t content = it.contentCol - (uint32_t)delta, num = 1;
    for (int32_t cur = item;;) {
        int32_t nb = NextInSource(d, LastOf(d, cur)), sib = nb >= 0 ? ItemOf(d, nb) : -1;
        if (sib < 0 || SiblingOf(d, nb, item) != sib) break;
        const ContainerSrc& s2 = d.containers[sib];
        uint32_t sc = Col(src, s2.markOff);
        int d2 = (int)content - (int)sc;
        if (d2) Shift(src, LineStartOf(src, s2.markOff), d.blockSrc[LastOf(d, sib)].outerEnd, d2 > 0 ? sc : content, d2, out);
        std::wstring nn = std::to_wstring(num++);
        if (s2.delim && Sub(src, s2.markOff, s2.markOff + s2.markLen - 1) != nn)
            out.push_back(Replace(src, s2.markOff, s2.markOff + s2.markLen - 1, nn));
        cur = sib;
    }
    return true;
}
// An item that becomes a paragraph (Backspace at the first item of a top-level list, Shift+Tab at the top level, F16):
// its marker and box go, and blank lines part it from the items left before and after it (`a⏎2. b` would be one
// paragraph); §7.4 then keeps its text a paragraph (`1\. Intro`).
EditResult Unlist(const EditCtx& c, const EditState& st, int32_t item) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const ContainerSrc& it = d.containers[item];
    const int32_t b = (int32_t)it.firstBlock;
    const BlockSrc& bs = d.blockSrc[b];
    const std::wstring E = Eol(c, it.markOff), blank = Strip(PrefixN(d, src, b, (int)Chain(d, b).size() - 1, nullptr));
    EditResult r = Nothing(st, EK_STRUCT);
    int32_t last = LastOf(d, item), nx = NextInSource(d, last), pv = PrevInSource(d, b);
    if (nx >= 0 && SiblingOf(d, nx, item) >= 0) r.splices.push_back(Splice{d.blockSrc[last].outerEnd, L"", E + blank});
    r.splices.push_back(Replace(src, it.markOff, bs.beg, L""));
    if (pv >= 0 && SiblingOf(d, pv, item) >= 0) r.splices.push_back(Splice{LineStartOf(src, it.markOff), L"", blank + E});
    r.after.focus = MapThrough(r.splices, st.focus, true);
    r.after.anchor = MapThrough(r.splices, st.anchor, true);
    r.after.atom = -1;
    if (Para(d, b)) Escape(r, Sub(src, bs.beg, LineEndOf(src, bs.beg)), MapThrough(r.splices, bs.beg, true), true);
    return r;
}

// ---- keys in an empty phantom (§6.7)
// out of the innermost container it is in: after all of it (before all of it)
EditResult PhPop(const EditCtx& c, const EditState& st) {
    const Doc& d = c.doc;
    const Phantom& ph = st.phantom;
    int32_t ab = PhantomBlock(d, c.src, ph);
    if (ab < 0 || !ph.depth) return Nothing(st, EK_OTHER);
    auto chain = Chain(d, ab);
    if (ph.depth > chain.size()) return Nothing(st, EK_OTHER);
    int nd = ph.depth - 1;
    const ContainerSrc* leave = chain[nd];
    if (ph.kind == PH_BEFORE) {
        int32_t fb = (int32_t)leave->firstBlock;
        while (fb < (int32_t)leave->lastBlock && (d.blockSrc[fb].flags & BS_SYNTH)) fb++;
        uint32_t at;
        PrefixN(d, c.src, fb, nd, &at);
        return NewPhantom(c, st, PH_BEFORE, fb, nd, at, true);
    }
    int32_t lb = (int32_t)std::max(leave->lastBlock, leave->firstBlock);
    return NewPhantom(c, st, PH_AFTER, lb, nd, d.blockSrc[lb].outerEnd, true);
}
EditResult PhEnter(const EditCtx& c, const EditState& st, int variant) {
    const Phantom& ph = st.phantom;
    int32_t ab = PhantomBlock(c.doc, c.src, ph);
    if (variant || ab < 0) return Nothing(st, EK_OTHER);
    if (ph.kind == PH_BREAK) return After(c, st, ab);  // a line break waiting at a block's end: a new paragraph instead
    if (ph.depth) return PhPop(c, st);
    EditResult r = Nothing(st, EK_OTHER);
    r.after.phantom.style = 0;  // a style is cleared; otherwise nothing
    return r;
}
EditResult PhBack(const EditCtx& c, const EditState& st) {
    const Phantom& ph = st.phantom;
    if (ph.depth && ph.kind != PH_BREAK) return PhPop(c, st);
    int32_t ab = PhantomBlock(c.doc, c.src, ph);
    EditResult r = ab >= 0 ? Goto(c, st, ab, ph.kind != PH_BEFORE) : Nothing(st, EK_OTHER);
    r.after.phantom = Phantom{};
    return r;
}
EditResult PhDel(const EditCtx& c, const EditState& st) {
    const Phantom& ph = st.phantom;
    int32_t ab = PhantomBlock(c.doc, c.src, ph), nx = ab >= 0 ? NextInSource(c.doc, ab) : -1;
    EditResult r = ab < 0 ? Nothing(st, EK_OTHER) : ph.kind == PH_BEFORE ? Goto(c, st, ab, false)
                          : nx >= 0 ? Goto(c, st, nx, false) : Goto(c, st, ab, true);
    r.after.phantom = Phantom{};
    return r;
}

// ---- Backspace at the start of a block, Delete at its end (§7.7)
EditResult BackAtStart(const EditCtx& c, const EditState& st, const TextPos& p) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const int32_t b = p.block;
    const BlockSrc& bs = d.blockSrc[b];
    const Block& bl = d.blocks[b];
    const int32_t P = PrevInSource(d, b);
    EditResult r = Nothing(st, EK_STRUCT);
    r.after.atom = -1;
    if (p.cell >= 0) {  // a cell: the caret to the end of the cell before, or of the block before the table
        if (!p.cell) return P >= 0 ? Goto(c, st, P, true) : Nothing(st, EK_OTHER);
        const Cell& pc = d.cells[TableOf(d, b)->cellOff + p.cell - 1];
        r.kind = EK_OTHER;
        r.after.focus = r.after.anchor = SrcOfText(d, src, TextPos{pc.textOff + pc.textLen, b, p.cell - 1}, MAP_CARET);
        return r;
    }
    if (bs.flags & BS_FOOTNOTE) return Nothing(st, EK_OTHER);  // a definition is one paragraph of its own (F5)
    if (bs.flags & BS_ATX) {
        if (!bl.textLen) {  // an empty heading: its line and one blank line before it go, the caret to the end before
            uint32_t a = bs.line, e = bs.outerEnd;
            if (a > 0) {
                a = BackEol(src, a);
                uint32_t ls = LineStartOf(src, a);
                if (ls > 0 && BlankLine(src, ls, a)) a = BackEol(src, ls);
            } else {
                e = SkipEol(src, e);
            }
            r.splices.push_back(Replace(src, a, e, L""));
            r.after.focus = r.after.anchor = a;
            return r;
        }
        uint32_t h = bs.beg;  // its # run and the blanks after it go, and a closing sequence: a paragraph (§7.4 after)
        while (h > bs.line && Blank(src[h - 1])) h--;
        while (h > bs.line && src[h - 1] == L'#') h--;
        if (!AtxClose(src, bs).empty()) r.splices.push_back(Replace(src, bs.end, bs.lineEnd, L""));
        r.splices.push_back(Replace(src, h, bs.beg, L""));
        r.after.focus = r.after.anchor = h;
        Escape(r, Sub(src, bs.beg, bs.end), h, true);
        return r;
    }
    if (bs.flags & BS_SETEXT) {  // its underline goes: a paragraph
        r.splices.push_back(Replace(src, bs.lineEnd, bs.outerEnd, L""));
        return r;
    }
    if (int32_t item = ItemOf(d, b); item >= 0) {
        const ContainerSrc& it = d.containers[item];
        if (bs.flags & BS_EMPTYITEM) {  // an empty item: its line goes, the caret to the end of the line before
            uint32_t a = bs.line, e = bs.lineEnd;
            if (a > 0) a = BackEol(src, a);
            else e = SkipEol(src, e);
            r.splices.push_back(Replace(src, a, e, L""));
            r.after.focus = r.after.anchor = a;
            return r;
        }
        // into the item before it in its list, or the item it is nested in: a hard break there when the list is tight,
        // a paragraph of that item when it is loose (UX-21, F16)
        int32_t par = it.parent >= 0 && d.containers[it.parent].kind == CT_ITEM ? it.parent : -1;
        if (P >= 0 && (SiblingOf(d, P, item) >= 0 || (par >= 0 && InCont(d, P, par)))) {
            if (!TextBlk(d, P)) return Goto(c, st, P, true);
            const BlockSrc& ps = d.blockSrc[P];
            if (it.tight) r.splices.push_back(Replace(src, ps.end, bs.beg, L"\\" + std::wstring(Eol(c, ps.end)) + ContPrefix(d, src, P)));
            else r.splices.push_back(Replace(src, bs.line, bs.beg, ContPrefix(d, src, P)));
            r.after.focus = r.after.anchor = r.splices.back().at + (uint32_t)r.splices.back().inserted.size();
            return r;
        }
        return Unlist(c, st, item);  // the first item of a list: a paragraph
    }
    const int32_t ci = bs.container;
    const ContainerSrc* ct = ci >= 0 && (size_t)ci < d.containers.size() ? &d.containers[ci] : nullptr;
    if (ct && ct->kind == CT_ALERT) {
        int32_t fb = (int32_t)ct->firstBlock;
        while (fb < b && (d.blockSrc[fb].flags & BS_SYNTH)) fb++;
        if (fb == b) {  // the alert's first text: its tag goes, a plain quote is left (on its own line, or before the text)
            uint32_t ls = LineStartOf(src, bs.beg);
            size_t tag = Sub(src, ls, bs.beg).find(L"[!");
            if (tag != std::wstring::npos) {
                r.splices.push_back(Replace(src, ls + (uint32_t)tag, bs.beg, L""));
            } else if (bs.line > 0) {
                uint32_t pl = LineStartOf(src, BackEol(src, bs.line));
                r.splices.push_back(Replace(src, pl, bs.line, L""));
            }
            r.after.focus = MapThrough(r.splices, st.focus, true);
            r.after.anchor = r.after.focus;
            return r;
        }
    }
    if (ct && ct->kind == CT_QUOTE && ct->firstBlock == (uint32_t)b && TextBlk(d, b)) {
        // the first block of a quote: one `>` level (and a blank after it) off each of its lines; it leaves the quote (F15)
        auto chain = Chain(d, b);
        size_t quotes = 0;
        for (const ContainerSrc* k : chain) quotes += k->kind == CT_QUOTE || k->kind == CT_ALERT;
        std::vector<std::pair<uint32_t, uint32_t>> pre{{bs.line, bs.beg}};  // the prefix of each of its lines
        SegSpan ss = SegsIn(d, b, TRange{bl.textOff, bl.textOff + bl.textLen});
        for (const SrcSeg* g = ss.b; g < ss.e; g++)
            if (LineBrk(src, g)) pre.push_back({BrkEol(src, *g), g->s + g->sLen});
        for (size_t i = pre.size(); i-- > 0;) {
            uint32_t gt = UINT32_MAX;
            size_t n = 0;
            for (uint32_t k = pre[i].first; k < pre[i].second; k++)
                if (src[k] == L'>') gt = k, n++;
            if (n < quotes) continue;  // a lazy line: no marker of this quote on it
            r.splices.push_back(Replace(src, gt, gt + (gt + 1 < pre[i].second && Blank(src[gt + 1]) ? 2 : 1), L""));
        }
        r.after.focus = MapThrough(r.splices, st.focus, true);
        r.after.anchor = r.after.focus;
        return r;
    }
    if (bl.kind == BK_CODE) {
        if (bl.textLen) return P >= 0 ? Goto(c, st, P, true) : Nothing(st, EK_OTHER);
        EditResult del = DropLines(c, st, b, bs.line, SkipEol(src, bs.outerEnd));  // an empty code block goes whole
        del.kind = EK_STRUCT;
        if (P >= 0) del.after.focus = del.after.anchor = SrcOfText(d, src, BlockEdge(d, P, true), MAP_CARET);
        return del;
    }
    if (P < 0 || (d.blockSrc[P].flags & BS_FOOTNOTE)) return Nothing(st, EK_OTHER);
    if (TextBlk(d, P) && TextBlk(d, b)) return JoinBlocks(c, st, P, b, d.blockSrc[P].end, bs.beg, L"", 0, EK_DEL_BACK);
    return Goto(c, st, P, true);  // an object before it is selected (the next press deletes it); code, a table: their end
}

EditResult DelAtEnd(const EditCtx& c, const EditState& st, const TextPos& p) {
    const Doc& d = c.doc;
    const int32_t b = p.block;
    const int32_t N = NextInSource(d, b);
    if (p.cell >= 0) {  // a cell: the caret to the start of the next cell, or of the block after the table
        const Table* tb = TableOf(d, b);
        if ((uint32_t)p.cell + 1 < tb->rows * tb->cols) {
            EditResult r = Nothing(st, EK_OTHER);
            const Cell& nc = d.cells[tb->cellOff + p.cell + 1];
            r.after.focus = r.after.anchor = SrcOfText(d, c.src, TextPos{nc.textOff, b, p.cell + 1}, MAP_CARET);
            return r;
        }
        return N >= 0 ? Goto(c, st, N, false) : Nothing(st, EK_OTHER);
    }
    if (N < 0 || ((d.blockSrc[b].flags | d.blockSrc[N].flags) & BS_FOOTNOTE)) return Nothing(st, EK_OTHER);
    if (TextBlk(d, b) && TextBlk(d, N))
        return JoinBlocks(c, st, b, N, d.blockSrc[b].end, d.blockSrc[N].beg, L"", 0, EK_DEL_FWD);
    return Goto(c, st, N, false);  // code, a table: their first stop; an object: selected
}

// ---- Enter, Shift+Enter (§7.6)
// an ATX heading's text between two offsets on one line: soft breaks as blanks, hard breaks as <br> (F15)
std::wstring OneLine(const Doc& d, const std::wstring& src, int32_t b, uint32_t from, uint32_t to) {
    const Block& bl = d.blocks[b];
    SegSpan ss = SegsIn(d, b, TRange{bl.textOff, bl.textOff + bl.textLen});
    std::wstring out;
    uint32_t p = from;
    for (const SrcSeg* g = ss.b; g < ss.e; g++) {
        if (g->s < p || g->s + g->sLen > to || !LineBrk(src, g)) continue;
        out += Sub(src, p, g->s) + (d.text[g->t] == L'\n' ? L"<br>" : L" ");
        p = g->s + g->sLen;
    }
    return out + Sub(src, p, to);
}
bool WsText(wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\n'; }

// The split rule (F2, F7) for Enter (variant 0) and Shift+Enter (1) at text [tA, tB] of a text block or cell: the
// whitespace on both sides goes - a line break there too, whose source the new break replaces - and every span open
// there is closed before the break and opened again after it (Enter); the break is a blank line and the prefixes (a new
// item's marker in a list), a backslash hard break, or a <br> where only that fits (a heading, a cell). At a block's end
// or start, a phantom instead (UX-7); an item gets an empty item there, since one can be written.
EditResult Split(const EditCtx& c, const EditState& st, const TextPos& p, int variant, uint32_t tA, uint32_t tB) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const int32_t b = p.block;
    const BlockSrc& bs = d.blockSrc[b];
    TRange rg;
    if (!RangeOf(d, p, &rg)) return Refuse(st, "split");
    SegSpan ss = SegsIn(d, b, rg);
    auto ws = [&](const SrcSeg* g, uint32_t t) {
        if (!g || g->kind == SEG_OBJATOM || g->kind == SEG_SYNTH || !WsText(d.text[t])) return false;
        for (uint32_t k = g->kind == SEG_PLAIN ? t : g->t; k < (g->kind == SEG_PLAIN ? t + 1 : g->t + g->tLen); k++)
            if (!WsText(d.text[k])) return false;
        return true;
    };
    uint32_t t0 = tA, t1 = tB;
    while (t0 > rg.beg) {
        const SrcSeg* g = SegCovering(ss, t0 - 1);
        if (!ws(g, t0 - 1)) break;
        t0 = g->kind == SEG_PLAIN ? t0 - 1 : g->t;
    }
    while (t1 < rg.end) {
        const SrcSeg* g = SegCovering(ss, t1);
        if (!ws(g, t1)) break;
        t1 = g->kind == SEG_PLAIN ? t1 + 1 : g->t + g->tLen;
    }
    const bool cell = p.cell >= 0, atx = (bs.flags & BS_ATX) != 0, setext = (bs.flags & BS_SETEXT) != 0;
    const int32_t item = cell || variant ? -1 : ItemOf(d, b);
    const int depth = (int)Chain(d, b).size();
    EditResult r = Nothing(st, EK_STRUCT);
    r.after.atom = -1;
    r.after.phantom = Phantom{};
    const bool atEnd = t1 >= (cell ? rg.end : LastStop(d, b)), atStart = t0 <= rg.beg;
    if (!cell && tA == tB && (atEnd || atStart)) {
        if (item >= 0) {
            const ContainerSrc& it = d.containers[item];
            const std::wstring E = Eol(c, bs.line), mp = PrefixN(d, src, b, depth - 1, nullptr);
            const std::wstring gap = it.tight ? E : E + BlankPrefix(d, src, b) + E;
            if (atEnd) {  // an empty item after it, its trailing blanks gone
                uint32_t s = SrcOfText(d, src, TextPos{rg.end, b, -1}, MAP_OUTER_END), e = s;
                while (e < src.size() && Blank(src[e])) e++;
                if (!LineEndAt(src, e)) e = s;
                std::wstring ins = gap + mp + NewMarker(d, src, item, true);
                r.splices.push_back(Replace(src, s, e, ins));
                r.after.focus = r.after.anchor = s + (uint32_t)ins.size();
            } else {  // an empty item before it; the caret stays with its text
                r.splices.push_back(Splice{it.markOff, L"", NewMarker(d, src, item, false) + gap + mp});
                r.after.focus = r.after.anchor = MapThrough(r.splices, st.focus, true);
            }
            return r;
        }
        if (atEnd && variant)  // a hard break waits at the end: nothing is written until text follows (§6.6)
            return NewPhantom(c, st, PH_BREAK, b, depth, SrcOfText(d, src, TextPos{LastStop(d, b), b, -1}, MAP_CARET), true);
        if (atEnd) return After(c, st, b);
        uint32_t at;
        PrefixN(d, src, b, -1, &at);
        return NewPhantom(c, st, PH_BEFORE, b, depth, at, false);
    }
    uint32_t sA = SrcOfText(d, src, TextPos{t0, b, p.cell}, MAP_OUTER_END);
    uint32_t sB = SrcOfText(d, src, TextPos{t1, b, p.cell}, MAP_OUTER_START);
    if (sA == UINT32_MAX || sB == UINT32_MAX || sA > sB) return Refuse(st, "split");
    // Only blanks, line breaks and span delimiters go with the split: source that shows no text of its own there (a
    // <div> in the line, a comment) stays, and so does the whitespace on its far side.
    bool shrunk = false;
    if (tA == tB) {
        auto kept = [&](uint32_t k) {
            if (Blank(src[k]) || EolChar(src[k])) return false;
            for (const SrcSeg* g = ss.b; g < ss.e; g++)
                if (k >= g->s && k < g->s + g->sLen) return !(g->kind == SEG_TEXTATOM && ws(g, g->t));
            bool delim = false;
            ForSpans(d, p, rg, [&](const SpanSrc& sp) {
                delim |= (k >= sp.openBeg && k < sp.openEnd) || (k >= sp.closeBeg && k < sp.closeEnd);
            });
            return !delim;
        };
        uint32_t sC = SrcOfText(d, src, TextPos{tA, b, p.cell}, MAP_CARET);
        sC = sC == UINT32_MAX ? sA : std::clamp(sC, sA, sB);
        for (uint32_t k = sC; k-- > sA;)
            if (kept(k)) { sA = k + 1; shrunk = true; break; }
        for (uint32_t k = sC; k < sB; k++)
            if (kept(k)) { sB = k; shrunk = true; break; }
    }
    Bal k = Balance(c, p, sA, sB, variant == 0 && !cell);
    if (k.torn) return Refuse(st, "split");
    const std::wstring E = Eol(c, sA), pre = ContPrefix(d, src, b);
    const bool br = cell || (variant && (atx || setext));  // where only a <br> fits
    std::wstring brk;
    if (br) brk = L"<br>";
    else if (variant || (bs.flags & BS_FOOTNOTE)) brk = L"\\" + E + pre;
    else if (item >= 0)
        brk = (d.containers[item].tight ? E : E + Strip(pre) + E) + PrefixN(d, src, b, depth - 1, nullptr) + NewMarker(d, src, item, true);
    else brk = E + Strip(pre) + E + pre;
    std::wstring closers = Joined(k.close), openers = Joined(k.open);
    uint32_t lineAt;
    if (setext) {  // made an ATX heading first, its lines on one
        std::wstring head = std::wstring(std::max<uint8_t>(1, d.blocks[b].heading), L'#') + L" " +
                            OneLine(d, src, b, bs.beg, sA) + closers + brk;
        std::wstring rest = openers + (variant ? OneLine(d, src, b, sB, bs.end) + Sub(src, bs.end, bs.lineEnd) : Sub(src, sB, bs.lineEnd));
        r.splices.push_back(Replace(src, bs.beg, bs.outerEnd, head + rest));
        lineAt = bs.beg + (uint32_t)head.size();
    } else {
        if (atx && !variant) {  // the closing sequence stays with the heading
            std::wstring cl = AtxClose(src, bs);
            if (!cl.empty()) {
                r.splices.push_back(Replace(src, bs.end, bs.lineEnd, L""));
                closers += cl;
            }
        }
        r.splices.push_back(Replace(src, sA, sB, closers + brk + openers));
        lineAt = sA + (uint32_t)(closers.size() + brk.size());
    }
    r.after.focus = r.after.anchor = lineAt + (uint32_t)openers.size();
    if (!br && !(bs.flags & BS_FOOTNOTE)) Escape(r, openers + Sub(src, sB, LineEndOf(src, sB)), lineAt, variant == 0);
    if ((!k.close.empty() || !k.open.empty()) && !shrunk) {
        r.keep.on = true;
        r.keep.t0 = t0;
        r.keep.t1 = t1;
        r.keep.text = variant ? L"\n" : L"";
    }
    return r;
}

// Enter in code (§7.6): a new line with the container prefix and the current line's code indentation
EditResult CodeEnter(const EditCtx& c, const EditState& st, const TextPos& p) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const BlockSrc& bs = d.blockSrc[p.block];
    const Block& bl = d.blocks[p.block];
    uint32_t ls = p.t, ie;
    while (ls > bl.textOff && d.text[ls - 1] != L'\n') ls--;
    for (ie = ls; ie < p.t && Blank(d.text[ie]); ie++) {}
    uint32_t s = (bs.flags & BS_NOCONTENT) ? bs.beg : SrcOfText(d, src, p, MAP_CARET);
    std::wstring ins = std::wstring(Eol(c, s)) + ContPrefix(d, src, p.block) + ((bs.flags & BS_FENCED) ? L"" : L"    ") +
                       d.text.substr(ls, ie - ls);
    if (bs.flags & BS_NOCONTENT) ins += ins;  // the first content line, and the new one
    if (st.anchor != st.focus) return CutRange(c, st, ins, EK_STRUCT);
    EditResult r = Nothing(st, EK_STRUCT);
    if (s == UINT32_MAX) return Refuse(st, "nowhere");
    r.splices.push_back(Splice{s, L"", ins});
    r.after.focus = r.after.anchor = s + (uint32_t)ins.size();
    return r;
}
// a new row after a table's last, cells empty, the caret in cell col (§7.10)
EditResult NewRow(const EditCtx& c, const EditState& st, int32_t b, uint32_t col) {
    const Doc& d = c.doc;
    const Table& tb = *TableOf(d, b);
    const TableSrc& ts = d.tableSrc[d.blocks[b].aux];
    EditResult r = Nothing(st, EK_STRUCT);
    if (ts.rows.empty()) return Refuse(st, "table");
    uint32_t at = ts.rows.back().lineEnd, caret = 0;
    std::wstring ins = std::wstring(Eol(c, at)) + ContPrefix(d, c.src, b) + L"|";
    for (uint32_t k = 0; k < tb.cols; k++) {
        if (k == col) caret = (uint32_t)ins.size() + 1;
        ins += L"  |";
    }
    r.splices.push_back(Splice{at, L"", ins});
    r.after.focus = r.after.anchor = at + caret;
    r.after.atom = -1;
    return r;
}
// Enter in a cell (§7.6): the cell below; in the last row a new row - unless the row is empty, then it goes and a
// paragraph follows the table (UX-3)
EditResult CellEnter(const EditCtx& c, const EditState& st, const TextPos& p) {
    const Doc& d = c.doc;
    const Table& tb = *TableOf(d, p.block);
    const TableSrc& ts = d.tableSrc[d.blocks[p.block].aux];
    uint32_t row = (uint32_t)p.cell / tb.cols, col = (uint32_t)p.cell % tb.cols;
    if (row + 1 < tb.rows) {
        EditResult r = Nothing(st, EK_OTHER);
        const Cell& nc = d.cells[tb.cellOff + (row + 1) * tb.cols + col];
        r.after.focus = r.after.anchor = SrcOfText(d, c.src, TextPos{nc.textOff, p.block, (int32_t)((row + 1) * tb.cols + col)}, MAP_CARET);
        return r;
    }
    bool empty = row > 0 && ts.rows.size() == tb.rows + 1;
    for (uint32_t k = 0; empty && k < tb.cols; k++) empty = !d.cells[tb.cellOff + row * tb.cols + k].textLen;
    if (!empty) return NewRow(c, st, p.block, col);
    uint32_t a = ts.rows[row].lineEnd;  // the line before the row (the delimiter row, or the row above)
    EditResult r = NewPhantom(c, st, PH_AFTER, p.block, (int)Chain(d, p.block).size(), a, true);
    r.splices.push_back(Replace(c.src, a, ts.rows[row + 1].lineEnd, L""));
    r.kind = EK_STRUCT;
    return r;
}
// Enter in an empty item (§7.6): a nested one moves a level out; otherwise its line goes and a paragraph at the list's
// own level takes its place (a phantom after the items before it, with blank lines on both sides once typed into, F18)
EditResult EmptyItemEnter(const EditCtx& c, const EditState& st, int32_t b) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    int32_t item = ItemOf(d, b);
    if (item < 0) return Nothing(st, EK_OTHER);
    const ContainerSrc& it = d.containers[item];
    if (it.parent >= 0 && d.containers[it.parent].kind == CT_ITEM) {
        std::vector<Splice> v;
        if (!OutdentItem(c, item, v)) return Nothing(st, EK_OTHER);
        SortDown(v);
        EditResult r = Nothing(st, EK_STRUCT);
        r.splices = std::move(v);
        r.after.focus = r.after.anchor = MapThrough(r.splices, st.focus, true);
        return r;
    }
    const BlockSrc& bs = d.blockSrc[b];
    uint32_t a = bs.line, e = bs.lineEnd;
    if (a > 0) a = BackEol(src, a);
    else e = SkipEol(src, e);
    const int depth = (int)Chain(d, b).size() - 1;
    const int32_t P = PrevInSource(d, b), N = NextInSource(d, b);
    EditResult r;
    if (P >= 0) {
        r = NewPhantom(c, st, PH_AFTER, P, std::min(depth, (int)Chain(d, P).size()), d.blockSrc[P].outerEnd, true);
    } else if (N >= 0) {
        uint32_t at;
        PrefixN(d, src, N, depth, &at);
        r = NewPhantom(c, st, PH_BEFORE, N, depth, at - (e - a), true);
    } else {
        r = Nothing(st, EK_STRUCT);
        r.after.focus = r.after.anchor = a;
    }
    r.splices.push_back(Replace(src, a, e, L""));
    r.kind = EK_STRUCT;
    return r;
}

// ---- selections (§7.9)
std::wstring EscapePipes(const std::wstring& s) {
    std::wstring e;
    for (wchar_t ch : s) {
        if (ch == L'|') e += L'\\';
        e += ch;
    }
    return e;
}
// in one block or cell: the cut of Phase 2a (the selection's own delimiters rebalanced, typed text inside the spans
// that held its first character)
EditResult CutOne(const EditCtx& c, const EditState& st, const std::wstring& ins, EditKind kind) {
    SelCut cut = CutSelection(c, st);
    if (!cut.ok) return Refuse(st, "selection");
    const std::wstring& src = c.src;
    EditResult r = Nothing(st, kind);
    r.after.atom = -1;
    if (!ins.empty()) {
        r.splices.push_back(Replace(src, cut.a, cut.b, cut.wrapOpen + ins + cut.wrapClose + cut.closers + cut.openersRest));
        r.after.focus = r.after.anchor = cut.a + (uint32_t)(cut.wrapOpen.size() + ins.size());
        return r;
    }
    std::wstring keep = cut.closers + cut.openers;
    if (keep.empty()) {  // spans the cut empties go with their delimiters, blanks left at the content's start with them
        EditResult del = Deletion(c, st, cut.A, cut.rg, cut.A.t, cut.B.t, cut.a, cut.b, kind);
        del.after.atom = -1;
        return del;
    }
    r.splices.push_back(Replace(src, cut.a, cut.b, keep));
    r.after.focus = r.after.anchor = cut.a + (uint32_t)cut.closers.size();
    r.keep.on = true;
    r.keep.t0 = cut.A.t;
    r.keep.t1 = cut.B.t;
    return r;
}
// the covered part of cell k of block b, [t0, t1): its text goes, the spans it cuts rebalanced; no pipe is removed (F20)
void CutCell(const EditCtx& c, int32_t b, int32_t k, uint32_t t0, uint32_t t1, const std::wstring& ins, std::vector<Splice>& out,
             bool* torn) {
    const Doc& d = c.doc;
    const Table& tb = *TableOf(d, b);
    if (d.cellSrc[tb.cellOff + k].missing) return;
    TextPos p{t0, b, k};
    uint32_t sA = SrcOfText(d, c.src, p, MAP_OUTER_START), sB = SrcOfText(d, c.src, TextPos{t1, b, k}, MAP_OUTER_END);
    if (sA == UINT32_MAX || sB == UINT32_MAX || sB < sA) return;
    Bal bal = Balance(c, p, sA, sB, false);
    Cancel(bal);
    *torn |= bal.torn;
    std::wstring put = ins + Joined(bal.close) + Joined(bal.open);
    if (sB > sA || !put.empty()) out.push_back(Replace(c.src, sA, sB, put));
}
// the whole lines of a block that goes (an HTML block drawn as several: all of them), and the blank line after it when
// one before it (or the document's start) keeps its neighbours apart
Splice WholeLines(const Doc& d, const std::wstring& src, int32_t b) {
    const BlockSrc& bs = d.blockSrc[b];
    uint32_t end = bs.outerEnd;
    for (size_t k = b + 1; k < d.blocks.size() && (bs.flags & BS_RAW) && d.blockSrc[k].rawId == bs.rawId; k++)
        end = std::max(end, d.blockSrc[k].outerEnd);
    uint32_t a = bs.line, e = SkipEol(src, end), le = LineEndOf(src, e), pe = BackEol(src, a);
    if (e < src.size() && (!a || BlankLine(src, LineStartOf(src, pe), pe)) && BlankLine(src, e, le)) e = SkipEol(src, le);
    return Replace(src, a, e, L"");
}
// across blocks (F13: a block counts as covered when the range crosses its end): two text blocks are joined as §7.7
// joins them; otherwise each end loses its covered part - a table its covered cells, code its covered text (all of it
// with the fences) - and the blocks between go whole, the invisible lines between them kept (F6)
EditResult CutBlocks(const EditCtx& c, const EditState& st, TextPos A, TextPos B, const std::wstring& ins, EditKind kind) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    const int32_t bA = A.block, bB = B.block;
    const BlockSrc& as = d.blockSrc[bA];
    const BlockSrc& zs = d.blockSrc[bB];
    // a footnote definition is written away from where it is shown: no cut runs into or out of one (F5)
    if (((as.flags | zs.flags) & (BS_FOOTNOTE | BS_SYNTH)) || as.line > zs.line) return Refuse(st, "selection");
    TRange ra, rb;
    if (!RangeOf(d, A, &ra) || !RangeOf(d, B, &rb)) return Refuse(st, "selection");
    uint32_t sA = SrcOfText(d, src, A, A.t >= ra.end ? MAP_OUTER_END : MAP_OUTER_START);
    uint32_t sB = SrcOfText(d, src, B, B.t <= rb.beg ? MAP_OUTER_START : MAP_OUTER_END);
    if (sA == UINT32_MAX || sB == UINT32_MAX || sB < sA) return Refuse(st, "selection");
    const bool tA = TextBlk(d, bA) && A.cell < 0, tB = TextBlk(d, bB) && B.cell < 0;
    if (tA && tB) {
        Bal k;
        Bal ka = Balance(c, A, sA, sB, false), kb = Balance(c, B, sA, sB, false);
        if (ka.torn || kb.torn) return Refuse(st, "selection");
        k.close = ka.close;
        k.open = kb.open;
        Cancel(k);
        std::wstring wo, wc;  // typed over: the spans that held the first selected character go around the text (§7.9)
        if (!ins.empty())
            ForSpans(d, A, ra, [&](const SpanSrc& sp) {
                if ((sp.flags & SF_ENTERABLE) && !(sp.flags & SF_UNCLOSED) && sp.tBeg == A.t && sp.tEnd > A.t && sp.openBeg >= sA) {
                    wo += Sub(src, sp.openBeg, sp.openEnd);
                    wc.insert(0, Sub(src, sp.closeBeg, sp.closeEnd));
                }
            });
        std::wstring gap = wo + ins + wc + Joined(k.close) + Joined(k.open);
        const bool start = A.t <= ra.beg;
        if (start && ins.empty() && k.open.empty())
            while (sB < src.size() && Blank(src[sB])) sB++;  // blanks left at the content's start go
        EditResult r = JoinBlocks(c, st, bA, bB, sA, sB, gap, (uint32_t)(wo.size() + ins.size()), kind);
        if (start && ins.empty() && Para(d, bA)) Escape(r, gap + Sub(src, sB, LineEndOf(src, sB)), sA, true);
        if (ins.empty() && (!ka.close.empty() || !kb.open.empty())) {
            r.keep.on = true;
            r.keep.t0 = A.t;
            r.keep.t1 = B.t;
        }
        return r;
    }
    std::vector<Splice> v;
    bool torn = false, wholeA = false, wholeB = false;
    // the end block, when it stays: its covered part
    if (tB) {
        uint32_t cs = SrcOfText(d, src, TextPos{rb.beg, bB, -1}, MAP_OUTER_START);
        if (cs != UINT32_MAX && sB > cs) {
            Bal k = Balance(c, B, cs, sB, false);
            torn |= k.torn;
            uint32_t e = sB;
            if (k.open.empty())
                while (e < src.size() && Blank(src[e])) e++;
            v.push_back(Replace(src, cs, e, Joined(k.open)));
        }
    } else if (const Table* tb = TableOf(d, bB)) {
        for (int32_t k = B.cell; k >= 0; k--) {
            const Cell& cl = d.cells[tb->cellOff + k];
            CutCell(c, bB, k, cl.textOff, k == B.cell ? B.t : cl.textOff + cl.textLen, L"", v, &torn);
        }
    } else if (!IsAtomBlock(d, bB)) {  // code: the covered text; all of it takes the fences too
        uint32_t cs = SrcOfText(d, src, TextPos{rb.beg, bB, -1}, MAP_CARET);
        if (B.t >= rb.end) wholeB = true;
        else if (cs != UINT32_MAX && sB > cs) v.push_back(Replace(src, cs, sB, L""));
    }
    // the start block, when it stays: its covered part (typed text goes there)
    uint32_t caret = sA;
    if (tA) {
        uint32_t ce = SrcOfText(d, src, TextPos{ra.end, bA, -1}, MAP_OUTER_END);
        if (ce != UINT32_MAX && ce >= sA) {
            Bal k = Balance(c, A, sA, ce, false);
            torn |= k.torn;
            v.push_back(Replace(src, sA, ce, ins + Joined(k.close)));
            caret = sA + (uint32_t)ins.size();
        }
    } else if (const Table* tb = TableOf(d, bA)) {
        for (int32_t k = (int32_t)(tb->rows * tb->cols) - 1; k >= A.cell; k--) {
            const Cell& cl = d.cells[tb->cellOff + k];
            CutCell(c, bA, k, k == A.cell ? A.t : cl.textOff, cl.textOff + cl.textLen, k == A.cell ? ins : L"", v, &torn);
        }
        caret = (uint32_t)std::min<size_t>(sA + ins.size(), src.size());
    } else if (IsAtomBlock(d, bA) || A.t <= ra.beg) {
        if (!ins.empty()) return Refuse(st, "selection");  // (typed over an object: nowhere for the text to stand)
        wholeA = true;
        // it goes from its first line's start, but the markers of the containers that hold the end block too stay:
        // what is left of the end block joins them (an item that starts with a picture keeps its "- ")
        const std::vector<const ContainerSrc*> ca = Chain(d, bA), cb = Chain(d, bB);
        int common = 0;
        while (common < (int)ca.size() && common < (int)cb.size() && ca[common] == cb[common]) common++;
        uint32_t at = as.line;
        PrefixN(d, src, bA, common, &at);
        caret = std::max(at, as.line);
    } else {
        uint32_t ce = SrcOfText(d, src, TextPos{ra.end, bA, -1}, MAP_CARET);
        if (ce != UINT32_MAX && ce > sA) v.push_back(Replace(src, sA, ce, L""));
    }
    // The whole lines between - of the blocks between, and of an end that goes whole - as one range, so no two line ends
    // of different kinds meet where lines went (a lone CR and a lone LF would make one CRLF); the invisible lines in it
    // are kept, with a blank line after them.
    uint32_t X = wholeA ? caret : UINT32_MAX, Y = zs.line;
    for (uint32_t k : d.blockOrder)
        if (X == UINT32_MAX && d.blockSrc[k].line > as.outerEnd && d.blockSrc[k].line < zs.line && !(d.blockSrc[k].flags & BS_FOOTNOTE))
            X = d.blockSrc[k].line;
    if (wholeB) {
        Splice w = WholeLines(d, src, bB);
        Y = w.at + (uint32_t)w.removed.size();
        X = std::min(X, zs.line);
    }
    if (X != UINT32_MAX && X < Y) {
        std::wstring keep;
        for (uint32_t l = X; l < Y;) {
            uint32_t le = LineEndOf(src, l), nx = SkipEol(src, le);
            if (!BlankLine(src, l, le) && !Owned(d, l)) keep += Sub(src, l, nx);
            l = nx > l ? nx : l + 1;
        }
        if (!keep.empty()) keep += Eol(c, X);
        else if (X > 0 && src[X - 1] == L'\r' && Y < src.size() && src[Y] == L'\n') Y++;
        v.push_back(Replace(src, X, Y, keep));
    }
    if (torn) return Refuse(st, "selection");
    SortDown(v);
    Coalesce(v);
    EditResult r = Nothing(st, kind);
    r.splices = std::move(v);
    r.after.focus = r.after.anchor = caret;
    r.after.atom = -1;
    r.after.phantom = Phantom{};
    return r;
}
EditResult CutRange(const EditCtx& c, const EditState& st, std::wstring ins, EditKind kind) {
    const Doc& d = c.doc;
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr), an = AnchorOf(c, st);
    if (!ValidBlock(d, f.block) || !ValidBlock(d, an.block)) return Refuse(st, "selection");
    TextPos A = PosBefore(an, f) ? an : f, B = PosBefore(an, f) ? f : an;
    if (TableOf(d, A.block) && A.cell >= 0) ins = EscapePipes(ins);
    if (A.block == B.block && A.cell == B.cell) return CutOne(c, st, ins, kind);
    if (A.block == B.block) {  // one table, several cells: each is cleared on its own, no pipe removed (F20)
        const Table& tb = *TableOf(d, A.block);
        std::vector<Splice> v;
        bool torn = false;
        for (int32_t k = B.cell; k >= A.cell; k--) {
            const Cell& cl = d.cells[tb.cellOff + k];
            CutCell(c, A.block, k, k == A.cell ? A.t : cl.textOff, k == B.cell ? B.t : cl.textOff + cl.textLen,
                    k == A.cell ? ins : L"", v, &torn);
        }
        if (torn) return Refuse(st, "selection");
        EditResult r = Nothing(st, kind);
        r.splices = std::move(v);
        uint32_t sA = SrcOfText(d, c.src, A, MAP_OUTER_START);
        r.after.focus = r.after.anchor = sA == UINT32_MAX ? st.focus : sA + (uint32_t)ins.size();
        r.after.atom = -1;
        return r;
    }
    return CutBlocks(c, st, A, B, ins, kind);
}
}  // namespace

static EditResult BackspaceOp(const EditCtx& c, const EditState& st, bool word) {
    if (InPh(st)) return PhBack(c, st);
    if (st.atom >= 0) return DeleteAtom(c, st);
    if (st.anchor != st.focus) return CutRange(c, st, L"", EK_OTHER);
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    EditResult r = Nothing(st, EK_DEL_BACK);
    uint16_t trail = 0;
    TextPos p = FocusOf(c, st, &trail);
    if (!ValidBlock(d, p.block)) return r;
    const BlockSrc& bs = d.blockSrc[p.block];
    if (AtomBlock(bs) || (bs.flags & BS_SYNTH)) return r;
    if (trail && st.focus > 0 && Blank(src[st.focus - 1])) {  // one of the trailing blanks, one at a time (§6.5)
        r.splices.push_back(Splice{st.focus - 1, src.substr(st.focus - 1, 1), L""});
        r.after.focus = r.after.anchor = st.focus - 1;
        return r;
    }
    TRange rg;
    if (!RangeOf(d, p, &rg)) return r;
    if (p.t <= rg.beg) return BackAtStart(c, st, p);  // a block's (a cell's) start: §7.7's table
    SegSpan ss = SegsIn(d, p.block, rg);
    const SrcSeg* L = SegCovering(ss, p.t - 1);
    if (!L) return r;
    if (L->kind == SEG_OBJATOM) return SelectAtom(d, st, ImageOfSeg(d, p.block, *L));  // the second press deletes it
    if (L->kind == SEG_SYNTH) return r;
    uint32_t t0, s0, s1;
    if (L->kind == SEG_TEXTATOM) {  // an entity, an escape, an emoji, a break: whole
        t0 = L->t;
        s0 = L->s;
        s1 = L->s + L->sLen;
    } else {
        t0 = word ? WordBack(d.text, p.t, rg.beg) : ClusterStep(c, p.t, -1, L->t, p.t);
        const SrcSeg* k = L;  // a word goes on into the plain text before only where no delimiter stands between
        while (t0 < k->t && k > ss.b && (k - 1)->kind == SEG_PLAIN && (k - 1)->s + (k - 1)->sLen == k->s &&
               (k - 1)->t + (k - 1)->tLen == k->t)
            k--;
        t0 = std::max(t0, k->t);
        s0 = k->s + (t0 - k->t);
        s1 = L->s + (p.t - L->t);
    }
    return Deletion(c, st, p, rg, t0, p.t, s0, s1, EK_DEL_BACK);
}

static EditResult DeleteOp(const EditCtx& c, const EditState& st, bool word) {
    if (InPh(st)) return PhDel(c, st);
    if (st.atom >= 0) return DeleteAtom(c, st);
    if (st.anchor != st.focus) return CutRange(c, st, L"", EK_OTHER);
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    EditResult r = Nothing(st, EK_DEL_FWD);
    uint16_t trail = 0;
    TextPos p = FocusOf(c, st, &trail);
    if (!ValidBlock(d, p.block)) return r;
    const BlockSrc& bs = d.blockSrc[p.block];
    if (AtomBlock(bs) || (bs.flags & BS_SYNTH)) return r;
    TRange rg;
    if (!RangeOf(d, p, &rg)) return r;
    SegSpan ss = SegsIn(d, p.block, rg);
    const SrcSeg* R = p.t < rg.end ? SegCovering(ss, p.t) : nullptr;
    // Trailing blanks go one at a time (§6.5) - but the blanks a break starts with, at its left edge, are the break:
    // Delete takes it whole (§6.6)
    if (TrailingRun(src, st.focus, p.cell >= 0) && !(!trail && R && R->kind == SEG_TEXTATOM && R->s == st.focus)) {
        r.splices.push_back(Splice{st.focus, src.substr(st.focus, 1), L""});
        return r;
    }
    if (p.t >= rg.end) return DelAtEnd(c, st, p);  // a block's (a cell's) end: §7.7's table
    if (!R || R->kind == SEG_SYNTH) return r;
    if (R->kind == SEG_OBJATOM) return SelectAtom(d, st, ImageOfSeg(d, p.block, *R));
    uint32_t t1, s0, s1;
    if (R->kind == SEG_TEXTATOM) {
        t1 = R->t + R->tLen;
        s0 = R->s;
        s1 = R->s + R->sLen;
    } else {
        t1 = word ? WordFwd(d.text, p.t, rg.end) : ClusterStep(c, p.t, 1, p.t, R->t + R->tLen);
        const SrcSeg* k = R;
        while (t1 > k->t + k->tLen && k + 1 < ss.e && (k + 1)->kind == SEG_PLAIN && k->s + k->sLen == (k + 1)->s &&
               k->t + k->tLen == (k + 1)->t)
            k++;
        t1 = std::min(t1, k->t + k->tLen);
        s0 = R->s + (p.t - R->t);
        s1 = k->s + (t1 - k->t);
    }
    return Deletion(c, st, p, rg, p.t, t1, s0, s1, EK_DEL_FWD);
}

EditResult OpBackspace(const EditCtx& c, const EditState& st, bool word) { return Carry(BackspaceOp(c, st, word)); }
EditResult OpDelete(const EditCtx& c, const EditState& st, bool word) { return Carry(DeleteOp(c, st, word)); }

// The selection goes (§7.9): inside one block or cell, rebalanced; across cells, each cleared on its own; across blocks,
// the ends joined when both are text. The caret collapses where the selection began.
EditResult OpDeleteSelection(const EditCtx& c, const EditState& st) { return Carry(CutRange(c, st, L"", EK_OTHER)); }

// Typing over a selection (F13): the typed text takes the place of the first selected character, inside the spans that
// held it (`**⟦bold⟧**` + `strong` → `**strong**`); the delimiters of spans the selection cut in two follow it.
EditResult OpReplaceSelection(const EditCtx& c, const EditState& st, std::wstring_view text) {
    return Carry(CutRange(c, st, std::wstring(text), EK_TYPE));
}

// ------------------------------------------------------------------------------------------------ structure (§6.7, §7.6-§7.11)
static EditResult EnterOp(const EditCtx& c, const EditState& st, int variant) {
    const Doc& d = c.doc;
    if (InPh(st)) return PhEnter(c, st, variant);
    if (st.atom >= 0) {  // a rule: a paragraph after it; Ctrl+Enter: after any object (their popups are Phase 3b's)
        int32_t ab = AtomBlockOf(d, st.atom);
        if (ab >= 0 && (variant == 2 || ((st.atom & kAtomBlock) && d.blocks[ab].kind == BK_HR))) return After(c, st, ab);
        return Nothing(st, EK_OTHER);
    }
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr), a = AnchorOf(c, st);
    if (!ValidBlock(d, f.block)) return Nothing(st, EK_OTHER);
    const BlockSrc& bs = d.blockSrc[f.block];
    if (variant == 2) return After(c, st, f.block);  // Ctrl+Enter: a new paragraph after the block, at its level
    if (bs.flags & (BS_SYNTH | BS_OBJECT | BS_RAW)) return Nothing(st, EK_OTHER);
    const bool sel = st.anchor != st.focus;
    if (sel && (a.block != f.block || a.cell != f.cell)) return CutRange(c, st, L"", EK_OTHER);  // across blocks: that first
    if (d.blocks[f.block].kind == BK_CODE) return CodeEnter(c, st, f);
    TRange rg;
    if (!RangeOf(d, f, &rg)) return Nothing(st, EK_OTHER);
    const uint32_t tA = std::min(a.t, f.t), tB = std::max(a.t, f.t);
    if (sel && (tA <= rg.beg || tB >= rg.end)) return CutRange(c, st, L"", EK_OTHER);
    if (TableOf(d, f.block)) return variant ? Split(c, st, f, 1, tA, tB) : sel ? CutRange(c, st, L"", EK_OTHER) : CellEnter(c, st, f);
    if (bs.flags & BS_EMPTYITEM) return variant ? Nothing(st, EK_OTHER) : EmptyItemEnter(c, st, f.block);
    return Split(c, st, f, (bs.flags & BS_FOOTNOTE) ? 1 : variant, tA, tB);  // a definition is one paragraph (F5)
}
EditResult OpEnter(const EditCtx& c, const EditState& st, int variant) { return Carry(EnterOp(c, st, variant)); }

static EditResult TabOp(const EditCtx& c, const EditState& st, bool shift) {
    const Doc& d = c.doc;
    if (InPh(st)) {  // in a phantom after a list item: it takes the item's continuation prefix
        const Phantom& ph = st.phantom;
        int32_t ab = PhantomBlock(d, c.src, ph);
        auto chain = ab >= 0 ? Chain(d, ab) : std::vector<const ContainerSrc*>();
        if (shift || ph.kind != PH_AFTER || ph.depth >= chain.size() || chain[ph.depth]->kind != CT_ITEM)
            return Nothing(st, EK_OTHER);
        return NewPhantom(c, st, PH_AFTER, ab, ph.depth + 1, ph.anchorSrc, true);
    }
    if (st.atom >= 0) return Nothing(st, EK_OTHER);
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr), a = AnchorOf(c, st);
    if (!ValidBlock(d, f.block) || !ValidBlock(d, a.block)) return Nothing(st, EK_OTHER);
    const Block& bl = d.blocks[f.block];
    const BlockSrc& bs = d.blockSrc[f.block];
    const std::wstring& src = c.src;
    if (TableOf(d, f.block) && f.cell >= 0) {  // the next (previous) cell, its text selected; after the last, a new row
        const Table& tb = *TableOf(d, f.block);
        int32_t to = f.cell + (shift ? -1 : 1);
        if (to < 0) return Nothing(st, EK_OTHER);
        if ((uint32_t)to >= tb.rows * tb.cols) return NewRow(c, st, f.block, 0);
        const Cell& cl = d.cells[tb.cellOff + to];
        EditResult r = Nothing(st, EK_OTHER);
        r.after.anchor = SrcOfText(d, src, TextPos{cl.textOff, f.block, to}, MAP_CARET);
        r.after.focus = SrcOfText(d, src, TextPos{cl.textOff + cl.textLen, f.block, to}, MAP_CARET);
        return r;
    }
    std::vector<Splice> v;
    if (bl.kind == BK_CODE) {
        // four blanks (a tab where the block's indentation has tabs) at the caret; on every line a selection touches, or
        // with Shift, at the line's start after the container prefix - Shift takes up to four blanks or a tab away
        bool tabs = false;
        SegSpan ss = SegsIn(d, f.block, TRange{bl.textOff, bl.textOff + bl.textLen});
        for (const SrcSeg* g = ss.b; g < ss.e; g++) tabs |= g->kind == SEG_TEXTATOM && g->sLen == 1 && src[g->s] == L'\t';
        const std::wstring unit = tabs ? L"\t" : L"    ";
        TextPos A = PosBefore(a, f) ? a : f, B = PosBefore(a, f) ? f : a;
        bool lines = A.block == B.block && d.text.find(L'\n', A.t) < B.t;
        if (!shift && !lines) {
            if (st.anchor != st.focus) return CutRange(c, st, unit, EK_STRUCT);
            uint32_t s = SrcOfText(d, src, f, (bs.flags & BS_NOCONTENT) ? MAP_OUTER_START : MAP_CARET);
            if (s == UINT32_MAX) return Nothing(st, EK_OTHER);
            EditResult r = Nothing(st, EK_STRUCT);
            r.splices.push_back(Splice{s, L"", unit});
            r.after.focus = r.after.anchor = s + (uint32_t)unit.size();
            return r;
        }
        uint32_t t = A.t;
        while (t > bl.textOff && d.text[t - 1] != L'\n') t--;
        for (;;) {
            uint32_t s = SrcOfText(d, src, TextPos{t, f.block, -1}, MAP_CARET);
            if (s != UINT32_MAX && !shift) v.push_back(Splice{s, L"", unit});
            if (s != UINT32_MAX && shift) {
                uint32_t q = s;
                if (q < src.size() && src[q] == L'\t') q++;
                else while (q < src.size() && q - s < 4 && src[q] == L' ') q++;
                if (q > s) v.push_back(Replace(src, s, q, L""));
            }
            size_t nl = d.text.find(L'\n', t);
            if (nl == std::wstring::npos || nl >= (lines ? B.t : A.t) || nl >= bl.textOff + bl.textLen) break;
            t = (uint32_t)nl + 1;
        }
    } else if (TextBlk(d, f.block)) {
        // list items: every one the caret or the selection is in - the outermost ones, their children go along
        TextPos A = PosBefore(a, f) ? a : f, B = PosBefore(a, f) ? f : a;
        std::vector<int32_t> items;
        for (int32_t b = A.block; b <= B.block; b++) {
            int32_t it = ValidBlock(d, b) ? ItemAround(d, b) : -1;
            if (it >= 0 && std::find(items.begin(), items.end(), it) == items.end()) items.push_back(it);
        }
        items.erase(std::remove_if(items.begin(), items.end(), [&](int32_t it) {
            for (int32_t k = d.containers[it].parent; k >= 0; k = d.containers[k].parent)
                if (std::find(items.begin(), items.end(), k) != items.end()) return true;
            return false;
        }), items.end());
        if (!items.empty()) {
            const ContainerSrc& first = d.containers[items[0]];
            if (shift && (first.parent < 0 || d.containers[first.parent].kind != CT_ITEM))
                return Unlist(c, st, items[0]);  // at the top level: a paragraph (as Backspace on a first item)
            if (shift) OutdentItem(c, items[0], v);  // (the items after it become its children: one at a time)
            else for (int32_t it : items) NestItem(c, it, v);
        } else if (!shift && Para(d, f.block)) {
            // a paragraph after a list item joins that item, as a paragraph of its own (UX-1)
            int32_t P = PrevInSource(d, f.block), I = -1;
            for (int32_t k = P >= 0 ? d.blockSrc[P].container : -1; k >= 0 && I < 0; k = d.containers[k].parent)
                if (d.containers[k].kind == CT_ITEM && !InCont(d, f.block, k)) I = k;
            if (I >= 0) {
                uint32_t want = d.containers[I].contentCol;
                auto add = [&](uint32_t pos) {
                    uint32_t col = Col(src, pos);
                    if (want > col) v.push_back(Splice{pos, L"", std::wstring(want - col, L' ')});
                };
                add(bs.beg);
                SegSpan ss = SegsIn(d, f.block, TRange{bl.textOff, bl.textOff + bl.textLen});
                for (const SrcSeg* g = ss.b; g < ss.e; g++)
                    if (LineBrk(src, g)) add(g->s + g->sLen);
            }
        }
    }
    if (v.empty()) return Nothing(st, EK_OTHER);
    SortDown(v);
    EditResult r = Nothing(st, EK_STRUCT);
    r.splices = std::move(v);
    r.after.focus = MapThrough(r.splices, st.focus, true);
    r.after.anchor = MapThrough(r.splices, st.anchor, true);
    return r;
}
EditResult OpTab(const EditCtx& c, const EditState& st, bool shift) { return Carry(TabOp(c, st, shift)); }

// Plain-text paste (§7.11): line ends become the file's (D15), lone surrogates U+FFFD; lines 2…n get the block's
// continuation prefix (a blank line its blank prefix; code keeps the pasted indentation after it); in a cell line ends
// become <br> and pipes \|, in an ATX heading blanks. Pasted source is taken as it is: no §7.4 escaping. The private
// format is Phase 3b's; until then it is pasted as plain text.
EditResult OpPaste(const EditCtx& c, const EditState& st, std::wstring_view text, bool) {
    const Doc& d = c.doc;
    const std::wstring& src = c.src;
    if (st.atom >= 0) return Refuse(st, "atom");
    std::vector<std::wstring> lines(1);
    for (size_t i = 0; i < text.size(); i++) {
        wchar_t ch = text[i];
        if (ch == L'\r' || ch == L'\n') {
            if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') i++;
            lines.emplace_back();
            continue;
        }
        bool pair = HighSur(ch) && i + 1 < text.size() && LowSur(text[i + 1]);
        if (pair) lines.back() += ch, ch = text[++i];
        else if (HighSur(ch) || LowSur(ch)) ch = 0xFFFD;
        lines.back() += ch;
    }
    uint16_t tr = 0;
    TextPos f = FocusOf(c, st, &tr), a = AnchorOf(c, st), at = PosBefore(a, f) ? a : f;
    std::wstring pre, E = Eol(c, InPh(st) ? st.phantom.anchorSrc : std::min<uint32_t>(st.focus, (uint32_t)src.size()));
    bool cell = false, atx = false;
    if (InPh(st)) {
        pre = st.phantom.prefix;
    } else if (ValidBlock(d, at.block)) {
        pre = ContPrefix(d, src, at.block);
        cell = TableOf(d, at.block) != nullptr;
        atx = (d.blockSrc[at.block].flags & BS_ATX) != 0;
        if (d.blocks[at.block].kind == BK_CODE && !(d.blockSrc[at.block].flags & BS_FENCED)) pre += L"    ";
    }
    std::wstring out = lines[0];
    for (size_t k = 1; k < lines.size(); k++)  // (the last line goes on with the rest of the line pasted into)
        out += (cell ? L"<br>" : atx ? L" " : E + (lines[k].empty() && k + 1 < lines.size() ? Strip(pre) : pre)) + lines[k];
    return Carry(Insert(c, st, out, false, EK_PASTE));
}

// ------------------------------------------------------------------------------------------------ phantoms (§6.7)
int32_t PhantomBlock(const Doc& d, const std::wstring& src, const Phantom& ph) {
    if (ph.kind == PH_NONE || !d.hasMap) return -1;
    return TextOfSrc(d, src, ph.anchorSrc, ph.kind == PH_BEFORE ? 1 : -1, nullptr).block;
}

EditResult OpPhantom(const EditCtx& c, const EditState& st, int32_t b, bool before, int depth) {
    if (!ValidBlock(c.doc, b) || (c.doc.blockSrc[b].flags & BS_SYNTH)) return Nothing(st, EK_OTHER);
    int full = (int)Chain(c.doc, b).size();
    depth = depth < 0 ? full : std::min(depth, full);
    if (!before) {
        EditResult r = After(c, st, b);
        if (depth == full) return r;
        return NewPhantom(c, st, PH_AFTER, b, depth, r.after.phantom.anchorSrc, true);
    }
    uint32_t at;
    PrefixN(c.doc, c.src, b, depth, &at);
    return NewPhantom(c, st, PH_BEFORE, b, depth, at, true);
}

bool PhantomAlive(const Doc& d, const std::wstring& src, const EditState& st, int32_t caretBlock) {
    if (st.phantom.kind == PH_NONE) return false;
    int32_t b = PhantomBlock(d, src, st.phantom);
    return b >= 0 && (st.phantom.in || caretBlock == b);
}

uint32_t MapThrough(const std::vector<Splice>& sps, uint32_t pos, bool after) {
    for (const Splice& sp : sps) {
        uint32_t rem = (uint32_t)sp.removed.size(), ins = (uint32_t)sp.inserted.size();
        if (pos < sp.at) continue;
        if (!rem && pos == sp.at) {
            if (after) pos += ins;
        } else if (pos >= sp.at + rem && pos > sp.at) {
            pos = pos - rem + ins;
        } else {
            pos = sp.at;  // inside what went: where its replacement starts
        }
    }
    return pos;
}

// §7.5 step 5, the check of a step that wrote delimiters back (EditResult::keep)
bool Kept(const Doc& a, const Doc& b, const EditResult::Keep& k) {
    if (!k.on) return true;
    const size_t n = a.text.size(), m = k.text.size();
    if (k.t0 > k.t1 || k.t1 > n || b.text.size() != n - (k.t1 - k.t0) + m) return false;
    if (a.text.compare(0, k.t0, b.text, 0, k.t0) || b.text.compare(k.t0, m, k.text) ||
        a.text.compare(k.t1, std::wstring::npos, b.text, k.t0 + m, std::wstring::npos))
        return false;
    // the characters beside the change keep their formatting (blanks aside: §7.5 moves them out of spans)
    auto same = [&](uint32_t ta, uint32_t tb) { return SpaceChar(a.text[ta]) || FlagsAt(a, ta) == FlagsAt(b, tb); };
    for (uint32_t i = 1; i <= 2 && i <= k.t0; i++)
        if (!same(k.t0 - i, k.t0 - i)) return false;
    for (uint32_t i = 0; i < 2 && k.t1 + i < n; i++)
        if (!same(k.t1 + i, k.t0 + (uint32_t)m + i)) return false;
    return true;
}

// ------------------------------------------------------------------------------------------------ task boxes (§8.10)
EditResult OpTaskToggle(const EditCtx& c, const EditState& st, int task) {
    const Doc& d = c.doc;
    if (task < 0 || (size_t)task >= d.tasks.size()) return Refuse(st, "task");
    uint32_t at = d.tasks[task].src;
    if (at == 0 || at + 1 >= c.src.size() || c.src[at - 1] != L'[' || c.src[at + 1] != L']') return Refuse(st, "task");
    wchar_t was = c.src[at];
    EditResult r = Nothing(st, EK_TASK);
    r.splices.push_back(Splice{at, std::wstring(1, was), std::wstring(1, was == L'x' || was == L'X' ? L' ' : L'x')});
    return r;
}

// Back to the compiler's own inlining before the end of the file, where the templates used above are instantiated: the
// whole program shares them (link-time code generation keeps one copy), the parser's string and vector growth among
// them. Left at depth 0 they made every parse about a third slower (found in Phase 2b, §5.8).
#pragma inline_depth()
