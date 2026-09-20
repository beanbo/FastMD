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
    g.textW = std::min(avail, tmax);
    g.wideW = g.cfg.column == COL_FULL ? avail : std::min(avail, tmax + Metrics::kBreakout);
}
float TextLeft() { return DocLeft() + std::floor((DocW() - g.textW) * 0.5f); }
float WideLeft() { return DocLeft() + std::floor((DocW() - g.wideW) * 0.5f); }
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
        *x = tl;
        *w = b.kind == BK_CODE ? g.textW : nat;
        return;
    }
    *w = std::min(nat, g.wideW);
    *x = std::max(WideLeft(), std::floor(tl - (*w - g.textW) * 0.5f));
}

// ------------------------------------------------------------------------------------------------ geometry
void RecomputeY() {
    float y = Metrics::kPadTop;
    size_t n = g.doc.blocks.size();
    for (size_t i = 0; i < n; i++) {
        y += g.doc.blocks[i].gap;
        g.Y[i] = y;
        y += g.H[i];
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

bool HScrollInfo(uint32_t i, float* visX, float* visW, float* contentW) {
    if (i >= g.doc.blocks.size() || i >= g.cache.size()) return false;
    const Block& b = g.doc.blocks[i];
    BlockLayout* L = g.cache[i];
    if (!L || (b.kind != BK_CODE && b.kind != BK_TABLE)) return false;
    float x, w;
    BlockBox(i, &x, &w);
    float content, vis;
    if (b.kind == BK_CODE) {
        content = L->natural;
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
        float bottom = g.Y[k] + g.H[k] + (g.doc.blocks[k].kind == BK_TABLE ? 12.f : 0.f);
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
static void ApplyColors(BlockLayout* L, const Block& b) {
    if (L->colored) return;
    L->colored = true;
    auto apply = [&](IDWriteTextLayout* tl, uint32_t textOff, uint32_t runOff, uint32_t runCount) {
        for (uint32_t k = 0; k < runCount; k++) {
            const Run& r = g.doc.runs[runOff + k];
            if (r.color != P_DEFAULT) tl->SetDrawingEffect(g.canvas->Effect(r.color), DWRITE_TEXT_RANGE{r.start - textOff, r.len});
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
                                  std::vector<D2D1_RECT_F>& out) {
    DWRITE_HIT_TEST_METRICS hm[16];
    for (uint32_t k = 0; k < runCount; k++) {
        const Run& r = g.doc.runs[runOff + k];
        if (!(r.flags & F_CODE)) continue;
        UINT32 cnt = 0;
        if (FAILED(tl->HitTestTextRange(r.start - textOff, r.len, 0, 0, hm, 16, &cnt))) continue;
        float fs = g.typo.size[role] * 0.85f, pad = fs * 0.2f;
        for (UINT32 j = 0; j < cnt; j++) {
            float base = hm[j].top + g.typo.baseline[role];
            out.push_back(D2D1::RectF(hm[j].left, std::round(base - g.typo.monoAscent * fs - pad),
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

static void DrawMarker(const Block& b, float x, float baseline) {
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
        float sz = 15.f * s, l = x - 24.f * s, t = baseline - 12.5f * s;
        if (b.marker == MK_TASK_DONE) {
            g.canvas->FillRoundRect(l, t, l + sz, t + sz, 3.5f * s, P_ACCENT);
            g.canvas->Line(l + 3.8f * s, t + 7.8f * s, l + 6.4f * s, t + 10.4f * s, 1.8f * s, P_ONACCENT);
            g.canvas->Line(l + 6.4f * s, t + 10.4f * s, l + 11.2f * s, t + 4.8f * s, 1.8f * s, P_ONACCENT);
        } else {
            g.canvas->StrokeRoundRect(l, t, l + sz, t + sz, 3.5f * s, 1.1f, P_MUTED);
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

static void DrawBlock(uint32_t i, float y) {
    const Block& b = g.doc.blocks[i];
    BlockLayout* L = EnsureLayout(i);
    ApplyColors(L, b);
    float x, w;
    BlockBox(i, &x, &w);
    float right = x + w;
    uint8_t col = b.muted ? P_MUTED : P_TEXT;
    int role = b.heading ? b.heading : R_BODY;
    float markerBase = y + g.typo.baseline[R_BODY];
    switch (b.kind) {
    case BK_TEXT:
        if (L->text) {
            if (!L->codeBgValid) {
                L->codeBg.clear();
                InlineCodeBackgrounds(L->text, b.textOff, b.runOff, b.runCount, role, L->codeBg);
                L->codeBgValid = true;
            }
            for (auto& r : L->codeBg) g.canvas->FillRoundRect(x + r.left, y + r.top, x + r.right, y + r.bottom, 4.f, P_INLINEBG);
            DrawHighlights(L->text, b.textOff, b.textLen, x, y);
            g.canvas->Text(L->text, x, y, col);
            DrawLinkFocus(L->text, b.textOff, b.textLen, x, y);
        }
        if (b.heading == 1 || b.heading == 2) g.canvas->FillRect(x, y + L->height - 1, right, y + L->height, P_BORDER);
        markerBase = y + g.typo.baseline[role];
        break;
    case BK_CODE: {
        g.canvas->FillRoundRect(x, y, right, y + L->height, 6.f, P_CODEBG);
        if (L->text) {
            float tx = x + Metrics::kCodePad - HScrollOf(i), ty = y + Metrics::kCodePad;
            g.canvas->PushClip(x, y, right, y + L->height);
            DrawHighlights(L->text, b.textOff, b.textLen, tx, ty);
            g.canvas->Text(L->text, tx, ty, P_TEXT);
            g.canvas->PopClip();
        }
        markerBase = y + Metrics::kCodePad + g.typo.baseline[R_CODE];
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
        Image& im0 = g.doc.images[b.aux];
        Image& im = im0.canon >= 0 ? g.doc.images[im0.canon] : im0;
        float iw = L->natural;
        if (im.state.load() == 2) g.canvas->DrawImage(im, x, y, x + iw, y + L->height);
        else {
            g.canvas->FillRoundRect(x, y, x + iw, y + L->height, 6.f, P_PLACEHOLDER);
            if (L->text && im0.w <= 0) g.canvas->Text(L->text, x + 12.f, y + 9.f, P_MUTED);
        }
        break;
    }
    }
    if (b.marker) DrawMarker(b, x, markerBase);
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

void DrawPill(const std::wstring& s, float cx, float y, bool centered) {
    IDWriteTextLayout* L = UiLayout(s, std::max(100.f, ViewW() - 48.f));
    if (!L) return;
    DWRITE_TEXT_METRICS m{};
    L->GetMetrics(&m);
    float w = std::ceil(m.width) + 24.f, h = 30.f;
    float x = centered ? std::floor(cx - w * 0.5f) : cx;
    g.canvas->FillRoundRect(x, y, x + w, y + h, 8.f, P_OVERLAY_BG);
    g.canvas->StrokeRoundRect(x, y, x + w, y + h, 8.f, 1.f, P_OVERLAY_BORDER);
    g.canvas->Text(L, x + 12.f, y + (h - m.height) * 0.5f, P_OVERLAY_TEXT);
    L->Release();
}

static void DrawScrollbar() {
    float vh = ViewH(), vw = ViewW();
    if (g.docH <= vh + 1) return;
    float trackT = 2, trackB = vh - 2, trackH = trackB - trackT;
    float th = std::max(32.f, trackH * vh / g.docH);
    float ty = trackT + (trackH - th) * (g.scrollY / MaxScroll());
    if (g.findOpen && !g.matches.empty()) DrawFindMarks(vw - 12.f, vw - 2.f);
    bool hot = g.draggingThumb || g.hotScroll;
    float w = hot ? 8.f : 5.f;
    g.canvas->FillRoundRect(vw - w - 3, ty, vw - 3, ty + th, w * 0.5f, hot ? P_SCROLL_HOT : P_SCROLL);
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
    DrawToc();
    if (g.findOpen) DrawFindBar();
    const std::wstring* pill = nullptr;
    if (!g.tip.empty()) pill = &g.tip;
    else if (g.hoverLink >= 0 && (size_t)g.hoverLink < g.doc.links.size() && !g.selecting) pill = &g.doc.links[g.hoverLink];
    else if (g.focusLink >= 0 && (size_t)g.focusLink < g.doc.links.size()) pill = &g.doc.links[g.focusLink];
    if (pill && !g.path.empty()) DrawPill(*pill, DocLeft() + 10.f, ViewH() - 40.f, false);
    else if (pill) DrawPill(*pill, 10.f, ViewH() - 40.f, false);
    if (!g.toast.empty() && GetTickCount() < g.toastUntil) DrawPill(g.toast, DocLeft() + DocW() * 0.5f, ViewH() - 64.f, true);
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
    uint32_t selA = 0, selB = 0;
    int hoverLink = 0, hoverCode = 0, hoverHBlock = 0, hbarFlash = 0, focusLink = 0, dragHBlock = 0, tocHover = 0,
        curMatch = 0, findHot = 0, recentHover = 0;
    size_t matches = 0;
    bool hoverCopyBtn = false, hotHBar = false, hotScroll = false, dark = false, selecting = false, tocBtnHot = false,
         settingsBtnHot = false, draggingThumb = false, home = false, overText = false;
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
    // things drawn over the text: they would have to be repaired pixel by pixel, so those frames are drawn in full
    bool pill = !g.tip.empty() || (g.hoverLink >= 0 && !g.selecting) || g.focusLink >= 0;
    bool toast = !g.toast.empty() && GetTickCount() < g.toastUntil;
    k.overText = g.findOpen || TocOverlayOpen() || pill || toast;
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
    if (!ScrollFrame(k)) {
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

static uint32_t HitLayout(IDWriteTextLayout* tl, uint32_t textOff, float lx, float ly, bool* inside) {
    BOOL trailing = FALSE, in = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(tl->HitTestPoint(lx, ly, &trailing, &in, &m))) return textOff;
    if (inside) *inside = in != FALSE;
    return textOff + m.textPosition + (trailing ? m.length : 0);
}

bool HitTestDoc(float px, float py, uint32_t* pos, bool* inside) {
    if (inside) *inside = false;
    size_t n = g.doc.blocks.size();
    if (!n) { *pos = 0; return false; }
    float docY = py + g.scrollY;
    uint32_t i = FirstVisible(docY);
    if (i >= n) { *pos = BlockEnd(g.doc.blocks[n - 1]); return true; }
    const Block& b = g.doc.blocks[i];
    if (docY < g.Y[i]) { *pos = b.textOff; return true; }
    BlockLayout* L = EnsureLayout(i);
    float bx, bw;
    BlockBox(i, &bx, &bw);
    float ly = docY - g.Y[i];
    switch (b.kind) {
    case BK_TEXT:
        *pos = L->text ? HitLayout(L->text, b.textOff, px - bx, ly, inside) : b.textOff;
        return true;
    case BK_CODE:
        *pos = L->text ? HitLayout(L->text, b.textOff, px - bx - Metrics::kCodePad + HScrollOf(i), ly - Metrics::kCodePad, inside)
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
                        *pos = cl ? HitLayout(cl, cell.textOff, px - xx - 1 - Metrics::kCellPadX,
                                              ly - yy - 1 - Metrics::kCellPadY, inside)
                                  : cell.textOff;
                        return true;
                    }
                    xx += cw;
                }
            }
            yy += rh;
        }
        *pos = b.textOff;
        return true;
    }
    default:
        *pos = (px < bx + bw * 0.5f) ? b.textOff : BlockEnd(b);
        return true;
    }
}

static int LinkOfRuns(uint32_t p, uint32_t runOff, uint32_t runCount) {
    for (uint32_t k = 0; k < runCount; k++) {
        const Run& r = g.doc.runs[runOff + k];
        if ((r.flags & F_LINK) && p >= r.start && p < r.start + r.len) return (int)r.link;
    }
    return -1;
}

int LinkAt(float px, float py) {
    uint32_t pos;
    bool inside = false;
    if (g.path.empty() || !HitTestDoc(px, py, &pos, &inside) || !inside) return -1;
    // HitTestPoint reports the character under the point; a trailing hit moved pos past it → check pos and pos-1
    for (uint32_t p : {pos, pos ? pos - 1 : 0}) {
        uint32_t bi = BlockOfPos(p);
        const Block& b = g.doc.blocks[bi];
        int li = -1;
        if (b.kind == BK_TEXT) li = LinkOfRuns(p, b.runOff, b.runCount);
        else if (b.kind == BK_TABLE) {
            const Table& t = g.doc.tables[b.aux];
            for (uint32_t c = 0; c < t.rows * t.cols && li < 0; c++) {
                const Cell& cell = g.doc.cells[t.cellOff + c];
                if (p >= cell.textOff && p < cell.textOff + cell.textLen) li = LinkOfRuns(p, cell.runOff, cell.runCount);
            }
        }
        if (li >= 0) return li;
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

void SelectWordAt(uint32_t pos) {
    uint32_t s, e;
    ContainerRange(pos, &s, &e);
    const std::wstring& t = g.doc.text;
    if (pos >= e && pos > s) pos = e - 1;
    if (pos >= t.size()) return;
    bool word = IsWordChar(t[pos]);
    bool space = iswspace(t[pos]) != 0;
    uint32_t a = pos, b = pos;
    auto same = [&](wchar_t c) { return word ? IsWordChar(c) : space ? iswspace(c) != 0 : (!IsWordChar(c) && !iswspace(c)); };
    while (a > s && same(t[a - 1])) a--;
    while (b < e && same(t[b])) b++;
    g.selAnchor = a;
    g.selFocus = b;
}

void SelectBlockAt(uint32_t pos) {
    uint32_t s, e;
    ContainerRange(pos, &s, &e);
    g.selAnchor = s;
    g.selFocus = e;
}

static void AppendCRLF(std::wstring& out, const wchar_t* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) out.push_back(L'\r');
        out.push_back(s[i]);
    }
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
        if (b.kind == BK_HR || b.kind == BK_IMAGE || be <= bs) continue;
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
                    if (ce > cs) out.append(g.doc.text, cs, ce - cs);
                    rowAny = true;
                }
                if (rowAny && r + 1 < t.rows && g.doc.cells[t.cellOff + (r + 1) * t.cols].textOff < be) out += L"\r\n";
            }
        } else {
            AppendCRLF(out, g.doc.text.data() + bs, be - bs);
        }
    }
    return out;
}

int HeadingBlockBySlug(const std::wstring& slug) {
    std::wstring low = ToLower(slug);
    for (const Heading& h : g.doc.headings)
        if (h.slug == low || h.slug == slug) return (int)h.block;
    return -1;
}

void ScrollToBlock(uint32_t i, bool animate) {
    if (i >= g.doc.blocks.size()) return;
    // lay out the target neighbourhood so its position is exact relative to what will be drawn
    EnsureLayout(i);
    RecomputeY();
    ScrollTo(g.Y[i] - 16.f, animate);
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
    if (center || y < g.scrollY + 56.f || y > g.scrollY + ViewH() - 48.f) ScrollTo(y - ViewH() * 0.35f, false);
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
