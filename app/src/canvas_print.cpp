// Printer canvas: the page is drawn straight onto the printer's device context, and text goes out as text.
//
// DirectWrite lays the page out exactly as on screen, but its glyph runs are handed to GDI as glyph indices
// (ETO_GLYPH_INDEX) with a GDI font made from the same font face. The printer driver then keeps the glyphs — the job
// is vector, sharp at 600 dpi, and "Microsoft Print to PDF" produces a PDF whose text can be searched and copied,
// instead of a picture of a page. Colour emoji keep their colours: COLR layers are outline glyphs too, so each layer
// is one more ExtTextOut.
#include "canvas.h"

#include <cmath>
#include <vector>

namespace {
struct PrintCanvas final : RendererCanvas {
    IDWriteFactory3* f = nullptr;
    IDWriteFactory4* f4 = nullptr;
    IDWriteGdiInterop* interop = nullptr;
    HDC dc = nullptr;
    int W = 0, H = 0;  // page in device pixels
    uint8_t curDefault = P_TEXT;
    ColorEffect fx[P_COUNT][3];
    struct Font { LOGFONTW lf; HFONT h; };
    std::vector<Font> fonts;
    std::vector<uint32_t> tmp;  // one picture composed over white

    PrintCanvas(IDWriteFactory3* fac, HDC hdc, int w, int h, float d) : f(fac), dc(hdc), W(w), H(h) {
        scale = d;
        for (int i = 0; i < P_COUNT; i++)
            for (int s = 0; s < 3; s++) { fx[i][s].pal = (uint8_t)i; fx[i][s].shift = (int8_t)(s - 1); }
        f->GetGdiInterop(&interop);
        f->QueryInterface(__uuidof(IDWriteFactory4), (void**)&f4);
        SetGraphicsMode(dc, GM_COMPATIBLE);
        SetBkMode(dc, TRANSPARENT);
        SetStretchBltMode(dc, HALFTONE);
    }
    ~PrintCanvas() override {
        for (Font& fo : fonts) DeleteObject(fo.h);
        SafeRelease(f4);
        SafeRelease(interop);
    }

