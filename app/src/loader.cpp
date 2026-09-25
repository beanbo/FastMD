// Document loading and background work.
//   start-up: StartupDocThread (read → parse → DirectWrite → first-viewport layout → render into the canvas) runs in
//             parallel with window creation; the first WM_PAINT only blits.
//   runtime : OpenDocument / Reload / history on the UI thread (big files: prefix parse first, full parse in background).
//   after the first frame of a document: exact heights on worker threads, WIC image decoding, file watcher.
#include "app.h"
#include "net.h"
#include "formulas.h"
#include "svg.h"
#include <shlobj.h>
#include <wincodec.h>

// documents above this size get a prefix parse for the first screen (the full parse runs concurrently)
static const size_t kPrefixThreshold = 256 * 1024, kPrefixChars = 48 * 1024;

// Closes the handles of workers that have finished, so a long session does not pile them up (the caller holds the lock).
static void DropFinishedWorkers() {
    auto& ws = g.workers;
    ws.erase(std::remove_if(ws.begin(), ws.end(), [](const Worker& w) {
                 if (WaitForSingleObject(w.h, 0) != WAIT_OBJECT_0) return false;
                 CloseHandle(w.h);
                 return true;
             }),
             ws.end());
}

HANDLE Spawn(LPTHREAD_START_ROUTINE fn, void* arg, int prio, SIZE_T stack, WorkerKind kind) {
    HANDLE th = CreateThread(nullptr, stack, fn, arg, 0, nullptr);
    if (!th) return nullptr;
    if (prio != THREAD_PRIORITY_NORMAL) SetThreadPriority(th, prio);
    AcquireSRWLockExclusive(&g.workersLock);
    DropFinishedWorkers();
    g.workers.push_back(Worker{th, kind});
    ReleaseSRWLockExclusive(&g.workersLock);
    return th;
}

void JoinWorkers() {
    g.gen++;      // measure threads check the layout generation and stop early
    g.docGen++;   // a full parse checks the document generation (a re-layout must not stop it)
    g.loadGen++;  // the picture worker skips what was queued for this document; its current job's result is dropped
    AcquireSRWLockExclusive(&g.workersLock);
    std::vector<Worker> ws;
    ws.swap(g.workers);
    ReleaseSRWLockExclusive(&g.workersLock);
    for (const Worker& w : ws) { WaitForSingleObject(w.h, INFINITE); CloseHandle(w.h); }
    g.jobsPending = 0;
    if (Doc* d = g.fullDoc.exchange(nullptr)) delete d;
    g.fullPending = false;
    g.renders.clear();  // a load renders everything anew (as it always did): the files may have changed meanwhile
}

// An edit replaces the model many times a second, so it waits only for what reads g.doc and stops within one block:
// the measure jobs. Everything else keeps running - the scaler holds its own pixels, the picture worker its own jobs,
// the updater nothing of the document (EDIT-MODE.md §5.1). The generations are bumped first, so a measure job in the
// middle of a big chunk gives up at its next block instead of finishing it.
void JoinDocReaders() {
    // the full parse reads g.src: edit mode is refused while it runs, so there is none to wait for here
    if (g.fullPending) DebugLog("JoinDocReaders while the full parse is pending");
    g.gen++;
    g.docGen++;
    std::vector<HANDLE> measure;
    AcquireSRWLockExclusive(&g.workersLock);
    DropFinishedWorkers();
    for (size_t i = 0; i < g.workers.size();) {
        if (g.workers[i].kind != WK_MEASURE) { i++; continue; }
        measure.push_back(g.workers[i].h);
        g.workers.erase(g.workers.begin() + i);
    }
    ReleaseSRWLockExclusive(&g.workersLock);
    for (HANDLE h : measure) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    g.jobsPending = 0;
}

const TestSlow& TestSlowMs() {
    static const TestSlow slow = [] {
        TestSlow t;
        wchar_t v[256];
        DWORD n = GetEnvironmentVariableW(L"FASTMD_TEST_SLOW", v, (DWORD)std::size(v));
        if (!n || n >= std::size(v)) return t;
        for (wchar_t *p = v, *end = nullptr; *p; p = *end ? end + 1 : end) {  // "images:300,scale:300"
            wchar_t* colon = wcschr(p, L':');
            if (!colon) break;
            *colon = 0;
            DWORD ms = 0;
            for (end = colon + 1; *end >= L'0' && *end <= L'9'; end++) ms = ms * 10 + (*end - L'0');
            if (!wcscmp(p, L"images")) t.images = ms;
            else if (!wcscmp(p, L"scale")) t.scale = ms;
            else if (!wcscmp(p, L"preview")) t.preview = ms;
            else if (!wcscmp(p, L"fullparse")) t.fullparse = ms;
            else if (!wcscmp(p, L"save")) t.save = ms;
        }
        return t;
    }();
    return slow;
}

std::wstring WindowTitle() {
    if (g.path.empty()) return L"FastMD";
    // unsaved edits: a star right after the name (so "name.md…" still starts the title, EDIT-MODE.md §2.6)
    std::wstring t = FileNameOf(g.path) + (EditDirty() ? L"*" : L"") + L" — FastMD";
    if (g.cfg.id[0]) t += std::wstring(L" (") + g.cfg.id + L")";
    return t;
}

// ------------------------------------------------------------------------------------------------ parse
static DWORD WINAPI FullParseThread(void* p) {
    uint32_t myGen = (uint32_t)(uintptr_t)p;
    if (DWORD ms = TestSlowMs().fullparse) Sleep(ms);
    Doc* d = new Doc();
    d->baseDir = g.doc.baseDir;
    ParseMarkdown(*d, g.src.data(), g.src.size());
    if (g.docGen != myGen) { delete d; return 0; }
    DebugLog("parsed_full");
    delete g.fullDoc.exchange(d);
    if (g.hwnd) PostMessageW(g.hwnd, WM_APP_FULLDOC, 0, 0);
    return 0;
}

// read + parse g.path into g.doc (g.src kept for a background full parse)
static void LoadSource(bool startup) {
    uint64_t tRead = 0;
    g.src.clear();
    g.loadFailed = false;
    if (!startup) g.reloads++;  // Q_RELOADS: how a test tells an edit or our own write from a reload
    // A baseline for saving is taken only when something is to be written (edit.cpp), from bytes read and checked
    // then; the load keeps what it learns for free: the encoding, the identity and the stamp, from the one handle.
    g.disk = DiskState();
    DiskBytes info;
    if (!ReadFileUtf16(g.path.c_str(), g.src, &tRead, &g.fileTime, &info)) {
        g.loadFailed = true;
        g.src = std::wstring(L"# ") + Tr(S_LOAD_FAILED) + L"\n\n`" + g.path + L"`\n";
        tRead = NowTicks();
        // The watcher's baseline is this stamp: a file that is there but cannot be read (locked, denied) must not look
        // changed to it, or every load would post the next reload at once.
        if (!GetFileStamp(g.path.c_str(), &g.fileTime, &g.fileSize)) {
            g.fileTime = {};
            g.fileSize = 0;
        }
    } else {
        static const char* kBom[] = {"", "", "\xFF\xFE", "\xEF\xBB\xBF", "", "\xEF\xBB\xBF\xFF\xFE"};
        g.fileSize = info.size;
        g.disk.cp = info.enc.cp;
        g.disk.header = info.enc.header < std::size(kBom) ? kBom[info.enc.header] : "";
        g.disk.volume = info.volume;
        g.disk.index = info.index;
        g.disk.mtime = info.mtime;
        g.disk.size = info.size;
        g.disk.attributes = info.attributes;
    }
    if (!startup) EditOnLoad();  // a new document session: its history and banners start over
    if (startup) MarkAt("file_read", tRead);
    g.doc = Doc();
    g.doc.baseDir = DirOf(g.path);
    size_t cut = g.src.size() > kPrefixThreshold ? FindPrefixCut(g.src.data(), g.src.size(), kPrefixChars) : g.src.size();
    if (cut < g.src.size()) {
        // big file: the first screen comes from a prefix that ends at a top-level heading (identical blocks); the
        // full model replaces it right after the first frame
        g.fullPending = true;
        Spawn(FullParseThread, (void*)(uintptr_t)(uint32_t)g.docGen, THREAD_PRIORITY_NORMAL, 0, WK_FULLPARSE);
        ParseMarkdown(g.doc, g.src.data(), cut);
        if (startup) Mark("parsed_prefix");
    } else {
        ParseMarkdown(g.doc, g.src.data(), g.src.size());
        if (startup) Mark("parsed");
    }
}

