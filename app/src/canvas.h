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
    virtual IUnknown* Effect(uint8_t pal) = 0;  // object for IDWriteTextLayout::SetDrawingEffect
    virtual void SetScale(float pixelsPerDip) = 0;
    virtual void Resize(int w, int h) = 0;
    virtual HDC DC() = 0;
    float scale = 1.f;  // pixels per DIP (DPI / 96 × zoom)
};

Canvas* CreateGdiCanvas(IDWriteFactory3* f, int w, int h, float pixelsPerDip);
