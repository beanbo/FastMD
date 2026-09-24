// Edit mode's glue (docs/EDIT-MODE.md §3.1): where the window-free core (editcore: mapping, operations, undo; editfile:
// encoding, the write, recovery) meets `g`. It holds the edit session - the caret in source space, the history, the
// save states - and runs entering and leaving, input, the model swap after every change, autosave with its retries,
// the conflict and adoption rules, the journal, the leave-document questions, the test hooks and the queries.
//
// Phase 1c built the swap, the baseline, the save and the recovery strip, which reading mode uses too (a ticked task
// box is a splice, a save and a swap, T2). Phase 2a adds the editor itself: the guards first (entry checks, the
// other-window mutex, the write probe, the leave-document rules, close and session end, conflicts, the modal queue, the
// journal), then the caret, navigation, typing, in-block Backspace / Delete, undo and autosave.
#include "app.h"
#include "editcore.h"
#include "editfile.h"
#include <commdlg.h>
#include <cstdarg>
#include <shlobj.h>

namespace {
bool EnvOn(const wchar_t* name) {
    wchar_t v[8] = {};
    return GetEnvironmentVariableW(name, v, 8) > 0 && v[0] == L'1';
}
std::wstring EnvStr(const wchar_t* name) {
    wchar_t v[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, v, (DWORD)std::size(v));
    return n && n < std::size(v) ? std::wstring(v, n) : std::wstring();
}
// FASTMD_EDIT_SELFCHECK=1: the map is checked after every swap, and every save decodes its whole output (§13.5)
bool SelfCheckOn() {
    static const bool on = EnvOn(L"FASTMD_EDIT_SELFCHECK");
    return on;
}
bool HooksOn() { return EditTestHooks(); }
// FASTMD_CARET_STEADY=1: the caret is always drawn and never blinks, whatever the activation (T17)
bool CaretSteady() {
    static const bool on = EnvOn(L"FASTMD_CARET_STEADY");
    return on;
}
// FASTMD_EDIT_DEBOUNCE_CHARS: from this source length typing re-parses at most every 150 ms (§5.7; 0 = always)
uint32_t DeferChars() {
    static const uint32_t n = [] {
        std::wstring v = EnvStr(L"FASTMD_EDIT_DEBOUNCE_CHARS");
        return v.empty() ? 262144u : (uint32_t)wcstoul(v.c_str(), nullptr, 10);
    }();
    return n;
}
// FASTMD_AUTOSAVE_MS: the autosave delay (tests, T8)
DWORD AutosaveOverride() {
    static const DWORD ms = [] {
        std::wstring v = EnvStr(L"FASTMD_AUTOSAVE_MS");
        return v.empty() ? 0u : (DWORD)wcstoul(v.c_str(), nullptr, 10);
    }();
    return ms;
}

// FASTMD_TEST_ANSWER=leave:yes|no|cancel,leave2:…,rellinks:yes|no[,auto-dismiss:<ms>] (§13.5, T1): every question
// edit mode asks is answered from here - a kind not named answers cancel - so a test never waits on a message box;
// with auto-dismiss the real box is shown and cancelled after that long (modal re-entrancy).
struct TestAnswers { bool on = false; int leave = IDCANCEL, leave2 = IDCANCEL, rellinks = IDCANCEL; DWORD dismiss = 0; };
const TestAnswers& Answers() {
    static const TestAnswers a = [] {
        TestAnswers t;
        std::wstring v = EnvStr(L"FASTMD_TEST_ANSWER");
        if (v.empty()) return t;
        t.on = true;
        for (size_t at = 0; at <= v.size();) {
            size_t e = v.find(L',', at);
            if (e == std::wstring::npos) e = v.size();
            std::wstring item = v.substr(at, e - at);
            size_t c = item.find(L':');
            if (c != std::wstring::npos) {
                std::wstring k = item.substr(0, c), val = item.substr(c + 1);
                int ans = val == L"yes" ? IDYES : val == L"no" ? IDNO : IDCANCEL;
                if (k == L"leave") t.leave = ans;
                else if (k == L"leave2") t.leave2 = ans;
                else if (k == L"rellinks") t.rellinks = ans;
                else if (k == L"auto-dismiss") t.dismiss = (DWORD)wcstoul(val.c_str(), nullptr, 10);
            }
            at = e + 1;
        }
        return t;
    }();
    return a;
}

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

// One save on the save worker (§10.3): sources of a million characters and more are written from a snapshot, off the
// UI thread; one at a time, and a flush point waits for it (without pumping) before it saves itself.
struct SaveJob {
    uint32_t serial = 0;
    std::wstring text, path, dir;
    DiskState disk;
    RecoveryInfo pending;
    bool hasPending = false, fullProof = false, flushPoint = false;
    SaveResult result;
    HANDLE thread = nullptr;
};

// what waits for the end of a modal loop (§10.10)
struct Deferred { UINT msg; WPARAM wp; LPARAM lp; };

const uint32_t kWorkerChars = 1u << 20;  // from here autosave runs on the save worker

struct Session {
    // ---- history, swaps, recovery (Phase 1c)
    UndoStack undo;                    // the document session's history (cleared by every load)
    SaveState saveState = SS_SAVED;    // what the last save (or the watcher, or the probe) found out
    uint32_t selfcheckFailures = 0;    // Q_MAP_SELFCHECK lp 1
    Sample ring[128] = {};
    uint32_t samples = 0;
    std::vector<RecoveryInfo> recovery;  // interrupted saves of the open file (the RECOVERY strip)
    RecoveryInfo pending;                // the recovery file standing for the last flush, while saves go unflushed
    // ---- the caret: source offsets are the truth (§6.1); the text positions are derived after every swap
    EditState st;
    TextPos focus{0, -1, -1}, anchor{0, -1, -1};
    uint16_t trail = 0;
    int dir = 1;                       // which way TextOfSrc resolves the caret after a change (after an insertion: +1)
    wchar_t pendingHigh = 0;           // a high surrogate waiting for its low half (§2.7)
    // ---- raw-while-typing (§6.9): what the model is parsed as while the caret stays where it was typed - an HTML block's
    // first `<` masked (the line reads as text), a picture or formula paragraph shown as its source
    std::vector<uint32_t> masks;
    std::vector<std::pair<uint32_t, uint32_t>> raw;
    // ---- entering and leaving
    HANDLE mutex = nullptr;            // Local\FastMD.edit.<volume>-<index> while editing (§10.11)
    DWORD enteredAt = 0;
    bool enteredByDouble = false;      // a third click right after it cancels the entry (UX-5)
    uint32_t entryReadPos = UINT32_MAX;  // ... and selects reading mode's paragraph at the double click's text position
    bool hintPending = false;          // the first entry's hint waits until that third click can no longer come
    uint32_t splicesSinceEntry = 0;
    DWORD leftAt = 0;                  // the Esc that left edit mode: the next one within 1 s never closes (UX-6)
    bool entryPending = false;         // asked for while a big document's full parse was running (§2.1)
    DWORD entryAskedAt = 0;
    EnterHow entryHow = ENTER_CARET;
    float entryX = 0, entryY = 0;
    float ctxX = 0, ctxY = 0;          // the right-click point of the reading context menu (CMD_EDIT_HERE)
    bool ctxValid = false;
    // ---- the bar's slide (§2.3, §12.1)
    float slideFrom = 0, slideTo = 0, maxComp = 0;
    uint64_t slideT0 = 0;
    // ---- the caret's blink (§12.2)
    bool phaseOn = true, focused = false, sysCaret = false;
    uint32_t toggles = 0;
    // ---- saving (§10.3, §10.4)
    SaveJob* job = nullptr;
    uint32_t jobSerial = 0;
    bool saveArmed = false, retryArmed = false, journalArmed = false;
    int retryStep = 0;
    uint32_t toasted = 0;              // failure kinds already toasted this session (one toast per kind, D8)
    uint32_t failToasts = 0;           // how many (tests: Q_EDIT_SAVE_STATE lp 1)
    uint32_t lastBad = UINT32_MAX;     // the first character the last save could not encode
    std::string lastReason;
    std::wstring keptRecovery;         // a failed save kept the old bytes here (the status tooltip says so)
    DWORD lastSaveAt = 0;
    SaveState shownState = SS_SAVED, lastState = SS_SAVED;  // the status slot, and when its state last changed
    DWORD stateSince = 0;
    bool titleDirty = false;
    // ---- the strips' details
    uint64_t conflictDisk = 0, conflictOurs = 0;
    SaveState leaveWhy = SS_SAVED;
    std::wstring theirs;               // "Overwrite": the disk's version kept until the document closes
    // ---- the journal of unsaved edits (§10.6)
    std::wstring journalFile;          // this process's journal of the open file
    std::vector<JournalInfo> journals; // journals left for it (the RECOVERY strip, when no recovery file waits)
    uint64_t textHash = 0, textHashKey[4] = {};  // the hash a journal is checked against, and what it was taken of
    // ---- the modal queue (§10.10)
    std::vector<Deferred> deferred;
    bool closePending = false;
    bool themePending = false, fullDocPending = false, activatePending = false;  // what else waits for the loop's end
    DWORD activatedAt = 0;             // the last activation that looked at a read-only or missing file
    // ---- questions (Q_LAST_PROMPT)
    int lastPrompt = 0;
    uint32_t prompts = 0;
    // ---- edit mode's file watch (§10.7)
    DWORD rereadMs = 0;
    uint32_t framesPartial = 0, framesFull = 0;  // Q_FRAME_STATS: the counts at the last query
} s;
bool g_autosaveHinted = false;         // the first splice of the process session says that edits save themselves

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

std::wstring Format(StrId id, ...) {
    wchar_t b[1024];
    va_list ap;
    va_start(ap, id);
    _vsnwprintf_s(b, _TRUNCATE, Tr(id), ap);
    va_end(ap);
    return b;
}

// a byte count as the strips show it
std::wstring SizeText(uint64_t n) {
    wchar_t b[64];
    if (n < 1024) swprintf_s(b, Tr(S_ED_SIZE_B), (unsigned)n);
    else if (n < 1024 * 1024) swprintf_s(b, Tr(S_ED_SIZE_KB), n / 1024.0);
    else swprintf_s(b, Tr(S_ED_SIZE_MB), n / (1024.0 * 1024.0));
    return b;
}

std::wstring CodePageName(UINT cp) {
    CPINFOEXW info{};
    if (!GetCPInfoExW(cp, 0, &info)) return std::to_wstring(cp);
    // Windows pads the number with two blanks ("1251  (ANSI - кириллица)"): one reads better in a sentence
    std::wstring name;
    for (const wchar_t* p = info.CodePageName; *p; p++)
        if (*p != L' ' || name.empty() || name.back() != L' ') name += *p;
    return name;
}

// why a save failed, in the words of the strips and the questions (§2.5, §10.8)
const wchar_t* WhyText(SaveState st) {
    switch (st) {
    case SS_BUSY: return Tr(S_ED_WHY_BUSY);
    case SS_DENIED: case SS_READONLY: return Tr(S_ED_WHY_DENIED);
    case SS_MISSING: return Tr(S_ED_WHY_MISSING);
    case SS_CONFLICT: return Tr(S_ED_WHY_CONFLICT);
    case SS_UNENCODABLE: return Tr(S_ED_WHY_ENCODING);
    case SS_UNKNOWN: return Tr(S_ED_WHY_UNKNOWN);
    default: return Tr(S_ED_WHY_FAILED);
    }
}

// the character a save could not encode, as the strip quotes it
std::wstring BadCharText() {
    if (s.lastBad == UINT32_MAX || s.lastBad >= g.src.size()) return L"?";
    uint32_t n = s.lastBad + 1 < g.src.size() && g.src[s.lastBad] >= 0xD800 && g.src[s.lastBad] <= 0xDBFF ? 2 : 1;
    return g.src.substr(s.lastBad, n);
}

// ------------------------------------------------------------------------------------------------ questions (§10.8)
// A message box inside the modal scope, or the test's answer (FASTMD_TEST_ANSWER). kind: 1 LEAVE, 2 LEAVE2, 3 RELLINKS.
BOOL CALLBACK DismissOne(HWND h, LPARAM) {
    wchar_t cls[16];
    if (GetClassNameW(h, cls, 16) && !wcscmp(cls, L"#32770")) {
        PostMessageW(h, WM_COMMAND, IDCANCEL, 0);
        PostMessageW(h, WM_COMMAND, IDNO, 0);  // a Yes / No box has no Cancel
    }
    return TRUE;
}
DWORD WINAPI DismissBox(void* p) {
    Sleep((DWORD)(uintptr_t)p);
    EnumThreadWindows(GetWindowThreadProcessId(g.hwnd, nullptr), DismissOne, 0);
    return 0;
}
int Ask(int kind, const std::wstring& text, UINT buttons) {
    s.lastPrompt = kind;
    s.prompts++;
    const TestAnswers& a = Answers();
    if (a.on && !a.dismiss) {
        int ans = kind == 1 ? a.leave : kind == 2 ? a.leave2 : a.rellinks;
        if ((buttons & MB_TYPEMASK) == MB_YESNO && ans == IDCANCEL) ans = IDNO;
        return ans;
    }
    if (a.on && a.dismiss)
        if (HANDLE th = CreateThread(nullptr, 64 * 1024, DismissBox, (void*)(uintptr_t)a.dismiss, 0, nullptr)) CloseHandle(th);
    ModalScope modal;
    return MessageBoxW(g.hwnd, text.c_str(), L"FastMD", buttons | MB_ICONWARNING);
}
}  // namespace

bool EditTestHooks() {
    static const bool on = EnvOn(L"FASTMD_TEST_HOOKS");
    return on;
}

