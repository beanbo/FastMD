// Outline panel: the document's headings, the current section highlighted while scrolling, a click jumps to the
// heading. Docked beside the column when the window is wide enough (the column moves right), an overlay drawer
// otherwise. Items are built from Doc::headings when the panel is shown; their text layouts are created lazily for the
// visible rows only.
#include "app.h"

namespace {
const float kPanelW = 264.f, kHeaderH = 46.f, kItemH = 28.f, kMinDocW = 600.f, kBtn = 30.f, kBtnX = 8.f, kBtnY = 8.f;
int g_minLevel = 1;
int g_lastCur = -2;

bool Visible() { return g.tocOpen && TocAvailable(); }

void CloseRect(float* l, float* t, float* r, float* b) {
    *l = kPanelW - 8.f - 28.f;
    *t = 9.f;
    *r = *l + 28.f;
    *b = *t + 28.f;
}

float ListH() { return std::max(0.f, ViewH() - kHeaderH - 6.f); }
float MaxListScroll() { return std::max(0.f, g.toc.size() * kItemH - ListH()); }

void DrawButton() {
    bool hot = g.tocBtnHot;
    if (hot) g.canvas->FillRoundRect(kBtnX, kBtnY, kBtnX + kBtn, kBtnY + kBtn, 6.f, P_HOVER);
    DrawIcon(0xE8FD, kBtnX, kBtnY, kBtn, 15.f, hot ? P_TEXT : P_MUTED);  // Segoe Fluent Icons: BulletedList
}
}  // namespace

static int g_frozen = -1;  // while editing: whether the document had headings at entry (-1 = not frozen)

bool TocAvailable() { return g_frozen >= 0 ? g_frozen != 0 : !g.path.empty() && !g.doc.headings.empty(); }
// An edit that adds or removes the only heading must not dock or undock the panel under the reader: the column would
// move and every layout go (§12.7, R21). Leaving lets it follow the headings again (EditExit's re-parse re-lays out).
void TocFreeze(bool on) {
    g_frozen = -1;
    if (on) g_frozen = TocAvailable() ? 1 : 0;
}
bool TocWideEnough() { return ViewW() >= kPanelW + kMinDocW + 2 * Metrics::kPadX; }
bool TocDocked() { return Visible() && TocWideEnough(); }
// on screen over the text; "open" alone is not enough: with no headings (start screen, a document without them) the
// panel is not drawn and must not swallow clicks or Esc
bool TocOverlayOpen() { return Visible() && !TocWideEnough(); }
float TocPanelW() { return kPanelW; }

// an item's text as the list shows it: the heading's text on one line
static bool SameText(const std::wstring& item, const Block& b) {
    if (item.size() != b.textLen) return false;
    for (uint32_t k = 0; k < b.textLen; k++) {
        wchar_t c = g.doc.text[b.textOff + k];
        if (item[k] != (c == L'\n' || c == L'\t' ? L' ' : c)) return false;
    }
    return true;
}

// Every edit swaps the model, and most leave the headings as they were: then the items, their layouts and the list's
// scroll stay exactly as they are. When the headings did change, an item that kept its level and text keeps its layout,
// and while editing the list is not centred on the current heading again - it would jump under the reader's typing
// (§12.7, R21).
void TocSync() {
    if (g.tocSerial == g.docSerial) return;
    g.tocSerial = g.docSerial;
    const auto& hs = g.doc.headings;
    size_t n = 0;
    bool same = true;
    for (const Heading& h : hs) {
        if (h.block >= g.doc.blocks.size()) continue;
        const TocItem* it = n < g.toc.size() ? &g.toc[n] : nullptr;
        same = same && it && it->block == h.block && it->level == h.level && SameText(it->text, g.doc.blocks[h.block]);
        n++;
    }
    if (same && n == g.toc.size()) return;
    std::vector<TocItem> old = std::move(g.toc);
    g.toc.clear();
    int minLevel = 6;
    for (const Heading& h : hs) {
        if (h.block >= g.doc.blocks.size()) continue;
        const Block& b = g.doc.blocks[h.block];
        std::wstring t(g.doc.text, b.textOff, b.textLen);
        for (auto& c : t) if (c == L'\n' || c == L'\t') c = L' ';
        g.toc.push_back(TocItem{h.block, h.level, std::move(t), nullptr});
        minLevel = std::min<int>(minLevel, h.level);
    }
    // a layout depends on the item's text, its level and the smallest level (indent and weight)
    if (minLevel == g_minLevel)
        for (size_t k = 0; k < g.toc.size() && k < old.size(); k++)
            if (old[k].level == g.toc[k].level && old[k].text == g.toc[k].text) std::swap(old[k].layout, g.toc[k].layout);
    for (auto& it : old) SafeRelease(it.layout);
    g_minLevel = minLevel;
    g.tocScroll = std::clamp(g.tocScroll, 0.f, MaxListScroll());
    if (!g.editing) g_lastCur = -2;
}

