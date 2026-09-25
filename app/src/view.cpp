// Document view: column geometry, virtualised layout, drawing, hit-testing, selection, horizontal scrolling of wide
// blocks, keyboard link focus, overlays.
// Units: DIP (layout space). Pixels = DIP × Scale(), Scale() = DPI/96 × zoom.
#include "app.h"

App g;

float Scale() { return g.dpi / 96.f * g.cfg.zoom; }
float ViewW() { return g.pxW / Scale(); }
float ViewH() { return g.pxH / Scale(); }
float MaxScroll() { return std::max(0.f, g.docH - ViewH()); }
float DocLeft() { return TocDocked() ? TocPanelW() : 0.f; }
float DocW() { return std::max(1.f, ViewW() - DocLeft()); }

// ------------------------------------------------------------------------------------------------ columns
static float TextMaxFor(uint8_t preset) {
    switch (preset) {
    case COL_NARROW: return 560.f;
    case COL_WIDE: return 900.f;
    case COL_FULL: return 1e9f;
    default: return Metrics::kTextMax;
    }
}

void UpdateColumns() {
    float avail = std::max(160.f, DocW() - 2 * Metrics::kPadX);
    float tmax = TextMaxFor(g.cfg.column);
    g.availW = avail;
    g.textW = std::min(avail, tmax);
    g.wideW = g.cfg.column == COL_FULL ? avail : std::min(avail, tmax + Metrics::kBreakout);
}
float TextLeft() { return DocLeft() + std::floor((DocW() - g.textW) * 0.5f); }
float WideLeft() { return DocLeft() + std::floor((DocW() - g.wideW) * 0.5f); }
float AvailLeft() { return DocLeft() + std::floor((DocW() - g.availW) * 0.5f); }
static bool IsWide(const Block& b) {
    return b.indent == 0 && (b.kind == BK_CODE || b.kind == BK_TABLE || b.kind == BK_IMAGE);
}
float LayoutWidthFor(const Block& b, float textW, float wideW) { return IsWide(b) ? wideW : textW; }

void BlockBox(uint32_t i, float* x, float* w) {
    const Block& b = g.doc.blocks[i];
    float tl = TextLeft();
    if (!IsWide(b)) { *x = tl + b.indent; *w = g.textW - b.indent; return; }
    BlockLayout* L = g.cache[i];
    float nat = L ? L->natural : g.textW;
    if (b.kind == BK_CODE) nat = std::min(nat, g.wideW);
    if (nat <= g.textW) {  // fits the text column: align with the text (code blocks span the column)
        *w = b.kind == BK_CODE ? g.textW : nat;
        *x = tl;
        if (b.align == 1) *x = tl + std::floor((g.textW - *w) * 0.5f);  // <p align=center><img …>
        else if (b.align == 2) *x = tl + std::max(0.f, g.textW - *w);
        return;
    }
    // A diagram gets the whole width between the margins, not just the breakout other wide blocks may use: it is
    // drawn at its own size, and every pixel of window saved is a pixel of it the reader does not have to scroll to.
    bool full = b.kind == BK_IMAGE && b.aux < g.doc.images.size() && ImageScrollsWide(g.doc.images[b.aux]);
    *w = std::min(nat, full ? g.availW : g.wideW);
    *x = std::max(full ? AvailLeft() : WideLeft(), std::floor(tl - (*w - g.textW) * 0.5f));
}

// ------------------------------------------------------------------------------------------------ geometry
// inside a folded <details>: everything but the summary line takes no space and is not drawn
bool BlockHidden(const Block& b) {
    if (!b.details || (b.details & 0x8000)) return false;
    uint32_t gi = (uint32_t)(b.details & 0x7FFF) - 1;
    return gi < g.doc.detailsOpen.size() && !g.doc.detailsOpen[gi];
}

// Edit mode's toolbar covers the top of the page (EDIT-MODE.md §12.1): the document starts that much lower, and the
// scroll position absorbs it (edit.cpp), so a document scrolled past the top does not move when the bar slides in.
// The bar is sized in DIP of the screen, not of the zoomed document. Printing and the Explorer pane: none.
float EditInset() { return (g.barT > 0 && !g.fitWide) ? 44.f / g.cfg.zoom * g.barT : 0.f; }
// where the caret counts as hidden at the top: under the bar, a strip and the find bar
float EditRevealTop() { return EditInset() + g.stripH + (g.findOpen ? 52.f : 0.f) + 8.f; }
float ScrollTrackTop() { return 2.f + EditInset() + g.stripH; }

// Edit mode's phantom row (§6.7) takes its room next to its block: a line and a paragraph's gap before or after it, or
// a line under it for a pending hard break.
void RecomputeY() {
    float y = Metrics::kPadTop + EditInset();
    size_t n = g.doc.blocks.size();
    const int32_t pb = g.phantomBlock;
    for (size_t i = 0; i < n; i++) {
        const Block& b = g.doc.blocks[i];
        if (BlockHidden(b)) { g.Y[i] = y; continue; }
        y += b.gap;
        if ((int32_t)i == pb && g.phantomBefore) {
            g.phantomY = y;
            y += g.phantomH;
        }
        g.Y[i] = y;
        y += g.H[i];
        if ((int32_t)i == pb && !g.phantomBefore) {
            g.phantomY = y + g.phantomH - g.phantomLine;
            y += g.phantomH;
        }
    }
    g.docH = y + Metrics::kPadBottom;
}

uint32_t FirstVisible(float y) {  // first block whose bottom is below y
    size_t lo = 0, hi = g.doc.blocks.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g.Y[mid] + g.H[mid] <= y) lo = mid + 1;
        else hi = mid;
    }
    return (uint32_t)lo;
}

void ClearLayoutCache() {
    for (auto*& c : g.cache) { delete c; c = nullptr; }
    g.cachedCount = 0;
}

void InitGeometry() {
    size_t n = g.doc.blocks.size();
    ClearLayoutCache();
    g.cache.assign(n, nullptr);
    g.H.resize(n);
    g.Y.resize(n);
    g.known.assign(n, 0);
    if (g.hx.size() != n) g.hx.resize(n, 0.f);  // horizontal offsets survive re-layouts (zoom, resize)
    for (size_t i = 0; i < n; i++) {
        bool exact = false;
        const Block& b = g.doc.blocks[i];
        if (BlockHidden(b)) {  // folded away: no height, and nothing to measure
            g.H[i] = 0.f;
            g.known[i] = 1;
            continue;
        }
        g.H[i] = BlockHeightEstimate(g.doc, g.typo, b, LayoutWidthFor(b, g.textW, g.wideW), &exact);
        g.known[i] = exact;
    }
}

BlockLayout* EnsureLayout(uint32_t i) {
    if (!g.cache[i]) {
        const Block& b = g.doc.blocks[i];
        g.cache[i] = LayoutBlock(g.doc, g.typo, i, LayoutWidthFor(b, g.textW, g.wideW));
        g.cachedCount++;
        g.H[i] = g.cache[i]->height;
        g.known[i] = 1;
    }
    return g.cache[i];
}

// lay out blocks until the viewport is covered with real layouts; height corrections shift the following blocks once
void EnsureVisible() {
    size_t n = g.doc.blocks.size();
    if (!n) return;
    float bottom = g.scrollY + ViewH();
    float delta = 0;
    size_t i = FirstVisible(g.scrollY);
    for (; i < n; i++) {
        g.Y[i] += delta;
        if (g.Y[i] >= bottom) break;
        if (BlockHidden(g.doc.blocks[i])) continue;
        if (!g.cache[i]) {
            float old = g.H[i];
            EnsureLayout((uint32_t)i);
            delta += g.H[i] - old;
        }
    }
    if (delta != 0) {
        for (size_t k = i + 1; k < n; k++) g.Y[k] += delta;
        g.docH += delta;
    }
}

void InitialLayout() {
    InitGeometry();
    RecomputeY();
    g.scrollY = g.targetY = std::clamp(g.scrollY, 0.f, MaxScroll());
    EnsureVisible();
}

// keep memory bounded on huge documents: drop cached layouts far away from the viewport
void TrimCache() {
    if (g.cachedCount < 3000) return;
    size_t n = g.doc.blocks.size();
    size_t first = FirstVisible(g.scrollY);
    size_t lo = first > 300 ? first - 300 : 0, hi = std::min(n, first + 600);
    for (size_t i = 0; i < n; i++) {
        if (i >= lo && i < hi) continue;
        if (g.cache[i]) { delete g.cache[i]; g.cache[i] = nullptr; g.cachedCount--; }
    }
}

void WithAnchor(void (*fn)()) {
    bool has = !g.doc.blocks.empty();
    uint32_t a = has ? std::min<uint32_t>(FirstVisible(g.scrollY), (uint32_t)g.doc.blocks.size() - 1) : 0;
    float off = has ? g.scrollY - g.Y[a] : 0;
    float toff = g.targetY - g.scrollY;
    fn();
    if (has && a < g.doc.blocks.size() && g.scrollY > 0) g.scrollY = g.Y[a] + off;
    g.scrollY = std::clamp(g.scrollY, 0.f, MaxScroll());
    g.targetY = std::clamp(g.scrollY + toff, 0.f, MaxScroll());
}

uint32_t BlockOfPos(uint32_t pos) {
    auto& bl = g.doc.blocks;
    size_t lo = 0, hi = bl.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (bl[mid].textOff <= pos) lo = mid + 1;
        else hi = mid;
    }
    return lo ? (uint32_t)lo - 1 : 0;
}

// ------------------------------------------------------------------------------------------------ horizontal scrolling
static float TableMaxW(float x) { return g.wideW - (x - WideLeft()); }

bool ImageScrollsWide(const Image& im) {
    return im.mathKind == 3 && im.attrW <= 0 && im.w > 0 && !g.fitWide;
}

bool HScrollInfo(uint32_t i, float* visX, float* visW, float* contentW) {
    if (i >= g.doc.blocks.size() || i >= g.cache.size()) return false;
    const Block& b = g.doc.blocks[i];
    BlockLayout* L = g.cache[i];
    if (!L || (b.kind != BK_CODE && b.kind != BK_TABLE && b.kind != BK_IMAGE)) return false;
    float x, w;
    BlockBox(i, &x, &w);
    float content, vis;
    if (b.kind == BK_CODE) {
        content = L->natural;
        vis = w;
    } else if (b.kind == BK_IMAGE) {
        content = L->natural;   // a diagram at its own size; BlockBox has already clamped the box to the column
        vis = w;
    } else {
        if (!L->table) return false;
        content = L->table->width;
        vis = std::min(content, TableMaxW(x));
    }
    if (content <= vis + 0.5f) return false;
    *visX = x;
    *visW = vis;
    *contentW = content;
    return true;
}

float HScrollOf(uint32_t i) {
    if (i >= g.hx.size() || g.hx[i] == 0) return 0;
    float vx, vw, cw;
    if (!HScrollInfo(i, &vx, &vw, &cw)) return 0;
    return std::clamp(g.hx[i], 0.f, cw - vw);
}

void HScrollSet(uint32_t i, float x) {
    float vx, vw, cw;
    if (!HScrollInfo(i, &vx, &vw, &cw)) return;
    if (g.hx.size() <= i) g.hx.resize(g.doc.blocks.size(), 0.f);
    x = std::round(std::clamp(x, 0.f, cw - vw));
    if (x == g.hx[i]) return;
    g.hx[i] = x;
    g.hxSerial++;
    g.hbarFlash = (int)i;  // the scrollbar shows for a moment: position feedback for wheel / touchpad / find
    g.hbarFlashUntil = GetTickCount() + 900;
    if (g.hwnd) SetTimer(g.hwnd, TIMER_HBAR, 950, nullptr);
    Invalidate();
}