// ------------------------------------------------------------------------------------------------ the caret (§6, §12.2)
namespace {
bool InPhantom() { return s.st.phantom.kind != PH_NONE && s.st.phantom.in; }

// The phantom row (§6.7) as the view draws it: next to which block, how tall, where its caret stands - at the content
// edge of the container level it is in. It lives while the caret is in it or in its block; it goes otherwise.
void SyncPhantom() {
    Phantom& ph = s.st.phantom;
    int32_t pb = -1;
    if (ph.kind != PH_NONE && g.editing && g.doc.hasMap) {
        pb = PhantomBlock(g.doc, g.src, ph);
        if (pb < 0 || !PhantomAlive(g.doc, g.src, s.st, s.focus.block)) {
            ph = Phantom{};
            pb = -1;
        }
    }
    const Doc& d = g.doc;
    if (pb >= 0 && ph.kind == PH_AFTER) {  // after an HTML block drawn as several: after the last of them
        const BlockSrc& bs = d.blockSrc[pb];
        if ((bs.flags & BS_RAW) && bs.rawId >= 0)
            while ((size_t)pb + 1 < d.blocks.size() && d.blockSrc[pb + 1].rawId == bs.rawId) pb++;
    }
    float x = 0;
    if (pb >= 0) {
        float w;
        BlockBox((uint32_t)pb, &x, &w);  // a pending break: a line of the block itself
        if (ph.kind != PH_BREAK) {
            // the edge of the innermost container it is in: where a block directly inside that container starts
            x = TextLeft();
            int32_t ci = d.blockSrc[pb].container;
            int depth = 0;
            for (int32_t k = ci; k >= 0; k = d.containers[k].parent) depth++;
            for (; ci >= 0 && depth > ph.depth; depth--) ci = d.containers[ci].parent;
            if (ci >= 0) {
                const ContainerSrc& c = d.containers[ci];
                for (uint32_t k = c.firstBlock; k <= c.lastBlock && k < d.blocks.size(); k++)
                    if (d.blockSrc[k].container == ci && !(d.blockSrc[k].flags & BS_SYNTH)) {
                        BlockBox(k, &x, &w);
                        break;
                    }
            }
        }
    }
    float line = g.typo.lineH[R_BODY], h = pb < 0 ? 0.f : ph.kind == PH_BREAK ? line : line + 16.f;
    bool caret = pb >= 0 && ph.in;
    if (pb == g.phantomBlock && h == g.phantomH && (ph.kind == PH_BEFORE) == g.phantomBefore && x == g.phantomX &&
        caret == g.phantomCaret)
        return;
    g.phantomBlock = pb;
    g.phantomBefore = ph.kind == PH_BEFORE;
    g.phantomBreak = ph.kind == PH_BREAK;
    g.phantomCaret = caret;
    g.phantomH = h;
    g.phantomLine = line;
    g.phantomX = x;
    if (g.Y.size() == d.blocks.size()) RecomputeY();
    Invalidate();
}

// the caret's text position and everything view.cpp draws from, out of the session
void Publish() {
    g.selFocus = s.focus.t;
    g.selAnchor = s.st.anchor == s.st.focus ? s.focus.t : s.anchor.t;
    g.caretBlock = s.focus.block;
    g.caretCell = s.focus.cell;
    g.caretTrail = s.trail;
    g.caretAff = s.st.lineAff;
    if (s.st.atom >= 0) {
        g.selAtomBlock = AtomBlockOf(g.doc, s.st.atom);
        g.selAtomImage = s.st.atom & kAtomBlock ? -1 : s.st.atom;
        g.selAnchor = g.selFocus;
    } else {
        g.selAtomBlock = g.selAtomImage = -1;
    }
    SyncPhantom();
}

// Where the caret is drawn, from where it is in the source (§6.4). Normalisation (F7): the caret goes to where typed
// text would really go - except in trailing blanks, which it stands in on purpose, and during a deferred burst.
void ResolveCaret(bool normalize) {
    if (!g.doc.hasMap) return;
    if (g.doc.blocks.empty()) {  // nothing left to stand in: typing starts the document again (OpType)
        s.focus = s.anchor = TextPos{0, -1, -1};
        s.trail = 0;
        s.st.atom = -1;
        Publish();
        return;
    }
    const uint32_t n = (uint32_t)g.src.size();
    s.st.focus = std::min(s.st.focus, n);
    s.st.anchor = std::min(s.st.anchor, n);
    uint16_t trail = 0;
    TextPos f = TextOfSrc(g.doc, g.src, s.st.focus, s.dir, &trail);
    if (f.block < 0) f = TextPos{0, 0, -1};
    if (normalize && !trail && s.st.burstBeg == UINT32_MAX && s.st.atom < 0 && !InPhantom()) {
        uint32_t k = SrcOfText(g.doc, g.src, f, MAP_CARET);
        if (k != UINT32_MAX && k <= n) {
            bool same = s.st.anchor == s.st.focus;
            s.st.focus = k;
            if (same) s.st.anchor = k;
        }
    }
    s.focus = f;
    s.trail = trail;
    if (s.st.anchor == s.st.focus) {
        s.anchor = f;
    } else {
        uint16_t tr = 0;
        s.anchor = TextOfSrc(g.doc, g.src, s.st.anchor, s.dir, &tr);
        if (s.anchor.block < 0) s.anchor = f;
    }
    if (s.st.atom >= 0 && AtomBlockOf(g.doc, s.st.atom) < 0) s.st.atom = -1;  // the atom is gone
    Publish();
}

// the hidden system caret follows the drawn one, for Magnifier and screen readers (§12.2)
void SystemCaret() {
    if (!s.sysCaret || !g.editing) return;
    float x, y, h;
    if (CaretPoint(g.selFocus, &x, &y, &h)) {
        float sc = Scale();
        SetCaretPos((int)std::lround(x * sc), (int)std::lround(y * sc));
    }
}

void UpdateCaretVisible() {
    bool v = g.editing && g.caretOn && s.st.atom < 0 && (CaretSteady() || (s.phaseOn && s.focused));
    if (v != g.caretVisible) {
        g.caretVisible = v;
        Invalidate();
    }
}

// the visible phase starts over on every key, click and move (§12.2)
void CaretRestart() {
    s.phaseOn = true;
    UpdateCaretVisible();
    UINT blink = GetCaretBlinkTime();
    if (g.editing && !CaretSteady() && blink != INFINITE && blink) SetTimer(g.hwnd, TIMER_CARET, blink, nullptr);
    else KillTimer(g.hwnd, TIMER_CARET);
    SystemCaret();
}

// The caret is under the bar, a strip or the find bar, or below the window: the document moves by the overflow only,
// never re-centred (§12.1).
void RevealCaret() {
    if (!g.editing || g.doc.blocks.empty()) return;
    float cx = 0, dy, h;
    int32_t ab = s.st.atom >= 0 ? AtomBlockOf(g.doc, s.st.atom) : -1;
    if (ab >= 0 && (s.st.atom & kAtomBlock)) {
        EnsureLayout((uint32_t)ab);
        RecomputeY();
        dy = g.Y[ab];
        h = g.H[ab];
    } else if (g.phantomCaret && g.phantomBlock >= 0) {  // the phantom row (§6.7)
        EnsureLayout((uint32_t)g.phantomBlock);
        RecomputeY();
        dy = g.phantomY;
        h = g.phantomLine;
    } else if (!CaretGeomAt(g.selFocus, g.caretBlock, g.caretCell, &cx, &dy, &h, true)) {
        return;
    }
    float top = EditRevealTop(), target = g.scrollY;
    if (dy < g.scrollY + top) target = dy - top;
    else if (dy + h > g.scrollY + ViewH() - 48.f) target = std::min(dy - top, dy + h - ViewH() + 48.f);
    target = std::clamp(target, 0.f, MaxScroll());
    if (std::fabs(target - g.scrollY) > 0.5f) {
        g.userMoved = true;
        g.restoreBlock = -1;
        ScrollTo(target, false);
    }
    int32_t b = g.caretBlock;
    float vx, vw, cw;
    if (ab < 0 && b >= 0 && HScrollInfo((uint32_t)b, &vx, &vw, &cw)) {  // a code block or table wider than its box
        float cur = HScrollOf((uint32_t)b), x = cx - vx + cur;
        if (x < cur + 24.f || x > cur + vw - 64.f) HScrollSet((uint32_t)b, x - vw * 0.35f);
    }
}

void RawLifetime();
void CaretMoved() {
    s.undo.BreakCoalescing();
    s.pendingHigh = 0;
    RawLifetime();  // raw-while-typing ends where the caret leaves what it holds (§6.9)
    CaretRestart();
    if (g.hwnd) SetTimer(g.hwnd, TIMER_EDIT_UI, 100, nullptr);  // a screen reader hears it once the moves pause
    BarChanged();  // the style label follows the caret's block
}

// ---- caret stops (§6.2), in text order
bool HasStops(int32_t b) {
    const Doc& d = g.doc;
    if (b < 0 || (size_t)b >= d.blocks.size() || !d.hasMap) return false;
    const BlockSrc& bs = d.blockSrc[b];
    if (BlockHidden(d.blocks[b]) || (bs.flags & BS_SYNTH)) return false;
    if ((bs.flags & BS_RAW) && bs.rawId >= 0 && bs.rawId != b) return false;  // an HTML block is one stop, at its first
    return true;
}
// the text range a position lives in: its block's text, or its cell's
bool RangeOfPos(const TextPos& p, uint32_t* lo, uint32_t* hi) {
    const Doc& d = g.doc;
    if (p.block < 0 || (size_t)p.block >= d.blocks.size()) return false;
    const Block& b = d.blocks[p.block];
    if (b.kind == BK_TABLE && b.aux < d.tables.size()) {
        const Table& tb = d.tables[b.aux];
        if (p.cell < 0 || (uint32_t)p.cell >= tb.rows * tb.cols) return false;
        const Cell& c = d.cells[tb.cellOff + p.cell];
        *lo = c.textOff;
        *hi = c.textOff + c.textLen;
        return true;
    }
    *lo = b.textOff;
    *hi = b.textOff + b.textLen;
    return true;
}
int32_t CellCount(int32_t b) {
    const Block& bl = g.doc.blocks[b];
    if (bl.kind != BK_TABLE || bl.aux >= g.doc.tables.size()) return 0;
    return (int32_t)(g.doc.tables[bl.aux].rows * g.doc.tables[bl.aux].cols);
}
TextPos CellEdge(int32_t b, int32_t c, bool end) {
    const Cell& cell = g.doc.cells[g.doc.tables[g.doc.blocks[b].aux].cellOff + c];
    return TextPos{end ? cell.textOff + cell.textLen : cell.textOff, b, c};
}
// the first and last caret stops of a block
TextPos FirstStopOf(int32_t b) {
    const Block& bl = g.doc.blocks[b];
    if (CellCount(b)) return CellEdge(b, 0, false);
    return TextPos{bl.textOff, b, -1};
}
TextPos LastStopOf(int32_t b) {
    const Block& bl = g.doc.blocks[b];
    if (int32_t n = CellCount(b)) return CellEdge(b, n - 1, true);
    if (IsAtomBlock(g.doc, b)) return TextPos{bl.textOff, b, -1};
    TextPos p{bl.textOff + bl.textLen, b, -1};
    while (p.t > bl.textOff && !CaretStop(g.doc, p)) p.t--;  // before a synthesized tail (the footnote's arrow)
    return p;
}
int32_t NextStopBlock(int32_t b, int dir) {
    for (int32_t i = b + dir; i >= 0 && (size_t)i < g.doc.blocks.size(); i += dir)
        if (HasStops(i)) return i;
    return -1;
}
TextPos DocFirst() {
    int32_t b = NextStopBlock(-1, 1);
    return b >= 0 ? FirstStopOf(b) : TextPos{0, -1, -1};
}
TextPos DocLast() {
    int32_t b = NextStopBlock((int32_t)g.doc.blocks.size(), -1);
    return b >= 0 ? LastStopOf(b) : TextPos{0, -1, -1};
}
// the atom stop of a block atom (an HTML block's first block)
TextPos AtomStop(int32_t b) {
    const BlockSrc& bs = g.doc.blockSrc[b];
    int32_t k = (bs.flags & BS_RAW) && bs.rawId >= 0 ? bs.rawId : b;
    return TextPos{g.doc.blocks[k].textOff, k, -1};
}
// the nearest caret stop to p, looking in dir first (0: either way)
TextPos SnapStop(TextPos p, int dir) {
    const Doc& d = g.doc;
    if (p.block < 0 || (size_t)p.block >= d.blocks.size()) return DocFirst();
    if (IsAtomBlock(d, p.block)) return AtomStop(p.block);
    if (!HasStops(p.block)) {
        int32_t b = NextStopBlock(p.block, dir < 0 ? -1 : 1);
        if (b < 0) b = NextStopBlock(p.block, dir < 0 ? 1 : -1);
        if (b < 0) return TextPos{0, -1, -1};
        return dir < 0 ? LastStopOf(b) : FirstStopOf(b);
    }
    if (CellCount(p.block) && p.cell < 0) {  // a table position names its cell
        for (int32_t c = 0; c < CellCount(p.block); c++) {
            TextPos q = CellEdge(p.block, c, false);
            uint32_t lo = q.t, hi = CellEdge(p.block, c, true).t;
            if (p.t >= lo && p.t <= hi) { p.cell = c; break; }
        }
        if (p.cell < 0) p = FirstStopOf(p.block);
    }
    if (CaretStop(d, p)) return p;
    uint32_t lo, hi;
    if (!RangeOfPos(p, &lo, &hi)) return FirstStopOf(p.block);
    p.t = std::clamp(p.t, lo, hi);
    for (uint32_t k = 1; k <= hi - lo + 1; k++) {
        int first = dir ? dir : 1;
        for (int sgn : {first, -first}) {
            int64_t t = (int64_t)p.t + sgn * (int64_t)k;
            if (t < lo || t > hi) continue;
            TextPos q{(uint32_t)t, p.block, p.cell};
            if (CaretStop(d, q)) return q;
        }
    }
    return FirstStopOf(p.block);
}

// ---- the cluster function of the app (§6.2): DirectWrite's clusters of the layout that draws the caret's text
struct ClusterCtx { int32_t block, cell; };
uint32_t AppClusters(const std::wstring& text, uint32_t pos, int dir, void* ctx) {
    const ClusterCtx* c = (const ClusterCtx*)ctx;
    if (!c || c->block < 0 || (size_t)c->block >= g.doc.blocks.size() || (size_t)c->block >= g.cache.size())
        return GraphemeLite(text, pos, dir, nullptr);
    const Block& b = g.doc.blocks[c->block];
    BlockLayout* L = EnsureLayout((uint32_t)c->block);
    IDWriteTextLayout* tl = nullptr;
    uint32_t off = b.textOff, len = b.textLen;
    if (b.kind == BK_TABLE && L->table && c->cell >= 0 && (size_t)c->cell < L->table->cells.size()) {
        tl = L->table->cells[c->cell];
        const Cell& cell = g.doc.cells[g.doc.tables[b.aux].cellOff + c->cell];
        off = cell.textOff;
        len = cell.textLen;
    } else if (b.kind == BK_TEXT || b.kind == BK_CODE) {
        tl = L->text;
    }
    UINT32 n = 0;
    if (!tl || tl->GetClusterMetrics(nullptr, 0, &n) != E_NOT_SUFFICIENT_BUFFER || !n) return GraphemeLite(text, pos, dir, nullptr);
    std::vector<DWRITE_CLUSTER_METRICS> m(n);
    if (FAILED(tl->GetClusterMetrics(m.data(), n, &n))) return GraphemeLite(text, pos, dir, nullptr);
    uint32_t sum = 0;
    for (UINT32 k = 0; k < n; k++) sum += m[k].length;
    if (sum != len || pos < off || pos > off + len) return GraphemeLite(text, pos, dir, nullptr);
    uint32_t at = off, prev = off;
    for (UINT32 k = 0; k < n; k++) {
        uint32_t next = at + m[k].length;
        if (dir > 0 && at <= pos && pos < next) return next;
        if (dir < 0 && at < pos && pos <= next) return at;
        prev = at;
        at = next;
    }
    (void)prev;
    return dir > 0 ? off + len : off;
}

ClusterCtx g_clusterCtx;
EditCtx Ctx() {
    g_clusterCtx = ClusterCtx{s.focus.block, s.focus.cell};
    EditCtx c{g.doc, g.src, g.eol.c_str(), AppClusters, &g_clusterCtx, GetTickCount64()};
    c.focusPos = s.focus;
    c.anchorPos = s.anchor;
    c.trail = s.trail;
    return c;
}

bool IsWordCh(wchar_t c) {
    WORD t = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &t);
    return (t & (C1_ALPHA | C1_DIGIT)) || c == L'_' || (c >= 0xD800 && c <= 0xDFFF);
}
bool IsSpaceCh(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == 0xA0 || c == 0x3000; }

// ---- moves in text space (§6.8)
TextPos CharStep(TextPos p, int dir) {
    const Doc& d = g.doc;
    uint32_t lo, hi;
    if (IsAtomBlock(d, p.block) || !RangeOfPos(p, &lo, &hi)) {  // from an object block: the neighbouring stop
        int32_t b = NextStopBlock(p.block, dir);
        return b < 0 ? p : dir > 0 ? FirstStopOf(b) : LastStopOf(b);
    }
    if (dir > 0 ? p.t < hi : p.t > lo) {
        g_clusterCtx = ClusterCtx{p.block, p.cell};
        EditCtx c{d, g.src, g.eol.c_str(), AppClusters, &g_clusterCtx, 0};
        TextPos q{ClusterStep(c, p.t, dir, lo, hi), p.block, p.cell};
        while (!CaretStop(d, q) && (dir > 0 ? q.t < hi : q.t > lo)) q.t += dir;  // over an atom in one step
        if (CaretStop(d, q)) return q;
    }
    // over the edge: the next cell of a table, else the next block
    if (p.cell >= 0) {
        int32_t c = p.cell + dir;
        if (c >= 0 && c < CellCount(p.block)) return CellEdge(p.block, c, dir < 0);
    }
    int32_t b = NextStopBlock(p.block, dir);
    if (b < 0) return p;
    return dir > 0 ? FirstStopOf(b) : LastStopOf(b);
}
TextPos WordStep(TextPos p, int dir) {
    const std::wstring& t = g.doc.text;
    uint32_t lo, hi;
    if (IsAtomBlock(g.doc, p.block) || !RangeOfPos(p, &lo, &hi)) return CharStep(p, dir);
    uint32_t q = p.t;
    if (dir > 0) {
        if (q >= hi) return CharStep(p, 1);
        if (IsWordCh(t[q])) while (q < hi && IsWordCh(t[q])) q++;
        else while (q < hi && !IsWordCh(t[q]) && !IsSpaceCh(t[q])) q++;
        while (q < hi && IsSpaceCh(t[q])) q++;  // to the start of the next word, as Windows does
    } else {
        if (q <= lo) return CharStep(p, -1);
        while (q > lo && IsSpaceCh(t[q - 1])) q--;
        if (q > lo && IsWordCh(t[q - 1])) while (q > lo && IsWordCh(t[q - 1])) q--;
        else while (q > lo && !IsWordCh(t[q - 1]) && !IsSpaceCh(t[q - 1])) q--;
    }
    return SnapStop(TextPos{q, p.block, p.cell}, dir);
}
bool Before(const TextPos& a, const TextPos& b) {  // a comes before b in the text
    if (a.block != b.block) return a.block < b.block;
    if (a.cell != b.cell) return a.cell < b.cell;
    return a.t < b.t;
}
// The end of a line that wraps and the start of the next one are the same text position. The caret belongs to the line
// the reader aimed at (End, a click right of a line's end, ↓ into a shorter line): lineAff -1 draws it at the end of the
// upper line, after the character before it (§12.2) - never at the start of the next line, where it would jump to.
int8_t AffFor(const TextPos& q, float docY) {
    uint32_t lo, hi;
    if (IsAtomBlock(g.doc, q.block) || !RangeOfPos(q, &lo, &hi) || q.t <= lo || g.doc.text[q.t - 1] == L'\n') return 0;
    int8_t keep = g.caretAff;
    g.caretAff = 0;  // where the position is drawn by itself
    float x, y, h;
    bool ok = CaretGeomAt(q.t, q.block, q.cell, &x, &y, &h, true);
    g.caretAff = keep;
    return ok && y > docY + 0.5f ? -1 : 0;
}

// one visual line (or a page) up or down, keeping the column the caret started from; the gaps between blocks are
// skipped by probing at the next block's top or the previous block's bottom
TextPos LineStep(TextPos p, int dir, float page, int8_t* aff) {
    *aff = 0;
    float cx, dy, h;
    if (IsAtomBlock(g.doc, p.block)) {
        EnsureLayout((uint32_t)p.block);
        RecomputeY();
        cx = TextLeft();
        dy = g.Y[p.block];
        h = g.H[p.block];
    } else if (!CaretGeomAt(p.t, p.block, p.cell, &cx, &dy, &h, true)) {
        return p;
    }
    if (s.st.wantX < 0) s.st.wantX = cx;
    float x = s.st.wantX;
    float probe = page > 0 ? dy + dir * page : dir > 0 ? dy + h + 1.f : dy - 1.f;
    for (int tries = 0; tries < 24; tries++) {
        if (probe < 0 || probe > g.docH) break;
        DocHit hit;
        if (!HitTestDocAt(x, probe - g.scrollY, &hit) || hit.block < 0) break;
        if (hit.above) {  // in the gap above a block: its first line going down, the line before it going up
            int32_t b = hit.block;
            if (dir > 0) probe = g.Y[b] + 1.f;
            else {
                int32_t pb = b - 1;
                while (pb >= 0 && BlockHidden(g.doc.blocks[pb])) pb--;
                if (pb < 0) break;
                probe = g.Y[pb] + g.H[pb] - 1.f;
            }
            if (!HitTestDocAt(x, probe - g.scrollY, &hit) || hit.block < 0) break;
        }
        TextPos q = SnapStop(TextPos{hit.pos, hit.block, hit.cell}, dir);
        if (q.block >= 0 && (dir > 0 ? Before(p, q) : Before(q, p))) {
            *aff = AffFor(q, probe);
            return q;
        }
        probe += dir * std::max(6.f, h * 0.5f);
    }
    return dir > 0 ? DocLast() : DocFirst();
}
// the start or end of the caret's visual line (a table cell's start or end)
TextPos LineEdge(TextPos p, int dir, int8_t* aff) {
    *aff = 0;
    if (IsAtomBlock(g.doc, p.block)) return p;
    uint32_t lo, hi;
    if (p.cell >= 0 && RangeOfPos(p, &lo, &hi)) return SnapStop(TextPos{dir > 0 ? hi : lo, p.block, p.cell}, -dir);
    float cx, dy, h;
    if (!CaretGeomAt(p.t, p.block, p.cell, &cx, &dy, &h, true)) return p;
    DocHit hit;
    if (!HitTestDocAt(dir > 0 ? 1e6f : -1e6f, dy + h * 0.5f - g.scrollY, &hit) || hit.block != p.block) return p;
    TextPos q = SnapStop(TextPos{hit.pos, hit.block, hit.cell}, -dir);
    if (dir > 0) *aff = AffFor(q, dy + h * 0.5f);  // End stays on this line (and is the same again when pressed again)
    return q;
}
// a hard break right after p (§6.6): its blanks are the break, not trailing blanks the caret steps into
bool HardBreakAt(const TextPos& p) {
    return p.block >= 0 && p.t < g.doc.text.size() && g.doc.text[p.t] == L'\n' && g.doc.blocks[p.block].kind != BK_CODE;
}
}  // namespace

// ------------------------------------------------------------------------------------------------ the swap (§5.5)
// A new model from the whole source, put in place of the old one so that whatever did not change stays exactly as it
// was: its layouts, heights, horizontal scroll and pictures, and the view does not move. at / oldLen / newLen: the
// source range the change replaced (at == UINT32_MAX: none, the entry parse). Nothing is pumped in here (R13).
// keepOld: the model before the change is handed back (typing checks what it rendered, §7.3).
void EditReparse(uint32_t at, uint32_t oldLen, uint32_t newLen, Doc* keepOld) {
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
    if (!s.masks.empty()) opt.masks = &s.masks;  // raw-while-typing (§6.9)
    if (!s.raw.empty()) opt.raw = &s.raw;
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
    // In edit mode the top is where the bar, a strip and the find bar end.
    const float top = g.editing ? EditRevealTop() : 0.f;
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
    // 9. The caret's block is laid out now (the paint path never lays out), then the anchor is applied.
    RecomputeY();
    if (g.editing) {
        ResolveCaret(true);
        if (g.caretBlock >= 0 && (size_t)g.caretBlock < g.cache.size() && !g.cache[g.caretBlock]) {
            float h0 = g.H[g.caretBlock];
            EnsureLayout((uint32_t)g.caretBlock);
            if (g.H[g.caretBlock] != h0) RecomputeY();
        }
    }
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
    // 12. The selection: edit mode derived it from the source caret above; reading mode keeps it where the text around
    // it is the same.
    if (!g.editing) {
        auto move = [&](uint32_t t) -> uint32_t {
            if (!textSame) return std::min<uint32_t>(t, (uint32_t)g.doc.text.size());
            if (t <= ts.oldBeg) return t;
            if (t >= ts.oldEnd) return t - ts.oldEnd + ts.newEnd;
            return ts.newBeg;
        };
        g.selAnchor = move(g.selAnchor);
        g.selFocus = move(g.selFocus);
    }
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
    if (!g.editing) MeasureUnknown();  // edit mode measures after a pause in typing (TIMER_EDIT_IDLE)
    // 16.
    s.ring[s.samples++ % std::size(s.ring)] = Sample{Micros(t0, t1), Micros(t1, t2), Micros(t2, t3)};
    if (keepOld) *keepOld = std::move(old);
    Invalidate();
}

// ------------------------------------------------------------------------------------------------ splice (§7.1)
// From here on the glue runs once per key, click, command, timer or save - never in a loop over the document's blocks as
// the swap above does - so nothing is inlined, for size (§1 principle 3; see editcore.cpp's operations).
#pragma inline_depth(0)
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

