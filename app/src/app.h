// FastMD application state (one document window per process) and the functions shared by the modules:
//   view.cpp        — geometry, virtualised layout, drawing, hit-testing, selection, horizontal scrolling, link focus
//   find.cpp        — find: matching, the find bar, its IME-capable input box (own thread)
//   toc.cpp         — outline panel
//   home.cpp        — start screen: recent documents
//   loader.cpp      — document loading (start-up doc thread, open / reload / history), background measure + images,
//                     file watch, reading positions
//   store.cpp       — settings (registry), reading positions + recent documents (positions.bin), editor detection
//   shell.cpp       — links, clipboard, editor / Explorer / dialogs, .md association
//   settings_ui.cpp — settings window and the gear button that opens it
//   tasks.cpp       — task lists: a click on a box ticks the item in the file itself
//   edit.cpp        — edit mode's glue: the model swap after a change, saving, recovery, test hooks (docs/EDIT-MODE.md)
//   editbar.cpp     — edit mode's chrome on the canvas: the strip (the toolbar follows)
//   strings.cpp     — UI strings (ru / en)
//   window.cpp      — Win32 window, input, commands, menus, wWinMain
#pragma once
#include "canvas.h"
#include "editcore.h"
#include "editfile.h"
#include "layout.h"
#include "strings.h"
#include <unordered_map>

enum : UINT {
    WM_APP_MEASURED = WM_APP + 1,  // lParam = MeasureJob*
    WM_APP_IMAGES,                 // picture worker: lParam = std::vector<RenderResult>* (a batch; the receiver deletes it)
    WM_APP_FULLDOC,                // full parse of a big document ready (first screen came from a prefix)
    WM_APP_FILECHANGED,            // watcher: the open file changed on disk
    WM_APP_POSITIONS,              // positions.bin read after the first frame (lParam = std::vector<PosEntry>*)
    WM_APP_FINDINPUT,              // find input box → UI thread: wParam = FI_*, lParam = event data
    WM_APP_SCALED,                 // images re-scaled to display size (lParam = std::vector<ScaledImage>*)
    WM_APP_UPDATE,                 // updater (update.cpp): wParam = what happened, lParam = its data
    // edit mode (docs/EDIT-MODE.md §13.4; the ids are frozen now, the handlers arrive with their phases)
    WM_APP_PREVIEW = WM_APP + 9,   // preview worker → UI: lParam = PreviewResult*
    WM_APP_EDITINPUT = WM_APP + 10,  // popup EDITs → UI: wParam EI_TEXT (lParam = seq), EI_KEY (vk | mods << 16), EI_FOCUS
    WM_APP_SAVED = WM_APP + 11,    // save worker → UI: lParam = SaveResult*
    WM_APP_TESTKEY = WM_APP + 12,  // FASTMD_TEST_HOOKS only: wParam vk, lParam KM_CTRL | KM_SHIFT | KM_ALT
    WM_APP_REPLAY = WM_APP + 13,   // the modal queue
    WM_APP_QUERY = WM_APP + 64,    // automation / UI tests: wParam = Query → LRESULT (read-only state)
};
enum : UINT_PTR { TIMER_TOAST = 1, TIMER_RELOAD = 2, TIMER_AUTOSCROLL = 3, TIMER_HBAR = 4, TIMER_UPDATE = 5,
                  // edit mode (§13.3); the bar slide runs in the message loop's animation branch, not on a timer
                  TIMER_CARET = 6, TIMER_EDIT_SAVE = 7, TIMER_EDIT_RETRY = 8, TIMER_EDIT_IDLE = 9, TIMER_EDIT_REPARSE = 10,
                  TIMER_EDIT_JOURNAL = 11, TIMER_EDIT_POPUP = 12, TIMER_EDIT_UI = 13 };

