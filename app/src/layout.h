// Typography + per-block layout (one IDWriteTextLayout per text block, a grid of layouts per table).
#pragma once
#include "doc.h"
#include <d2d1.h>

enum Role { R_BODY = 0, R_H1, R_H2, R_H3, R_H4, R_H5, R_H6, R_CODE, R_COUNT };

struct Metrics {  // product style (DIP), research/06 §8.2–8.3
    static constexpr float kTextMax = 680.f;   // text column (≈ 86–97 chars at 16 px, research/06 §8.3)
    static constexpr float kWideMax = 960.f;   // top-level code blocks / tables / images may break out to this
    static constexpr float kPadX = 32.f, kPadTop = 32.f, kPadBottom = 96.f;
    static constexpr float kCodePad = 16.f, kCellPadX = 13.f, kCellPadY = 6.f;
    static constexpr float kQuoteBar = 4.f;
};

struct Typography {
    IDWriteFactory3* factory = nullptr;
    IDWriteTextFormat* fmt[R_COUNT] = {};
    IDWriteTextFormat* ui = nullptr;      // overlays (status line, find bar)
    IDWriteTextFormat* uiIcon = nullptr;  // Segoe Fluent Icons for overlay buttons
    float size[R_COUNT] = {}, lineH[R_COUNT] = {}, baseline[R_COUNT] = {};
    float monoAscent = 0.8f, monoDescent = 0.2f;  // em ratios for inline code backgrounds
    wchar_t bodyFamily[64] = L"Segoe UI Variable Text";
    wchar_t displayFamily[64] = L"Segoe UI Variable Display";
    wchar_t monoFamily[64] = L"Cascadia Mono";
    wchar_t iconFamily[64] = L"Segoe Fluent Icons";
    bool Init(IDWriteFactory3* f, const Typography* copyFrom = nullptr);  // copyFrom: reuse resolved families/metrics
    void Release();
};

struct TableLayout {
    std::vector<IDWriteTextLayout*> cells;
    std::vector<float> colW, rowH;
    float width = 0, height = 0;
    ~TableLayout() { for (auto* c : cells) if (c) c->Release(); }
};

struct BlockLayout {
    IDWriteTextLayout* text = nullptr;  // BK_TEXT / BK_CODE / image alt text
    TableLayout* table = nullptr;       // BK_TABLE
    float height = 0;                   // full block height incl. decorations (DIP)
    float width = 0;                    // width the block was laid out for
    float natural = 0;                  // code / table / image: natural content width (DIP) for breakout
    bool colored = false;               // drawing effects applied
    std::vector<D2D1_RECT_F> codeBg;    // inline-code backgrounds (layout-relative), lazily computed
    bool codeBgValid = false;
    ~BlockLayout() { if (text) text->Release(); delete table; }
};

float BlockHeightEstimate(const Doc& d, const Block& b, float width, bool* exact);
BlockLayout* LayoutBlock(const Doc& d, const Typography& t, uint32_t index, float width);
int ImageSize(Doc& d, uint32_t imageIndex, int* w, int* h);  // reads PNG/JPEG/GIF/BMP header; thread-safe
float ImageDisplayHeight(const Image& im, float width);
float HeadingRuleExtra(int heading);  // padding + 1 px rule under h1/h2