DWORD WINAPI StartupDocThread(void*) {
    if (!g.path.empty()) LoadSource(true);
    else if (!BenchActive()) {  // start screen: the recent documents are its content, read before the first frame
        PositionsLoad(g.recentAll);
        g.positions = g.recentAll;
        g.positionsLoaded = true;
        HomeRebuild();
    }
    g.docSerial++;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), (IUnknown**)&g.dwf);
    g.typo.Init(g.dwf);
    Mark("dwrite_ready");
    UpdateColumns();
    if (!g.path.empty()) InitialLayout();
    Mark("layout_first_viewport");
    g.canvas = CreateGdiCanvas(g.dwf, g.pxW, g.pxH, Scale());
    Render();
    g.offscreenValid = true;
    Mark("rendered_offscreen");
    return 0;
}

// ------------------------------------------------------------------------------------------------ runtime open
static void CancelPendingRestore();

static void ResetViewState() {
    CancelPendingRestore();  // a restore waiting for a big document's full parse belongs to that document only
    ClearLayoutCache();
    g.cache.clear();
    g.H.clear();
    g.Y.clear();
    g.known.clear();
    g.selAnchor = g.selFocus = 0;
    g.selecting = false;
    g.caretOn = false;
    g.caretWantX = -1.f;
    g.matches.clear();
    g.curMatch = -1;
    g.lowerText.clear();
    g.hoverLink = g.hoverCode = g.hoverHBlock = g.dragHBlock = g.hbarFlash = -1;
    g.hotHBar = false;
    g.hx.clear();
    g.focusLink = g.ctxLink = g.ctxImage = -1;
    g.hoverTask = g.downTask = -1;
    g.tocHover = -1;
    g.restoreBlock = -1;
    g.restored = false;
    g.scalingImages = false;  // JoinWorkers has already waited for the scaler; a late result matches no pixels any more
    g.docH = 0;
}

static bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}
static PosEntry CapturePosition();
static void RememberPosition(const PosEntry& e);
static void SaveAsync(const PosEntry& e, bool keepPosition);
static void RestorePositionFor(const std::wstring& path);
static void OnFullDocRestore();

void OpenDocument(const std::wstring& path, bool pushHistory, float scrollY, bool restorePosition) {
    // edits go into the file (or the reader decides what becomes of them) before the document goes (§10.8)
    if (!g.path.empty() && !CanLeaveDocument()) return;
    wchar_t full[MAX_PATH * 4];
    std::wstring p = GetFullPathNameW(path.c_str(), MAX_PATH * 4, full, nullptr) ? std::wstring(full) : path;
    bool other = g.path.empty() || !SamePath(g.path, p);
    bool track = !BenchActive();
    if (other && track && !g.path.empty() && !g.loadFailed) {  // leaving a document: remember where the reader was
        PosEntry e = CapturePosition();
        RememberPosition(e);
        SaveAsync(e, false);
    }
    if (pushHistory && !g.path.empty()) {
        g.back.push_back(HistoryEntry{g.path, g.scrollY});
        g.fwd.clear();
    }
    if (!g.path.empty()) EditLeaveDocument();  // a flush point for the document being left
    StopWatcher();
    JoinWorkers();
    ResetViewState();
    g.path = p;
    LoadSource(false);
    g.docSerial++;
    g.scrollY = g.targetY = scrollY;
    g.animating = false;
    g.userMoved = false;
    UpdateColumns();  // the docked outline depends on the document having headings
    InitialLayout();
    g.offscreenValid = false;
    SetWindowTextW(g.hwnd, WindowTitle().c_str());
    if (g.findOpen && !g.findQuery.empty()) FindUpdate(false);
    Invalidate();
    StartBackgroundWork();
    SHAddToRecentDocs(SHARD_PATHW, g.path.c_str());
    UiaDocumentChanged();  // a screen reader is reading this window: the document under it just changed
    if (other && track && !g.loadFailed) {
        PosEntry touch;
        touch.path = g.path;
        touch.opened = NowTicks();
        SaveAsync(touch, true);  // recently opened (start screen, positions of other windows stay intact)
        if (restorePosition && g.positionsLoaded) RestorePositionFor(g.path);
    }
}

void ReloadDocument() {
    if (g.path.empty()) return;
    float y = g.scrollY;
    uint32_t selA = g.selAnchor, selF = g.selFocus;
    std::vector<float> hx = g.hx;  // live reload keeps the horizontal scroll of wide blocks
    OpenDocument(g.path, false, y);
    uint32_t n = (uint32_t)g.doc.text.size();
    g.selAnchor = std::min(selA, n);
    g.selFocus = std::min(selF, n);
    for (size_t i = 0; i < hx.size() && i < g.hx.size(); i++) g.hx[i] = hx[i];
}

void NavigateBack() {
    if (g.back.empty() || !CanLeaveDocument()) return;  // before the history moves (T20)
    HistoryEntry e = g.back.back();
    g.back.pop_back();
    g.fwd.push_back(HistoryEntry{g.path, g.scrollY});
    OpenDocument(e.path, false, e.scrollY);
}

void NavigateForward() {
    if (g.fwd.empty() || !CanLeaveDocument()) return;
    HistoryEntry e = g.fwd.back();
    g.fwd.pop_back();
    g.back.push_back(HistoryEntry{g.path, g.scrollY});
    OpenDocument(e.path, false, e.scrollY);
}

// ------------------------------------------------------------------------------------------------ exact heights
static uint32_t MeasuredBlock(const MeasureJob* job, size_t k) { return job->idx.empty() ? job->from + (uint32_t)k : job->idx[k]; }

static DWORD WINAPI MeasureThread(void* p) {
    auto* job = (MeasureJob*)p;
    SetMeasureThread();  // no picture header from a share or the cloud: an edit waits for this thread (JoinDocReaders)
    Typography t;
    t.Init(g.dwf, &g.typo);
    for (size_t k = 0; k < job->h.size(); k++) {
        if (g.gen != job->gen || g.closing) break;
        uint32_t i = MeasuredBlock(job, k);
        const Block& b = g.doc.blocks[i];
        if (BlockHidden(b)) { job->h[k] = 0.f; continue; }  // folded <details>: no height
        float w = LayoutWidthFor(b, job->textW, job->wideW);
        bool exact = false;
        float h = BlockHeightEstimate(g.doc, t, b, w, &exact);
        if (!exact || b.kind == BK_IMAGE) {
            BlockLayout* L = LayoutBlock(g.doc, t, i, w);
            h = L->height;
            delete L;
        }
        job->h[k] = h;
    }
    t.Release();
    if (g.closing || !PostMessageW(g.hwnd, WM_APP_MEASURED, 0, (LPARAM)job)) delete job;
    return 0;
}

// Edit mode (EDIT-MODE.md §5.4): after a pause in typing only the blocks whose height is still a guess are measured,
// nearest to the viewport first, at most 64 at a time on one thread - the next edit stops the job within one block,
// and the next batch follows when this one is in. Reading mode does the same after a model swap (a ticked task box).
static bool g_measureUnknown = false;  // the batches in flight measure guessed heights only

static int MeasureThreads(size_t blocks) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)std::clamp<size_t>(blocks / 400, 1, std::min<DWORD>(8, std::max<DWORD>(1, si.dwNumberOfProcessors / 2)));
}

