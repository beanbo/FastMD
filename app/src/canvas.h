// Drawing surface. The only back-end is the CPU one (DirectWrite → GDI DIB, no D2D/D3D): on the measured machine it
// is both the fastest first frame (no GPU driver load, ~200 ms saved) and the fastest scrolling path (≈2 ms/frame).
#pragma once
#include "doc.h"

struct Canvas {
    virtual ~Canvas() {}
    virtual void Begin() = 0;
    virtual void End() = 0;
    virtual void Clear(uint8_t pal) = 0;
    virtual void FillRect(float l, float t, float r, float b, uint8_t pal) = 0;
    virtual void FillRoundRect(float l, float t, float r, float b, float rad, uint8_t pal) = 0;
    virtual void StrokeRoundRect(float l, float t, float r, float b, float rad, float w, uint8_t pal) = 0;
    virtual void FillCircle(float cx, float cy, float rad, uint8_t pal) = 0;
    virtual void StrokeCircle(float cx, float cy, float rad, float w, uint8_t pal) = 0;
    virtual void Line(float x0, float y0, float x1, float y1, float w, uint8_t pal) = 0;  // round caps, AA
    virtual void Text(IDWriteTextLayout* layout, float x, float y, uint8_t defPal) = 0;
    virtual void DrawImage(::Image& im, float l, float t, float r, float b) = 0;
    virtual void PushClip(float l, float t, float r, float b) = 0;
    virtual void PopClip() = 0;
    // object for IDWriteTextLayout::SetDrawingEffect: a palette entry (P_DEFAULT = the block's own colour) and a
    // baseline shift of +1 for <sup> or -1 for <sub>
    virtual IUnknown* Effect(uint8_t pal, int shift = 0) = 0;
    virtual void SetScale(float pixelsPerDip) = 0;
    virtual void Resize(int w, int h) = 0;
    virtual HDC DC() = 0;
    // Scrolling without moving pixels: the buffer is taller than the window, so the window's rows inside it simply
    // shift. The caller then redraws the strip that came into view. false = too far, redraw the whole frame.
    virtual bool ScrollViewport(int dyPx) = 0;
    virtual int ViewportTop() const = 0;  // first buffer row of the window (source row of the blit)
    float scale = 1.f;  // pixels per DIP (DPI / 96 × zoom)
};

Canvas* CreateGdiCanvas(IDWriteFactory3* f, int w, int h, float pixelsPerDip);
