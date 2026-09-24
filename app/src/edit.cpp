// Edit mode's glue (docs/EDIT-MODE.md §3.1): the model swap after a change of the source, the baseline and the save,
// the recovery banner, the test hooks and edit mode's queries. The window-free parts live in editcore (mapping, undo)
// and editfile (encoding, the write); this file is where they meet `g`.
//
// Phase 1c: everything here runs in reading mode already - a ticked task box is a splice, a save and a swap (T2), and
// the test hooks drive the same path - while the editor itself (caret, bar, typing) arrives with 2a.
#include "app.h"
#include "editcore.h"
#include "editfile.h"

namespace {
bool EnvOn(const wchar_t* name) {
    wchar_t v[8] = {};
    return GetEnvironmentVariableW(name, v, 8) > 0 && v[0] == L'1';
}
// FASTMD_EDIT_SELFCHECK=1: the map is checked after every swap, and every save decodes its whole output (§13.5)
bool SelfCheckOn() {
    static const bool on = EnvOn(L"FASTMD_EDIT_SELFCHECK");
    return on;
}
bool HooksOn() { return EditTestHooks(); }

uint64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}
uint32_t Micros(uint64_t a, uint64_t b) {
    static const uint64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (uint64_t)q.QuadPart; }();
    return (uint32_t)((b - a) * 1000000 / f);
}

// the time of each swap, split as §5.8 asks: parse, carry + diff, install + layout (µs)
struct Sample { uint32_t parse, carry, install; };

struct Session {
    UndoStack undo;                    // the document session's history (cleared by every load)
    SaveState saveState = SS_SAVED;    // what the last save answered
    uint32_t selfcheckFailures = 0;    // Q_MAP_SELFCHECK lp 1
    Sample ring[128] = {};
    uint32_t samples = 0;
    std::vector<RecoveryInfo> recovery;  // interrupted saves of the open file (the RECOVERY strip)
    RecoveryInfo pending;                // the recovery file standing for the last flush, while saves go unflushed
} s;

std::wstring RecoveryDir() {
    std::wstring d = DataDir();
    return d.empty() ? d : d + L"recovery\\";
}

// FNV-1a-32 over the UTF-16LE bytes of the text (Q_SRC_HASH; ui_smoke.py's src_hash computes the same)
uint32_t Fnv32(const std::wstring& t) {
    uint32_t h = 2166136261u;
    for (wchar_t c : t) {
        h = (h ^ (uint8_t)(c & 0xFF)) * 16777619u;
        h = (h ^ (uint8_t)(c >> 8)) * 16777619u;
    }
    return h;
}

bool HiddenIn(const Doc& d, const Block& b) {  // BlockHidden, for a Doc that is not installed yet
    if (!b.details || (b.details & 0x8000)) return false;
    uint32_t gi = (uint32_t)(b.details & 0x7FFF) - 1;
    return gi < d.detailsOpen.size() && !d.detailsOpen[gi];
}

// A carried layout draws its inline pictures by their index into the model: it stays only while those are the same.
bool SameImageIndices(const Doc& a, uint32_t i, const Doc& b, uint32_t j) {
    auto runs = [](const Doc& a, uint32_t ra, const Doc& b, uint32_t rb, uint32_t n) {
        for (uint32_t k = 0; k < n; k++)
            if ((a.runs[ra + k].flags & F_IMAGE) && a.runs[ra + k].image != b.runs[rb + k].image) return false;
        return true;
    };
    const Block& x = a.blocks[i];
    const Block& y = b.blocks[j];
    if (!runs(a, x.runOff, b, y.runOff, x.runCount)) return false;
    if (x.kind == BK_TABLE && x.aux < a.tables.size() && y.aux < b.tables.size()) {
        const Table& p = a.tables[x.aux];
        const Table& q = b.tables[y.aux];
        for (uint32_t c = 0; c < p.rows * p.cols; c++)
            if (!runs(a, a.cells[p.cellOff + c].runOff, b, b.cells[q.cellOff + c].runOff, a.cells[p.cellOff + c].runCount))
                return false;
    }
    return true;
}

