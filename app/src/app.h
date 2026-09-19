// FastMD application state (one document window per process) and the functions shared by the modules:
//   view.cpp   — geometry, virtualised layout, drawing, hit-testing, selection, find
//   loader.cpp — document loading (start-up doc thread, open / reload / history), background measure + images, file watch
//   window.cpp — Win32 window, input, commands, menus, settings, association, wWinMain
#pragma once
#include "canvas.h"
#include "layout.h"
#include <unordered_map>

enum : UINT {
    WM_APP_MEASURED = WM_APP + 1,  // lParam = MeasureJob*
    WM_APP_IMAGES,                 // image thread finished
    WM_APP_FULLDOC,                // full parse of a big document ready (first screen came from a prefix)
    WM_APP_FILECHANGED,            // watcher: the open file changed on disk
};
enum : UINT_PTR { TIMER_TOAST = 1, TIMER_RELOAD = 2, TIMER_AUTOSCROLL = 3 };

struct Config {
    bool noIme = true;          // ImmDisableIME(0) on the UI thread: ~10–30 ms saved, no TSF stalls (the find box takes WM_CHAR)
    bool noAnim = true;         // DWMWA_TRANSITIONS_FORCEDISABLED: document visible at once, no fade/zoom-in
    ThemeMode theme = TM_SYSTEM;
    float zoom = 1.f;
    int sizeW = 1000, sizeH = 800;  // initial client size (DIP)
    wchar_t id[64] = L"";       // bench variant id (window title suffix required by bench/PROTOCOL.md)
    int scrollTest = 0;         // debug: render N scrolling frames after the first one, log to %TEMP%
};

struct MeasureJob {
    uint32_t gen, from, to;
    float textW, wideW;
    std::vector<float> h;
};

struct HistoryEntry { std::wstring path; float scrollY; };

struct App {
    Config cfg;
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    float dpi = 96.f;
    int pxW = 0, pxH = 0;          // client size in pixels

    // ---- document
    std::wstring path;             // "" = empty state (no document)
    std::wstring src;              // UTF-16 source (immutable while workers run)
    Doc doc;
    std::atomic<Doc*> fullDoc{nullptr};
    bool fullPending = false, imagesStarted = false, loadFailed = false;
    FILETIME fileTime{};
    uint64_t fileSize = 0;
    std::vector<HistoryEntry> back, fwd;

    // ---- text & layout
    IDWriteFactory3* dwf = nullptr;
    Typography typo;
    std::vector<BlockLayout*> cache;
    std::vector<float> H, Y;
    std::vector<uint8_t> known;    // height is exact (measured / computed)
    float docH = 0, scrollY = 0, targetY = 0;
    float textW = 0, wideW = 0;    // layout widths (DIP) the cache was built for
    uint32_t cachedCount = 0;
    std::unordered_map<uint32_t, IDWriteTextLayout*> numLayouts;

    // ---- rendering
    Canvas* canvas = nullptr;
    bool offscreenValid = false;   // canvas holds the current frame (first frame rendered on the doc thread)

    // ---- threads
    HANDLE docThread = nullptr;
    std::vector<HANDLE> workers;
    SRWLOCK workersLock = SRWLOCK_INIT;
    std::atomic<uint32_t> gen{0};  // bumped whenever the document / widths change → stale jobs stop
    std::atomic<bool> closing{false};
    int jobsPending = 0;
    HANDLE watchThread = nullptr, watchStop = nullptr;

    // ---- state
    bool ready = false, firstFrame = true, animating = false;

    // ---- selection (absolute offsets in doc.text)
    uint32_t selAnchor = 0, selFocus = 0;
    bool selecting = false;
    int clickCount = 0;
    DWORD lastClickTime = 0;
    POINT lastClickPt{};

    // ---- find
    bool findOpen = false;
    std::wstring findQuery;
    std::wstring lowerText;         // lazily built lower-case copy of doc.text
    std::vector<uint32_t> matches;  // match start offsets, sorted
    int curMatch = -1;

    // ---- mouse / overlays
    bool draggingThumb = false, hotScroll = false, downOnLink = false;
    float dragGrab = 0;
    int downX = 0, downY = 0;
    int hoverLink = -1;             // link index under the mouse
    int hoverCode = -1;             // code block under the mouse (copy button)
    bool hoverCopyBtn = false;
    std::wstring toast;
    DWORD toastUntil = 0;
};
extern App g;

// ------------------------------------------------------------------------------------------------ view.cpp
float Scale();                       // pixels per DIP (DPI / 96 × zoom)
float ViewW();
float ViewH();
float MaxScroll();
void UpdateColumns();                // textW / wideW for the current window width
float LayoutWidthFor(const Block& b, float textW, float wideW);
void BlockBox(uint32_t i, float* x, float* w);  // drawn box of a block (DIP, document x)
uint32_t FirstVisible(float y);
void InitGeometry();
void RecomputeY();
void EnsureVisible();
void InitialLayout();
BlockLayout* EnsureLayout(uint32_t i);
void ClearLayoutCache();
void TrimCache();
void Render();                       // draw the full frame into the canvas
void WithAnchor(void (*fn)());       // keep the top visible block in place while heights change

// positions & links
bool HitTestDoc(float x, float y, uint32_t* pos, bool* inside);  // client DIP → absolute text offset
int LinkAt(float x, float y);        // link index or -1
int CodeBlockAt(float x, float y, bool* onCopyButton);
void SelectAll();
void SelectWordAt(uint32_t pos);
void SelectBlockAt(uint32_t pos);
bool HasSelection();
std::wstring SelectionText();
std::wstring BlockPlainText(uint32_t i);
int HeadingBlockBySlug(const std::wstring& slug);
void ScrollToBlock(uint32_t i, bool animate);

// find
void FindUpdate(bool keepCurrent);
void FindStep(int dir);

// overlays
void ShowToast(const std::wstring& text, DWORD ms = 1200);

// ------------------------------------------------------------------------------------------------ loader.cpp
DWORD WINAPI StartupDocThread(void*);
void OpenDocument(const std::wstring& path, bool pushHistory, float scrollY);  // runtime (UI thread)
void ReloadDocument();
void NavigateBack();
void NavigateForward();
void StartBackgroundWork();          // after the first frame: measure, images, full doc, watcher
void OnMeasured(MeasureJob* job);
void OnFullDoc();
void OnImagesLoaded();
void StartMeasure();
void JoinWorkers();
void StartWatcher();
void StopWatcher();
HANDLE Spawn(LPTHREAD_START_ROUTINE fn, void* arg, int prio = THREAD_PRIORITY_NORMAL, SIZE_T stack = 0);
std::wstring WindowTitle();

// ------------------------------------------------------------------------------------------------ window.cpp
void Invalidate();
void ScrollTo(float y, bool animate);
void OpenLink(int linkIndex);
void CopyToClipboard(const std::wstring& text);
void ApplyTheme();                   // re-evaluate system / forced theme, repaint
