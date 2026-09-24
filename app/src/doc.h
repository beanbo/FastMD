// Document model: a flat list of blocks over one concatenated UTF-16 text buffer. All text offsets (blocks,
// runs, table cells) are absolute offsets into Doc::text, in document order — so a text position is one
// uint32_t and a selection is a [start, end) range of it.
#pragma once
#include "common.h"
#include "theme.h"
#include <memory>

enum RunFlags : uint16_t {
    F_BOLD = 1, F_ITALIC = 2, F_CODE = 4, F_STRIKE = 8, F_LINK = 16, F_ICON = 32,
    F_KBD = 64,     // <kbd>: a key in a frame
    F_SUP = 128,    // <sup> / <sub>: smaller and lifted or dropped off the baseline
    F_SUB = 256,
    F_IMAGE = 512,  // one U+FFFC standing for a picture inside the line (HTML <img>: badges, icons)
};

struct Run {             // inline style run; offsets are absolute in Doc::text
    uint32_t start, len;
    uint16_t flags;
    uint8_t color;       // Pal, P_DEFAULT = inherit
    uint8_t _pad;
    uint32_t link;       // index into Doc::links when F_LINK
    uint32_t image;      // index into Doc::images when F_IMAGE
};

enum BlockKind : uint8_t { BK_TEXT, BK_CODE, BK_HR, BK_TABLE, BK_IMAGE };
enum Marker : uint8_t { MK_NONE, MK_BULLET, MK_NUMBER, MK_TASK_OPEN, MK_TASK_DONE };
enum Alert : uint8_t { AL_NONE, AL_NOTE, AL_TIP, AL_IMPORTANT, AL_WARNING, AL_CAUTION };

struct Block {
    uint8_t kind;        // BlockKind
    uint8_t heading;     // 0 = paragraph, 1..6
    uint8_t marker;      // Marker (list item marker attached to this, the item's first leaf)
    uint8_t listLevel;   // nesting depth of the list the marker belongs to (1 = top level)
    uint8_t muted;       // blockquote / h6 → muted text colour
    uint8_t lang;        // code language id (highlighter)
    uint8_t alertTitle;  // Alert: this block is the title line of a GitHub alert
    uint8_t align;       // 0 = left, 1 = centre, 2 = right (<p align=…>, <div align=…>)
    float indent;        // x offset from the column's content box (DIP)
    float gap;           // collapsed vertical margin above the block (DIP)
    uint32_t textOff, textLen;
    uint32_t runOff, runCount;
    uint32_t number;     // ordered list number
    uint32_t aux;        // table index / image index
    uint16_t details;    // <details>: 0 = none, else group + 1; bit 15 marks the <summary> line
    uint16_t _pad2;
};

struct Cell { uint32_t textOff, textLen, runOff, runCount; };
struct Table {
    uint32_t cols, rows;     // rows include the header row (row 0)
    uint32_t cellOff;        // Doc::cells[cellOff + r*cols + c]
    uint32_t alignOff;       // Doc::aligns[alignOff + c]: 0 default, 1 left, 2 center, 3 right
};
// Decoded pixels never change once they are published: every Image that shows the same picture points at one Pixels,
// and a worker that reads them (the scaler) holds its own reference, so replacing a picture on the UI thread can never
// free what a worker is still reading (EDIT-MODE.md §5.2, §5.3).
struct Pixels {
    std::vector<uint32_t> px;   // premultiplied BGRA
    int pxW = 0, pxH = 0;       // size of px as decoded (a GIF frame can be smaller than its header's screen)
    std::vector<uint8_t> svg;   // the SVG source, kept so the picture can be redrawn crisply at any size
    uint32_t serial = 0;        // NewPixelSerial(): a display-size copy is only ever used with the pixels it came from
};
// the same picture at its display size, made by the scaler from the Pixels with this serial (px empty = that size
// could not be made, and is not tried again)
struct Scaled { std::vector<uint32_t> px; int w = 0, h = 0; uint32_t serial = 0; };
enum RenderState : uint8_t { RS_NONE = 0, RS_PENDING = 1, RS_OK = 2, RS_FAILED = 3 };