// WM_COMMAND ids (menus; tests and automation drive the viewer with them too — keep the numbers stable)
enum Cmd : UINT {
    CMD_COPY = 100, CMD_SELECT_ALL, CMD_OPEN, CMD_RELOAD, CMD_EDIT, CMD_FOLDER, CMD_FIND,
    CMD_THEME_SYSTEM, CMD_THEME_LIGHT, CMD_THEME_DARK, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET,
    CMD_BACK, CMD_FORWARD, CMD_LINK_COPY, CMD_ASSOCIATE,
    CMD_TOC, CMD_COL_NARROW, CMD_COL_NORMAL, CMD_COL_WIDE, CMD_COL_FULL, CMD_COL_NARROWER, CMD_COL_WIDER,
    CMD_WRAP, CMD_SETTINGS, CMD_LINK_OPEN, CMD_IMG_COPY, CMD_IMG_OPEN,
    CMD_FIND_CASE, CMD_FIND_WORD, CMD_FIND_NEXT, CMD_FIND_PREV, CMD_FIND_CLOSE, CMD_LINK_NEXT, CMD_LINK_PREV,
    CMD_LOAD_REMOTE, CMD_COPY_MD, CMD_PRINT, CMD_EXPORT_PDF, CMD_UPDATE,
    // edit mode (docs/EDIT-MODE.md §13.1): the complete list for v1, declared before the features exist - an id whose
    // feature is not built yet does nothing. WM_COMMAND passes HIWORD(wParam) as the argument (0 = the default).
    CMD_EDIT_TOGGLE = 141, CMD_EDIT_HERE, CMD_EDIT_EXIT, CMD_UNDO, CMD_REDO, CMD_CUT, CMD_PASTE, CMD_SAVE, CMD_SAVE_AS,
    CMD_FMT_BOLD = 150, CMD_FMT_ITALIC, CMD_FMT_STRIKE, CMD_FMT_CODE, CMD_LINK, CMD_LINK_REMOVE,
    CMD_BLOCK_P = 156, CMD_BLOCK_H1, CMD_BLOCK_H2, CMD_BLOCK_H3, CMD_BLOCK_H4, CMD_BLOCK_H5, CMD_BLOCK_H6,
    CMD_LIST_BULLET = 163, CMD_LIST_NUMBER, CMD_LIST_TASK, CMD_QUOTE, CMD_CODEBLOCK, CMD_CODE_LANG,
    CMD_INS_TABLE = 169 /* arg = rows << 4 | cols, 0 = 3 x 3 */, CMD_INS_FORMULA, CMD_INS_FORMULA_BLOCK,
    CMD_INS_DIAGRAM /* arg = template 0-8 */, CMD_INS_IMAGE, CMD_INS_HR, CMD_NEW_PARAGRAPH,
    CMD_TABLE_ROW_ABOVE = 176, CMD_TABLE_ROW_BELOW, CMD_TABLE_COL_LEFT, CMD_TABLE_COL_RIGHT, CMD_TABLE_DEL_ROW,
    CMD_TABLE_DEL_COL, CMD_TABLE_ALIGN_L, CMD_TABLE_ALIGN_C, CMD_TABLE_ALIGN_R, CMD_TABLE_DEL,
    CMD_BLOCK_MENU = 186, CMD_TABLE_MENU, CMD_FORMULA_MENU, CMD_DIAGRAM_MENU, CMD_EDIT_MORE, CMD_ATOM_EDIT,
    CMD_POPUP_DONE = 192, CMD_POPUP_CANCEL, CMD_CONFLICT_LOAD, CMD_CONFLICT_KEEP, CMD_SAVE_RETRY, CMD_DISCARD_EDITS,
    CMD_ENC_UTF8 = 198, CMD_ENC_REMOVE_CHAR, CMD_RECOVERY_OPEN, CMD_RECOVERY_RESTORE, CMD_RECOVERY_DELETE,
    CMD_OTHER_WINDOW = 203, CMD_STRIP_CLOSE = 204,
};

// WM_APP_QUERY ids (tests): pixel values are client pixels
enum Query : UINT {
    Q_SCROLLY = 1, Q_DOCH, Q_TOC_OPEN, Q_TOC_DOCKED, Q_TOC_COUNT, Q_TOC_CURRENT, Q_TOC_ITEM_Y /* lp = item */,
    Q_HSCROLL_BLOCK /* first h-scrollable block or -1 */, Q_HSCROLL_X /* lp = block */, Q_FOCUS_LINK, Q_MATCHES,
    Q_CUR_MATCH, Q_TEXT_LEFT, Q_TEXT_W, Q_RECENT_COUNT, Q_FIND_EDIT, Q_SETTINGS_HWND, Q_FIND_OPEN, Q_COLUMN,
    Q_FONT_SIZE, Q_WRAP, Q_LANG, Q_FIND_PART_X /* lp = part → x | y << 16 */, Q_BLOCK_Y /* lp = block */,
    Q_RESTORED, Q_THEME_DARK, Q_TARGETY, Q_SETTINGS_HIT /* lp = row * 100 + option */, Q_FONT_FAMILY_SITKA,
    Q_SETTINGS_BTN /* centre of the gear button x | y << 16, -1 = not shown */,
    Q_IMG_SCALED /* width of the first display-size image copy, 0 = none */,
    Q_FULL_REDRAW /* forget the last frame and repaint everything (tests compare it with a scrolled frame) */,
    Q_SEL_ANCHOR, Q_SEL_FOCUS /* the two ends of the selection, in text offsets */,
    Q_CARET /* MAKELONG(x, y) of the caret in client pixels, or -1 when no caret is drawn */,
    Q_DRAG /* formats a drag would carry: lp = kind * 65536 + arg → DragFormat bits */,
    Q_MATH /* formulas and diagrams: lp = 0 all, 1 drawn, 2 failed */,
    Q_UPDATE /* lp = 0 newer version known, 1 start a test download, 2 installer downloaded and checked,
                3 UpdateStatus, 4 check now (the settings button) */,
    Q_TASK /* lp = task → 1 ticked, 0 not, -1 no such task */,
    Q_TASK_BOX /* lp = task → centre of its box x | y << 16 in client px, -1 = not on screen */,
    Q_DOC_SERIAL /* changes with every load of a document: a reload shows up here */,
    // edit mode (docs/EDIT-MODE.md §13.2): declared now, answered as their features arrive; until then -1
    Q_EDITING = 42 /* 1 editing, 0 not */, Q_EDIT_DIRTY, Q_EDIT_CARET_SRC /* -1 when not editing */, Q_EDIT_ANCHOR_SRC,
    Q_EDIT_TOOL /* lp = id | row << 16 → centre MAKELONG(x, y), -1 hidden */, Q_EDIT_BAR /* slide 0-100 */,
    Q_UNDO_DEPTH /* lp 0 undo, 1 redo */, Q_RELOADS, Q_SAVES, Q_EDIT_POPUP /* lp = field → hwnd */,
    Q_MAP_SELFCHECK /* lp 0: MapSelfCheck now → 1 ok, 0 broken; lp 1: failures under FASTMD_EDIT_SELFCHECK */,
    Q_SRC_HASH /* FNV-1a-32, lp 0 g.src, 1 disk text */, Q_SRC_LEN, Q_EDIT_BUSY, Q_EDIT_PHANTOM, Q_BLOCK_COUNT,
    Q_EDIT_SAVE_STATE, Q_EDIT_CONFLICT, Q_EDIT_ENC, Q_EDIT_EOL, Q_EDIT_ACTIVE, Q_LAST_PROMPT, Q_RELAYOUT_ALL,
    Q_FRAME_STATS, Q_RENDERS, Q_EDIT_STATS, Q_EDIT_CARET_VISIBLE, Q_EDIT_CARET_PHASE, Q_EDIT_POPUP_STATE,
    Q_EDIT_ATOM, Q_EDIT_STRIP, Q_EDIT_RAW, Q_EDIT_BUBBLE, Q_EDIT_COLLAPSE = 75,
};