static void StartMeasureUnknown() {
    g_measureUnknown = true;
    size_t n = g.doc.blocks.size();
    if (g.jobsPending || !n) return;
    if (!g.editing) {
        // Reading mode with many guesses left (a tick landed while a big document was still being measured): nothing
        // is typed next, so they are split over the threads StartMeasure uses, all at once - batches of 64 on one
        // thread, each followed by a re-layout of the view, would take seconds there.
        std::vector<uint32_t> unknown;
        for (uint32_t i = 0; i < n; i++)
            if (!g.known[i]) unknown.push_back(i);
        if (unknown.size() > 512) {
            g_measureUnknown = false;
            int threads = MeasureThreads(unknown.size());
            size_t chunk = (unknown.size() + threads - 1) / threads;
            for (size_t from = 0; from < unknown.size(); from += chunk) {
                auto* job = new MeasureJob{g.gen, 0, 0, g.textW, g.wideW, {}, {}};
                job->idx.assign(unknown.begin() + from, unknown.begin() + std::min(unknown.size(), from + chunk));
                job->h.assign(job->idx.size(), 0.f);
                g.jobsPending++;
                if (!Spawn(MeasureThread, job, THREAD_PRIORITY_BELOW_NORMAL, 256 * 1024, WK_MEASURE)) { g.jobsPending--; delete job; }
            }
            return;
        }
    }
    uint32_t a = std::min<uint32_t>(FirstVisible(g.scrollY), (uint32_t)n - 1);
    auto* job = new MeasureJob{g.gen, 0, 0, g.textW, g.wideW, {}, {}};
    for (size_t d = 0; job->idx.size() < 64 && (d <= a || a + d < n); d++) {  // outwards from the top of the view
        if (a + d < n && !g.known[a + d]) job->idx.push_back(a + (uint32_t)d);
        if (d && d <= a && !g.known[a - d] && job->idx.size() < 64) job->idx.push_back(a - (uint32_t)d);
    }
    if (job->idx.empty()) { delete job; return; }
    job->h.assign(job->idx.size(), 0.f);
    g.jobsPending++;
    if (!Spawn(MeasureThread, job, THREAD_PRIORITY_BELOW_NORMAL, 256 * 1024, WK_MEASURE)) { g.jobsPending--; delete job; }
}

void MeasureUnknown() { StartMeasureUnknown(); }

void StartMeasure() {
    if (g.editing) { StartMeasureUnknown(); return; }
    g_measureUnknown = false;
    size_t n = g.doc.blocks.size();
    size_t unknown = 0;
    for (size_t i = 0; i < n; i++) unknown += !g.known[i];
    if (!unknown) return;
    int threads = MeasureThreads(n);
    size_t chunk = (n + threads - 1) / threads;
    for (int k = 0; k < threads; k++) {
        auto* job = new MeasureJob{g.gen, (uint32_t)(k * chunk), (uint32_t)std::min(n, (k + 1) * chunk), g.textW, g.wideW, {}, {}};
        if (job->from >= job->to) { delete job; continue; }
        job->h.assign(job->to - job->from, 0.f);
        g.jobsPending++;
        if (!Spawn(MeasureThread, job, THREAD_PRIORITY_BELOW_NORMAL, 256 * 1024, WK_MEASURE)) { g.jobsPending--; delete job; }
    }
}

void OnMeasured(MeasureJob* job) {
    if (job->gen == g.gen) {
        g.jobsPending--;
        static MeasureJob* cur;
        cur = job;
        WithAnchor([] {
            for (size_t k = 0; k < cur->h.size(); k++) {
                uint32_t i = MeasuredBlock(cur, k);
                if (!g.cache[i]) g.H[i] = cur->h[k];
                g.known[i] = 1;
            }
            RecomputeY();
        });
        ResolveRestore();
        if (g.jobsPending == 0) {
            Mark("measured_all");
            DebugFlush();
            if (g_measureUnknown) StartMeasureUnknown();  // the next batch of guessed heights
        }
        Invalidate();
    }
    delete job;
}

// ------------------------------------------------------------------------------------------------ pictures
// Pictures, formulas and diagrams are made on one long-lived worker thread that never touches the document
// (EDIT-MODE.md §5.2). StartImages hands it jobs by value, one per source and render context, and the results come
// back in batches that the UI thread puts on every entry of the document showing that source. So the model can be
// replaced at any moment - a reload no longer waits for a download, an edit will not wait for a diagram - the same
// formula is rendered once however often it appears, nothing is queued twice, and a failure is remembered instead of
// being tried again on every occasion. The render table (g.renders) is the memory of all of that.
namespace {
struct ImageJob {
    std::wstring key;         // render-table key
    uint32_t ctx, loadGen;
    uint8_t kind;             // Image::mathKind
    std::string math;
    std::wstring path, url;
    float fontPx;             // the render context of a formula or a diagram, as it was when the job was queued
    uint32_t rgb;
    bool dark;
};
}  // namespace

static SRWLOCK g_jobLock = SRWLOCK_INIT;
static std::vector<ImageJob> g_jobs;           // queued, oldest first (under g_jobLock)
static HANDLE g_jobEvent = nullptr;            // auto-reset: something was queued
static std::vector<std::wstring> g_imgKeys;    // the render-table key of every entry of g.doc.images ...
static uint32_t g_imgKeysSerial = UINT32_MAX, g_imgKeysCtx = 0;  // ... for this document and render context
static uint64_t g_useTick = 0;                 // RenderEntry::lastUse

// A formula is typeset in the text colour at the text size, a diagram in the theme's colours, so its pixels belong to
// that context; a picture from a file looks the same in every one (context 0).
uint32_t MathContext() {
    const uint32_t parts[] = {(uint32_t)g.cfg.fontSize, g_pal[P_TEXT], (uint32_t)PaletteIsDark()};
    uint32_t h = 2166136261u;
    for (uint32_t v : parts) h = (h ^ v) * 16777619u;
    return h;
}

// the source (`m<kind>:<TeX or Mermaid>`, `u:<url>`, `p:<path>`, the same shape as editcore's PictureKey), then
// '\x1f' and the context in hex
static std::wstring RenderKey(const Image& im, uint32_t mctx) {
    std::wstring k = im.mathKind ? L"m0:" : im.url.empty() ? L"p:" : L"u:";
    if (im.mathKind) k[1] = (wchar_t)(L'0' + im.mathKind);
    k += im.mathKind ? im.alt : im.url.empty() ? im.path : im.url;
    uint32_t ctx = im.mathKind ? mctx : 0;
    k.push_back(L'\x1f');
    for (int s = 28; s >= 0; s -= 4) k.push_back(L"0123456789abcdef"[(ctx >> s) & 15]);
    return k;
}

std::wstring ImageRenderKey(const Image& im) { return RenderKey(im, MathContext()); }

static const std::vector<std::wstring>& ImageKeys(uint32_t mctx) {
    if (g_imgKeysSerial != g.docSerial || g_imgKeysCtx != mctx || g_imgKeys.size() != g.doc.images.size()) {
        g_imgKeys.clear();
        for (const Image& im : g.doc.images) g_imgKeys.push_back(RenderKey(im, mctx));
        g_imgKeysSerial = g.docSerial;
        g_imgKeysCtx = mctx;
    }
    return g_imgKeys;
}

static void PostImages(std::vector<RenderResult>*& batch) {
    if (!batch) return;
    if (g.closing || !g.hwnd || !PostMessageW(g.hwnd, WM_APP_IMAGES, 0, (LPARAM)batch)) delete batch;
    batch = nullptr;
}

