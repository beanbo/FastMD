// DirectWrite typography + per-block layout.
#include "layout.h"
#include "canvas.h"  // pictures inside a line draw straight onto the canvas behind the text renderer

namespace {
struct RoleSpec { float size, lineH; DWRITE_FONT_WEIGHT weight; bool display; };
// Product type ramp at text size 16 (research/06 §8.2): body 16/26, h1 32/40, h2 24/32, h3 20/28, h4 17/24,
// h5 16/24, h6 14/20, code 14/21. h1–h3 use the Display optical size of Segoe UI Variable. The text-size setting
// scales the whole ramp (Typography::textScale).
const RoleSpec kRoles[R_COUNT] = {
    {16.f, 26.f, DWRITE_FONT_WEIGHT_NORMAL, false},
    {32.f, 40.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true},
    {24.f, 32.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true},
    {20.f, 28.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true},
    {17.f, 24.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
    {16.f, 24.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
    {14.f, 20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
    {14.f, 21.f, DWRITE_FONT_WEIGHT_NORMAL, false},
};

bool FontMetricsFor(IDWriteFontCollection* coll, const wchar_t* family, float* asc, float* desc) {
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (FAILED(coll->FindFamilyName(family, &idx, &exists)) || !exists) return false;
    IDWriteFontFamily* fam = nullptr;
    IDWriteFont* font = nullptr;
    bool ok = false;
    if (SUCCEEDED(coll->GetFontFamily(idx, &fam)) &&
        SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font))) {
        DWRITE_FONT_METRICS m{};
        font->GetMetrics(&m);
        if (asc) *asc = (float)m.ascent / m.designUnitsPerEm;
        if (desc) *desc = (float)m.descent / m.designUnitsPerEm;
        ok = true;
    }
    SafeRelease(font);
    SafeRelease(fam);
    return ok;
}

// Optical-size family for a text role. Segoe UI Variable: Display for h1–h3, Small below 12 px, Text otherwise.
// Sitka (the "book" option): Small ≤ 10 pt, Text ≤ 14 pt, Subheading ≤ 18 pt, Heading ≤ 23.5 pt, Display ≤ 27.5 pt,
// Banner above (1 DIP = 0.75 pt).
const wchar_t* FamilyFor(uint8_t set, int role, float px) {
    if (set == FONT_SITKA) {
        float pt = px * 0.75f;
        return pt <= 10.f ? L"Sitka Small" : pt <= 14.f ? L"Sitka Text" : pt <= 18.f ? L"Sitka Subheading"
             : pt <= 23.5f ? L"Sitka Heading" : pt <= 27.5f ? L"Sitka Display" : L"Sitka Banner";
    }
    return kRoles[role].display ? L"Segoe UI Variable Display" : px < 12.f ? L"Segoe UI Variable Small" : L"Segoe UI Variable Text";
}
}  // namespace

float HeadingRuleExtra(const Typography& t, int heading) {
    return (heading == 1 || heading == 2) ? std::round(t.size[heading] * 0.3f) + 1.f : 0.f;
}

bool Typography::Init(IDWriteFactory3* f, const Typography* from) {
    factory = f;
    if (from) {
        fontSet = from->fontSet;
        textScale = from->textScale;
        wrapCode = from->wrapCode;
        memcpy(family, from->family, sizeof(family));
        wcscpy_s(uiFamily, from->uiFamily);
        wcscpy_s(monoFamily, from->monoFamily);
        wcscpy_s(iconFamily, from->iconFamily);
        monoAscent = from->monoAscent;
        monoDescent = from->monoDescent;
        for (int r = 0; r < R_COUNT; r++) baseline[r] = from->baseline[r];
    }
    for (int r = 0; r < R_COUNT; r++) {
        size[r] = std::round(kRoles[r].size * textScale * 2.f) / 2.f;
        lineH[r] = std::round(kRoles[r].lineH * textScale);
    }
    if (!from) {
        float asc[R_COUNT], desc[R_COUNT];
        IDWriteFontCollection* coll = nullptr;
        if (SUCCEEDED(f->GetSystemFontCollection(&coll, FALSE))) {
            struct Known { const wchar_t* name; float a, d; bool ok; } known[8];
            int nk = 0;
            auto metrics = [&](const wchar_t* name, float* a, float* d) {  // one font lookup per distinct family
                for (int k = 0; k < nk; k++)
                    if (!wcscmp(known[k].name, name)) { *a = known[k].a; *d = known[k].d; return known[k].ok; }
                *a = 0.93f;
                *d = 0.23f;
                bool ok = FontMetricsFor(coll, name, a, d);
                if (nk < 8) known[nk++] = Known{name, *a, *d, ok};
                return ok;
            };
            for (int r = 0; r < R_CODE; r++) {
                const wchar_t* want = FamilyFor(fontSet, r, size[r]);
                if (!metrics(want, &asc[r], &desc[r])) {
                    want = FamilyFor(FONT_SEGOE, r, size[r]);
                    if (!metrics(want, &asc[r], &desc[r])) { want = L"Segoe UI"; metrics(want, &asc[r], &desc[r]); }
                }
                wcscpy_s(family[r], want);
            }
            if (!FontMetricsFor(coll, uiFamily, nullptr, nullptr)) wcscpy_s(uiFamily, L"Segoe UI");
            if (!FontMetricsFor(coll, monoFamily, &monoAscent, &monoDescent)) {
                wcscpy_s(monoFamily, L"Consolas");
                FontMetricsFor(coll, monoFamily, &monoAscent, &monoDescent);
            }
            if (!FontMetricsFor(coll, iconFamily, nullptr, nullptr)) wcscpy_s(iconFamily, L"Segoe MDL2 Assets");
            coll->Release();
        } else {
            for (int r = 0; r < R_CODE; r++) { wcscpy_s(family[r], L"Segoe UI"); asc[r] = 0.93f; desc[r] = 0.23f; }
        }
        wcscpy_s(family[R_CODE], monoFamily);
        asc[R_CODE] = monoAscent;
        desc[R_CODE] = monoDescent;
        // CSS half-leading: the line box is lineH tall, content area (ascent+descent) centred in it
        for (int r = 0; r < R_COUNT; r++)
            baseline[r] = std::round(((lineH[r] - (asc[r] + desc[r]) * size[r]) * 0.5f + asc[r] * size[r]) * 4.f) / 4.f;
    }
    for (int r = 0; r < R_COUNT; r++) {
        if (FAILED(f->CreateTextFormat(family[r], nullptr, kRoles[r].weight, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size[r], L"en-us", &fmt[r])))
            return false;
        fmt[r]->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH[r], baseline[r]);
        fmt[r]->SetWordWrapping(r == R_CODE ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
    }
    f->CreateTextFormat(uiFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, 13.f, L"en-us", &ui);
    if (ui) ui->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->CreateTextFormat(iconFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, 12.f, L"en-us", &uiIcon);
    return true;
}

void Typography::Release() {
    for (auto*& f : fmt) SafeRelease(f);
    SafeRelease(ui);
    SafeRelease(uiIcon);
}

// ------------------------------------------------------------------------------------------------ images
static SRWLOCK g_imgLock = SRWLOCK_INIT;

uint32_t NewPixelSerial() {
    static std::atomic<uint32_t> serial{0};
    return ++serial;
}

static bool ReadImageHeader(const wchar_t* path, int* w, int* h) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    static thread_local uint8_t b[64 * 1024];
    DWORD n = 0;
    ReadFile(f, b, sizeof(b), &n, nullptr);
    CloseHandle(f);
    auto be16 = [&](size_t i) { return (int)(b[i] << 8 | b[i + 1]); };
    if (n >= 24 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        *w = (int)((uint32_t)b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19]);
        *h = (int)((uint32_t)b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23]);
        return true;
    }
    if (n >= 10 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F') {
        *w = b[6] | b[7] << 8;
        *h = b[8] | b[9] << 8;
        return true;
    }
    if (n >= 26 && b[0] == 'B' && b[1] == 'M') {
        *w = (int)((uint32_t)b[18] | b[19] << 8 | b[20] << 16 | (uint32_t)b[21] << 24);
        *h = std::abs((int)((uint32_t)b[22] | b[23] << 8 | b[24] << 16 | (uint32_t)b[25] << 24));
        return true;
    }
    if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8) {  // JPEG: find SOFn
        size_t i = 2;
        while (i + 9 < n) {
            if (b[i] != 0xFF) { i++; continue; }
            uint8_t m = b[i + 1];
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                *h = be16(i + 5);
                *w = be16(i + 7);
                return true;
            }
            i += 2 + be16(i + 2);
        }
    }
    return false;
}