// what can be dragged out of the window, and the formats it is offered in (drag.cpp)
enum DragKind : int { DRAG_TEXT = 0, DRAG_LINK = 1, DRAG_IMAGE = 2 };
enum DragFormat : uint32_t { DF_TEXT = 1, DF_HTML = 2, DF_RTF = 4, DF_URL = 8, DF_DIB = 16, DF_FILE = 32 };

enum ColumnPreset : uint8_t { COL_NARROW = 0, COL_NORMAL, COL_WIDE, COL_FULL };
enum LangSetting : uint8_t { LANG_AUTO = 0, LANG_RU, LANG_EN };

struct Config {
    bool noIme = true;          // ImmDisableIME(0) on the UI thread: ~10–30 ms saved; the find box lives on its own thread
    bool noAnim = true;         // DWMWA_TRANSITIONS_FORCEDISABLED: document visible at once, no fade/zoom-in
    ThemeMode theme = TM_SYSTEM;
    float zoom = 1.f;
    int sizeW = 1000, sizeH = 800;  // initial client size (DIP)
    wchar_t id[64] = L"";       // bench variant id (window title suffix required by bench/PROTOCOL.md)
    int scrollTest = 0;         // debug: render N scrolling frames after the first one, log to %TEMP%
    // v0.2 settings (store.cpp)
    uint8_t column = COL_NORMAL;
    bool wrapCode = false;
    bool tocOpen = false;       // outline was open when the last window closed
    uint8_t font = FONT_SEGOE;
    int fontSize = 16;          // body text (DIP); scales the whole type ramp
    bool smoothScroll = true;
    uint8_t language = LANG_AUTO;
    uint8_t remoteImages = 0;   // pictures from the network: 0 = always (as on GitHub), 1 = ask, 2 = never
    bool updateCheck = true;    // once a day, ask GitHub whether a newer release exists (plan 5.6)
    std::wstring editor;        // exe for Ctrl+E; "" = the system "edit" verb
    bool findCase = false, findWord = false;
};

struct MeasureJob {
    uint32_t gen, from, to;
    float textW, wideW;
    std::vector<float> h;          // one height per measured block, in the order below
    std::vector<uint32_t> idx;     // the blocks to measure; empty = from .. to
};

// Worker threads that read the document are joined by kind (EDIT-MODE.md §5.1): an edit waits only for the measure
// jobs, which stop within one block; opening a document waits for everything Spawn started.
enum WorkerKind : uint8_t { WK_OTHER, WK_MEASURE, WK_SCALE, WK_FULLPARSE };
struct Worker { HANDLE h; WorkerKind kind; };

// The render table (EDIT-MODE.md §5.2, UI thread only): one entry per picture, formula or diagram source and render
// context, so the same source is rendered once however often it appears, is not rendered again while it is pending,
// and a failure is remembered instead of retried.
struct RenderEntry {
    uint8_t state = RS_PENDING;              // RenderState
    std::shared_ptr<const Pixels> pix;
    int w = 0, h = 0;                        // layout size (a picture's decoded size; a formula's rendered size)
    float ascent = 0;
    std::shared_ptr<const Scaled> sc;        // the last display-size copy made from pix
    std::wstring cachePath;                  // a remote picture: its file in the download cache
    FILETIME failTime{};                     // a local picture that failed: its file's stamp then (retried when it
    uint64_t failSize = 0;                   // changes)
    std::string error;
    uint64_t lastUse = 0;                    // for evicting what the document no longer shows
};
// what the picture worker hands back for one job (WM_APP_IMAGES)
struct RenderResult {
    std::wstring key;                        // render-table key (source key + context)
    uint32_t ctx = 0, loadGen = 0;
    bool math = false, ok = false;
    std::shared_ptr<const Pixels> pix;
    int w = 0, h = 0;
    float ascent = 0;
    std::wstring cachePath;
    FILETIME failTime{};
    uint64_t failSize = 0;
    std::string error;
};

// FASTMD_TEST_SLOW=images:<ms>,scale:<ms>,preview:<ms>,fullparse:<ms>,save:<ms> (tests: every job of that worker
// sleeps this long, so races that are too quick to happen on their own can be forced; §13.5)
struct TestSlow { DWORD images = 0, scale = 0, preview = 0, fullparse = 0, save = 0; };

struct HistoryEntry { std::wstring path; float scrollY; };