// The first differing and the last equal offsets of two texts that share a prefix and a suffix of blocks: the part of
// the rendered text an edit replaced. false when the texts around it are not the same after all (then nothing that
// caches the text - find's lower-case copy, the selection - is carried).
struct TextSplit { uint32_t oldBeg, oldEnd, newBeg, newEnd; };
bool SplitText(const Doc& od, const Doc& nd, uint32_t p, uint32_t q, TextSplit& t) {
    uint32_t no = (uint32_t)od.blocks.size(), nn = (uint32_t)nd.blocks.size();
    t.oldBeg = p < no ? od.blocks[p].textOff : (uint32_t)od.text.size();
    t.newBeg = p < nn ? nd.blocks[p].textOff : (uint32_t)nd.text.size();
    t.oldEnd = q ? od.blocks[no - q].textOff : (uint32_t)od.text.size();
    t.newEnd = q ? nd.blocks[nn - q].textOff : (uint32_t)nd.text.size();
    if (t.oldBeg != t.newBeg || t.oldEnd < t.oldBeg || t.newEnd < t.newBeg ||
        od.text.size() - t.oldEnd != nd.text.size() - t.newEnd)
        return false;
    return od.text.compare(0, t.oldBeg, nd.text, 0, t.newBeg) == 0 &&
           od.text.compare(t.oldEnd, std::wstring::npos, nd.text, t.newEnd, std::wstring::npos) == 0;
}

bool SelfCheckNow(std::string* why) {
    if (g.doc.hasMap) return MapSelfCheck(g.doc, g.src, why);
    Doc m;  // reading mode parses without the map (§4.3): check a map parse of the same source
    m.baseDir = g.doc.baseDir;
    ParseOptions opt;
    opt.wantMap = true;
    ParseMarkdown(m, g.src.data(), g.src.size(), &opt);
    return MapSelfCheck(m, g.src, why);
}

uint32_t Percentile(std::vector<uint32_t> v, int pct) {
    if (v.empty()) return 0;
    for (size_t i = 1; i < v.size(); i++)  // at most 128 samples: an insertion sort is plenty
        for (size_t k = i; k > 0 && v[k - 1] > v[k]; k--) std::swap(v[k - 1], v[k]);
    return v[std::min(v.size() - 1, v.size() * pct / 100)];
}
}  // namespace

bool EditTestHooks() {
    static const bool on = EnvOn(L"FASTMD_TEST_HOOKS");
    return on;
}