void TocSetOpen(bool open) {
    if (g.tocOpen == open) return;
    float tw = g.textW, ww = g.wideW;
    g.tocOpen = open;
    g.cfg.tocOpen = open;
    g.tocHover = -1;
    g.tocBtnHot = false;
    if (open) TocSync();
    UpdateColumns();
    if (std::fabs(tw - g.textW) > 0.1f || std::fabs(ww - g.wideW) > 0.1f) Relayout();  // docking narrowed the column
    g_lastCur = -2;  // re-centre the current item
    Invalidate();
}

int TocCurrent() {
    auto& hs = g.doc.headings;
    if (hs.empty() || g.Y.size() != g.doc.blocks.size()) return -1;
    float line = g.scrollY + std::max(48.f, g.editing ? EditRevealTop() + 4.f : 0.f);  // (under edit mode's bar, §12.1)
    bool atEnd = g.scrollY >= MaxScroll() - 1.f && MaxScroll() > 0;
    if (atEnd) line = g.scrollY + ViewH() * 0.5f;  // the last sections never reach the top
    int cur = 0;
    for (size_t k = 0; k < hs.size(); k++) {
        if (hs[k].block >= g.Y.size() || g.Y[hs[k].block] > line) break;
        cur = (int)k;
    }
    return cur;
}