struct Image {
    std::wstring path;          // absolute local path ("" = not fetched yet / unsupported)
    std::wstring url;           // https:// source, downloaded into the cache after the first frame
    int w = -1, h = -1;         // pixel size from the file header (-1 = unknown yet, 0 = failed) — layout
    int attrW = 0, attrH = 0;   // size asked for by HTML width / height attributes (0 = not given)
    std::wstring alt;           // alt text, shown in the placeholder of a picture that stands on its own line
    // A formula or a diagram is a picture whose source is text: fastmd-tex.dll / fastmd-mermaid.dll turn it into SVG
    // on a worker thread after the first frame, and from there it is an ordinary vector picture (plan 4.1, 4.2).
    std::string math;           // the TeX or Mermaid source, UTF-8 (empty = an ordinary picture)
    uint8_t mathKind = 0;       // 0 = picture, 1 = formula in the line, 2 = formula of its own, 3 = Mermaid diagram
    float ascent = 0;           // formula in the line: how much of its height stands above the text baseline
    // Where the picture is in the source (maps only, UINT32_MAX = unknown): outer with its delimiters or fences; src is
    // the TeX between the dollars, the Mermaid content lines, or a picture's destination; alt a picture's alt text.
    uint32_t outerBeg = UINT32_MAX, outerEnd = UINT32_MAX, srcBeg = UINT32_MAX, srcEnd = UINT32_MAX;
    uint32_t altBeg = UINT32_MAX, altEnd = UINT32_MAX;
    // What is drawn, UI thread only: the window fills these from its render table (loader.cpp), the preview pane
    // while it loads. Pixels are shared, so a copy of an Image keeps its picture.
    std::shared_ptr<const Pixels> pix;
    // the picture at its display size: drawn as a row copy, and scaled with a real filter instead of the
    // nearest-neighbour fallback. Made in the background (loader.cpp) for the size the canvas asks for.
    std::shared_ptr<const Scaled> sc;
    uint8_t state = RS_NONE;    // RenderState
    bool renderFailed = false;  // the pixels are the last good picture of a source that no longer renders
    std::wstring pxFor;         // render-table key the pixels were made from
    int wantW = 0, wantH = 0;   // display size the canvas last drew at (0 = the decoded size fits)
};
uint32_t NewPixelSerial();      // layout.cpp: a fresh Pixels::serial (any thread)
struct QuoteSpan { float x; uint32_t first, last; uint8_t alert; };
struct Heading { uint32_t block; uint8_t level; std::wstring slug; };
struct Anchor { std::wstring slug; uint32_t block; };  // #target that is not a heading (footnotes)
struct Task { uint32_t block, src; };  // task list item: the block that draws its box, the mark's offset in the source

// ---- the text <-> source map of edit mode (docs/EDIT-MODE.md §4.2). Built only when ParseOptions::wantMap asks for
// it; every offset is into the source handed to ParseMarkdown.
// A segment says where a piece of the rendered text came from. PLAIN text is the source character for character; an
// atom is indivisible (the caret steps over it, one Backspace removes it); SYNTH text has no source at all.
enum SegKind : uint8_t { SEG_PLAIN = 0,  // tLen == sLen, char for char (text, code, table cells)
                         SEG_TEXTATOM,   // entity, emoji, escape, soft/hard break, <br>, NUL, footnote ref, code-indent tab
                         SEG_OBJATOM,    // inline image, inline formula, HTML <img> (one U+FFFC)
                         SEG_SYNTH };    // synthesized text with no source (the footnote " ↩"): never a caret stop
enum SegFlags : uint8_t { SEGF_SPLITTAB = 1 };  // a tab only part of which is code indentation (the rest is a container's)
struct SrcSeg { uint32_t t, tLen, s, sLen; uint8_t kind; uint8_t flags; };
enum BlockSrcFlags : uint16_t {
    BS_RAW = 1,        // HTML block / front matter: an object atom edited in a popup
    BS_SETEXT = 2, BS_ATX = 4, BS_FENCED = 8, BS_UNCLOSED = 16,  // heading and fence shapes
    BS_SYNTH = 32,     // alert title, footnote-section HR: no source, not a caret stop
    BS_FOOTNOTE = 64,  // footnote definition text (rendered far from its source)
    BS_EMPTYITEM = 128,// synthesized leaf of an empty list item
    BS_OBJECT = 256,   // BK_IMAGE block or HR: an object atom
    BS_NOCONTENT = 512,// fenced block with no content line at all
    BS_RAWTEXT = 1024, // raw-while-typing leaf (§6.9)
    BS_FRONT = 2048, BS_HTML = 4096 };