// reading position of one file (positions.bin): exact place when the file is unchanged, the nearest section when not
struct PosEntry {
    std::wstring path;
    uint64_t size = 0, mtime = 0, opened = 0;  // file stamp at save; last opened (FILETIME ticks)
    uint32_t block = 0;
    float blockOff = 0;                        // DIP from the top of `block` to the top of the viewport
    std::wstring slug;                         // nearest heading at or above the viewport top ("" = none)
    float slugOff = 0;                         // DIP from that heading to the viewport top
};

struct TocItem { uint32_t block; uint8_t level; std::wstring text; IDWriteTextLayout* layout = nullptr; };

// one picture scaled to its display size on a worker thread, handed to the UI thread (WM_APP_SCALED): it belongs to
// every entry showing the pixels with that serial, whatever their index is by then
struct ScaledImage { std::wstring key; uint32_t pxSerial; int w, h; std::vector<uint32_t> px; };

struct App {
    Config cfg;
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    float dpi = 96.f;
    int pxW = 0, pxH = 0;          // client size in pixels

    // ---- document
    std::wstring path;             // "" = start screen (no document)
    std::wstring src;              // UTF-16 source (immutable while workers run)
    Doc doc;
    std::atomic<Doc*> fullDoc{nullptr};
    bool fullPending = false, loadFailed = false;
    bool scalingImages = false;    // a scaler thread is making display-size copies right now
    bool editing = false;          // edit mode (docs/EDIT-MODE.md; entered from Phase 2a on)
    int editModal = 0;             // modal depth (§10.10): no model swap inside a menu, a dialog or a print job
    uint32_t editSerial = 0;       // bumped by every model swap after an edit (edit.cpp)
    uint32_t reloads = 0, saves = 0;  // loads after start-up (Q_RELOADS), successful saves (Q_SAVES)
    std::wstring eol = L"\n";      // the line end the editor writes where a line has none (§7.2)
    DiskState disk;                // what the disk holds (§10.2): encoding and identity from the load, the rest once
                                   // something is to be written
    float stripH = 0;              // the strip over the top of the document (editbar.cpp), 0 = none
    std::unordered_map<std::wstring, RenderEntry> renders;  // the render table (loader.cpp), UI thread only
    std::atomic<uint32_t> loadGen{0};         // bumped by every load: queued picture jobs of the old document are skipped
    std::atomic<uint32_t> rendersStarted{0};  // renders and decodes the workers started (Q_RENDERS)
    bool remoteAllowedOnce = false;  // "ask": the reader allowed the network pictures of this document
    FILETIME fileTime{};
    uint64_t fileSize = 0;
    std::vector<HistoryEntry> back, fwd;
    uint32_t docSerial = 0;        // bumped whenever g.doc is replaced (outline / link caches key on it)
    uint32_t pixelSerial = 0;      // bumped when content draws differently without any other change (images arrived)
    uint32_t hxSerial = 0;         // bumped by every horizontal scroll of a wide block

    // ---- text & layout
    IDWriteFactory3* dwf = nullptr;
    Typography typo;
    std::vector<BlockLayout*> cache;
    std::vector<float> H, Y;
    std::vector<uint8_t> known;    // height is exact (measured / computed)
    float docH = 0, scrollY = 0, targetY = 0;
    float textW = 0, wideW = 0;    // layout widths (DIP) the cache was built for
    float availW = 0;              // everything between the page margins: what a diagram may use
    uint32_t cachedCount = 0;
    std::unordered_map<uint32_t, IDWriteTextLayout*> numLayouts;
    std::vector<float> hx;         // per block: horizontal scroll of a wide code block / table / diagram (DIP)
    bool fitWide = false;          // nothing can be scrolled sideways here (printing, the Explorer pane): fit instead

    // ---- rendering
    Canvas* canvas = nullptr;
    bool offscreenValid = false;   // canvas holds the current frame (first frame rendered on the doc thread)

    // ---- threads
    HANDLE docThread = nullptr;
    std::vector<Worker> workers;      // Spawn: measure, scale, full parse (the picture worker and the updater are detached)
    SRWLOCK workersLock = SRWLOCK_INIT;
    std::atomic<uint32_t> gen{0};     // layout generation: bumped whenever the document / widths change → measure jobs stop
    std::atomic<uint32_t> docGen{0};  // document generation: bumped when the document changes → a full parse is dropped
    std::atomic<bool> closing{false};
    int jobsPending = 0;
    HANDLE watchThread = nullptr, watchStop = nullptr;

    // ---- state
    bool ready = false, firstFrame = true, animating = false;

    // ---- selection (absolute offsets in doc.text)
    uint32_t selAnchor = 0, selFocus = 0;
    bool selecting = false;
    bool caretOn = false;       // the caret is drawn only while the keyboard drives the selection
    float caretWantX = -1.f;    // x the caret aims for while it moves up or down (-1 = take it from the caret)
    int clickCount = 0;
    DWORD lastClickTime = 0;
    POINT lastClickPt{};

    // ---- find
    bool findOpen = false;
    std::wstring findQuery;
    std::wstring lowerText;         // lazily built lower-case copy of doc.text
    std::vector<uint32_t> matches;  // match start offsets, sorted
    int curMatch = -1;
    int findHot = -1;               // hovered find-bar part (FindPart)

    // ---- outline
    bool tocOpen = false;
    float tocScroll = 0;
    int tocHover = -1;              // item index, -2 = the panel's close button
    bool tocBtnHot = false;
    std::vector<TocItem> toc;
    uint32_t tocSerial = UINT32_MAX;  // docSerial the items were built for