namespace {
// what a save's result means for the baseline, the stamp and the recovery strip (both the synchronous save and the
// worker's end here)
SaveState TakeResult(SaveResult& r) {
    DebugLog("save: state %d (%s), error %lu, flush %.2f ms", (int)r.state, r.reason, r.error, r.flushMs);
    s.pending = std::move(r.pending);
    s.lastBad = r.bad;
    s.lastReason = r.reason;
    if (r.state == SS_SAVED || r.adopted) {
        g.disk = std::move(r.disk);
        if (r.adopted) g.eol = DiskEol(g.disk);  // re-encoded outside: its line ends from now on (D13)
    }
    if (r.state == SS_SAVED) {
        // our own write: the watcher sees the stamp it left and does not reload
        g.fileTime = g.disk.mtime;
        g.fileSize = g.disk.size;
        g.saves++;
        s.lastSaveAt = GetTickCount();
    }
    if (!r.recoveryKept.empty()) {
        // The write failed half-way and so did putting the old bytes back: the recovery file is now the only copy of
        // them. It goes on the strip at once (§10.3 step 9), not only at the next open.
        s.keptRecovery = r.recoveryKept;
        RecoveryInfo ri;
        if (ReadRecovery(r.recoveryKept, ri)) {
            s.recovery.push_back(std::move(ri));
            ClassifyLeftovers();
        }
        if (!s.recovery.empty()) StripShow(STRIP_RECOVERY);
    }
    return r.state;
}

SaveState SaveWith(bool flushPoint, bool whole, UINT toCp, const std::string& toHeader) {
    if (!g.disk.valid) return SS_FAILED;
    if (g.src == g.disk.text && !whole) {  // equal bytes are never written (UX-5); a flush point still flushes
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
    rq.whole = whole;
    rq.toCp = toCp;
    rq.toHeader = toHeader;
    SaveResult r = SaveSource(rq);
    return s.saveState = TakeResult(r);
}
}  // namespace

SaveState EditSave(bool flushPoint) { return SaveWith(flushPoint, false, 0, std::string()); }

void EditPushStep(EditStep step) {
    if (s.undo.Depth() + s.undo.RedoDepth() == 0) return;  // reading mode adds to a history only where there is one
    s.undo.Push(std::move(step), GetTickCount64());
}

// ------------------------------------------------------------------------------------------------ saving states (§10.4)
namespace {
bool Paused(SaveState st) {  // no autosave until the reader acts: the strip says what
    return st == SS_CONFLICT || st == SS_UNENCODABLE || st == SS_READONLY || st == SS_DENIED || st == SS_MISSING;
}
bool Failing(SaveState st) { return st != SS_SAVED && st != SS_PENDING && st != SS_SAVING && st != SS_OFF; }

void UpdateTitle(bool force) {
    bool d = EditDirty();
    if (!force && d == s.titleDirty) return;
    s.titleDirty = d;
    if (g.hwnd) SetWindowTextW(g.hwnd, WindowTitle().c_str());
}

// the status slot's state changed: PENDING and SAVING show only after 300 ms (UX-24), so a repaint then
void StatusTick() {
    SaveState st = EditSaveState();
    if (st != s.lastState) {
        s.lastState = st;
        s.stateSince = GetTickCount();
        if ((st == SS_PENDING || st == SS_SAVING) && g.hwnd) SetTimer(g.hwnd, TIMER_EDIT_UI, 320, nullptr);
    }
    BarChanged();
    UpdateTitle(false);
    CrashPrivacy(g.editing || EditDirty());
}

DWORD AutosaveDelay() {
    if (DWORD o = AutosaveOverride()) return o;
    size_t n = g.src.size();
    DWORD ms = n < (1u << 20) ? 800 : n < (8u << 20) ? 2000 : 5000;
    if (g.disk.remote || g.disk.cloud) {  // at least 5 s between saves on remote or cloud files (D8)
        DWORD since = GetTickCount() - s.lastSaveAt;
        if (s.lastSaveAt && since < 5000) ms = std::max<DWORD>(ms, 5000 - since);
    }
    return ms;
}

void ArmAutosave() {
    if (!g.cfg.autosave || Paused(s.saveState) || !EditDirty() || !g.hwnd) return;
    SetTimer(g.hwnd, TIMER_EDIT_SAVE, AutosaveDelay(), nullptr);
    s.saveArmed = true;
}

// the journal protects edits the real save does not (§10.6): autosave off or failing
void ArmJournal() {
    if (!EditDirty() || !g.hwnd || g.disk.remote) return;
    if (g.cfg.autosave && !Failing(s.saveState)) return;
    SetTimer(g.hwnd, TIMER_EDIT_JOURNAL, 3000, nullptr);
    s.journalArmed = true;
}

void DeleteJournal() {
    KillTimer(g.hwnd, TIMER_EDIT_JOURNAL);
    s.journalArmed = false;
    if (!s.journalFile.empty()) {
        DeleteFileW(s.journalFile.c_str());
        s.journalFile.clear();
    }
}

void WriteJournalNow() {
    s.journalArmed = false;
    if (!EditDirty() || g.disk.remote) return;
    std::wstring file;
    if (WriteJournal(RecoveryDir(), g.disk.volume, g.disk.index, (g.disk.attributes & FILE_ATTRIBUTE_ENCRYPTED) != 0,
                     g.path, TextHash(g.disk.text), g.disk.cp, g.src, &file))
        s.journalFile = file;
}

void ToastOnce(SaveState st) {
    if (st != SS_BUSY || (s.toasted & (1u << st))) return;  // transient trouble: one toast per kind per session
    s.toasted |= 1u << st;
    s.failToasts++;
    ShowToast(Tr(S_ED_BUSY_TOAST), 3000);
}

void ArmRetry(SaveState st) {
    static const DWORD steps[] = {500, 1000, 2000, 4000, 8000};
    DWORD ms = st == SS_FAILED ? 15000 : s.retryStep < (int)std::size(steps) ? steps[s.retryStep] : 15000;
    s.retryStep++;
    if (g.hwnd) SetTimer(g.hwnd, TIMER_EDIT_RETRY, ms, nullptr);
    s.retryArmed = true;
}

// the bytes our text takes in the file's encoding (the conflict strip's "yours")
uint64_t OurSize() {
    std::string out;
    size_t bad;
    const char* why;
    if (!EncodeText(g.disk.cp, g.src.data(), g.src.size(), out, &bad, &why)) return g.src.size();
    return out.size() + g.disk.header.size();
}

// A save answered: the state, the strips, the retry and the journal follow (§10.4).
void Saved(SaveState st) {
    s.saveState = st;
    switch (st) {
    case SS_SAVED:
        s.retryStep = 0;
        KillTimer(g.hwnd, TIMER_EDIT_RETRY);
        s.retryArmed = false;
        s.keptRecovery.clear();
        for (int k : {STRIP_READONLY, STRIP_MISSING, STRIP_CONFLICT, STRIP_ENCODING, STRIP_LEAVE}) StripHide(k);
        if (!EditDirty()) DeleteJournal();
        else ArmAutosave();  // typed on while the worker saved
        break;
    case SS_BUSY: case SS_UNKNOWN: case SS_FAILED:
        ToastOnce(st);
        ArmRetry(st);
        break;
    case SS_DENIED: case SS_READONLY:
        StripShow(STRIP_READONLY);
        break;
    case SS_MISSING:
        StripShow(STRIP_MISSING);
        ArmRetry(st);
        break;
    case SS_CONFLICT: {
        std::string bytes;
        DWORD e = 0;
        if (ReadDisk(g.path.c_str(), bytes, nullptr, &e) == SS_SAVED) s.conflictDisk = bytes.size();
        s.conflictOurs = OurSize();
        StripShow(STRIP_CONFLICT);
        KillTimer(g.hwnd, TIMER_EDIT_SAVE);
        s.saveArmed = false;
        break;
    }
    case SS_UNENCODABLE:
        StripShow(STRIP_ENCODING);
        KillTimer(g.hwnd, TIMER_EDIT_SAVE);
        s.saveArmed = false;
        break;
    default: break;
    }
    if (Failing(st)) ArmJournal();
    StatusTick();
}

// ---- the save worker (§10.3)
DWORD WINAPI SaveWorker(void* p) {
    SaveJob* j = (SaveJob*)p;
    if (DWORD ms = TestSlowMs().save) Sleep(ms);
    SaveRequest rq;
    rq.path = j->path.c_str();
    rq.text = &j->text;
    rq.disk = &j->disk;
    rq.recoveryDir = j->dir;
    rq.flushPoint = j->flushPoint;
    rq.fullProof = j->fullProof;
    rq.pending = j->hasPending ? &j->pending : nullptr;
    j->result = SaveSource(rq);
    if (g.hwnd && !g.closing) PostMessageW(g.hwnd, WM_APP_SAVED, j->serial, 0);
    return 0;
}

bool StartJob(bool flushPoint) {
    if (s.job || !s.recovery.empty()) return false;
    auto* j = new SaveJob;
    j->serial = ++s.jobSerial;
    j->text = g.src;
    j->path = g.path;
    j->dir = RecoveryDir();
    j->disk = g.disk;
    j->hasPending = !s.pending.file.empty();
    j->pending = s.pending;
    j->fullProof = SelfCheckOn();
    j->flushPoint = flushPoint;
    j->thread = CreateThread(nullptr, 256 * 1024, SaveWorker, j, 0, nullptr);
    if (!j->thread) {
        delete j;
        return false;
    }
    s.pending = RecoveryInfo();  // the job holds it now
    s.job = j;
    StatusTick();
    return true;
}

// a flush point waits for the save in flight (never pumping) and takes its result
void WaitJob(DWORD ms = INFINITE) {
    if (!s.job) return;
    if (WaitForSingleObject(s.job->thread, ms) != WAIT_OBJECT_0) return;
    SaveJob* j = s.job;
    s.job = nullptr;
    CloseHandle(j->thread);
    SaveState st = TakeResult(j->result);
    delete j;
    Saved(st);
}

// The save now. Big sources go to the worker unless this is a flush point; a flush point waits for a save in flight
// first. Returns the state the file is in afterwards (SS_SAVED = the disk holds the text).
SaveState SaveNow(bool flushPoint) {
    if (g.hwnd) KillTimer(g.hwnd, TIMER_EDIT_SAVE);
    s.saveArmed = false;
    if (flushPoint) WaitJob();
    else if (s.job) return SS_SAVING;
    if (!g.disk.valid) return SS_FAILED;
    if (!EditDirty()) {
        if (flushPoint) EditLeaveDocument();
        Saved(SS_SAVED);
        return SS_SAVED;
    }
    if (!flushPoint && g.src.size() >= kWorkerChars && StartJob(false)) return SS_SAVING;
    SaveState st = EditSave(flushPoint);
    Saved(st);
    return st == SS_SAVED && !EditDirty() ? SS_SAVED : st == SS_SAVED ? SS_PENDING : st;
}

// Everything that must be on disk now (leaving, Ctrl+S, Ctrl+E, F5): SS_SAVED, or why not.
SaveState Flush() {
    EditSync();
    return SaveNow(true);
}
}  // namespace

SaveState EditSaveState() {
    if (!EditDirty()) return SS_SAVED;
    if (s.job) return SS_SAVING;
    if (Failing(s.saveState)) return s.saveState;
    if (!g.cfg.autosave) return SS_OFF;
    return SS_PENDING;
}

SaveState EditStatusShown() {
    SaveState st = EditSaveState();
    if (st != s.lastState) {
        s.lastState = st;
        s.stateSince = GetTickCount();
    }
    // nothing new for the first 300 ms of an edit or a save: a quick save never flickers through "Not saved"
    if ((st == SS_PENDING || st == SS_SAVING) && GetTickCount() - s.stateSince < 300) return s.shownState;
    s.shownState = st;
    return st;
}

std::wstring EditStatusTip() {
    SaveState st = EditStatusShown();
    StrId id;
    switch (st) {
    case SS_SAVED: id = S_ED_ST_SAVED; break;
    case SS_PENDING: id = S_ED_ST_PENDING; break;
    case SS_SAVING: id = S_ED_ST_SAVING; break;
    case SS_BUSY: id = S_ED_ST_BUSY; break;
    case SS_DENIED: case SS_READONLY: id = S_ED_ST_DENIED; break;
    case SS_MISSING: id = S_ED_ST_MISSING; break;
    case SS_CONFLICT: id = S_ED_ST_CONFLICT; break;
    case SS_UNENCODABLE: id = S_ED_ST_ENCODING; break;
    case SS_UNKNOWN: id = S_ED_ST_UNKNOWN; break;
    case SS_OFF: id = S_ED_ST_OFF; break;
    default: id = S_ED_ST_FAILED; break;
    }
    std::wstring t = Tr(id);
    if (st == SS_FAILED && !s.keptRecovery.empty()) t += L". " + Format(S_ED_ST_RECOVERY_FMT, s.keptRecovery.c_str());
    return t;
}

bool EditCanUndo() { return g.editing && s.undo.Depth() > 0; }
bool EditCanRedo() { return g.editing && s.undo.RedoDepth() > 0; }

int EditStyleId() {
    if (!g.editing || s.focus.block < 0 || (size_t)s.focus.block >= g.doc.blocks.size()) return 0;
    if (InPhantom()) return 0;  // a phantom row is a new paragraph, whatever block it stands next to (§6.7)
    if (s.st.atom >= 0) return 9;
    const Block& b = g.doc.blocks[s.focus.block];
    const BlockSrc* bs = g.doc.blockSrc.size() == g.doc.blocks.size() ? &g.doc.blockSrc[s.focus.block] : nullptr;
    if (bs && (bs->flags & BS_FOOTNOTE)) return 10;
    if (bs && (bs->flags & BS_RAWTEXT)) return 11;  // raw-while-typing (§6.9)
    if (b.kind == BK_TABLE) return 8;
    if (b.kind == BK_CODE) return 7;
    if (b.kind == BK_TEXT && b.heading) return b.heading;
    return 0;
}

std::wstring EditStripText(int kind) {
    switch (kind) {
    case STRIP_CONFLICT: return Format(S_ED_CONFLICT_FMT, SizeText(s.conflictDisk).c_str(), SizeText(s.conflictOurs).c_str());
    case STRIP_ENCODING:
        if (s.lastReason == "BOM_LOOKALIKE") return Tr(S_ED_BOM_LOOKALIKE);
        return Format(S_ED_ENCODING_FMT, BadCharText().c_str(), CodePageName(g.disk.cp).c_str());
    case STRIP_LEAVE: return Format(S_ED_LEAVE_FMT, WhyText(s.leaveWhy));
    case STRIP_RECOVERY:
        if (!s.recovery.empty()) return Tr(EditRecoveryRestorable() ? S_ED_RECOVERY_INTERRUPTED : S_ED_RECOVERY_CHANGED);
        if (!s.journals.empty()) {
            FILETIME ft, lt;
            ft.dwLowDateTime = (DWORD)s.journals[0].time;
            ft.dwHighDateTime = (DWORD)(s.journals[0].time >> 32);
            SYSTEMTIME t{};
            FileTimeToLocalFileTime(&ft, &lt);
            FileTimeToSystemTime(&lt, &t);
            wchar_t when[64];
            swprintf_s(when, L"%02d.%02d.%04d %02d:%02d", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute);
            return Format(S_ED_JOURNAL_FMT, when);
        }
        return L"";
    default: return L"";
    }
}

// the external version "Overwrite" kept aside lives until the document is closed (§10.5, D22: nothing lingers)
static void DropTheirs() {
    if (s.theirs.empty()) return;
    DeleteFileW(s.theirs.c_str());
    s.theirs.clear();
}

void EditOnLoad() {
    s.undo.Clear();  // a new document session (§11)
    s.saveState = SS_SAVED;
    s.recovery.clear();
    s.journals.clear();
    s.pending = RecoveryInfo();  // (EditLeaveDocument has flushed its file already)
    s.toasted = 0;
    s.retryStep = 0;
    s.keptRecovery.clear();
    s.lastBad = UINT32_MAX;
    s.lastReason.clear();
    s.st = EditState();
    s.focus = s.anchor = TextPos{0, -1, -1};
    s.trail = 0;
    s.journalFile.clear();
    DropTheirs();  // the overwritten disk version was kept for this document only
    StripHide(STRIP_RECOVERY);
    StripHide(STRIP_OTHER_WINDOW);
    StripHideEditing();
}

// A flush point (§10.3 step 7): leaving the document for another one or a reload, and the window closing. The recovery
// file that stood for the last flush while saves went unflushed goes once the file is flushed; a flush that fails
// leaves it, and the next open finds it.
void EditLeaveDocument() {
    WaitJob();
    if (s.pending.file.empty()) return;
    if (!RecoveryFlushPending(s.pending, g.path.c_str())) DebugLog("flush point: the flush failed, the recovery file stays");
    s.pending = RecoveryInfo();
}

// ------------------------------------------------------------------------------------------------ recovery (§10.5, §10.6)
// After the first frame of an open: a recovery file left for this file means a save was interrupted - unless the file
// shows the save went through or never began, and then it is deleted without a word. With none, a journal of unsaved
// edits of a window that is gone is offered the same way.
void EditAfterOpen() {
    s.recovery.clear();
    s.journals.clear();
    if (!g.path.empty() && !g.loadFailed && !BenchActive()) {
        s.recovery = FindRecovery(RecoveryDir(), g.disk.volume, g.disk.index, g.path);
        if (!s.recovery.empty()) ClassifyLeftovers();
        if (s.recovery.empty()) s.journals = FindJournals(RecoveryDir(), g.disk.volume, g.disk.index, g.path);
    }
    if (s.recovery.empty() && s.journals.empty()) StripHide(STRIP_RECOVERY);
    else StripShow(STRIP_RECOVERY);
}

// The text the file holds now: the baseline once edit mode took one, else what was loaded (reading mode is never dirty).
// A journal is restored only onto the text it was written against (§10.6) - asked at the moment it matters, since a
// save, an adoption or another journal dealt with meanwhile changes the answer. The strip asks at every frame, so the
// hash is kept while what it was taken of stays the same (exact: fresh when acting on it).
static bool JournalFits(const JournalInfo& j, bool exact) {
    const std::wstring& t = g.disk.valid ? g.disk.text : g.src;
    const uint64_t key[4] = {g.docSerial, g.disk.valid ? g.disk.hash + 1 : 0, t.size(), (uint64_t)(uintptr_t)t.data()};
    if (exact || memcmp(key, s.textHashKey, sizeof key)) {
        s.textHash = TextHash(t);
        memcpy(s.textHashKey, key, sizeof key);
    }
    return j.diskHash == s.textHash;
}

bool EditRecoveryRestorable() {
    if (!s.recovery.empty()) return s.recovery.back().verdict == RV_TORN;
    return !s.journals.empty() && JournalFits(s.journals[0], false);
}

static void RecoveryDone() {
    if (!s.recovery.empty()) s.recovery.pop_back();
    else if (!s.journals.empty()) s.journals.erase(s.journals.begin());
    if (s.recovery.empty() && s.journals.empty()) StripHide(STRIP_RECOVERY);
    ForceFullRedraw();
    Invalidate();
}

namespace {
void ReplaceAll(const std::wstring& text, EditKind kind);
}

// a copy of a lost version, written beside it in %TEMP%\FastMD and opened in a window of its own
static void OpenCopy(const std::wstring& out) {
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
}
// A name of its own: a copy made earlier may be open in another window, edited and saved there, and is never written
// over - the next one is "… (2)", "… (3)" (and the file is created only where none is, CREATE_NEW).
static std::wstring TempCopyName(StrId suffix) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"FastMD\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring name = FileNameOf(g.path);
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dir + name.substr(0, dot) + Tr(suffix), ext = dot == std::wstring::npos ? L".md" : name.substr(dot);
    for (int n = 1; n < 100; n++) {
        std::wstring f = stem + (n > 1 ? L" (" + std::to_wstring(n) + L")" : L"") + ext;
        if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) return f;
    }
    return std::wstring();
}