struct BlockSrc {
    uint32_t beg = 0, end = 0;   // content extent (after prefixes and markers; ATX: before the closing sequence)
    uint32_t line = 0;           // start of the block's first source line (container prefix included)
    uint32_t lineEnd = 0;        // end of the block's last content line before its EOL, trailing blanks included
    uint32_t outerEnd = 0;       // end of the last line that belongs to the block: closing fence / setext underline;
                                 // = lineEnd otherwise; unclosed fence = last content line end
    uint32_t segOff = 0, segCount = 0, spanOff = 0, spanCount = 0;  // this block's slices of segs / spans
    int32_t container = -1;      // innermost container (Doc::containers), -1 = top level
    int32_t rawId = -1;          // BS_RAW: index of the first block made from the same HTML block / front matter
    uint32_t aux = UINT32_MAX;   // BS_FENCED: end of the opening fence line; BS_FOOTNOTE: def_beg
    uint16_t flags = 0;
};
enum ContainerKind : uint8_t { CT_QUOTE, CT_ALERT, CT_ITEM, CT_FOOTNOTE };
struct ContainerSrc {
    ContainerKind kind = CT_QUOTE;
    int32_t parent = -1;                      // parent container or -1
    uint32_t firstBlock = 0, lastBlock = 0;   // lastBlock < firstBlock: the container holds no block
    uint32_t markOff = UINT32_MAX;            // CT_ITEM: marker's first char; CT_FOOTNOTE: def_beg
    uint8_t markLen = 0;                      // "-" 1, "12." 3
    wchar_t bullet = 0, delim = 0;            // '-' '+' '*' / '.' ')'
    uint32_t number = 0;                      // ordered: the number as written
    uint16_t contentCol = 0;                  // CT_ITEM: absolute column of the content (tab stops of 4)
    uint32_t taskOff = UINT32_MAX;            // task mark offset (the char between [ ])
    bool tight = true;
};
// SpanSrc::type: an MD_SPANTYPE for EM, STRONG, A, IMG, CODE, DEL, LATEXMATH(_DISPLAY), FOOTNOTE_REF, or an inline
// HTML tag pair (a pseudo-span) of one of these classes
enum SpanSrcType : uint8_t {
    ST_HTML_B = 0x80, ST_HTML_I, ST_HTML_CODE, ST_HTML_S, ST_HTML_KBD, ST_HTML_SUP, ST_HTML_SUB, ST_HTML_A };
enum SpanFlags : uint8_t { SF_AUTOLINK = 1, SF_REF = 2, SF_UNCLOSED = 4, SF_ENTERABLE = 8, SF_UNDERSCORE = 16 };
struct SpanSrc { int32_t block; uint32_t tBeg, tEnd; uint32_t openBeg, openEnd, closeBeg, closeEnd;
                 uint8_t type, flags; };
struct CellSrc { uint32_t beg, end; bool missing; };
struct RowSrc { uint32_t lineStart, contentStart, lineEnd; std::vector<uint32_t> pipes; };  // pipes: offsets of '|'
struct TableSrc { std::vector<RowSrc> rows; };  // rows[1] is the delimiter row; empty for HTML and front-matter tables