// A formula or a diagram (plan 4.1, 4.2): its source becomes SVG, and from there it is a vector picture like any other,
// drawn at `scale` pixels per DIP (the preview worker draws at the screen's, §9.4) - its longer side at most 4096 (both
// sides clamped on their own would squash a big diagram: it is scaled down whole). r gets the layout size in DIP.
bool RenderMath(uint8_t kind, const std::string& src, float fontPx, uint32_t rgb, bool dark, float scale, Pixels& pix,
                RenderResult& r) {
    float mw = 0, mh = 0, asc = 0;
    bool ok = kind == 3 ? MermaidSvg(src, dark, pix.svg) : TexSvg(src, kind == 2, fontPx, rgb, pix.svg, &mw, &mh, &asc);
    if (ok && (mw <= 0 || mh <= 0)) ok = SvgMeasure(pix.svg.data(), pix.svg.size(), &mw, &mh);
    if (!ok || mw < 1.f || mh < 1.f) return false;
    const float kd = std::min(1.f, 4096.f / std::max(mw, mh)), kp = std::min(scale * kd, 4096.f / std::max(mw, mh));
    const int w = std::max(1, (int)std::lround(mw * kp)), h = std::max(1, (int)std::lround(mh * kp));
    if (!SvgRender(pix.svg.data(), pix.svg.size(), w, h, pix.px)) return false;
    pix.pxW = w;
    pix.pxH = h;
    r.w = std::max(1, (int)std::lround(mw * kd));
    r.h = std::max(1, (int)std::lround(mh * kd));
    r.ascent = asc > 0 ? asc * kd : (float)r.h;
    return r.ok = true;
}

// One job, on the worker: the source becomes pixels, or a failure. True when it went to the network (the picture is
// then handed over as soon as it arrives rather than with the next batch).
static bool RenderImage(const ImageJob& job, IWICImagingFactory*& wic, RenderResult& r, bool& fetched) {
    r.key = job.key;
    r.ctx = job.ctx;
    r.loadGen = job.loadGen;
    r.math = job.kind != 0;
    auto pix = std::make_shared<Pixels>();
    if (job.kind) {
        RenderMath(job.kind, job.math, job.fontPx, job.rgb, job.dark, 1.f, *pix, r);
    } else {
        std::wstring path = job.path;
        if (!job.url.empty()) {
            // From the network, strictly after the first frame: the cache file is the picture's path from then on. The
            // setting is asked once more here: it may have changed while the job waited in the queue.
            path.clear();
            if (RemoteImagesAllowed()) {
                std::wstring cache = CacheFileFor(job.url);
                if (GetFileAttributesW(cache.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    std::vector<uint8_t> body;
                    if (HttpGet(job.url, body, 16u << 20, false)) {
                        CreateDirectoryW((DataDir() + L"cache").c_str(), nullptr);
                        std::wstring tmp = cache + L".part";
                        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                        if (f != INVALID_HANDLE_VALUE) {
                            DWORD wrote = 0;
                            bool okw = WriteFile(f, body.data(), (DWORD)body.size(), &wrote, nullptr) && wrote == body.size();
                            CloseHandle(f);
                            if (okw && MoveFileExW(tmp.c_str(), cache.c_str(), MOVEFILE_REPLACE_EXISTING)) path = cache;
                            else DeleteFileW(tmp.c_str());
                        }
                    }
                } else {
                    path = cache;
                }
                fetched = true;
            }
            r.cachePath = path;
        }
        std::vector<uint8_t> bytes;  // SVG is drawn by fastmd-svg.dll; everything else goes through WIC
        if (!path.empty() && ReadFileBytes(path.c_str(), bytes, 16u << 20) && IsSvgData(bytes.data(), bytes.size())) {
            float sw = 0, sh = 0;
            if (SvgMeasure(bytes.data(), bytes.size(), &sw, &sh) && sw >= 1.f && sh >= 1.f) {
                int w = std::clamp((int)std::lround(sw), 1, 4096), h = std::clamp((int)std::lround(sh), 1, 4096);
                if (SvgRender(bytes.data(), bytes.size(), w, h, pix->px)) {
                    pix->pxW = r.w = w;
                    pix->pxH = r.h = h;
                    pix->svg.swap(bytes);
                    r.ok = true;
                }
            }
        } else if (!path.empty()) {
            if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
            IWICBitmapDecoder* dec = nullptr;
            IWICBitmapFrameDecode* fr = nullptr;
            IWICFormatConverter* conv = nullptr;
            if (wic && SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
                SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                UINT w = 0, h = 0;
                conv->GetSize(&w, &h);
                pix->px.resize((size_t)w * h);
                if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)(pix->px.size() * 4), (BYTE*)pix->px.data()))) {
                    pix->pxW = r.w = (int)w;  // the decoded size (a GIF frame may be smaller than its header's screen size)
                    pix->pxH = r.h = (int)h;
                    r.ok = true;
                }
            }
            SafeRelease(conv);
            SafeRelease(fr);
            SafeRelease(dec);
        }
        // a file that could not be read is tried again when it changes (the window's activation looks, §5.2)
        if (!r.ok && job.url.empty()) GetFileStamp(path.c_str(), &r.failTime, &r.failSize);
    }
    if (r.ok) {
        pix->serial = NewPixelSerial();
        r.pix = std::move(pix);
    }
    return !job.url.empty();
}

// The worker: started with the first picture of the process and alive as long as the process. It stops only for a
// load (a job of an older loadGen is skipped) or the window closing - never for an edit.
static DWORD WINAPI PictureWorker(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC; the apartment is kept for the life of the thread
    IWICImagingFactory* wic = nullptr;
    std::vector<ImageJob> todo;
    size_t next = 0;
    std::vector<RenderResult>* batch = nullptr;
    ULONGLONG batchStart = 0;
    bool fetched = false;
    while (!g.closing) {
        if (next == todo.size()) {
            todo.clear();
            next = 0;
            AcquireSRWLockExclusive(&g_jobLock);
            todo.swap(g_jobs);
            ReleaseSRWLockExclusive(&g_jobLock);
            if (todo.empty()) {  // nothing to do: hand over what is done, then sleep until something is queued
                PostImages(batch);
                if (fetched) TrimHttpCache();
                fetched = false;
                WaitForSingleObject(g_jobEvent, INFINITE);
            }
            continue;
        }
        const ImageJob& job = todo[next++];
        if (job.loadGen != g.loadGen) continue;  // queued for a document that is not open any more
        g.rendersStarted++;
        if (DWORD ms = TestSlowMs().images) Sleep(ms);
        if (!batch) {
            batch = new std::vector<RenderResult>();
            batchStart = GetTickCount64();
        }
        batch->emplace_back();
        bool net = RenderImage(job, wic, batch->back(), fetched);
        // handed over in batches (every 50 ms or 8 pictures), so a document of many formulas is not re-laid out for
        // each; a download at once, since it took its time
        if (net || batch->size() >= 8 || GetTickCount64() - batchStart >= 50) PostImages(batch);
    }
    return 0;
}

static void QueueImageJobs(std::vector<ImageJob>& jobs) {
    AcquireSRWLockExclusive(&g_jobLock);
    for (ImageJob& j : jobs) g_jobs.push_back(std::move(j));
    ReleaseSRWLockExclusive(&g_jobLock);
    if (!g_jobEvent) {
        g_jobEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE th = g_jobEvent ? CreateThread(nullptr, 0, PictureWorker, nullptr, 0, nullptr) : nullptr;
        if (th) {
            SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(th);  // detached: nothing ever waits for it
        }
    }
    if (g_jobEvent) SetEvent(g_jobEvent);
}

// Puts what the table knows about a source on one entry of the document; true when that changes what is drawn. keep: a
// failure keeps the picture shown, outlined as one that no longer renders (the formula a popup edits, §9.4) - or shows
// the one it brought: the popup's last good source drawn for the context of now.
static bool ShowEntry(Image& im, const RenderEntry& e, const std::wstring& key, bool keep = false) {
    if (e.state == RS_FAILED && keep && im.state == RS_OK && im.pix) {
        bool was = im.renderFailed;
        im.renderFailed = true;
        if (!e.pix || e.pix == im.pix) return !was;
        im.pix = e.pix;
        im.sc.reset();
        im.w = e.w;
        im.h = e.h;
        im.ascent = e.ascent;
        return true;
    }
    if (im.state == e.state && im.pix == e.pix) return false;
    if (e.state == RS_PENDING) {  // nothing new yet: a picture already shown stays until its successor arrives
        if (im.state == RS_NONE) im.state = RS_PENDING;
        return false;
    }
    im.state = e.state;
    im.renderFailed = false;
    if (e.state == RS_OK) {
        im.pix = e.pix;
        im.sc = e.sc;
        im.pxFor = key;
        if (im.mathKind) {  // the real size replaces the parser's guess
            im.w = e.w;
            im.h = e.h;
            im.ascent = e.ascent;
        } else if (im.w <= 0) {  // a picture whose header said nothing (a download, an unreadable header)
            im.w = e.w;
            im.h = e.h;
        }
    } else {
        im.pix.reset();
        im.sc.reset();
    }
    if (!e.cachePath.empty()) im.path = e.cachePath;  // a remote picture: its file in the download cache (R20)
    return true;
}