static void RecoveryCommand(UINT id) {
    if (!s.recovery.empty()) {
        const RecoveryInfo& r = s.recovery.back();  // the latest interrupted save
        switch (id) {
        case CMD_RECOVERY_OPEN: {  // the file as it was before that save
            std::wstring out = TempCopyName(S_ED_RECOVERED);
            if (out.empty() || !RecoveryRebuild(r, g.path.c_str(), out.c_str())) { ShowToast(Tr(S_ED_RECOVERY_FAILED), 3000); break; }
            OpenCopy(out);
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
        return;
    }
    if (s.journals.empty()) return;
    JournalInfo j = s.journals[0];  // the newest journal of a window that is gone
    switch (id) {
    case CMD_RECOVERY_OPEN: {  // its text as a document of its own
        std::wstring out = TempCopyName(S_ED_JOURNAL_COPY);
        std::string bytes = "\xEF\xBB\xBF", body;
        size_t bad;
        const char* why;
        EncodeText(CP_UTF8, j.text.data(), j.text.size(), body, &bad, &why);
        DWORD e = 0;
        if (out.empty() || !WriteNewFile(out.c_str(), bytes + body, &e, false)) { ShowToast(Tr(S_ED_RECOVERY_FAILED), 3000); break; }
        OpenCopy(out);
        break;
    }
    case CMD_RECOVERY_RESTORE:  // into edit mode, the journal's text as one ADOPT step (§10.6)
        // Checked now, and again once edit mode has read the file: the journal fits only the text it was written against
        if (!JournalFits(j, true) || !EditEnter(ENTER_CARET) || !JournalFits(j, true)) {
            ShowToast(Tr(S_ED_RECOVERY_FAILED), 3000);
            ForceFullRedraw();  // (the strip loses its Restore button)
            Invalidate();
            break;
        }
        // the minimal differing middle as one step, never cutting a pair or a CRLF, the file's own text untouched
        ReplaceAll(j.text, EK_ADOPT);
        RevealCaret();
        DeleteFileW(j.file.c_str());
        RecoveryDone();
        ArmAutosave();
        ArmJournal();
        StatusTick();
        break;
    case CMD_RECOVERY_DELETE:
        DeleteFileW(j.file.c_str());
        RecoveryDone();
        break;
    }
}

// ------------------------------------------------------------------------------------------------ changes (§7, §11)
namespace {
// after every change of the source in edit mode: the title, autosave, the journal, the first-edit toast
void AfterChange() {
    s.splicesSinceEntry++;
    if (!EditDirty()) StripHide(STRIP_LEAVE);  // a strip closes itself when its condition ends
    ArmAutosave();
    ArmJournal();
    if (g.hwnd) SetTimer(g.hwnd, TIMER_EDIT_IDLE, 1000, nullptr);
    if (!g_autosaveHinted && g.cfg.autosave && !Paused(s.saveState)) {
        g_autosaveHinted = true;
        ShowToast(Tr(S_ED_HINT_AUTOSAVE), 4000);
    }
    StatusTick();
}

// the range of the source a set of splices touched, for the swap (its picture carry-over)
void SpliceRange(const std::vector<Splice>& sps, uint32_t* at, uint32_t* oldLen, uint32_t* newLen) {
    if (sps.size() == 1) {
        *at = sps[0].at;
        *oldLen = (uint32_t)sps[0].removed.size();
        *newLen = (uint32_t)sps[0].inserted.size();
        return;
    }
    uint32_t lo = UINT32_MAX, hi = 0;
    int64_t delta = 0;
    for (const Splice& sp : sps) {
        lo = std::min(lo, sp.at);
        hi = std::max(hi, sp.at + (uint32_t)sp.removed.size());
        delta += (int64_t)sp.inserted.size() - (int64_t)sp.removed.size();
    }
    if (lo == UINT32_MAX) lo = 0;
    *at = lo;
    *oldLen = hi - lo;
    *newLen = (uint32_t)std::max<int64_t>(0, (int64_t)(hi - lo) + delta);
}

void Sanitize(std::wstring& text) {  // what EditSplice writes for a lone surrogate, so the history holds the same
    for (size_t i = 0; i < text.size(); i++) {
        bool pair = i + 1 < text.size() && text[i] >= 0xD800 && text[i] <= 0xDBFF && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF;
        if (pair) i++;
        else if (text[i] >= 0xD800 && text[i] <= 0xDFFF) text[i] = 0xFFFD;
    }
}

bool ApplyRaw(const std::vector<Splice>& sps) {  // all or nothing, through the splice primitive
    size_t k = 0;
    for (; k < sps.size(); k++) {
        const Splice& sp = sps[k];
        if (sp.at > g.src.size() || g.src.compare(sp.at, sp.removed.size(), sp.removed) != 0 ||
            !EditSplice(sp.at, (uint32_t)sp.removed.size(), sp.inserted))
            break;
    }
    if (k == sps.size()) return true;
    while (k-- > 0) g.src.replace(sps[k].at, sps[k].inserted.size(), sps[k].removed);
    return false;
}
void Revert(const std::vector<Splice>& sps) {
    for (size_t k = sps.size(); k-- > 0;) g.src.replace(sps[k].at, sps[k].inserted.size(), sps[k].removed);
}

// ---- raw-while-typing (§6.9)
uint32_t SrcLineStart(uint32_t s) {
    const std::wstring& t = g.src;
    s = std::min<uint32_t>(s, (uint32_t)t.size());
    while (s > 0 && t[s - 1] != L'\n' && t[s - 1] != L'\r') s--;
    return s;
}
// Typing turned the caret's text block into an object - an HTML block, a picture or formula paragraph - mid-word: it is
// kept as text while the caret stays there, at the cost of one more parse on this keystroke only (F22). The swap had
// to move the typed caret `want` - onto the new object, or, when the HTML block shows nothing (`<!--`, or md4c's `<!`
// at a line's end) and swallows the rest of the document, into the block before - so it is put back.
void RawDetect(bool wasText, uint32_t want) {
    if (!wasText || !g.editing) return;
    const int32_t b = s.focus.block;
    const bool known = b >= 0 && (size_t)b < g.doc.blockSrc.size(), atom = known && IsAtomBlock(g.doc, b);
    const bool hidden = !atom && s.st.focus != want &&
                        (!known || want < g.doc.blockSrc[b].line || want > g.doc.blockSrc[b].outerEnd);
    if (!atom && !hidden) return;
    if (hidden || (g.doc.blockSrc[b].flags & BS_HTML)) {  // the line's first `<` is hidden from the parser: it reads as text
        uint32_t k = SrcLineStart(want);
        while (k < want && g.src[k] != L'<') k++;
        if (k >= want) return;
        s.masks.push_back(k);
    } else if (g.doc.blocks[b].kind == BK_IMAGE) {  // shown as its source until the caret leaves it
        const BlockSrc& bs = g.doc.blockSrc[b];
        s.raw.push_back({bs.line, bs.outerEnd});
    } else {
        return;
    }
    s.st.focus = s.st.anchor = want;
    s.st.atom = -1;
    EditReparse(UINT32_MAX, 0, 0);
    UpdateCaretVisible();
    RevealCaret();
}
// An override ends when the caret leaves what it holds - the masked line (a `<!--` nothing closes stays masked, or the
// rest of the document would vanish into it), the raw paragraph - and the model is parsed as it is.
void RawLifetime() {
    if (s.masks.empty() && s.raw.empty()) return;
    const std::wstring& t = g.src;
    const uint32_t f = std::min<uint32_t>(s.st.focus, (uint32_t)t.size()), fl = SrcLineStart(f);
    bool keep = false;
    for (uint32_t m : s.masks) {
        if (m >= t.size() || t[m] != L'<') continue;  // the `<` itself went
        keep |= SrcLineStart(m) == fl || (!t.compare(m, 4, L"<!--") && t.find(L"-->", m + 4) == std::wstring::npos);
    }
    for (const auto& k : s.raw) keep |= f >= k.first && f <= k.second;
    if (keep) return;
    s.masks.clear();
    s.raw.clear();
    EditReparse(UINT32_MAX, 0, 0);
    UpdateCaretVisible();
}

// An operation's result applied: the splices, the swap, the caret, one undo step (§7, §11). typed / fallbacks: the
// check of typing (§7.3 step 5) - the rendered text must be the old one with the typed characters at the caret.
bool Apply(EditResult r, std::wstring_view typed = {}, const std::vector<TypeCandidate>* fallbacks = nullptr) {
    if (!r.refused.empty()) {
        DebugLog("edit refused: %s", r.refused.c_str());
        if (r.refused == "atom") ShowToast(Tr(S_ED_ATOM_HINT), 2500);
        else ShowToast(Tr(S_ED_REFUSED), 2000);
        return false;
    }
    for (Splice& sp : r.splices) Sanitize(sp.inserted);
    r.after.wantX = -1;  // an edit (or a caret an edit moved) starts a new column for ↑/↓, and a new visual line
    r.after.lineAff = 0;
    if (r.splices.empty()) {  // the caret alone: over a soft break, onto an atom
        s.st = r.after;
        s.dir = 1;
        ResolveCaret(s.st.atom < 0);
        UpdateCaretVisible();
        CaretMoved();
        RevealCaret();
        return true;
    }
    EditState before = s.st;
    if (!ApplyRaw(r.splices)) {
        ShowToast(Tr(S_ED_REFUSED), 2000);
        return false;
    }
    const uint32_t t0 = s.focus.t;
    const std::vector<uint32_t> masks = s.masks;
    const std::vector<std::pair<uint32_t, uint32_t>> raw = s.raw;
    for (uint32_t& m : s.masks) m = MapThrough(r.splices, m, false);  // raw-while-typing moves with the text (§6.9)
    for (auto& k : s.raw) k = {MapThrough(r.splices, k.first, false), MapThrough(r.splices, k.second, true)};
    s.st = r.after;
    s.dir = r.kind == EK_DEL_BACK ? -1 : 1;
    uint32_t at, oldLen, newLen;
    SpliceRange(r.splices, &at, &oldLen, &newLen);
    bool check = !typed.empty() && fallbacks;
    Doc old;
    EditReparse(at, oldLen, newLen, check || r.keep.on ? &old : nullptr);
    if (r.keep.on && !Kept(old, g.doc, r.keep)) {
        // §7.5 step 5: the delimiters it wrote back did not render as they must (a flank lost): taken back, refused
        Revert(r.splices);
        s.st = before;
        s.masks = masks;
        s.raw = raw;
        EditReparse(at, newLen, oldLen);
        ShowToast(Tr(S_ED_CANT_FORMAT), 3000);
        return false;
    }
    if (check && !TypedOk(old, t0, g.doc, typed)) {
        // It did not render as typed (a closer lost its flank, a formula stopped being one): the other places, in
        // order; if none renders right the first choice stays - typing is never blocked.
        const std::vector<Splice> first = r.splices;
        const EditState firstAfter = r.after;
        std::vector<Splice> cur = first;  // what the source holds now
        bool found = false;
        for (const TypeCandidate& c : *fallbacks) {
            Revert(cur);
            cur.clear();
            std::vector<Splice> alt{Splice{c.at, L"", c.text}};
            if (!ApplyRaw(alt)) continue;
            cur = alt;
            s.st.focus = s.st.anchor = c.caret;
            EditReparse(c.at, 0, (uint32_t)c.text.size());
            if (TypedOk(old, t0, g.doc, c.rendered)) {
                found = true;
                break;
            }
        }
        if (!found) {
            Revert(cur);
            ApplyRaw(first);
            cur = first;
            s.st = firstAfter;
            EditReparse(at, oldLen, newLen);
        }
        r.splices = cur;
    }
    EditStep step;
    step.splices = r.splices;
    step.before = before;
    step.before.burstBeg = UINT32_MAX;
    step.after = s.st;
    step.kind = r.kind;
    s.undo.Push(std::move(step), GetTickCount64());
    if (r.kind != EK_TYPE && r.kind != EK_DEL_BACK && r.kind != EK_DEL_FWD) s.undo.BreakCoalescing();
    UpdateCaretVisible();
    CaretRestart();
    RevealCaret();
    AfterChange();
    RawLifetime();
    return true;
}

// ---- big documents (§5.7): typing is applied at once and the model catches up every 150 ms
bool DeferredDoc() { return g.src.size() >= DeferChars(); }

// A blank typed on in a burst where the rest of the source line is blank: a trailing blank, or - before a soft break -
// the second blank of an accidental hard break (§6.6). Only the model can tell the break from the end of the block,
// unless the next line is blank or there is none: then the block ends here, and trailing blanks are harmless.
bool BlankNeedsModel(uint32_t at) {
    const std::wstring& t = g.src;
    uint32_t p = at;
    while (p < t.size() && (t[p] == L' ' || t[p] == L'\t')) p++;
    if (p < t.size() && t[p] != L'\n' && t[p] != L'\r') return false;  // text follows on the line
    if (p < t.size() && t[p] == L'\r') p++;
    if (p < t.size() && t[p] == L'\n') p++;
    while (p < t.size() && (t[p] == L' ' || t[p] == L'\t' || t[p] == L'>')) p++;  // (a blank line inside a quote)
    return p < t.size() && t[p] != L'\n' && t[p] != L'\r';
}

bool TryDeferredType(const std::wstring& text) {
    if (!DeferredDoc() || s.st.anchor != s.st.focus || s.st.atom >= 0 || !NeedsTypeCheck(text) || InPhantom() ||
        !s.masks.empty() || !s.raw.empty())
        return false;
    bool blank = false;
    for (wchar_t ch : text) {
        if (ch == L'\t') return false;
        blank |= ch == L' ';
    }
    EditState before = s.st;
    uint32_t at;
    if (s.st.burstBeg != UINT32_MAX) {  // on in a burst: at the caret, the model is stale
        at = s.st.focus;
        if (blank && BlankNeedsModel(at)) return false;
    } else {  // the first keystroke of a burst: the fresh model says where (§6.5, §6.6), and it must be a plain insertion
        if (s.focus.cell >= 0 || (s.trail && blank)) return false;
        EditResult r = OpType(Ctx(), s.st, text);
        if (!r.refused.empty() || r.splices.size() != 1 || !r.splices[0].removed.empty() || r.splices[0].inserted != text)
            return false;
        at = r.splices[0].at;
    }
    if (!EditSplice(at, 0, text)) return true;
    if (s.st.burstBeg == UINT32_MAX) {
        s.st.burstBeg = at;
        if (g.hwnd) SetTimer(g.hwnd, TIMER_EDIT_REPARSE, 150, nullptr);  // later keystrokes do not re-arm it
    }
    s.st.focus = s.st.anchor = at + (uint32_t)text.size();
    s.st.wantX = -1;
    s.st.lineAff = 0;
    EditStep step;
    step.splices.push_back(Splice{at, L"", text});
    step.before = before;
    step.before.burstBeg = UINT32_MAX;
    step.after = s.st;
    step.after.burstBeg = UINT32_MAX;
    step.kind = EK_TYPE;
    s.undo.Push(std::move(step), GetTickCount64());
    CaretRestart();
    AfterChange();
    return true;
}

bool TryDeferredBackspace() {
    if (s.st.burstBeg == UINT32_MAX || s.st.focus <= s.st.burstBeg || s.st.anchor != s.st.focus) return false;
    // one cluster (§5.7), and only one typed in this burst: the source there is what was typed, so its clusters are
    uint32_t at = GraphemeLite(g.src, s.st.focus, -1, nullptr);
    if (at < s.st.burstBeg || at >= s.st.focus) return false;
    std::wstring removed = g.src.substr(at, s.st.focus - at);
    EditState before = s.st;
    if (!EditSplice(at, (uint32_t)removed.size(), L"")) return true;
    s.st.focus = s.st.anchor = at;
    s.st.wantX = -1;
    EditStep step;
    step.splices.push_back(Splice{at, removed, L""});
    step.before = before;
    step.before.burstBeg = UINT32_MAX;
    step.after = s.st;
    step.after.burstBeg = UINT32_MAX;
    step.kind = EK_DEL_BACK;
    s.undo.Push(std::move(step), GetTickCount64());
    CaretRestart();
    AfterChange();
    return true;
}

void Type(std::wstring text) {
    if (s.st.atom >= 0) {  // never into the paragraph beside a selected atom (§2.10)
        ShowToast(Tr(S_ED_ATOM_HINT), 2500);
        return;
    }
    if (g.editModal > 0) return;
    Sanitize(text);  // (a lone low surrogate): the history must hold what the source gets, or undo finds a mismatch
    if (TryDeferredType(text)) return;
    EditSync();
    EditCtx c = Ctx();
    EditResult r = OpType(c, s.st, text);
    // The check of §7.3 step 5, but not for blanks (OpType put them where they belong, and one at a line's end shows as
    // nothing) nor after trailing blanks (they show once text follows): both would always "fail", at two swaps each.
    bool blanks = std::all_of(text.begin(), text.end(), [](wchar_t ch) { return ch == L' ' || ch == L'\t'; });
    bool check = !blanks && !s.trail && NeedsTypeCheck(text) && r.splices.size() == 1 && r.splices[0].removed.empty() &&
                 s.st.anchor == s.st.focus && !InPhantom();
    std::vector<TypeCandidate> fb;
    if (check) fb = TypeFallbacks(c, s.st, r.splices[0].at, text);
    bool wasText = s.focus.block >= 0 && !IsAtomBlock(g.doc, s.focus.block);
    const uint32_t want = r.after.focus;  // (where the typing puts the caret, before the swap may have to move it)
    if (Apply(std::move(r), check ? std::wstring_view(text) : std::wstring_view(), check ? &fb : nullptr))
        RawDetect(wasText, want);
}

void Backspace(bool word) {
    if (g.editModal > 0) return;
    if (!word && TryDeferredBackspace()) return;
    EditSync();
    Apply(OpBackspace(Ctx(), s.st, word));
}

void DeleteKey(bool word) {
    if (g.editModal > 0) return;
    EditSync();
    Apply(OpDelete(Ctx(), s.st, word));
}

// Enter (0), Shift+Enter (1), Ctrl+Enter (2): §7.6
void EnterKey(int variant) {
    if (g.editModal > 0) return;
    EditSync();
    Apply(OpEnter(Ctx(), s.st, variant));
}

void TabKey(bool shift) {  // §7.8
    if (g.editModal > 0) return;
    EditSync();
    Apply(OpTab(Ctx(), s.st, shift));
}

// Cut (§7.11): copied as Ctrl+C copies it, then the selection goes, as one step
void Cut() {
    if (g.editModal > 0 || !HasSelection()) return;
    EditSync();
    if (!CopySelectionRich()) return;  // (the clipboard stayed busy: the text is not taken away without a copy)
    EditResult r = OpDeleteSelection(Ctx(), s.st);
    r.kind = EK_CUT;
    Apply(std::move(r));
}

// Paste (§7.11): the clipboard's text, taken as Markdown source (FastMD's own format and files are Phase 3b's); a
// picture alone cannot be pasted yet, and a toast says what works instead
void Paste() {
    if (g.editModal > 0) return;
    EditSync();
    std::wstring text;
    bool picture = false;
    if (OpenClipboardRetry()) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
                text.assign(p, wcsnlen(p, GlobalSize(h) / sizeof(wchar_t)));
                GlobalUnlock(h);
            }
        } else {
            picture = IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP);
        }
        CloseClipboard();
    }
    if (!text.empty()) Apply(OpPaste(Ctx(), s.st, text, false));
    else if (picture) ShowToast(Tr(S_ED_PASTE_PICTURE), 4000);
}