int ImageSize(Doc& d, uint32_t idx, int* w, int* h) {
    Image& im = d.images[idx];
    AcquireSRWLockShared(&g_imgLock);
    int cw = im.w, ch = im.h;
    ReleaseSRWLockShared(&g_imgLock);
    if (cw == -1) {
        cw = ch = 0;  // 0 = failed / remote
        if (!im.path.empty()) {
            int iw = 0, ih = 0;
            if (ReadImageHeader(im.path.c_str(), &iw, &ih) && iw > 0 && ih > 0) { cw = iw; ch = ih; }
        }
        AcquireSRWLockExclusive(&g_imgLock);
        im.w = cw;
        im.h = ch;
        ReleaseSRWLockExclusive(&g_imgLock);
    }
    *w = cw;
    *h = ch;
    return cw > 0;
}

float ImageDisplayWidth(const Image& im, float width) {
    if (im.attrW > 0) return std::min((float)im.attrW, width);  // width="…" from HTML wins, as in a browser
    if (im.w > 0) return ImageScrollsWide(im) ? (float)im.w : std::min((float)im.w, width);
    return std::min(width, 320.f);
}

float ImageDisplayHeight(const Image& im, float width) {
    float w = ImageDisplayWidth(im, width);
    if (im.attrW > 0) {  // keep the ratio the attributes ask for, or the file's own
        float ratio = im.attrH > 0 ? (float)im.attrH / im.attrW : (im.w > 0 && im.h > 0 ? (float)im.h / im.w : 0.5f);
        return std::round(w * ratio);
    }
    if (im.w <= 0 || im.h <= 0) return 44.f;  // placeholder with alt text
    return std::round(im.h * w / im.w);
}