    // ---- keyboard link focus / context menu target
    int focusLink = -1;
    int ctxLink = -1, ctxImage = -1;

    // ---- start screen
    std::vector<PosEntry> recentAll;  // positions.bin, most recently opened first
    std::wstring recentFilter;
    std::vector<int> recentShown;     // indices into recentAll
    int recentSel = 0, recentHover = -1;

    // ---- reading position
    std::vector<PosEntry> positions;  // loaded after the first frame
    bool positionsLoaded = false;
    bool userMoved = false;           // the reader scrolled / navigated: a late position restore must not jump
    int restoreBlock = -1;            // pending restore target (re-resolved while heights are measured)
    float restoreOff = 0;
    bool restored = false;

    // ---- mouse / overlays
    bool draggingThumb = false, hotScroll = false, downOnLink = false;
    int dragKind = -1, dragArg = 0;  // what the press under the mouse could start dragging (DragKind, -1 = nothing)
    uint32_t dragPos = 0;            // text position of that press: where the selection collapses if it never moved
    float dragGrab = 0;
    int downX = 0, downY = 0;
    int hoverLink = -1;             // link index under the mouse
    int hoverCode = -1;             // code block under the mouse (copy button)
    bool hoverCopyBtn = false;
    int hoverHBlock = -1;           // wide block under the mouse (its horizontal scrollbar is shown)
    bool hotHBar = false;
    int dragHBlock = -1;            // horizontal thumb being dragged
    float dragHGrab = 0;
    int hbarFlash = -1;             // block whose horizontal scrollbar shows briefly after it scrolled
    DWORD hbarFlashUntil = 0;
    bool settingsBtnHot = false;    // the gear button (top-right corner) under the mouse
    int hoverHeading = -1;          // heading under the mouse: shows the link icon beside it
    int hoverTask = -1;             // task list item whose box is under the mouse (block index)
    int downTask = -1;              // the box the left button went down on: ticked if it comes up there too
    std::wstring tip;               // tooltip pill (buttons)
    std::wstring toast;
    DWORD toastUntil = 0;
};
extern App g;

// Modal depth (EDIT-MODE.md §10.10): every modal loop - a menu, a message box, a file or print dialog, a print job, a
// drag - runs inside one of these. Messages still arrive in such a loop, and whoever opened it holds on to the model
// (a print job to its pages), so meanwhile the reload waits, a test splice is refused, window activation looks at no
// file, and pictures that arrive are put in place only once the last scope has closed (WM_APP_REPLAY).
struct ModalScope {
    ModalScope() { g.editModal++; }
    ~ModalScope();
    ModalScope(const ModalScope&) = delete;
    ModalScope& operator=(const ModalScope&) = delete;
};

// ------------------------------------------------------------------------------------------------ view.cpp
float Scale();                       // pixels per DIP (DPI / 96 × zoom)
float ViewW();
float ViewH();
float MaxScroll();
float DocLeft();                     // left edge of the document area (the docked outline takes the rest)
float DocW();
float TextLeft();
float WideLeft();
void UpdateColumns();                // textW / wideW for the current window width and column preset
float LayoutWidthFor(const Block& b, float textW, float wideW);
float AvailLeft();   // left edge of everything between the page margins
void BlockBox(uint32_t i, float* x, float* w);  // drawn box of a block (DIP, document x)
uint32_t FirstVisible(float y);
uint32_t BlockOfPos(uint32_t pos);   // last block whose textOff <= pos
void InitGeometry();
void RecomputeY();
void EnsureVisible();
void InitialLayout();
BlockLayout* EnsureLayout(uint32_t i);
void ClearLayoutCache();
void TrimCache();
void Render();                       // draw the frame into the canvas (a pure scroll only redraws what changed)
void ForceFullRedraw();              // the canvas content is no longer trusted: the next frame is drawn in full
void WithAnchor(void (*fn)());       // keep the top visible block in place while heights change
void DrawPill(const std::wstring& s, float x, float y, bool centered);
IDWriteTextLayout* UiLayout(const std::wstring& s, float maxW, IDWriteTextFormat* fmt = nullptr);
// one icon-font glyph centred in a box × box square; the font loads on first use, so never in the first frame
void DrawIcon(wchar_t icon, float l, float t, float box, float size, uint8_t pal);

// horizontal scrolling of code blocks / tables wider than their box
bool HScrollInfo(uint32_t i, float* visX, float* visW, float* contentW);
float HScrollOf(uint32_t i);         // clamped offset
void HScrollSet(uint32_t i, float x);
int HScrollBlockAt(float x, float y, bool* onBar);  // wide block (or its scrollbar) under the point
bool HScrollBarRect(uint32_t i, float* l, float* t, float* r, float* b, float* thumbL, float* thumbR);  // client DIP

