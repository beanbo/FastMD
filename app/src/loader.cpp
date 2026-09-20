// Document loading and background work.
//   start-up: StartupDocThread (read → parse → DirectWrite → first-viewport layout → render into the canvas) runs in
//             parallel with window creation; the first WM_PAINT only blits.
//   runtime : OpenDocument / Reload / history on the UI thread (big files: prefix parse first, full parse in background).
//   after the first frame of a document: exact heights on worker threads, WIC image decoding, file watcher.
#include "app.h"
#include <shlobj.h>
#include <wincodec.h>

// documents above this size get a prefix parse for the first screen (the full parse runs concurrently)
static const size_t kPrefixThreshold = 256 * 1024, kPrefixChars = 48 * 1024;

HANDLE Spawn(LPTHREAD_START_ROUTINE fn, void* arg, int prio, SIZE_T stack) {
    HANDLE th = CreateThread(nullptr, stack, fn, arg, 0, nullptr);
    if (!th) return nullptr;
    if (prio != THREAD_PRIORITY_NORMAL) SetThreadPriority(th, prio);
    AcquireSRWLockExclusive(&g.workersLock);
    g.workers.push_back(th);
    ReleaseSRWLockExclusive(&g.workersLock);
    return th;
}

void JoinWorkers() {
    g.gen++;     // measure threads check the layout generation and stop early
    g.docGen++;  // image / full-parse threads check the document generation (a re-layout must not stop them)
    AcquireSRWLockExclusive(&g.workersLock);
    std::vector<HANDLE> ws;
    ws.swap(g.workers);
    ReleaseSRWLockExclusive(&g.workersLock);
    for (HANDLE h : ws) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    g.jobsPending = 0;
    if (Doc* d = g.fullDoc.exchange(nullptr)) delete d;
    g.fullPending = false;
}

std::wstring WindowTitle() {
    if (g.path.empty()) return L"FastMD";
    std::wstring t = FileNameOf(g.path) + L" — FastMD";
    if (g.cfg.id[0]) t += std::wstring(L" (") + g.cfg.id + L")";
    return t;
}