bool HScrollBarRect(uint32_t i, float* l, float* t, float* r, float* b, float* thumbL, float* thumbR) {
    float vx, vw, cw;
    if (!HScrollInfo(i, &vx, &vw, &cw)) return false;
    float y = g.Y[i] - g.scrollY, h = g.H[i];
    float top = g.doc.blocks[i].kind == BK_CODE ? y + h - 10.f : y + h + 4.f;  // tables: in the margin below
    *l = vx + 8.f;
    *r = vx + vw - 8.f;
    *t = top;
    *b = top + 6.f;
    float trackW = *r - *l, thumbW = std::max(28.f, trackW * vw / cw), maxOff = cw - vw;
    *thumbL = *l + (trackW - thumbW) * (maxOff > 0 ? HScrollOf(i) / maxOff : 0.f);
    *thumbR = *thumbL + thumbW;
    return true;
}

int HScrollBlockAt(float px, float py, bool* onBar) {
    *onBar = false;
    size_t n = g.doc.blocks.size();
    if (g.path.empty() || !n) return -1;
    float docY = py + g.scrollY;
    uint32_t first = FirstVisible(docY);
    for (uint32_t k : {first, first ? first - 1 : UINT32_MAX}) {  // a table's bar sits in the gap below it
        if (k >= n) continue;
        float vx, vw, cw;
        if (!HScrollInfo(k, &vx, &vw, &cw)) continue;
        float bottom = g.Y[k] + g.H[k] + (g.doc.blocks[k].kind == BK_CODE ? 0.f : 12.f);
        if (docY < g.Y[k] || docY >= bottom || px < vx || px > vx + vw) continue;
        float l, t, r, b, tl, tr;
        if (HScrollBarRect(k, &l, &t, &r, &b, &tl, &tr)) *onBar = py >= t - 5.f && py <= b + 5.f;
        return (int)k;
    }
    return -1;
}

static void DrawHScrollBar(uint32_t i) {
    bool flash = (int)i == g.hbarFlash && GetTickCount() < g.hbarFlashUntil;
    if ((int)i != g.hoverHBlock && (int)i != g.dragHBlock && !flash) return;
    float l, t, r, b, tl, tr;
    if (!HScrollBarRect(i, &l, &t, &r, &b, &tl, &tr)) return;
    bool hot = g.dragHBlock == (int)i || g.hotHBar;
    g.canvas->FillRoundRect(tl, t, tr, b, 3.f, hot ? P_SCROLL_HOT : P_SCROLL);
}

// ------------------------------------------------------------------------------------------------ highlight ranges
static uint32_t SelMin() { return std::min(g.selAnchor, g.selFocus); }
static uint32_t SelMax() { return std::max(g.selAnchor, g.selFocus); }
bool HasSelection() { return g.selAnchor != g.selFocus; }
bool PosInSelection(uint32_t pos) { return HasSelection() && pos >= SelMin() && pos < SelMax(); }

// hit-test rectangles of [s, e) inside a layout that starts at text offset textOff
static void RangeRects(IDWriteTextLayout* tl, uint32_t textOff, uint32_t s, uint32_t e, std::vector<DWRITE_HIT_TEST_METRICS>& out) {
    out.clear();
    if (e <= s) return;
    UINT32 cnt = 0;
    out.resize(16);
    HRESULT hr = tl->HitTestTextRange(s - textOff, e - s, 0, 0, out.data(), (UINT32)out.size(), &cnt);
    if (hr == E_NOT_SUFFICIENT_BUFFER) {
        out.resize(cnt);
        hr = tl->HitTestTextRange(s - textOff, e - s, 0, 0, out.data(), cnt, &cnt);
    }
    out.resize(SUCCEEDED(hr) ? cnt : 0);
}

static void FillTextRange(IDWriteTextLayout* tl, uint32_t textOff, uint32_t s, uint32_t e, float x, float y, uint8_t pal) {
    static std::vector<DWRITE_HIT_TEST_METRICS> m;
    RangeRects(tl, textOff, s, e, m);
    for (auto& r : m) g.canvas->FillRect(x + r.left, y + r.top, x + r.left + std::max(r.width, 4.f), y + r.top + r.height, pal);
}

// keyboard focus ring around the focused link (Tab)
static uint32_t g_focusS = 0, g_focusE = 0;
static void DrawLinkFocus(IDWriteTextLayout* tl, uint32_t textOff, uint32_t textLen, float x, float y) {
    if (g.focusLink < 0 || g_focusE <= g_focusS) return;
    uint32_t s = std::max(g_focusS, textOff), e = std::min(g_focusE, textOff + textLen);
    if (s >= e) return;
    static std::vector<DWRITE_HIT_TEST_METRICS> m;
    RangeRects(tl, textOff, s, e, m);
    for (auto& r : m)
        g.canvas->StrokeRoundRect(x + r.left - 3.f, y + r.top - 1.f, x + r.left + r.width + 3.f, y + r.top + r.height + 1.f,
                                  4.f, 2.f, P_ACCENT);
}

// selection + find matches behind the text of one layout covering [textOff, textOff+textLen)
static void DrawHighlights(IDWriteTextLayout* tl, uint32_t textOff, uint32_t textLen, float x, float y) {
    uint32_t end = textOff + textLen;
    if (!g.matches.empty() && g.findOpen) {
        uint32_t ml = (uint32_t)g.findQuery.size();
        auto it = std::lower_bound(g.matches.begin(), g.matches.end(), textOff > ml ? textOff - ml : 0);
        for (; it != g.matches.end() && *it < end; ++it) {
            uint32_t s = std::max(*it, textOff), e = std::min(*it + ml, end);
            bool cur = g.curMatch >= 0 && (size_t)g.curMatch < g.matches.size() && g.matches[g.curMatch] == *it;
            FillTextRange(tl, textOff, s, e, x, y, cur ? P_FIND_CUR : P_FIND);
        }
    }
    if (HasSelection()) {
        uint32_t s = std::max(SelMin(), textOff), e = std::min(SelMax(), end);
        if (s < e) FillTextRange(tl, textOff, s, e, x, y, P_SELECTION);
    }
}

// ------------------------------------------------------------------------------------------------ drawing
// The caret (the moving end of a keyboard selection) is drawn with the document, so a scroll carries it along with
// the text. Its geometry lives with the rest of the selection code, below.
static bool CaretGeom(uint32_t pos, float* cx, float* docY, float* h, bool relayout);

static void ApplyColors(BlockLayout* L, const Block& b) {
    if (L->colored) return;
    L->colored = true;
    auto apply = [&](IDWriteTextLayout* tl, uint32_t textOff, uint32_t runOff, uint32_t runCount) {
        for (uint32_t k = 0; k < runCount; k++) {
            const Run& r = g.doc.runs[runOff + k];
            int shift = (r.flags & F_SUP) ? 1 : (r.flags & F_SUB) ? -1 : 0;
            if (r.color != P_DEFAULT || shift)
                tl->SetDrawingEffect(g.canvas->Effect(r.color, shift), DWRITE_TEXT_RANGE{r.start - textOff, r.len});
        }
    };
    if (L->text && b.kind != BK_IMAGE) apply(L->text, b.textOff, b.runOff, b.runCount);
    if (L->table) {
        const Table& t = g.doc.tables[b.aux];
        for (uint32_t c = 0; c < t.rows * t.cols; c++) {
            const Cell& cell = g.doc.cells[t.cellOff + c];
            if (L->table->cells[c]) apply(L->table->cells[c], cell.textOff, cell.runOff, cell.runCount);
        }
    }
}

static void InlineCodeBackgrounds(IDWriteTextLayout* tl, uint32_t textOff, uint32_t runOff, uint32_t runCount, int role,
                                  std::vector<D2D1_RECT_F>& out, std::vector<D2D1_RECT_F>* kbd = nullptr) {
    DWRITE_HIT_TEST_METRICS hm[16];
    for (uint32_t k = 0; k < runCount; k++) {
        const Run& r = g.doc.runs[runOff + k];
        if (!(r.flags & (F_CODE | F_KBD))) continue;
        std::vector<D2D1_RECT_F>& dst = (r.flags & F_KBD) && kbd ? *kbd : out;
        UINT32 cnt = 0;
        if (FAILED(tl->HitTestTextRange(r.start - textOff, r.len, 0, 0, hm, 16, &cnt))) continue;
        float fs = g.typo.size[role] * 0.85f, pad = fs * 0.2f;
        for (UINT32 j = 0; j < cnt; j++) {
            float base = hm[j].top + g.typo.baseline[role];
            dst.push_back(D2D1::RectF(hm[j].left, std::round(base - g.typo.monoAscent * fs - pad),
                                      hm[j].left + hm[j].width, std::round(base + g.typo.monoDescent * fs + pad)));
        }
    }
}

static IDWriteTextLayout* NumberLayout(uint32_t n) {
    auto it = g.numLayouts.find(n);
    if (it != g.numLayouts.end()) return it->second;
    wchar_t buf[16];
    int len = swprintf_s(buf, L"%u.", n);
    IDWriteTextLayout* L = nullptr;
    g.dwf->CreateTextLayout(buf, len, g.typo.fmt[R_BODY], 200.f, 100.f, &L);
    if (L) L->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g.numLayouts[n] = L;
    return L;
}

static bool IsTask(const Block& b) { return b.marker == MK_TASK_OPEN || b.marker == MK_TASK_DONE; }

// a list marker stands on the baseline of its block's first line: DIP below the block's top
static float MarkerBaseline(const Block& b) {
    if (b.kind == BK_CODE) return Metrics::kCodePad + g.typo.baseline[R_CODE];
    if (b.kind == BK_TEXT && b.heading) return g.typo.baseline[b.heading];
    return g.typo.baseline[R_BODY];
}

// the box of a task list item, left of the text that starts at x
static D2D1_RECT_F TaskBox(float x, float baseline) {
    float s = g.typo.textScale, l = x - 24.f * s, t = baseline - 12.5f * s;
    return D2D1::RectF(l, t, l + 15.f * s, t + 15.f * s);
}

static void DrawMarker(const Block& b, float x, float baseline, bool hot) {
    uint8_t col = b.muted ? P_MUTED : P_TEXT;
    float s = g.typo.textScale;
    switch (b.marker) {
    case MK_BULLET: {
        float cy = baseline - 5.f * s, cx = x - 14.f * s;
        if (b.listLevel <= 1) g.canvas->FillCircle(cx, cy, 2.75f * s, col);
        else if (b.listLevel == 2) g.canvas->StrokeCircle(cx, cy, 3.f * s, 1.1f, col);
        else g.canvas->FillRect(cx - 2.5f * s, cy - 2.5f * s, cx + 2.5f * s, cy + 2.5f * s, col);
        break;
    }
    case MK_NUMBER: {
        IDWriteTextLayout* L = NumberLayout(b.number);
        if (!L) break;
        DWRITE_TEXT_METRICS m{};
        L->GetMetrics(&m);
        g.canvas->Text(L, x - 6.f - m.widthIncludingTrailingWhitespace, baseline - g.typo.baseline[R_BODY], col);
        break;
    }
    case MK_TASK_OPEN:
    case MK_TASK_DONE: {
        D2D1_RECT_F r = TaskBox(x, baseline);
        float l = r.left, t = r.top;
        if (b.marker == MK_TASK_DONE) {
            g.canvas->FillRoundRect(l, t, r.right, r.bottom, 3.5f * s, P_ACCENT);
            g.canvas->Line(l + 3.8f * s, t + 7.8f * s, l + 6.4f * s, t + 10.4f * s, 1.8f * s, P_ONACCENT);
            g.canvas->Line(l + 6.4f * s, t + 10.4f * s, l + 11.2f * s, t + 4.8f * s, 1.8f * s, P_ONACCENT);
        } else {  // under the pointer the empty box takes the accent: it can be clicked
            g.canvas->StrokeRoundRect(l, t, r.right, r.bottom, 3.5f * s, hot ? 1.5f : 1.1f, hot ? P_ACCENT : P_MUTED);
        }
        break;
    }
    default: break;
    }
}