// positions & links
bool HitTestDoc(float x, float y, uint32_t* pos, bool* inside);  // client DIP → absolute text offset
int LinkAt(float x, float y);        // link index or -1
int ImageAt(float x, float y);       // image block index or -1
int CodeBlockAt(float x, float y, bool* onCopyButton);
void SelectAll();
void SelectWordAt(uint32_t pos);
void SelectBlockAt(uint32_t pos);
bool KeySelect(unsigned vk, bool ctrl, bool shift);  // Shift+arrows / Home / End: move the caret, extend the selection
bool CaretPoint(uint32_t pos, float* x, float* y, float* h);  // caret in client DIP
bool HasSelection();
bool PosInSelection(uint32_t pos);
// text ranges over the document, shared by the keyboard, the clipboard and the screen-reader provider (uia.cpp)
bool WordRange(uint32_t pos, uint32_t* from, uint32_t* to);
bool LineRange(uint32_t pos, uint32_t* from, uint32_t* to);
bool ParagraphRange(uint32_t pos, uint32_t* from, uint32_t* to);
uint32_t TextStep(uint32_t pos, int dir);
void RangeScreenRects(uint32_t from, uint32_t to, std::vector<double>& out);
int HeadingLevelAt(uint32_t pos);
std::wstring SelectionText();
std::wstring BlockPlainText(uint32_t i);
int HeadingBlockBySlug(const std::wstring& slug);
int HeadingAt(float x, float y, bool* onIcon);  // heading under the pointer (-1 = none), and its link icon
int SummaryAt(float x, float y);                // <summary> line under the pointer (-1 = none)
void ToggleDetails(uint32_t block);             // fold the block's <details> open or shut
int TaskAt(float x, float y);                   // task list box under the pointer: its block (-1 = none)
bool TaskBoxRect(uint32_t block, float* l, float* t, float* r, float* b);  // client DIP; false = none on screen
bool BlockHidden(const Block& b);               // inside a folded <details>
std::wstring SlugOfBlock(uint32_t block);       // "" if the block is not a heading
void ScrollToBlock(uint32_t i, bool animate);
void DrawDocumentPage(float docTop, float docBottom);   // one printed page, drawn on the print canvas
float BlockSplitY(uint32_t i, float limit);             // where a page may end inside a tall block
void RevealTextPos(uint32_t pos, bool center);  // scroll (and h-scroll a wide block) so a text position is visible
bool LinkRange(int li, uint32_t* start, uint32_t* end);  // text range of a link (first run … last run)
void FocusLinkStep(int dir);         // Tab / Shift+Tab

// overlays
void ShowToast(const std::wstring& text, DWORD ms = 1200);

// ------------------------------------------------------------------------------------------------ find.cpp
enum FindPart { FP_NONE = -1, FP_BAR, FP_FIELD, FP_TEXT, FP_CASE, FP_WORD, FP_COUNT, FP_PREV, FP_NEXT, FP_CLOSE };
enum FindInputEvent : WPARAM { FI_TEXT = 1, FI_KEY, FI_FOCUS };
void FindOpen();
void FindClose();
void FindUpdate(bool keepCurrent);
void FindRefresh();                  // the text changed under the matches: find them again without scrolling
void FindStep(int dir);
void FindToggleCase();
void FindToggleWord();
void DrawFindBar();
void DrawFindMarks(float x0, float x1);       // match ticks on the scrollbar track
int FindPartAt(float x, float y);             // FindPart or FP_NONE
void FindPartRect(int part, float* l, float* t, float* r, float* b);
void FindClick(int part);
void FindOnInput(WPARAM ev, LPARAM lp);       // WM_APP_FINDINPUT
void FindRelayoutInput();                     // window size / zoom / theme / language changed
bool FindTypeChar(wchar_t c);                 // WM_CHAR on the document while the bar is open
bool FindInputFocused();
void FindFocusInput();
HWND FindEditHwnd();                          // the box's EDIT (tests type into it)

// ------------------------------------------------------------------------------------------------ toc.cpp
bool TocAvailable();                 // the document has headings
bool TocWideEnough();                // the window can keep the column beside the panel
bool TocDocked();                    // open and the window is wide enough to keep the column beside it
bool TocOverlayOpen();               // shown as a drawer over the text (narrow window)
float TocPanelW();
void TocSetOpen(bool open);
void TocSync();                      // rebuild the items after the document changed
int TocCurrent();
void DrawToc();
bool TocHit(float x, float y, int* item);    // point in the panel; item index, -2 = close button, -1 = none
bool TocButtonHit(float x, float y);         // the floating outline button (panel closed)
bool TocButtonRect(float* l, float* t, float* r, float* b);  // client DIP; false = not shown
void TocClick(int item);
void TocWheel(float dy);
float TocItemY(int item);            // client DIP of an item's centre (tests)

// ------------------------------------------------------------------------------------------------ home.cpp
void HomeRebuild();                  // recentShown for the current filter
void DrawHome();
int HomeItemAt(float x, float y);
void HomeOpen(int shownIndex);
bool HomeKey(WPARAM vk);
bool HomeChar(wchar_t c);

