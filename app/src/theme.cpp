#include "common.h"
#include "theme.h"

namespace {
const uint32_t kLight[] = {
    0x1f2328,  // P_DEFAULT (unused)
    0x1f2328,  // P_TEXT
    0x59636e,  // P_MUTED
    0x0969da,  // P_LINK
    0xd1d9e0,  // P_BORDER
    0xf6f8fa,  // P_CODEBG
    0xeff1f3,  // P_INLINEBG (rgba(129,139,152,.12) over white)
    0xf6f8fa,  // P_ZEBRA
    0x0969da,  // P_ACCENT (checked task box)
    0xffffff,  // P_ONACCENT
    0xffffff,  // P_BG
    0xc8cdd3,  // P_SCROLL
    0x8c959f,  // P_SCROLL_HOT
    0xf0f2f4,  // P_PLACEHOLDER
    0xb6d7fb,  // P_SELECTION
    0xfff1a8,  // P_FIND
    0xffa657,  // P_FIND_CUR
    0xffffff,  // P_OVERLAY_BG
    0x1f2328,  // P_OVERLAY_TEXT
    0xd1d9e0,  // P_OVERLAY_BORDER
    0xf6f8fa,  // P_PANEL
    0xe7ebef,  // P_HOVER (neutral.muted over the panel)
    0xddf4ff,  // P_CURRENT (accent.subtle)
    0xbf8700,  // P_MARK
    0x0969da,  // P_ALERT_NOTE
    0x1a7f37,  // P_ALERT_TIP
    0x8250df,  // P_ALERT_IMPORTANT
    0x9a6700,  // P_ALERT_WARNING
    0xd1242f,  // P_ALERT_CAUTION
    0xcf222e,  // P_KEYWORD
    0x0a3069,  // P_STRING
    0x59636e,  // P_COMMENT
    0x0550ae,  // P_CONST
    0x6639ba,  // P_FUNC
    0x953800,  // P_TYPE
    0x953800,  // P_VAR
};
const uint32_t kDark[] = {
    0xf0f6fc,  // P_DEFAULT (unused)
    0xf0f6fc,  // P_TEXT
    0x9198a1,  // P_MUTED
    0x4493f8,  // P_LINK
    0x3d444d,  // P_BORDER
    0x151b23,  // P_CODEBG
    0x262c36,  // P_INLINEBG (rgba(101,108,118,.2) over #0d1117)
    0x151b23,  // P_ZEBRA
    0x1f6feb,  // P_ACCENT
    0xffffff,  // P_ONACCENT
    0x0d1117,  // P_BG
    0x3d444d,  // P_SCROLL
    0x656c76,  // P_SCROLL_HOT
    0x151b23,  // P_PLACEHOLDER
    0x1f4a7a,  // P_SELECTION
    0x5c4a0e,  // P_FIND
    0x9e6a03,  // P_FIND_CUR
    0x1c2129,  // P_OVERLAY_BG
    0xf0f6fc,  // P_OVERLAY_TEXT
    0x3d444d,  // P_OVERLAY_BORDER
    0x010409,  // P_PANEL (canvas.inset)
    0x262c36,  // P_HOVER
    0x122844,  // P_CURRENT
    0xd29922,  // P_MARK
    0x4493f8,  // P_ALERT_NOTE
    0x3fb950,  // P_ALERT_TIP
    0xab7df8,  // P_ALERT_IMPORTANT
    0xd29922,  // P_ALERT_WARNING
    0xf85149,  // P_ALERT_CAUTION
    0xff7b72,  // P_KEYWORD
    0xa5d6ff,  // P_STRING
    0x9198a1,  // P_COMMENT
    0x79c0ff,  // P_CONST
    0xd2a8ff,  // P_FUNC
    0xffa657,  // P_TYPE
    0xffa657,  // P_VAR
};
static_assert(std::size(kLight) == P_COUNT && std::size(kDark) == P_COUNT, "one colour per Pal");
bool g_dark = false;
uint32_t kHigh[P_COUNT];  // built from the system colours when Windows is in high contrast

uint32_t Sys(int idx) {
    DWORD c = GetSysColor(idx);
    return ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);  // COLORREF is BGR, our palette is RGB
}

// High contrast (plan 6.2): the reader's own colours are replaced by the ones Windows was told to use, and syntax
// colouring goes away - in this mode the point is contrast, not prettiness.
void BuildHighContrast() {
    uint32_t bg = Sys(COLOR_WINDOW), text = Sys(COLOR_WINDOWTEXT), muted = Sys(COLOR_GRAYTEXT);
    uint32_t link = Sys(COLOR_HOTLIGHT), sel = Sys(COLOR_HIGHLIGHT), selText = Sys(COLOR_HIGHLIGHTTEXT);
    uint32_t panel = Sys(COLOR_BTNFACE), panelText = Sys(COLOR_BTNTEXT), border = Sys(COLOR_WINDOWFRAME);
    for (int i = 0; i < P_COUNT; i++) kHigh[i] = text;  // everything that draws text, including syntax colours
    kHigh[P_BG] = kHigh[P_CODEBG] = kHigh[P_INLINEBG] = kHigh[P_ZEBRA] = bg;
    kHigh[P_MUTED] = kHigh[P_COMMENT] = muted;
    kHigh[P_LINK] = link;
    kHigh[P_BORDER] = kHigh[P_OVERLAY_BORDER] = border;
    kHigh[P_ACCENT] = sel;
    kHigh[P_ONACCENT] = selText;
    kHigh[P_SCROLL] = muted;
    kHigh[P_SCROLL_HOT] = text;
    kHigh[P_PLACEHOLDER] = panel;
    kHigh[P_SELECTION] = sel;
    kHigh[P_FIND] = panel;
    kHigh[P_FIND_CUR] = sel;
    kHigh[P_MARK] = link;
    kHigh[P_OVERLAY_BG] = kHigh[P_PANEL] = panel;
    kHigh[P_OVERLAY_TEXT] = panelText;
    kHigh[P_HOVER] = kHigh[P_CURRENT] = panel;
}
}  // namespace

const uint32_t* g_pal = kLight;

bool SystemHighContrast() {
    HIGHCONTRASTW hc{sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

void SetDarkPalette(bool dark) {
    g_dark = dark;
    if (SystemHighContrast()) {
        BuildHighContrast();
        g_pal = kHigh;
        g_dark = (kHigh[P_BG] & 0xff) + ((kHigh[P_BG] >> 8) & 0xff) + ((kHigh[P_BG] >> 16) & 0xff) < 3 * 128;
        return;
    }
    g_pal = dark ? kDark : kLight;
}

bool PaletteIsHighContrast() { return g_pal == kHigh; }
bool PaletteIsDark() { return g_dark; }

bool SystemPrefersDark() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v == 0;
}
