// Edit mode's core, the part the two halves share: the helpers of editcore.cpp (the map, typing, deleting and the
// structure of Phases 1a-2b) that editops.cpp (the formatting commands and inserts of Phase 3a, docs/EDIT-MODE.md §8)
// builds on. Not an API: only editcore.cpp and editops.cpp include it. Pure like the rest of the core - no `g`, no window.
#pragma once
#include "editcore.h"

namespace ec {
// ---- characters and lines
bool Blank(wchar_t c);
bool EolChar(wchar_t c);
bool HighSur(wchar_t c);
bool LowSur(wchar_t c);
bool WordChar(wchar_t c);
bool SpaceChar(wchar_t c);
bool Significant(wchar_t c);
uint32_t LineStartOf(const std::wstring& src, uint32_t s);
uint32_t LineEndOf(const std::wstring& src, uint32_t s);
bool LineEndAt(const std::wstring& src, uint32_t s);
uint32_t ColsOf(const std::wstring& src, uint32_t ls, uint32_t from, uint32_t to);
uint32_t Col(const std::wstring& src, uint32_t s);
bool AllBlank(const std::wstring& src, uint32_t from, uint32_t to);
bool BlankLine(const std::wstring& src, uint32_t ls, uint32_t le);
uint32_t SkipEol(const std::wstring& src, uint32_t s);
uint32_t BackEol(const std::wstring& src, uint32_t x);
std::wstring Strip(std::wstring p);
std::wstring Sub(const std::wstring& s, uint32_t a, uint32_t b);
Splice Replace(const std::wstring& src, uint32_t a, uint32_t b, std::wstring ins);
uint32_t WordBack(const std::wstring& t, uint32_t p, uint32_t lo);
uint32_t WordFwd(const std::wstring& t, uint32_t p, uint32_t hi);

// a few elements put in order: an insertion sort (std::sort's code is kilobytes for every place it is used)
template <class T, class Less> void Order(std::vector<T>& v, Less less) {
    for (size_t i = 1; i < v.size(); i++)
        for (size_t k = i; k > 0 && less(v[k], v[k - 1]); k--) std::swap(v[k], v[k - 1]);
}

// ---- blocks, cells, segments, spans
bool Hidden(const Doc& d, const Block& b);
bool AtomBlock(const BlockSrc& bs);
bool ValidBlock(const Doc& d, int32_t b);
// A text range a position lives in: its block's text, or in a table the text of its cell.
struct TRange { uint32_t beg, end; };
const Table* TableOf(const Doc& d, int32_t block);
bool RangeOf(const Doc& d, const TextPos& p, TRange* r);
// the segments of a block that lie in [r.beg, r.end): its whole slice, or the part a cell owns
struct SegSpan { const SrcSeg* b; const SrcSeg* e; };
SegSpan SegsIn(const Doc& d, int32_t block, TRange r);
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
// the same spans as a list: for the commands, which run once per key - one loop each instead of a template copy each
std::vector<const SpanSrc*> SpansIn(const Doc& d, const TextPos& p, TRange r);
uint32_t InsertionPoint(const Doc& d, const std::wstring& src, const TextPos& p);
uint32_t LastStop(const Doc& d, int32_t block);
TextPos BlockEdge(const Doc& d, int32_t block, bool atEnd);
std::vector<const ContainerSrc*> Chain(const Doc& d, int block);
std::wstring PrefixN(const Doc& d, const std::wstring& src, int block, int n, uint32_t* at);
const SrcSeg* SegCovering(SegSpan ss, uint32_t t);
const SrcSeg* SegEndingAt(SegSpan ss, uint32_t t);
const SrcSeg* SegStartingAt(SegSpan ss, uint32_t t);
bool SoftBreak(const Doc& d, const std::wstring& src, const SrcSeg* g);
bool LineBrk(const std::wstring& src, const SrcSeg* g);
uint32_t BrkEol(const std::wstring& src, const SrcSeg& g);
uint16_t FlagsAt(const Doc& d, uint32_t t);
int32_t PrevInSource(const Doc& d, int32_t b);
int32_t NextInSource(const Doc& d, int32_t b);
bool TextBlk(const Doc& d, int32_t b);
bool Para(const Doc& d, int32_t b);
int32_t ItemOf(const Doc& d, int32_t b);
int32_t ItemAround(const Doc& d, int32_t b);
bool InCont(const Doc& d, int32_t b, int32_t ci);
std::wstring AtxClose(const std::wstring& src, const BlockSrc& bs);
bool Owned(const Doc& d, uint32_t l);
std::wstring OneLine(const Doc& d, const std::wstring& src, int32_t b, uint32_t from, uint32_t to);

// ---- results, the caret, phantoms
EditResult Nothing(const EditState& st, EditKind k);
EditResult Refuse(const EditState& st, const char* why);
TextPos FocusOf(const EditCtx& c, const EditState& st, uint16_t* trail);
TextPos AnchorOf(const EditCtx& c, const EditState& st);
bool PosBefore(const TextPos& a, const TextPos& b);
bool InPh(const EditState& st);
const wchar_t* Eol(const EditCtx& c, uint32_t s);
EditResult Goto(const EditCtx& c, const EditState& st, int32_t b, bool end);
std::wstring StylePrefix(uint8_t style);
EditResult NewPhantom(const EditCtx& c, const EditState& st, PhantomKind k, int32_t b, int depth, uint32_t at, bool in);
EditResult After(const EditCtx& c, const EditState& st, int32_t b);
EditResult Carry(EditResult r, bool always = false);
// the check after the re-parse of a structural step (EditResult::Shape, Phase 4 notes)
void SetShape(EditResult& r, int32_t b0, int32_t b1, int32_t delta, bool same);
// the line starting at l belongs to a paragraph (not a heading): text under or over it would run into it
bool ParaLine(const Doc& d, uint32_t l);
int32_t OwnerOf(const Doc& d, uint32_t l);
bool OpensContainer(const Doc& d, int32_t b);
// Block b made a paragraph (a heading's markers gone, §7.7 / §8.4) would run into the paragraph line right above it -
// or, below its last line (ending at lastEnd), into the one under it: a blank line keeps each apart (Phase 4 notes)
bool RunsIntoAbove(const Doc& d, const std::wstring& src, int32_t b);
bool RunsIntoBelow(const Doc& d, const std::wstring& src, uint32_t lastEnd);

// ---- §7.4 escaping, §7.5 / §7.9 the delimiters written back where a cut goes through spans
int Trigger(const std::wstring& l, bool first, uint32_t* drop);
bool HardLine(const std::wstring& src, uint32_t l, uint32_t le);
void Escape(EditResult& r, const std::wstring& l, uint32_t at, bool first);
uint32_t LineContent(const Doc& d, const std::wstring& src, int32_t b, uint32_t t, bool* first);
struct Delim { uint32_t at; uint8_t type; std::wstring text; };
struct Bal { std::vector<Delim> close, open; bool torn = false; };
Bal Balance(const EditCtx& c, const TextPos& p, uint32_t sA, uint32_t sB, bool reopen);
std::wstring Joined(const std::vector<Delim>& v);
EditResult DropLines(const EditCtx& c, const EditState& st, int32_t b, uint32_t a, uint32_t e);
Splice WholeLines(const Doc& d, const std::wstring& src, int32_t b);
EditResult CutRange(const EditCtx& c, const EditState& st, std::wstring ins, EditKind kind);
// an edit of [s0, s1) inside a collapsed or shortcut reference link's text: its closer names the old label (Phase 4)
void RefCloser(const EditCtx& c, const TextPos& p, TRange rg, uint32_t s0, uint32_t s1, std::vector<Splice>& out);
// text at the caret: typed (with §7.3's transforms) or put in as it is (pasted, inserted); over a selection it replaces it
EditResult Insert(const EditCtx& c, const EditState& st, std::wstring_view text, bool typing, EditKind kind);

// ---- lists (§7.6-§7.8)
std::wstring NewMarker(const Doc& d, const std::wstring& src, int32_t item, bool next);
int32_t SiblingOf(const Doc& d, int32_t b, int32_t item);
int32_t LastOf(const Doc& d, int32_t item);
void Shift(const std::wstring& src, uint32_t a, uint32_t e, uint32_t col, int delta, std::vector<Splice>& out);
void SortDown(std::vector<Splice>& v);
void Coalesce(std::vector<Splice>& v);

// ---- editops.cpp, called from typing (§7.3, §8.2): text typed with a pending format - materialised at the first
// non-blank character, or split out of the span it turns off
EditResult TypePending(const EditCtx& c, const EditState& st, std::wstring_view text);
// the enterable spans a blank typed at text position p (their end) leaves behind: they stay "sticky" (§7.3), and the
// next non-blank character extends them over the blanks
uint16_t StickyAt(const Doc& d, const TextPos& p, uint32_t caret);  // caret: its source offset (past a closer: none)
}  // namespace ec
