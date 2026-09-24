// Edit mode's core, the map half (docs/EDIT-MODE.md §4.5, §5.5, §6.2-§6.4, §7.2): where a caret may stand, which
// source offset a text position stands for and back, the map's self-check, block diffing, line ends and the prefixes
// a new line inside containers needs.
//
// Everything here is a pure function of a Doc parsed with ParseOptions::wantMap and the source it was parsed from:
// no `g`, no window, no Win32 object. The unit tests (tests/edit) and the fuzzer call it directly.
#include "editcore.h"

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
        // the blanks a soft or hard break starts with are the trailing blanks of its line (§6.5)
        if (pv.kind == SEG_TEXTATOM && pv.tLen == 1 && (d.text[pv.t] == L' ' || d.text[pv.t] == L'\n') &&
            AllBlank(src, pv.s, s)) {
            if (trailCols) *trailCols = (uint16_t)std::min<uint32_t>(0xFFFF, ColsOf(src, LineStartOf(src, pv.s), pv.s, s));
            return TextPos{pv.t, block, cell};
        }
        return TextPos{dir < 0 ? pv.t : pv.t + pv.tLen, block, cell};  // inside an atom: its nearer edge that way
    }
    if (pv.kind == SEG_SYNTH) return TextPos{SynthRunStart(ss.b, it - 1)->t, block, cell};  // never past its start
    uint32_t after = pv.t + pv.tLen;
    // trailing blanks: after the last segment of a line, before its end (or the cell's pipe)
    uint32_t le = LineEndOf(src, pvEnd);
    if (s > pvEnd && s <= le && AllBlank(src, pvEnd, s) && (!nx || nx->s > le || nx->kind == SEG_SYNTH)) {
        if (trailCols) *trailCols = (uint16_t)std::min<uint32_t>(0xFFFF, ColsOf(src, LineStartOf(src, pvEnd), pvEnd, s));
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
// What a new continuation line of block b starts with: for each container outer -> inner, a quote's ">" (and the blank
// after it when b's own first line has one there), a list item's indentation up to its content column; a footnote
// adds nothing.
std::wstring ContPrefix(const Doc& d, const std::wstring& src, int block) {
    std::wstring out;
    if (!ValidBlock(d, block)) return out;
    const BlockSrc& bs = d.blockSrc[block];
    if (bs.flags & BS_SYNTH) return out;
    // p walks b's first line, col is p's column there: an item's content column is counted from the line start, with
    // every '>' and blank before it, so the spaces it needs are what is left after the quote markers written so far
    uint32_t le = LineEndOf(src, bs.line), p = bs.line, col = 0;
    auto step = [&]() { col = src[p] == L'\t' ? (col + 4) & ~3u : col + 1; p++; };
    for (const ContainerSrc* c : Chain(d, block)) {
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
    return out;
}

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
