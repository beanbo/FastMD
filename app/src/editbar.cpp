// Edit mode's chrome on the canvas (docs/EDIT-MODE.md §2.3-§2.5): geometry, drawing and hit-testing.
//
// Phase 1c: the strip - the band that tells the reader something is wrong with the file and offers what can be done
// about it. Reading mode needs it already for an interrupted save (RECOVERY); the toolbar, the popovers and the other
// strips arrive with 2a. A strip covers the top of the document (no inset) and every button in it runs its command
// through Command(), so tests and a click do exactly the same.
#include "app.h"

namespace {
const float kStripH = 36.f, kBtnH = 26.f, kPad = 14.f, kGap = 8.f;

struct StripButton { UINT cmd; StrId label; float l, t, r, b; };
int g_strip = STRIP_NONE;
int g_hot = -1;  // hovered button (index)

// what a strip says, in which colour, and its buttons from left to right
void Describe(int kind, std::wstring* text, uint8_t* accent, std::vector<StripButton>* btns) {
    switch (kind) {
    case STRIP_RECOVERY:
        *text = Tr(S_ED_RECOVERY_INTERRUPTED);
        *accent = P_ALERT_WARNING;
        *btns = {{CMD_RECOVERY_OPEN, S_ED_RECOVERY_OPEN}, {CMD_RECOVERY_RESTORE, S_ED_RECOVERY_RESTORE},
                 {CMD_RECOVERY_DELETE, S_ED_RECOVERY_DELETE}};
        break;
    default: *text = L""; *accent = P_ALERT_NOTE; btns->clear();
    }
}

float TextWidth(const std::wstring& s) {
    IDWriteTextLayout* L = UiLayout(s, 1000.f);
    if (!L) return 60.f;
    DWRITE_TEXT_METRICS m{};
    L->GetMetrics(&m);
    L->Release();
    return std::ceil(m.widthIncludingTrailingWhitespace);
}

// the buttons, right-aligned where the gear sits in reading mode (client DIP)
std::vector<StripButton> Buttons(int kind) {
    std::wstring text;
    uint8_t accent;
    std::vector<StripButton> b;
    Describe(kind, &text, &accent, &b);
    float x = ViewW() - kPad, t = (kStripH - kBtnH) * 0.5f;
    for (size_t k = b.size(); k-- > 0;) {
        float w = TextWidth(Tr(b[k].label)) + 28.f;
        b[k].l = x - w;
        b[k].r = x;
        b[k].t = t;
        b[k].b = t + kBtnH;
        x = b[k].l - kGap;
    }
    return b;
}

void Text(const std::wstring& s, float x, float top, float h, float maxW, uint8_t pal) {
    IDWriteTextLayout* L = UiLayout(s, std::max(10.f, maxW));
    if (!L) return;
    L->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* ellipsis = nullptr;
    if (SUCCEEDED(g.dwf->CreateEllipsisTrimmingSign(L, &ellipsis))) L->SetTrimming(&trim, ellipsis);
    DWRITE_TEXT_METRICS m{};
    L->GetMetrics(&m);
    g.canvas->Text(L, x, top + (h - m.height) * 0.5f, pal);
    SafeRelease(ellipsis);
    L->Release();
}
}  // namespace

void StripShow(int kind) {
    if (g_strip == kind) return;
    g_strip = kind;
    g_hot = -1;
    g.stripH = kStripH;
    ForceFullRedraw();
    Invalidate();
}

void StripHide(int kind) {
    if (g_strip != kind || kind == STRIP_NONE) return;
    g_strip = STRIP_NONE;
    g_hot = -1;
    g.stripH = 0;
    ForceFullRedraw();
    Invalidate();
}

int StripKind() { return g_strip; }

LRESULT StripButtonCenter(UINT cmd) {
    if (!g_strip || g.firstFrame) return -1;
    float s = Scale();
    for (const StripButton& b : Buttons(g_strip))
        if (b.cmd == cmd) return MAKELONG(std::lround((b.l + b.r) * 0.5f * s), std::lround((b.t + b.b) * 0.5f * s));
    return -1;
}

bool StripMouse(float x, float y, bool click) {
    if (!g_strip || g.firstFrame || x < DocLeft() || y < 0 || y >= kStripH) {
        if (g_hot >= 0) { g_hot = -1; Invalidate(); }
        return false;
    }
    std::vector<StripButton> b = Buttons(g_strip);
    int hot = -1;
    for (size_t k = 0; k < b.size(); k++)
        if (x >= b[k].l && x < b[k].r && y >= b[k].t && y < b[k].b) hot = (int)k;
    if (hot != g_hot) { g_hot = hot; Invalidate(); }
    SetCursor(LoadCursorW(nullptr, hot >= 0 ? IDC_HAND : IDC_ARROW));
    if (click && hot >= 0) Command(b[hot].cmd);
    return true;  // the strip covers the document: nothing under it gets the mouse
}

void DrawEditChrome() {
    if (!g_strip || g.firstFrame) return;
    std::wstring text;
    uint8_t accent;
    std::vector<StripButton> unused;
    Describe(g_strip, &text, &accent, &unused);
    std::vector<StripButton> b = Buttons(g_strip);
    float l = DocLeft(), r = ViewW();
    g.canvas->FillRect(l, 0, r, kStripH, P_OVERLAY_BG);
    g.canvas->FillRect(l, kStripH - 1.f, r, kStripH, P_OVERLAY_BORDER);
    g.canvas->FillRect(l, 0, l + 3.f, kStripH, accent);
    for (size_t k = 0; k < b.size(); k++) {  // the settings window's "Button" look
        g.canvas->FillRoundRect(b[k].l, b[k].t, b[k].r, b[k].b, 6.f, (int)k == g_hot ? P_HOVER : P_PANEL);
        g.canvas->StrokeRoundRect(b[k].l, b[k].t, b[k].r, b[k].b, 6.f, 1.f, P_BORDER);
        Text(Tr(b[k].label), b[k].l + 14.f, b[k].t, kBtnH, b[k].r - b[k].l, P_TEXT);
    }
    float textR = b.empty() ? r - kPad : b.front().l - kGap;
    Text(text, l + kPad, 0, kStripH, textR - (l + kPad), P_OVERLAY_TEXT);
}