// ------------------------------------------------------------------------------------------------ the swap (§5.5)
// A new model from the whole source, put in place of the old one so that whatever did not change stays exactly as it
// was: its layouts, heights, horizontal scroll and pictures, and the view does not move. at / oldLen / newLen: the
// source range the change replaced (at == UINT32_MAX: none, the entry parse). Nothing is pumped in here (R13).
void EditReparse(uint32_t at, uint32_t oldLen, uint32_t newLen) {
    // 1. the splice primitive refuses both (and the reload timer waits for the modal loop), so neither can hold here
    if (g.fullPending || g.editModal > 0) DebugLog("EditReparse with the full parse pending or inside a modal");
    uint64_t t0 = Qpc();
    // 2. only the measure jobs read the model and stop within a block; the picture worker and the scaler hold their own
    JoinDocReaders();
    // 3. the whole source, always. Edit mode parses with the map; reading mode (a task tick, the test hook) without,
    // so it draws what a load would draw and "copy as Markdown" keeps its srcMap (§4.3)
    Doc nd;
    nd.baseDir = g.doc.baseDir;
    ParseOptions opt;
    opt.wantMap = true;
    ParseMarkdown(nd, g.src.data(), g.src.size(), g.editing ? &opt : nullptr);
    uint64_t t1 = Qpc();
    // 4. pictures the render table knows are shown at once; a formula being typed keeps its old picture
    uint32_t editBeg = at, oldEnd = at == UINT32_MAX ? 0 : at + oldLen, newEnd = at == UINT32_MAX ? 0 : at + newLen;
    CarryRenders(g.doc, nd, editBeg, oldEnd, newEnd);
    if (nd.detailsOpen.size() == g.doc.detailsOpen.size()) nd.detailsOpen = g.doc.detailsOpen;
    // 5. the blocks that lay out the same before and after the change
    const uint32_t nOld = (uint32_t)g.doc.blocks.size(), nNew = (uint32_t)nd.blocks.size();
    BlockDiff df = DiffBlocks(g.doc, nd);
    const uint32_t p = df.p, q = df.q;
    uint64_t t2 = Qpc();

    // 6. What keeps the view in place, on the old geometry: the view's top when the change is below it (A), the first
    // changed block when it is the top one (B), else the first unchanged block after the change that is on screen (C).
    const float top = 0.f;  // EditRevealTop(): the toolbar's inset arrives with 2a
    int mode = 0;
    float pTop = 0, off = 0;
    uint32_t anchorNew = 0;
    if (nOld && g.Y.size() == nOld) {
        uint32_t a = std::min<uint32_t>(FirstVisible(g.scrollY + top), nOld - 1);
        if (a == p) {
            mode = 1;
            pTop = g.Y[p] - g.scrollY;
        } else if (a > p) {
            uint32_t u = std::max(a, nOld - q);
            if (u < nOld && g.Y[u] < g.scrollY + ViewH()) {
                mode = 2;
                anchorNew = u + nNew - nOld;
                off = g.scrollY - g.Y[u];
            }
        }
    }
    TextSplit ts{};
    bool textSame = SplitText(g.doc, nd, p, q, ts);

    // 7. The per-block state: prefix and suffix blocks keep theirs (a layout that shows inline pictures only while
    // their indices are unchanged), the middle is estimated and measured again.
    std::vector<BlockLayout*> cache(nNew, nullptr);
    std::vector<float> H(nNew, 0.f), hx(nNew, 0.f);
    std::vector<uint8_t> known(nNew, 0), middle(nNew, 0);
    bool hxMoved = false;
    for (uint32_t i = 0; i < nNew; i++) {
        bool pre = i < p, suf = i >= nNew - q;
        if (!pre && !suf) {
            middle[i] = 1;
            continue;
        }
        uint32_t o = pre ? i : i + nOld - nNew;
        if (o < g.cache.size() && g.cache[o]) {
            if (SameImageIndices(g.doc, o, nd, i)) cache[i] = g.cache[o];
            else delete g.cache[o];
            g.cache[o] = nullptr;
        }
        H[i] = o < g.H.size() ? g.H[o] : 0.f;
        known[i] = o < g.known.size() ? g.known[o] : 0;
        hx[i] = o < g.hx.size() ? g.hx[o] : 0.f;
        hxMoved |= hx[i] != 0 && o != i;
    }
    for (uint32_t o = p; o < nOld - q && o < g.hx.size(); o++) hxMoved |= g.hx[o] != 0;
    ClearLayoutCache();  // what is left are the old middle's layouts

    // 8. Install. The Doc object stays where it is, so a carried InlineImage (it points at g.doc) stays valid.
    Doc old = std::move(g.doc);
    g.doc = std::move(nd);
    g.cache.swap(cache);
    g.H.swap(H);
    g.known.swap(known);
    g.hx.swap(hx);
    g.Y.assign(nNew, 0.f);
    g.cachedCount = 0;
    for (uint32_t i = 0; i < nNew; i++) {
        if (g.cache[i]) g.cachedCount++;
        if (!middle[i]) continue;
        const Block& b = g.doc.blocks[i];
        if (HiddenIn(g.doc, b)) {
            g.H[i] = 0.f;
            g.known[i] = 1;
            continue;
        }
        bool exact = false;
        g.H[i] = BlockHeightEstimate(g.doc, g.typo, b, LayoutWidthFor(b, g.textW, g.wideW), &exact);
        g.known[i] = exact;  // an exact estimate needs no measuring (InitGeometry's rule)
    }
    float tw = g.textW, ww = g.wideW;
    UpdateColumns();  // the outline docks only while the document has headings
    bool relaid = std::fabs(tw - g.textW) > 0.1f || std::fabs(ww - g.wideW) > 0.1f;
    if (relaid) InitGeometry();  // the widths moved after all: Relayout()'s way
    // 9.
    RecomputeY();
    // 10. the anchor on the new geometry; a glide in flight stops where it is
    if (!relaid && mode == 1 && p < nNew) g.scrollY = g.Y[p] - pTop;
    else if (!relaid && mode == 2 && anchorNew < nNew) g.scrollY = g.Y[anchorNew] + off;
    g.scrollY = std::clamp(g.scrollY, 0.f, MaxScroll());
    g.targetY = g.scrollY;
    g.animating = false;
    uint64_t t3 = Qpc();

    // 11. Bookkeeping. Index-keyed hover and focus state points at blocks that may not be there any more.
    g.jobsPending = 0;
    g.docSerial++;
    g.editSerial++;
    if (!g.lowerText.empty()) {  // find's lower-case copy: the middle replaced, not rebuilt (R16)
        if (textSame && g.lowerText.size() == old.text.size()) {
            g.lowerText.replace(ts.oldBeg, ts.oldEnd - ts.oldBeg, ToLower(g.doc.text.substr(ts.newBeg, ts.newEnd - ts.newBeg)));
            if (g.lowerText.size() != g.doc.text.size()) g.lowerText.clear();
        } else {
            g.lowerText.clear();
        }
    }
    if (g.findOpen && !g.findQuery.empty()) FindRefresh();
    g.hoverLink = g.hoverCode = g.hoverHBlock = g.dragHBlock = g.hbarFlash = g.hoverHeading = -1;
    g.hoverTask = g.downTask = g.focusLink = g.ctxLink = g.ctxImage = g.tocHover = -1;
    g.restoreBlock = -1;
    g.userMoved = true;
    if (hxMoved) g.hxSerial++;
    // 12. The selection: reading mode keeps it where the text around it is the same (edit mode maps its source caret,
    // 2a).
    auto move = [&](uint32_t t) -> uint32_t {
        if (!textSame) return std::min<uint32_t>(t, (uint32_t)g.doc.text.size());
        if (t <= ts.oldBeg) return t;
        if (t >= ts.oldEnd) return t - ts.oldEnd + ts.newEnd;
        return ts.newBeg;
    };
    g.selAnchor = move(g.selAnchor);
    g.selFocus = move(g.selFocus);
    // 13. sources the table has not seen go to the picture worker
    StartImages();
    // 14. a screen reader hears about it once the typing pauses
    if (g.hwnd) SetTimer(g.hwnd, TIMER_EDIT_UI, 100, nullptr);
    // 15.
    if (SelfCheckOn()) {
        std::string why;
        if (!SelfCheckNow(&why)) {
            s.selfcheckFailures++;
            DebugLog("map self-check failed after an edit: %s", why.c_str());
        }
    }
    if (!g.editing) MeasureUnknown();  // edit mode measures after a pause in typing (TIMER_EDIT_IDLE, 2a)
    // 16.
    s.ring[s.samples++ % std::size(s.ring)] = Sample{Micros(t0, t1), Micros(t1, t2), Micros(t2, t3)};
    Invalidate();
}