struct Doc {
    std::wstring text;             // concatenated rendered text of all blocks
    std::vector<Run> runs;
    std::vector<Block> blocks;
    std::vector<Table> tables;
    std::vector<Cell> cells;
    std::vector<uint8_t> aligns;
    std::vector<Image> images;
    std::vector<QuoteSpan> quotes;
    std::vector<std::wstring> links;
    std::vector<Heading> headings;
    std::vector<Anchor> anchors;
    std::vector<Task> tasks;       // sorted by block: a click on a box ticks the item in the file (tasks.cpp)
    // text position → position in the Markdown source, one entry per chunk md4c handed over (sorted by first):
    // "copy as Markdown" gives back the author's own source rather than something rebuilt from the model
    std::vector<std::pair<uint32_t, uint32_t>> srcMap;
    std::vector<std::wstring> langNames;  // as typed after the fence; Block::aux of a code block is the index + 1
    std::vector<uint8_t> detailsOpen;  // one per <details> group: is it unfolded right now
    bool themed = false;               // holds a <picture> that depends on the colour theme
    std::wstring baseDir;          // directory of the .md file (with trailing backslash)

    // edit mode's map (§4.2), filled only when ParseOptions::wantMap asked for it (srcMap is left empty then)
    std::vector<SrcSeg> segs;            // text order; each block's segments are contiguous (BlockSrc::segOff)
    std::vector<BlockSrc> blockSrc;      // parallel to blocks
    std::vector<uint32_t> blockOrder;    // block indices sorted by blockSrc.line (BS_SYNTH blocks left out)
    std::vector<SpanSrc> spans;          // per block, in opener order (BlockSrc::spanOff)
    std::vector<CellSrc> cellSrc;        // parallel to cells
    std::vector<TableSrc> tableSrc;      // parallel to tables
    std::vector<ContainerSrc> containers;
    bool hasMap = false;
};

// parse.cpp
// Startup, the preview pane, the full parse of a big document and every reading-mode parse pass no options: no md4c
// hooks, no map vectors, the same cost as ever. Edit mode asks for the map (§4.3).
struct ParseOptions {
    bool wantMap = false;                                             // build Doc's map and install the md4c hooks
    const std::vector<uint32_t>* masks = nullptr;                     // offsets parsed as U+E000 (§6.9, Phase 2b)
    const std::vector<std::pair<uint32_t, uint32_t>>* raw = nullptr;  // source ranges built as raw-text leaves (§6.9)
};
bool ParseMarkdown(Doc& d, const wchar_t* src, size_t n, const ParseOptions* opt = nullptr);  // md4c on UTF-16
// Safe cut for a first-screen prefix parse: start of a top-level ATX heading preceded by a blank line, outside fenced
// code, at or after minChars. Returns n when there is none.
size_t FindPrefixCut(const wchar_t* s, size_t n, size_t minChars);
std::wstring GithubSlug(const wchar_t* s, size_t n);
uint32_t AlertColor(uint8_t alert);  // 0xRRGGBB for the current palette

// highlight.cpp
void Highlight(Doc& d, const wchar_t* lang, uint32_t langLen, uint32_t textOff, uint32_t textLen, uint8_t& langId);

// util.cpp
// How a file's bytes became the UTF-16 text: a byte-order mark (header bytes) and a code page — CP_UTF8, the ANSI code
// page's number (AnsiCodePage()) for bytes that are not UTF-8, or 1200 for UTF-16 LE. Edits are written back in it.
struct TextEncoding { uint32_t header = 0; UINT cp = CP_UTF8; };
UINT AnsiCodePage();                    // GetACP(), or FASTMD_ACP (tests); read once
void SetAnsiCodePageForTests(UINT cp);  // the unit tests switch it per case
void DecodeText(const char* p, int len, std::wstring& out, TextEncoding* enc);
// acp: the code page for bytes that are not UTF-8 (0 = AnsiCodePage())
void DecodeTextCp(const char* p, int len, UINT acp, std::wstring& out, TextEncoding* enc);
// what the read learned besides the text, all from the handle it read through (edit mode's baseline, §10.1)
struct DiskBytes { TextEncoding enc; uint32_t volume = 0; uint64_t index = 0; FILETIME mtime{}; uint64_t size = 0;
                   DWORD attributes = 0; };
bool ReadFileUtf16(const wchar_t* path, std::wstring& out, uint64_t* ticksRead, FILETIME* writeTime,
                   DiskBytes* info = nullptr);
bool ReadFileBytes(const wchar_t* path, std::vector<uint8_t>& out, size_t maxBytes);
bool GetFileStamp(const wchar_t* path, FILETIME* writeTime, uint64_t* size);