static void DrawTable(uint32_t i, const Block& b, BlockLayout* L, float x, float y) {
    const Table& t = g.doc.tables[b.aux];
    TableLayout* tl = L->table;
    float maxW = TableMaxW(x);
    bool clip = tl->width > maxW + 0.5f;
    float x0 = x - HScrollOf(i);  // content origin (scrolled)
    float right = x0 + tl->width;
    if (clip) g.canvas->PushClip(x, y, x + maxW, y + tl->height);
    float yy = y;
    for (uint32_t r = 0; r < t.rows; r++) {
        float rh = tl->rowH[r];
        if (yy > ViewH() + 1 || yy + rh < -1) { yy += rh; continue; }
        if (r >= 2 && (r % 2) == 0) g.canvas->FillRect(x0, yy, right, yy + rh, P_ZEBRA);
        g.canvas->FillRect(x0, yy, right, yy + 1, P_BORDER);
        float xx = x0;
        for (uint32_t c = 0; c < t.cols; c++) {
            IDWriteTextLayout* cl = tl->cells[r * t.cols + c];
            if (cl && xx < x + maxW && xx + tl->colW[c] > x) {
                const Cell& cell = g.doc.cells[t.cellOff + r * t.cols + c];
                float tx = xx + 1 + Metrics::kCellPadX, ty = yy + 1 + Metrics::kCellPadY;
                bool hasCode = false;
                for (uint32_t k = 0; k < cell.runCount; k++) hasCode |= (g.doc.runs[cell.runOff + k].flags & F_CODE) != 0;
                if (hasCode) {
                    std::vector<D2D1_RECT_F> bgs;
                    InlineCodeBackgrounds(cl, cell.textOff, cell.runOff, cell.runCount, R_BODY, bgs);
                    for (auto& rc : bgs) g.canvas->FillRoundRect(tx + rc.left, ty + rc.top, tx + rc.right, ty + rc.bottom, 4.f, P_INLINEBG);
                }
                DrawHighlights(cl, cell.textOff, cell.textLen, tx, ty);
                g.canvas->Text(cl, tx, ty, b.muted ? P_MUTED : P_TEXT);
                DrawLinkFocus(cl, cell.textOff, cell.textLen, tx, ty);
                if (g.editing && r == 0 && !cell.textLen) {  // an empty header cell says what it is, on screen only (UX-24)
                    // (in the column's own width, cut with "…"; an empty column is laid out wide enough, Phase 4 notes)
                    wchar_t ph[64];
                    swprintf_s(ph, Tr(S_ED_COLUMN_FMT), (int)c + 1);
                    float room = std::max(1.f, tl->colW[c] - 1 - 2 * Metrics::kCellPadX);
                    if (IDWriteTextLayout* pl = UiLayout(ph, room, g.typo.fmt[R_BODY])) {
                        pl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                        IDWriteInlineObject* ellipsis = nullptr;
                        if (SUCCEEDED(g.dwf->CreateEllipsisTrimmingSign(pl, &ellipsis))) pl->SetTrimming(&trim, ellipsis);
                        g.canvas->PushClip(xx + 1, yy, xx + tl->colW[c], yy + rh);
                        g.canvas->Text(pl, tx, ty, P_MUTED);
                        g.canvas->PopClip();
                        SafeRelease(ellipsis);
                        pl->Release();
                    }
                }
            }
            xx += tl->colW[c];
        }
        yy += rh;
    }
    g.canvas->FillRect(x0, yy, right, yy + 1, P_BORDER);
    float xx = x0;
    for (uint32_t c = 0; c <= t.cols; c++) {
        g.canvas->FillRect(xx, y, xx + 1, y + tl->height, P_BORDER);
        if (c < t.cols) xx += tl->colW[c];
    }
    if (clip) g.canvas->PopClip();
}

const float kAnchorGap = 24.f;  // where the heading's link icon sits, left of the text column

// the <summary> line under the pointer (-1 = none): clicking it folds the <details> open or shut
int SummaryAt(float px, float py) {
    if (g.path.empty() || g.Y.size() != g.doc.blocks.size()) return -1;
    float vh = ViewH();
    size_t n = g.doc.blocks.size();
    for (uint32_t i = FirstVisible(g.scrollY); i < n; i++) {
        float top = g.Y[i] - g.scrollY;
        if (top > vh) break;
        const Block& b = g.doc.blocks[i];
        if (!(b.details & 0x8000) || py < top || py >= top + g.H[i]) continue;
        float x, w;
        BlockBox(i, &x, &w);
        if (px >= x - 24.f && px <= x + w) return (int)i;
    }
    return -1;
}

void ToggleDetails(uint32_t i) {
    if (i >= g.doc.blocks.size()) return;
    uint32_t gi = (uint32_t)(g.doc.blocks[i].details & 0x7FFF);
    if (!gi || gi > g.doc.detailsOpen.size()) return;
    g.doc.detailsOpen[gi - 1] = !g.doc.detailsOpen[gi - 1];
    Relayout();
}

// the heading under the pointer, and whether the pointer is on its link icon rather than on the text
int HeadingAt(float px, float py, bool* onIcon) {
    if (onIcon) *onIcon = false;
    if (g.path.empty() || g.Y.size() != g.doc.blocks.size()) return -1;
    float vh = ViewH();
    size_t n = g.doc.blocks.size();
    for (uint32_t i = FirstVisible(g.scrollY); i < n; i++) {
        float top = g.Y[i] - g.scrollY;
        if (top > vh) break;
        const Block& b = g.doc.blocks[i];
        if (!b.heading || b.kind != BK_TEXT || py < top || py >= top + g.H[i]) continue;
        float x, w;
        BlockBox(i, &x, &w);
        if (px < x - kAnchorGap - 4.f || px > x + w) return -1;
        if (onIcon) *onIcon = px < x - 2.f && !IsTask(b);
        return (int)i;
    }
    return -1;
}

// ------------------------------------------------------------------------------------------------ task lists
// box of a task list item in client DIP; false = the block has no box or it is not on screen
bool TaskBoxRect(uint32_t i, float* l, float* t, float* r, float* b) {
    if (i >= g.doc.blocks.size() || g.Y.size() != g.doc.blocks.size()) return false;
    const Block& bl = g.doc.blocks[i];
    if (!IsTask(bl) || BlockHidden(bl)) return false;
    float x, w, top = g.Y[i] - g.scrollY;
    if (top > ViewH() || top + g.H[i] < 0) return false;
    BlockBox(i, &x, &w);
    D2D1_RECT_F rc = TaskBox(x, top + MarkerBaseline(bl));
    *l = rc.left;
    *t = rc.top;
    *r = rc.right;
    *b = rc.bottom;
    return true;
}

// the task box under the pointer (its block, -1 = none), with a little room around it: a 15 px target is small
int TaskAt(float px, float py) {
    const std::vector<Task>& ts = g.doc.tasks;
    if (g.path.empty() || ts.empty() || g.Y.size() != g.doc.blocks.size()) return -1;
    const float kSlack = 3.f;
    uint32_t first = FirstVisible(std::max(0.f, py + g.scrollY - 32.f));
    auto it = std::lower_bound(ts.begin(), ts.end(), first, [](const Task& t, uint32_t bi) { return t.block < bi; });
    for (; it != ts.end() && g.Y[it->block] - g.scrollY < py + 32.f; ++it) {
        float l, t, r, b;
        if (TaskBoxRect(it->block, &l, &t, &r, &b) && px >= l - kSlack && px <= r + kSlack && py >= t - kSlack &&
            py <= b + kSlack)
            return (int)it->block;
    }
    return -1;
}

static void DrawBlock(uint32_t i, float y) {
    const Block& b = g.doc.blocks[i];
    if (BlockHidden(b)) return;
    BlockLayout* L = EnsureLayout(i);
    ApplyColors(L, b);
    float x, w;
    BlockBox(i, &x, &w);
    float right = x + w;
    uint8_t col = b.muted ? P_MUTED : P_TEXT;
    int role = b.heading ? b.heading : R_BODY;
    switch (b.kind) {
    case BK_TEXT:
        if (L->text) {
            if (!L->codeBgValid) {
                L->codeBg.clear();
                L->kbdBg.clear();
                InlineCodeBackgrounds(L->text, b.textOff, b.runOff, b.runCount, role, L->codeBg, &L->kbdBg);
                L->codeBgValid = true;
            }
            for (auto& r : L->codeBg) g.canvas->FillRoundRect(x + r.left, y + r.top, x + r.right, y + r.bottom, 4.f, P_INLINEBG);
            for (auto& r : L->kbdBg) {  // <kbd>: a key cap
                g.canvas->FillRoundRect(x + r.left, y + r.top, x + r.right, y + r.bottom + 1, 4.f, P_PANEL);
                g.canvas->StrokeRoundRect(x + r.left, y + r.top, x + r.right, y + r.bottom + 1, 4.f, 1.f, P_BORDER);
            }
            DrawHighlights(L->text, b.textOff, b.textLen, x, y);
            g.canvas->Text(L->text, x, y, col);
            DrawLinkFocus(L->text, b.textOff, b.textLen, x, y);
        }
        if (b.heading == 1 || b.heading == 2) g.canvas->FillRect(x, y + L->height - 1, right, y + L->height, P_BORDER);
        if (b.details & 0x8000) {  // <summary>: a triangle that shows whether the block is folded (drawn, not a glyph)
            uint32_t gi = (uint32_t)(b.details & 0x7FFF) - 1;
            bool open = gi < g.doc.detailsOpen.size() && g.doc.detailsOpen[gi];
            float cx = x - 14.f, cy = y + g.typo.baseline[role] - 5.f, s = 4.f;
            if (open) {
                g.canvas->Line(cx - s, cy - 1.f, cx, cy + s - 1.f, 1.6f, P_MUTED);
                g.canvas->Line(cx, cy + s - 1.f, cx + s, cy - 1.f, 1.6f, P_MUTED);
            } else {
                g.canvas->Line(cx - 1.f, cy - s, cx + s - 1.f, cy, 1.6f, P_MUTED);
                g.canvas->Line(cx + s - 1.f, cy, cx - 1.f, cy + s, 1.6f, P_MUTED);
            }
        }
        // pointing at a heading offers its own link, in the column's left padding (a task box keeps that place)
        if (b.heading && (int)i == g.hoverHeading && !g.firstFrame && !IsTask(b))
            DrawIcon(0xE71B, x - kAnchorGap, y + g.typo.baseline[role] - 16.f, 20.f, 12.f, P_MUTED);  // Segoe Fluent: Link
        break;
    case BK_CODE: {
        g.canvas->FillRoundRect(x, y, right, y + L->height, 6.f, P_CODEBG);
        if (L->label && (int)i != g.hoverCode) {  // the language in the corner; the copy button takes that spot on hover
            DWRITE_TEXT_METRICS lm{};
            L->label->GetMetrics(&lm);
            g.canvas->Text(L->label, right - std::ceil(lm.widthIncludingTrailingWhitespace) - 10.f, y + 6.f, P_MUTED);
        }
        if (L->text) {
            float tx = x + Metrics::kCodePad - HScrollOf(i), ty = y + Metrics::kCodePad;
            g.canvas->PushClip(x, y, right, y + L->height);
            DrawHighlights(L->text, b.textOff, b.textLen, tx, ty);
            g.canvas->Text(L->text, tx, ty, P_TEXT);
            g.canvas->PopClip();
        }
        if ((int)i == g.hoverCode) {  // copy button
            float bs = 30.f, bx = right - bs - 8.f, by = y + 8.f;
            g.canvas->FillRoundRect(bx, by, bx + bs, by + bs, 6.f, g.hoverCopyBtn ? P_BORDER : P_BG);
            g.canvas->StrokeRoundRect(bx, by, bx + bs, by + bs, 6.f, 1.f, P_BORDER);
            static const wchar_t kCopy = 0xE8C8;  // Segoe Fluent Icons: Copy
            IDWriteTextLayout* il = nullptr;
            if (SUCCEEDED(g.dwf->CreateTextLayout(&kCopy, 1, g.typo.uiIcon, bs, bs, &il))) {
                il->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                il->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                il->SetFontSize(14.f, DWRITE_TEXT_RANGE{0, 1});
                g.canvas->Text(il, bx, by, P_MUTED);
                il->Release();
            }
        }
        DrawHScrollBar(i);
        break;
    }
    case BK_HR: g.canvas->FillRect(x, y, right, y + 4, P_BORDER); break;
    case BK_TABLE:
        DrawTable(i, b, L, x, y);
        DrawHScrollBar(i);
        break;
    case BK_IMAGE: {
        Image& im = g.doc.images[b.aux];
        float iw = L->natural;  // BlockBox has already placed the box for <p align=…>
        bool wide = iw > w + 0.5f;  // a diagram too wide for the column: draw it whole and let the block scroll
        float ix = wide ? x - HScrollOf(i) : x;
        if (wide) g.canvas->PushClip(x, y, right, y + L->height);
        if (im.state == RS_OK) {
            g.canvas->DrawImage(im, ix, y, ix + iw, y + L->height);
            // a source that no longer renders keeps its last good picture, outlined in the caution colour (§2.10)
            if (im.renderFailed) g.canvas->StrokeRoundRect(ix - 1.f, y - 1.f, ix + iw + 1.f, y + L->height + 1.f, 2.f, 1.f, P_ALERT_CAUTION);
        } else {
            g.canvas->FillRoundRect(ix, y, ix + iw, y + L->height, 6.f, P_PLACEHOLDER);
            if (L->text && im.w <= 0) g.canvas->Text(L->text, ix + 12.f, y + 9.f, P_MUTED);
            // a formula or diagram that cannot be drawn shows its source instead of a blank box (§9.4)
            IDWriteTextLayout* s = nullptr;
            if (im.state == RS_FAILED && im.mathKind >= 2 && !im.alt.empty() &&
                SUCCEEDED(g.dwf->CreateTextLayout(im.alt.data(), (UINT32)im.alt.size(), g.typo.fmt[R_CODE], iw - 24.f, 1e6f, &s))) {
                g.canvas->PushClip(ix, y, ix + iw, y + L->height);
                g.canvas->Text(s, ix + 12.f, y + 9.f, P_MUTED);
                g.canvas->PopClip();
                s->Release();
            }
        }
        if (wide) {
            g.canvas->PopClip();
            DrawHScrollBar(i);
        }
        break;
    }
    }
    if (b.marker) DrawMarker(b, x, y + MarkerBaseline(b), (int)i == g.hoverTask);
}