    // ------------------------------------------------------------------------------------------- helpers
    int D(float v) const { return (int)std::lround(v * scale); }
    static COLORREF Ref(uint32_t rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
    const ColorEffect* EffectOf(IUnknown* e) const {
        const ColorEffect* c = static_cast<const ColorEffect*>(e);
        return (c && c->pal < P_COUNT) ? c : nullptr;
    }
    uint8_t PalOf(IUnknown* e) const {
        const ColorEffect* c = EffectOf(e);
        return (c && c->pal != P_DEFAULT) ? c->pal : curDefault;
    }
    float ShiftOf(IUnknown* e) const {
        const ColorEffect* c = EffectOf(e);
        return c ? (c->shift > 0 ? -0.34f : c->shift < 0 ? 0.16f : 0.f) : 0.f;
    }
    void Solid(int l, int t, int r, int b, uint8_t pal) {
        if (r <= l || b <= t) return;
        RECT rc{l, t, r, b};
        HBRUSH br = CreateSolidBrush(Ref(g_pal[pal]));
        ::FillRect(dc, &rc, br);
        DeleteObject(br);
    }
    // GDI shapes need a pen and a brush; a fill uses a pen of its own colour so the outline does not show
    void Shape(int l, int t, int r, int b, float rad, float stroke, uint8_t pal, bool circle) {
        COLORREF c = Ref(g_pal[pal]);
        int w = stroke > 0 ? std::max(1, D(stroke)) : 1;
        HPEN pen = CreatePen(PS_SOLID, w, c);
        HBRUSH br = stroke > 0 ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(c);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
        if (circle) Ellipse(dc, l, t, r, b);
        else if (rad > 0) RoundRect(dc, l, t, r, b, D(rad) * 2, D(rad) * 2);
        else Rectangle(dc, l, t, r, b);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
        if (stroke <= 0) DeleteObject(br);
    }
    HFONT GetFont(const LOGFONTW& lf) {
        for (Font& fo : fonts)
            if (fo.lf.lfHeight == lf.lfHeight && fo.lf.lfWeight == lf.lfWeight && fo.lf.lfItalic == lf.lfItalic &&
                wcscmp(fo.lf.lfFaceName, lf.lfFaceName) == 0)
                return fo.h;
        HFONT h = CreateFontIndirectW(&lf);
        if (h) fonts.push_back(Font{lf, h});
        return h;
    }

    // ------------------------------------------------------------------------------------------- canvas
    void Begin() override {}
    void End() override {}
    HDC DC() override { return dc; }
    void Clear(uint8_t pal) override { Solid(0, 0, W, H, pal); }
    void FillRect(float l, float t, float r, float b, uint8_t pal) override { Solid(D(l), D(t), D(r), D(b), pal); }
    void FillRoundRect(float l, float t, float r, float b, float rad, uint8_t pal) override {
        Shape(D(l), D(t), D(r), D(b), rad, 0, pal, false);
    }
    void StrokeRoundRect(float l, float t, float r, float b, float rad, float w, uint8_t pal) override {
        Shape(D(l), D(t), D(r), D(b), rad, w, pal, false);
    }
    void FillCircle(float cx, float cy, float rad, uint8_t pal) override {
        Shape(D(cx - rad), D(cy - rad), D(cx + rad), D(cy + rad), 0, 0, pal, true);
    }
    void StrokeCircle(float cx, float cy, float rad, float w, uint8_t pal) override {
        Shape(D(cx - rad), D(cy - rad), D(cx + rad), D(cy + rad), 0, w, pal, true);
    }
    void Line(float x0, float y0, float x1, float y1, float w, uint8_t pal) override {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, D(w)), Ref(g_pal[pal]));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, D(x0), D(y0), nullptr);
        LineTo(dc, D(x1), D(y1));
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    void Text(IDWriteTextLayout* layout, float x, float y, uint8_t defPal) override {
        curDefault = defPal;
        layout->Draw(nullptr, this, x, y);
    }
    // The page has no pixels to read back, so a picture with transparency is composed over the page colour first, at
    // its own size, and the printer driver scales it to the box.
    void DrawImage(::Image& im, float l, float t, float r, float b) override {
        if (im.state.load() != 2 || im.pxW <= 0 || im.pxH <= 0 || im.px.size() < (size_t)im.pxW * im.pxH) return;
        int x0 = D(l), y0 = D(t), x1 = D(r), y1 = D(b);
        if (x1 <= x0 || y1 <= y0) return;
        uint32_t bg = g_pal[P_BG];
        tmp.resize((size_t)im.pxW * im.pxH);
        for (size_t i = 0; i < tmp.size(); i++) {  // premultiplied BGRA over the page colour
            uint32_t sp = im.px[i], a = sp >> 24;
            if (a == 255) { tmp[i] = sp & 0xffffff; continue; }
            uint32_t ia = 255 - a;
            uint32_t rb = (sp & 0xff00ff) + ((((bg & 0xff00ff) * ia) >> 8) & 0xff00ff);
            uint32_t gg = (sp & 0x00ff00) + ((((bg & 0x00ff00) * ia) >> 8) & 0x00ff00);
            tmp[i] = (rb & 0xff00ff) | (gg & 0x00ff00);
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = im.pxW;
        bi.bmiHeader.biHeight = -im.pxH;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, x0, y0, x1 - x0, y1 - y0, 0, 0, im.pxW, im.pxH, tmp.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    void PushClip(float l, float t, float r, float b) override {
        SaveDC(dc);
        IntersectClipRect(dc, D(l), D(t), D(r), D(b));
    }
    void PopClip() override { RestoreDC(dc, -1); }
    IUnknown* Effect(uint8_t pal, int shift) override { return &fx[pal][std::clamp(shift, -1, 1) + 1]; }
    void SetScale(float pixelsPerDip) override { scale = pixelsPerDip; }
    void Resize(int w, int h) override { W = w; H = h; }
    bool ScrollViewport(int) override { return false; }
    int ViewportTop() const override { return 0; }

    // ------------------------------------------------------------------------------------------- text renderer
    void PlainRun(float bx, float by, const DWRITE_GLYPH_RUN& run, COLORREF c) {
        if (!run.glyphCount || !run.fontFace) return;
        LOGFONTW lf{};
        if (FAILED(interop->ConvertFontFaceToLOGFONT(run.fontFace, &lf))) return;
        lf.lfHeight = -(LONG)std::lround(run.fontEmSize * scale);
        lf.lfWidth = 0;
        lf.lfEscapement = lf.lfOrientation = 0;
        lf.lfQuality = ANTIALIASED_QUALITY;
        HFONT font = GetFont(lf);
        if (!font) return;
        float total = 0;
        bool offsets = false;
        for (UINT32 i = 0; i < run.glyphCount; i++) {
            total += run.glyphAdvances ? run.glyphAdvances[i] : 0;
            if (run.glyphOffsets && (run.glyphOffsets[i].advanceOffset != 0 || run.glyphOffsets[i].ascenderOffset != 0))
                offsets = true;
        }
        if (run.bidiLevel & 1) bx -= total;  // a right-to-left run is placed from its right edge
        HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, c);
        UINT align = SetTextAlign(dc, TA_LEFT | TA_BASELINE | TA_NOUPDATECP);
        if (!offsets) {
            // one pen position per glyph, rounded once: the run keeps its exact width instead of drifting
            std::vector<INT> dx(run.glyphCount);
            float pen = 0;
            int prev = 0;
            for (UINT32 i = 0; i < run.glyphCount; i++) {
                pen += run.glyphAdvances ? run.glyphAdvances[i] : 0;
                int at = (int)std::lround(pen * scale);
                dx[i] = at - prev;
                prev = at;
            }
            ExtTextOutW(dc, D(bx), D(by), ETO_GLYPH_INDEX, nullptr, (LPCWSTR)run.glyphIndices, run.glyphCount, dx.data());
        } else {
            float pen = 0;
            for (UINT32 i = 0; i < run.glyphCount; i++) {
                const DWRITE_GLYPH_OFFSET& o = run.glyphOffsets[i];
                ExtTextOutW(dc, D(bx + pen + o.advanceOffset), D(by - o.ascenderOffset), ETO_GLYPH_INDEX, nullptr,
                            (LPCWSTR)&run.glyphIndices[i], 1, nullptr);
                pen += run.glyphAdvances ? run.glyphAdvances[i] : 0;
            }
        }
        SetTextAlign(dc, align);
        SelectObject(dc, old);
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION* desc, IUnknown* effect) override {
        if (!run) return S_OK;
        y += ShiftOf(effect) * run->fontEmSize;
        COLORREF c = Ref(g_pal[PalOf(effect)]);
        IDWriteFontFace2* f2 = nullptr;
        bool color = run->fontFace &&
                     SUCCEEDED(run->fontFace->QueryInterface(__uuidof(IDWriteFontFace2), (void**)&f2)) && f2->IsColorFont();
        SafeRelease(f2);
        if (color && f4) {  // COLR layers are ordinary outline glyphs: one ExtTextOut per layer keeps emoji in colour
            IDWriteColorGlyphRunEnumerator1* en = nullptr;
            if (SUCCEEDED(f4->TranslateColorGlyphRun(D2D1_POINT_2F{x, y}, run, desc,
                                                     DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE | DWRITE_GLYPH_IMAGE_FORMATS_CFF |
                                                         DWRITE_GLYPH_IMAGE_FORMATS_COLR,
                                                     mode, nullptr, 0, &en))) {
                BOOL more = FALSE;
                while (SUCCEEDED(en->MoveNext(&more)) && more) {
                    const DWRITE_COLOR_GLYPH_RUN1* cr = nullptr;
                    if (FAILED(en->GetCurrentRun(&cr))) break;
                    COLORREF lc = cr->paletteIndex == 0xFFFF
                                      ? c
                                      : RGB((int)(cr->runColor.r * 255.f + .5f), (int)(cr->runColor.g * 255.f + .5f),
                                            (int)(cr->runColor.b * 255.f + .5f));
                    PlainRun(cr->baselineOriginX, cr->baselineOriginY, cr->glyphRun, lc);
                }
                en->Release();
                return S_OK;
            }
        }
        PlainRun(x, y, *run, c);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* u, IUnknown* effect) override {
        FillRect(x, y + u->offset, x + u->width, y + u->offset + std::max(u->thickness, 1.f / scale), PalOf(effect));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* st, IUnknown* effect) override {
        FillRect(x, y + st->offset, x + st->width, y + st->offset + std::max(st->thickness, 1.f / scale), PalOf(effect));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* ctx, FLOAT x, FLOAT y, IDWriteInlineObject* obj, BOOL sideways,
                                               BOOL rtl, IUnknown* effect) override {
        return obj ? obj->Draw(ctx, this, x, y, sideways, rtl, effect) : S_OK;
    }

    // IUnknown / IDWritePixelSnapping: the renderer is owned by the canvas, so reference counting is a formality
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *ppv = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* ppd) override { *ppd = scale; return S_OK; }
};
}  // namespace

Canvas* CreatePrintCanvas(IDWriteFactory3* f, HDC dc, int w, int h, float pixelsPerDip) {
    return new PrintCanvas(f, dc, w, h, pixelsPerDip);
}