// ------------------------------------------------------------------------------------------------ blocks
float BlockHeightEstimate(const Doc& d, const Typography& t, const Block& b, float width, bool* exact) {
    *exact = false;
    float w = width - b.indent;
    switch (b.kind) {
    case BK_HR: *exact = true; return 4.f;
    case BK_CODE: {
        uint32_t lines = 1;
        const wchar_t* s = d.text.data() + b.textOff;
        for (uint32_t i = 0; i < b.textLen; i++) lines += s[i] == L'\n';
        *exact = !t.wrapCode;  // wrapped code: at least this tall, measured later
        return lines * t.lineH[R_CODE] + 2 * Metrics::kCodePad;
    }
    case BK_TABLE: {
        const Table& tb = d.tables[b.aux];
        return tb.rows * (t.lineH[R_BODY] + 2 * Metrics::kCellPadY + 1) + 1;
    }
    case BK_IMAGE: {
        const Image& im = d.images[b.aux];
        if (im.w >= 0) { *exact = true; return ImageDisplayHeight(im, w); }
        return 240.f;
    }
    default: {
        int role = b.heading ? b.heading : R_BODY;
        float charW = t.size[role] * 0.5f;
        float lines = std::ceil((b.textLen * charW + 1) / std::max(50.f, w));
        return std::max(1.f, lines) * t.lineH[role] + HeadingRuleExtra(t, b.heading);
    }
    }
}