// ------------------------------------------------------------------------------------------------ splice (§7.1)
bool EditSplice(uint32_t at, uint32_t len, std::wstring text) {
    // the full parse of a big document reads g.src on its thread; the error document is not the file; inside a modal
    // loop (a sent WM_COPYDATA arrives there too) whoever opened it holds on to the model (§10.10)
    if (g.fullPending || g.loadFailed || g.path.empty() || g.editModal > 0) return false;
    if (at > g.src.size() || len > g.src.size() - at || SpliceSplits(g.src, at, len)) {
        DebugLog("splice refused: %u+%u would cut a character or a line end in two", at, len);
        ShowToast(Tr(S_ED_REFUSED), 3000);
        return false;
    }
    for (size_t i = 0; i < text.size(); i++) {  // a lone surrogate typed or pasted becomes U+FFFD; the file's own stay
        bool pair = i + 1 < text.size() && text[i] >= 0xD800 && text[i] <= 0xDBFF && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF;
        if (pair) i++;
        else if (text[i] >= 0xD800 && text[i] <= 0xDFFF) text[i] = 0xFFFD;
    }
    g.src.replace(at, len, text);
    return true;
}

// ------------------------------------------------------------------------------------------------ baseline and save
bool EditDirty() { return g.disk.valid && g.src != g.disk.text; }

