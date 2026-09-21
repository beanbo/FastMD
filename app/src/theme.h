// Colour themes: GitHub light / dark palettes (github-markdown-css v5.9, see research/06 §8.2).
#pragma once
#include <cstdint>

enum Pal : uint8_t {
    P_DEFAULT = 0,  // "use the block's default colour"
    P_TEXT, P_MUTED, P_LINK, P_BORDER, P_CODEBG, P_INLINEBG, P_ZEBRA, P_ACCENT, P_ONACCENT, P_BG,
    P_SCROLL, P_SCROLL_HOT, P_PLACEHOLDER, P_SELECTION, P_FIND, P_FIND_CUR,
    P_OVERLAY_BG, P_OVERLAY_TEXT, P_OVERLAY_BORDER,
    P_PANEL,        // outline panel / settings sections
    P_HOVER,        // hovered list item / button
    P_CURRENT,      // current outline item, selected recent document
    P_MARK,         // find matches on the scrollbar
    // GitHub alerts: NOTE, TIP, IMPORTANT, WARNING, CAUTION (index = P_ALERT_NOTE + Alert - 1)
    P_ALERT_NOTE, P_ALERT_TIP, P_ALERT_IMPORTANT, P_ALERT_WARNING, P_ALERT_CAUTION,
    // syntax ("prettylights")
    P_KEYWORD, P_STRING, P_COMMENT, P_CONST, P_FUNC, P_TYPE, P_VAR,
    P_COUNT
};

enum ThemeMode : uint8_t { TM_SYSTEM = 0, TM_LIGHT, TM_DARK };

extern const uint32_t* g_pal;  // current palette, 0xRRGGBB per Pal
void SetDarkPalette(bool dark);
bool PaletteIsDark();
bool SystemPrefersDark();      // HKCU ...\Themes\Personalize\AppsUseLightTheme == 0
bool SystemHighContrast();     // Windows is in a high-contrast theme
bool PaletteIsHighContrast();  // the palette in force is built from the system colours