// ---- undo and redo (§11): verified - a history that no longer fits the text is dropped, never applied
void UndoRedo(bool redo) {
    if (g.editModal > 0 || g.fullPending) return;
    EditSync();
    const EditStep* st = redo ? s.undo.PeekRedo() : s.undo.PeekUndo();
    if (!st) return;
    std::string why;
    if (!ApplySplices(g.src, st->splices, !redo, &why)) {
        DebugLog("undo: %s", why.c_str());
        s.undo.Clear();
        ShowToast(Tr(S_ED_UNDO_RESET), 3000);
        BarChanged();
        return;
    }
    uint32_t at, oldLen, newLen;
    SpliceRange(st->splices, &at, &oldLen, &newLen);
    if (!redo) std::swap(oldLen, newLen);
    s.st = redo ? st->after : st->before;
    s.st.atom = -1;
    s.st.burstBeg = UINT32_MAX;
    s.st.pendOn = s.st.pendOff = 0;
    s.st.wantX = -1;
    s.st.lineAff = 0;
    s.dir = 1;
    s.masks.clear();  // raw-while-typing is about the text being typed, not the text a step brings back
    s.raw.clear();
    if (redo) s.undo.DidRedo();
    else s.undo.DidUndo();
    EditReparse(at, oldLen, newLen);
    UpdateCaretVisible();
    CaretRestart();
    RevealCaret();
    s.splicesSinceEntry++;
    if (!EditDirty()) StripHide(STRIP_LEAVE);
    ArmAutosave();
    ArmJournal();
    StatusTick();
}

// the source text in [0, n) replaced by `text` as one step of the given kind (adoption, discard): the minimal middle
// that differs is the splice, so undo visibly brings the other version back (UX-17)
void ReplaceAll(const std::wstring& text, EditKind kind) {
    const std::wstring& a = g.src;
    size_t p = 0, n = std::min(a.size(), text.size());
    while (p < n && a[p] == text[p]) p++;
    size_t q = 0;
    while (q < n - p && a[a.size() - 1 - q] == text[text.size() - 1 - q]) q++;
    // never inside a CRLF or a surrogate pair
    auto cut = [](const std::wstring& t, size_t i) {
        return i > 0 && i < t.size() && ((t[i - 1] == L'\r' && t[i] == L'\n') ||
                                         (t[i - 1] >= 0xD800 && t[i - 1] <= 0xDBFF && t[i] >= 0xDC00 && t[i] <= 0xDFFF));
    };
    while (p > 0 && (cut(a, p) || cut(text, p))) p--;
    while (q > 0 && (cut(a, a.size() - q) || cut(text, text.size() - q))) q--;
    std::wstring removed = a.substr(p, a.size() - q - p), inserted = text.substr(p, text.size() - q - p);
    if (removed.empty() && inserted.empty()) return;
    EditState before = s.st;
    g.src.replace(p, removed.size(), inserted);  // not through EditSplice: the file's own text, lone surrogates and all
    EditStep step;
    step.splices.push_back(Splice{(uint32_t)p, removed, inserted});
    step.before = before;
    // the caret keeps its place by source offset, clamped (§10.7)
    auto shift = [&](uint32_t o) -> uint32_t {
        if (o <= p) return o;
        if (o >= p + removed.size()) return (uint32_t)(o - removed.size() + inserted.size());
        return (uint32_t)(p + inserted.size());
    };
    s.st.focus = shift(s.st.focus);
    s.st.anchor = shift(s.st.anchor);
    s.st.atom = -1;
    s.st.burstBeg = UINT32_MAX;
    s.st.wantX = -1;
    s.st.lineAff = 0;
    s.st.phantom = Phantom{};  // another text: what the editor showed beside the old one goes
    s.masks.clear();
    s.raw.clear();
    step.after = s.st;
    step.kind = kind;
    s.undo.Push(std::move(step), GetTickCount64());
    s.undo.BreakCoalescing();
    if (g.editing) EditReparse((uint32_t)p, (uint32_t)removed.size(), (uint32_t)inserted.size());
}
}  // namespace

void EditSync() {
    if (s.st.burstBeg == UINT32_MAX) return;
    if (g.hwnd) KillTimer(g.hwnd, TIMER_EDIT_REPARSE);
    uint32_t beg = s.st.burstBeg, len = s.st.focus >= beg ? s.st.focus - beg : 0;
    s.st.burstBeg = UINT32_MAX;
    s.dir = 1;
    EditReparse(beg, 0, len);
    UpdateCaretVisible();
}

// ------------------------------------------------------------------------------------------------ entering (§2.1)
namespace {
// the bar slides in or out in 150 ms, the scroll position absorbing it (§12.1)
void SetBarT(float t) {
    g.barT = std::clamp(t, 0.f, 1.f);
    if (!g.doc.blocks.empty() && g.Y.size() == g.doc.blocks.size()) RecomputeY();
    float comp = std::min(EditInset(), s.maxComp), d = comp - g.barComp;
    g.barComp = comp;
    g.scrollY = std::clamp(g.scrollY + d, 0.f, MaxScroll());
    g.targetY = std::clamp(g.targetY + d, 0.f, MaxScroll());
    BarChanged();
}

void SlideDone() {
    g.barSliding = false;
    FindRelayoutInput();  // the find box shows again, under the bar
    if (g.editing) RevealCaret();
    BarChanged();
}

void StartSlide(bool in, bool instant) {
    s.slideFrom = g.barT;
    s.slideTo = in ? 1.f : 0.f;
    s.slideT0 = Qpc();
    s.maxComp = in ? std::min(44.f / g.cfg.zoom, g.scrollY) : g.barComp;
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    if (instant || !g.cfg.smoothScroll || !anim) {
        SetBarT(s.slideTo);
        SlideDone();
        return;
    }
    g.barSliding = true;
    FindRelayoutInput();  // hidden while the bar moves (R18)
}

void ShowHint() {  // the first entry ever, per profile (§2.1)
    s.hintPending = false;
    RegSetDword(L"EditHintShown", 1);
    ShowToast(Tr(S_ED_HINT_FIRST), 4000);
}

bool ReadForEntry(DiskState& d, DiskRefusal* dr) {
    std::string bytes;
    DWORD e = 0;
    SaveState rs = ReadDisk(g.path.c_str(), bytes, &d, &e);
    if (rs != SS_SAVED) {
        ShowToast(Format(S_ED_NO_READ_FMT, WhyText(rs)), 3000);
        return false;
    }
    *dr = DecodeDisk(bytes, AnsiCodePage(), d);
    return true;
}

void ReleaseMutex_() {
    if (s.mutex) {
        CloseHandle(s.mutex);
        s.mutex = nullptr;
    }
}

// where the reading-mode caret is, by block and offset: the map parse has the same blocks (only code blocks' text can
// differ at their end, F4)
struct ReadingPos { int32_t block = -1, cell = -1; uint32_t off = 0; };
ReadingPos ReadingCaret() {
    ReadingPos r;
    if (g.doc.blocks.empty()) return r;
    uint32_t pos = g.selFocus;
    uint32_t b = BlockOfPos(pos);
    const Block& bl = g.doc.blocks[b];
    r.block = (int32_t)b;
    r.off = pos >= bl.textOff ? pos - bl.textOff : 0;
    if (bl.kind == BK_TABLE && bl.aux < g.doc.tables.size()) {
        const Table& tb = g.doc.tables[bl.aux];
        for (uint32_t c = 0; c < tb.rows * tb.cols; c++) {
            const Cell& cell = g.doc.cells[tb.cellOff + c];
            if (pos >= cell.textOff && pos <= cell.textOff + cell.textLen) {
                r.cell = (int32_t)c;
                r.off = pos - cell.textOff;
                break;
            }
        }
    }
    return r;
}
TextPos FromReading(const ReadingPos& r) {
    if (r.block < 0 || (size_t)r.block >= g.doc.blocks.size()) return DocFirst();
    const Block& bl = g.doc.blocks[r.block];
    if (r.cell >= 0 && CellCount(r.block) > r.cell) {
        const Cell& c = g.doc.cells[g.doc.tables[bl.aux].cellOff + r.cell];
        return TextPos{c.textOff + std::min(r.off, c.textLen), r.block, r.cell};
    }
    return TextPos{bl.textOff + std::min(r.off, bl.textLen), r.block, -1};
}

// a caret (or an object atom) at a text position
void PlaceAt(TextPos p, bool keepAnchor) {
    p = SnapStop(p, 1);
    if (p.block < 0) return;
    if (IsAtomBlock(g.doc, p.block) && !keepAnchor) {
        s.st.atom = AtomOfBlock(g.doc, p.block);
        p = AtomStop(p.block);
    } else {
        s.st.atom = -1;
    }
    uint32_t src = SrcOfText(g.doc, g.src, p, MAP_CARET);
    if (src == UINT32_MAX) return;
    s.st.focus = src;
    s.focus = p;
    s.trail = 0;
    if (!keepAnchor) {
        s.st.anchor = src;
        s.anchor = p;
    }
    Publish();
}

// the object atom under a point, if the press was on one: a picture or formula in a line (-1 = none)
int32_t InlineAtomAt(const DocHit& h) {
    if (!h.inside || h.block < 0 || h.under == UINT32_MAX || h.under >= g.doc.text.size() || g.doc.text[h.under] != 0xFFFC)
        return -1;
    const Block& b = g.doc.blocks[h.block];
    auto find = [&](uint32_t off, uint32_t n) -> int32_t {
        for (uint32_t k = 0; k < n; k++) {
            const Run& r = g.doc.runs[off + k];
            if ((r.flags & F_IMAGE) && r.start == h.under) return (int32_t)r.image;
        }
        return -1;
    };
    int32_t im = find(b.runOff, b.runCount);
    if (im < 0 && h.cell >= 0 && CellCount(h.block) > h.cell) {
        const Cell& c = g.doc.cells[g.doc.tables[b.aux].cellOff + h.cell];
        im = find(c.runOff, c.runCount);
    }
    return im;
}
void SelectInlineAtom(int32_t image, const DocHit& h) {
    TextPos p{h.under, h.block, h.cell};
    uint32_t src = SrcOfText(g.doc, g.src, p, MAP_OUTER_START);
    if (src == UINT32_MAX) return;
    s.st.focus = s.st.anchor = src;
    s.st.atom = image;
    s.focus = s.anchor = p;
    s.trail = 0;
    Publish();
}
}  // namespace

bool EditEnter(EnterHow how, float x, float y) {
    if (g.editing) return true;
    if (g.path.empty() || g.editModal > 0 || !g.ready || g.firstFrame || BenchActive()) return false;
    if (g.loadFailed) {
        ShowToast(Tr(S_ED_NO_LOAD), 3000);
        return false;
    }
    if (g.fullPending) {  // entered by itself once the full parse arrives (within 2 s)
        ShowToast(Tr(S_ED_LOADING), 3000);
        s.entryPending = true;
        s.entryAskedAt = GetTickCount();
        s.entryHow = how;
        s.entryX = x;
        s.entryY = y;
        return false;
    }
    s.entryPending = false;
    if (DataDir().empty()) {
        ShowToast(Tr(S_ED_NO_DATADIR), 3000);
        return false;
    }
    // An interrupted save of this file waits on the strip: the file may be torn, and a torn file is no baseline - the
    // edits would go on top of it, and a Restore after them would meet them as a conflict (§10.5). The reader decides
    // what to do with it first.
    if (!s.recovery.empty()) {
        ShowToast(Tr(S_ED_RECOVERY_FIRST), 3000);
        return false;
    }
    // §10.1, in this order: readable, not binary, not UTF-16 BE, UTF-16 of even length, not a stateful code page; the
    // text on screen (else it is read again, once); written back byte for byte
    DiskState d;
    DiskRefusal dr = DR_OK;
    for (int round = 0;; round++) {
        if (!ReadForEntry(d, &dr)) return false;
        StrId why = S_COUNT;
        switch (dr) {
        case DR_BINARY: why = S_ED_BINARY; break;
        case DR_UTF16BE: why = S_ED_UTF16BE; break;
        case DR_UTF16ODD: why = S_ED_UTF16ODD; break;
        case DR_STATEFUL: why = S_ED_LOSSY; break;
        default: break;
        }
        if (why != S_COUNT) {
            ShowToast(Tr(why), 3000);
            return false;
        }
        if (d.text == g.src) break;
        if (round) {  // changed again meanwhile
            ShowToast(Format(S_ED_NO_READ_FMT, Tr(S_ED_WHY_UNKNOWN)), 3000);
            return false;
        }
        ReloadDocument();  // the file is not what the window shows: read it again, and check it all once more
        if (g.loadFailed || g.fullPending) return EditEnter(how, x, y);
    }
    if (dr != DR_OK) {
        ShowToast(Tr(S_ED_LOSSY), 3000);
        return false;
    }
    // another FastMD window edits the same file (D18)
    HANDLE m = CreateMutexW(nullptr, FALSE, EditMutexName(d.volume, d.index).c_str());
    if (!m || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (m) CloseHandle(m);
        StripShow(STRIP_OTHER_WINDOW);
        return false;
    }
    ReleaseMutex_();
    s.mutex = m;
    StripHide(STRIP_OTHER_WINDOW);
    d.valid = true;
    g.disk = std::move(d);
    g.eol = DiskEol(g.disk);
    // the write probe (D17): a read-only file enters and says so at once, and is never autosaved
    DWORD pe = 0;
    SaveState probe = WriteProbe(g.path.c_str(), &pe);
    s.saveState = probe == SS_READONLY || probe == SS_DENIED ? SS_READONLY : SS_SAVED;
    PurgeRecovery(RecoveryDir(), 14, RecoveryPrefix(g.disk.volume, g.disk.index));  // (not what the strip offers)

    // the reading caret only while the reader can see it: not the one left far above by the last edit session (§2.1)
    float cx, cy, ch;
    bool haveCaret = (g.caretOn || HasSelection()) && CaretPoint(g.selFocus, &cx, &cy, &ch) && cy + ch > 0 && cy < ViewH();
    ReadingPos rp = ReadingCaret();
    s.entryReadPos = UINT32_MAX;
    uint32_t pos;
    if (how == ENTER_POINT && HitTestDoc(x, y, &pos, nullptr)) s.entryReadPos = pos;  // (a triple click's paragraph)
    TocFreeze(true);  // the outline neither docks nor undocks while editing (§12.7)
    g.editing = true;
    s.st = EditState();
    s.dir = 1;
    s.pendingHigh = 0;
    EditReparse();  // the map parse; every block that lays out the same keeps its layout
    // where the caret lands (§2.1)
    TextPos p = DocFirst();
    if (how == ENTER_POINT) {
        DocHit h;
        if (HitTestDocAt(x, y, &h) && h.block >= 0) {
            int32_t im = InlineAtomAt(h);
            if (im >= 0) {
                SelectInlineAtom(im, h);
                p.block = -2;  // placed
            } else {
                p = TextPos{h.pos, h.block, h.cell};
            }
        }
    } else if (haveCaret) {
        p = FromReading(rp);
    } else {  // the first text block whose top is visible below the bar
        float below = g.scrollY + std::min(44.f / g.cfg.zoom, g.scrollY) + 1.f;
        for (uint32_t i = FirstVisible(g.scrollY); i < g.doc.blocks.size() && g.Y[i] < g.scrollY + ViewH(); i++) {
            if (!HasStops((int32_t)i) || IsAtomBlock(g.doc, (int32_t)i)) continue;
            p = FirstStopOf((int32_t)i);
            if (g.Y[i] >= below) break;
        }
    }
    if (p.block != -2) PlaceAt(p, false);
    if (how == ENTER_POINT && s.st.atom < 0 && s.focus.block >= 0) {  // pressed right of a wrapped line: at its end
        s.st.lineAff = AffFor(s.focus, y + g.scrollY);
        Publish();
    }
    s.enteredAt = GetTickCount();
    s.enteredByDouble = how == ENTER_POINT && g.clickCount == 2;
    s.splicesSinceEntry = 0;
    g.caretOn = true;
    g.selecting = false;
    s.focused = GetFocus() == g.hwnd;
    if (s.focused && CreateCaret(g.hwnd, nullptr, 2, 16)) s.sysCaret = true;
    StartSlide(true, false);
    if (s.saveState == SS_READONLY) StripShow(STRIP_READONLY);
    CaretRestart();
    UpdateTitle(true);
    UiaDocumentChanged();
    StatusTick();
    // The first entry ever, per profile, says how to leave - once the double click that entered can no longer turn out
    // to be a triple click (which would take the entry back, UX-5)
    s.hintPending = !RegGetDword(L"EditHintShown", 0);
    if (s.hintPending && !s.enteredByDouble) ShowHint();
    else if (s.hintPending) SetTimer(g.hwnd, TIMER_EDIT_UI, GetDoubleClickTime() + 20, nullptr);
    return true;
}

void EditExit(bool silent) {
    if (!g.editing) return;
    EditSync();
    WaitJob();  // a save in flight lands now, while edit mode can still take its result - never after leaving
    for (UINT_PTR t : {TIMER_CARET, TIMER_EDIT_SAVE, TIMER_EDIT_RETRY, TIMER_EDIT_REPARSE, TIMER_EDIT_JOURNAL, TIMER_EDIT_IDLE})
        KillTimer(g.hwnd, t);
    s.saveArmed = s.retryArmed = s.journalArmed = false;
    s.hintPending = false;
    ReleaseMutex_();
    TocFreeze(false);
    // the caret by block and offset: the reading parse has the same blocks
    ReadingPos rp;
    rp.block = s.focus.block;
    rp.cell = s.focus.cell;
    uint32_t lo = 0, hi = 0;
    if (RangeOfPos(s.focus, &lo, &hi)) rp.off = s.focus.t - lo;
    g.editing = false;
    s.st.atom = -1;
    s.st.burstBeg = UINT32_MAX;
    s.st.phantom = Phantom{};  // a phantom lives only in edit mode (§6.7)
    s.masks.clear();
    s.raw.clear();
    g.phantomBlock = -1;
    g.phantomCaret = false;
    g.phantomH = 0;
    g.selAtomBlock = g.selAtomImage = -1;
    g.caretVisible = false;
    g.caretBlock = g.caretCell = -1;
    g.caretTrail = 0;
    g.caretAff = 0;
    if (s.sysCaret) {
        DestroyCaret();
        s.sysCaret = false;
    }
    StripHideEditing();
    if (!Failing(s.saveState) || !EditDirty()) s.saveState = SS_SAVED;
    if (!silent) {
        EditReparse();  // the reading parse: copy as Markdown gets its map back, code blocks their usual ends
        TextPos p = FromReading(rp);
        g.selAnchor = g.selFocus = p.t;
        g.caretOn = true;  // the caret stays where it was, collapsed (§2.2)
        StartMeasure();
    }
    StartSlide(false, silent);
    UpdateTitle(true);
    UiaDocumentChanged();
    CrashPrivacy(EditDirty());
    BarChanged();
}

bool EditLeave() {
    if (!g.editing) return true;
    SaveState st = Flush();
    if (st != SS_SAVED) {  // the edits stay, and the strip says why and what can be done (§2.2)
        s.leaveWhy = st;
        StripShow(STRIP_LEAVE);
        return false;
    }
    StripHide(STRIP_LEAVE);
    EditExit(false);
    return true;
}

void EditOnFullDoc() {
    if (!s.entryPending) return;
    if (g.editModal > 0) {  // inside a menu or a dialog nothing enters: once it is over (§10.10)
        s.fullDocPending = true;
        return;
    }
    s.entryPending = false;
    if (GetTickCount() - s.entryAskedAt <= 2000) EditEnter(s.entryHow, s.entryX, s.entryY);
}

void EditSlideStep() {
    if (!g.barSliding) return;
    float u = std::min(1.f, Micros(s.slideT0, Qpc()) / 150000.f);
    float e = 1.f - (1.f - u) * (1.f - u) * (1.f - u);  // cubic ease-out
    SetBarT(s.slideFrom + (s.slideTo - s.slideFrom) * e);
    if (u >= 1.f) SlideDone();
}