// The entries in `changed` draw differently now. The blocks that show one lose their layout (a picture's size is part
// of it) and a picture block takes the picture's height; the view keeps its place.
static const std::vector<uint8_t>* g_changed;

static bool RunsShowChanged(uint32_t runOff, uint32_t runCount) {
    for (uint32_t k = 0; k < runCount; k++) {
        const Run& r = g.doc.runs[runOff + k];
        if ((r.flags & F_IMAGE) && r.image < g_changed->size() && (*g_changed)[r.image]) return true;
    }
    return false;
}

static void RefreshImageBlocks(const std::vector<uint8_t>& changed) {
    g.pixelSerial++;  // the same layout may now draw differently: a scrolled frame must not reuse the old pixels
    g_changed = &changed;
    WithAnchor([] {
        const Doc& d = g.doc;
        for (size_t i = 0; i < d.blocks.size(); i++) {
            const Block& b = d.blocks[i];
            bool hit = b.kind == BK_IMAGE ? b.aux < g_changed->size() && (*g_changed)[b.aux]
                                          : RunsShowChanged(b.runOff, b.runCount);  // a badge inside a line
            if (!hit && b.kind == BK_TABLE && b.aux < d.tables.size()) {           // ... or inside a cell
                const Table& t = d.tables[b.aux];
                for (uint32_t c = 0; c < t.rows * t.cols && !hit; c++)
                    hit = RunsShowChanged(d.cells[t.cellOff + c].runOff, d.cells[t.cellOff + c].runCount);
            }
            if (!hit) continue;
            if (g.cache[i]) { delete g.cache[i]; g.cache[i] = nullptr; g.cachedCount--; }
            if (b.kind == BK_IMAGE)
                g.H[i] = ImageDisplayHeight(d.images[b.aux], LayoutWidthFor(b, g.textW, g.wideW) - b.indent);
        }
        RecomputeY();
    });
    ResolveRestore();
    Invalidate();
}

// Pixels the document no longer shows (an edit took their source away) are kept for a while - an undo brings the source
// back - but not beyond 64 MB: past that the least recently shown go. StartImages has just stamped every entry the
// document shows with a use from `shownFrom` on, so the others are exactly the older stamps.
static void EvictRenders(uint64_t shownFrom) {
    const size_t budget = 64u << 20;
    auto bytes = [](const RenderEntry& e) -> size_t { return e.pix ? e.pix->px.size() * 4 + e.pix->svg.size() : 0; };
    size_t spare = 0;
    for (const auto& kv : g.renders)
        if (kv.second.lastUse < shownFrom) spare += bytes(kv.second);
    while (spare > budget) {
        auto old = g.renders.end();
        for (auto it = g.renders.begin(); it != g.renders.end(); ++it)
            if (it->second.lastUse < shownFrom && it->second.pix &&
                (old == g.renders.end() || it->second.lastUse < old->second.lastUse))
                old = it;
        spare -= bytes(old->second);
        g.renders.erase(old);
    }
}

// An edit replaces the model (EDIT-MODE.md §5.5 step 4). Before the new one is installed it gets what is known about its
// pictures: what the render table holds (so an unchanged formula keeps its real size and the block diff sees nothing
// changed), the header size and display-size copy the old model had for the same source, and - for a formula or a
// diagram whose source is being typed, which the table does not know yet - the picture of the one it replaces, until
// its own render arrives. That borrowed picture is never stored under the new key (R15).
void CarryRenders(const Doc& oldD, Doc& nd, uint32_t editBeg, uint32_t oldEnd, uint32_t newEnd) {
    if (nd.images.empty()) return;
    const uint32_t mctx = MathContext();
    std::unordered_map<std::wstring, uint32_t> before;
    for (uint32_t i = 0; i < oldD.images.size(); i++) before.emplace(RenderKey(oldD.images[i], mctx), i);
    for (Image& im : nd.images) {
        std::wstring key = RenderKey(im, mctx);
        auto o = before.find(key);
        const Image* oi = o != before.end() ? &oldD.images[o->second] : nullptr;
        if (oi && !im.mathKind && im.w < 0) {  // the header the old model already read (layout reads it lazily)
            im.w = oi->w;
            im.h = oi->h;
        }
        auto it = g.renders.find(key);
        // a formula being edited that does not render keeps showing the picture it replaces (§9.4: outlined) - through a
        // theme switch too, while its popup is open
        const bool edited = im.mathKind && im.outerBeg != UINT32_MAX &&
                            ((editBeg != UINT32_MAX && im.outerBeg <= newEnd && im.outerEnd >= editBeg) || EditPopupHolds(im));
        const bool failed = edited && it != g.renders.end() && it->second.state == RS_FAILED;
        if (it != g.renders.end() && !failed) {
            ShowEntry(im, it->second, key);
            if (oi && oi->pix == im.pix && oi->sc) {  // its own display-size copy, not the one the table kept last
                im.sc = oi->sc;
                im.wantW = oi->wantW;
                im.wantH = oi->wantH;
            }
            continue;
        }
        // The table has nothing for it in this render context (the text size or theme changed since it was drawn, or
        // the entry was evicted): it shows the old model's picture of the same source meanwhile - with that picture's
        // size, so the block diff sees nothing changed - until its own render arrives (StartImages queues it, ShowEntry
        // keeps these pixels while it is pending). pxFor stays the old key: it is not the picture of the new one.
        if (oi && oi->state == RS_OK && oi->pix) {
            im.pix = oi->pix;
            im.sc = oi->sc;
            im.w = oi->w;
            im.h = oi->h;
            im.ascent = oi->ascent;
            im.pxFor = oi->pxFor;
            im.state = RS_OK;
            im.renderFailed = oi->renderFailed || failed;
            im.wantW = oi->wantW;
            im.wantH = oi->wantH;
            continue;
        }
        if (!im.mathKind || editBeg == UINT32_MAX || im.outerBeg == UINT32_MAX || im.outerBeg > newEnd || im.outerEnd < editBeg)
            continue;
        for (const Image& was : oldD.images) {
            if (was.mathKind != im.mathKind || !was.pix || was.outerBeg == UINT32_MAX || was.outerBeg > oldEnd ||
                was.outerEnd < editBeg)
                continue;
            im.pix = was.pix;
            im.sc = was.sc;
            im.w = was.w;
            im.h = was.h;
            im.ascent = was.ascent;
            im.pxFor = was.pxFor;
            im.state = RS_OK;
            im.renderFailed = was.renderFailed || failed;
            im.wantW = was.wantW;
            im.wantH = was.wantH;
            break;
        }
    }
}

void StartImages() {
    if (g.doc.images.empty() || g.closing) return;
    const uint64_t shownFrom = g_useTick + 1;
    const uint32_t mctx = MathContext();
    const std::vector<std::wstring>& keys = ImageKeys(mctx);
    const float fontPx = (float)g.cfg.fontSize;
    const uint32_t rgb = g_pal[P_TEXT];
    const bool dark = PaletteIsDark(), remoteOk = RemoteImagesAllowed();
    std::vector<ImageJob> jobs;
    std::vector<uint8_t> changed(keys.size(), 0);
    bool any = false;
    for (size_t i = 0; i < keys.size(); i++) {
        Image& im = g.doc.images[i];
        auto it = g.renders.find(keys[i]);
        if (it == g.renders.end()) {  // a source not seen yet (whatever state a known one is in, it is not queued again)
            RenderEntry e;
            bool remote = !im.mathKind && !im.url.empty();
            // a picture that may not be fetched (the setting says ask or never), or that has no file: nothing to render
            if (remote ? !remoteOk : !im.mathKind && im.path.empty()) e.state = RS_FAILED;
            else if (!im.mathKind || !EditPreviewClaim(im, keys[i]))  // (the formula a popup edits: the preview worker's)
                jobs.push_back(ImageJob{keys[i], im.mathKind ? mctx : 0, g.loadGen.load(), im.mathKind, im.math, im.path,
                                        im.url, fontPx, rgb, dark});
            it = g.renders.emplace(keys[i], std::move(e)).first;
        }
        it->second.lastUse = ++g_useTick;
        if (ShowEntry(im, it->second, keys[i], im.renderFailed)) changed[i] = any = true;
    }
    if (!jobs.empty()) QueueImageJobs(jobs);
    if (any) RefreshImageBlocks(changed);
    EvictRenders(shownFrom);
}