IDWriteTextLayout* UiLayout(const std::wstring& s, float maxW, IDWriteTextFormat* fmt) {
    IDWriteTextLayout* L = nullptr;
    g.dwf->CreateTextLayout(s.data(), (UINT32)s.size(), fmt ? fmt : g.typo.ui, maxW, 100.f, &L);
    return L;
}

void DrawIcon(wchar_t icon, float l, float t, float box, float size, uint8_t pal) {
    IDWriteTextLayout* L = nullptr;
    if (FAILED(g.dwf->CreateTextLayout(&icon, 1, g.typo.uiIcon, box, box, &L))) return;
    L->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    L->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    L->SetFontSize(size, DWRITE_TEXT_RANGE{0, 1});
    g.canvas->Text(L, l, t, pal);
    L->Release();
}

float DrawPill(const std::wstring& s, float cx, float y, bool centered, bool measure) {
    IDWriteTextLayout* L = UiLayout(s, std::max(100.f, ViewW() - 48.f));
    if (!L) return 0.f;
    DWRITE_TEXT_METRICS m{};
    L->GetMetrics(&m);
    float w = std::ceil(m.width) + 24.f, h = 30.f;
    float x = centered ? std::floor(cx - w * 0.5f) : cx;
    if (!measure) {
        g.canvas->FillRoundRect(x, y, x + w, y + h, 8.f, P_OVERLAY_BG);
        g.canvas->StrokeRoundRect(x, y, x + w, y + h, 8.f, 1.f, P_OVERLAY_BORDER);
        g.canvas->Text(L, x + 12.f, y + (h - m.height) * 0.5f, P_OVERLAY_TEXT);
    }
    L->Release();
    return w;
}

static void DrawScrollbar() {
    float vh = ViewH(), vw = ViewW();
    if (g.docH <= vh + 1) return;
    float trackT = ScrollTrackTop(), trackB = vh - 2, trackH = trackB - trackT;
    float th = std::max(32.f, trackH * vh / g.docH);
    float ty = trackT + (trackH - th) * (g.scrollY / MaxScroll());
    if (g.findOpen && !g.matches.empty()) DrawFindMarks(vw - 12.f, vw - 2.f);
    bool hot = g.draggingThumb || g.hotScroll;
    float w = hot ? 8.f : 5.f;
    g.canvas->FillRoundRect(vw - w - 3, ty, vw - 3, ty + th, w * 0.5f, hot ? P_SCROLL_HOT : P_SCROLL);
}

// ------------------------------------------------------------------------------------------------ the edit caret
// the width of a blank in a block's font: the caret stands that far right per column of trailing blanks (§6.5)
float SpaceAdvance(uint32_t bi) {
    const Block& b = g.doc.blocks[bi];
    int role = b.kind == BK_CODE ? R_CODE : b.heading ? b.heading : R_BODY;
    IDWriteTextLayout* L = nullptr;
    if (FAILED(g.dwf->CreateTextLayout(L" ", 1, g.typo.fmt[role], 100.f, 100.f, &L))) return 4.f;
    DWRITE_TEXT_METRICS m{};
    L->GetMetrics(&m);
    L->Release();
    return m.widthIncludingTrailingWhitespace;
}

// An object atom's box in client DIP - a picture or formula in a line: its character's; a block of its own (an HTML
// block drawn as several: all of them) - when it is laid out (the paint path never lays out)
bool AtomRect(int32_t bi, int32_t image, float box[4]) {
    size_t n = g.doc.blocks.size();
    if (bi < 0 || (size_t)bi >= n || (size_t)bi >= g.cache.size() || g.Y.size() != n || BlockHidden(g.doc.blocks[bi])) return false;
    float l, t, r, b;
    if (image >= 0) {  // a picture or formula in the line: the box of its one character
        const Block& bl = g.doc.blocks[bi];
        uint32_t at = UINT32_MAX;
        int32_t cell = -1;
        auto scan = [&](uint32_t runOff, uint32_t runCount, int32_t c) {
            for (uint32_t k = 0; k < runCount && at == UINT32_MAX; k++) {
                const Run& run = g.doc.runs[runOff + k];
                if ((run.flags & F_IMAGE) && run.image == (uint32_t)image) { at = run.start; cell = c; }
            }
        };
        scan(bl.runOff, bl.runCount, -1);
        if (bl.kind == BK_TABLE && bl.aux < g.doc.tables.size()) {
            const Table& tb = g.doc.tables[bl.aux];
            for (uint32_t c = 0; c < tb.rows * tb.cols && at == UINT32_MAX; c++)
                scan(g.doc.cells[tb.cellOff + c].runOff, g.doc.cells[tb.cellOff + c].runCount, (int32_t)c);
        }
        float x0, y0, h0, x1, y1, h1;
        if (at == UINT32_MAX || !g.cache[bi] || !CaretGeomAt(at, bi, cell, &x0, &y0, &h0, false) ||
            !CaretGeomAt(at + 1, bi, cell, &x1, &y1, &h1, false))
            return false;
        l = x0;
        r = std::max(x1, x0 + 4.f);
        t = y0 - g.scrollY;
        b = t + h0;
    } else {  // a block of its own; an HTML block or front matter may have been drawn as several
        int32_t last = bi;
        if (g.doc.blockSrc.size() == n && g.doc.blockSrc[bi].rawId >= 0)
            while ((size_t)last + 1 < n && g.doc.blockSrc[last + 1].rawId == g.doc.blockSrc[bi].rawId) last++;
        float x, w;
        BlockBox(bi, &x, &w);
        const Block& bl = g.doc.blocks[bi];
        if (bl.kind == BK_IMAGE && g.cache[bi]) w = std::min(w, g.cache[bi]->natural);
        l = x;
        r = x + w;
        t = g.Y[bi] - g.scrollY;
        b = g.Y[last] + g.H[last] - g.scrollY;
    }
    box[0] = l;
    box[1] = t;
    box[2] = r;
    box[3] = b;
    return true;
}

// the selected object atom: a 1 px accent outline 2 px outside its box (§12.2), no caret
static void DrawAtomOutline(float top, float bottom) {
    float r[4];
    if (!AtomRect(g.selAtomBlock, g.selAtomImage, r) || r[3] + 3.f < top || r[1] - 3.f > bottom) return;
    // (a picture that no longer renders has its own outline 1 px out: this one goes a pixel further, not over it)
    const Doc& d = g.doc;
    int32_t ii = g.selAtomImage;
    if (ii < 0 && (size_t)g.selAtomBlock < d.blocks.size() && d.blocks[g.selAtomBlock].kind == BK_IMAGE) ii = (int32_t)d.blocks[g.selAtomBlock].aux;
    const float o = ii >= 0 && (size_t)ii < d.images.size() && d.images[ii].renderFailed ? 3.f : 2.f;
    g.canvas->StrokeRoundRect(r[0] - o, r[1] - o, r[2] + o, r[3] + o, 2.f, 1.f, P_ACCENT);
}

// A styled phantom row (§6.7) shows what it will be: a list's marker (a bullet, `1.`, an empty box) left of where the
// caret stands, a quote's bar, or «Заголовок 2» in the muted colour at that heading's size
static void DrawPhantomStyle(float top, float bottom) {
    const uint8_t st = g.phantomStyle;
    if (!st || g.phantomBlock < 0 || g.phantomBreak) return;
    const float y = g.phantomY - g.scrollY, x = g.phantomX;
    if (y + g.phantomLine <= top || y >= bottom) return;
    if (st <= 6) {
        wchar_t b[64];
        swprintf_s(b, Tr(S_ED_STYLE_HEADING_FMT), (int)st);
        IDWriteTextLayout* L = nullptr;
        if (SUCCEEDED(g.dwf->CreateTextLayout(b, (UINT32)wcslen(b), g.typo.fmt[st], 2000.f, 400.f, &L)) && L) {
            L->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            g.canvas->Text(L, x, y, P_MUTED);
            L->Release();
        }
    } else if (st <= 9) {
        Block m{};
        m.marker = st == 7 ? MK_BULLET : st == 8 ? MK_NUMBER : MK_TASK_OPEN;
        m.number = 1;
        m.listLevel = 1;
        DrawMarker(m, x, y + g.typo.baseline[R_BODY], false);
    } else {
        g.canvas->FillRect(x - 20.f, y, x - 20.f + Metrics::kQuoteBar, y + g.phantomLine, P_BORDER);
    }
}