// A picture inside a line of text (HTML <img>): DirectWrite reserves the box, the canvas paints it. The bottom of
// the picture sits on the baseline, the way a browser places it.
namespace {
struct InlineImage final : IDWriteInlineObject {
    const Doc* doc;
    IDWriteFactory3* factory;  // for a formula that could not be typeset: its source, set in the UI font
    IDWriteTextFormat* fmt;
    uint32_t index;
    float w, h, baseline;  // a formula sits on the text baseline; a picture stands on it
    ULONG refs = 1;
    InlineImage(const Doc* d, IDWriteFactory3* fac, IDWriteTextFormat* f, uint32_t i, float ww, float hh, float bl)
        : doc(d), factory(fac), fmt(f), index(i), w(ww), h(hh), baseline(bl) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) {
            *ppv = static_cast<IDWriteInlineObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer* renderer, FLOAT x, FLOAT y, BOOL, BOOL,
                                   IUnknown*) override {
        Canvas* c = CanvasOfRenderer(renderer);
        if (!c || index >= doc->images.size()) return S_OK;
        Image& im = const_cast<Doc*>(doc)->images[index];
        if (im.state == RS_OK) {
            c->DrawImage(im, x, y, x + w, y + h);
        } else if (im.state == RS_FAILED && im.mathKind && !im.alt.empty() && factory && fmt) {
            // a formula the engine could not typeset: its source, as the author wrote it
            IDWriteTextLayout* L = nullptr;
            if (SUCCEEDED(factory->CreateTextLayout(im.alt.c_str(), (UINT32)im.alt.size(), fmt,
                                                    std::max(w, 8.f) * 4.f, std::max(h, 8.f), &L))) {
                c->Text(L, x, y, P_MUTED);
                L->Release();
            }
        } else {
            c->FillRoundRect(x, y, x + w, y + h, 4.f, P_PLACEHOLDER);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* m) override {
        m->width = w;
        m->height = h;
        m->baseline = baseline;
        m->supportsSideways = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* o) override {
        *o = DWRITE_OVERHANG_METRICS{};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before,
                                                 DWRITE_BREAK_CONDITION* after) override {
        *before = *after = DWRITE_BREAK_CONDITION_CAN_BREAK;
        return S_OK;
    }
};
}  // namespace