// ------------------------------------------------------------------------------------------------ leaving the document
namespace {
std::wstring SaveAsTarget() {
    s.lastPrompt = 4;
    s.prompts++;
    std::wstring preset = EnvStr(L"FASTMD_SAVE_AS");
    if (!preset.empty() || Answers().on) return preset;  // tests say where (an empty answer = cancelled)
    wchar_t file[MAX_PATH * 4] = L"";
    wcsncpy_s(file, FileNameOf(g.path).c_str(), _TRUNCATE);
    std::wstring dir = DirOf(g.path);
    std::wstring filter = std::wstring(Tr(S_FILTER_MD)) + L'\0' + L"*.md;*.markdown;*.mdown;*.mkd;*.mdx;*.txt" + L'\0' +
                          Tr(S_FILTER_ALL) + L'\0' + L"*.*" + L'\0';
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = g.hwnd;
    of.lpstrFilter = filter.c_str();
    of.lpstrFile = file;
    of.nMaxFile = (DWORD)std::size(file);
    of.lpstrDefExt = L"md";
    of.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOREADONLYRETURN;
    ModalScope modal;
    return GetSaveFileNameW(&of) ? std::wstring(file) : std::wstring();
}

bool Relative(const std::wstring& ref) {
    if (ref.empty() || ref[0] == L'#' || ref[0] == L'/' || ref[0] == L'\\') return false;
    size_t colon = ref.find(L':'), slash = ref.find_first_of(L"/\\");
    return colon == std::wstring::npos || (slash != std::wstring::npos && slash < colon);
}
int RelativeRefs() {
    int n = 0;
    for (const std::wstring& l : g.doc.links) n += Relative(l);
    for (const Image& im : g.doc.images)
        if (!im.mathKind && im.url.empty() && im.srcBeg != UINT32_MAX && im.srcEnd <= g.src.size() && im.srcEnd > im.srcBeg)
            n += Relative(g.src.substr(im.srcBeg, im.srcEnd - im.srcBeg));
    return n;
}

// Save As (§10.7, D16): the whole text in the current encoding to a new file, moved over the target; then the document
// is that file - its path, folder, watcher, identity, mutex, title, recent list.
bool SaveAs() {
    EditSync();
    std::wstring target = SaveAsTarget();
    if (target.empty()) return false;
    wchar_t full[MAX_PATH * 4];
    if (GetFullPathNameW(target.c_str(), (DWORD)std::size(full), full, nullptr)) target = full;
    // another window edits the file at that path: writing over it would leave two editors on one file (D18)
    std::string had;
    DiskState od;
    DWORD oe = 0;
    if (ReadDisk(target.c_str(), had, &od, &oe) == SS_SAVED && (od.volume != g.disk.volume || od.index != g.disk.index)) {
        if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, EditMutexName(od.volume, od.index).c_str())) {
            CloseHandle(m);
            ShowToast(Tr(S_ED_OTHER_WINDOW), 3000);
            return false;
        }
    }
    bool otherFolder = CompareStringOrdinal(DirOf(target).c_str(), -1, DirOf(g.path).c_str(), -1, TRUE) != CSTR_EQUAL;
    if (otherFolder) {
        int n = RelativeRefs();
        if (n > 0 && Ask(3, Format(S_ED_ASK_RELLINKS_FMT, n), MB_YESNO) != IDYES) return false;
    }
    std::string bytes = g.disk.header, body;
    size_t bad;
    const char* why;
    if (!EncodeText(g.disk.cp, g.src.data(), g.src.size(), body, &bad, &why)) {
        bytes = "\xEF\xBB\xBF";  // not in the file's encoding: UTF-8 then
        EncodeText(CP_UTF8, g.src.data(), g.src.size(), body, &bad, &why);
    }
    bytes += body;
    DWORD e = 0;
    WaitJob();
    if (!WriteNewFile(target.c_str(), bytes, &e)) {
        ShowToast(Format(S_ED_NO_READ_FMT, WhyText(SS_FAILED)), 3000);
        return false;
    }
    std::string back;
    DiskState d;
    if (ReadDisk(target.c_str(), back, &d, &e) != SS_SAVED || DecodeDisk(back, AnsiCodePage(), d) != DR_OK) return false;
    DeleteJournal();
    s.pending = RecoveryInfo();
    StopWatcher();
    g.path = target;
    g.doc.baseDir = DirOf(target);
    d.valid = true;
    g.disk = std::move(d);
    g.eol = DiskEol(g.disk);
    g.fileTime = g.disk.mtime;
    g.fileSize = g.disk.size;
    if (g.editing) {
        ReleaseMutex_();
        s.mutex = CreateMutexW(nullptr, FALSE, EditMutexName(g.disk.volume, g.disk.index).c_str());
    }
    DWORD pe = 0;
    SaveState probe = WriteProbe(g.path.c_str(), &pe);
    s.saveState = probe == SS_READONLY || probe == SS_DENIED ? SS_READONLY : SS_SAVED;
    for (int k : {STRIP_READONLY, STRIP_MISSING, STRIP_CONFLICT, STRIP_ENCODING, STRIP_LEAVE}) StripHide(k);
    if (s.saveState == SS_READONLY && g.editing) StripShow(STRIP_READONLY);
    EditReparse();  // relative pictures resolve anew
    StartWatcher();
    SHAddToRecentDocs(SHARD_PATHW, g.path.c_str());
    UpdateTitle(true);
    StatusTick();
    return true;
}

void Discard() {  // the edits go: the source is the disk's text again, and so is nothing left to save
    // A save in flight lands first: had the worker written the edits, reverting only the window would leave the file
    // with them and reading mode showing what it does not hold. If it did, there is nothing left to discard.
    WaitJob();
    DeleteJournal();
    if (EditDirty()) ReplaceAll(g.disk.text, EK_DISCARD);
    KillTimer(g.hwnd, TIMER_EDIT_SAVE);
    s.saveArmed = false;
    s.saveState = SS_SAVED;
    StatusTick();
}

// "Overwrite the file with my edits" (§10.7): the disk's version is kept aside, taken as the baseline (the reader
// consented), and the edits are saved over it at once.
SaveState OverwriteDisk() {
    WaitJob();
    std::string bytes;
    DiskState now;
    DWORD e = 0;
    SaveState rs = ReadDisk(g.path.c_str(), bytes, &now, &e);
    if (rs != SS_SAVED) return rs;
    std::wstring kept;
    if (WriteTheirs(RecoveryDir(), g.disk.volume, g.disk.index, bytes, &kept)) {
        if (!s.theirs.empty() && s.theirs != kept) DeleteFileW(s.theirs.c_str());
        s.theirs = kept;
    }
    UINT cp = g.disk.cp;
    std::string header = g.disk.header;
    DiskRefusal dr = DecodeDisk(bytes, g.disk.cp != CP_UTF8 && g.disk.cp != 1200 ? g.disk.cp : AnsiCodePage(), now);
    now.valid = true;
    bool whole = dr != DR_OK;  // bytes that cannot be spliced are written anew, in the file's own encoding
    if (whole) {
        now.cp = cp;
        now.header = header;
    }
    g.disk = std::move(now);
    g.fileTime = g.disk.mtime;
    g.fileSize = g.disk.size;
    s.saveState = SS_SAVED;
    StripHide(STRIP_CONFLICT);
    SaveState st = SaveWith(true, whole, 0, std::string());
    Saved(st);
    return st;
}

// "Save as UTF-8" (§10.4): the header becomes EF BB BF and the whole text is written as UTF-8, once
SaveState ConvertUtf8() {
    WaitJob();
    s.saveState = SS_SAVED;
    SaveState st = SaveWith(true, true, CP_UTF8, "\xEF\xBB\xBF");
    Saved(st);
    return st;
}
}  // namespace

bool CanLeaveDocument() {
    // Reading mode is never dirty: leaving edit mode saved or discarded (only the test hook splices without it)
    if (!g.editing) return true;
    SaveState st = Flush();
    const std::wstring name = FileNameOf(g.path);
    auto saveAsOrDiscard = [&](int kind, const std::wstring& q) -> bool {  // false = stay
        int b = Ask(kind, q, MB_YESNOCANCEL);
        if (b == IDCANCEL) return false;
        if (b == IDYES) return SaveAs();
        Discard();
        return true;
    };
    if (st != SS_SAVED) {
        switch (st) {
        case SS_MISSING: case SS_DENIED: case SS_READONLY:
            if (!saveAsOrDiscard(1, Format(S_ED_ASK_SAVE_AS_FMT, name.c_str(), WhyText(st)))) return false;
            break;
        case SS_CONFLICT: {
            int a = Ask(1, Format(S_ED_ASK_CONFLICT_FMT, name.c_str()), MB_YESNOCANCEL);
            if (a == IDCANCEL) return false;
            if (a == IDYES) {
                if (OverwriteDisk() != SS_SAVED) return false;
            } else if (!saveAsOrDiscard(2, Tr(S_ED_ASK_CONFLICT2))) {
                return false;
            }
            break;
        }
        case SS_UNENCODABLE: {
            int a = Ask(1, Format(S_ED_ASK_UTF8_FMT, BadCharText().c_str(), CodePageName(g.disk.cp).c_str()), MB_YESNOCANCEL);
            if (a == IDCANCEL) return false;
            if (a == IDYES) {
                if (ConvertUtf8() != SS_SAVED) return false;
            } else if (!saveAsOrDiscard(2, Format(S_ED_ASK_SAVE_AS_FMT, name.c_str(), WhyText(st)))) {
                return false;
            }
            break;
        }
        default: {  // busy, unknown, failed: once more, then the Save As question
            int a = Ask(1, Format(S_ED_ASK_RETRY_FMT, name.c_str(), WhyText(st)), MB_YESNOCANCEL);
            if (a == IDCANCEL) return false;
            if (a == IDYES) {
                SaveState again = SaveNow(true);
                if (again != SS_SAVED && !saveAsOrDiscard(2, Format(S_ED_ASK_SAVE_AS_FMT, name.c_str(), WhyText(again))))
                    return false;
            } else {
                Discard();
            }
            break;
        }
        }
    }
    DeleteJournal();
    if (g.editing) EditExit(true);
    return true;
}

bool EditBeforeClose() {
    if (g.editModal > 0) {  // a close inside a menu or a dialog waits for it (§10.9)
        s.closePending = true;
        return false;
    }
    if (!CanLeaveDocument()) return false;
    DropTheirs();
    return true;
}

void EditQueryEndSession() {
    if (!EditDirty()) return;
    EditSync();
    WriteJournalNow();  // first: whatever happens next, the edits are on disk somewhere
    std::wstring why = UiLanguage() == UL_RU ? L"FastMD сохраняет правки" : L"FastMD is saving edits";
    ShutdownBlockReasonCreate(g.hwnd, why.c_str());
    if (!s.job && g.src.size() >= kWorkerChars) StartJob(true);
    if (s.job) WaitJob(3000);  // the real save, waited for at most 3 s
    else Saved(EditSave(true));
    ShutdownBlockReasonDestroy(g.hwnd);
}

void EditEndSession() {
    EditSync();
    WaitJob(3000);
    if (EditDirty() && !s.job) Saved(EditSave(true));
    DropTheirs();
}

// ------------------------------------------------------------------------------------------------ external changes (§10.7)
void EditOnFileChanged() {
    FILETIME t{};
    uint64_t size = 0;
    bool there = GetFileStamp(g.path.c_str(), &t, &size);
    const bool wasMissing = s.saveState == SS_MISSING, wasUnknown = s.saveState == SS_UNKNOWN;
    // 1. the stamp we know (the load's, or the one our own write left)
    if (there && !wasMissing && !wasUnknown && size == g.fileSize && CompareFileTime(&t, &g.fileTime) == 0) {
        s.rereadMs = 0;
        return;
    }
    // 2. gone: a missing file in a folder that is there, else the folder (a share) is away
    if (!there) {
        std::wstring dir = DirOf(g.path);
        DWORD a = dir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(dir.c_str());
        bool folder = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        s.saveState = folder ? SS_MISSING : SS_UNKNOWN;
        if (folder) StripShow(STRIP_MISSING);
        ArmJournal();
        StatusTick();
        return;
    }
    WaitJob();
    std::string bytes;
    DiskState now;
    DWORD e = 0;
    SaveState rs = ReadDisk(g.path.c_str(), bytes, &now, &e);
    if (rs != SS_SAVED) {  // 3. a read that failed says nothing about the text: again, later (D5)
        s.saveState = SS_UNKNOWN;
        s.rereadMs = s.rereadMs ? std::min<DWORD>(s.rereadMs * 2, 4000) : 250;
        SetTimer(g.hwnd, TIMER_RELOAD, s.rereadMs, nullptr);
        ArmJournal();
        StatusTick();
        return;
    }
    s.rereadMs = 0;
    if (wasMissing || wasUnknown) {
        s.saveState = SS_SAVED;
        StripHide(STRIP_MISSING);
    }
    UINT acp = g.disk.cp != CP_UTF8 && g.disk.cp != 1200 ? g.disk.cp : AnsiCodePage();
    DiskRefusal dr = DecodeDisk(bytes, acp, now);
    if (now.text == g.disk.text) {
        // 4. the same text: the stamp, and - bytes of another encoding - its code page, mark and line ends (D13)
        if (dr == DR_OK && (now.hash != g.disk.hash || now.length != g.disk.length)) {
            bool ascii = now.header.empty() && std::all_of(bytes.begin(), bytes.end(), [](char c) { return (uint8_t)c < 0x80; });
            UINT keep = g.disk.cp;
            now.valid = true;
            g.disk = std::move(now);
            if (ascii && keep != 1200) g.disk.cp = keep;
            g.eol = DiskEol(g.disk);
        } else {
            g.disk.mtime = now.mtime;
            g.disk.size = now.size;
            g.disk.volume = now.volume;
            g.disk.index = now.index;
            g.disk.attributes = now.attributes;
        }
        g.fileTime = now.mtime;
        g.fileSize = now.size;
        // it is back: the edits go in now - by autosave's rule (with it off, Ctrl+S and leaving save, §2.12)
        if (EditDirty() && (wasMissing || wasUnknown) && g.cfg.autosave) Saved(SaveNow(false));
        StatusTick();
        return;
    }
    if (!EditDirty()) {
        // 5. nothing unsaved: the new version is adopted as one ADOPT step, so Ctrl+Z shows what changed (UX-17)
        if (dr != DR_OK) {  // bytes that cannot be written back as they are: reading mode takes over
            EditExit(true);
            ReloadDocument();
            ShowToast(Tr(S_ED_ADOPTED), 3000);
            return;
        }
        now.valid = true;
        g.disk = std::move(now);
        g.eol = DiskEol(g.disk);
        g.fileTime = g.disk.mtime;
        g.fileSize = g.disk.size;
        ReplaceAll(g.disk.text, EK_ADOPT);
        s.saveState = SS_SAVED;
        RevealCaret();
        ShowToast(Tr(S_ED_ADOPTED), 3000);
        StatusTick();
        return;
    }
    // 6. edits of ours and another version on disk: the reader decides (the conflict strip)
    s.conflictDisk = bytes.size();
    s.conflictOurs = OurSize();
    s.saveState = SS_CONFLICT;
    StripShow(STRIP_CONFLICT);
    KillTimer(g.hwnd, TIMER_EDIT_SAVE);
    s.saveArmed = false;
    ArmJournal();
    StatusTick();
}

void EditActivated() {
    if (!g.editing || (s.saveState != SS_READONLY && s.saveState != SS_DENIED && s.saveState != SS_MISSING)) return;
    // Windows activates the window while a message box closes, still inside its modal scope: a save there would land
    // under the question being answered (a "discard" would find the edits written). Later, then (§10.10). A share that
    // went away can take its timeout to answer: not on every Alt+Tab, and not more than once in 2 s.
    if (g.editModal > 0) {
        s.activatePending = true;
        return;
    }
    if (g.disk.remote || GetTickCount() - s.activatedAt < 2000) return;
    s.activatedAt = GetTickCount();
    if (s.saveState == SS_MISSING) {
        EditOnFileChanged();
        return;
    }
    DWORD e = 0;
    if (WriteProbe(g.path.c_str(), &e) == SS_SAVED) {  // writable again
        s.saveState = SS_SAVED;
        StripHide(STRIP_READONLY);
        if (EditDirty() && g.cfg.autosave) Saved(SaveNow(false));  // by autosave's rule (§2.12)
        StatusTick();
    }
}

void EditThemeChanged() {
    if (!g.editing) return;
    if (g.editModal > 0) {  // (a theme broadcast arrives inside menus, dialogs and print jobs too) - once it is over
        s.themePending = true;
        return;
    }
    EditSync();
    EditReparse();  // formula contexts change and re-render; nothing is read from disk
}

// ------------------------------------------------------------------------------------------------ the other window (§10.11)
LRESULT EditOwnerMessage(WPARAM vol, LPARAM index) {
    if (!g.editing || g.disk.volume != (uint32_t)vol || g.disk.index != (uint64_t)index) return 0;
    if (IsIconic(g.hwnd)) ShowWindow(g.hwnd, SW_RESTORE);
    SetForegroundWindow(g.hwnd);
    return 1;
}

static void GoToOwner() {
    AllowSetForegroundWindow(ASFW_ANY);
    UINT msg = RegisterWindowMessageW(L"FastMD.EditOwner");
    for (HWND h = FindWindowExW(nullptr, nullptr, L"FastMD.Document", nullptr); h;
         h = FindWindowExW(nullptr, h, L"FastMD.Document", nullptr)) {
        if (h == g.hwnd) continue;
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(h, msg, g.disk.volume, (LPARAM)g.disk.index, SMTO_ABORTIFHUNG, 200, &r) && r == 1) break;
    }
}

// ------------------------------------------------------------------------------------------------ the modal queue (§10.10)
bool EditDeferred(UINT msg, WPARAM wp, LPARAM lp) {
    if (g.editModal <= 0) return false;
    s.deferred.push_back(Deferred{msg, wp, lp});
    return true;
}
bool EditPendingReplay() {
    return !s.deferred.empty() || s.closePending || s.themePending || s.fullDocPending || s.activatePending;
}
void EditReplay() {
    std::vector<Deferred> q;
    q.swap(s.deferred);
    for (const Deferred& d : q) {
        if (d.msg == WM_APP_SAVED) EditOnSaved(d.wp);
        else if (d.msg == WM_APP_FILECHANGED) SetTimer(g.hwnd, TIMER_RELOAD, 120, nullptr);
    }
    if (s.themePending) {
        s.themePending = false;
        EditThemeChanged();
    }
    if (s.fullDocPending) {
        s.fullDocPending = false;
        EditOnFullDoc();
    }
    if (s.activatePending) {
        s.activatePending = false;
        EditActivated();
    }
    if (s.closePending) {
        s.closePending = false;
        PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
    }
}

void EditOnSaved(WPARAM serial) {
    if (s.job && s.job->serial == (uint32_t)serial) WaitJob();
}

