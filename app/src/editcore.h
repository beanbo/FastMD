// Edit mode's core (docs/EDIT-MODE.md §3.2): the text <-> source map queries, the map's self-check, block diffing,
// line ends and container prefixes - and, from later phases, the editing operations and the undo stack.
//
// Window-free and pure: everything here works on a const Doc parsed with ParseOptions::wantMap and the source it was
// parsed from, never touches the application state `g`, and allocates no Win32 object. That is what lets
// fastmd-edit-tests (tests/edit) and the fuzzer drive it without a window.
//
// Phase 1a implements the map queries (CaretStop, SrcOfText, TextOfSrc), MapSelfCheck, DiffBlocks, ContPrefix,
// BlankPrefix and LineEol; phase 1c the undo stack and the splice helpers; 2a typing and deleting inside a block; 2b
// the structure: Enter, Backspace and Delete across blocks, Tab, selections, the phantom rows, escaping, cut and plain
// paste. The operations are declared in their final shape and arrive with the phases that use them (2a-3b).
#pragma once
#include "doc.h"

#include <string_view>

enum EditKind : uint8_t { EK_TYPE, EK_DEL_BACK, EK_DEL_FWD, EK_STRUCT, EK_FORMAT, EK_PASTE, EK_CUT, EK_POPUP,
                          EK_TASK, EK_ADOPT, EK_DISCARD, EK_OTHER };
struct Splice { uint32_t at; std::wstring removed, inserted; };        // src[at, at+removed.size()) := inserted
// A text position: an offset in Doc::text plus the block (and, in a table, the cell) it belongs to. The block matters
// because an empty block shares its offset with the next one, and a block's end is the next block's start.
struct TextPos { uint32_t t; int32_t block; int32_t cell = -1; };
enum PhantomKind : uint8_t { PH_NONE, PH_AFTER, PH_BEFORE, PH_BREAK };
struct Phantom {
    PhantomKind kind = PH_NONE;
    uint32_t anchorSrc = 0;            // PH_AFTER: anchor's outerEnd; PH_BEFORE: anchor line + container prefix; PH_BREAK: content end
    int32_t anchorBlock = -1;          // resolved after every re-parse from anchorSrc
    std::wstring prefix, blankPrefix;  // ContPrefix / BlankPrefix the materialised text gets
    uint8_t depth = 0;                 // container depth (Enter/Backspace in an empty phantom pop one level)
    uint8_t style = 0;                 // 0 paragraph, 1..6 heading, 7 bullet, 8 numbered, 9 task, 10 quote (§8.1)
    // The caret stands in the phantom row (focus = anchor = anchorSrc). A phantom before a block made by Enter at the
    // block's start keeps the caret in the block: ↑ goes into the row (§6.7, UX-7), so this cannot be told from the
    // offsets alone.
    uint8_t in = 0;
};
enum Fmt : uint16_t { FMT_BOLD = 1, FMT_ITALIC = 2, FMT_STRIKE = 4, FMT_CODE = 8, FMT_LINK = 16 };
// EditState::pendOn: the formats came from a blank typed at the end of their spans (§7.3's sticky end) - the next
// non-blank character extends those spans over the blanks instead of starting new ones
constexpr uint16_t FMT_STICKY = 0x100;
struct EditState {
    uint32_t focus = 0, anchor = 0;    // source offsets; anchor == focus → collapsed (THE caret: survives re-parses)
    int8_t lineAff = 0;                // visual affinity at a soft-wrap boundary: -1 end of the upper line, +1 lower start
    Phantom phantom;
    uint16_t pendOn = 0, pendOff = 0;  // pending format for the next typed text (§7.3)
    int32_t atom = -1;                 // selected object atom id (§6.2) or -1
    uint32_t burstBeg = UINT32_MAX;    // deferred typing burst (§5.8)
    float wantX = -1;                  // column for vertical moves
};
using ClusterFn = uint32_t (*)(const std::wstring& text, uint32_t pos, int dir, void* ctx);
struct EditCtx {
    const Doc& doc; const std::wstring& src;
    const wchar_t* eol;                // g.eol: L"\n", L"\r\n" or L"\r"
    ClusterFn clusters; void* clusterCtx;  // app: IDWriteTextLayout::GetClusterMetrics; tests: a grapheme-lite table
    uint64_t nowMs;
    // Where the caret and the anchor are drawn, when the caller knows (block -1: found from the source offsets). An
    // empty block or cell shares its text offset with the next one, so the source offset alone can be ambiguous.
    TextPos focusPos{0, -1, -1}, anchorPos{0, -1, -1};
    uint16_t trail = 0;                // columns of trailing blanks the caret stands in (§6.5)
};
// the next cluster boundary from pos in dir inside [lo, hi] (surrogate pairs whole when there is no ClusterFn)
uint32_t ClusterStep(const EditCtx&, uint32_t pos, int dir, uint32_t lo, uint32_t hi);
// A test / app helper for ClusterFn: surrogate pairs, combining marks, variation selectors, ZWJ sequences and
// regional-indicator pairs stay whole (a grapheme-lite table, §6.2).
uint32_t GraphemeLite(const std::wstring& text, uint32_t pos, int dir, void* ctx);
struct EditResult {
    std::vector<Splice> splices;       // applied in order
    EditState after;
    EditKind kind = EK_OTHER;
    // post-reparse checks of emitted delimiters (§7.5 step 5): every character of the new text [tBeg, tEnd) but blanks
    // has (present) or lacks the format fmt (FMT_*); empty = none
    struct Expect { uint32_t tBeg, tEnd; uint16_t fmt; bool present; };
    std::vector<Expect> verify;
    // The check the glue runs after the re-parse when an operation wrote delimiters back (a split's closers and openers,
    // a cut's rebalancing, §7.5 step 5): the rendered text must be the old one with [t0, t1) replaced by `text`, and the
    // characters beside the change must keep their formatting. When it fails the step is taken back and refused.
    struct Keep { bool on = false; uint32_t t0 = 0, t1 = 0; std::wstring text; } keep;
    // a formatting command keeps the text: its selection is found again by text position on the new document, from
    // inside the delimiters at its start (MAP_INNER_START) to before those at its end (MAP_INNER_END); block -1 = none
    TextPos selA{0, -1, -1}, selB{0, -1, -1};
    std::string refused;               // non-empty: nothing applied, the glue shows the toast named here
};
bool Kept(const Doc& oldD, const Doc& newD, const EditResult::Keep&);
// Kept, and every one of the result's format expectations: what the glue (and the golden runner) checks after the
// re-parse of a step that wrote delimiters; false = take it back
bool Verified(const Doc& oldD, const Doc& newD, const EditResult&);
// the character at text offset t has the format (FMT_*) as the markup gives it: inside a span of it (§8.2)
bool HasFormat(const Doc&, uint32_t t, uint16_t fmt);