// returns the height of the tallest picture inside the line, so the caller can make room for it
static float ApplyRuns(const Doc& d, const Typography& t, IDWriteTextLayout* L, uint32_t textOff, uint32_t runOff,
                       uint32_t runCount, int role, bool heading) {
    IDWriteTextLayout1* L1 = nullptr;
    float tallest = 0;
    for (uint32_t k = 0; k < runCount; k++) {
        const Run& r = d.runs[runOff + k];
        if (!r.flags) continue;
        DWRITE_TEXT_RANGE rg{r.start - textOff, r.len};
        if (r.flags & F_BOLD) L->SetFontWeight(heading ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, rg);
        if (r.flags & F_ITALIC) L->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, rg);
        if (r.flags & F_STRIKE) L->SetStrikethrough(TRUE, rg);
        if (r.flags & F_ICON) {
            L->SetFontFamilyName(t.iconFamily, rg);
            L->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, rg);
        }
        if (r.flags & F_IMAGE) {
            int iw = 0, ih = 0;
            ImageSize(const_cast<Doc&>(d), r.image, &iw, &ih);  // header size, cached in the image
            float maxW = std::max(24.f, L->GetMaxWidth());
            const Image& im = d.images[r.image];
            // nothing known yet (a badge still being fetched): hold a badge-sized box so the line does not jump much
            bool unknown = im.attrW <= 0 && im.w <= 0;
            float boxW = unknown ? 96.f : ImageDisplayWidth(im, maxW);
            float boxH = unknown ? 20.f : ImageDisplayHeight(im, maxW);
            // a formula in the line stands on the text baseline, with its own descenders below it
            float bl = boxH;
            if (im.mathKind == 1 && im.ascent > 0 && im.h > 0) bl = boxH * im.ascent / (float)im.h;
            auto* obj = new InlineImage(&d, t.factory, t.ui, r.image, boxW, boxH, bl);
            L->SetInlineObject(obj, rg);
            obj->Release();
            tallest = std::max(tallest, boxH);
            continue;
        }
        if (r.flags & (F_SUP | F_SUB)) L->SetFontSize(std::round(t.size[role] * 0.72f), rg);  // lifted in the canvas
        if (r.flags & (F_CODE | F_KBD)) {
            float fs = std::round(t.size[role] * 0.85f * 2.f) / 2.f;
            L->SetFontFamilyName(t.monoFamily, rg);
            L->SetFontSize(fs, rg);
            if (!heading) L->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, rg);
            // padding .2em .4em → leading spacing on the first char, trailing on the last
            if (!L1) L->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&L1);
            if (L1) {
                float pad = std::round(fs * 0.4f);
                if (r.len == 1) L1->SetCharacterSpacing(pad, pad, 0, rg);
                else {
                    L1->SetCharacterSpacing(pad, 0, 0, DWRITE_TEXT_RANGE{rg.startPosition, 1});
                    L1->SetCharacterSpacing(0, pad, 0, DWRITE_TEXT_RANGE{rg.startPosition + rg.length - 1, 1});
                }
            }
        }
    }
    SafeRelease(L1);
    // our lines have a fixed height; without this a picture taller than the line would hang above it
    if (tallest > 0) {
        float lh = std::max(t.lineH[role], tallest + 6.f);
        L->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lh, std::max(t.baseline[role], tallest + 3.f));
    }
    return tallest;
}

static TableLayout* LayoutTable(const Doc& d, const Typography& t, const Table& tb, float avail) {
    auto* tl = new TableLayout();
    size_t n = (size_t)tb.rows * tb.cols;
    tl->cells.assign(n, nullptr);
    std::vector<float> colNat(tb.cols, 0.f), colMin(tb.cols, 0.f);
    const float padX = 2 * Metrics::kCellPadX + 1;
    for (uint32_t r = 0; r < tb.rows; r++) {
        for (uint32_t c = 0; c < tb.cols; c++) {
            const Cell& cell = d.cells[tb.cellOff + r * tb.cols + c];
            IDWriteTextLayout* L = nullptr;
            if (FAILED(t.factory->CreateTextLayout(d.text.data() + cell.textOff, cell.textLen, t.fmt[R_BODY], 100000.f,
                                                   100000.f, &L)))
                continue;
            ApplyRuns(d, t, L, cell.textOff, cell.runOff, cell.runCount, R_BODY, false);
            DWRITE_TEXT_METRICS m{};
            L->GetMetrics(&m);
            FLOAT mw = 0;
            L->DetermineMinWidth(&mw);
            colNat[c] = std::max(colNat[c], std::ceil(m.widthIncludingTrailingWhitespace) + padX);
            colMin[c] = std::max(colMin[c], std::ceil(mw) + padX);
            tl->cells[r * tb.cols + c] = L;
        }
    }
    float sumNat = 0, sumMin = 0;
    for (uint32_t c = 0; c < tb.cols; c++) { sumNat += colNat[c]; sumMin += colMin[c]; }
    tl->colW.resize(tb.cols);
    if (sumNat + 1 <= avail || sumNat <= sumMin) tl->colW = colNat;
    else if (sumMin + 1 >= avail) tl->colW = colMin;
    else {
        float extra = avail - 1 - sumMin;
        for (uint32_t c = 0; c < tb.cols; c++)
            tl->colW[c] = std::floor(colMin[c] + extra * (colNat[c] - colMin[c]) / (sumNat - sumMin));
    }
    tl->rowH.assign(tb.rows, 0.f);
    for (uint32_t r = 0; r < tb.rows; r++) {
        float rh = t.lineH[R_BODY];
        for (uint32_t c = 0; c < tb.cols; c++) {
            IDWriteTextLayout* L = tl->cells[r * tb.cols + c];
            if (!L) continue;
            L->SetMaxWidth(std::max(1.f, tl->colW[c] - padX));
            uint8_t al = d.aligns[tb.alignOff + c];
            L->SetTextAlignment(al == 2 ? DWRITE_TEXT_ALIGNMENT_CENTER : al == 3 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                                               : DWRITE_TEXT_ALIGNMENT_LEADING);
            DWRITE_TEXT_METRICS m{};
            L->GetMetrics(&m);
            rh = std::max(rh, m.height);
        }
        tl->rowH[r] = rh + 2 * Metrics::kCellPadY + 1;
    }
    tl->width = 1;
    for (float w : tl->colW) tl->width += w;
    tl->height = 1;
    for (float h : tl->rowH) tl->height += h;
    return tl;
}