// The caret of edit mode (§12.2): the width Windows asks for, only while the blink phase is on and the document has
// the keyboard (edit.cpp decides), in a block that is laid out already - the paint path never lays out - and clipped to
// the block's box inside the document column.
static void DrawEditCaret(float top, float bottom) {
    if (g.selAtomBlock >= 0) { DrawAtomOutline(top, bottom); return; }
    int32_t bi = g.caretBlock;
    if (!g.caretOn || !g.caretVisible || g.fitWide) return;
    UINT sysW = 1;
    SystemParametersInfoW(SPI_GETCARETWIDTH, 0, &sysW, 0);
    float w = std::max(2.f, sysW * g.dpi / 96.f) / Scale();
    if (g.phantomCaret && g.phantomBlock >= 0) {  // in the phantom row: at the content edge of its level (§6.7)
        float ct = g.phantomY - g.scrollY, cb = ct + g.phantomLine;
        if (cb > top && ct < bottom) g.canvas->FillRect(g.phantomX, ct, g.phantomX + w, cb, P_TEXT);
        return;
    }
    if (bi < 0 || (size_t)bi >= g.cache.size() || !g.cache[bi]) return;
    float cx, dy, ch;
    if (!CaretGeomAt(g.selFocus, bi, g.caretCell, &cx, &dy, &ch, false)) return;
    float bx, bw;
    BlockBox(bi, &bx, &bw);
    float right = std::min(bx + bw, ViewW() - Metrics::kPadX) - w;
    if (g.caretTrail) cx = std::min(cx + g.caretTrail * SpaceAdvance((uint32_t)bi), std::max(cx, right));
    float ct = dy - g.scrollY, cb = ct + ch;
    if (cb <= top || ct >= bottom || cx < std::max(bx, DocLeft()) - w || cx > right + w) return;
    g.canvas->FillRect(cx, ct, cx + w, cb, P_TEXT);
}

// the document itself: the blocks of a band of the window and the quote bars beside them
static void DrawDocumentBand(float top, float bottom) {
    float vh = ViewH();
    top = std::max(0.f, top);
    bottom = std::min(vh, bottom);
    if (bottom <= top) return;
    size_t n = g.doc.blocks.size();
    for (uint32_t i = FirstVisible(g.scrollY + top); i < n; i++) {
        float y = g.Y[i] - g.scrollY;
        if (y >= bottom) break;
        DrawBlock(i, y);
    }
    float tl = TextLeft();
    for (const QuoteSpan& q : g.doc.quotes) {
        if (q.first == UINT32_MAX) continue;
        float t = g.Y[q.first] - g.scrollY, b = g.Y[q.last] + g.H[q.last] - g.scrollY;
        if (b < top || t > bottom) continue;
        uint8_t pal = q.alert ? (uint8_t)(P_ALERT_NOTE + q.alert - 1) : P_BORDER;
        g.canvas->FillRect(tl + q.x, t, tl + q.x + Metrics::kQuoteBar, b, pal);
    }
    if (g.editing) {
        DrawPhantomStyle(top, bottom);
        DrawEditCaret(top, bottom);
    } else if (g.caretOn) {  // the blocks around it are laid out already: no need to measure anything here
        float cx, dy, ch;
        if (CaretGeom(g.selFocus, &cx, &dy, &ch, false)) {
            float ct = dy - g.scrollY, cb = ct + ch;
            float w = 2.f / Scale();  // two device pixels, so it stays visible at any scale
            if (cb > top && ct < bottom) g.canvas->FillRect(cx, ct, cx + w, cb, P_TEXT);
        }
    }
}

// One page of the printed document (print.cpp): the same blocks, moved so that docTop lands at the top of the page.
// The canvas is the printer's and the caller has clipped the page area.
void DrawDocumentPage(float docTop, float docBottom) {
    size_t n = g.doc.blocks.size();
    float saved = g.scrollY;
    g.scrollY = docTop;
    for (uint32_t i = FirstVisible(docTop); i < n; i++) {
        if (g.Y[i] >= docBottom) break;
        if (BlockHidden(g.doc.blocks[i])) continue;
        DrawBlock(i, g.Y[i] - docTop);
    }
    float tl = TextLeft();
    for (const QuoteSpan& q : g.doc.quotes) {
        if (q.first == UINT32_MAX) continue;
        float t = g.Y[q.first] - docTop, b = g.Y[q.last] + g.H[q.last] - docTop;
        if (b < 0 || t > docBottom - docTop) continue;
        uint8_t pal = q.alert ? (uint8_t)(P_ALERT_NOTE + q.alert - 1) : P_BORDER;
        g.canvas->FillRect(tl + q.x, t, tl + q.x + Metrics::kQuoteBar, b, pal);
    }
    g.scrollY = saved;
}

// Where a page may end inside a block taller than the page: after the last line of text (or the last table row) that
// still fits in `limit`. 0 = nowhere, the caller cuts at the page edge.
float BlockSplitY(uint32_t i, float limit) {
    if (i >= g.doc.blocks.size() || limit <= 0) return 0.f;
    const Block& b = g.doc.blocks[i];
    BlockLayout* L = EnsureLayout(i);
    if (!L) return 0.f;
    if ((b.kind == BK_TEXT || b.kind == BK_CODE) && L->text) {
        float pad = b.kind == BK_CODE ? Metrics::kCodePad : 0.f;
        UINT32 n = 0;
        L->text->GetLineMetrics(nullptr, 0, &n);
        if (!n) return 0.f;
        std::vector<DWRITE_LINE_METRICS> lm(n);
        if (FAILED(L->text->GetLineMetrics(lm.data(), n, &n))) return 0.f;
        float y = pad, best = 0;
        for (UINT32 k = 0; k + 1 < n; k++) {  // never leave the last line alone on the next page
            if (y + lm[k].height > limit) break;
            y += lm[k].height;
            best = y;
        }
        return best > 0 && best < g.H[i] ? best : 0.f;
    }
    if (b.kind == BK_TABLE && L->table) {
        const Table& t = g.doc.tables[b.aux];
        float y = 0, best = 0;
        for (uint32_t r = 0; r + 1 < t.rows; r++) {
            if (y + L->table->rowH[r] > limit) break;
            y += L->table->rowH[r];
            best = y;
        }
        return best > 0 && best < g.H[i] ? best : 0.f;
    }
    return 0.f;
}

// a rectangle of the document redrawn from scratch (background included), for a strip that scrolled into view or a
// place where something that stays put used to be
static void RedrawDocRect(float l, float t, float r, float b) {
    if (b <= t || r <= l) return;
    g.canvas->PushClip(l, t, r, b);
    g.canvas->FillRect(l, t, r, b, P_BG);
    DrawDocumentBand(t, b);
    g.canvas->PopClip();
}

// what stays in place while the document scrolls
static void DrawChrome() {
    if (!g.path.empty()) DrawScrollbar();
    DrawSettingsButton();  // under the outline drawer: in a narrow window the drawer may cover it (and takes the click)
    DrawEditChrome(0);     // edit mode's bar, a strip over the top of the document (it covers the corners), the pencil
    DrawToc();             // the drawer over them all; docked, the panel is beside them
    if (g.findOpen) DrawFindBar();
    const std::wstring* pill = nullptr;
    std::wstring linkTip;
    if (!g.tip.empty()) pill = &g.tip;
    else if (g.hoverLink >= 0 && (size_t)g.hoverLink < g.doc.links.size() && !g.selecting) {
        pill = &g.doc.links[g.hoverLink];
        if (g.editing) {  // a click there places the caret: say how the link opens, then where it goes (UX-19)
            linkTip = Tr(S_ED_LINK_TIP) + (L"  ·  " + *pill);
            pill = &linkTip;
        }
    } else if (g.focusLink >= 0 && (size_t)g.focusLink < g.doc.links.size()) pill = &g.doc.links[g.focusLink];
    float pillR = -1.f;
    if (pill) {
        float x = g.path.empty() ? 10.f : DocLeft() + 10.f;
        pillR = x + DrawPill(*pill, x, ViewH() - 40.f, false);
    }
    if (!g.toast.empty() && GetTickCount() < g.toastUntil) {
        // a long pill at the bottom left (a link's in edit mode) reaches under the toast: the toast goes a row up
        float cx = DocLeft() + DocW() * 0.5f, y = ViewH() - 64.f;
        if (pillR > 0.f && pillR > cx - DrawPill(g.toast, cx, y, true, true) * 0.5f) y -= 30.f;
        DrawPill(g.toast, cx, y, true);
    }
    DrawEditChrome(1);     // a toolbar button's tooltip, over everything
}

// ------------------------------------------------------------------------------------------------ scrolled frames
// Scrolling repeats the same picture moved by a few pixels, so redrawing all of it costs more than it should. The
// canvas moves its window inside a taller buffer for free, and only the strip that came into view is drawn. That is
// allowed when *nothing except* the scroll position changed — this key is everything a frame is drawn from, and it is
// compared with the previous frame's.
namespace {
struct FrameKey {
    uint32_t docSerial = 0, gen = 0, pixelSerial = 0, hxSerial = 0;
    float scrollY = 0, textW = 0, wideW = 0, docH = 0, viewW = 0, viewH = 0, scale = 0, docLeft = 0;
    uint32_t selA = 0, selB = 0, caret = 0;
    int hoverLink = 0, hoverCode = 0, hoverHBlock = 0, hbarFlash = 0, focusLink = 0, dragHBlock = 0, tocHover = 0,
        curMatch = 0, findHot = 0, recentHover = 0, hoverHeading = 0, hoverTask = 0;
    size_t matches = 0;
    bool hoverCopyBtn = false, hotHBar = false, hotScroll = false, dark = false, selecting = false, tocBtnHot = false,
         settingsBtnHot = false, draggingThumb = false, home = false, overText = false;
    // edit mode (§12.3): the bar's slide and the state of everything it draws, the caret, the selected atom
    bool editing = false, caretVisible = false, pencilHot = false;
    int barT = 0;
    uint32_t chrome = 0, editSerial = 0;
    int32_t caretBlock = -1, caretCell = -1, atomBlock = -1, atomImage = -1, phantomBlock = -1;
    uint16_t caretTrail = 0;
    int8_t caretAff = 0;
    float stripH = 0, phantomY = 0;
    bool phantomCaret = false;
    uint8_t phantomStyle = 0;
    bool operator==(const FrameKey&) const = default;
};
FrameKey g_last;
bool g_lastValid = false;

FrameKey CurrentKey() {
    FrameKey k;
    k.docSerial = g.docSerial;
    k.gen = g.gen.load();
    k.pixelSerial = g.pixelSerial;
    k.hxSerial = g.hxSerial;
    k.scrollY = g.scrollY;
    k.textW = g.textW;
    k.wideW = g.wideW;
    k.docH = g.docH;
    k.viewW = ViewW();
    k.viewH = ViewH();
    k.scale = Scale();
    k.docLeft = DocLeft();
    k.selA = g.selAnchor;
    k.selB = g.selFocus;
    k.caret = g.caretOn ? g.selFocus + 1 : 0;
    k.hoverLink = g.hoverLink;
    k.hoverCode = g.hoverCode;
    k.hoverHBlock = g.hoverHBlock;
    k.hbarFlash = (g.hbarFlash >= 0 && GetTickCount() < g.hbarFlashUntil) ? g.hbarFlash : -1;
    k.focusLink = g.focusLink;
    k.dragHBlock = g.dragHBlock;
    k.tocHover = g.tocHover;
    k.curMatch = g.curMatch;
    k.findHot = g.findHot;
    k.recentHover = g.recentHover;
    k.hoverHeading = g.hoverHeading;
    k.hoverTask = g.hoverTask;
    k.matches = g.matches.size();
    k.hoverCopyBtn = g.hoverCopyBtn;
    k.hotHBar = g.hotHBar;
    k.hotScroll = g.hotScroll;
    k.dark = PaletteIsDark();
    k.selecting = g.selecting;
    k.tocBtnHot = g.tocBtnHot;
    k.settingsBtnHot = g.settingsBtnHot;
    k.draggingThumb = g.draggingThumb;
    k.home = g.path.empty();
    k.editing = g.editing;
    k.caretVisible = g.caretVisible;
    k.pencilHot = g.pencilHot;
    k.barT = (int)std::lround(g.barT * 256);
    k.chrome = g.editChrome;
    k.editSerial = g.editSerial;
    k.caretBlock = g.caretBlock;
    k.caretCell = g.caretCell;
    k.caretTrail = g.caretTrail;
    k.caretAff = g.caretAff;
    k.atomBlock = g.selAtomBlock;
    k.atomImage = g.selAtomImage;
    k.stripH = g.stripH;
    k.phantomBlock = g.phantomBlock;
    k.phantomY = g.phantomY;
    k.phantomCaret = g.phantomCaret;
    k.phantomStyle = g.phantomStyle;
    // things drawn over the text: they would have to be repaired pixel by pixel, so those frames are drawn in full
    bool pill = !g.tip.empty() || (g.hoverLink >= 0 && !g.selecting) || g.focusLink >= 0;
    bool toast = !g.toast.empty() && GetTickCount() < g.toastUntil;
    k.overText = g.findOpen || TocOverlayOpen() || pill || toast || g.editOverText;
    return k;
}

// returns true when the frame was produced by moving the window and redrawing the strip that came into view
bool ScrollFrame(const FrameKey& k) {
    if (!g_lastValid || k.home || k.overText || g.firstFrame) return false;
    FrameKey prev = g_last;
    if (k.scrollY == prev.scrollY) return false;
    float dy = k.scrollY - prev.scrollY;
    prev.scrollY = k.scrollY;
    if (!(k == prev)) return false;
    int dpx = (int)std::lround(dy * k.scale);
    if (dpx == 0 || !g.canvas->ScrollViewport(dpx)) return false;
    float vh = ViewH(), vw = ViewW(), dl = DocLeft(), d = dpx / k.scale;
    if (dpx > 0) RedrawDocRect(dl, vh - d, vw, vh);  // scrolled down: the strip came in at the bottom
    else RedrawDocRect(dl, 0, vw, -d);
    // the chrome of the previous frame moved with the document: put back what its pixels covered, then draw it again
    g.canvas->FillRect(vw - Metrics::kPadX, 0, vw, vh, P_BG);  // the scrollbar's column: the layout keeps text out of it
    float l, t, r, b;
    if (SettingsButtonRect(&l, &t, &r, &b)) RedrawDocRect(l, std::min(t, t - d), r, std::max(b, b - d));
    if (TocButtonRect(&l, &t, &r, &b)) RedrawDocRect(l, std::min(t, t - d), r, std::max(b, b - d));
    float rc[4][4];  // edit mode's bar, strip and pencil stay put too
    for (int k = 0, n = EditChromeRects(rc, 4); k < n; k++)
        RedrawDocRect(rc[k][0], std::min(rc[k][1], rc[k][1] - d), rc[k][2], std::max(rc[k][3], rc[k][3] - d));
    DrawChrome();  // the outline panel is opaque and repaints itself
    return true;
}
}  // namespace