// ---- mapping (§6)
// Is pos a place the caret may stand? Not inside an atom or a synthesized piece, not in a hidden or synthesized block;
// a table position must name its cell. Clusters are the caller's business (ClusterFn).
bool     CaretStop(const Doc&, const TextPos&);
enum MapMode { MAP_CARET, MAP_OUTER_START, MAP_OUTER_END, MAP_INNER_START, MAP_INNER_END };
// UINT32_MAX when there is nothing to map: no map, no such block or cell, a synthesized block. An object atom (a
// picture block, an HTML block, front matter) maps to its first line (MAP_OUTER_END: the end of its last line).
uint32_t SrcOfText(const Doc&, const std::wstring& src, const TextPos&, MapMode);
// dir: which way to go from a source position that has no text of its own (a delimiter, a prefix, a gap between
// blocks): -1 to the text boundary before it, +1 to the one after. trailCols (may be null): columns of trailing blanks
// between the line's last text and s (§6.5), 0 otherwise.
TextPos  TextOfSrc(const Doc&, const std::wstring& src, uint32_t s, int dir, uint16_t* trailCols);
bool     MapSelfCheck(const Doc&, const std::wstring& src, std::string* why);   // §4.5, pure: links into fastmd-fuzz
struct BlockDiff { uint32_t p, q; };
BlockDiff DiffBlocks(const Doc& oldD, const Doc& newD);                     // §5.5 step 5
std::wstring ContPrefix(const Doc&, const std::wstring& src, int block);
std::wstring BlankPrefix(const Doc&, const std::wstring& src, int block);
const wchar_t* LineEol(const std::wstring& src, uint32_t s, const wchar_t* fallback);

// ---- operations (§7, §8): pure; the glue applies the splices, re-parses and verifies
EditResult OpType(const EditCtx&, const EditState&, std::wstring_view text);
EditResult OpBackspace(const EditCtx&, const EditState&, bool word);
EditResult OpDelete(const EditCtx&, const EditState&, bool word);
EditResult OpEnter(const EditCtx&, const EditState&, int variant /* 0 Enter, 1 Shift+Enter, 2 Ctrl+Enter */);
EditResult OpTab(const EditCtx&, const EditState&, bool shift);
EditResult OpDeleteSelection(const EditCtx&, const EditState&);
EditResult OpReplaceSelection(const EditCtx&, const EditState&, std::wstring_view text);
EditResult OpPaste(const EditCtx&, const EditState&, std::wstring_view text, bool privateFormat);
std::wstring BalancedSlice(const EditCtx&, const EditState&);             // §7.11
EditResult OpToggleInline(const EditCtx&, const EditState&, uint16_t fmt);
EditResult OpTypePlain(const EditCtx&, const EditState&, std::wstring_view text);  // at the caret, pending formats kept
EditResult OpLink(const EditCtx&, const EditState&, std::wstring_view url, bool confirmRefDef);
EditResult OpLinkRemove(const EditCtx&, const EditState&);
EditResult OpBlockStyle(const EditCtx&, const EditState&, int level /* 0 paragraph, 1..6 */);
EditResult OpList(const EditCtx&, const EditState&, int kind /* 7 bullet, 8 numbered, 9 task */);
EditResult OpQuote(const EditCtx&, const EditState&);
EditResult OpCodeBlock(const EditCtx&, const EditState&);
EditResult OpCodeLang(const EditCtx&, const EditState&, std::wstring_view info);
EditResult OpInsertTable(const EditCtx&, const EditState&, int rows, int cols);
EditResult OpInsertFormula(const EditCtx&, const EditState&, bool block);
EditResult OpInsertDiagram(const EditCtx&, const EditState&, int tmpl);
EditResult OpInsertImage(const EditCtx&, const EditState&, std::wstring_view dest, std::wstring_view alt);
EditResult OpInsertHr(const EditCtx&, const EditState&);
EditResult OpTable(const EditCtx&, const EditState&, int op /* CMD_TABLE_* − CMD_TABLE_ROW_ABOVE */);
EditResult OpTaskToggle(const EditCtx&, const EditState&, int task);
EditResult OpAtomSource(const EditCtx&, const EditState&, int atom, int field, std::wstring_view text);

