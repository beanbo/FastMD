// Edit mode's chrome on the canvas (docs/EDIT-MODE.md §2.3-§2.5, §12.1-§12.4): the toolbar, the pencil button that
// enters edit mode, the strips that say what is wrong with the file, and the toolbar's tooltips - geometry, drawing and
// hit-testing. Every button runs its command through Command(), so tests, UI Automation and a click do exactly the same.
//
// The bar is sized in DIP of the screen (u = 1 / zoom in canvas units): zooming the document does not grow it. It
// covers the top of the page; view.cpp starts the document EditInset() lower and edit.cpp moves the scroll position
// with it, so nothing the reader looks at jumps (§12.1).
//
// Phase 2a: the bar's shell - every button in its place, the collapse levels, undo / redo, the status slot and the ✕.
// The formatting and insert buttons are drawn disabled until their commands arrive (3a); the popovers are 3a's.
#include "app.h"

// Drawn once a frame and hit once a mouse move, nothing that the frame's time would notice: nothing is inlined, for
// size (§1 principle 3; see editcore.cpp)
#pragma inline_depth(0)
namespace {
float U() { return 1.f / std::max(0.25f, g.cfg.zoom); }  // one DIP of the screen, in canvas units
const float kBarH = 44.f, kBtn = 30.f, kGap = 2.f, kDivM = 6.f, kPadL = 8.f, kRight = 14.f;

// ------------------------------------------------------------------------------------------------ the buttons (§2.3)
enum Group : uint8_t { G_NAV, G_HISTORY, G_STYLE, G_INLINE, G_LISTS, G_INSERT, G_STATUS, G_MORE, G_CLOSE };
struct Def {
    wchar_t glyph, mdl2;   // Segoe Fluent Icons, and the Segoe MDL2 Assets stand-in where Fluent has its own
    UINT cmd;
    Group group;
    StrId tip;
    uint8_t collapse;      // the level from which it moves into "…" (0 = never)
    UINT vk;               // the shortcut: key and modifiers (1 Ctrl, 2 Shift), 0 = none
    uint8_t mods;
};
const Def kDefs[] = {
    {0xE8FD, 0, CMD_TOC, G_NAV, S_TOC_TITLE, 6, 'O', 3},
    {0xE7A7, 0, CMD_UNDO, G_HISTORY, S_ED_TIP_UNDO, 0, 'Z', 1},
    {0xE7A6, 0, CMD_REDO, G_HISTORY, S_ED_TIP_REDO, 0, 'Y', 1},
    {0xE8E9, 0, CMD_BLOCK_MENU, G_STYLE, S_ED_TIP_STYLE, 0, 0, 0},
    {0xE8DD, 0, CMD_FMT_BOLD, G_INLINE, S_ED_TIP_BOLD, 0, 'B', 1},
    {0xE8DB, 0, CMD_FMT_ITALIC, G_INLINE, S_ED_TIP_ITALIC, 0, 'I', 1},
    {0xEDE0, 0, CMD_FMT_STRIKE, G_INLINE, S_ED_TIP_STRIKE, 2, 'X', 3},
    {0xF54C, 0xE943, CMD_FMT_CODE, G_INLINE, S_ED_TIP_CODE, 2, VK_OEM_3, 1},
    {0xE71B, 0, CMD_LINK, G_INLINE, S_ED_TIP_LINK, 6, 'K', 1},
    {0xE292, 0, CMD_LIST_BULLET, G_LISTS, S_ED_TIP_BULLET, 3, '8', 3},
    {0, 0, CMD_LIST_NUMBER, G_LISTS, S_ED_TIP_NUMBER, 3, '7', 3},  // "1." drawn as text
    {0xE9D5, 0, CMD_LIST_TASK, G_LISTS, S_ED_TIP_TASK, 3, '9', 3},
    {0xE9B2, 0, CMD_QUOTE, G_LISTS, S_ED_TIP_QUOTE, 3, 'Q', 3},
    {0xE943, 0, CMD_CODEBLOCK, G_INSERT, S_ED_TIP_CODEBLOCK, 5, 'K', 3},
    {0xF232, 0xE80A, CMD_TABLE_MENU, G_INSERT, S_ED_TIP_TABLE, 5, 'T', 1},
    {0xE94B, 0, CMD_FORMULA_MENU, G_INSERT, S_ED_TIP_FORMULA, 5, 'M', 1},
    {0xEF90, 0, CMD_DIAGRAM_MENU, G_INSERT, S_ED_TIP_DIAGRAM, 5, 0, 0},
    {0xEB9F, 0, CMD_INS_IMAGE, G_INSERT, S_ED_TIP_IMAGE, 5, 0, 0},
    {0xE738, 0, CMD_INS_HR, G_INSERT, S_ED_TIP_HR, 5, 0, 0},
    {0, 0, CMD_SAVE, G_STATUS, S_ED_ST_SAVED, 0, 'S', 1},
    {0xE712, 0, CMD_EDIT_MORE, G_MORE, S_ED_TIP_MORE, 0, 0, 0},
    {0xE711, 0, CMD_EDIT_EXIT, G_CLOSE, S_ED_TIP_EXIT, 0, VK_ESCAPE, 0},
};
const int kCount = (int)std::size(kDefs);
const int kStyle = 3, kStatus = 19, kMore = 20, kClose = 21;

struct Item { float l = 0, r = 0; bool shown = false; };
struct Layout {
    Item items[std::size(kDefs)];
    std::vector<float> dividers;  // x of each group divider
    int level = 0;
    float leftEnd = 0;            // where the left-hand buttons end (the status text may grow left up to here)
};

int g_hot = -1;          // hovered button (index into kDefs)
bool g_pencilHot = false;
// the collapse level is kept until the width, the DPI, the zoom or the language change (UX-11: no reflow while typing)
struct LevelKey { float w = -1, zoom = 0, dpi = 0; UiLang lang = UL_RU; bool toc = false; };
LevelKey g_levelKey;
int g_level = 0;

bool Mdl2() { return !wcscmp(g.typo.iconFamily, L"Segoe MDL2 Assets"); }
bool HighContrast() { return PaletteIsHighContrast(); }

IDWriteTextLayout* UiText(const std::wstring& s, float size, bool semibold = false) {
    IDWriteTextLayout* L = UiLayout(s, 2000.f);
    if (!L) return nullptr;
    DWRITE_TEXT_RANGE all{0, (UINT32)s.size()};
    L->SetFontSize(size, all);
    if (semibold) L->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
    return L;
}
float TextW(const std::wstring& s, float size, bool semibold = false) {
    IDWriteTextLayout* L = UiText(s, size, semibold);
    if (!L) return 60.f * size / 13.f;
    DWRITE_TEXT_METRICS m{};
    L->GetMetrics(&m);
    L->Release();
    return std::ceil(m.widthIncludingTrailingWhitespace);
}

std::wstring StyleLabel(int id) {
    wchar_t b[64];
    switch (id) {
    case 0: return Tr(S_ED_STYLE_TEXT);
    case 7: return Tr(S_ED_STYLE_CODE);
    case 8: return Tr(S_ED_STYLE_TABLE);
    case 10: return Tr(S_ED_STYLE_FOOTNOTE);
    case 9: case 11: return L"—";  // an object, a raw leaf (§8.1)
    default:
        swprintf_s(b, Tr(S_ED_STYLE_HEADING_FMT), id);
        return b;
    }
}

// The style button and the status slot have fixed widths - the widest thing they can show - so nothing moves while
// the caret goes from a heading to a paragraph or a save starts and ends. Measured once per zoom and language.
struct FixedW { float u = 0; UiLang lang = UL_RU; float style = 0, status = 0; };
FixedW g_fixed;
const FixedW& Fixed() {
    if (g_fixed.u != U() || g_fixed.lang != UiLanguage()) {
        g_fixed.u = U();
        g_fixed.lang = UiLanguage();
        float w = 0;
        for (int id : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) w = std::max(w, TextW(StyleLabel(id), 13.f * U()));
        g_fixed.style = w + (22.f + 16.f) * U();
        w = 0;
        for (StrId s : {S_ED_ST_SAVED, S_ED_ST_PENDING, S_ED_ST_SAVING}) w = std::max(w, TextW(Tr(s), 12.5f * U()));
        g_fixed.status = w + 16.f * U();
    }
    return g_fixed;
}
float StyleW() { return Fixed().style; }
float StatusW() { return Fixed().status; }

bool Visible(int k, int level) {
    const Def& d = kDefs[k];
    if (d.cmd == CMD_TOC && !TocAvailable()) return false;
    if (k == kMore) return level >= 2;  // shown only when something went into it
    return !d.collapse || level < d.collapse;
}

Layout Compute(int level) {
    Layout L;
    L.level = level;
    const float u = U(), btn = kBtn * u, gap = kGap * u, div = (kDivM * 2 + 1) * u;
    float x = DocLeft() + kPadL * u;
    int lastGroup = -1;
    for (int k = 0; k < kCount; k++) {
        const Def& d = kDefs[k];
        if (d.group >= G_STATUS) break;
        if (!Visible(k, level)) continue;
        if (lastGroup >= 0 && d.group != lastGroup) {
            L.dividers.push_back(x - gap + kDivM * u);
            x += div - gap;
        }
        float w = k == kStyle && level < 4 ? StyleW() : btn;
        L.items[k] = Item{x, x + w, true};
        x += w + gap;
        lastGroup = d.group;
    }
    float leftEnd = x;
    L.leftEnd = leftEnd;
    // from the right: ✕ where the gear is in reading mode, a divider, then "…" and the status left of it
    float r = ViewW() - kRight * u;
    L.items[kClose] = Item{r - btn, r, true};
    r -= btn;
    L.dividers.push_back(r - kDivM * u - 1.f * u);
    r -= div;
    for (int k : {kMore, kStatus}) {
        if (!Visible(k, level)) continue;
        float w = k == kStatus && level < 1 ? StatusW() : btn;
        L.items[k] = Item{r - w, r, true};
        r -= w + gap;
    }
    if (leftEnd + 8.f * u > r) L.level = -1;  // does not fit
    return L;
}

// the smallest collapse level at which everything fits (§2.3), kept while nothing that decides it changes
int Level() {
    LevelKey k{ViewW() - DocLeft(), g.cfg.zoom, g.dpi, UiLanguage(), TocAvailable()};
    if (k.w != g_levelKey.w || k.zoom != g_levelKey.zoom || k.dpi != g_levelKey.dpi || k.lang != g_levelKey.lang ||
        k.toc != g_levelKey.toc) {
        g_levelKey = k;
        g_level = 6;
        for (int l = 0; l <= 6; l++)
            if (Compute(l).level >= 0) { g_level = l; break; }
    }
    return g_level;
}

bool BarShown() { return g.barT > 0 && !g.firstFrame && !g.path.empty(); }
float BarTop() { return (g.barT - 1.f) * kBarH * U(); }  // slides down from above the window

UINT g_pop = 0;  // the command whose popover is open (§2.4), 0 = none
bool Enabled(int k, int* why = nullptr) {
    if (why) *why = 0;
    switch (kDefs[k].cmd) {
    case CMD_TOC: return TocAvailable();
    case CMD_UNDO: return EditCanUndo();
    case CMD_REDO: return EditCanRedo();
    case CMD_SAVE: case CMD_EDIT_EXIT: return true;
    default: return EditCmdEnabled(kDefs[k].cmd, why);  // the context matrix (§8.1)
    }
}
// the format or block at the caret is on (§2.3's "active" look) - and a button whose popover is open
bool Active(int k) {
    UINT cmd = kDefs[k].cmd;
    if (cmd == CMD_TOC) return g.tocOpen;
    return (g_pop && g_pop == cmd) || EditCmdActive(cmd);
}

// "Ctrl+Shift+O"; an OEM key by its name on the current layout (Ctrl+Ё on the Russian one, UX-12)
std::wstring KeyLabel(const Def& d) {
    if (!d.vk) return L"";
    std::wstring s;
    if (d.mods & 1) s += L"Ctrl+";
    if (d.mods & 2) s += L"Shift+";
    if (d.vk == VK_ESCAPE) return s + L"Esc";
    if (d.vk == VK_OEM_3) {
        wchar_t n[32] = L"";
        LONG sc = (LONG)MapVirtualKeyW(d.vk, MAPVK_VK_TO_VSC) << 16;
        if (GetKeyNameTextW(sc, n, 32) > 0) {
            CharUpperW(n);  // (the layout names the key in lower case: «ё»; shortcuts read «Ctrl+Ё»)
            return s + n;
        }
        return s + L"`";
    }
    return s + (wchar_t)d.vk;
}

std::wstring TipOf(int k) {
    const Def& d = kDefs[k];
    std::wstring name = k == kStatus ? EditStatusTip() : std::wstring(Tr(d.tip));
    int why = 0;
    if (!Enabled(k, &why) && why) {  // greyed: why (§2.3, «Полужирный — недоступно в блоке кода»)
        static const StrId kWhy[] = {S_ED_NA_TABLE_FMT, S_ED_NA_CODE_FMT, S_ED_NA_OBJECT_FMT, S_ED_NA_FOOTNOTE_FMT, S_ED_NA_RAW_FMT,
                                     S_ED_NA_TASK_FMT};
        wchar_t b[256];
        swprintf_s(b, Tr(kWhy[std::clamp(why, 1, 6) - 1]), name.c_str());
        return b;
    }
    std::wstring keys = KeyLabel(d);
    return keys.empty() ? name : name + L" (" + keys + L")";
}

wchar_t StatusIcon(SaveState st, uint8_t* pal) {
    *pal = P_MUTED;
    switch (st) {
    case SS_SAVED: return 0xE73E;
    case SS_SAVING: return 0xE895;
    case SS_PENDING: case SS_OFF: return 0;  // a dot
    default: *pal = P_ALERT_CAUTION; return 0xE7BA;
    }
}
StrId StatusText(SaveState st) {
    switch (st) {
    case SS_SAVED: return S_ED_ST_SAVED;
    case SS_PENDING: return S_ED_ST_PENDING;
    case SS_SAVING: return S_ED_ST_SAVING;
    case SS_BUSY: return S_ED_ST_BUSY;
    case SS_DENIED: case SS_READONLY: return S_ED_ST_DENIED;
    case SS_MISSING: return S_ED_ST_MISSING;
    case SS_CONFLICT: return S_ED_ST_CONFLICT;
    case SS_UNENCODABLE: return S_ED_ST_ENCODING;
    case SS_UNKNOWN: return S_ED_ST_UNKNOWN;
    case SS_OFF: return S_ED_ST_OFF;
    default: return S_ED_ST_FAILED;
    }
}

// one line of UI text, vertically centred in [top, top + h), cut with "…" at maxW
void DrawText1(const std::wstring& s, float x, float top, float h, float maxW, float size, uint8_t pal, bool semibold = false) {
    IDWriteTextLayout* L = UiText(s, size, semibold);
    if (!L) return;
    L->SetMaxWidth(std::max(10.f, maxW));
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

void DrawButtonBack(float l, float t, float r, float b, bool hot, bool active, bool enabled) {
    const float rad = 6.f * U();
    if (HighContrast()) {
        if (active) g.canvas->FillRoundRect(l, t, r, b, rad, P_ACCENT);
        else if (hot && enabled) g.canvas->StrokeRoundRect(l, t, r, b, rad, 1.f * U(), P_OVERLAY_TEXT);
        return;
    }
    if (active) {
        g.canvas->FillRoundRect(l, t, r, b, rad, P_CURRENT);
        g.canvas->StrokeRoundRect(l, t, r, b, rad, 1.f * U(), P_ACCENT);
    } else if (hot && enabled) {
        g.canvas->FillRoundRect(l, t, r, b, rad, P_HOVER);
    }
}
uint8_t GlyphPal(bool hot, bool active, bool enabled) {
    if (HighContrast()) return active ? P_ONACCENT : !enabled ? P_MUTED : P_OVERLAY_TEXT;
    return active ? P_ACCENT : !enabled ? P_BORDER : hot ? P_TEXT : P_MUTED;
}

void DrawBar() {
    const float u = U(), top = BarTop(), h = kBarH * u, bt = top + (kBarH - kBtn) * 0.5f * u, bb = bt + kBtn * u;
    const float l = DocLeft(), r = ViewW();
    g.canvas->FillRect(l, top, r, top + h, P_OVERLAY_BG);
    g.canvas->FillRect(l, top + h - 1.f * u, r, top + h, HighContrast() ? P_OVERLAY_TEXT : P_OVERLAY_BORDER);
    Layout L = Compute(Level());
    for (float x : L.dividers)
        g.canvas->FillRect(x, top + (kBarH - 18.f) * 0.5f * u, x + 1.f * u, top + (kBarH + 18.f) * 0.5f * u, P_BORDER);
    for (int k = 0; k < kCount; k++) {
        const Item& it = L.items[k];
        if (!it.shown) continue;
        const Def& d = kDefs[k];
        bool en = Enabled(k), act = Active(k), hot = k == g_hot;
        if (k == kStatus && L.level < 1) {
            // The status text: the slot is as wide as its short states; a longer one (why a save failed) grows to the
            // left into the free space - the slot's right edge and every button stay where they are - and is cut with
            // "…" only where that space ends (the tooltip has it whole, §2.6)
            SaveState st = EditStatusShown();
            std::wstring text = Tr(StatusText(st));
            const float textR = it.r - 4.f * u;
            float x = std::max(L.leftEnd + 8.f * u, std::min(it.l + 8.f * u, textR - TextW(text, 12.5f * u)));
            DrawButtonBack(std::min(it.l, x - 8.f * u), bt, it.r, bb, hot, act, en);
            DrawText1(text, x, bt, kBtn * u, textR - x, 12.5f * u,
                      st >= SS_BUSY && st != SS_OFF ? P_ALERT_CAUTION : hot ? P_TEXT : P_MUTED);
            continue;
        }
        DrawButtonBack(it.l, bt, it.r, bb, hot, act, en);
        uint8_t pal = GlyphPal(hot, act, en);
        if (k == kStyle && L.level < 4) {  // the style of the caret's block, and a chevron
            DrawText1(StyleLabel(EditStyleId()), it.l + 8.f * u, bt, kBtn * u, it.r - it.l - 30.f * u, 13.f * u, pal);
            DrawIcon(0xE70D, it.r - 22.f * u - 2.f * u, bt, 22.f * u, 9.f * u, pal);
        } else if (k == kStatus) {  // collapsed to its icon (level >= 1)
            SaveState st = EditStatusShown();
            uint8_t sp;
            wchar_t icon = StatusIcon(st, &sp);
            if (hot) sp = st >= SS_BUSY && st != SS_OFF ? P_ALERT_CAUTION : P_TEXT;
            if (icon) DrawIcon(icon, it.l, bt, it.r - it.l, 14.f * u, sp);
            else g.canvas->FillCircle((it.l + it.r) * 0.5f, (bt + bb) * 0.5f, 3.f * u, P_ACCENT);
        } else if (d.cmd == CMD_LIST_NUMBER) {  // "1." in the UI font, semibold
            IDWriteTextLayout* T = UiText(L"1.", 13.f * u, true);
            if (T) {
                T->SetMaxWidth(it.r - it.l);
                T->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                DWRITE_TEXT_METRICS m{};
                T->GetMetrics(&m);
                g.canvas->Text(T, it.l, bt + (kBtn * u - m.height) * 0.5f, pal);
                T->Release();
            }
        } else {
            wchar_t glyph = d.mdl2 && Mdl2() ? d.mdl2 : d.glyph;
            float size = (d.cmd == CMD_QUOTE ? 16.f : d.cmd == CMD_EDIT_EXIT ? 13.f : 15.f) * u;
            DrawIcon(glyph, it.l, bt, it.r - it.l, size, pal);
        }
    }
}

// the tooltip of the hovered button: a pill under it, clamped to the window (the bottom-left pill stays empty); none
// while a popover hangs under the bar
void DrawBarTip() {
    if (g_hot < 0 || !BarShown() || g_pop) return;
    Layout L = Compute(Level());
    const Item& it = L.items[g_hot];
    if (!it.shown) return;
    const float u = U();
    std::wstring s = TipOf(g_hot);
    IDWriteTextLayout* T = UiText(s, 12.5f * u);
    if (!T) return;
    DWRITE_TEXT_METRICS m{};
    T->GetMetrics(&m);
    float w = std::ceil(m.width) + 20.f * u, h = 26.f * u;
    float x = std::clamp((it.l + it.r - w) * 0.5f, DocLeft() + 4.f * u, std::max(DocLeft() + 4.f * u, ViewW() - w - 4.f * u));
    float y = BarTop() + kBarH * u + 4.f * u;
    g.canvas->FillRoundRect(x, y, x + w, y + h, 8.f * u, P_OVERLAY_BG);
    g.canvas->StrokeRoundRect(x, y, x + w, y + h, 8.f * u, 1.f * u, HighContrast() ? P_OVERLAY_TEXT : P_OVERLAY_BORDER);
    g.canvas->Text(T, x + 10.f * u, y + (h - m.height) * 0.5f, P_OVERLAY_TEXT);
    T->Release();
}

// ------------------------------------------------------------------------------------------------ popovers (§2.4)
// Drawn on the canvas like the bar (a menu from TrackPopupMenu stays light in the dark theme): a panel under the button
// that opened it, rows of 28 u - or, for a table where the caret is not in one, a grid to pick its size from. Every
// row runs its command through Command(), as the bar's buttons do.
const float kRowH = 28.f, kPopPad = 4.f, kCell = 18.f, kCellGap = 3.f, kGridPad = 10.f, kGridLabel = 24.f;
const int kGridCols = 8, kGridRows = 10;
struct PopRow { UINT cmd, arg; std::wstring label, keys; wchar_t icon; bool semibold, current, enabled; };
bool g_grid = false;  // the table popover as the size grid
int g_popHot = -1;    // the hot row (from 0); the grid's hot cell: rows << 4 | columns (from 1)
float g_popL = 0;     // the left edge: under its button, or under "…" when it came from there

bool g_bubbleOn = false;  // the link bubble was drawn in the last frame
// drawn over the text - a tooltip, a popover, a popup, the bubble: frames in full
void OverText() { g.editOverText = g_hot >= 0 || g_pop || EditPopup() || g_bubbleOn; }

std::vector<PopRow> Rows() {
    std::vector<PopRow> v;
    auto add = [&](UINT cmd, UINT arg, std::wstring label, std::wstring keys = L"", wchar_t icon = 0, bool semibold = false) {
        v.push_back(PopRow{cmd, arg, std::move(label), std::move(keys), icon, semibold, false, EditCmdEnabled(cmd, nullptr)});
    };
    wchar_t b[64];
    switch (g_pop) {
    case CMD_BLOCK_MENU: {
        const int cur = EditStyleId();
        add(CMD_BLOCK_P, 0, Tr(S_ED_POP_TEXT));
        v.back().current = cur == 0;
        for (int n = 1; n <= 6; n++) {
            swprintf_s(b, Tr(S_ED_STYLE_HEADING_FMT), n);
            add(CMD_BLOCK_H1 + n - 1, 0, b, L"Ctrl+" + std::to_wstring(n), 0, true);
            v.back().current = cur == n;
        }
        break;
    }
    case CMD_TABLE_MENU:
        for (UINT k = 0; k < 10; k++) add(CMD_TABLE_ROW_ABOVE + k, 0, Tr((StrId)(S_ED_TBL_ROW_ABOVE + k)));
        break;
    case CMD_FORMULA_MENU:
        add(CMD_INS_FORMULA, 0, Tr(S_ED_FORMULA_INLINE), L"Ctrl+M");
        add(CMD_INS_FORMULA_BLOCK, 0, Tr(S_ED_FORMULA_BLOCK), L"Ctrl+Shift+M");
        break;
    case CMD_DIAGRAM_MENU:
        for (UINT k = 0; k < 9; k++) add(CMD_INS_DIAGRAM, k, Tr((StrId)(S_ED_DIA_FLOW + k)));
        break;
    case CMD_EDIT_MORE: {  // the buttons the bar has no room for, in its order
        const int level = Level();
        for (int k = 0; k < kCount; k++) {
            const Def& d = kDefs[k];
            if (d.group >= G_STATUS || Visible(k, level) || (d.cmd == CMD_TOC && !TocAvailable())) continue;
            add(d.cmd, 0, Tr(d.tip), KeyLabel(d), d.mdl2 && Mdl2() ? d.mdl2 : d.glyph);
            v.back().enabled = Enabled(k);
        }
        break;
    }
    }
    return v;
}

// the panel (client DIP)
struct PopBox { float l, t, r, b; };
PopBox Box(const std::vector<PopRow>& rows) {
    const float u = U();
    float w, h;
    if (g_grid) {
        w = (kGridCols * kCell + (kGridCols - 1) * kCellGap + 2 * kGridPad) * u;
        h = (kGridRows * kCell + (kGridRows - 1) * kCellGap + 2 * kGridPad + kGridLabel) * u;
    } else {
        float lw = 0, kw = 0;
        bool icons = false;
        for (const PopRow& r : rows) {
            lw = std::max(lw, TextW(r.label, 13.f * u, r.semibold));
            if (!r.keys.empty()) kw = std::max(kw, TextW(r.keys, 12.5f * u));
            icons |= r.icon || r.cmd == CMD_LIST_NUMBER;
        }
        w = std::max(180.f * u, (icons ? 28.f * u : 0.f) + lw + (kw > 0 ? kw + 28.f * u : 0.f) + 26.f * u);
        h = (rows.size() * kRowH + 2 * kPopPad) * u;
    }
    float l = std::clamp(g_popL, DocLeft() + 4.f * u, std::max(DocLeft() + 4.f * u, ViewW() - w - 4.f * u));
    float t = BarTop() + (kBarH + 2.f) * u;
    return PopBox{l, t, l + w, t + h};
}
// the grid's cell k (columns c, rows r from 1)
void CellRect(const PopBox& p, int c, int r, float* l, float* t) {
    const float u = U();
    *l = p.l + (kGridPad + (c - 1) * (kCell + kCellGap)) * u;
    *t = p.t + (kGridPad + (r - 1) * (kCell + kCellGap)) * u;
}
// the row (the grid's cell) under a point, -1 none
int PopHit(const PopBox& p, const std::vector<PopRow>& rows, float x, float y) {
    const float u = U();
    if (g_grid) {
        for (int r = 1; r <= kGridRows; r++)
            for (int c = 1; c <= kGridCols; c++) {
                float l, t;
                CellRect(p, c, r, &l, &t);
                if (x >= l - kCellGap * u * 0.5f && x < l + (kCell + kCellGap * 0.5f) * u && y >= t - kCellGap * u * 0.5f &&
                    y < t + (kCell + kCellGap * 0.5f) * u)
                    return r << 4 | c;
            }
        return -1;
    }
    int k = (int)std::floor((y - p.t - kPopPad * u) / (kRowH * u));
    return x >= p.l && x < p.r && k >= 0 && k < (int)rows.size() ? k : -1;
}

void DrawPopover() {
    if (!g_pop || !BarShown()) return;
    const float u = U();
    const bool hc = HighContrast();
    std::vector<PopRow> rows = Rows();
    PopBox p = Box(rows);
    g.canvas->FillRoundRect(p.l, p.t, p.r, p.b, 8.f * u, P_OVERLAY_BG);
    g.canvas->StrokeRoundRect(p.l, p.t, p.r, p.b, 8.f * u, 1.f * u, hc ? P_OVERLAY_TEXT : P_OVERLAY_BORDER);
    if (g_grid) {  // the size: the hovered range lit, "columns × rows" under it
        const int hr = g_popHot > 0 ? g_popHot >> 4 : 0, hcol = g_popHot > 0 ? g_popHot & 15 : 0;
        for (int r = 1; r <= kGridRows; r++)
            for (int c = 1; c <= kGridCols; c++) {
                float l, t;
                CellRect(p, c, r, &l, &t);
                bool on = r <= hr && c <= hcol;
                if (on) g.canvas->FillRoundRect(l, t, l + kCell * u, t + kCell * u, 3.f * u, hc ? P_ACCENT : P_CURRENT);
                g.canvas->StrokeRoundRect(l, t, l + kCell * u, t + kCell * u, 3.f * u, 1.f * u,
                                          on ? P_ACCENT : hc ? P_OVERLAY_TEXT : P_BORDER);
            }
        wchar_t b[64];
        if (hr) swprintf_s(b, L"%d × %d", hcol, hr);
        DrawText1(hr ? std::wstring(b) : std::wstring(Tr(S_ED_TIP_TABLE)), p.l + kGridPad * u, p.b - (kGridPad + kGridLabel) * u,
                  kGridLabel * u, p.r - p.l - 2 * kGridPad * u, 13.f * u, hr ? P_OVERLAY_TEXT : P_MUTED);
        return;
    }
    bool icons = false;
    for (const PopRow& r : rows) icons |= r.icon || r.cmd == CMD_LIST_NUMBER;
    for (size_t k = 0; k < rows.size(); k++) {
        const PopRow& r = rows[k];
        const float t = p.t + (kPopPad + k * kRowH) * u, b = t + kRowH * u, l = p.l + 4.f * u, rr = p.r - 4.f * u;
        const bool hot = (int)k == g_popHot && r.enabled;
        if (r.current) g.canvas->FillRoundRect(l, t, rr, b, 6.f * u, hc ? P_ACCENT : P_CURRENT);
        if (hot) {
            if (hc) g.canvas->StrokeRoundRect(l, t, rr, b, 6.f * u, 1.f * u, r.current ? P_ONACCENT : P_OVERLAY_TEXT);
            else if (!r.current) g.canvas->FillRoundRect(l, t, rr, b, 6.f * u, P_HOVER);
        }
        uint8_t pal = !r.enabled ? (hc ? P_MUTED : P_BORDER) : hc && r.current ? P_ONACCENT : P_OVERLAY_TEXT;
        float x = p.l + 12.f * u;
        if (icons) {
            if (r.cmd == CMD_LIST_NUMBER) DrawText1(L"1.", x + 2.f * u, t, kRowH * u, 22.f * u, 13.f * u, pal, true);
            else if (r.icon) DrawIcon(r.icon, x - 4.f * u, t, 24.f * u, 14.f * u, pal);
            x += 28.f * u;
        }
        float kw = r.keys.empty() ? 0.f : TextW(r.keys, 12.5f * u);
        DrawText1(r.label, x, t, kRowH * u, p.r - x - (kw ? kw + 24.f * u : 12.f * u), 13.f * u, pal, r.semibold);
        if (kw) DrawText1(r.keys, p.r - 12.f * u - kw, t, kRowH * u, kw + 2.f * u, 12.5f * u, hc && r.current ? P_ONACCENT : P_MUTED);
    }
}

// ------------------------------------------------------------------------------------------------ popups (§9.1, §2.4)
// The panel of a source popup - under the object it edits (above when there is no room), as wide as the text column but
// at least 420 u, scrolling with the document - and of the link and code-language popovers, under their bar button: a
// title, the field boxes the EDITs of the input thread sit in, a picture's labels, the hint, the error line, buttons.
const float kFieldH = 28.f, kLineH = 18.f, kLabelH = 18.f, kPad = 12.f;
int g_popupHot = -1;  // the panel's hovered button
int g_bubbleHot = -1;

StrId TitleOf(PopupKind k) {
    switch (k) {
    case PK_FORMULA: return S_ED_POP_FORMULA;
    case PK_FORMULA_BLOCK: return S_ED_POP_FORMULA_BLOCK;
    case PK_DIAGRAM: return S_ED_POP_DIAGRAM;
    case PK_HTML: return S_ED_POP_HTML;
    case PK_FRONT: return S_ED_POP_FRONT;
    case PK_IMAGE: return S_ED_POP_IMAGE;
    case PK_LINK: return S_ED_TIP_LINK;
    default: return S_ED_POP_LANG;
    }
}
UINT OpenerOf(PopupKind k) { return k == PK_LINK ? CMD_LINK : CMD_BLOCK_MENU; }
StrId ButtonLabel(UINT cmd) {
    return cmd == CMD_INS_IMAGE ? S_ED_POP_CHOOSE : cmd == CMD_LINK_REMOVE ? S_ED_LINK_REMOVE : S_ED_POP_DONE;
}
void Rect4(float* r, float l, float t, float rr, float b) {
    r[0] = l;
    r[1] = t;
    r[2] = rr;
    r[3] = b;
}
// a button of the settings window's look (as the strips' are)
void DrawButton(const float* r, StrId label, bool hot) {
    const float u = U();
    const bool hc = HighContrast();
    g.canvas->FillRoundRect(r[0], r[1], r[2], r[3], 6.f * u, hot && !hc ? P_HOVER : P_PANEL);
    g.canvas->StrokeRoundRect(r[0], r[1], r[2], r[3], 6.f * u, (hot && hc ? 2.f : 1.f) * u, hc ? P_OVERLAY_TEXT : P_BORDER);
    DrawText1(Tr(label), r[0] + 14.f * u, r[1], r[3] - r[1], r[2] - r[0], 13.f * u, P_TEXT);
}
// text of several lines at a width (a link's notice), its height
float Wrapped(const std::wstring& s, float x, float y, float w, float size, uint8_t pal, bool draw) {
    IDWriteTextLayout* L = UiText(s, size);
    if (!L) return 0.f;
    L->SetMaxWidth(std::max(10.f, w));
    DWRITE_TEXT_METRICS m{};
    L->GetMetrics(&m);
    if (draw) g.canvas->Text(L, x, y, pal);
    L->Release();
    return std::ceil(m.height);
}
bool Inside(const float* r, float x, float y) { return x >= r[0] && x < r[2] && y >= r[1] && y < r[3]; }

// a row runs: the popover closes first (the command may open another one, or a dialog)
void RunRow(int hot) {
    if (g_grid) {
        if (hot <= 0) return;
        PopoverClose();
        Command(CMD_INS_TABLE, (UINT)((hot >> 4) << 4 | (hot & 15)));
        return;
    }
    std::vector<PopRow> rows = Rows();
    if (hot < 0 || hot >= (int)rows.size() || !rows[hot].enabled) return;
    UINT cmd = rows[hot].cmd, arg = rows[hot].arg;
    PopoverClose();
    Command(cmd, arg);
}

// ------------------------------------------------------------------------------------------------ the pencil (§2.1)
// Reading mode's way into edit mode besides F2 and a double click: left of the gear, the same size and look.
bool PencilRect(float* l, float* t, float* r, float* b) {
    if (g.firstFrame || g.path.empty() || g.loadFailed || g.findOpen || g.editing || g.barT > 0 || BenchActive())
        return false;
    *r = ViewW() - 48.f;
    *l = *r - 30.f;
    *t = 8.f;
    *b = 38.f;
    return true;
}

// ------------------------------------------------------------------------------------------------ strips (§2.5)
const float kStripH = 36.f, kSBtnH = 26.f, kSPad = 14.f, kSGap = 8.f;

struct StripButton { UINT cmd; StrId label; float l, t, r, b; };
uint32_t g_strips = 0;   // conditions that hold, one bit per StripId
int g_sHot = -1;         // hovered strip button (index)

int TopStrip() {
    for (int k = STRIP_CONFLICT; k <= STRIP_OTHER_WINDOW; k++)
        if (g_strips & (1u << k)) return k;
    return STRIP_NONE;
}

// what a strip says, in which colour, and its buttons from left to right
void Describe(int kind, std::wstring* text, uint8_t* accent, std::vector<StripButton>* btns) {
    btns->clear();
    *text = L"";
    *accent = P_ALERT_NOTE;
    switch (kind) {
    case STRIP_CONFLICT:
        *accent = P_ALERT_WARNING;
        *text = EditStripText(kind);
        *btns = {{CMD_CONFLICT_LOAD, S_ED_CONFLICT_LOAD}, {CMD_CONFLICT_KEEP, S_ED_CONFLICT_KEEP}};
        break;
    case STRIP_ENCODING:
        *accent = P_ALERT_WARNING;
        *text = EditStripText(kind);
        if (text->rfind(Tr(S_ED_BOM_LOOKALIKE), 0) == 0) *btns = {{CMD_ENC_REMOVE_CHAR, S_ED_ENC_REMOVE}};
        else *btns = {{CMD_ENC_UTF8, S_ED_ENC_UTF8}, {CMD_ENC_REMOVE_CHAR, S_ED_ENC_REMOVE}};
        break;
    case STRIP_LEAVE:
        *accent = P_ALERT_CAUTION;
        *text = EditStripText(kind);
        *btns = {{CMD_SAVE_RETRY, S_ED_RETRY}, {CMD_SAVE_AS, S_ED_SAVE_AS_BTN}, {CMD_DISCARD_EDITS, S_ED_DISCARD}};
        break;
    case STRIP_READONLY:
        *text = Tr(S_ED_READONLY);
        *btns = {{CMD_SAVE_AS, S_ED_SAVE_AS_BTN}};
        break;
    case STRIP_MISSING:
        *accent = P_ALERT_CAUTION;
        *text = Tr(S_ED_MISSING);
        *btns = {{CMD_SAVE_AS, S_ED_SAVE_AS_BTN}};
        break;
    case STRIP_RECOVERY:
        *accent = P_ALERT_WARNING;
        *text = EditStripText(kind);
        if (EditRecoveryRestorable())
            *btns = {{CMD_RECOVERY_OPEN, S_ED_RECOVERY_OPEN}, {CMD_RECOVERY_RESTORE, S_ED_RECOVERY_RESTORE},
                     {CMD_RECOVERY_DELETE, S_ED_RECOVERY_DELETE}};
        else  // the file moved on since: putting the old bytes back would lose what came after, a copy cannot
            *btns = {{CMD_RECOVERY_OPEN, S_ED_RECOVERY_OPEN}, {CMD_RECOVERY_DELETE, S_ED_RECOVERY_DELETE}};
        break;
    case STRIP_OTHER_WINDOW:
        *text = Tr(S_ED_OTHER_WINDOW);
        *btns = {{CMD_OTHER_WINDOW, S_ED_GO_TO_IT}, {CMD_STRIP_CLOSE, S_ED_READ_ONLY_BTN}};
        break;
    default: break;
    }
}

// A strip spans the bar's width. In reading mode it leaves the corner buttons free - the outline button at the left,
// the pencil and the gear at the right - so they stay in sight and take their clicks while it is shown.
void StripSpan(float* l, float* r) {
    *l = DocLeft();
    *r = ViewW();
    if (g.barT > 0) return;
    float bl, bt, br, bb;
    if (TocButtonRect(&bl, &bt, &br, &bb)) *l = std::max(*l, br + 6.f);
    if (PencilRect(&bl, &bt, &br, &bb)) *r = std::min(*r, bl - 6.f);
    if (SettingsButtonRect(&bl, &bt, &br, &bb)) *r = std::min(*r, bl - 6.f);
}

// the buttons, right-aligned at the strip's end (client DIP)
std::vector<StripButton> Buttons(int kind) {
    std::wstring text;
    uint8_t accent;
    std::vector<StripButton> b;
    Describe(kind, &text, &accent, &b);
    const float u = U(), top = StripTop();
    float sl, sr;
    StripSpan(&sl, &sr);
    float x = sr - kSPad * u, t = top + (kStripH - kSBtnH) * 0.5f * u;
    for (size_t k = b.size(); k-- > 0;) {
        float w = TextW(Tr(b[k].label), 13.f * u) + 28.f * u;
        b[k].l = x - w;
        b[k].r = x;
        b[k].t = t;
        b[k].b = t + kSBtnH * u;
        x = b[k].l - kSGap * u;
    }
    return b;
}

void SetStripH() {
    float h = TopStrip() != STRIP_NONE ? kStripH * U() : 0.f;
    if (h != g.stripH) {
        g.stripH = h;
        FindRelayoutInput();  // the find bar sits under the strip
    }
}

void DrawStrip() {
    int kind = TopStrip();
    if (kind == STRIP_NONE || g.firstFrame) return;
    std::wstring text;
    uint8_t accent;
    std::vector<StripButton> unused;
    Describe(kind, &text, &accent, &unused);
    std::vector<StripButton> b = Buttons(kind);
    const float u = U(), top = StripTop(), h = kStripH * u;
    float l, r;
    StripSpan(&l, &r);
    bool hc = HighContrast();
    g.canvas->FillRect(l, top, r, top + h, P_OVERLAY_BG);
    g.canvas->FillRect(l, top + h - 1.f * u, r, top + h, hc ? P_OVERLAY_TEXT : P_OVERLAY_BORDER);
    g.canvas->FillRect(l, top, l + 3.f * u, top + h, accent);
    for (size_t k = 0; k < b.size(); k++) {  // the settings window's "Button" look
        bool hot = (int)k == g_sHot;
        g.canvas->FillRoundRect(b[k].l, b[k].t, b[k].r, b[k].b, 6.f * u, hot && !hc ? P_HOVER : P_PANEL);
        g.canvas->StrokeRoundRect(b[k].l, b[k].t, b[k].r, b[k].b, 6.f * u, (hot && hc ? 2.f : 1.f) * u,
                                  hc ? P_OVERLAY_TEXT : P_BORDER);
        DrawText1(Tr(b[k].label), b[k].l + 14.f * u, b[k].t, kSBtnH * u, b[k].r - b[k].l, 13.f * u, P_TEXT);
    }
    float textR = b.empty() ? r - kSPad * u : b.front().l - kSGap * u;
    DrawText1(text, l + kSPad * u, top, h, textR - (l + kSPad * u), 13.f * u, P_OVERLAY_TEXT);
}
}  // namespace

// ------------------------------------------------------------------------------------------------ strips: API
void StripShow(int kind) {
    if (kind <= STRIP_NONE || kind > STRIP_OTHER_WINDOW) return;
    uint32_t was = g_strips;
    g_strips |= 1u << kind;
    if (was == g_strips) return;
    g_sHot = -1;
    SetStripH();
    BarChanged();
    UiaChromeChanged();
}

void StripHide(int kind) {
    if (kind <= STRIP_NONE || kind > STRIP_OTHER_WINDOW || !(g_strips & (1u << kind))) return;
    g_strips &= ~(1u << kind);
    g_sHot = -1;
    SetStripH();
    BarChanged();
    UiaChromeChanged();
}

void StripHideEditing() {
    for (int k : {STRIP_CONFLICT, STRIP_ENCODING, STRIP_LEAVE, STRIP_READONLY, STRIP_MISSING}) StripHide(k);
}

void StripRelayout() {
    SetStripH();
    BarChanged();
}

// The strip's text, whole, while the pointer is over it and it did not fit beside the buttons (the conflict strip's
// sizes at 1000 px): shown in the tooltip pill, as the status slot's full text is (§2.5, §2.6)
std::wstring StripTipAt(float x, float y) {
    int kind = TopStrip();
    float top = StripTop(), l, r;
    StripSpan(&l, &r);
    if (!kind || g.firstFrame || x < l || x >= r || y < top || y >= top + g.stripH) return L"";
    std::wstring text;
    uint8_t accent;
    std::vector<StripButton> unused;
    Describe(kind, &text, &accent, &unused);
    std::vector<StripButton> b = Buttons(kind);
    const float u = U(), textL = l + kSPad * u, textR = b.empty() ? r - kSPad * u : b.front().l - kSGap * u;
    if (x >= textR || TextW(text, 13.f * u) <= textR - textL) return L"";
    return text;
}

int StripKind() { return TopStrip(); }

float StripTop() { return g.barT > 0 ? EditInset() : 0.f; }

LRESULT StripButtonCenter(UINT cmd) {
    int kind = TopStrip();
    if (!kind || g.firstFrame) return -1;
    float s = Scale();
    for (const StripButton& b : Buttons(kind))
        if (b.cmd == cmd) return MAKELONG(std::lround((b.l + b.r) * 0.5f * s), std::lround((b.t + b.b) * 0.5f * s));
    return -1;
}

bool StripMouse(float x, float y, bool click) {
    int kind = TopStrip();
    float top = StripTop(), l, r;
    StripSpan(&l, &r);
    if (!kind || g.firstFrame || x < l || x >= r || y < top || y >= top + g.stripH) {
        if (g_sHot >= 0) { g_sHot = -1; BarChanged(); }
        return false;
    }
    std::vector<StripButton> b = Buttons(kind);
    int hot = -1;
    for (size_t k = 0; k < b.size(); k++)
        if (x >= b[k].l && x < b[k].r && y >= b[k].t && y < b[k].b) hot = (int)k;
    if (hot != g_sHot) { g_sHot = hot; BarChanged(); }
    SetCursor(LoadCursorW(nullptr, hot >= 0 ? IDC_HAND : IDC_ARROW));
    if (click && hot >= 0) Command(b[hot].cmd);
    return true;  // the strip covers the document: nothing under it gets the mouse
}

// ------------------------------------------------------------------------------------------------ the bar: API
void BarChanged() {
    g.editChrome++;
    Invalidate();
}

bool BarMouse(float x, float y, bool click) {
    if (!BarShown()) {
        if (g_hot >= 0) { g_hot = -1; OverText(); BarChanged(); }
        return false;
    }
    const float u = U(), top = BarTop();
    if (x < DocLeft() || y < top || y >= top + kBarH * u) {
        if (g_hot >= 0) { g_hot = -1; OverText(); BarChanged(); }
        return false;
    }
    Layout L = Compute(Level());
    const float bt = top + (kBarH - kBtn) * 0.5f * u, bb = bt + kBtn * u;
    int hot = -1;
    for (int k = 0; k < kCount; k++)
        if (L.items[k].shown && x >= L.items[k].l && x < L.items[k].r && y >= bt && y < bb) hot = k;
    if (hot != g_hot) {
        g_hot = hot;
        OverText();  // the tooltip is drawn over the text: full frames while it shows
        BarChanged();
    }
    SetCursor(LoadCursorW(nullptr, hot >= 0 && Enabled(hot) ? IDC_HAND : IDC_ARROW));
    if (click && hot >= 0 && Enabled(hot)) Command(kDefs[hot].cmd);
    return true;
}

bool PencilMouse(float x, float y, bool click) {
    float l, t, r, b;
    bool on = PencilRect(&l, &t, &r, &b) && x >= l && x < r && y >= t && y < b;
    if (on != g.pencilHot) {
        g.pencilHot = on;
        BarChanged();
    }
    if (on && click) Command(CMD_EDIT_TOGGLE);
    return on;
}

void BarMouseLeave() {
    if (g_hot >= 0 || g.pencilHot || g_sHot >= 0 || (g_pop && g_popHot >= 0 && g_grid)) {
        g_hot = g_sHot = -1;
        if (g_grid) g_popHot = -1;  // (a list popover keeps its hot row for the keyboard)
        g.pencilHot = false;
        OverText();
        BarChanged();
    }
}

std::wstring BarTipText() { return g_hot >= 0 && BarShown() && !g_pop ? TipOf(g_hot) : std::wstring(); }

// ------------------------------------------------------------------------------------------------ popovers: API
bool PopoverOpen() { return g_pop != 0; }

void PopoverToggle(UINT cmd) {
    if (g_pop == cmd) {
        PopoverClose();
        return;
    }
    if (!BarShown()) return;
    Layout L = Compute(Level());
    int k = 0;  // under its button - or under "…" when the bar had no room for it
    while (k < kCount && kDefs[k].cmd != cmd) k++;
    if (k >= kCount || !L.items[k].shown) k = kMore;
    g_popL = L.items[k].shown ? L.items[k].l : ViewW() - 200.f * U();
    g_pop = cmd;
    g_grid = cmd == CMD_TABLE_MENU && !EditCmdEnabled(CMD_TABLE_DEL, nullptr);  // not in a table: its size
    g_popHot = -1;
    std::vector<PopRow> rows = Rows();  // the keyboard starts at the current value
    for (size_t i = 0; i < rows.size() && !g_grid; i++)
        if (rows[i].current) g_popHot = (int)i;
    OverText();
    BarChanged();
    UiaChromeChanged();
}

void PopoverClose() {
    if (!g_pop) return;
    g_pop = 0;
    g_popHot = -1;
    g_grid = false;
    OverText();
    BarChanged();
    UiaChromeChanged();
}

bool PopoverKey(unsigned vk) {
    if (!g_pop) return false;
    if (vk == VK_ESCAPE) {
        PopoverClose();
        return true;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
        RunRow(g_popHot);
        return true;
    }
    if (g_grid) {  // the size, a cell at a time (from 1 × 1)
        int r = g_popHot > 0 ? g_popHot >> 4 : 0, c = g_popHot > 0 ? g_popHot & 15 : 0;
        if (vk != VK_LEFT && vk != VK_RIGHT && vk != VK_UP && vk != VK_DOWN) return false;
        if (!r) r = c = 1;
        else {
            c = std::clamp(c + (vk == VK_RIGHT) - (vk == VK_LEFT), 1, kGridCols);
            r = std::clamp(r + (vk == VK_DOWN) - (vk == VK_UP), 1, kGridRows);
        }
        g_popHot = r << 4 | c;
        BarChanged();
        return true;
    }
    std::vector<PopRow> rows = Rows();
    const int n = (int)rows.size();
    if (!n || (vk != VK_UP && vk != VK_DOWN && vk != VK_HOME && vk != VK_END)) return vk == VK_LEFT || vk == VK_RIGHT;
    const int dir = vk == VK_UP || vk == VK_END ? -1 : 1;
    int k = vk == VK_HOME ? -1 : vk == VK_END ? n : g_popHot >= 0 && g_popHot < n ? g_popHot : dir > 0 ? -1 : n;
    for (int i = 0; i < n; i++) {  // the next enabled row, round the ends
        k = ((k + dir) % n + n) % n;
        if (rows[k].enabled) break;
    }
    g_popHot = k;
    BarChanged();
    return true;
}

bool PopoverMouse(float x, float y, bool click) {
    if (!g_pop) return false;
    if (!BarShown()) {
        PopoverClose();
        return false;
    }
    std::vector<PopRow> rows = Rows();
    PopBox p = Box(rows);
    if (x < p.l || x >= p.r || y < p.t || y >= p.b) {
        if (click) {  // a click outside closes it; one on the button that opened it only closes it
            Layout L = Compute(Level());
            const float u = U(), bt = BarTop() + (kBarH - kBtn) * 0.5f * u;
            bool opener = false;
            for (int k = 0; k < kCount; k++)
                opener |= L.items[k].shown && (kDefs[k].cmd == g_pop || (k == kMore && L.items[k].l == g_popL)) &&
                          x >= L.items[k].l && x < L.items[k].r && y >= bt && y < bt + kBtn * u;
            PopoverClose();
            return opener;
        }
        if (g_grid && g_popHot >= 0) {
            g_popHot = -1;
            BarChanged();
        }
        return false;
    }
    int hot = PopHit(p, rows, x, y);
    if (hot != g_popHot && (hot >= 0 || g_grid)) {
        g_popHot = hot;
        BarChanged();
    }
    bool en = g_grid ? hot > 0 : hot >= 0 && rows[hot].enabled;
    SetCursor(LoadCursorW(nullptr, en ? IDC_HAND : IDC_ARROW));
    if (click && en) RunRow(hot);
    return true;
}

// ------------------------------------------------------------------------------------------------ popups: API
bool PopupGeometry(PopupRects* o) {
    const PopupView* v = EditPopup();
    *o = PopupRects{};
    if (!v || !g.editing || g.firstFrame) return false;
    const float u = U(), pad = kPad * u;
    const bool source = v->kind < PK_LINK, image = v->kind == PK_IMAGE;
    float W = source ? std::min(std::max(g.textW, 420.f * u), std::max(g.availW, 240.f * u)) : (v->notice.empty() ? 360.f : 440.f) * u;
    W = std::max(160.f * u, std::min(W, ViewW() - DocLeft() - 24.f * u));
    // the rows, from the panel's top: the title, the fields (a picture's with their labels), a notice, the hint, the
    // error line, a popover's buttons
    float y = pad + 26.f * u, f0[4], f1[4] = {};
    if (image) {
        o->label[0] = y;
        y += kLabelH * u;
        Rect4(f0, pad, y, W - pad, y + kFieldH * u);
        o->label[1] = y += kFieldH * u + 8.f * u;
        y += kLabelH * u;
        const float bw = TextW(Tr(S_ED_POP_CHOOSE), 13.f * u) + 28.f * u;
        Rect4(f1, pad, y, W - pad - bw - 8.f * u, y + kFieldH * u);
        o->buttons = 1;
        o->cmd[0] = CMD_INS_IMAGE;
        Rect4(o->btn[0], W - pad - bw, y, W - pad, y + kFieldH * u);
        y += kFieldH * u;
    } else {
        const int lines = source ? std::clamp(v->lines, v->kind == PK_FORMULA ? 1 : 3, 16) : 0;
        Rect4(f0, pad, y, W - pad, y + (source ? lines * kLineH + 10.f : kFieldH) * u);
        y = f0[3];
    }
    if (!v->notice.empty()) {
        o->notice = y += 6.f * u;
        y += Wrapped(v->notice, 0, 0, W - 2 * pad, 12.f * u, 0, false);
    }
    if (source) {
        o->hint = y += 4.f * u;
        y += kLabelH * u;
    }
    if (!v->error.empty()) {
        o->error = y;
        y += kLabelH * u;
    }
    if (!source) {  // [Remove link] [Done], at the right
        y += 10.f * u;
        float x = W - pad;
        const UINT cmds[2] = {CMD_POPUP_DONE, CMD_LINK_REMOVE};
        for (int k = 0; k < (v->remove ? 2 : 1); k++) {
            const float bw = TextW(Tr(ButtonLabel(cmds[k])), 13.f * u) + 28.f * u;
            o->cmd[k] = cmds[k];
            Rect4(o->btn[k], x - bw, y, x, y + kFieldH * u);
            x -= bw + 8.f * u;
            o->buttons = k + 1;
        }
        y += kFieldH * u;
    }
    const float H = y + pad;
    // where: under the object it edits (above when there is no room below), or under its popover's button
    float l, t;
    if (source) {
        float a[4];
        if (!EditPopupAnchor(a) || a[3] < EditRevealTop() || a[1] > ViewH()) {
            o->off = true;
            return true;
        }
        l = a[0];
        t = a[3] + 6.f * u;
        if (t + H > ViewH() - 8.f * u && a[1] - 6.f * u - H >= EditRevealTop()) t = a[1] - 6.f * u - H;
        // an object taller than the room on either side (a long front matter): the panel over its lower part
        t = std::max(std::min(t, ViewH() - 8.f * u - H), EditRevealTop() + 4.f * u);
    } else {
        Layout L = Compute(Level());
        int k = 0;
        while (k < kCount && kDefs[k].cmd != OpenerOf(v->kind)) k++;
        if (k >= kCount || !L.items[k].shown) k = kMore;
        l = L.items[k].shown ? L.items[k].l : ViewW() - W;
        t = BarTop() + (kBarH + 2.f) * u;
    }
    l = std::clamp(l, DocLeft() + 8.f * u, std::max(DocLeft() + 8.f * u, ViewW() - W - 16.f * u));
    o->l = l;
    o->t = t;
    o->r = l + W;
    o->b = t + H;
    auto place = [&](float* r) {
        r[0] += l;
        r[2] += l;
        r[1] += t;
        r[3] += t;
    };
    place(f0);
    memcpy(o->field[0], f0, sizeof f0);
    if (image) {
        place(f1);
        memcpy(o->field[1], f1, sizeof f1);
    }
    for (int k = 0; k < o->buttons; k++) place(o->btn[k]);
    for (float* r : {&o->label[0], &o->label[1], &o->notice, &o->hint, &o->error}) *r += t;
    for (int f = 0; f < (image ? 2 : 1); f++) {  // the EDIT inside its box (a multi-line one's scrollbar at the edge)
        const float* b = o->field[f];
        if (v->multi) Rect4(o->edit[f], b[0] + 6.f * u, b[1] + 5.f * u, b[2] - 2.f * u, b[3] - 5.f * u);
        else Rect4(o->edit[f], b[0] + 8.f * u, b[1] + 2.f * u, b[2] - 8.f * u, b[3] - 2.f * u);
    }
    return true;
}

bool PopupMouse(float x, float y, bool click) {
    PopupRects p;
    if (!PopupGeometry(&p) || p.off) return false;
    if (x < p.l || x >= p.r || y < p.t || y >= p.b) {
        if (g_popupHot >= 0) {
            g_popupHot = -1;
            BarChanged();
        }
        if (!click) return false;
        const PopupView* v = EditPopup();
        bool opener = false;  // a popover's own button only closes it
        if (v->kind >= PK_LINK) {
            Layout L = Compute(Level());
            const float u = U(), bt = BarTop() + (kBarH - kBtn) * 0.5f * u;
            for (int k = 0; k < kCount; k++)
                opener |= L.items[k].shown && kDefs[k].cmd == OpenerOf(v->kind) && x >= L.items[k].l && x < L.items[k].r &&
                          y >= bt && y < bt + kBtn * u;
        }
        EditPopupClickOutside(opener);  // a source popup keeps its text, a popover closes
        return opener;
    }
    int hot = -1;
    for (int k = 0; k < p.buttons; k++)
        if (Inside(p.btn[k], x, y)) hot = k;
    if (hot != g_popupHot) {
        g_popupHot = hot;
        BarChanged();
    }
    SetCursor(LoadCursorW(nullptr, hot >= 0 ? IDC_HAND : IDC_ARROW));
    if (click && hot >= 0) Command(p.cmd[hot]);
    return true;
}

// The panel on the canvas: its frame, title, the boxes the fields' EDITs sit in (while the EDITs are away because the
// panel moves, their text drawn there), a picture's labels, the notice, the hint, the error line, the buttons. Then the
// EDITs are told where the boxes are now.
static void DrawPopup() {
    PopupRects p;
    if (!PopupGeometry(&p)) return;
    if (p.off) {  // the object went out of sight: the popup closes, keeping its text (§9.2)
        EditPopupPlaced(p.edit, true);
        return;
    }
    const PopupView* v = EditPopup();
    const float u = U(), pad = kPad * u, w = p.r - p.l - 2 * pad;
    const bool hc = HighContrast();
    g.canvas->FillRoundRect(p.l, p.t, p.r, p.b, 8.f * u, P_OVERLAY_BG);
    g.canvas->StrokeRoundRect(p.l, p.t, p.r, p.b, 8.f * u, 1.f * u, hc ? P_OVERLAY_TEXT : P_OVERLAY_BORDER);
    DrawText1(Tr(TitleOf(v->kind)), p.l + pad, p.t + pad, 20.f * u, w, 13.f * u, P_OVERLAY_TEXT, true);
    if (v->kind == PK_IMAGE)
        for (int f = 0; f < 2; f++) DrawText1(Tr(f ? S_ED_POP_PATH : S_ED_POP_ALT), p.l + pad, p.label[f], kLabelH * u, w, 12.f * u, P_MUTED);
    for (int f = 0; f < v->fields; f++) {
        const float *b = p.field[f], *e = p.edit[f];
        const bool focus = v->focus == f;
        g.canvas->FillRoundRect(b[0], b[1], b[2], b[3], 5.f * u, P_BG);
        g.canvas->StrokeRoundRect(b[0], b[1], b[2], b[3], 5.f * u, (focus ? 1.5f : 1.f) * u, focus ? P_ACCENT : hc ? P_OVERLAY_TEXT : P_BORDER);
        if (!v->hidden) continue;
        std::wstring t;
        PopupFieldText(f, &t);
        IDWriteTextLayout* L = nullptr;
        if (!v->multi) DrawText1(t, e[0], e[1], e[3] - e[1], e[2] - e[0], 13.f * u, P_TEXT);
        else if (SUCCEEDED(g.dwf->CreateTextLayout(t.data(), (UINT32)t.size(), g.typo.fmt[R_CODE], 1e5f, 1e5f, &L))) {
            L->SetFontSize(13.f * u, DWRITE_TEXT_RANGE{0, (UINT32)t.size()});
            g.canvas->PushClip(e[0], e[1], e[2], e[3]);
            g.canvas->Text(L, e[0] + 1.f * u, e[1], P_TEXT);
            g.canvas->PopClip();
            L->Release();
        }
    }
    if (!v->notice.empty()) Wrapped(v->notice, p.l + pad, p.notice, w, 12.f * u, P_OVERLAY_TEXT, true);
    if (v->kind < PK_LINK) DrawText1(Tr(S_ED_POP_HINT), p.l + pad, p.hint, kLabelH * u, w, 12.f * u, P_MUTED);
    if (!v->error.empty()) DrawText1(v->error, p.l + pad, p.error, kLabelH * u, w, 12.f * u, P_ALERT_CAUTION);
    for (int k = 0; k < p.buttons; k++) DrawButton(p.btn[k], ButtonLabel(p.cmd[k]), k == g_popupHot);
    EditPopupPlaced(p.edit, false);
}

// ---- the link bubble (§2.11): under the caret's link - its address cut to 48 characters, [Edit] [Remove] [Open]
namespace {
const UINT kBubbleCmd[3] = {CMD_LINK, CMD_LINK_REMOVE, CMD_LINK_OPEN};
const StrId kBubbleLabel[3] = {S_ED_BUBBLE_EDIT, S_ED_BUBBLE_REMOVE, S_ED_BUBBLE_OPEN};
struct Bubble { float l, t, r, b; float btn[3][4]; std::wstring dest; };
bool BubbleGeometry(Bubble* o) {
    float box[4];
    if (!BarShown() || !EditBubble(box, &o->dest)) return false;
    const float u = U(), h = 30.f * u;
    if (o->dest.size() > 48) o->dest = o->dest.substr(0, 47) + L"…";
    float W = 10.f * u + TextW(o->dest, 12.5f * u) + 10.f * u, bw[3];
    for (int k = 0; k < 3; k++) W += (bw[k] = TextW(Tr(kBubbleLabel[k]), 12.5f * u) + 16.f * u) + 2.f * u;
    W += 2.f * u;
    o->l = std::clamp(box[0], DocLeft() + 4.f * u, std::max(DocLeft() + 4.f * u, ViewW() - W - 16.f * u));
    o->t = std::min(box[1] + 4.f * u, ViewH() - h - 4.f * u);
    o->r = o->l + W;
    o->b = o->t + h;
    float x = o->r - 4.f * u;
    for (int k = 3; k-- > 0;) {
        Rect4(o->btn[k], x - bw[k], o->t + 3.f * u, x, o->b - 3.f * u);
        x -= bw[k] + 2.f * u;
    }
    return true;
}
void DrawBubble() {
    Bubble b;
    g_bubbleOn = BubbleGeometry(&b);
    if (!g_bubbleOn) return;
    const float u = U();
    const bool hc = HighContrast();
    g.canvas->FillRoundRect(b.l, b.t, b.r, b.b, 8.f * u, P_OVERLAY_BG);
    g.canvas->StrokeRoundRect(b.l, b.t, b.r, b.b, 8.f * u, 1.f * u, hc ? P_OVERLAY_TEXT : P_OVERLAY_BORDER);
    DrawText1(b.dest, b.l + 10.f * u, b.t, b.b - b.t, b.btn[0][0] - b.l - 16.f * u, 12.5f * u, P_MUTED);
    for (int k = 0; k < 3; k++) {
        const float* r = b.btn[k];
        if (k == g_bubbleHot) {
            if (hc) g.canvas->StrokeRoundRect(r[0], r[1], r[2], r[3], 6.f * u, 1.f * u, P_OVERLAY_TEXT);
            else g.canvas->FillRoundRect(r[0], r[1], r[2], r[3], 6.f * u, P_HOVER);
        }
        DrawText1(Tr(kBubbleLabel[k]), r[0] + 8.f * u, r[1], r[3] - r[1], r[2] - r[0], 12.5f * u, hc ? P_OVERLAY_TEXT : P_ACCENT);
    }
}
}  // namespace

bool BubbleMouse(float x, float y, bool click) {
    Bubble b;
    int hot = -1;
    const bool on = BubbleGeometry(&b) && x >= b.l && x < b.r && y >= b.t && y < b.b;
    for (int k = 0; on && k < 3; k++)
        if (Inside(b.btn[k], x, y)) hot = k;
    if (hot != g_bubbleHot) {
        g_bubbleHot = hot;
        BarChanged();
    }
    if (!on) return false;
    SetCursor(LoadCursorW(nullptr, hot >= 0 ? IDC_HAND : IDC_ARROW));
    if (click && hot >= 0) Command(kBubbleCmd[hot]);
    return true;
}

LRESULT BarToolCenter(UINT cmd, UINT row) {
    const float s = Scale();
    auto center = [&](const float* r) { return MAKELONG(std::lround((r[0] + r[2]) * 0.5f * s), std::lround((r[1] + r[3]) * 0.5f * s)); };
    PopupRects pr;  // a popup's buttons ([Choose file…]: CMD_INS_IMAGE with row 1); the bubble's (its [Edit]: CMD_LINK, row 1)
    if (PopupGeometry(&pr) && !pr.off)
        for (int k = 0; k < pr.buttons; k++)
            if (pr.cmd[k] == cmd && row == (cmd == CMD_INS_IMAGE ? 1u : 0u)) return center(pr.btn[k]);
    Bubble bb;
    if ((cmd == CMD_LINK_REMOVE || cmd == CMD_LINK_OPEN || (cmd == CMD_LINK && row == 1)) && BubbleGeometry(&bb))
        for (int k = 0; k < 3; k++)
            if (kBubbleCmd[k] == cmd) return center(bb.btn[k]);
    float l, t, r, b;
    if (row) {  // a popover's row (from 1) - the grid's cell: rows << 4 | columns
        if (!g_pop || g_pop != cmd || !BarShown()) return -1;
        std::vector<PopRow> rows = Rows();
        PopBox p = Box(rows);
        const float u = U();
        if (g_grid) {
            int rr = (int)(row >> 4), cc = (int)(row & 15);
            if (rr < 1 || rr > kGridRows || cc < 1 || cc > kGridCols) return -1;
            CellRect(p, cc, rr, &l, &t);
            return MAKELONG(std::lround((l + kCell * u * 0.5f) * s), std::lround((t + kCell * u * 0.5f) * s));
        }
        if (row > rows.size()) return -1;
        return MAKELONG(std::lround((p.l + p.r) * 0.5f * s), std::lround((p.t + (kPopPad + (row - 0.5f) * kRowH) * u) * s));
    }
    if (cmd == CMD_EDIT_TOGGLE && !g.editing) {
        if (!PencilRect(&l, &t, &r, &b)) return -1;
        return MAKELONG(std::lround((l + r) * 0.5f * s), std::lround((t + b) * 0.5f * s));
    }
    if (BarShown()) {
        Layout L = Compute(Level());
        const float u = U(), bt = BarTop() + (kBarH - kBtn) * 0.5f * u;
        for (int k = 0; k < kCount; k++)
            if (kDefs[k].cmd == cmd && L.items[k].shown)
                return MAKELONG(std::lround((L.items[k].l + L.items[k].r) * 0.5f * s), std::lround((bt + kBtn * u * 0.5f) * s));
    }
    return StripButtonCenter(cmd);
}

int BarCollapse() { return Level(); }

// UI Automation's view of the chrome (§12.8): what is on screen, left to right and top to bottom - the bar, the strip
// under it (or at the top in reading mode), the pencil
int EditChromeButtons(ChromeButton* out, int max) {
    int n = 0;
    auto add = [&](UINT cmd, float l, float t, float r, float b, bool en) {
        if (n < max) out[n++] = ChromeButton{cmd, l, t, r, b, en};
    };
    if (BarShown()) {
        Layout L = Compute(Level());
        const float u = U(), bt = BarTop() + (kBarH - kBtn) * 0.5f * u;
        for (int k = 0; k < kCount; k++)
            if (L.items[k].shown) add(kDefs[k].cmd, L.items[k].l, bt, L.items[k].r, bt + kBtn * u, Enabled(k));
        if (g_pop) {  // a popover's rows (the grid's cells), each with its argument
            std::vector<PopRow> rows = Rows();
            PopBox p = Box(rows);
            if (g_grid) {
                for (int r = 1; r <= kGridRows; r++)
                    for (int c = 1; c <= kGridCols; c++) {
                        float l, t;
                        CellRect(p, c, r, &l, &t);
                        add(CMD_INS_TABLE | (UINT)(r << 4 | c) << 16, l, t, l + kCell * u, t + kCell * u, true);
                    }
            } else {
                for (size_t k = 0; k < rows.size(); k++) {
                    float t = p.t + (kPopPad + k * kRowH) * u;
                    add(rows[k].cmd | rows[k].arg << 16, p.l, t, p.r, t + kRowH * u, rows[k].enabled);
                }
            }
        }
    }
    // a popup's buttons and the bubble's ([Choose file…] and the bubble's [Edit] carry an argument: the bar has buttons
    // of their commands too)
    PopupRects pr;
    if (PopupGeometry(&pr) && !pr.off)
        for (int k = 0; k < pr.buttons; k++)
            add(pr.cmd[k] | (pr.cmd[k] == CMD_INS_IMAGE ? 1u << 16 : 0u), pr.btn[k][0], pr.btn[k][1], pr.btn[k][2], pr.btn[k][3], true);
    Bubble bb;
    if (BubbleGeometry(&bb))
        for (int k = 0; k < 3; k++) add(kBubbleCmd[k] | (k ? 0u : 1u << 16), bb.btn[k][0], bb.btn[k][1], bb.btn[k][2], bb.btn[k][3], true);
    if (int kind = TopStrip(); kind && !g.firstFrame)
        for (const StripButton& b : Buttons(kind)) add(b.cmd, b.l, b.t, b.r, b.b, true);
    float l, t, r, b;
    if (PencilRect(&l, &t, &r, &b)) add(CMD_EDIT_TOGGLE, l, t, r, b, true);
    return n;
}

// a button's name is its tooltip without the shortcut, which is the accelerator key; the status slot's is the status
// in full, a strip button's its label
std::wstring EditChromeName(UINT cmd, std::wstring* keys) {
    keys->clear();
    if (cmd == CMD_EDIT_TOGGLE && !g.editing) {
        *keys = L"F2";
        return Tr(S_ED_PENCIL_TIP);
    }
    switch (cmd) {  // a popup's buttons, the bubble's
    case CMD_INS_IMAGE | 1u << 16: return Tr(S_ED_POP_CHOOSE);
    case CMD_LINK | 1u << 16: return Tr(S_ED_BUBBLE_EDIT);
    case CMD_POPUP_DONE: return Tr(S_ED_POP_DONE);
    case CMD_LINK_REMOVE: return Tr(EditPopup() ? S_ED_LINK_REMOVE : S_ED_BUBBLE_REMOVE);
    case CMD_LINK_OPEN: return Tr(S_ED_BUBBLE_OPEN);
    }
    if (g_pop) {  // a popover's row, by its command and argument; the grid's cell by its size
        if (g_grid && (cmd & 0xFFFF) == CMD_INS_TABLE && cmd >> 16) {
            wchar_t b[64];
            swprintf_s(b, Tr(S_ED_GRID_FMT), (int)(cmd >> 16 & 15), (int)(cmd >> 20));
            return b;
        }
        for (const PopRow& r : Rows())
            if ((r.cmd | r.arg << 16) == cmd && !g_grid) {
                *keys = r.keys;
                return r.label;
            }
    }
    for (int k = 0; k < kCount; k++)
        if (kDefs[k].cmd == cmd) {
            *keys = KeyLabel(kDefs[k]);
            return k == kStatus ? EditStatusTip() : std::wstring(Tr(kDefs[k].tip));
        }
    std::wstring text;
    uint8_t accent;
    std::vector<StripButton> b;
    Describe(TopStrip(), &text, &accent, &b);
    for (const StripButton& x : b)
        if (x.cmd == cmd) return Tr(x.label);
    return L"";
}

void DrawEditChrome(int layer) {
    if (g.firstFrame) return;
    if (layer == 1) {
        DrawPopover();
        DrawPopup();
        DrawBubble();
        DrawBarTip();
        OverText();
        return;
    }
    float l, t, r, b;
    if (PencilRect(&l, &t, &r, &b)) {
        bool hot = g.pencilHot;
        if (hot) {
            if (HighContrast()) g.canvas->StrokeRoundRect(l, t, r, b, 6.f, 1.f, P_OVERLAY_TEXT);
            else g.canvas->FillRoundRect(l, t, r, b, 6.f, P_HOVER);
        }
        DrawIcon(0xE70F, l, t, r - l, 15.f, hot ? P_TEXT : P_MUTED);  // Segoe Fluent Icons: Edit
    }
    DrawStrip();
    if (BarShown()) DrawBar();
}

int EditChromeRects(float (*rc)[4], int max) {
    int n = 0;
    auto add = [&](float l, float t, float r, float b) {
        if (n < max && r > l && b > t) { rc[n][0] = l; rc[n][1] = t; rc[n][2] = r; rc[n][3] = b; n++; }
    };
    if (BarShown()) add(DocLeft(), 0, ViewW(), BarTop() + kBarH * U());
    float l, t, r, b;
    StripSpan(&l, &r);
    if (TopStrip() != STRIP_NONE && !g.firstFrame) add(l, StripTop(), r, StripTop() + g.stripH);
    if (PencilRect(&l, &t, &r, &b)) add(l, t, r, b);
    return n;
}

// Back to the compiler's own inlining for the templates instantiated at the end of the file (see editcore.cpp's end)
#pragma inline_depth()