void ForceFullRedraw() { g_lastValid = false; }

void Render() {
    TrimCache();
    EnsureVisible();
    float s = Scale();  // whole device pixels: a scrolled frame moves the picture by an exact number of them, so the
    if (s > 0) g.scrollY = std::round(g.scrollY * s) / s;  // text keeps the same pixel grid it was drawn on
    FrameKey k = CurrentKey();
    k.scrollY = g.scrollY;
    g.canvas->Begin();
    if (ScrollFrame(k)) {
        g.framesPartial++;
    } else {
        g.framesFull++;
        g.canvas->Clear(P_BG);
        if (g.path.empty()) DrawHome();
        else DrawDocumentBand(0, ViewH());
        DrawChrome();
    }
    g.canvas->End();
    g_last = k;
    g_lastValid = true;
}

void ShowToast(const std::wstring& text, DWORD ms) {
    g.toast = text;
    g.toastUntil = GetTickCount() + ms;
    if (g.hwnd) SetTimer(g.hwnd, TIMER_TOAST, ms + 20, nullptr);
    Invalidate();
}

// ------------------------------------------------------------------------------------------------ hit-testing
static uint32_t BlockEnd(const Block& b) { return b.textOff + b.textLen; }

static uint32_t HitLayout(IDWriteTextLayout* tl, uint32_t textOff, float lx, float ly, DocHit* h) {
    BOOL trailing = FALSE, in = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(tl->HitTestPoint(lx, ly, &trailing, &in, &m))) return textOff;
    h->inside = in != FALSE;
    h->under = textOff + m.textPosition;
    return textOff + m.textPosition + (trailing ? m.length : 0);
}

bool HitTestDocAt(float px, float py, DocHit* h) {
    *h = DocHit();
    size_t n = g.doc.blocks.size();
    if (!n) return false;
    float docY = py + g.scrollY;
    uint32_t i = FirstVisible(docY);
    if (i >= n) {
        h->block = (int32_t)n - 1;
        h->pos = BlockEnd(g.doc.blocks[n - 1]);
        return true;
    }
    const Block& b = g.doc.blocks[i];
    h->block = (int32_t)i;
    if (docY < g.Y[i]) { h->pos = b.textOff; h->above = true; return true; }
    BlockLayout* L = EnsureLayout(i);
    float bx, bw;
    BlockBox(i, &bx, &bw);
    float ly = docY - g.Y[i];
    switch (b.kind) {
    case BK_TEXT:
        h->pos = L->text ? HitLayout(L->text, b.textOff, px - bx, ly, h) : b.textOff;
        return true;
    case BK_CODE:
        h->pos = L->text ? HitLayout(L->text, b.textOff, px - bx - Metrics::kCodePad + HScrollOf(i), ly - Metrics::kCodePad, h)
                         : b.textOff;
        return true;
    case BK_TABLE: {
        const Table& t = g.doc.tables[b.aux];
        float yy = 0;
        for (uint32_t r = 0; r < t.rows; r++) {
            float rh = L->table->rowH[r];
            if (ly < yy + rh || r + 1 == t.rows) {
                float xx = bx - HScrollOf(i);
                for (uint32_t c = 0; c < t.cols; c++) {
                    float cw = L->table->colW[c];
                    if (px < xx + cw || c + 1 == t.cols) {
                        const Cell& cell = g.doc.cells[t.cellOff + r * t.cols + c];
                        IDWriteTextLayout* cl = L->table->cells[r * t.cols + c];
                        h->cell = (int32_t)(r * t.cols + c);
                        h->pos = cl ? HitLayout(cl, cell.textOff, px - xx - 1 - Metrics::kCellPadX,
                                                ly - yy - 1 - Metrics::kCellPadY, h)
                                    : cell.textOff;
                        return true;
                    }
                    xx += cw;
                }
            }
            yy += rh;
        }
        h->pos = b.textOff;
        return true;
    }
    default:
        h->pos = (px < bx + bw * 0.5f) ? b.textOff : BlockEnd(b);
        h->inside = px >= bx && px <= bx + bw;
        return true;
    }
}

bool HitTestDoc(float px, float py, uint32_t* pos, bool* inside) {
    DocHit h;
    bool ok = HitTestDocAt(px, py, &h);
    *pos = h.pos;
    if (inside) *inside = h.inside && h.block >= 0 && g.doc.blocks[h.block].kind != BK_HR && g.doc.blocks[h.block].kind != BK_IMAGE;
    return ok;
}

static int LinkOfRuns(uint32_t p, uint32_t runOff, uint32_t runCount) {
    for (uint32_t k = 0; k < runCount; k++) {
        const Run& r = g.doc.runs[runOff + k];
        if ((r.flags & F_LINK) && p >= r.start && p < r.start + r.len) return (int)r.link;
    }
    return -1;
}

int LinkAt(float px, float py) {
    DocHit h;
    if (g.path.empty() || !HitTestDocAt(px, py, &h) || !h.inside || h.block < 0 || h.under == UINT32_MAX) return -1;
    // the character HitTestPoint found under the point, in the block and cell under it (pos is past it after a trailing
    // hit, and at a block's left edge pos - 1 would be the end of the block above, a link there not under the point)
    const Block& b = g.doc.blocks[h.block];
    if (b.kind == BK_TEXT) return LinkOfRuns(h.under, b.runOff, b.runCount);
    if (b.kind == BK_TABLE && h.cell >= 0) {
        const Cell& cell = g.doc.cells[g.doc.tables[b.aux].cellOff + h.cell];
        return LinkOfRuns(h.under, cell.runOff, cell.runCount);
    }
    return -1;
}

int ImageAt(float px, float py) {
    size_t n = g.doc.blocks.size();
    if (g.path.empty() || !n) return -1;
    float docY = py + g.scrollY;
    uint32_t i = FirstVisible(docY);
    if (i >= n || g.Y[i] > docY || g.doc.blocks[i].kind != BK_IMAGE || !g.cache[i]) return -1;
    float x, w;
    BlockBox(i, &x, &w);
    return (px >= x && px <= x + g.cache[i]->natural) ? (int)i : -1;
}

int CodeBlockAt(float px, float py, bool* onCopyButton) {
    *onCopyButton = false;
    if (g.path.empty() || g.doc.blocks.empty()) return -1;
    float docY = py + g.scrollY;
    uint32_t i = FirstVisible(docY);
    if (i >= g.doc.blocks.size() || g.Y[i] > docY || g.doc.blocks[i].kind != BK_CODE || !g.cache[i]) return -1;
    float x, w;
    BlockBox(i, &x, &w);
    if (px < x || px > x + w) return -1;
    float bs = 30.f, bx = x + w - bs - 8.f, by = g.Y[i] + 8.f;
    *onCopyButton = px >= bx && px <= bx + bs && docY >= by && docY <= by + bs;
    return (int)i;
}

// ------------------------------------------------------------------------------------------------ selection
void SelectAll() {
    if (g.doc.blocks.empty()) return;
    g.selAnchor = g.doc.blocks.front().textOff;
    g.selFocus = (uint32_t)g.doc.text.size();
}

static bool IsWordChar(wchar_t c) {
    WORD t = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &t);
    return (t & (C1_ALPHA | C1_DIGIT)) || c == L'_' || (c >= 0xD800 && c <= 0xDFFF);
}

// text range that contains pos: a block's text or a table cell
static void ContainerRange(uint32_t pos, uint32_t* s, uint32_t* e) {
    uint32_t bi = BlockOfPos(pos);
    const Block& b = g.doc.blocks[bi];
    *s = b.textOff;
    *e = BlockEnd(b);
    if (b.kind == BK_TABLE) {
        const Table& t = g.doc.tables[b.aux];
        for (uint32_t c = 0; c < t.rows * t.cols; c++) {
            const Cell& cell = g.doc.cells[t.cellOff + c];
            if (pos >= cell.textOff && pos <= cell.textOff + cell.textLen) { *s = cell.textOff; *e = cell.textOff + cell.textLen; return; }
        }
    }
}

// the run of like characters around pos: a word, a run of spaces, or a run of punctuation
bool WordRange(uint32_t pos, uint32_t* from, uint32_t* to) {
    uint32_t s, e;
    if (g.doc.blocks.empty()) return false;
    ContainerRange(pos, &s, &e);
    const std::wstring& t = g.doc.text;
    if (pos >= e && pos > s) pos = e - 1;
    if (pos >= t.size()) return false;
    bool word = IsWordChar(t[pos]);
    bool space = iswspace(t[pos]) != 0;
    uint32_t a = pos, b = pos;
    auto same = [&](wchar_t c) { return word ? IsWordChar(c) : space ? iswspace(c) != 0 : (!IsWordChar(c) && !iswspace(c)); };
    while (a > s && same(t[a - 1])) a--;
    while (b < e && same(t[b])) b++;
    *from = a;
    *to = b;
    return true;
}