void DrawToc() {
    // icons (Segoe Fluent Icons) stay out of the first frame: the font is loaded right after it (AfterFirstFrame repaints)
    if (!Visible()) {  // edit mode's bar has its own outline button; a reading-mode strip leaves this corner free
        if (TocAvailable() && !g.firstFrame && g.barT <= 0) DrawButton();
        return;
    }
    TocSync();
    float w = kPanelW, h = ViewH();
    g.canvas->FillRect(0, 0, w - 1.f, h, P_PANEL);
    g.canvas->FillRect(w - 1.f, 0, w, h, P_BORDER);
    // header
    if (IDWriteTextLayout* t = UiLayout(Tr(S_TOC_TITLE), w - 60.f)) {
        t->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_RANGE{0, 64});
        t->SetFontSize(14.f, DWRITE_TEXT_RANGE{0, 64});
        DWRITE_TEXT_METRICS m{};
        t->GetMetrics(&m);
        g.canvas->Text(t, 18.f, (kHeaderH - m.height) * 0.5f, P_TEXT);
        t->Release();
    }
    float cl, ct, cr, cb;
    CloseRect(&cl, &ct, &cr, &cb);
    if (g.tocHover == -2) g.canvas->FillRoundRect(cl, ct, cr, cb, 6.f, P_HOVER);
    if (!g.firstFrame) DrawIcon(0xE711, cl, ct, 28.f, 11.f, g.tocHover == -2 ? P_TEXT : P_MUTED);  // Cancel
    // items
    int cur = TocCurrent();
    float listTop = kHeaderH, listH = ListH();
    g.tocScroll = std::clamp(g.tocScroll, 0.f, MaxListScroll());
    if (cur != g_lastCur && cur >= 0) {  // follow the reading position
        float iy = cur * kItemH;
        if (g_lastCur == -2) g.tocScroll = std::clamp(iy - listH * 0.4f, 0.f, MaxListScroll());
        else if (iy < g.tocScroll) g.tocScroll = iy;
        else if (iy + kItemH > g.tocScroll + listH) g.tocScroll = iy + kItemH - listH;
        g_lastCur = cur;
    }
    g.canvas->PushClip(0, listTop, w - 1.f, h);
    size_t first = (size_t)std::max(0.f, std::floor(g.tocScroll / kItemH));
    for (size_t k = first; k < g.toc.size(); k++) {
        float y = listTop + k * kItemH - g.tocScroll;
        if (y > h) break;
        TocItem& it = g.toc[k];
        float indent = 14.f + (it.level - g_minLevel) * 14.f;
        if ((int)k == cur) {
            g.canvas->FillRoundRect(6.f, y + 2.f, w - 8.f, y + kItemH - 2.f, 5.f, P_CURRENT);
            g.canvas->FillRoundRect(6.f, y + 7.f, 9.f, y + kItemH - 7.f, 1.5f, P_ACCENT);
        } else if ((int)k == g.tocHover) {
            g.canvas->FillRoundRect(6.f, y + 2.f, w - 8.f, y + kItemH - 2.f, 5.f, P_HOVER);
        }
        if (!it.layout) {
            it.layout = UiLayout(it.text, std::max(40.f, w - indent - 18.f));
            if (it.layout) {
                DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                IDWriteInlineObject* ell = nullptr;
                if (SUCCEEDED(g.dwf->CreateEllipsisTrimmingSign(it.layout, &ell))) {
                    it.layout->SetTrimming(&tr, ell);
                    ell->Release();
                }
                DWRITE_TEXT_RANGE all{0, (UINT32)it.text.size()};
                it.layout->SetFontSize(13.5f, all);
                if (it.level == g_minLevel) it.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
            }
        }
        if (it.layout) {
            DWRITE_TEXT_METRICS m{};
            it.layout->GetMetrics(&m);
            bool strong = (int)k == cur || it.level <= g_minLevel + 1;
            g.canvas->Text(it.layout, indent, y + (kItemH - m.height) * 0.5f, strong ? P_TEXT : P_MUTED);
        }
    }
    g.canvas->PopClip();
}

bool TocHit(float x, float y, int* item) {
    *item = -1;
    if (!Visible() || x >= kPanelW) return false;
    float cl, ct, cr, cb;
    CloseRect(&cl, &ct, &cr, &cb);
    if (x >= cl && x < cr && y >= ct && y < cb) { *item = -2; return true; }
    if (y >= kHeaderH) {
        TocSync();
        int k = (int)std::floor((y - kHeaderH + g.tocScroll) / kItemH);
        if (k >= 0 && k < (int)g.toc.size()) *item = k;
    }
    return true;
}

bool TocButtonRect(float* l, float* t, float* r, float* b) {
    if (Visible() || !TocAvailable() || g.firstFrame || g.barT > 0) return false;
    *l = kBtnX;
    *t = kBtnY;
    *r = kBtnX + kBtn;
    *b = kBtnY + kBtn;
    return true;
}

bool TocButtonHit(float x, float y) {
    return !Visible() && TocAvailable() && g.barT <= 0 && x >= kBtnX && x < kBtnX + kBtn && y >= kBtnY && y < kBtnY + kBtn;
}

void TocClick(int item) {
    if (item == -2) { TocSetOpen(false); return; }
    TocSync();
    if (item < 0 || item >= (int)g.toc.size()) return;
    g.userMoved = true;
    ScrollToBlock(g.toc[item].block, true);
    if (!TocDocked()) TocSetOpen(false);  // overlay drawer: out of the way once you jumped
}

void TocWheel(float dy) {
    float s = std::clamp(g.tocScroll + dy, 0.f, MaxListScroll());
    if (s != g.tocScroll) { g.tocScroll = s; Invalidate(); }
}

float TocItemY(int item) { return kHeaderH + item * kItemH - g.tocScroll + kItemH * 0.5f; }
