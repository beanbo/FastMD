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
//   strings.cpp     — UI strings (ru / en)
//   window.cpp      — Win32 window, input, commands, menus, wWinMain
#pragma once
#include "canvas.h"
#include "layout.h"
#include "strings.h"
#include <unordered_map>

enum : UINT {
    WM_APP_MEASURED = WM_APP + 1,  // lParam = MeasureJob*
    WM_APP_IMAGES,                 // image thread finished
    WM_APP_FULLDOC,                // full parse of a big document ready (first screen came from a prefix)
    WM_APP_FILECHANGED,            // watcher: the open file changed on disk
    WM_APP_POSITIONS,              // positions.bin read after the first frame (lParam = std::vector<PosEntry>*)
    WM_APP_FINDINPUT,              // find input box → UI thread: wParam = FI_*, lParam = event data
    WM_APP_SCALED,                 // images re-scaled to display size (wParam = docGen, lParam = std::vector<ScaledImage>*)
    WM_APP_UPDATE,                 // update check: wParam 0 = newer version found, 1 = installer ready, 2 = failed
    WM_APP_QUERY = WM_APP + 64,    // automation / UI tests: wParam = Query → LRESULT (read-only state)
};
enum : UINT_PTR { TIMER_TOAST = 1, TIMER_RELOAD = 2, TIMER_AUTOSCROLL = 3, TIMER_HBAR = 4 };

// WM_COMMAND ids (menus; tests and automation drive the viewer with them too — keep the numbers stable)
enum Cmd : UINT {
    CMD_COPY = 100, CMD_SELECT_ALL, CMD_OPEN, CMD_RELOAD, CMD_EDIT, CMD_FOLDER, CMD_FIND,
    CMD_THEME_SYSTEM, CMD_THEME_LIGHT, CMD_THEME_DARK, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET,
    CMD_BACK, CMD_FORWARD, CMD_LINK_COPY, CMD_ASSOCIATE,
    CMD_TOC, CMD_COL_NARROW, CMD_COL_NORMAL, CMD_COL_WIDE, CMD_COL_FULL, CMD_COL_NARROWER, CMD_COL_WIDER,
    CMD_WRAP, CMD_SETTINGS, CMD_LINK_OPEN, CMD_IMG_COPY, CMD_IMG_OPEN,
    CMD_FIND_CASE, CMD_FIND_WORD, CMD_FIND_NEXT, CMD_FIND_PREV, CMD_FIND_CLOSE, CMD_LINK_NEXT, CMD_LINK_PREV,
    CMD_LOAD_REMOTE, CMD_COPY_MD, CMD_PRINT, CMD_EXPORT_PDF, CMD_UPDATE,
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
    Q_UPDATE /* lp = 0 newer version found, 1 start a test download, 2 installer downloaded and checked */,
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
    std::vector<float> h;
};

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

// one image scaled to its display size on a worker thread, handed to the UI thread (WM_APP_SCALED)
struct ScaledImage { uint32_t index; int w, h; std::vector<uint32_t> px; };

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
    bool fullPending = false, imagesStarted = false, loadFailed = false;
    bool scalingImages = false;    // a scaler thread is making display-size copies right now
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
    std::vector<HANDLE> workers;
    SRWLOCK workersLock = SRWLOCK_INIT;
    std::atomic<uint32_t> gen{0};     // layout generation: bumped whenever the document / widths change → measure jobs stop
    std::atomic<uint32_t> docGen{0};  // document generation: bumped only when the document changes → full parse / images stop
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
    std::wstring tip;               // tooltip pill (buttons)
    std::wstring toast;
    DWORD toastUntil = 0;
};
extern App g;

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
void OnImagesLoaded();
bool RemoteImagesAllowed();          // the privacy setting, plus a one-off allowance for this document
bool DocHasRemoteImages();           // something is waiting to be fetched
void LoadRemoteImages();             // allow them for this document and start fetching
void ScheduleImageScaling();         // after a frame: start the scaler if an image was drawn at a size we have no copy of
void OnScaledImages(std::vector<ScaledImage>* list, uint32_t gen);
void StartMeasure();
void JoinWorkers();
void StartWatcher();
void StopWatcher();
HANDLE Spawn(LPTHREAD_START_ROUTINE fn, void* arg, int prio = THREAD_PRIORITY_NORMAL, SIZE_T stack = 0);
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
void UpdateCheckAsync();             // after the first frame: at most one request a day
bool UpdateAvailable();
std::wstring UpdateVersion();
void UpdateInstall();                // ask, download, check the hash, run the installer
void OnUpdateMessage(WPARAM what);
void UpdateFetchForTest();           // automation: download + verify, without running anything
bool UpdateFetched();
uint64_t LastUpdateCheck();          // store.cpp
void SetLastUpdateCheck(uint64_t t);

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
void Relayout();                     // column widths / typography changed: drop layouts, re-estimate, re-measure
bool KeyCommand(WPARAM vk, bool ctrl, bool shift, bool alt);  // keyboard shortcuts (also forwarded by the find box)
void ScrollTo(float y, bool animate);
void UserScrollTo(float y, bool animate);  // reader-initiated: cancels a pending position restore
void ApplyTheme();                   // re-evaluate system / forced theme, repaint
void ApplySettings(uint32_t changed, bool persist);  // g.cfg changed → re-layout / repaint (+ save, notify other windows)
void Command(UINT id);
