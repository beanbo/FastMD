// DirectWrite → GDI canvas: no Direct2D, no Direct3D, no GPU driver. Glyphs are rasterised by DirectWrite into a
// 32-bpp DIB (IDWriteBitmapRenderTarget; colour emoji = COLR layers from IDWriteFactory4::TranslateColorGlyphRun,
// because IDWriteBitmapRenderTarget3::DrawGlyphRunWithColorSupport is not implemented on Win11 26200); shapes are
// drawn by a tiny signed-distance-field rasteriser (anti-aliased rounded rects, circles, lines).
#include "canvas.h"
#include <cmath>

namespace {
struct ColorEffect final : IUnknown {  // drawing effect = palette index
    uint8_t pal = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown)) { *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

inline uint32_t Blend(uint32_t dst, uint32_t src, float a) {
    if (a >= 0.999f) return src;
    int ia = (int)(a * 256.f + 0.5f);
    uint32_t rb = ((((src & 0xff00ff) * ia) + ((dst & 0xff00ff) * (256 - ia))) >> 8) & 0xff00ff;
    uint32_t g = ((((src & 0x00ff00) * ia) + ((dst & 0x00ff00) * (256 - ia))) >> 8) & 0x00ff00;
    return rb | g;
}

struct GdiCanvas final : Canvas, IDWriteTextRenderer {
    IDWriteFactory3* f;
    IDWriteGdiInterop* interop = nullptr;
    IDWriteBitmapRenderTarget* brt = nullptr;
    IDWriteBitmapRenderTarget3* brt3 = nullptr;
    IDWriteFactory4* f4 = nullptr;
    IDWriteFontFace* lastFace = nullptr;
    bool lastFaceColor = false;
    IDWriteRenderingParams* params = nullptr;
    HDC dc = nullptr;
    uint32_t* bits = nullptr;
    int W = 0, H = 0;              // the window's client size in pixels
    static const int kSlack = 512;  // spare buffer rows above and below it, so scrolling never copies the frame
    int bufH = 0, origin = 0;       // buffer height; buffer row shown as client y = 0
    long stride = 0;  // in pixels, negative for bottom-up
    uint32_t* row0 = nullptr;
    ColorEffect fx[P_COUNT];
    struct Clip { int l, t, r, b; float lDip, tDip, rDip, bDip; };
    std::vector<Clip> clips;
    std::vector<uint32_t> saved;  // pixels around a glyph run that crosses the clip rect
    std::vector<int> xmap;        // DrawImage: source column of each visible column
    uint8_t curDefault = P_TEXT;

    GdiCanvas(IDWriteFactory3* fac, int w, int h, float d) : f(fac) {
        scale = d;
        for (int i = 0; i < P_COUNT; i++) fx[i].pal = (uint8_t)i;
        f->GetGdiInterop(&interop);
        f->QueryInterface(__uuidof(IDWriteFactory4), (void**)&f4);
        f->CreateRenderingParams(&params);
        Resize(w, h);
    }
    ~GdiCanvas() override {
        SafeRelease(brt3);
        SafeRelease(f4);
        SafeRelease(brt);
        SafeRelease(params);
        SafeRelease(interop);
    }
    void Resize(int w, int h) override {
        W = std::max(1, w);
        H = std::max(1, h);
        bufH = H + kSlack;
        origin = (bufH - H) / 2;
        if (brt) brt->Resize(W, bufH);
        else {
            interop->CreateBitmapRenderTarget(nullptr, W, bufH, &brt);
            brt->QueryInterface(__uuidof(IDWriteBitmapRenderTarget3), (void**)&brt3);  // not on Win11 26200
        }
        brt->SetPixelsPerDip(scale);
        dc = brt->GetMemoryDC();
        DIBSECTION ds{};
        GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof(ds), &ds);
        bits = (uint32_t*)ds.dsBm.bmBits;
        long pitch = ds.dsBm.bmWidthBytes / 4;
        // orientation: probe with GDI instead of trusting biHeight (the bitmap render target's DIB is top-down
        // although GetObject reports a positive height)
        uint32_t save = bits[0];
        bits[0] = 0;
        SetPixelV(dc, 0, 0, RGB(1, 2, 3));
        GdiFlush();
        bool topDown = (bits[0] & 0xffffff) == 0x010203;
        bits[0] = save;
        if (!topDown) { row0 = bits + (size_t)(bufH - 1) * pitch; stride = -pitch; }
        else { row0 = bits; stride = pitch; }
    }
    inline uint32_t* Row(int y) { return row0 + (ptrdiff_t)(y + origin) * stride; }
    int ViewportTop() const override { return origin; }
    bool ScrollViewport(int dy) override {
        if (dy == 0 || std::abs(dy) >= H || H >= bufH) return false;
        int next = origin + dy;
        if (next < 0 || next + H > bufH) {  // out of spare rows: move the window's rows back to the middle, once
            int mid = (bufH - H) / 2;
            MoveRows(origin, mid, H);
            origin = mid;
            next = origin + dy;
            if (next < 0 || next + H > bufH) return false;
        }
        origin = next;
        return true;
    }
    void MoveRows(int from, int to, int count) {  // whole rows inside the buffer; the ranges may overlap
        if (from == to) return;
        int step = to < from ? 1 : -1;
        int first = to < from ? 0 : count - 1;
        for (int k = 0, y = first; k < count; k++, y += step)
            memcpy(row0 + (ptrdiff_t)(to + y) * stride, row0 + (ptrdiff_t)(from + y) * stride, (size_t)W * 4);
    }
    float S() const { return scale; }
    void ClipPx(int& x0, int& y0, int& x1, int& y1) {
        x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, W); y1 = std::min(y1, H);
        if (!clips.empty()) {
            const Clip& c = clips.back();
            x0 = std::max(x0, c.l); y0 = std::max(y0, c.t); x1 = std::min(x1, c.r); y1 = std::min(y1, c.b);
        }
    }

    // --------------------------------------------------------------------------------------- canvas
    void Begin() override { GdiFlush(); }
    void End() override { GdiFlush(); }
    HDC DC() override { return dc; }
    void Clear(uint8_t pal) override {
        uint32_t c = g_pal[pal];
        // bounds in locals: a pixel store may alias the int members, so with W / H in the loop the compiler re-read W
        // after every pixel and wrote one pixel per iteration; now it emits rep stosd (≈0.4 ms less per full-screen
        // frame at 3440×1440; hand-written SSE2 stores, streaming or not, were slower than rep stosd)
        const int w = W, h = H;
        for (int y = 0; y < h; y++) { uint32_t* p = Row(y); for (int x = 0; x < w; x++) p[x] = c; }
    }
    void FillRect(float l, float t, float r, float b, uint8_t pal) override {
        float s = S();
        // snap edges to pixels (all callers pass pixel-aligned rects at integer DPI scales)
        int x0 = (int)std::lround(l * s), y0 = (int)std::lround(t * s), x1 = (int)std::lround(r * s), y1 = (int)std::lround(b * s);
        if (x1 == x0 && r > l) x1 = x0 + 1;
        if (y1 == y0 && b > t) y1 = y0 + 1;
        ClipPx(x0, y0, x1, y1);
        uint32_t c = g_pal[pal];
        for (int y = y0; y < y1; y++) { uint32_t* p = Row(y); for (int x = x0; x < x1; x++) p[x] = c; }
    }
    // SDF shape rasteriser. kind: 0 = rounded rect fill, 1 = rounded rect stroke, 2 = segment (capsule)
    template <class SDF> void Shape(float bl, float bt, float br_, float bb, uint32_t c, SDF sdf) {
        int x0 = (int)std::floor(bl) - 1, y0 = (int)std::floor(bt) - 1, x1 = (int)std::ceil(br_) + 1, y1 = (int)std::ceil(bb) + 1;
        ClipPx(x0, y0, x1, y1);
        for (int y = y0; y < y1; y++) {
            uint32_t* p = Row(y);
            float py = y + 0.5f;
            for (int x = x0; x < x1; x++) {
                float a = sdf(x + 0.5f, py);
                if (a <= 0.f) continue;
                p[x] = Blend(p[x], c, a > 1.f ? 1.f : a);
            }
        }
    }
    static float RRDist(float px, float py, float cx, float cy, float hx, float hy, float r) {
        float qx = std::fabs(px - cx) - (hx - r), qy = std::fabs(py - cy) - (hy - r);
        float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
        return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
    }
    void RoundRectPx(float l, float t, float r, float b, float rad, uint32_t c, float strokeW) {
        float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f, hx = (r - l) * 0.5f, hy = (b - t) * 0.5f;
        rad = std::min(rad, std::min(hx, hy));
        if (strokeW <= 0) {
            // interior rows without corners: solid spans (fast path for big code-block backgrounds)
            int iy0 = (int)std::ceil(t + rad), iy1 = (int)std::floor(b - rad);
            int ix0 = (int)std::ceil(l), ix1 = (int)std::floor(r);
            int cx0 = ix0, cy0 = iy0, cx1 = ix1, cy1 = iy1;
            ClipPx(cx0, cy0, cx1, cy1);
            for (int y = cy0; y < cy1; y++) { uint32_t* p = Row(y); for (int x = cx0; x < cx1; x++) p[x] = c; }
            auto sdf = [&](float px, float py) { return 0.5f - RRDist(px, py, cx, cy, hx, hy, rad); };
            Shape(l, t, r, (float)iy0, c, sdf);          // top band incl. corners
            Shape(l, (float)iy1, r, b, c, sdf);          // bottom band
            if (ix0 > l) Shape(l, (float)iy0, (float)ix0, (float)iy1, c, sdf);  // fractional side edges
            if (ix1 < r) Shape((float)ix1, (float)iy0, r, (float)iy1, c, sdf);
            return;
        }
        Shape(l, t, r, b, c, [&](float px, float py) {
            float d = RRDist(px, py, cx, cy, hx, hy, rad);
            float outer = std::clamp(0.5f - d, 0.f, 1.f), inner = std::clamp(0.5f - (d + strokeW), 0.f, 1.f);
            return outer - inner;
        });
    }
    void FillRoundRect(float l, float t, float r, float b, float rad, uint8_t pal) override {
        float s = S();
        RoundRectPx(l * s, t * s, r * s, b * s, rad * s, g_pal[pal], 0);
    }
    void StrokeRoundRect(float l, float t, float r, float b, float rad, float w, uint8_t pal) override {
        float s = S();
        RoundRectPx(l * s, t * s, r * s, b * s, rad * s, g_pal[pal], w * s);
    }
    void FillCircle(float cx, float cy, float rad, uint8_t pal) override {
        float s = S();
        cx *= s; cy *= s; rad *= s;
        Shape(cx - rad, cy - rad, cx + rad, cy + rad, g_pal[pal], [&](float px, float py) {
            return 0.5f - (std::hypot(px - cx, py - cy) - rad);
        });
    }
    void StrokeCircle(float cx, float cy, float rad, float w, uint8_t pal) override {
        float s = S();
        cx *= s; cy *= s; rad *= s; w *= s;
        Shape(cx - rad, cy - rad, cx + rad, cy + rad, g_pal[pal], [&](float px, float py) {
            float d = std::hypot(px - cx, py - cy) - rad;
            return std::clamp(0.5f - d, 0.f, 1.f) - std::clamp(0.5f - (d + w), 0.f, 1.f);
        });
    }
    void Line(float x0, float y0, float x1, float y1, float w, uint8_t pal) override {
        float s = S();
        x0 *= s; y0 *= s; x1 *= s; y1 *= s;
        float hw = w * s * 0.5f;
        float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
        Shape(std::min(x0, x1) - hw, std::min(y0, y1) - hw, std::max(x0, x1) + hw, std::max(y0, y1) + hw, g_pal[pal],
              [&](float px, float py) {
                  float tt = len2 > 0 ? std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.f, 1.f) : 0.f;
                  float ex = px - (x0 + tt * dx), ey = py - (y0 + tt * dy);
                  return 0.5f - (std::sqrt(ex * ex + ey * ey) - hw);
              });
    }
    void Text(IDWriteTextLayout* layout, float x, float y, uint8_t defPal) override {
        curDefault = defPal;
        layout->Draw(nullptr, this, x, y);
    }
    void DrawImage(::Image& im, float l, float t, float r, float b) override {
        if (im.state.load() != 2 || im.pxW <= 0 || im.pxH <= 0 || im.px.size() < (size_t)im.pxW * im.pxH) return;
        float s = S();
        int x0 = (int)std::lround(l * s), y0 = (int)std::lround(t * s), x1 = (int)std::lround(r * s), y1 = (int)std::lround(b * s);
        int dw = x1 - x0, dh = y1 - y0;
        if (dw <= 0 || dh <= 0) return;
        int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
        ClipPx(cx0, cy0, cx1, cy1);
        if (cx0 >= cx1) return;
        const int n = cx1 - cx0;
        auto put = [](uint32_t* p, uint32_t sp) {
            uint32_t a = sp >> 24;
            if (a == 255) *p = sp & 0xffffff;
            else if (a) {  // premultiplied over opaque
                uint32_t d = *p, ia = 255 - a;
                uint32_t rb = (sp & 0xff00ff) + ((((d & 0xff00ff) * ia) >> 8) & 0xff00ff);
                uint32_t g = (sp & 0x00ff00) + ((((d & 0x00ff00) * ia) >> 8) & 0x00ff00);
                *p = (rb & 0xff00ff) | (g & 0x00ff00);
            }
        };
        // a copy at exactly this size (loader.cpp makes it in the background): one row at a time, no scaling here
        if (im.scW.load(std::memory_order_acquire) == dw && im.scH.load() == dh && im.sc.size() >= (size_t)dw * dh) {
            const uint32_t* src = im.sc.data() + (size_t)(cy0 - y0) * dw + (cx0 - x0);
            for (int y = cy0; y < cy1; y++, src += dw) {
                uint32_t* p = Row(y) + cx0;
                for (int i = 0; i < n; i++) put(p + i, src[i]);
            }
            return;
        }
        if (dw != im.pxW || dh != im.pxH) {  // ask for that copy; until it is ready, scale by the nearest neighbour
            im.wantW.store(dw, std::memory_order_relaxed);
            im.wantH.store(dh, std::memory_order_relaxed);
        }
        // the nearest source column of every visible column, once per call: a 64-bit division per pixel cost ≈0.5 ms
        // per frame with large images in a full-screen window
        const int sw = im.pxW, sh = im.pxH;
        xmap.resize(n);
        for (int i = 0; i < n; i++) xmap[i] = (int)((int64_t)(cx0 + i - x0) * sw / dw);
        const int* xm = xmap.data();
        const uint32_t* src = im.px.data();
        for (int y = cy0; y < cy1; y++) {
            uint32_t* p = Row(y) + cx0;
            const uint32_t* srow = src + (size_t)((int64_t)(y - y0) * sh / dh) * sw;
            for (int i = 0; i < n; i++) put(p + i, srow[xm[i]]);  // nearest (1:1 at 100 %)
        }
    }
    void PushClip(float l, float t, float r, float b) override {
        float s = S();
        Clip c{(int)std::lround(l * s), (int)std::lround(t * s), (int)std::lround(r * s), (int)std::lround(b * s), l, t, r, b};
        if (!clips.empty()) {
            const Clip& p = clips.back();
            c.l = std::max(c.l, p.l); c.t = std::max(c.t, p.t); c.r = std::min(c.r, p.r); c.b = std::min(c.b, p.b);
            c.lDip = std::max(c.lDip, p.lDip); c.tDip = std::max(c.tDip, p.tDip);
            c.rDip = std::min(c.rDip, p.rDip); c.bDip = std::min(c.bDip, p.bDip);
        }
        clips.push_back(c);
    }
    void PopClip() override { if (!clips.empty()) clips.pop_back(); }
    IUnknown* Effect(uint8_t pal) override { return &fx[pal]; }
    void SetScale(float d) override { scale = d; if (brt) brt->SetPixelsPerDip(d); }

    // --------------------------------------------------------------------------------------- IDWriteTextRenderer
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
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* v) override { *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override { *p = scale; return S_OK; }
    uint8_t PalOf(IUnknown* e) const {
        if (e >= (const IUnknown*)&fx[0] && e < (const IUnknown*)&fx[P_COUNT]) return static_cast<ColorEffect*>(e)->pal;
        return curDefault;
    }
    static COLORREF Ref(uint32_t rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
    // Text is rasterised by DirectWrite straight into the DIB, which knows nothing about our clip rects. Inside a clip:
    // glyphs far outside are dropped (long code lines cost nothing), and a run that crosses the clip edge is drawn
    // with the pixels around it saved and restored outside the clip — a pixel-exact clip for scrolled code / tables.
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION* desc, IUnknown* effect) override {
        if (clips.empty()) return DrawRun(x, y, mode, *run, desc, effect);
        const Clip& c = clips.back();
        DWRITE_GLYPH_RUN gr = *run;
        float em = gr.fontEmSize, top = y - em * 1.3f, bottom = y + em * 0.6f;
        if (top > c.bDip || bottom < c.tDip) return S_OK;
        bool rtl = (gr.bidiLevel & 1) != 0;
        if (!gr.isSideways && !rtl && gr.glyphAdvances) {
            UINT32 k = 0;
            while (k < gr.glyphCount && x + gr.glyphAdvances[k] < c.lDip - em) x += gr.glyphAdvances[k++];
            if (k) {
                gr.glyphIndices += k;
                gr.glyphAdvances += k;
                if (gr.glyphOffsets) gr.glyphOffsets += k;
                gr.glyphCount -= k;
                desc = nullptr;  // cluster map no longer matches
            }
            UINT32 n = 0;
            float pen = x;
            while (n < gr.glyphCount && pen < c.rDip + em) pen += gr.glyphAdvances[n++];
            if (n < gr.glyphCount) { gr.glyphCount = n; desc = nullptr; }
            if (!gr.glyphCount) return S_OK;
        }
        float runW = 0;
        for (UINT32 k = 0; k < gr.glyphCount && gr.glyphAdvances; k++) runW += gr.glyphAdvances[k];
        float s = S(), l = rtl ? x - runW - em : x - em, r = rtl ? x + em : x + runW + em;
        if (gr.isSideways) { l = x - em * 2; r = x + runW + em * 2; }
        int L = std::max(0, (int)std::floor(l * s)), T = std::max(0, (int)std::floor(top * s));
        int R = std::min(W, (int)std::ceil(r * s)), B = std::min(H, (int)std::ceil(bottom * s));
        if (L >= R || T >= B) return S_OK;
        if (L >= c.l && R <= c.r && T >= c.t && B <= c.b) return DrawRun(x, y, mode, gr, desc, effect);
        int w = R - L;
        saved.resize((size_t)w * (B - T));
        for (int yy = T; yy < B; yy++) memcpy(&saved[(size_t)(yy - T) * w], Row(yy) + L, w * 4);
        HRESULT hr = DrawRun(x, y, mode, gr, desc, effect);
        GdiFlush();
        for (int yy = T; yy < B; yy++) {
            uint32_t* p = Row(yy);
            const uint32_t* sp = &saved[(size_t)(yy - T) * w];
            if (yy < c.t || yy >= c.b) { memcpy(p + L, sp, w * 4); continue; }
            for (int xx = L; xx < std::min(R, c.l); xx++) p[xx] = sp[xx - L];
            for (int xx = std::max(L, c.r); xx < R; xx++) p[xx] = sp[xx - L];
        }
        return hr;
    }
    // DirectWrite rasterises into the buffer's own coordinates and knows nothing about the scrolled viewport, so the
    // baseline moves down by the viewport's offset (a whole number of pixels, so pixel snapping is unaffected).
    HRESULT DrawRun(FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN& gr,
                    const DWRITE_GLYPH_RUN_DESCRIPTION* desc, IUnknown* effect) {
        y += origin / scale;
        COLORREF c = Ref(g_pal[PalOf(effect)]);
        if (brt3) return brt3->DrawGlyphRunWithColorSupport(x, y, mode, &gr, params, c, 0, nullptr);
        // colour fonts (Segoe UI Emoji): draw the COLR v0 layers one by one
        if (gr.fontFace != lastFace) {
            lastFace = gr.fontFace;
            IDWriteFontFace2* f2 = nullptr;
            lastFaceColor = SUCCEEDED(gr.fontFace->QueryInterface(__uuidof(IDWriteFontFace2), (void**)&f2)) && f2->IsColorFont();
            SafeRelease(f2);
        }
        if (lastFaceColor && f4) {
            IDWriteColorGlyphRunEnumerator1* en = nullptr;
            if (SUCCEEDED(f4->TranslateColorGlyphRun(D2D1_POINT_2F{x, y}, &gr, desc,
                                                     DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE | DWRITE_GLYPH_IMAGE_FORMATS_CFF |
                                                         DWRITE_GLYPH_IMAGE_FORMATS_COLR,
                                                     mode, nullptr, 0, &en))) {
                BOOL more = FALSE;
                while (SUCCEEDED(en->MoveNext(&more)) && more) {
                    const DWRITE_COLOR_GLYPH_RUN1* cr = nullptr;
                    if (FAILED(en->GetCurrentRun(&cr))) break;
                    COLORREF lc = cr->paletteIndex == 0xFFFF ? c
                                  : RGB((int)(cr->runColor.r * 255.f + .5f), (int)(cr->runColor.g * 255.f + .5f), (int)(cr->runColor.b * 255.f + .5f));
                    brt->DrawGlyphRun(cr->baselineOriginX, cr->baselineOriginY, mode, &cr->glyphRun, params, lc, nullptr);
                }
                en->Release();
                return S_OK;
            }
        }
        return brt->DrawGlyphRun(x, y, mode, &gr, params, c, nullptr);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* u, IUnknown* effect) override {
        FillRect(x, y + u->offset, x + u->width, y + u->offset + std::max(u->thickness, 1.f / scale), PalOf(effect));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* st, IUnknown* effect) override {
        FillRect(x, y + st->offset, x + st->width, y + st->offset + std::max(st->thickness, 1.f / scale), PalOf(effect));
        return S_OK;
    }
    // inline objects: the ellipsis trimming sign (outline items, recent documents) draws itself through this renderer
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* ctx, FLOAT x, FLOAT y, IDWriteInlineObject* obj, BOOL sideways, BOOL rtl,
                                               IUnknown* effect) override {
        return obj ? obj->Draw(ctx, this, x, y, sideways, rtl, effect) : S_OK;
    }
};
}  // namespace

Canvas* CreateGdiCanvas(IDWriteFactory3* f, int w, int h, float pixelsPerDip) { return new GdiCanvas(f, w, h, pixelsPerDip); }