// ------------------------------------------------------------------------------------------------ parse
static DWORD WINAPI FullParseThread(void* p) {
    uint32_t myGen = (uint32_t)(uintptr_t)p;
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
    if (!ReadFileUtf16(g.path.c_str(), g.src, &tRead, &g.fileTime)) {
        g.loadFailed = true;
        g.src = std::wstring(L"# ") + Tr(S_LOAD_FAILED) + L"\n\n`" + g.path + L"`\n";
        tRead = NowTicks();
    }
    GetFileStamp(g.path.c_str(), nullptr, &g.fileSize);
    if (startup) MarkAt("file_read", tRead);
    g.doc = Doc();
    g.doc.baseDir = DirOf(g.path);
    size_t cut = g.src.size() > kPrefixThreshold ? FindPrefixCut(g.src.data(), g.src.size(), kPrefixChars) : g.src.size();
    if (cut < g.src.size()) {
        // big file: the first screen comes from a prefix that ends at a top-level heading (identical blocks); the
        // full model replaces it right after the first frame
        g.fullPending = true;
        Spawn(FullParseThread, (void*)(uintptr_t)(uint32_t)g.docGen);
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
    g.matches.clear();
    g.curMatch = -1;
    g.lowerText.clear();
    g.hoverLink = g.hoverCode = g.hoverHBlock = g.dragHBlock = g.hbarFlash = -1;
    g.hotHBar = false;
    g.hx.clear();
    g.focusLink = g.ctxLink = g.ctxImage = -1;
    g.tocHover = -1;
    g.restoreBlock = -1;
    g.restored = false;
    g.imagesStarted = false;
    g.scalingImages = false;  // JoinWorkers has already waited for the scaler; a late result is dropped by its gen
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
    if (g.back.empty()) return;
    HistoryEntry e = g.back.back();
    g.back.pop_back();
    g.fwd.push_back(HistoryEntry{g.path, g.scrollY});
    OpenDocument(e.path, false, e.scrollY);
}

void NavigateForward() {
    if (g.fwd.empty()) return;
    HistoryEntry e = g.fwd.back();
    g.fwd.pop_back();
    g.back.push_back(HistoryEntry{g.path, g.scrollY});
    OpenDocument(e.path, false, e.scrollY);
}

// ------------------------------------------------------------------------------------------------ exact heights
static DWORD WINAPI MeasureThread(void* p) {
    auto* job = (MeasureJob*)p;
    Typography t;
    t.Init(g.dwf, &g.typo);
    for (uint32_t i = job->from; i < job->to; i++) {
        if (g.gen != job->gen || g.closing) break;
        const Block& b = g.doc.blocks[i];
        if (BlockHidden(b)) { job->h[i - job->from] = 0.f; continue; }  // folded <details>: no height
        float w = LayoutWidthFor(b, job->textW, job->wideW);
        bool exact = false;
        float h = BlockHeightEstimate(g.doc, t, b, w, &exact);
        if (!exact || b.kind == BK_IMAGE) {
            BlockLayout* L = LayoutBlock(g.doc, t, i, w);
            h = L->height;
            delete L;
        }
        job->h[i - job->from] = h;
    }
    t.Release();
    if (g.closing || !PostMessageW(g.hwnd, WM_APP_MEASURED, 0, (LPARAM)job)) delete job;
    return 0;
}

void StartMeasure() {
    size_t n = g.doc.blocks.size();
    size_t unknown = 0;
    for (size_t i = 0; i < n; i++) unknown += !g.known[i];
    if (!unknown) return;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int threads = (int)std::clamp<size_t>(n / 400, 1, std::min<DWORD>(8, std::max<DWORD>(1, si.dwNumberOfProcessors / 2)));
    size_t chunk = (n + threads - 1) / threads;
    for (int k = 0; k < threads; k++) {
        auto* job = new MeasureJob{g.gen, (uint32_t)(k * chunk), (uint32_t)std::min(n, (k + 1) * chunk), g.textW, g.wideW, {}};
        if (job->from >= job->to) { delete job; continue; }
        job->h.assign(job->to - job->from, 0.f);
        g.jobsPending++;
        if (!Spawn(MeasureThread, job, THREAD_PRIORITY_BELOW_NORMAL, 256 * 1024)) { g.jobsPending--; delete job; }
    }
}

void OnMeasured(MeasureJob* job) {
    if (job->gen == g.gen) {
        g.jobsPending--;
        static MeasureJob* cur;
        cur = job;
        WithAnchor([] {
            for (uint32_t i = cur->from; i < cur->to; i++) {
                if (!g.cache[i]) g.H[i] = cur->h[i - cur->from];
                g.known[i] = 1;
            }
            RecomputeY();
        });
        ResolveRestore();
        if (g.jobsPending == 0) {
            Mark("measured_all");
            DebugFlush();
        }
        Invalidate();
    }
    delete job;
}

// ------------------------------------------------------------------------------------------------ images (WIC)
static DWORD WINAPI ImageThread(void* p) {
    uint32_t myGen = (uint32_t)(uintptr_t)p;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    auto& imgs = g.doc.images;
    for (size_t i = 0; i < imgs.size() && !g.closing && g.docGen == myGen; i++) {
        Image& im = imgs[i];
        if (im.path.empty()) { im.state = 3; continue; }
        size_t c = i;
        for (size_t k = 0; k < i; k++) if (imgs[k].path == im.path) { c = k; break; }
        im.canon = (int)c;
        if (c != i) continue;
        bool ok = false;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* fr = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (wic && SUCCEEDED(wic->CreateDecoderFromFilename(im.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            UINT w = 0, h = 0;
            conv->GetSize(&w, &h);
            std::vector<uint32_t> px((size_t)w * h);
            if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)(px.size() * 4), (BYTE*)px.data()))) {
                im.px.swap(px);
                im.pxW = (int)w;  // the decoded size (a GIF frame may be smaller than its header's screen size)
                im.pxH = (int)h;
                if (im.w <= 0) { im.w = (int)w; im.h = (int)h; }
                ok = true;
            }
        }
        SafeRelease(conv);
        SafeRelease(fr);
        SafeRelease(dec);
        im.state = ok ? 2 : 3;
    }
    SafeRelease(wic);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    if (!g.closing && g.docGen == myGen) PostMessageW(g.hwnd, WM_APP_IMAGES, 0, 0);
    return 0;
}