void OnImagesLoaded(std::vector<RenderResult>* batch) {
    if (!batch) return;
    if (g.closing) { delete batch; return; }
    const uint32_t mctx = MathContext();
    const std::vector<std::wstring>& keys = ImageKeys(mctx);
    std::vector<uint8_t> changed(keys.size(), 0);
    bool any = false, again = false;
    for (RenderResult& r : *batch) {
        if (r.loadGen != g.loadGen) continue;  // made for a document that is not open any more
        if (r.math && r.ctx != mctx) {
            // typeset for another theme or text size (it changed while this was rendering): not shown; a source the
            // document still has is rendered again for the context of now
            g.renders.erase(r.key);
            again = true;
            continue;
        }
        auto it = g.renders.find(r.key);
        if (it == g.renders.end()) it = g.renders.emplace(r.key, RenderEntry()).first;
        RenderEntry& e = it->second;
        e.state = r.ok ? RS_OK : RS_FAILED;
        e.pix = std::move(r.pix);
        e.sc.reset();
        e.w = r.w;
        e.h = r.h;
        e.ascent = r.ascent;
        e.cachePath = std::move(r.cachePath);
        e.failTime = r.failTime;
        e.failSize = r.failSize;
        e.error = std::move(r.error);
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == r.key && ShowEntry(g.doc.images[i], e, r.key, r.keep)) changed[i] = any = true;
    }
    delete batch;
    if (any) RefreshImageBlocks(changed);
    if (again) StartImages();
}

void RetryChangedPictures() {
    // The looks are file-system calls on the UI thread: not inside a modal loop (a print job's pages are cut already),
    // at most every 2 s, and never on a network drive, where an unreachable share would hang the window for its
    // timeout on every Alt+Tab - those are tried again at the next load.
    static ULONGLONG last = 0;
    if (g.editModal > 0 || GetTickCount64() - last < 2000) return;
    last = GetTickCount64();
    bool any = false;
    for (auto it = g.renders.begin(); it != g.renders.end();) {
        const std::wstring& k = it->first;
        if (it->second.state == RS_FAILED && k.compare(0, 2, L"p:") == 0) {
            std::wstring path = k.substr(2, k.rfind(L'\x1f') - 2);
            FILETIME t{};
            uint64_t size = 0;
            if (!path.empty() && !IsNetworkPath(path) && GetFileStamp(path.c_str(), &t, &size) &&
                (CompareFileTime(&t, &it->second.failTime) != 0 || size != it->second.failSize)) {
                it = g.renders.erase(it);
                any = true;
                continue;
            }
        }
        ++it;
    }
    if (any) StartImages();
}

bool RemoteImagesAllowed() { return g.cfg.remoteImages == 0 || (g.cfg.remoteImages == 1 && g.remoteAllowedOnce); }

bool DocHasRemoteImages() {
    for (const Image& im : g.doc.images)
        if (!im.url.empty() && im.path.empty()) return true;
    return false;
}

void LoadRemoteImages() {  // "ask": the reader said yes for this document
    if (!DocHasRemoteImages()) return;
    g.remoteAllowedOnce = true;
    for (auto it = g.renders.begin(); it != g.renders.end();)  // what was not fetched is fetched now
        it = it->second.state == RS_FAILED && it->first.compare(0, 2, L"u:") == 0 ? g.renders.erase(it) : std::next(it);
    StartImages();
}

// --------------------------------------------------------------------------------------- display-size copies (WIC)
// Drawing a picture straight from its decoded pixels means scaling every pixel of every frame by the nearest
// neighbour: slow while scrolling and visibly jagged when the picture is bigger than its column. So the canvas
// records the size it drew at, and this thread makes a copy at exactly that size with a real filter; after that a
// frame only copies rows. A copy is remade when the size changes (zoom, column width, window resize).
// The jobs hold their own reference to the pixels, and the copies come back keyed by the pixels' serial, so the
// scaler never needs the document: replacing it (or a picture in it) neither waits for the scaler nor gets a copy of
// the wrong picture (EDIT-MODE.md §5.3).
static const size_t kScaledBudget = 48u << 20;  // bytes of display-size copies kept outside the viewport

namespace {
struct ScaleJob { std::wstring key; std::shared_ptr<const Pixels> pix; int w, h; };
}  // namespace

static DWORD WINAPI ScaleThread(void* p) {
    auto* jobs = (std::vector<ScaleJob>*)p;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    auto* out = new std::vector<ScaledImage>();
    for (ScaleJob& j : *jobs) {
        if (g.closing) break;
        if (DWORD ms = TestSlowMs().scale) Sleep(ms);
        const Pixels& im = *j.pix;
        std::vector<uint32_t> px;
        bool ok = false;
        if (!im.svg.empty()) {  // vector art: drawn again at the new size instead of scaling pixels
            ok = SvgRender(im.svg.data(), im.svg.size(), j.w, j.h, px);
        } else if (wic) {
            IWICBitmap* src = nullptr;
            IWICBitmapScaler* scaler = nullptr;
            px.resize((size_t)j.w * j.h);
            ok = SUCCEEDED(wic->CreateBitmapFromMemory((UINT)im.pxW, (UINT)im.pxH, GUID_WICPixelFormat32bppPBGRA,
                                                       (UINT)im.pxW * 4, (UINT)(im.px.size() * 4), (BYTE*)im.px.data(), &src)) &&
                 SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                 SUCCEEDED(scaler->Initialize(src, (UINT)j.w, (UINT)j.h, WICBitmapInterpolationModeFant)) &&
                 SUCCEEDED(scaler->CopyPixels(nullptr, (UINT)j.w * 4, (UINT)(px.size() * 4), (BYTE*)px.data()));
            SafeRelease(scaler);
            SafeRelease(src);
        }
        // a failure is reported too (empty pixels): the size is then known to be unusable and is not retried
        out->push_back(ScaledImage{std::move(j.key), im.serial, j.w, j.h, ok ? std::move(px) : std::vector<uint32_t>()});
    }
    SafeRelease(wic);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    delete jobs;
    // always handed back, even after stopping early: that is how the UI thread learns that no scaler runs any more
    if (g.closing || !g.hwnd || !PostMessageW(g.hwnd, WM_APP_SCALED, 0, (LPARAM)out)) delete out;
    return 0;
}

void ScheduleImageScaling() {
    if (g.scalingImages || g.firstFrame || !g.hwnd || g.closing) return;
    std::vector<ScaleJob>* jobs = nullptr;
    bool shared = false;
    for (Image& im : g.doc.images) {
        const Pixels* p = im.pix.get();
        int w = im.wantW, h = im.wantH;
        if (im.state != RS_OK || !p || w <= 0 || h <= 0 || (w == p->pxW && h == p->pxH)) continue;
        if (im.sc && im.sc->serial == p->serial && im.sc->w == w && im.sc->h == h) continue;
        // the same picture elsewhere in the document may have been given that copy already
        auto it = g.renders.find(im.pxFor);
        if (it != g.renders.end()) {
            const Scaled* s = it->second.sc.get();
            if (s && s->serial == p->serial && s->w == w && s->h == h) {
                im.sc = it->second.sc;
                shared = true;
                continue;
            }
        }
        if (!jobs) jobs = new std::vector<ScaleJob>();
        bool queued = false;
        for (const ScaleJob& j : *jobs) queued |= j.pix.get() == p && j.w == w && j.h == h;
        if (!queued) jobs->push_back(ScaleJob{im.pxFor, im.pix, w, h});
    }
    if (shared) {
        g.pixelSerial++;
        Invalidate();
    }
    if (!jobs) return;
    g.scalingImages = true;
    if (!Spawn(ScaleThread, jobs, THREAD_PRIORITY_BELOW_NORMAL, 0, WK_SCALE)) {
        g.scalingImages = false;
        delete jobs;
    }
}