BaselineResult EditBaseline(SaveState* st) {
    *st = SS_SAVED;
    if (g.disk.valid) return BL_OK;
    if (g.path.empty() || g.loadFailed || g.fullPending) return BL_REFUSED;
    std::string bytes;
    DWORD e = 0;
    DiskState d;
    *st = ReadDisk(g.path.c_str(), bytes, &d, &e);
    if (*st != SS_SAVED) return BL_UNREADABLE;
    DiskRefusal dr = DecodeDisk(bytes, AnsiCodePage(), d);
    if (dr == DR_UTF16BE) return BL_REFUSED;  // not decoded at all: nothing to compare
    if (d.text != g.src) return BL_CHANGED;   // it is not what the window shows (a reload comes first)
    if (dr != DR_OK) {
        DebugLog("baseline refused: %d", (int)dr);
        return BL_REFUSED;
    }
    d.valid = true;
    g.disk = std::move(d);
    g.eol = DiskEol(g.disk);
    return BL_OK;
}

// What the file on disk says about each recovery file on the strip; the ones of a save that went through (or never
// wrote a byte) are deleted without a word.
static void ClassifyLeftovers() {
    std::string bytes;
    DiskState now;
    DWORD e = 0;
    bool read = ReadDisk(g.path.c_str(), bytes, &now, &e) == SS_SAVED;
    uint64_t mtime = FileTimeU64(now.mtime);
    for (size_t i = 0; i < s.recovery.size();) {
        RecoveryInfo& r = s.recovery[i];
        r.verdict = read ? ClassifyRecovery(r, bytes, mtime) : RV_CHANGED;
        if (r.verdict == RV_DONE || r.verdict == RV_UNTOUCHED) {
            DebugLog("recovery file of a save that %s: deleted", r.verdict == RV_DONE ? "went through" : "never wrote");
            DeleteFileW(r.file.c_str());
            s.recovery.erase(s.recovery.begin() + i);
        } else {
            i++;
        }
    }
}