static void StartImages() {
    if (g.imagesStarted || g.doc.images.empty()) return;
    g.imagesStarted = true;
    Spawn(ImageThread, (void*)(uintptr_t)(uint32_t)g.docGen, THREAD_PRIORITY_BELOW_NORMAL);
}

// --------------------------------------------------------------------------------------- display-size copies (WIC)
// Drawing a picture straight from its decoded pixels means scaling every pixel of every frame by the nearest
// neighbour: slow while scrolling and visibly jagged when the picture is bigger than its column. So the canvas
// records the size it drew at, and this thread makes a copy at exactly that size with a real filter; after that a
// frame only copies rows. A copy is remade when the size changes (zoom, column width, window resize).
static const size_t kScaledBudget = 48u << 20;  // bytes of display-size copies kept outside the viewport

static DWORD WINAPI ScaleThread(void* p) {
    uint32_t myGen = (uint32_t)(uintptr_t)p;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    auto* out = new std::vector<ScaledImage>();
    auto& imgs = g.doc.images;
    for (size_t i = 0; wic && i < imgs.size() && !g.closing && g.docGen == myGen; i++) {
        Image& im = imgs[i];
        if (im.canon >= 0 && im.canon != (int)i) continue;  // only the entry that holds the pixels
        if (im.state.load() != 2 || im.pxW <= 0 || im.pxH <= 0) continue;
        int w = im.wantW.load(), h = im.wantH.load();
        if (w <= 0 || h <= 0 || (w == im.scW.load() && h == im.scH.load())) continue;
        if (w == im.pxW && h == im.pxH) continue;  // drawn 1:1: the decoded pixels are already the right size
        IWICBitmap* src = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        std::vector<uint32_t> px((size_t)w * h);
        bool ok = SUCCEEDED(wic->CreateBitmapFromMemory((UINT)im.pxW, (UINT)im.pxH, GUID_WICPixelFormat32bppPBGRA,
                                                        (UINT)im.pxW * 4, (UINT)(im.px.size() * 4), (BYTE*)im.px.data(), &src)) &&
                  SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                  SUCCEEDED(scaler->Initialize(src, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) &&
                  SUCCEEDED(scaler->CopyPixels(nullptr, (UINT)w * 4, (UINT)(px.size() * 4), (BYTE*)px.data()));
        SafeRelease(scaler);
        SafeRelease(src);
        // a failure is reported too (empty pixels): the size is then known to be unusable and is not retried
        out->push_back(ScaledImage{(uint32_t)i, w, h, ok ? std::move(px) : std::vector<uint32_t>()});
    }
    SafeRelease(wic);
    if (SUCCEEDED(hrCo) ) CoUninitialize();
    if (g.closing || g.docGen != myGen || !g.hwnd || !PostMessageW(g.hwnd, WM_APP_SCALED, myGen, (LPARAM)out)) delete out;
    return 0;
}

void ScheduleImageScaling() {
    if (g.scalingImages || g.firstFrame || !g.hwnd || g.closing) return;
    for (size_t i = 0; i < g.doc.images.size(); i++) {
        Image& im = g.doc.images[i];
        if (im.canon >= 0 && im.canon != (int)i) continue;
        int w = im.wantW.load(), h = im.wantH.load();
        if (w <= 0 || h <= 0 || (w == im.scW.load() && h == im.scH.load()) || (w == im.pxW && h == im.pxH)) continue;
        if (im.state.load() != 2) continue;
        g.scalingImages = true;
        Spawn(ScaleThread, (void*)(uintptr_t)(uint32_t)g.docGen, THREAD_PRIORITY_BELOW_NORMAL);
        return;
    }
}

// keep the display-size copies of the pictures around the viewport; drop the rest once they add up (a long gallery)
static void TrimScaledImages() {
    size_t total = 0;
    for (const Image& im : g.doc.images) total += im.sc.size() * 4;
    if (total <= kScaledBudget) return;
    std::vector<uint8_t> keep(g.doc.images.size(), 0);
    float top = g.scrollY - ViewH(), bottom = g.scrollY + 2 * ViewH();
    for (size_t i = 0; i < g.doc.blocks.size() && i < g.Y.size(); i++) {
        const Block& b = g.doc.blocks[i];
        if (b.kind != BK_IMAGE || b.aux >= keep.size() || g.Y[i] + g.H[i] < top || g.Y[i] > bottom) continue;
        keep[b.aux] = 1;
        int c = g.doc.images[b.aux].canon;
        if (c >= 0 && (size_t)c < keep.size()) keep[c] = 1;
    }
    for (size_t i = 0; i < g.doc.images.size(); i++) {
        Image& im = g.doc.images[i];
        if (keep[i] || im.sc.empty()) continue;
        im.scW = 0;  // the canvas checks the size before it reads the pixels
        im.scH = 0;
        im.wantW = 0;
        im.wantH = 0;
        std::vector<uint32_t>().swap(im.sc);
    }
}

void OnScaledImages(std::vector<ScaledImage>* list, uint32_t gen) {
    g.scalingImages = false;
    if (gen == g.docGen && !g.closing) {
        for (ScaledImage& s : *list) {
            if (s.index >= g.doc.images.size()) continue;
            Image& im = g.doc.images[s.index];
            im.sc.swap(s.px);
            im.scW = s.w;
            im.scH = s.h;
        }
        if (!list->empty()) {
            g.pixelSerial++;
            TrimScaledImages();
            Invalidate();
        }
        ScheduleImageScaling();  // the size may have changed again while this batch was being made
    }
    delete list;
}

void OnImagesLoaded() {
    g.pixelSerial++;  // the same layout now draws differently: a scrolled frame must not reuse the old pixels
    WithAnchor([] {
        for (size_t i = 0; i < g.doc.blocks.size(); i++) {
            const Block& b = g.doc.blocks[i];
            bool inlineImg = false;  // a badge inside a line changes the line's height once it is known
            for (uint32_t k = 0; k < b.runCount && !inlineImg; k++)
                inlineImg = (g.doc.runs[b.runOff + k].flags & F_IMAGE) != 0;
            if (b.kind != BK_IMAGE && !inlineImg) continue;
            if (g.cache[i]) { delete g.cache[i]; g.cache[i] = nullptr; g.cachedCount--; }
            if (b.kind == BK_IMAGE)
                g.H[i] = ImageDisplayHeight(g.doc.images[b.aux], LayoutWidthFor(b, g.textW, g.wideW) - b.indent);
        }
        RecomputeY();
    });
    ResolveRestore();
    Invalidate();
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
}

void StartBackgroundWork() {
    if (!g.path.empty()) StartWatcher();
    if (g.fullPending) {  // the full model is (or will be) posted; measure/images start after the swap
        if (g.fullDoc.load()) PostMessageW(g.hwnd, WM_APP_FULLDOC, 0, 0);
        return;
    }
    StartMeasure();
    StartImages();
}

// ------------------------------------------------------------------------------------------------ file watcher
static DWORD WINAPI WatchThread(void* p) {
    std::wstring* path = (std::wstring*)p;
    std::wstring dir = DirOf(*path);
    HANDLE ch = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                                             FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
    if (ch == INVALID_HANDLE_VALUE) { delete path; return 0; }
    FILETIME t0{};
    uint64_t s0 = 0;
    GetFileStamp(path->c_str(), &t0, &s0);
    HANDLE hs[2] = {g.watchStop, ch};
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0 + 1) break;
        FILETIME t1{};
        uint64_t s1 = 0;
        if (GetFileStamp(path->c_str(), &t1, &s1) && (CompareFileTime(&t0, &t1) != 0 || s0 != s1)) {
            t0 = t1;
            s0 = s1;
            PostMessageW(g.hwnd, WM_APP_FILECHANGED, 0, 0);
        }
        if (!FindNextChangeNotification(ch)) break;
    }
    FindCloseChangeNotification(ch);
    delete path;
    return 0;
}

void StartWatcher() {
    StopWatcher();
    if (!g.watchStop) g.watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(g.watchStop);
    g.watchThread = CreateThread(nullptr, 64 * 1024, WatchThread, new std::wstring(g.path), 0, nullptr);
}

void StopWatcher() {
    if (!g.watchThread) return;
    SetEvent(g.watchStop);
    WaitForSingleObject(g.watchThread, 2000);
    CloseHandle(g.watchThread);
    g.watchThread = nullptr;
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