// ------------------------------------------------------------------------------------------------ loader.cpp
DWORD WINAPI StartupDocThread(void*);
void OpenDocument(const std::wstring& path, bool pushHistory, float scrollY, bool restorePosition = false);
void ReloadDocument();
void NavigateBack();
void NavigateForward();
void StartBackgroundWork();          // after the first frame: measure, images, full doc, watcher
void OnMeasured(MeasureJob* job);
void OnFullDoc();
// Pictures, formulas and diagrams through the render table: what the table has is shown at once, the rest is queued
// for the picture worker (once per source and context). Runs after every load and every model swap.
void StartImages();
void OnImagesLoaded(std::vector<RenderResult>* batch);  // WM_APP_IMAGES
std::wstring ImageRenderKey(const Image& im);  // the render-table key of an entry in the current render context
void RetryChangedPictures();         // the window was activated: retry pictures whose failed file has changed since
bool RemoteImagesAllowed();          // the privacy setting, plus a one-off allowance for this document
bool DocHasRemoteImages();           // something is waiting to be fetched
void LoadRemoteImages();             // allow them for this document and start fetching
void ScheduleImageScaling();         // after a frame: start the scaler if an image was drawn at a size we have no copy of
void OnScaledImages(std::vector<ScaledImage>* list);
void StartMeasure();
void JoinWorkers();                  // open / reload: stop and wait for every worker that reads the document
void JoinDocReaders();               // an edit swap: stop the measure jobs and wait for them only (§5.1)
void MeasureUnknown();               // measure only the blocks whose height is still a guess (after an edit swap)
// §5.5 step 4: what the render table (and the old model) know about the pictures of a new model, put on it before it
// is installed; a formula whose source is being typed in [editBeg, newEnd) keeps the old one's picture meanwhile
void CarryRenders(const Doc& oldD, Doc& nd, uint32_t editBeg, uint32_t oldEnd, uint32_t newEnd);
void StartWatcher();
void StopWatcher();
void OnFileChanged();                // the watcher saw the file change: reload unless the disk holds what is shown
HANDLE Spawn(LPTHREAD_START_ROUTINE fn, void* arg, int prio = THREAD_PRIORITY_NORMAL, SIZE_T stack = 0,
             WorkerKind kind = WK_OTHER);
const TestSlow& TestSlowMs();        // FASTMD_TEST_SLOW, read once
std::wstring WindowTitle();
void LoadPositionsAsync();           // after the first frame
void OnPositionsLoaded(std::vector<PosEntry>* list);
void SaveReadingPosition();          // current document → positions.bin
void ResolveRestore();               // re-aim a pending position restore after heights changed

// ------------------------------------------------------------------------------------------------ store.cpp
const wchar_t* RegKeyPath();         // HKCU\Software\FastMD (FASTMD_REGKEY overrides: tests)
std::wstring DataDir();              // %LOCALAPPDATA%\FastMD\ (FASTMD_DATA overrides: tests), with trailing backslash
std::string Sha256Hex(const uint8_t* data, size_t n);  // what the updater checks a download against
void LoadConfig(Config& c, std::wstring* findQuery);  // everything but the window placement
void SaveConfig(const Config& c, const std::wstring& findQuery);
bool RegReadBinary(const wchar_t* name, void* data, DWORD size);
void RegWriteBinary(const wchar_t* name, const void* data, DWORD size);
bool PositionsLoad(std::vector<PosEntry>& out);             // most recently opened first
void PositionsSave(const PosEntry& e, bool keepPosition);   // merge one entry (keepPosition: only touch `opened`)
void PositionsRemove(const std::wstring& path);
uint64_t FileTimeU64(const FILETIME& ft);
struct EditorInfo { std::wstring name, exe; };
std::vector<EditorInfo> DetectEditors();

// ------------------------------------------------------------------------------------------------ shell.cpp
void OpenLink(int linkIndex);
void CopyToClipboard(const std::wstring& text);
// copy.cpp: the selection with its formatting (CF_HTML + RTF + text), and as the Markdown source it came from
void CopySelectionRich();
void SelectionRichFormats(std::wstring& text, std::string& cfHtml, std::string& rtf);  // clipboard and drag
std::wstring SelectionMarkdown();
bool CopyImageToClipboard(uint32_t imageBlock);
HGLOBAL ImageAsDib(uint32_t imageBlock);  // the picture as a 32-bit DIB (clipboard and drag)
void OpenImageFile(uint32_t imageBlock);
void OpenInEditor();
void ShowInFolder();
void OpenDialog();
std::wstring PickExeDialog(HWND owner);
std::wstring SavePdfDialog();        // where to write the exported PDF ("" = cancelled)

// ------------------------------------------------------------------------------------------------ update.cpp
enum UpdateStatus : int {
    US_IDLE, US_CHECKING, US_LATEST, US_AVAILABLE, US_DOWNLOADING, US_INSTALLING, US_INSTALLED, US_CHECK_FAILED,
    US_DOWNLOAD_FAILED,
};
void UpdateCheckAsync();             // after the first frame: at most one request a day (a failed one: an hour later)
void UpdateCheckNow();               // the settings button: ask GitHub right away
bool UpdateAvailable();              // a newer release is known (found now or remembered from an earlier check)
std::wstring UpdateVersion();
UpdateStatus UpdateGetStatus();
void UpdateInstall(bool ask);        // (ask,) download, check the hash, run the installer, offer a restart
void UpdateRestart();                // start the installed FastMD on this document and close this window
void OnUpdateMessage(WPARAM what, LPARAM lp);
void UpdateFetchForTest();           // automation: download + verify, without running anything
bool UpdateFetched();
uint64_t LastUpdateCheck();          // store.cpp
void SetLastUpdateCheck(uint64_t t);
// store.cpp: the newest release a check found, so later windows show it without asking ("" = none)
void LoadFoundUpdate(std::wstring& version, std::wstring& url, std::wstring& shaUrl);
void SaveFoundUpdate(const std::wstring& version, const std::wstring& url, const std::wstring& shaUrl);

// ------------------------------------------------------------------------------------------------ uia.cpp
LRESULT UiaHandleGetObject(WPARAM wp, LPARAM lp);  // WM_GETOBJECT: hand a screen reader the document
void UiaDocumentChanged();                         // another document was opened
void UiaSelectionChanged();                        // the selection moved
void UiaShutdown();                                // on close