// ---- typing's check (§7.3 step 5): an ordinary character (none of the Markdown-significant ones) must render as
// itself at the caret, and its neighbours must keep their formatting. When it does not, the glue tries the other places
// TypeFallbacks names, in order; if none renders right, the first splice stays (typing is never blocked).
bool NeedsTypeCheck(std::wstring_view text);
bool TypedOk(const Doc& oldD, uint32_t t, const Doc& newD, std::wstring_view rendered);
// text put in at `at`; `also`: splices after that one, on the source it left (F9-2's `_` → `*`)
struct TypeCandidate { uint32_t at; std::wstring text; uint32_t caret; std::wstring rendered; std::vector<Splice> also; };
std::vector<TypeCandidate> TypeFallbacks(const EditCtx&, const EditState&, uint32_t s, std::wstring_view text);
// object atoms (§6.2): ids are the image index for one in a line, 0x40000000 | block for a block of its own
constexpr int32_t kAtomBlock = 0x40000000;
int32_t AtomOfBlock(const Doc&, int32_t block);   // the atom id of a block atom (an HTML block's first block), -1 = none
int32_t AtomBlockOf(const Doc&, int32_t atom);    // the block that draws an atom (inline: the block holding it)
bool IsAtomBlock(const Doc&, int32_t block);
// phantoms (§6.7): the block a phantom stands next to (-1: none), found again from its anchorSrc after every re-parse;
// whether it lives on with the caret where it is (in the row, or in that block)
int32_t PhantomBlock(const Doc&, const std::wstring& src, const Phantom&);
bool PhantomAlive(const Doc&, const std::wstring& src, const EditState&, int32_t caretBlock);
// a new phantom next to a block, the caret in it (the document's edges next to code, a table or an object, a click below
// the last block, §6.8): inside the block's `depth` outermost containers (< 0: all of them)
EditResult OpPhantom(const EditCtx&, const EditState&, int32_t block, bool before, int depth);
// A source offset moved by splices applied in order (the caret, a phantom's anchor, a raw-while-typing range): after
// an insertion at it when `after`, else before it; inside removed text, to where the replacement starts.
uint32_t MapThrough(const std::vector<Splice>&, uint32_t pos, bool after);

// ---- undo (§11)
struct EditStep { std::vector<Splice> splices; EditState before, after; EditKind kind = EK_OTHER; uint64_t t0 = 0, t1 = 0; };
// The history of one document session. A step is merged into the one before it while the same kind of typing or
// deleting goes on at the caret it left, less than 1.5 s apart and with nothing in between (BreakCoalescing); a word
// typed after a blank starts a step of its own. Time comes in from the caller (the tests inject it).
class UndoStack {
public:
    void Push(EditStep, uint64_t nowMs);
    void BreakCoalescing() { broken_ = true; }
    const EditStep* PeekUndo() const { return undo_.empty() ? nullptr : &undo_.back(); }
    const EditStep* PeekRedo() const { return redo_.empty() ? nullptr : &redo_.back(); }
    void DidUndo();                    // the top step was undone: it moves to the redo stack
    void DidRedo();
    void Clear();
    size_t Depth() const { return undo_.size(); }
    size_t RedoDepth() const { return redo_.size(); }
    static constexpr size_t kMaxSteps = 1000, kMaxBytes = 32u << 20;  // the oldest steps go beyond either
private:
    std::vector<EditStep> undo_, redo_;
    size_t bytes_ = 0;                 // text held by the undo and redo steps
    bool broken_ = true;
    void Trim();
};
// Applies splices in order (inverse: their inverses in reverse order), each only if the text it replaces is there; a
// mismatch changes nothing and says where (the undo history no longer fits the text, §11).
bool ApplySplices(std::wstring& src, const std::vector<Splice>&, bool inverse, std::string* why);
// A splice may not cut a surrogate pair or a CRLF in two (§7.1).
bool SpliceSplits(const std::wstring& src, uint32_t at, uint32_t len);

// ---- keyboard (§12.5)
unsigned EditChord(unsigned vk, bool ctrl, bool shift, bool alt);  // → CMD id or 0; ctrl && alt → always 0