void SelectWordAt(uint32_t pos) {
    uint32_t a = 0, b = 0;
    if (!WordRange(pos, &a, &b)) return;
    g.selAnchor = a;
    g.selFocus = b;
}

// the block (paragraph, heading, list item, code block) that holds pos
bool ParagraphRange(uint32_t pos, uint32_t* from, uint32_t* to) {
    if (g.doc.blocks.empty()) return false;
    ContainerRange(pos, from, to);
    return true;
}

// the visual line that holds pos, as DirectWrite wrapped it
bool LineRange(uint32_t pos, uint32_t* from, uint32_t* to) {
    if (g.doc.blocks.empty()) return false;
    uint32_t bi = BlockOfPos(pos);
    const Block& b = g.doc.blocks[bi];
    uint32_t s = b.textOff, e = BlockEnd(b);
    *from = s;
    *to = e;
    if (b.kind != BK_TEXT && b.kind != BK_CODE) return true;
    BlockLayout* L = EnsureLayout(bi);
    if (!L || !L->text) return true;
    UINT32 n = 0;
    L->text->GetLineMetrics(nullptr, 0, &n);
    if (!n) return true;
    std::vector<DWRITE_LINE_METRICS> lm(n);
    if (FAILED(L->text->GetLineMetrics(lm.data(), n, &n))) return true;
    uint32_t at = s;
    for (UINT32 k = 0; k < n; k++) {
        uint32_t next = at + lm[k].length;
        if (pos < next || k + 1 == n) {
            *from = at;
            *to = std::min(next, e);
            return true;
        }
        at = next;
    }
    return true;
}

// one character forward or back, never stopping inside a surrogate pair; stays inside the document
uint32_t TextStep(uint32_t pos, int dir) {
    const std::wstring& t = g.doc.text;
    if (dir > 0) {
        uint32_t p = std::min<uint32_t>(pos + 1, (uint32_t)t.size());
        while (p < t.size() && t[p] >= 0xDC00 && t[p] <= 0xDFFF) p++;
        return p;
    }
    if (pos == 0) return 0;
    uint32_t p = pos - 1;
    while (p > 0 && t[p] >= 0xDC00 && t[p] <= 0xDFFF) p--;
    return p;
}

// screen rectangles of a text range, for a screen reader's highlight (client DIP → screen pixels)
void RangeScreenRects(uint32_t from, uint32_t to, std::vector<double>& out) {
    out.clear();
    if (g.doc.blocks.empty() || to <= from || !g.hwnd) return;
    float s = Scale();
    static std::vector<DWRITE_HIT_TEST_METRICS> hits;
    for (uint32_t i = BlockOfPos(from); i < g.doc.blocks.size(); i++) {
        const Block& b = g.doc.blocks[i];
        if (b.textOff >= to) break;
        if (BlockHidden(b) || b.kind == BK_HR || b.kind == BK_IMAGE) continue;
        uint32_t bs = std::max(from, b.textOff), be = std::min(to, BlockEnd(b));
        if (be <= bs) continue;
        BlockLayout* L = EnsureLayout(i);
        if (!L || !L->text) continue;
        float bx, bw;
        BlockBox(i, &bx, &bw);
        float pad = b.kind == BK_CODE ? Metrics::kCodePad : 0.f;
        RangeRects(L->text, b.textOff, bs, be, hits);
        for (const auto& r : hits) {
            POINT p{(LONG)std::lround((bx + pad + r.left) * s),
                    (LONG)std::lround((g.Y[i] + pad + r.top - g.scrollY) * s)};
            ClientToScreen(g.hwnd, &p);
            out.push_back((double)p.x);
            out.push_back((double)p.y);
            out.push_back((double)std::lround(r.width * s));
            out.push_back((double)std::lround(r.height * s));
        }
    }
}

// the heading level of the block holding pos (0 = not a heading): screen readers navigate by these
int HeadingLevelAt(uint32_t pos) {
    if (g.doc.blocks.empty()) return 0;
    return g.doc.blocks[BlockOfPos(pos)].heading;
}

void SelectBlockAt(uint32_t pos) {
    uint32_t s, e;
    ContainerRange(pos, &s, &e);
    g.selAnchor = s;
    g.selFocus = e;
}

// ------------------------------------------------------------------------------------------- keyboard selection 3.3
// Shift+arrows move the caret and the selection follows it; Ctrl+Shift+arrows move by words, Shift+Home/End along the
// visual line, Ctrl+Shift+Home/End to the ends of the document. The caret appears only once the keyboard starts a
// selection — the mouse hides it again.

// client x and document y of the caret at pos, and the height of its line. block / cell: where pos is, when the caller
// knows (an empty block or cell shares its offset with the next one); -1 = found from pos.
bool CaretGeomAt(uint32_t pos, int32_t block, int32_t cellIdx, float* cx, float* docY, float* h, bool relayout) {
    if (g.doc.blocks.empty()) return false;
    uint32_t bi = block >= 0 && (size_t)block < g.doc.blocks.size() ? (uint32_t)block : BlockOfPos(pos);
    const Block& b = g.doc.blocks[bi];
    if (BlockHidden(b)) return false;
    if (!relayout && !g.cache[bi]) return false;  // the paint path never lays out
    BlockLayout* L = EnsureLayout(bi);
    if (relayout) RecomputeY();
    float bx, bw;
    BlockBox(bi, &bx, &bw);
    IDWriteTextLayout* tl = nullptr;
    uint32_t off = b.textOff;
    float lx = bx, ly = 0;
    if ((b.kind == BK_TEXT || b.kind == BK_CODE) && L->text) {
        tl = L->text;
        if (b.kind == BK_CODE) {
            lx += Metrics::kCodePad - HScrollOf(bi);
            ly = Metrics::kCodePad;
        }
    } else if (b.kind == BK_TABLE && L->table) {
        const Table& t = g.doc.tables[b.aux];
        float yy = 0;
        for (uint32_t r = 0; r < t.rows && !tl; r++) {
            float xx = bx - HScrollOf(bi);
            for (uint32_t c = 0; c < t.cols; c++) {
                const Cell& cell = g.doc.cells[t.cellOff + r * t.cols + c];
                bool mine = cellIdx >= 0 ? (int32_t)(r * t.cols + c) == cellIdx
                                         : pos >= cell.textOff && pos <= cell.textOff + cell.textLen;
                if (mine && L->table->cells[r * t.cols + c]) {
                    tl = L->table->cells[r * t.cols + c];
                    off = cell.textOff;
                    lx = xx + 1 + Metrics::kCellPadX;
                    ly = yy + 1 + Metrics::kCellPadY;
                    break;
                }
                xx += L->table->colW[c];
            }
            yy += L->table->rowH[r];
        }
    }
    if (!tl) {  // a picture or a rule: the caret stands at its left edge
        *cx = bx;
        *docY = g.Y[bi];
        *h = std::max(8.f, g.H[bi]);
        return true;
    }
    FLOAT px = 0, py = 0;
    DWRITE_HIT_TEST_METRICS m{};
    uint32_t rel = pos >= off ? pos - off : 0;
    // edit mode's caret with the upper line's affinity (End, a click right of a wrapped line): at the end of that line,
    // after the character before it - its own position is the start of the next line (§12.2)
    bool aff = g.caretAff < 0 && g.editing && rel > 0 && pos == g.selFocus && (int32_t)bi == g.caretBlock &&
               cellIdx == g.caretCell;
    if (FAILED(tl->HitTestTextPosition(aff ? rel - 1 : rel, aff ? TRUE : FALSE, &px, &py, &m))) return false;
    *cx = lx + px;
    *docY = g.Y[bi] + ly + py;
    *h = m.height > 1.f ? m.height : 16.f;
    return true;
}

static bool CaretGeom(uint32_t pos, float* cx, float* docY, float* h, bool relayout) {
    return CaretGeomAt(pos, -1, -1, cx, docY, h, relayout);
}

// where the caret stands in client DIP — automation reads it to see what the keyboard did; in edit mode the caret
// knows its block and cell, and stands right of trailing blanks it was typed after
bool CaretPoint(uint32_t pos, float* x, float* y, float* h) {
    float dy;
    if (g.editing && g.phantomCaret && g.phantomBlock >= 0 && pos == g.selFocus) {  // in the phantom row (§6.7)
        *x = g.phantomX;
        *y = g.phantomY - g.scrollY;
        *h = g.phantomLine;
        return true;
    }
    bool edit = g.editing && pos == g.selFocus && g.caretBlock >= 0;
    if (!CaretGeomAt(pos, edit ? g.caretBlock : -1, edit ? g.caretCell : -1, x, &dy, h, true)) return false;
    if (edit && g.caretTrail) *x += g.caretTrail * SpaceAdvance((uint32_t)g.caretBlock);
    *y = dy - g.scrollY;
    return true;
}

static bool CaretStop(const Block& b) { return !BlockHidden(b) && b.textLen > 0; }

// a position between blocks belongs to no text: snap it to the neighbour the caret is heading for
static uint32_t SnapPos(uint32_t pos, int dir) {
    size_t n = g.doc.blocks.size();
    if (!n) return 0;
    uint32_t bi = BlockOfPos(pos);
    if (CaretStop(g.doc.blocks[bi]) && pos >= g.doc.blocks[bi].textOff && pos <= BlockEnd(g.doc.blocks[bi])) return pos;
    if (dir >= 0)
        for (size_t i = bi; i < n; i++)
            if (CaretStop(g.doc.blocks[i]) && BlockEnd(g.doc.blocks[i]) >= pos)
                return std::max(pos, g.doc.blocks[i].textOff);
    for (size_t i = bi + 1; i-- > 0;)
        if (CaretStop(g.doc.blocks[i])) return std::min(pos, BlockEnd(g.doc.blocks[i]));
    return pos;
}

static bool IsTrailSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

static uint32_t MoveChar(uint32_t pos, int dir) {
    const std::wstring& t = g.doc.text;
    const Block& b = g.doc.blocks[BlockOfPos(pos)];
    uint32_t s = b.textOff, e = BlockEnd(b);
    if (dir > 0 && pos < e) {
        uint32_t p = pos + 1;
        while (p < e && IsTrailSurrogate(t[p])) p++;  // never stop inside a surrogate pair
        return p;
    }
    if (dir < 0 && pos > s) {
        uint32_t p = pos - 1;
        while (p > s && IsTrailSurrogate(t[p])) p--;
        return p;
    }
    return SnapPos(dir > 0 ? e + 1 : (s ? s - 1 : 0), dir);  // over the edge: into the next block
}

static uint32_t MoveWord(uint32_t pos, int dir) {
    const std::wstring& t = g.doc.text;
    uint32_t s, e;
    ContainerRange(pos, &s, &e);
    e = std::min(e, (uint32_t)t.size());
    uint32_t p = pos;
    if (dir > 0) {
        if (p >= e) return SnapPos(e + 1, 1);
        if (IsWordChar(t[p])) while (p < e && IsWordChar(t[p])) p++;
        else while (p < e && !IsWordChar(t[p]) && !iswspace(t[p])) p++;
        while (p < e && iswspace(t[p])) p++;  // stop at the start of the next word, as Windows does
        return p > pos ? p : SnapPos(e + 1, 1);
    }
    if (p <= s) return SnapPos(s ? s - 1 : 0, -1);
    while (p > s && iswspace(t[p - 1])) p--;
    if (p > s && IsWordChar(t[p - 1])) while (p > s && IsWordChar(t[p - 1])) p--;
    else while (p > s && !IsWordChar(t[p - 1]) && !iswspace(t[p - 1])) p--;
    return p < pos ? p : SnapPos(s ? s - 1 : 0, -1);
}