// keep the display-size copies of the pictures around the viewport; drop the rest once they add up (a long gallery)
static void TrimScaledImages() {
    size_t total = 0;  // (a copy two entries share counts twice: trimmed a little early, which does no harm)
    for (const Image& im : g.doc.images)
        if (im.sc) total += im.sc->px.size() * 4;
    if (total <= kScaledBudget) return;
    std::vector<uint8_t> keep(g.doc.images.size(), 0);
    float top = g.scrollY - ViewH(), bottom = g.scrollY + 2 * ViewH();
    for (size_t i = 0; i < g.doc.blocks.size() && i < g.Y.size(); i++) {
        const Block& b = g.doc.blocks[i];
        if (b.kind != BK_IMAGE || b.aux >= keep.size() || g.Y[i] + g.H[i] < top || g.Y[i] > bottom) continue;
        keep[b.aux] = 1;
    }
    for (size_t i = 0; i < g.doc.images.size(); i++) {
        Image& im = g.doc.images[i];
        if (keep[i] || !im.sc) continue;
        auto it = g.renders.find(im.pxFor);
        if (it != g.renders.end() && it->second.sc == im.sc) it->second.sc.reset();
        im.sc.reset();
        im.wantW = 0;
        im.wantH = 0;
    }
}

void OnScaledImages(std::vector<ScaledImage>* list) {
    g.scalingImages = false;  // always: a scaler posts even when it stopped early, so the flag can never stick
    if (!list) return;
    if (!g.closing) {
        bool any = false;
        for (ScaledImage& s : *list) {
            auto sc = std::make_shared<Scaled>();
            sc->px = std::move(s.px);
            sc->w = s.w;
            sc->h = s.h;
            sc->serial = s.pxSerial;
            // only for the pixels it was made from: a picture replaced meanwhile has another serial
            auto it = g.renders.find(s.key);
            if (it != g.renders.end() && it->second.pix && it->second.pix->serial == s.pxSerial) it->second.sc = sc;
            for (Image& im : g.doc.images)
                if (im.pix && im.pix->serial == s.pxSerial && im.wantW == s.w && im.wantH == s.h) {
                    im.sc = sc;
                    any = true;
                }
        }
        if (any) {
            g.pixelSerial++;
            TrimScaledImages();
            Invalidate();
        }
        ScheduleImageScaling();  // the size may have changed again while this batch was being made
    }
    delete list;
}

// ------------------------------------------------------------------------------------------------ after first frame
void OnFullDoc() {
    Doc* d = g.fullDoc.exchange(nullptr);
    if (!d || g.closing) { delete d; return; }
    static Doc* nd;
    nd = d;
    WithAnchor([] {
        ClearLayoutCache();
        g.doc = std::move(*nd);
        UpdateColumns();  // headings may appear only now (outline docking)
        InitGeometry();
        RecomputeY();
    });
    delete d;
    g.docSerial++;
    g.fullPending = false;
    g.lowerText.clear();
    if (g.findOpen && !g.findQuery.empty()) FindUpdate(true);
    OnFullDocRestore();
    Mark("full_doc_swapped");
    StartMeasure();
    StartImages();
    Invalidate();
    UiaDocumentChanged();  // a screen reader read the prefix: the rest of the document is there now (§12.8)
    EditOnFullDoc();  // edit mode was asked for while this was on its way: it is entered now
}

static DWORD g_rereadMs = 0;  // OnFileChanged's backoff for a file that cannot be read now

void StartBackgroundWork() {
    if (!g.path.empty()) StartWatcher();
    if (g.loadFailed && !g.path.empty()) {  // a file there but locked is looked at again (OnFileChanged backs off)
        g_rereadMs = 0;
        SetTimer(g.hwnd, TIMER_RELOAD, 250, nullptr);
    }
    EditAfterOpen();  // a save of this file was interrupted: the recovery strip (a directory listing, after the frame)
    if (g.fullPending) {  // the full model is (or will be) posted; measure/images start after the swap
        if (g.fullDoc.load()) PostMessageW(g.hwnd, WM_APP_FULLDOC, 0, 0);
        return;
    }
    StartMeasure();
    StartImages();
}

// ------------------------------------------------------------------------------------------------ file watcher
// The watcher reports when the file's stamp moves away from the one it last reported - at first the load's own, so a
// change between the load and the watcher's start is not lost - and when the file cannot be looked at any more
// (wParam 1: deleted, renamed, its share gone). What that means is decided on the UI thread (OnFileChanged). When the
// folder's notification handle fails (the folder went away, the network dropped) it starts again after a pause of 1,
// 2, 4 ... 30 s (EDIT-MODE.md §10.7).
namespace {
// Each watcher has a stop event of its own (its own handle to it: the thread closes one, StopWatcher the other). A
// watcher stuck on a share that went away can outlive StopWatcher's wait; with a shared event re-armed for the next
// document it would then run on forever, looking at a file nobody shows any more.
struct WatchArgs { std::wstring path; FILETIME t0; uint64_t s0; HANDLE stop; };
}  // namespace

static DWORD WINAPI WatchThread(void* p) {
    auto* a = (WatchArgs*)p;
    std::wstring dir = DirOf(a->path);
    bool avail = true;
    auto stopped = [&] { return WaitForSingleObject(a->stop, 0) == WAIT_OBJECT_0; };
    auto look = [&] {
        FILETIME t1{};
        uint64_t s1 = 0;
        bool there = GetFileStamp(a->path.c_str(), &t1, &s1);
        if (stopped()) return;  // the look took long (a share gone): the window has moved on meanwhile
        if (!there) {
            if (avail) PostMessageW(g.hwnd, WM_APP_FILECHANGED, 1, 0);
            avail = false;
        } else if (!avail || CompareFileTime(&a->t0, &t1) != 0 || a->s0 != s1) {
            avail = true;
            a->t0 = t1;
            a->s0 = s1;
            PostMessageW(g.hwnd, WM_APP_FILECHANGED, 0, 0);
        }
    };
    DWORD pause = 1000;
    for (;;) {
        HANDLE ch = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                                                 FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
        look();  // whatever happened before there was a handle
        bool stop = false;
        if (ch != INVALID_HANDLE_VALUE) {
            pause = 1000;
            HANDLE hs[2] = {a->stop, ch};
            for (;;) {
                if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) { stop = true; break; }
                look();
                if (!FindNextChangeNotification(ch)) break;
            }
            FindCloseChangeNotification(ch);
        }
        if (stop || WaitForSingleObject(a->stop, pause) != WAIT_TIMEOUT) break;
        pause = std::min<DWORD>(pause * 2, 30000);
    }
    CloseHandle(a->stop);
    delete a;
    return 0;
}

void StartWatcher() {
    StopWatcher();
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr), mine = nullptr;
    if (!stop || !DuplicateHandle(GetCurrentProcess(), stop, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        if (stop) CloseHandle(stop);
        return;
    }
    auto* args = new WatchArgs{g.path, g.fileTime, g.fileSize, mine};
    g.watchThread = CreateThread(nullptr, 64 * 1024, WatchThread, args, 0, nullptr);
    if (!g.watchThread) {
        CloseHandle(mine);
        CloseHandle(stop);
        delete args;
        return;
    }
    g.watchStop = stop;
}

void StopWatcher() {
    if (!g.watchThread) return;
    SetEvent(g.watchStop);
    WaitForSingleObject(g.watchThread, 2000);  // a thread stuck on a share ends by itself once the call returns
    CloseHandle(g.watchThread);
    CloseHandle(g.watchStop);
    g.watchThread = g.watchStop = nullptr;
}

