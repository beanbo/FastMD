// Document model: a flat list of blocks over one concatenated UTF-16 text buffer. All text offsets (blocks,
// runs, table cells) are absolute offsets into Doc::text, in document order — so a text position is one
// uint32_t and a selection is a [start, end) range of it.
#pragma once
#include "common.h"
#include "theme.h"

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
struct Image {
    std::wstring path;          // absolute local path ("" = not fetched yet / unsupported)
    std::wstring url;           // https:// source, downloaded into the cache after the first frame
    int w = -1, h = -1;         // pixel size from the file header (-1 = unknown yet, 0 = failed) — layout
    int attrW = 0, attrH = 0;   // size asked for by HTML width / height attributes (0 = not given)
    int canon = -1;             // index of the first image with the same path (holds the pixels)
    std::vector<uint32_t> px;   // decoded premultiplied BGRA (canonical entry only), filled by the image thread
    std::vector<uint8_t> svg;   // the SVG source, kept so the picture can be redrawn crisply at any size
    std::wstring alt;           // alt text, shown in the placeholder of a picture that stands on its own line
    int pxW = 0, pxH = 0;       // size of px as decoded (a GIF frame can be smaller than its header's screen)
    std::atomic<int> state{0};  // 0 = not requested, 1 = loading, 2 = ready, 3 = failed
    // the same picture at its display size: drawn as a row copy, and scaled with a real filter instead of the
    // nearest-neighbour fallback. Made in the background (loader.cpp) for the size the canvas asks for.
    std::vector<uint32_t> sc;             // UI thread only
    std::atomic<int> scW{0}, scH{0};      // size of sc (0 = none yet)
    std::atomic<int> wantW{0}, wantH{0};  // display size the canvas last drew at (0 = the decoded size fits)
    Image() = default;
    Image(const Image& o) : path(o.path), url(o.url), w(o.w), h(o.h), attrW(o.attrW), attrH(o.attrH) {}
    Image& operator=(const Image& o) {
        path = o.path; url = o.url; w = o.w; h = o.h; attrW = o.attrW; attrH = o.attrH; canon = -1;
        px.clear(); pxW = pxH = 0; state = 0;
        sc.clear(); scW = 0; scH = 0; wantW = 0; wantH = 0;
        return *this;
    }
};
struct QuoteSpan { float x; uint32_t first, last; uint8_t alert; };
struct Heading { uint32_t block; uint8_t level; std::wstring slug; };
struct Anchor { std::wstring slug; uint32_t block; };  // #target that is not a heading (footnotes)

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
    // text position → position in the Markdown source, one entry per chunk md4c handed over (sorted by first):
    // "copy as Markdown" gives back the author's own source rather than something rebuilt from the model
    std::vector<std::pair<uint32_t, uint32_t>> srcMap;
    std::vector<std::wstring> langNames;  // as typed after the fence; Block::aux of a code block is the index + 1
    std::vector<uint8_t> detailsOpen;  // one per <details> group: is it unfolded right now
    bool themed = false;               // holds a <picture> that depends on the colour theme
    std::wstring baseDir;          // directory of the .md file (with trailing backslash)
};

// parse.cpp
bool ParseMarkdown(Doc& d, const wchar_t* src, size_t n);  // md4c on UTF-16 (MD4C_USE_UTF16)
// Safe cut for a first-screen prefix parse: start of a top-level ATX heading preceded by a blank line, outside fenced
// code, at or after minChars. Returns n when there is none.
size_t FindPrefixCut(const wchar_t* s, size_t n, size_t minChars);
std::wstring GithubSlug(const wchar_t* s, size_t n);
uint32_t AlertColor(uint8_t alert);  // 0xRRGGBB for the current palette

// highlight.cpp
void Highlight(Doc& d, const wchar_t* lang, uint32_t langLen, uint32_t textOff, uint32_t textLen, uint8_t& langId);

// util.cpp
bool ReadFileUtf16(const wchar_t* path, std::wstring& out, uint64_t* ticksRead, FILETIME* writeTime);
bool ReadFileBytes(const wchar_t* path, std::vector<uint8_t>& out, size_t maxBytes);
bool GetFileStamp(const wchar_t* path, FILETIME* writeTime, uint64_t* size);