// ------------------------------------------------------------------------------------------------ input (§2.7, §2.8)
namespace {
void MoveCaret(TextPos next, bool shift, int8_t aff = 0) {
    if (next.block < 0) return;
    s.st.atom = -1;
    s.st.lineAff = aff;
    s.st.phantom.in = 0;  // out of a phantom row, if the caret was in one (it lives on while the caret is in its block)
    if (!shift && IsAtomBlock(g.doc, next.block)) {  // arriving on an object block selects it (§6.2)
        s.st.atom = AtomOfBlock(g.doc, next.block);
        next = AtomStop(next.block);
    }
    uint32_t src = SrcOfText(g.doc, g.src, next, MAP_CARET);
    if (src == UINT32_MAX) return;
    s.st.focus = src;
    s.focus = next;
    s.trail = 0;
    if (!shift) {
        s.st.anchor = src;
        s.anchor = next;
    }
    Publish();
    UpdateCaretVisible();
    CaretMoved();
    RevealCaret();
}
void MoveCaretSrc(uint32_t src, bool shift) {  // along trailing blanks, in the source (§6.5)
    s.st.focus = src;
    if (!shift) s.st.anchor = src;
    s.st.atom = -1;
    s.st.lineAff = 0;
    s.st.phantom.in = 0;
    s.dir = 1;
    ResolveCaret(false);
    UpdateCaretVisible();
    CaretMoved();
    RevealCaret();
}

// ---- the phantom row (§6.7): the caret goes into it from its block's edge, and out of it to the text around
void EnterPhantom() {
    s.st.phantom.in = 1;
    s.st.focus = s.st.anchor = s.st.phantom.anchorSrc;
    s.st.atom = -1;
    s.st.wantX = -1;
    s.dir = 1;
    ResolveCaret(false);
    UpdateCaretVisible();
    CaretMoved();
    RevealCaret();
}
// a new phantom next to block b - at the document's top level, or (depth < 0) at b's own
void NewPhantomAt(int32_t b, bool before, int depth) {
    Apply(OpPhantom(Ctx(), s.st, b, before, depth));
}
bool PhantomMove(unsigned vk) {
    const Phantom& ph = s.st.phantom;
    int32_t ab = PhantomBlock(g.doc, g.src, ph);
    bool back = vk == VK_UP || vk == VK_LEFT || vk == VK_PRIOR, fwd = vk == VK_DOWN || vk == VK_RIGHT || vk == VK_NEXT;
    if (ab < 0 || (!back && !fwd)) return true;  // Home, End: it is one position
    int32_t to = ph.kind == PH_BEFORE ? (back ? NextStopBlock(ab, -1) : ab)
                                       : (back ? ab : NextStopBlock(g.phantomBlock >= 0 ? g.phantomBlock : ab, 1));
    if (to >= 0) MoveCaret(back ? LastStopOf(to) : FirstStopOf(to), false);
    return true;
}
bool InTrailingRun(uint32_t at, bool cell) {
    const std::wstring& t = g.src;
    if (at >= t.size() || (t[at] != L' ' && t[at] != L'\t')) return false;
    uint32_t p = at;
    while (p < t.size() && (t[p] == L' ' || t[p] == L'\t')) p++;
    // (in a cell the blank right before the pipe is its padding, not text: End and → stop before it, and what is typed
    // or pasted there goes into the cell - found by the paste tests of Phase 2b)
    if (cell && p < t.size() && t[p] == L'|') return p - at > 1;
    return p >= t.size() || t[p] == L'\n' || t[p] == L'\r';
}

void Move(unsigned vk, bool ctrl, bool shift) {
    EditSync();
    TextPos cur = s.focus;
    if (cur.block < 0) return;
    if (InPhantom() && PhantomMove(vk)) return;
    const bool sel = s.st.anchor != s.st.focus;
    if (s.st.atom >= 0) {  // a selected atom: the arrows leave it (§6.2)
        int32_t atom = s.st.atom;
        s.st.atom = -1;
        if (!(atom & kAtomBlock)) {  // one in a line: → after it, ← before it
            TextPos p = cur;
            if (vk == VK_RIGHT || vk == VK_END) p.t = cur.t + 1;
            if (vk == VK_LEFT || vk == VK_RIGHT) { MoveCaret(SnapStop(p, vk == VK_RIGHT ? 1 : -1), false); return; }
        } else {
            int dir = vk == VK_LEFT || vk == VK_UP || vk == VK_PRIOR || vk == VK_HOME ? -1 : 1;
            const Phantom& ph = s.st.phantom;
            if (!shift && ph.kind != PH_NONE && PhantomBlock(g.doc, g.src, ph) == cur.block && (dir > 0) == (ph.kind != PH_BEFORE)) {
                EnterPhantom();  // the phantom row beside it comes first
                return;
            }
            int32_t b = NextStopBlock(cur.block, dir);
            if (b >= 0) { MoveCaret(dir > 0 ? FirstStopOf(b) : LastStopOf(b), shift); return; }
            s.st.atom = atom;  // nowhere to go but a new paragraph at the document's edge (§6.7)
            if (!shift && vk != VK_HOME && vk != VK_END) NewPhantomAt(cur.block, dir < 0, 0);
            return;
        }
    }
    TextPos next = cur;
    bool vertical = false;
    int8_t aff = 0;
    switch (vk) {
    case VK_LEFT:
        if (sel && !shift && !ctrl) { MoveCaret(Before(s.anchor, s.focus) ? s.anchor : s.focus, false); return; }
        if (!ctrl && s.trail && s.st.focus > 0) { MoveCaretSrc(s.st.focus - 1, shift); return; }
        next = ctrl ? WordStep(cur, -1) : CharStep(cur, -1);
        break;
    case VK_RIGHT:
        if (sel && !shift && !ctrl) { MoveCaret(Before(s.anchor, s.focus) ? s.focus : s.anchor, false); return; }
        if (!ctrl && InTrailingRun(s.st.focus, cur.cell >= 0) && !HardBreakAt(cur)) {
            MoveCaretSrc(s.st.focus + 1, shift);
            return;
        }
        next = ctrl ? WordStep(cur, 1) : CharStep(cur, 1);
        break;
    case VK_UP: case VK_DOWN: {
        int dir = vk == VK_DOWN ? 1 : -1;
        if (ctrl) {  // the start of the block (then of the one before), or of the next block
            TextPos first = FirstStopOf(cur.block);
            if (dir < 0 && (Before(first, cur))) next = first;
            else {
                int32_t b = NextStopBlock(cur.block, dir);
                next = b >= 0 ? FirstStopOf(b) : cur;
            }
        } else {
            next = LineStep(cur, dir, 0, &aff);
            vertical = true;
        }
        break;
    }
    case VK_PRIOR: case VK_NEXT: {
        int dir = vk == VK_NEXT ? 1 : -1;
        float page = std::max(40.f, ViewH() - 56.f - EditRevealTop());
        next = LineStep(cur, dir, page, &aff);
        UserScrollTo(g.scrollY + dir * page, false);
        vertical = true;
        break;
    }
    case VK_HOME: next = ctrl ? DocFirst() : LineEdge(cur, -1, &aff); break;
    case VK_END:
        next = ctrl ? DocLast() : LineEdge(cur, 1, &aff);
        // after the trailing blanks of its source line (§6.5) - not into a hard break's blanks, which are the break
        if (!ctrl && !IsAtomBlock(g.doc, next.block) && !HardBreakAt(next)) {
            uint32_t src = SrcOfText(g.doc, g.src, next, MAP_CARET);
            if (src != UINT32_MAX && InTrailingRun(src, next.cell >= 0)) {
                while (src < g.src.size() && (g.src[src] == L' ' || g.src[src] == L'\t')) src++;
                if (next.cell >= 0 && src < g.src.size() && g.src[src] == L'|') src--;  // (the cell's padding stays)
                if (!vertical) s.st.wantX = -1;
                MoveCaretSrc(src, shift);
                return;
            }
        }
        break;
    default: return;
    }
    // Out of a block's edge towards a phantom row beside it: into the row (§6.7). At the document's edge next to code,
    // a table or an object, where no text could be typed: a new paragraph there (§6.8).
    const bool fwd = vk == VK_DOWN || vk == VK_RIGHT, back = vk == VK_UP || vk == VK_LEFT;
    const bool out = next.block != cur.block || (next.t == cur.t && next.cell == cur.cell);
    const Phantom& ph = s.st.phantom;
    if (!shift && !ctrl && out && (fwd || back)) {
        if (ph.kind != PH_NONE && PhantomBlock(g.doc, g.src, ph) == cur.block && fwd == (ph.kind != PH_BEFORE)) {
            EnterPhantom();
            return;
        }
        int32_t edge = NextStopBlock(cur.block, fwd ? 1 : -1);
        const Block& cb = g.doc.blocks[cur.block];
        if (edge < 0 && (cb.kind == BK_CODE || cb.kind == BK_TABLE)) {
            NewPhantomAt(cur.block, back, 0);
            return;
        }
    }
    if (!vertical) s.st.wantX = -1;
    MoveCaret(next, shift, aff);
}

void SelectAllEdit() {
    EditSync();
    TextPos a = DocFirst(), b = DocLast();
    if (a.block < 0) return;
    uint32_t sa = SrcOfText(g.doc, g.src, a, MAP_CARET), sb = SrcOfText(g.doc, g.src, b, MAP_CARET);
    if (sa == UINT32_MAX || sb == UINT32_MAX) return;
    s.st.anchor = sa;
    s.st.focus = sb;
    s.st.atom = -1;
    s.anchor = a;
    s.focus = b;
    s.trail = 0;
    Publish();
    UpdateCaretVisible();
    CaretMoved();
}

void EscChain() {
    if (g.findOpen) FindClose();
    else if (TocOverlayOpen()) TocSetOpen(false);
    else if (s.st.atom >= 0) {  // deselected, the caret after the atom
        int32_t atom = s.st.atom;
        s.st.atom = -1;
        TextPos p = s.focus;
        if (!(atom & kAtomBlock)) p.t++;
        else {
            int32_t b = NextStopBlock(p.block, 1);
            if (b >= 0) p = FirstStopOf(b);
        }
        MoveCaret(SnapStop(p, 1), false);
    } else if (EditLeave()) {
        s.leftAt = GetTickCount();
    }
}
}  // namespace

bool EditEscGuard() { return s.leftAt && GetTickCount() - s.leftAt < 1000; }

bool EditKey(unsigned vk, bool ctrl, bool shift, bool alt) {
    if (!g.editing) return false;
    if (ctrl && alt) return false;  // AltGr text reaches WM_CHAR; Ctrl+Alt+←/→ stay the column's
    if (alt) return false;          // Alt+←/→ leave the document through CanLeaveDocument
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU) return false;
    if (unsigned cmd = EditChord(vk, ctrl, shift, alt)) {
        Command(cmd);
        return true;
    }
    switch (vk) {
    case VK_ESCAPE: EscChain(); return true;
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        Move(vk, ctrl, shift);
        return true;
    case VK_BACK: Backspace(ctrl); return true;
    case VK_DELETE: DeleteKey(ctrl); return true;
    case VK_RETURN: EnterKey(shift ? 1 : 0); return true;  // (Ctrl+Enter is a chord: CMD_NEW_PARAGRAPH)
    case VK_TAB: TabKey(shift); return true;                // never the link focus of reading mode
    case VK_SPACE:   // the blank arrives as WM_CHAR, and never pages
    case VK_INSERT:
        return true;
    }
    return false;  // Ctrl+F, P, W, O, the zoom, F3 … stay what they are in reading mode
}

void EditChar(wchar_t c) {
    if (!g.editing || c < 0x20 || c == 0x7F) return;  // the keys of those arrive as WM_KEYDOWN (§12.5)
    if (c >= 0xD800 && c <= 0xDBFF) {  // the low half follows
        s.pendingHigh = c;
        return;
    }
    std::wstring text;
    if (c >= 0xDC00 && c <= 0xDFFF && s.pendingHigh) text = {s.pendingHigh, c};
    else text = std::wstring(1, c);  // a lone high one is dropped; a lone low one becomes U+FFFD (§7.1)
    s.pendingHigh = 0;
    Type(text);
}

void EditMouseDown(float x, float y, WPARAM keys, int clickCount) {
    if (!g.editing) return;
    EditSync();
    // the phantom row, or the room below the last block (more than 8 DIP under it): the caret in a new paragraph (UX-3)
    const float docY = y + g.scrollY;
    if (!(keys & MK_SHIFT) && g.phantomBlock >= 0 && docY >= g.phantomY - 4.f && docY < g.phantomY + g.phantomLine + 4.f) {
        EnterPhantom();
        return;
    }
    int32_t last = NextStopBlock((int32_t)g.doc.blocks.size(), -1);
    if (!(keys & MK_SHIFT) && last >= 0 && docY > g.Y[last] + g.H[last] + (g.phantomBlock == last ? g.phantomH : 0.f) + 8.f) {
        NewPhantomAt(last, false, 0);
        return;
    }
    DocHit h;
    if (!HitTestDocAt(x, y, &h) || h.block < 0) return;
    s.st.wantX = -1;
    // an object: a click selects it (its popup is Phase 3b's)
    if (IsAtomBlock(g.doc, h.block) && !h.above) {
        MoveCaret(AtomStop(h.block), false);
        return;
    }
    int32_t im = InlineAtomAt(h);
    if (im >= 0 && !(keys & MK_SHIFT)) {
        SelectInlineAtom(im, h);
        UpdateCaretVisible();
        CaretMoved();
        return;
    }
    TextPos p = SnapStop(TextPos{h.pos, h.block, h.cell}, 0);
    uint32_t lo, hi;
    if (clickCount == 2 && RangeOfPos(p, &lo, &hi)) {  // a word (§2.8)
        uint32_t a = p.t, b = p.t;
        WordRange(p.t, &a, &b);
        a = std::clamp(a, lo, hi);
        b = std::clamp(b, lo, hi);
        MoveCaret(SnapStop(TextPos{a, p.block, p.cell}, -1), false);
        MoveCaret(SnapStop(TextPos{b, p.block, p.cell}, 1), true);
        return;
    }
    if (clickCount == 3 && RangeOfPos(p, &lo, &hi)) {  // the block's text (a cell's in a table)
        MoveCaret(SnapStop(TextPos{lo, p.block, p.cell}, 1), false);
        MoveCaret(SnapStop(TextPos{hi, p.block, p.cell}, -1), true);
        return;
    }
    if (!(keys & MK_SHIFT) && HasSelection() && PosInSelection(p.t)) {  // a press in the selection may drag it out
        g.dragKind = DRAG_TEXT;
        g.dragPos = p.t;
        return;
    }
    MoveCaret(p, (keys & MK_SHIFT) != 0, AffFor(p, y + g.scrollY));  // (right of a wrapped line: at its end)
    g.selecting = true;
}

void EditMouseDrag(float x, float y) {
    if (!g.editing) return;
    DocHit h;
    if (!HitTestDocAt(x, y, &h) || h.block < 0) return;
    TextPos p = SnapStop(TextPos{h.pos, h.block, h.cell}, 0);
    if (IsAtomBlock(g.doc, p.block)) p = AtomStop(p.block);
    int8_t aff = AffFor(p, y + g.scrollY);
    if (p.block == s.focus.block && p.cell == s.focus.cell && p.t == s.focus.t && aff == s.st.lineAff) return;
    uint32_t src = SrcOfText(g.doc, g.src, p, MAP_CARET);
    if (src == UINT32_MAX) return;
    s.st.focus = src;
    s.st.atom = -1;
    s.st.lineAff = aff;
    s.st.phantom.in = 0;
    s.focus = p;
    s.trail = 0;
    Publish();
    UpdateCaretVisible();
    CaretRestart();
    Invalidate();
}

bool EditContextPoint(float x, float y) {
    if (!g.editing) return false;
    EditSync();
    DocHit h;
    if (!HitTestDocAt(x, y, &h) || h.block < 0) return false;
    if (HasSelection() && PosInSelection(h.pos)) return false;
    EditMouseDown(x, y, 0, 1);  // outside the selection: the caret goes there first (UX-18)
    g.selecting = false;
    return true;
}

bool EditTripleClickCancels(float, float) {
    // A third press right after the double click that entered, before any splice: that was a triple click (UX-5). Its
    // paragraph is the one the double click was on - near the top the bar has slid the page down under the pointer since.
    if (!g.editing || g.clickCount != 3 || !s.enteredByDouble || s.splicesSinceEntry ||
        GetTickCount() - s.enteredAt > GetDoubleClickTime())
        return false;
    uint32_t pos = s.entryReadPos;
    EditExit(false);  // (the reading parse: the text positions are reading mode's again)
    if (pos != UINT32_MAX && pos <= g.doc.text.size()) SelectBlockAt(pos);
    g.caretOn = false;
    Invalidate();
    return true;
}

void EditTaskClick(uint32_t block) {
    if (!g.editing) return;
    EditSync();
    for (size_t k = 0; k < g.doc.tasks.size(); k++) {
        if (g.doc.tasks[k].block != block) continue;
        EditState keep = s.st;
        EditResult r = OpTaskToggle(Ctx(), s.st, (int)k);
        r.after = keep;  // the caret stays where it was
        Apply(std::move(r));
        return;
    }
}

bool EditOpenLinkOnClick(WPARAM keys) { return !g.editing || (keys & MK_CONTROL); }

void EditFocus(bool on) {
    s.focused = on;
    if (!g.editing) return;
    if (on && !s.sysCaret && CreateCaret(g.hwnd, nullptr, 2, 16)) s.sysCaret = true;
    if (!on && s.sysCaret) {
        DestroyCaret();
        s.sysCaret = false;
    }
    if (on) CaretRestart();
    else UpdateCaretVisible();
}

std::wstring EditSelectionSource() {
    if (!g.editing || !HasSelection()) return L"";
    EditSync();
    TextPos a = Before(s.anchor, s.focus) ? s.anchor : s.focus, b = Before(s.anchor, s.focus) ? s.focus : s.anchor;
    uint32_t sa = SrcOfText(g.doc, g.src, a, MAP_OUTER_START), sb = SrcOfText(g.doc, g.src, b, MAP_OUTER_END);
    if (sa == UINT32_MAX || sb == UINT32_MAX || sb <= sa) return L"";
    std::wstring out;
    for (uint32_t i = sa; i < sb; i++) {  // the clipboard wants CRLF
        if (g.src[i] == L'\n' && (i == sa || g.src[i - 1] != L'\r')) out.push_back(L'\r');
        out.push_back(g.src[i]);
    }
    return out;
}

void EditContextMenu(int sx, int sy, bool keyboard) {
    EditSync();
    POINT cp{sx, sy};
    ScreenToClient(g.hwnd, &cp);
    float x = cp.x / Scale(), y = cp.y / Scale();
    // on the bar or a strip the menu is the same, but the text hidden under them is no place for the caret (§12.4)
    bool onChrome = y < EditInset() + g.stripH;
    if (!keyboard && !onChrome) EditContextPoint(x, y);
    int link = keyboard || onChrome ? -1 : LinkAt(x, y);
    HMENU m = CreatePopupMenu();
    bool sel = HasSelection();
    AppendMenuW(m, MF_STRING | (EditCanUndo() ? 0 : MF_GRAYED), CMD_UNDO, Tr(S_ED_MENU_UNDO));
    AppendMenuW(m, MF_STRING | (EditCanRedo() ? 0 : MF_GRAYED), CMD_REDO, Tr(S_ED_MENU_REDO));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), CMD_CUT, Tr(S_ED_MENU_CUT));
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), CMD_COPY, Tr(S_MENU_COPY));
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), CMD_COPY_MD, Tr(S_MENU_COPY_MD));
    AppendMenuW(m, MF_STRING | (IsClipboardFormatAvailable(CF_UNICODETEXT) ? 0 : MF_GRAYED), CMD_PASTE, Tr(S_ED_MENU_PASTE));
    if (link >= 0) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CMD_LINK_OPEN, Tr(S_ED_MENU_OPEN_LINK));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_SELECT_ALL, Tr(S_MENU_SELECT_ALL));
    AppendMenuW(m, MF_STRING, CMD_FIND, Tr(S_MENU_FIND));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_SAVE, Tr(S_ED_MENU_SAVE));
    AppendMenuW(m, MF_STRING, CMD_SAVE_AS, Tr(S_ED_MENU_SAVE_AS));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EDIT_EXIT, Tr(S_ED_MENU_EXIT));
    if (keyboard) {  // at the caret
        float cx, cy, ch;
        if (CaretPoint(g.selFocus, &cx, &cy, &ch)) {
            POINT p{(LONG)std::lround(cx * Scale()), (LONG)std::lround((cy + ch) * Scale())};
            ClientToScreen(g.hwnd, &p);
            sx = p.x;
            sy = p.y;
        }
    }
    g.ctxLink = link;
    UINT id;
    {
        ModalScope modal;
        id = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, g.hwnd, nullptr);
    }
    DestroyMenu(m);
    if (id) Command(id);
    g.ctxLink = -1;
}

