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
    g.gen++;  // measure / image / parse threads check the generation and stop early
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
    if (g.gen != myGen) { delete d; return 0; }
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
        g.src = L"# Не удалось открыть файл\n\n`" + g.path + L"`\n";
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
        Spawn(FullParseThread, (void*)(uintptr_t)(uint32_t)g.gen);
        ParseMarkdown(g.doc, g.src.data(), cut);
        if (startup) Mark("parsed_prefix");
    } else {
        ParseMarkdown(g.doc, g.src.data(), g.src.size());
        if (startup) Mark("parsed");
    }
}

DWORD WINAPI StartupDocThread(void*) {
    if (!g.path.empty()) LoadSource(true);
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
static void ResetViewState() {
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
    g.hoverLink = g.hoverCode = -1;
    g.imagesStarted = false;
    g.docH = 0;
}

void OpenDocument(const std::wstring& path, bool pushHistory, float scrollY) {
    wchar_t full[MAX_PATH * 4];
    std::wstring p = GetFullPathNameW(path.c_str(), MAX_PATH * 4, full, nullptr) ? std::wstring(full) : path;
    if (pushHistory && !g.path.empty()) {
        g.back.push_back(HistoryEntry{g.path, g.scrollY});
        g.fwd.clear();
    }
    StopWatcher();
    JoinWorkers();
    ResetViewState();
    g.path = p;
    LoadSource(false);
    g.scrollY = g.targetY = scrollY;
    g.animating = false;
    InitialLayout();
    g.offscreenValid = false;
    SetWindowTextW(g.hwnd, WindowTitle().c_str());
    if (g.findOpen && !g.findQuery.empty()) FindUpdate(false);
    Invalidate();
    StartBackgroundWork();
    SHAddToRecentDocs(SHARD_PATHW, g.path.c_str());
}

void ReloadDocument() {
    if (g.path.empty()) return;
    float y = g.scrollY;
    uint32_t selA = g.selAnchor, selF = g.selFocus;
    OpenDocument(g.path, false, y);
    uint32_t n = (uint32_t)g.doc.text.size();
    g.selAnchor = std::min(selA, n);
    g.selFocus = std::min(selF, n);
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
        float w = LayoutWidthFor(b, job->textW, job->wideW);
        bool exact = false;
        float h = BlockHeightEstimate(g.doc, b, w, &exact);
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
    for (size_t i = 0; i < imgs.size() && !g.closing && g.gen == myGen; i++) {
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
    if (!g.closing && g.gen == myGen) PostMessageW(g.hwnd, WM_APP_IMAGES, 0, 0);
    return 0;
}

static void StartImages() {
    if (g.imagesStarted || g.doc.images.empty()) return;
    g.imagesStarted = true;
    Spawn(ImageThread, (void*)(uintptr_t)(uint32_t)g.gen, THREAD_PRIORITY_BELOW_NORMAL);
}

void OnImagesLoaded() {
    WithAnchor([] {
        for (size_t i = 0; i < g.doc.blocks.size(); i++) {
            const Block& b = g.doc.blocks[i];
            if (b.kind != BK_IMAGE) continue;
            if (g.cache[i]) { delete g.cache[i]; g.cache[i] = nullptr; g.cachedCount--; }
            g.H[i] = ImageDisplayHeight(g.doc.images[b.aux], LayoutWidthFor(b, g.textW, g.wideW) - b.indent);
        }
        RecomputeY();
    });
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
        InitGeometry();
        RecomputeY();
    });
    delete d;
    g.fullPending = false;
    g.lowerText.clear();
    if (g.findOpen && !g.findQuery.empty()) FindUpdate(true);
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