// ------------------------------------------------------------------------------------------------ crash.cpp
void CrashHandlerInstall();          // wWinMain: minidumps into %LOCALAPPDATA%\FastMD\crashes
void CrashReportIfAny();             // after the first frame: offer the folder if the last run left a dump
uint64_t LastCrashSeen();            // store.cpp
void SetLastCrashSeen(uint64_t t);

// ------------------------------------------------------------------------------------------------ drag.cpp
void StartDrag(int kind, int arg);   // runs the shell's drag loop with the selection / link / picture
uint32_t DragFormats(int kind, int arg);  // which formats that drag would carry (automation)

// ------------------------------------------------------------------------------------------------ print.cpp
void PrintDocument();                // Ctrl+P: the system print dialog, then the job
void ExportPdf();                    // the same job through "Microsoft Print to PDF" into a chosen file
bool RegisterAssociation(bool openSettings);
bool PreviewRegister(bool on);       // window.cpp: the preview pane and thumbnails in Explorer (plan 5.2, 5.3)
void UnregisterAssociation();
std::wstring UrlDecode(const std::wstring& s);

// ------------------------------------------------------------------------------------------------ tasks.cpp
bool ToggleTask(uint32_t block);     // tick or untick a task list item in the file, then on screen

// ------------------------------------------------------------------------------------------------ edit.cpp
// The model swap (§5.5): g.src changed in [at, at + oldLen) → [at, at + newLen) (at = UINT32_MAX: nothing changed).
void EditReparse(uint32_t at = UINT32_MAX, uint32_t oldLen = 0, uint32_t newLen = 0);
bool EditSplice(uint32_t at, uint32_t len, std::wstring text);  // the one way g.src changes (§7.1); false = refused
bool EditDirty();                    // g.src != the baseline's text (compared, not flagged)
enum BaselineResult { BL_OK, BL_CHANGED, BL_UNREADABLE, BL_REFUSED };
// Takes the baseline from the disk when there is none (§10.1): the file must hold exactly the text on screen, in bytes
// that can be written back byte for byte. BL_CHANGED: it holds something else; BL_UNREADABLE: *st says why.
BaselineResult EditBaseline(SaveState* st);
SaveState EditSave(bool flushPoint); // g.src into the file (§10.3); flushPoint: leave, close, Ctrl+S
void EditPushStep(EditStep step);    // an undo step for a change made outside edit mode, if a history exists
void EditOnLoad();                   // a document was (re)loaded: a new session
void EditLeaveDocument();            // before another document (or a reload, or the close): a flush point
void EditAfterOpen();                // after the first frame of an open: an interrupted save's recovery file?
bool EditRecoveryRestorable();       // the recovery strip may offer Restore (the file is still the torn one)
void EditCommand(UINT id, UINT arg); // the recovery strip's commands
LRESULT EditCopyData(const COPYDATASTRUCT* cd);  // FASTMD_TEST_HOOKS: splice / save
bool EditTestHooks();                // FASTMD_TEST_HOOKS=1 (read once)
void EditTimer(UINT_PTR id);
bool EditQuery(UINT q, LPARAM lp, LRESULT* out);  // edit mode's queries; false = not one of them

// ------------------------------------------------------------------------------------------------ editbar.cpp
enum StripId : int { STRIP_NONE, STRIP_CONFLICT, STRIP_ENCODING, STRIP_LEAVE, STRIP_READONLY, STRIP_MISSING,
                     STRIP_RECOVERY, STRIP_OTHER_WINDOW };  // Q_EDIT_STRIP
void StripShow(int kind);
void StripHide(int kind);            // only if that one is shown
int StripKind();
LRESULT StripButtonCenter(UINT cmd); // Q_EDIT_TOOL: client px MAKELONG(x, y), -1 = not shown
bool StripMouse(float x, float y, bool click);  // true = the point is on the strip (a click runs its button)
void DrawEditChrome();               // view.cpp, over the document and under the find bar

// ------------------------------------------------------------------------------------------------ settings_ui.cpp
void SettingsOpen();
void SettingsRefresh();              // settings changed elsewhere (menu, shortcut, other window)
HWND SettingsHwnd();
LRESULT SettingsHitCenter(int id);   // tests: control centre (client px), -1 if absent
// the gear button in the document window's top-right corner (not shown in the first frame or under the find bar)
bool SettingsButtonRect(float* l, float* t, float* r, float* b);  // client DIP; false = not shown
bool SettingsButtonHit(float x, float y);
void DrawSettingsButton();

// ------------------------------------------------------------------------------------------------ window.cpp
enum SettingsChange : uint32_t {
    SC_THEME = 1, SC_TYPE = 2 /* font, size, wrap */, SC_COLUMN = 4, SC_LANGUAGE = 8, SC_OTHER = 16, SC_ALL = 31,
};
void Invalidate();
bool PrepareToClose();               // the window is about to close (or restart): false = the reader chose to stay
void Relayout();                     // column widths / typography changed: drop layouts, re-estimate, re-measure
bool KeyCommand(WPARAM vk, bool ctrl, bool shift, bool alt);  // keyboard shortcuts (also forwarded by the find box)
void ScrollTo(float y, bool animate);
void UserScrollTo(float y, bool animate);  // reader-initiated: cancels a pending position restore
void ApplyTheme();                   // re-evaluate system / forced theme, repaint
void ApplySettings(uint32_t changed, bool persist);  // g.cfg changed → re-layout / repaint (+ save, notify other windows)
void Command(UINT id, UINT arg = 0);  // arg: HIWORD(wParam) of WM_COMMAND (0 from menus), e.g. a table's size