SaveState EditSave(bool flushPoint) {
    if (!g.disk.valid) return SS_FAILED;
    if (g.src == g.disk.text) {  // equal bytes are never written (UX-5); a flush point still flushes
        if (flushPoint) EditLeaveDocument();
        return s.saveState = SS_SAVED;
    }
    // An interrupted save of this file waits on the strip: nothing is written until the reader has chosen what to do
    // with it - its recovery file may be the only copy of the bytes it holds, and a torn file is no baseline.
    if (!s.recovery.empty()) {
        DebugLog("save refused: a recovery file of this file is waiting on the strip");
        return s.saveState = SS_FAILED;
    }
    SaveRequest rq;
    rq.path = g.path.c_str();
    rq.text = &g.src;
    rq.disk = &g.disk;
    rq.recoveryDir = RecoveryDir();
    rq.flushPoint = flushPoint;
    rq.fullProof = SelfCheckOn();
    rq.pending = s.pending.file.empty() ? nullptr : &s.pending;
    SaveResult r = SaveSource(rq);
    DebugLog("save: state %d (%s), error %lu, flush %.2f ms", (int)r.state, r.reason, r.error, r.flushMs);
    s.pending = std::move(r.pending);
    if (r.state == SS_SAVED || r.adopted) {
        g.disk = std::move(r.disk);
        if (r.adopted) g.eol = DiskEol(g.disk);
    }
    if (r.state == SS_SAVED) {
        // our own write: the watcher sees the stamp it left and does not reload
        g.fileTime = g.disk.mtime;
        g.fileSize = g.disk.size;
        g.saves++;
    }
    if (!r.recoveryKept.empty()) {
        // The write failed half-way and so did putting the old bytes back: the recovery file is now the only copy of
        // them. It goes on the strip at once (§10.3 step 9), not only at the next open.
        RecoveryInfo ri;
        if (ReadRecovery(r.recoveryKept, ri)) {
            s.recovery.push_back(std::move(ri));
            ClassifyLeftovers();
        }
        if (!s.recovery.empty()) StripShow(STRIP_RECOVERY);
    }
    return s.saveState = r.state;
}

void EditPushStep(EditStep step) {
    if (s.undo.Depth() + s.undo.RedoDepth() == 0) return;  // reading mode adds to a history only where there is one
    s.undo.Push(std::move(step), GetTickCount64());
}

void EditOnLoad() {
    s.undo.Clear();  // a new document session (§11)
    s.saveState = SS_SAVED;
    s.recovery.clear();
    s.pending = RecoveryInfo();  // (EditLeaveDocument has flushed its file already)
    StripHide(STRIP_RECOVERY);
}

// A flush point (§10.3 step 7): leaving the document for another one or a reload, and the window closing. The recovery
// file that stood for the last flush while saves went unflushed goes once the file is flushed; a flush that fails
// leaves it, and the next open finds it.
void EditLeaveDocument() {
    if (s.pending.file.empty()) return;
    if (!RecoveryFlushPending(s.pending, g.path.c_str())) DebugLog("flush point: the flush failed, the recovery file stays");
    s.pending = RecoveryInfo();
}

// ------------------------------------------------------------------------------------------------ recovery (§10.5)
// After the first frame of an open: a recovery file left for this file means a save was interrupted - unless the file
// shows the save went through or never began, and then it is deleted without a word.
void EditAfterOpen() {
    s.recovery.clear();
    if (!g.path.empty() && !g.loadFailed && !BenchActive()) {
        s.recovery = FindRecovery(RecoveryDir(), g.disk.volume, g.disk.index, g.path);
        if (!s.recovery.empty()) ClassifyLeftovers();
    }
    if (s.recovery.empty()) StripHide(STRIP_RECOVERY);
    else StripShow(STRIP_RECOVERY);
}

bool EditRecoveryRestorable() { return !s.recovery.empty() && s.recovery.back().verdict == RV_TORN; }

static void RecoveryDone() {
    s.recovery.pop_back();
    if (s.recovery.empty()) StripHide(STRIP_RECOVERY);
    Invalidate();
}