void EditSetContextPoint(float x, float y) {
    if (y >= 0 && y < g.stripH) x = y = -1.f;  // on a reading-mode strip: "Edit here" means at the caret, not under it
    s.ctxX = x;
    s.ctxY = y;
    s.ctxValid = true;
}

// Ctrl+± / Ctrl+0 while the bar or a strip is shown: they are sized in screen DIP (u = 1 / zoom), so their height in
// canvas units follows the zoom, and so does the scroll the bar's slide added (§12.1) - else a scrolled frame repairs
// the strip at its old size, a click below its middle goes to the text, and leaving shifts the page.
void EditZoomChanged(float from) {
    float k = from / g.cfg.zoom;
    g.barComp *= k;
    s.maxComp *= k;
    StripRelayout();
}

// A <details> folded shut in edit mode: a caret inside it would stand in text nobody sees, and typing would go there.
// It goes to the summary line (its atom).
void EditDetailsToggled(uint32_t summary) {
    if (!g.editing || s.focus.block < 0 || (size_t)s.focus.block >= g.doc.blocks.size()) return;
    if (!BlockHidden(g.doc.blocks[s.focus.block]) && (s.st.anchor == s.st.focus || s.anchor.block < 0 ||
                                                        !BlockHidden(g.doc.blocks[s.anchor.block])))
        return;
    MoveCaret(SnapStop(TextPos{g.doc.blocks[summary].textOff, (int32_t)summary, -1}, -1), false);
}

// ------------------------------------------------------------------------------------------------ commands (§13.1)
bool EditCommand(UINT id, UINT) {
    switch (id) {
    case CMD_RECOVERY_OPEN: case CMD_RECOVERY_RESTORE: case CMD_RECOVERY_DELETE:
        RecoveryCommand(id);
        return true;
    case CMD_OTHER_WINDOW: GoToOwner(); return true;
    case CMD_STRIP_CLOSE: StripHide(STRIP_OTHER_WINDOW); return true;
    case CMD_EDIT_TOGGLE:  // (F2 on a selected atom opens its popup from 3b on; until then it leaves, as without one)
        if (!g.editing) EditEnter(ENTER_CARET);
        else if (EditLeave()) s.leftAt = GetTickCount();
        return true;
    case CMD_EDIT_HERE:
        if (!g.editing) {
            if (s.ctxValid && s.ctxX >= 0) EditEnter(ENTER_POINT, s.ctxX, s.ctxY);
            else EditEnter(ENTER_CARET);  // a menu opened by a key: at the caret
        }
        s.ctxValid = false;
        return true;
    case CMD_EDIT_EXIT:
        if (g.editing) EditLeave();
        return true;
    }
    if (!g.editing) {
        if (id == CMD_SAVE_AS && EditDirty()) { SaveAs(); return true; }
        return id >= CMD_EDIT_TOGGLE && id <= CMD_STRIP_CLOSE;  // edit mode's commands do nothing in reading mode
    }
    switch (id) {
    case CMD_UNDO: UndoRedo(false); return true;
    case CMD_REDO: UndoRedo(true); return true;
    case CMD_SAVE: {
        SaveState st = Flush();
        if (st == SS_SAVED) StripHide(STRIP_LEAVE);
        return true;
    }
    case CMD_SAVE_AS: SaveAs(); return true;
    case CMD_SELECT_ALL: SelectAllEdit(); return true;
    case CMD_COPY_MD: {
        std::wstring md = EditSelectionSource();
        if (!md.empty()) {
            CopyToClipboard(md);
            ShowToast(Tr(S_COPIED_MD), 900);
        }
        return true;
    }
    case CMD_RELOAD:  // F5: the edits into the file first, then the file is read again (R14)
        if (Flush() != SS_SAVED) { s.leaveWhy = s.saveState; StripShow(STRIP_LEAVE); return true; }
        EditExit(true);
        ReloadDocument();
        ShowToast(Tr(S_RELOADED), 700);
        return true;
    case CMD_EDIT:  // Ctrl+E: saved, left, then the external editor sees the edits (D19)
        if (Flush() != SS_SAVED) { s.leaveWhy = s.saveState; StripShow(STRIP_LEAVE); return true; }
        EditExit(false);
        OpenInEditor();
        return true;
    case CMD_CONFLICT_LOAD: {  // the disk's version, as one ADOPT step (Ctrl+Z brings the edits back)
        WaitJob();
        std::string bytes;
        DiskState now;
        DWORD e = 0;
        if (ReadDisk(g.path.c_str(), bytes, &now, &e) != SS_SAVED) { ShowToast(Format(S_ED_NO_READ_FMT, WhyText(SS_UNKNOWN)), 3000); return true; }
        UINT acp = g.disk.cp != CP_UTF8 && g.disk.cp != 1200 ? g.disk.cp : AnsiCodePage();
        if (DecodeDisk(bytes, acp, now) != DR_OK) {
            // Not something edit mode can hold (another encoding, binary): reading mode shows it. No undo reaches
            // across that reload, so the edits go to the journal first - the reload's open offers them (§10.6). If
            // they cannot be kept there, the conflict stays: Save As and Overwrite are still there.
            s.journalFile.clear();
            const uint32_t vol = g.disk.volume;
            const uint64_t index = g.disk.index;
            g.disk.volume = now.volume;  // named for the file as it is now (an editor that saves by renaming makes a new one)
            g.disk.index = now.index;
            WriteJournalNow();
            if (s.journalFile.empty()) {
                g.disk.volume = vol;
                g.disk.index = index;
                ShowToast(Tr(S_ED_LOSSY), 3000);
                return true;
            }
            s.saveState = SS_SAVED;
            g.src = g.disk.text;
            EditExit(true);
            ReloadDocument();
            return true;
        }
        now.valid = true;
        g.disk = std::move(now);
        g.eol = DiskEol(g.disk);
        g.fileTime = g.disk.mtime;
        g.fileSize = g.disk.size;
        ReplaceAll(g.disk.text, EK_ADOPT);
        s.saveState = SS_SAVED;
        StripHide(STRIP_CONFLICT);
        DeleteJournal();
        RevealCaret();
        StatusTick();
        return true;
    }
    case CMD_CONFLICT_KEEP: OverwriteDisk(); return true;
    case CMD_SAVE_RETRY:
        if (Flush() == SS_SAVED) {
            StripHide(STRIP_LEAVE);
            EditExit(false);
        }
        return true;
    case CMD_DISCARD_EDITS:  // back to the disk's text as one undoable step, then out (§2.2)
        Discard();
        StripHide(STRIP_LEAVE);
        EditExit(false);
        return true;
    case CMD_ENC_UTF8: ConvertUtf8(); return true;
    case CMD_ENC_REMOVE_CHAR: {  // the characters the encoding cannot hold go, as one undo step
        EditSync();
        const std::wstring& a = g.disk.text;
        const std::wstring& b = g.src;
        size_t p = 0, n = std::min(a.size(), b.size());
        while (p < n && a[p] == b[p]) p++;
        size_t q = 0;
        while (q < n - p && a[a.size() - 1 - q] == b[b.size() - 1 - q]) q++;
        EditResult r;
        r.after = s.st;
        r.kind = EK_OTHER;
        std::vector<Splice> sp;
        for (size_t i = p; i < b.size() - q;) {
            size_t k = b[i] >= 0xD800 && b[i] <= 0xDBFF && i + 1 < b.size() ? 2 : 1;
            std::string out;
            size_t bad;
            const char* why;
            bool look = s.lastReason == "BOM_LOOKALIKE" && i == 0 && b[0] == 0xFEFF;
            if (look || (b[i] >= 0x80 && !EncodeText(g.disk.cp, b.data() + i, k, out, &bad, &why)))
                sp.push_back(Splice{(uint32_t)i, b.substr(i, k), L""});
            i += k;
        }
        if (s.lastReason == "BOM_LOOKALIKE" && sp.empty() && !b.empty())
            sp.push_back(Splice{0, b.substr(0, 1), L""});
        for (size_t k = sp.size(); k-- > 0;) r.splices.push_back(sp[k]);  // back to front: the offsets hold
        for (const Splice& x : sp)
            if (x.at < r.after.focus) r.after.focus = r.after.anchor = std::max<uint32_t>(x.at, r.after.focus - (uint32_t)x.removed.size());
        s.saveState = SS_SAVED;
        StripHide(STRIP_ENCODING);
        if (!r.splices.empty()) Apply(std::move(r));
        if (EditDirty()) Saved(SaveNow(false));
        else StatusTick();
        return true;
    }
    case CMD_CUT: Cut(); return true;
    case CMD_PASTE: Paste(); return true;
    case CMD_NEW_PARAGRAPH: EnterKey(2); return true;
    }
    // the formatting, insert, table and popup commands arrive with Phase 3a and 3b
    return id >= CMD_EDIT_TOGGLE && id <= CMD_STRIP_CLOSE;
}

// ------------------------------------------------------------------------------------------------ test hooks (§13.5)
// FASTMD_TEST_HOOKS=1: WM_COPYDATA dwData 1 = a splice "at\tlen\ttext" through the splice primitive and the swap (reading
// mode too, nothing saved) → 1 done, 0 refused; dwData 2 = SaveSource now (a flush point) → 1 + the SaveState;
// dwData 3 = a modal loop of its own for "<ms>" milliseconds → 1 when it is over.
LRESULT EditCopyData(const COPYDATASTRUCT* cd) {
    if (!HooksOn() || !cd || !g.ready || g.firstFrame || g.path.empty()) return 0;
    SaveState st;
    if (cd->dwData == 1) {
        std::wstring m((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        size_t t1 = m.find(L'\t'), t2 = t1 == std::wstring::npos ? t1 : m.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos) return 0;
        uint32_t at = (uint32_t)wcstoul(m.c_str(), nullptr, 10), len = (uint32_t)wcstoul(m.c_str() + t1 + 1, nullptr, 10);
        std::wstring text = m.substr(t2 + 1);
        EditSync();
        if (EditBaseline(&st) != BL_OK || !EditSplice(at, len, text)) return 0;
        if (g.editing) {  // the caret keeps its place by source offset
            auto shift = [&](uint32_t o) -> uint32_t {
                if (o <= at) return o;
                if (o >= at + len) return o - len + (uint32_t)text.size();
                return at + (uint32_t)text.size();
            };
            s.st.focus = shift(s.st.focus);
            s.st.anchor = shift(s.st.anchor);
        }
        EditReparse(at, len, (uint32_t)text.size());
        if (g.editing) {
            s.undo.BreakCoalescing();
            AfterChange();
        }
        return 1;
    }
    if (cd->dwData == 2) {
        if (EditBaseline(&st) != BL_OK) return 1 + SS_FAILED;
        EditSync();
        WaitJob();
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

// ------------------------------------------------------------------------------------------------ timers (§13.3)
void EditTimer(UINT_PTR id) {
    // inside a menu, a dialog or a print job the model is not swapped and nothing is written: later (§10.10)
    if (g.editModal > 0 && (id == TIMER_EDIT_SAVE || id == TIMER_EDIT_RETRY || id == TIMER_EDIT_REPARSE ||
                            id == TIMER_EDIT_POPUP || id == TIMER_EDIT_IDLE)) {
        SetTimer(g.hwnd, id, 250, nullptr);
        return;
    }
    switch (id) {
    case TIMER_CARET:
        s.phaseOn = !s.phaseOn;
        s.toggles++;
        UpdateCaretVisible();
        break;
    case TIMER_EDIT_UI:
        KillTimer(g.hwnd, TIMER_EDIT_UI);
        UiaDocumentChanged();
        UiaSelectionChanged();
        BarChanged();  // the status slot's 300 ms have passed
        if (s.hintPending && g.editing) {  // the first entry's hint, once no third click can take the entry back
            DWORD since = GetTickCount() - s.enteredAt, wait = GetDoubleClickTime();
            if (since > wait) ShowHint();
            else SetTimer(g.hwnd, TIMER_EDIT_UI, wait - since + 20, nullptr);
        }
        break;
    case TIMER_EDIT_SAVE:
        KillTimer(g.hwnd, TIMER_EDIT_SAVE);
        s.saveArmed = false;
        if (g.editing && g.cfg.autosave && !Paused(s.saveState)) {
            SaveState st = SaveNow(false);
            (void)st;
        }
        break;
    case TIMER_EDIT_RETRY:
        KillTimer(g.hwnd, TIMER_EDIT_RETRY);
        s.retryArmed = false;
        if (s.saveState == SS_MISSING) EditOnFileChanged();  // is it back?
        else if (EditDirty() && (g.cfg.autosave || !g.editing)) SaveNow(false);
        break;
    case TIMER_EDIT_JOURNAL:
        KillTimer(g.hwnd, TIMER_EDIT_JOURNAL);
        WriteJournalNow();
        break;
    case TIMER_EDIT_REPARSE: EditSync(); break;
    case TIMER_EDIT_IDLE:
        KillTimer(g.hwnd, TIMER_EDIT_IDLE);
        if (g.editing) {
            EditSync();
            StartMeasure();  // the guessed heights, nearest the view first (§5.4)
        }
        break;
    }
}

void EditAutosaveChanged() {
    if (!g.editing) return;
    if (g.cfg.autosave) ArmAutosave();
    else {
        KillTimer(g.hwnd, TIMER_EDIT_SAVE);
        s.saveArmed = false;
        ArmJournal();
    }
    StatusTick();
}

// ------------------------------------------------------------------------------------------------ queries (§13.2)
bool EditQuery(UINT q, LPARAM lp, LRESULT* out) {
    const bool doc = !g.path.empty() && !g.loadFailed;
    // the text the disk holds: the baseline once one was taken, else what was loaded (reading mode is never dirty)
    const std::wstring* disk = g.disk.valid ? &g.disk.text : doc ? &g.src : nullptr;
    switch (q) {
    case Q_EDITING: *out = g.editing; return true;
    case Q_EDIT_DIRTY: *out = EditDirty(); return true;
    case Q_EDIT_CARET_SRC: EditSync(); *out = g.editing ? (LRESULT)s.st.focus : -1; return true;
    case Q_EDIT_ANCHOR_SRC: EditSync(); *out = g.editing ? (LRESULT)s.st.anchor : -1; return true;
    case Q_EDIT_BAR: *out = std::lround(g.barT * 100); return true;
    case Q_RELOADS: *out = g.reloads; return true;
    case Q_SAVES: *out = g.saves; return true;
    case Q_SRC_HASH:
    case Q_SRC_LEN: {
        const std::wstring* t = lp == 0 ? (doc ? &g.src : nullptr) : disk;
        *out = !t ? -1 : q == Q_SRC_HASH ? (LRESULT)Fnv32(*t) : (LRESULT)t->size();
        return true;
    }
    case Q_EDIT_SAVE_STATE: *out = lp == 1 ? (LRESULT)s.failToasts : (LRESULT)EditSaveState(); return true;
    case Q_EDIT_CONFLICT: *out = s.saveState == SS_CONFLICT; return true;
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
    case Q_EDIT_POPUP: case Q_EDIT_POPUP_STATE: case Q_EDIT_BUBBLE: *out = 0; return true;
    case Q_EDIT_RAW: *out = g.editing && (!s.masks.empty() || !s.raw.empty()); return true;
    case Q_EDIT_PHANTOM:  // lp 0 the block it stands next to (-1 none), 1 its kind, 2 its style (§13.2)
        EditSync();
        *out = lp == 0 ? g.phantomBlock : g.phantomBlock < 0 ? 0 : lp == 1 ? s.st.phantom.kind : s.st.phantom.style;
        return true;
    case Q_EDIT_ATOM: *out = g.editing ? s.st.atom : -1; return true;
    case Q_EDIT_COLLAPSE: *out = BarCollapse(); return true;
    case Q_EDIT_CARET_VISIBLE: *out = g.editing && g.caretOn && g.caretVisible; return true;
    case Q_EDIT_CARET_PHASE: *out = s.toggles; return true;
    case Q_LAST_PROMPT:  // (lp 2: the hash of the last toast's text, which one said why - Phase 2a notes)
        *out = lp == 1 ? (LRESULT)s.prompts : lp == 2 ? (LRESULT)Fnv32(g.toast) : (LRESULT)s.lastPrompt;
        return true;
    case Q_FRAME_STATS: {
        if (lp == 1) { *out = g.framesFull - s.framesFull; s.framesFull = g.framesFull; }
        else { *out = g.framesPartial - s.framesPartial; s.framesPartial = g.framesPartial; }
        return true;
    }
    case Q_EDIT_ACTIVE: {  // what the caret stands in (§13.2): the bits Phase 2a knows
        LRESULT b = 0;
        if (!g.editing) { *out = 0; return true; }
        EditSync();
        if (s.st.atom >= 0) b |= 1 << 14;
        if (InPhantom()) b |= 1 << 15;
        int32_t bi = s.focus.block;
        if (bi >= 0 && (size_t)bi < g.doc.blocks.size()) {
            const Block& bl = g.doc.blocks[bi];
            if (bl.marker == MK_BULLET) b |= 1 << 8;
            if (bl.marker == MK_NUMBER) b |= 1 << 9;
            if (bl.marker == MK_TASK_OPEN || bl.marker == MK_TASK_DONE) b |= 1 << 10;
            if (bl.muted && !bl.heading) b |= 1 << 11;
            if (bl.kind == BK_TABLE) b |= 1 << 12;
            if (bl.kind == BK_CODE) b |= 1 << 13;
            if (g.doc.blockSrc.size() == g.doc.blocks.size() && (g.doc.blockSrc[bi].flags & BS_FOOTNOTE)) b |= 1 << 16;
            // the formatting of the character before the caret (or after it at a block's start)
            uint32_t t = s.focus.t > bl.textOff ? s.focus.t - 1 : s.focus.t;
            for (uint32_t k = 0; k < bl.runCount; k++) {
                const Run& r = g.doc.runs[bl.runOff + k];
                if (t < r.start || t >= r.start + r.len) continue;
                if (r.flags & F_BOLD) b |= FMT_BOLD;
                if (r.flags & F_ITALIC) b |= FMT_ITALIC;
                if (r.flags & F_STRIKE) b |= FMT_STRIKE;
                if (r.flags & F_CODE) b |= FMT_CODE;
                if (r.flags & F_LINK) b |= FMT_LINK | (1 << 17);
            }
        }
        b |= (LRESULT)EditStyleId() << 24;
        *out = b;
        return true;
    }
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
    case Q_EDIT_BUSY: {  // what is still on its way (§13.2): tests settle on 0
        LRESULT b = 0;
        if (s.st.burstBeg != UINT32_MAX) b |= 1;
        if (s.saveArmed) b |= 2;
        if (s.job) b |= 4;
        if (g.jobsPending > 0) b |= 8;
        for (const Image& im : g.doc.images)
            if (im.state == RS_PENDING) { b |= 32; break; }
        if (g.barSliding) b |= 64;
        if (g.animating) b |= 128;
        if (s.retryArmed) b |= 512;
        if (s.journalArmed) b |= 1024;
        if (!s.deferred.empty()) b |= 2048;
        *out = b;
        return true;
    }
    case Q_EDIT_STRIP: *out = StripKind(); return true;
    case Q_EDIT_TOOL: *out = BarToolCenter((UINT)(lp & 0xFFFF)); return true;
    case Q_RELAYOUT_ALL: {
        // The oracle for the swap (T7): every layout made anew from the model, every height exact as the background
        // measuring leaves it, the scroll position kept; a shot of this frame must equal one taken after an edit.
        EditSync();
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
        if (g.editing && g.caretBlock >= 0 && (size_t)g.caretBlock < g.doc.blocks.size()) EnsureLayout((uint32_t)g.caretBlock);
        ForceFullRedraw();
        Invalidate();
        *out = 1;
        return true;
    }
    }
    return false;
}

// Back to the compiler's own inlining for the templates instantiated at the end of the file: the whole program shares
// them (see the end of editcore.cpp).
#pragma inline_depth()