BlockLayout* LayoutBlock(const Doc& d, const Typography& t, uint32_t index, float width) {
    const Block& b = d.blocks[index];
    auto* bl = new BlockLayout();
    float w = std::max(40.f, width - b.indent);
    bl->width = width;
    switch (b.kind) {
    case BK_TEXT: {
        int role = b.heading ? b.heading : R_BODY;
        if (SUCCEEDED(t.factory->CreateTextLayout(d.text.data() + b.textOff, b.textLen, t.fmt[role], w, 1e7f, &bl->text))) {
            if (b.align)  // <p align=center> and friends
                bl->text->SetTextAlignment(b.align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
            ApplyRuns(d, t, bl->text, b.textOff, b.runOff, b.runCount, role, b.heading != 0);
            DWRITE_TEXT_METRICS m{};
            bl->text->GetMetrics(&m);
            bl->height = m.height + HeadingRuleExtra(t, b.heading);
        }
        break;
    }
    case BK_CODE: {
        float inner = std::max(10.f, w - 2 * Metrics::kCodePad);
        if (SUCCEEDED(t.factory->CreateTextLayout(d.text.data() + b.textOff, b.textLen, t.fmt[R_CODE], inner, 1e7f, &bl->text))) {
            if (t.wrapCode) bl->text->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
            DWRITE_TEXT_METRICS m{};
            bl->text->GetMetrics(&m);
            bl->height = m.height + 2 * Metrics::kCodePad;
            bl->natural = std::ceil(m.widthIncludingTrailingWhitespace) + 2 * Metrics::kCodePad;
        }
        if (b.aux && b.aux - 1 < d.langNames.size()) {  // the language name for the block's corner
            const std::wstring& name = d.langNames[b.aux - 1];
            if (SUCCEEDED(t.factory->CreateTextLayout(name.data(), (UINT32)name.size(), t.ui, 200.f, 40.f, &bl->label)))
                bl->label->SetFontSize(11.5f, DWRITE_TEXT_RANGE{0, (UINT32)name.size()});
        }
        break;
    }
    case BK_HR: bl->height = 4.f; break;
    case BK_TABLE:
        bl->table = LayoutTable(d, t, d.tables[b.aux], w);
        bl->height = bl->table->height;
        bl->natural = bl->table->width;
        break;
    case BK_IMAGE: {
        int iw, ih;
        ImageSize(const_cast<Doc&>(d), b.aux, &iw, &ih);
        bl->height = ImageDisplayHeight(d.images[b.aux], w);
        bl->natural = ImageDisplayWidth(d.images[b.aux], w);
        if (b.textLen)  // alt text for the placeholder / failed image
            t.factory->CreateTextLayout(d.text.data() + b.textOff, b.textLen, t.fmt[R_BODY], w, 1e7f, &bl->text);
        break;
    }
    }
    return bl;
}