void EditCommand(UINT id, UINT) {
    if (s.recovery.empty()) return;
    const RecoveryInfo& r = s.recovery.back();  // the latest interrupted save
    switch (id) {
    case CMD_RECOVERY_OPEN: {  // the file as it was before that save, beside it in %TEMP%, in a window of its own
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"FastMD\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring name = FileNameOf(g.path);
        size_t dot = name.find_last_of(L'.');
        std::wstring out = dir + name.substr(0, dot) + Tr(S_ED_RECOVERED) + (dot == std::wstring::npos ? L".md" : name.substr(dot));
        if (!RecoveryRebuild(r, g.path.c_str(), out.c_str())) { ShowToast(Tr(S_ED_RECOVERY_FAILED), 3000); break; }
        wchar_t exe[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
        std::wstring cl = L"\"" + std::wstring(exe) + L"\" \"" + out + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe, cl.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            AllowSetForegroundWindow(pi.dwProcessId);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        break;
    }
    case CMD_RECOVERY_RESTORE: {  // the saved bytes back at pb, the old length, then the file read again
        DWORD e = 0;
        if (r.verdict != RV_TORN || !RecoveryRestore(r, g.path.c_str(), RecoveryDir(), &e)) {
            DebugLog("recovery restore refused or failed: %lu", e);
            // the file is not the torn one any more (checked again under an exclusive handle): Restore goes
            if (e == ERROR_INVALID_DATA) s.recovery.back().verdict = RV_CHANGED;
            ForceFullRedraw();
            Invalidate();
            ShowToast(Tr(S_ED_RECOVERY_FAILED), 3000);
            break;
        }
        DeleteFileW(r.file.c_str());
        RecoveryDone();
        ReloadDocument();
        break;
    }
    case CMD_RECOVERY_DELETE:
        DeleteFileW(r.file.c_str());
        RecoveryDone();
        break;
    }
}

// ------------------------------------------------------------------------------------------------ test hooks (§13.5)
// FASTMD_TEST_HOOKS=1: WM_COPYDATA dwData 1 = a splice "at\tlen\ttext" through the splice primitive and the swap (reading
// mode too, nothing saved) → 1 done, 0 refused; dwData 2 = SaveSource now (a flush point) → 1 + the SaveState;
// dwData 3 = a modal loop for "<ms>" milliseconds → 1 when it is over.
LRESULT EditCopyData(const COPYDATASTRUCT* cd) {
    if (!HooksOn() || !cd || !g.ready || g.firstFrame || g.path.empty()) return 0;
    SaveState st;
    if (cd->dwData == 1) {
        std::wstring m((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        size_t t1 = m.find(L'\t'), t2 = t1 == std::wstring::npos ? t1 : m.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos) return 0;
        uint32_t at = (uint32_t)wcstoul(m.c_str(), nullptr, 10), len = (uint32_t)wcstoul(m.c_str() + t1 + 1, nullptr, 10);
        std::wstring text = m.substr(t2 + 1);
        if (EditBaseline(&st) != BL_OK || !EditSplice(at, len, text)) return 0;
        EditReparse(at, len, (uint32_t)text.size());
        return 1;
    }
    if (cd->dwData == 2) {
        if (EditBaseline(&st) != BL_OK) return 1 + SS_FAILED;
        return 1 + EditSave(true);
    }
    if (cd->dwData == 3) {
        // a modal loop of its own for the given milliseconds, as a menu or a dialog runs one (§10.10): a test sees
        // what has to wait inside it - the reload, a splice - and that it happens once the loop is over
        std::wstring arg((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        DWORD ms = std::min<DWORD>(10000, (DWORD)wcstoul(arg.c_str(), nullptr, 10));
        ModalScope modal;
        ULONGLONG end = GetTickCount64() + ms;
        MSG m;
        while (GetTickCount64() < end) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) {
                    PostQuitMessage((int)m.wParam);
                    return 1;
                }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        return 1;
    }
    return 0;
}

void EditTimer(UINT_PTR id) {
    if (id == TIMER_EDIT_UI) {
        KillTimer(g.hwnd, TIMER_EDIT_UI);
        UiaDocumentChanged();
        UiaSelectionChanged();
    }
}

// ------------------------------------------------------------------------------------------------ queries (§13.2)
bool EditQuery(UINT q, LPARAM lp, LRESULT* out) {
    const bool doc = !g.path.empty() && !g.loadFailed;
    // the text the disk holds: the baseline once one was taken, else what was loaded (reading mode is never dirty)
    const std::wstring* disk = g.disk.valid ? &g.disk.text : doc ? &g.src : nullptr;
    switch (q) {
    case Q_EDIT_DIRTY: *out = EditDirty(); return true;
    case Q_RELOADS: *out = g.reloads; return true;
    case Q_SAVES: *out = g.saves; return true;
    case Q_SRC_HASH:
    case Q_SRC_LEN: {
        const std::wstring* t = lp == 0 ? (doc ? &g.src : nullptr) : disk;
        *out = !t ? -1 : q == Q_SRC_HASH ? (LRESULT)Fnv32(*t) : (LRESULT)t->size();
        return true;
    }
    case Q_EDIT_SAVE_STATE:
        *out = !EditDirty() ? SS_SAVED : s.saveState != SS_SAVED ? s.saveState : SS_PENDING;
        return true;
    case Q_EDIT_ENC: *out = doc ? (LRESULT)(g.disk.cp | (UINT)g.disk.header.size() << 24) : -1; return true;
    case Q_EDIT_EOL: {
        if (!disk) { *out = -1; return true; }
        DiskState d;
        CountEols(*disk, d);  // before a baseline exists: the loaded text's
        const wchar_t* e = g.disk.valid ? g.eol.c_str() : DiskEol(d);
        *out = !wcscmp(e, L"\r\n") ? 1 : !wcscmp(e, L"\r") ? 2 : 0;
        return true;
    }
    case Q_UNDO_DEPTH: *out = (LRESULT)(lp ? s.undo.RedoDepth() : s.undo.Depth()); return true;
    case Q_MAP_SELFCHECK:
        if (lp != 1) return false;
        *out = s.selfcheckFailures;
        return true;
    case Q_EDIT_STATS: {
        uint32_t n = std::min<uint32_t>(s.samples, (uint32_t)std::size(s.ring));
        std::vector<uint32_t> all, part;
        for (uint32_t i = 0; i < n; i++) all.push_back(s.ring[i].parse + s.ring[i].carry + s.ring[i].install);
        switch (lp) {
        case 0: *out = Percentile(all, 50); break;
        case 1: *out = Percentile(all, 95); break;
        case 2: *out = Percentile(all, 100); break;
        case 3: *out = n; break;
        default:
            for (uint32_t i = 0; i < n; i++) part.push_back(lp == 4 ? s.ring[i].parse : lp == 5 ? s.ring[i].carry : s.ring[i].install);
            *out = Percentile(part, 50);
        }
        return true;
    }
    case Q_EDIT_BUSY: {  // what is still on its way (the bits of §13.2 that exist so far): tests settle on 0
        LRESULT b = g.jobsPending > 0 ? 8 : 0;
        for (const Image& im : g.doc.images)
            if (im.state == RS_PENDING) { b |= 32; break; }
        if (g.animating) b |= 128;
        *out = b;
        return true;
    }
    case Q_EDIT_STRIP: *out = StripKind(); return true;
    case Q_EDIT_TOOL: *out = StripButtonCenter((UINT)(lp & 0xFFFF)); return true;
    case Q_RELAYOUT_ALL: {
        // The oracle for the swap (T7): every layout made anew from the model, every height exact as the background
        // measuring leaves it, the scroll position kept; a shot of this frame must equal one taken after an edit.
        g.gen++;  // measure jobs in flight are dropped
        g.jobsPending = 0;
        ClearLayoutCache();
        UpdateColumns();
        InitGeometry();
        for (uint32_t i = 0; i < g.doc.blocks.size(); i++) {
            const Block& b = g.doc.blocks[i];
            if (BlockHidden(b) || (g.known[i] && b.kind != BK_IMAGE)) continue;  // as MeasureThread does
            BlockLayout* L = LayoutBlock(g.doc, g.typo, i, LayoutWidthFor(b, g.textW, g.wideW));
            g.H[i] = L->height;
            g.known[i] = 1;
            delete L;
        }
        RecomputeY();
        g.scrollY = g.targetY = std::clamp(g.scrollY, 0.f, MaxScroll());
        g.animating = false;
        ForceFullRedraw();
        Invalidate();
        *out = 1;
        return true;
    }
    }
    return false;
}