// one visual line (or a page) up or down, keeping the x the caret started from — the way every text editor does it
static uint32_t MoveLine(uint32_t pos, int dir, float pageH) {
    float cx, dy, h;
    if (!CaretGeom(pos, &cx, &dy, &h, true)) return pos;
    if (g.caretWantX < 0) g.caretWantX = cx;
    cx = g.caretWantX;
    float probe = pageH > 0 ? dy + dir * pageH : dir > 0 ? dy + h + 2.f : dy - 2.f;
    for (int tries = 0; tries < 8; tries++) {  // gaps between blocks are not text: keep looking past them
        uint32_t p = pos;
        HitTestDoc(cx, probe - g.scrollY, &p, nullptr);
        p = SnapPos(p, dir);
        if (dir > 0 ? p > pos : p < pos) return p;
        probe += dir * std::max(6.f, h * 0.5f);
        if (probe < 0 || probe > g.docH) break;
    }
    return dir > 0 ? SnapPos((uint32_t)g.doc.text.size(), -1) : SnapPos(0, 1);
}

static uint32_t LineEdge(uint32_t pos, int dir) {
    float cx, dy, h;
    if (!CaretGeom(pos, &cx, &dy, &h, true)) return pos;
    uint32_t p = pos;
    HitTestDoc(dir > 0 ? 1e6f : -1e6f, dy + h * 0.5f - g.scrollY, &p, nullptr);
    return SnapPos(p, dir);
}

bool KeySelect(unsigned vk, bool ctrl, bool shift) {
    if (!shift || g.path.empty() || g.doc.blocks.empty() || g.findOpen || g.editing) return false;
    uint32_t pos = g.selFocus;
    if (!g.caretOn && !HasSelection()) {  // start where the reader is looking, not at a document top far above
        float cx, dy, h;
        if (!CaretGeom(pos, &cx, &dy, &h, true) || dy + h < g.scrollY || dy > g.scrollY + ViewH()) {
            uint32_t p = pos;
            HitTestDoc(TextLeft(), 4.f, &p, nullptr);
            pos = g.selAnchor = SnapPos(p, 1);
        }
    }
    uint32_t next = pos;
    bool vertical = false;
    switch (vk) {
    case VK_LEFT: next = ctrl ? MoveWord(pos, -1) : MoveChar(pos, -1); break;
    case VK_RIGHT: next = ctrl ? MoveWord(pos, 1) : MoveChar(pos, 1); break;
    case VK_UP: next = MoveLine(pos, -1, 0); vertical = true; break;
    case VK_DOWN: next = MoveLine(pos, 1, 0); vertical = true; break;
    case VK_PRIOR: next = MoveLine(pos, -1, ViewH() - 56.f); vertical = true; break;
    case VK_NEXT: next = MoveLine(pos, 1, ViewH() - 56.f); vertical = true; break;
    case VK_HOME: next = ctrl ? SnapPos(0, 1) : LineEdge(pos, -1); break;
    case VK_END: next = ctrl ? SnapPos((uint32_t)g.doc.text.size(), -1) : LineEdge(pos, 1); break;
    default: return false;
    }
    if (!vertical) g.caretWantX = -1;
    g.caretOn = true;
    g.selFocus = next;
    RevealTextPos(next, false);
    Invalidate();
    return true;
}

static void AppendCRLF(std::wstring& out, const wchar_t* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == L'\xFFFC') continue;  // the stand-in for a picture inside the line is not text
        if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) out.push_back(L'\r');
        out.push_back(s[i]);
    }
}

// Text of a range, with a formula coming out as the source it was written from: what is copied is what the author
// typed, not a hole where the picture of the formula stands.
static void AppendWithMath(std::wstring& out, uint32_t runOff, uint32_t runCount, uint32_t s, uint32_t e) {
    uint32_t at = s;
    for (uint32_t k = 0; k < runCount && at < e; k++) {
        const Run& r = g.doc.runs[runOff + k];
        if (!(r.flags & F_IMAGE) || r.start < at || r.start >= e || r.image >= g.doc.images.size()) continue;
        const Image& im = g.doc.images[r.image];
        if (!im.mathKind || im.alt.empty()) continue;
        AppendCRLF(out, g.doc.text.data() + at, r.start - at);
        out += im.alt;
        at = r.start + r.len;
    }
    if (at < e) AppendCRLF(out, g.doc.text.data() + at, e - at);
}

std::wstring BlockPlainText(uint32_t i) {
    const Block& b = g.doc.blocks[i];
    std::wstring out;
    AppendCRLF(out, g.doc.text.data() + b.textOff, b.textLen);
    return out;
}

std::wstring SelectionText() {
    std::wstring out;
    if (!HasSelection()) return out;
    uint32_t s = SelMin(), e = SelMax();
    bool any = false;
    for (uint32_t i = BlockOfPos(s); i < g.doc.blocks.size(); i++) {
        const Block& b = g.doc.blocks[i];
        if (b.textOff >= e && b.textLen) break;
        if (b.textOff > e) break;
        uint32_t bs = std::max(s, b.textOff), be = std::min(e, BlockEnd(b));
        // a formula or a diagram of its own: its source is the block's text, so it copies as it was written
        bool mathBlock = b.kind == BK_IMAGE && b.aux < g.doc.images.size() && g.doc.images[b.aux].mathKind;
        if (b.kind == BK_HR || (b.kind == BK_IMAGE && !mathBlock) || be <= bs || BlockHidden(b)) continue;
        if (any) out += b.gap >= 12.f ? L"\r\n\r\n" : L"\r\n";
        any = true;
        if (b.kind == BK_TABLE) {
            const Table& t = g.doc.tables[b.aux];
            for (uint32_t r = 0; r < t.rows; r++) {
                bool rowAny = false;
                for (uint32_t c = 0; c < t.cols; c++) {
                    const Cell& cell = g.doc.cells[t.cellOff + r * t.cols + c];
                    uint32_t cs = std::max(bs, cell.textOff), ce = std::min(be, cell.textOff + cell.textLen);
                    bool in = cell.textOff + cell.textLen > bs && cell.textOff < be;
                    if (!in) continue;
                    if (rowAny) out.push_back(L'\t');
                    if (ce > cs) AppendWithMath(out, cell.runOff, cell.runCount, cs, ce);
                    rowAny = true;
                }
                if (rowAny && r + 1 < t.rows && g.doc.cells[t.cellOff + (r + 1) * t.cols].textOff < be) out += L"\r\n";
            }
        } else {
            AppendWithMath(out, b.runOff, b.runCount, bs, be);
        }
    }
    return out;
}

std::wstring SlugOfBlock(uint32_t block) {
    for (const Heading& h : g.doc.headings)
        if (h.block == block) return h.slug;
    return L"";
}

int HeadingBlockBySlug(const std::wstring& slug) {
    std::wstring low = ToLower(slug);
    for (const Heading& h : g.doc.headings)
        if (h.slug == low || h.slug == slug) return (int)h.block;
    for (const Anchor& a : g.doc.anchors)  // footnote jumps, both ways
        if (a.slug == slug || a.slug == low) return (int)(a.block < g.doc.blocks.size() ? a.block : 0);
    return -1;
}

void ScrollToBlock(uint32_t i, bool animate) {
    if (i >= g.doc.blocks.size()) return;
    // lay out the target neighbourhood so its position is exact relative to what will be drawn
    EnsureLayout(i);
    RecomputeY();
    ScrollTo(g.Y[i] - (g.editing ? std::max(16.f, EditRevealTop()) : 16.f), animate);  // (below edit mode's bar, §12.1)
}

// make a text position visible: vertically (keeps a margin, or centres it) and inside a horizontally scrolled block
void RevealTextPos(uint32_t pos, bool center) {
    if (g.doc.blocks.empty()) return;
    uint32_t bi = BlockOfPos(pos);
    BlockLayout* L = EnsureLayout(bi);
    RecomputeY();
    const Block& b = g.doc.blocks[bi];
    IDWriteTextLayout* tl = nullptr;
    uint32_t off = b.textOff;
    float lx = 0, ly = 0;
    if ((b.kind == BK_TEXT || b.kind == BK_CODE) && L->text) {
        tl = L->text;
        if (b.kind == BK_CODE) lx = ly = Metrics::kCodePad;
    } else if (b.kind == BK_TABLE && L->table) {
        const Table& t = g.doc.tables[b.aux];
        float yy = 0;
        for (uint32_t r = 0; r < t.rows && !tl; r++) {
            float xx = 0;
            for (uint32_t c = 0; c < t.cols; c++) {
                const Cell& cell = g.doc.cells[t.cellOff + r * t.cols + c];
                if (pos >= cell.textOff && pos <= cell.textOff + cell.textLen && L->table->cells[r * t.cols + c]) {
                    tl = L->table->cells[r * t.cols + c];
                    off = cell.textOff;
                    lx = xx + 1 + Metrics::kCellPadX;
                    ly = yy + 1 + Metrics::kCellPadY;
                    break;
                }
                xx += L->table->colW[c];
            }
            yy += L->table->rowH[r];
        }
    }
    float y = g.Y[bi], x = 0;
    bool haveX = false;
    if (tl) {
        FLOAT px = 0, py = 0;
        DWRITE_HIT_TEST_METRICS m{};
        if (SUCCEEDED(tl->HitTestTextPosition(pos - off, FALSE, &px, &py, &m))) {
            y += ly + py;
            x = lx + px;
            haveX = true;
        }
    }
    float topMargin = std::max(56.f, EditRevealTop());  // edit mode: under the bar, a strip and the find bar
    if (center || y < g.scrollY + topMargin || y > g.scrollY + ViewH() - 48.f) ScrollTo(y - ViewH() * 0.35f, false);
    float vx, vw, cw;
    if (haveX && HScrollInfo(bi, &vx, &vw, &cw)) {
        float cur = HScrollOf(bi);
        if (x < cur + 24.f || x > cur + vw - 64.f) HScrollSet(bi, x - vw * 0.35f);
    }
}

// ------------------------------------------------------------------------------------------------ keyboard link focus
bool LinkRange(int li, uint32_t* start, uint32_t* end) {
    if (li < 0 || (size_t)li >= g.doc.links.size()) return false;
    bool found = false;
    for (const Run& r : g.doc.runs) {
        if ((r.flags & F_LINK) && r.link == (uint32_t)li) {
            if (!found) *start = r.start;
            *end = r.start + r.len;
            found = true;
        } else if (found && r.start >= *end) {
            break;  // a link's runs are contiguous
        }
    }
    return found;
}

void FocusLinkStep(int dir) {
    int n = (int)g.doc.links.size();
    if (!n || g.doc.blocks.empty()) return;
    uint32_t s = 0, e = 0;
    int li = g.focusLink;
    if (li < 0 || li >= n) {  // start from the viewport
        uint32_t first = std::min<uint32_t>(FirstVisible(g.scrollY), (uint32_t)g.doc.blocks.size() - 1);
        uint32_t last = std::min<uint32_t>(FirstVisible(g.scrollY + ViewH()), (uint32_t)g.doc.blocks.size() - 1);
        uint32_t top = g.doc.blocks[first].textOff, bottom = g.doc.blocks[last].textOff + g.doc.blocks[last].textLen;
        li = -1;
        if (dir > 0) {
            for (int k = 0; k < n && li < 0; k++)
                if (LinkRange(k, &s, &e) && s >= top) li = k;
        } else {
            for (int k = n - 1; k >= 0 && li < 0; k--)
                if (LinkRange(k, &s, &e) && s < bottom) li = k;
        }
        if (li < 0) li = dir > 0 ? 0 : n - 1;
    } else {
        li += dir;
    }
    for (int tries = 0; tries < n; tries++, li += dir) {  // skip links without text (image links), wrap around
        li = (li % n + n) % n;
        if (LinkRange(li, &s, &e)) break;
    }
    if (!LinkRange(li, &s, &e)) return;
    g.focusLink = li;
    g_focusS = s;
    g_focusE = e;
    RevealTextPos(s, false);
    Invalidate();
}
