// Edit mode's core (docs/EDIT-MODE.md §3.2): the text <-> source map queries, the map's self-check, block diffing,
// line ends and container prefixes - and, from later phases, the editing operations and the undo stack.
//
// Window-free and pure: everything here works on a const Doc parsed with ParseOptions::wantMap and the source it was
// parsed from, never touches the application state `g`, and allocates no Win32 object. That is what lets
// fastmd-edit-tests (tests/edit) and the fuzzer drive it without a window.
//
// Phase 1a implements the map queries (CaretStop, SrcOfText, TextOfSrc), MapSelfCheck, DiffBlocks, ContPrefix,
// BlankPrefix and LineEol. The operations and the undo stack are declared in their final shape and arrive with the
// phases that use them (1c, 2a-3b).
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
};
enum Fmt : uint16_t { FMT_BOLD = 1, FMT_ITALIC = 2, FMT_STRIKE = 4, FMT_CODE = 8, FMT_LINK = 16 };
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
};
struct EditResult {
    std::vector<Splice> splices;       // applied in order
    EditState after;
    EditKind kind = EK_OTHER;
    struct Expect { uint32_t sBeg, sEnd; uint16_t fmt; bool present; };
    std::vector<Expect> verify;        // post-reparse checks of emitted delimiters (§7.5); empty = none
    std::string refused;               // non-empty: nothing applied, the glue shows the toast named here
};

// ---- mapping (§6)
// Is pos a place the caret may stand? Not inside an atom or a synthesized piece, not in a hidden or synthesized block;
// a table position must name its cell. Clusters are the caller's business (ClusterFn).
bool     CaretStop(const Doc&, const TextPos&);
enum MapMode { MAP_CARET, MAP_OUTER_START, MAP_OUTER_END, MAP_INNER_START };
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

// ---- undo (§11)
struct EditStep { std::vector<Splice> splices; EditState before, after; EditKind kind; uint64_t t0, t1; };
class UndoStack { public: void Push(EditStep, uint64_t nowMs); void BreakCoalescing(); const EditStep* PeekUndo() const;
                  const EditStep* PeekRedo() const; void DidUndo(); void DidRedo(); void Clear();
                  size_t Depth() const; size_t RedoDepth() const; };
bool ApplySplices(std::wstring& src, const std::vector<Splice>&, bool inverse, std::string* why);  // verifies first

// ---- keyboard (§12.5)
unsigned EditChord(unsigned vk, bool ctrl, bool shift, bool alt);  // → CMD id or 0; ctrl && alt → always 0