// Reading mode (EDIT-MODE.md §10.7). The file is what the window shows when its stamp is the one taken at load or left by
// our own write (a ticked task box) - or, whatever its stamp, when it still holds the same text (touched, or saved
// unchanged by another program): then only the stamp is taken, and the reader keeps the pictures, the layout and the
// history. Other text is reloaded. A file that cannot be read now is not taken for an empty or a changed one: it is
// read again a little later (250 ms, doubling to 4 s), and a file that is gone waits for the watcher to see it again.
void OnFileChanged() {
    if (g.editing) {  // edit mode never reloads under the edits: it adopts, or asks (§10.7)
        EditOnFileChanged();
        return;
    }
    FILETIME t{};
    uint64_t size = 0;
    bool there = GetFileStamp(g.path.c_str(), &t, &size);
    if (!g.loadFailed && there && size == g.fileSize && CompareFileTime(&t, &g.fileTime) == 0) {
        g_rereadMs = 0;
        return;
    }
    if (g.loadFailed) {  // the error document: reloaded once the file can be read
        if (!there) return;  // still gone: the watcher reports its return
        std::string probe;
        DWORD pe = 0;
        SaveState ps = ReadDisk(g.path.c_str(), probe, nullptr, &pe);
        if (ps == SS_SAVED) {
            g_rereadMs = 0;
            ReloadDocument();
        } else if (ps == SS_BUSY || ps == SS_UNKNOWN) {  // locked by another program: looked at again, less and less often
            g_rereadMs = g_rereadMs ? std::min<DWORD>(g_rereadMs * 2, 4000) : 250;
            SetTimer(g.hwnd, TIMER_RELOAD, g_rereadMs, nullptr);
        }
        return;  // denied: nothing will change that by itself (F5 tries again)
    }
    std::string bytes;
    DWORD e = 0;
    DiskState now;
    SaveState st = there ? ReadDisk(g.path.c_str(), bytes, &now, &e) : SS_MISSING;
    if (st != SS_SAVED) {
        if (st != SS_MISSING) {
            g_rereadMs = g_rereadMs ? std::min<DWORD>(g_rereadMs * 2, 4000) : 250;
            SetTimer(g.hwnd, TIMER_RELOAD, g_rereadMs, nullptr);
        }
        return;
    }
    g_rereadMs = 0;
    std::wstring text;
    DecodeText(bytes.data(), (int)bytes.size(), text, nullptr);
    if (text == (g.disk.valid ? g.disk.text : g.src)) {
        g.fileTime = now.mtime;
        g.fileSize = now.size;
        return;
    }
    ReloadDocument();
}

// ------------------------------------------------------------------------------------------------ reading positions
// positions.bin keeps, per file, the block at the top of the viewport (exact when the file is unchanged) and the nearest
// heading above it (when the file changed). It is read after the first frame; the document then glides to the place.
static PosEntry CapturePosition() {
    PosEntry e;
    e.path = g.path;
    e.size = g.fileSize;
    e.mtime = FileTimeU64(g.fileTime);
    e.opened = NowTicks();
    size_t n = g.doc.blocks.size();
    if (!n || g.Y.size() != n || g.scrollY < 1.f) return e;
    uint32_t a = std::min<uint32_t>(FirstVisible(g.scrollY), (uint32_t)n - 1);
    e.block = a;
    e.blockOff = g.scrollY - g.Y[a];
    for (const Heading& h : g.doc.headings) {
        if (h.block > a) break;
        e.slug = h.slug;
        e.slugOff = g.scrollY - g.Y[h.block];
    }
    return e;
}

static void RememberPosition(const PosEntry& e) {  // this window's copy (reopening a document in the same window)
    auto it = std::find_if(g.positions.begin(), g.positions.end(), [&](const PosEntry& o) { return SamePath(o.path, e.path); });
    if (it != g.positions.end()) *it = e;
    else g.positions.insert(g.positions.begin(), e);
}

struct SaveJob { PosEntry e; bool keep; };
static DWORD WINAPI SaveThread(void* p) {
    auto* j = (SaveJob*)p;
    PositionsSave(j->e, j->keep);
    delete j;
    return 0;
}
static void SaveAsync(const PosEntry& e, bool keepPosition) {
    auto* j = new SaveJob{e, keepPosition};
    if (HANDLE th = CreateThread(nullptr, 64 * 1024, SaveThread, j, 0, nullptr)) CloseHandle(th);
    else { PositionsSave(j->e, j->keep); delete j; }
}

void SaveReadingPosition() {  // window closing: synchronous, the process ends right after
    if (g.path.empty() || g.loadFailed || BenchActive()) return;
    PosEntry e = CapturePosition();
    RememberPosition(e);
    PositionsSave(e, false);
}

static DWORD WINAPI PositionsThread(void* p) {
    auto* path = (std::wstring*)p;
    auto* list = new std::vector<PosEntry>();
    PositionsLoad(*list);
    if (!PostMessageW(g.hwnd, WM_APP_POSITIONS, 0, (LPARAM)list)) delete list;
    if (!path->empty()) {  // recently opened (the position itself is saved when the window closes)
        PosEntry e;
        e.path = *path;
        e.opened = NowTicks();
        PositionsSave(e, true);
    }
    delete path;
    return 0;
}

void LoadPositionsAsync() {
    if (BenchActive() || g.positionsLoaded) return;
    if (HANDLE th = CreateThread(nullptr, 128 * 1024, PositionsThread, new std::wstring(g.loadFailed ? L"" : g.path), 0, nullptr))
        CloseHandle(th);
}

static bool g_restorePending = false;  // a big document's first screen came from a prefix that ends before the place
static PosEntry g_pendingEntry;

static void CancelPendingRestore() {
    g_restorePending = false;
    g_pendingEntry = PosEntry();
}

// false = the place is not in the document yet (prefix parse): retry after the full parse
static bool TryRestore(const PosEntry& e) {
    if (g.userMoved || g.doc.blocks.empty()) return true;
    int block = -1;
    float off = 0;
    bool same = e.size && e.size == g.fileSize && e.mtime == FileTimeU64(g.fileTime);
    if (same && e.block < g.doc.blocks.size()) { block = (int)e.block; off = e.blockOff; }
    else if (!e.slug.empty()) {
        int b = HeadingBlockBySlug(e.slug);
        if (b >= 0) { block = b; off = e.slugOff; }
    }
    if (block < 0) return !g.fullPending;
    if (block == 0 && off < 1.f) return true;
    EnsureLayout((uint32_t)block);
    RecomputeY();
    g.restoreBlock = block;
    g.restoreOff = off;
    g.restored = true;
    ScrollTo(g.Y[block] + off, true);
    return true;
}

static void RestorePositionFor(const std::wstring& path) {
    g_restorePending = false;
    for (const PosEntry& e : g.positions) {
        if (!SamePath(e.path, path)) continue;
        PosEntry copy = e;
        if (!TryRestore(copy)) { g_restorePending = true; g_pendingEntry = copy; }
        return;
    }
}

void OnPositionsLoaded(std::vector<PosEntry>* list) {
    g.positions = std::move(*list);
    delete list;
    g.positionsLoaded = true;
    if (!g.path.empty() && !g.loadFailed) RestorePositionFor(g.path);
}

static void OnFullDocRestore() {
    if (g_restorePending) {
        g_restorePending = false;
        PosEntry e = g_pendingEntry;
        TryRestore(e);
    } else {
        ResolveRestore();
    }
}

// heights changed (measured / images / full parse): keep aiming at the saved place until the glide ends
void ResolveRestore() {
    if (g.restoreBlock < 0) return;
    if (g.userMoved || g.restoreBlock >= (int)g.doc.blocks.size()) { g.restoreBlock = -1; return; }
    if (g.animating) g.targetY = std::clamp(g.Y[g.restoreBlock] + g.restoreOff, 0.f, MaxScroll());
    else g.restoreBlock = -1;  // arrived: the scroll anchor keeps it in place from here on
}
