// DirectWrite typography + per-block layout.
#include "layout.h"

namespace {
struct RoleSpec { float size, lineH; DWRITE_FONT_WEIGHT weight; bool display; };
// Product type ramp (research/06 §8.2): body 16/26, h1 32/40, h2 24/32, h3 20/28, h4 17/24, h5 16/24, h6 14/20,
// code 14/21. h1–h3 use the Display optical size of Segoe UI Variable.
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
}  // namespace

float HeadingRuleExtra(int heading) {
    return (heading == 1 || heading == 2) ? std::round(kRoles[heading].size * 0.3f) + 1.f : 0.f;
}

bool Typography::Init(IDWriteFactory3* f, const Typography* from) {
    factory = f;
    float bodyAsc = 0.93f, bodyDesc = 0.23f, dispAsc = 0.93f, dispDesc = 0.23f;
    if (from) {
        wcscpy_s(bodyFamily, from->bodyFamily);
        wcscpy_s(displayFamily, from->displayFamily);
        wcscpy_s(monoFamily, from->monoFamily);
        wcscpy_s(iconFamily, from->iconFamily);
        monoAscent = from->monoAscent;
        monoDescent = from->monoDescent;
        for (int r = 0; r < R_COUNT; r++) baseline[r] = from->baseline[r];
    } else {
        IDWriteFontCollection* coll = nullptr;
        if (SUCCEEDED(f->GetSystemFontCollection(&coll, FALSE))) {
            if (!FontMetricsFor(coll, bodyFamily, &bodyAsc, &bodyDesc)) {
                wcscpy_s(bodyFamily, L"Segoe UI");
                FontMetricsFor(coll, bodyFamily, &bodyAsc, &bodyDesc);
            }
            if (!FontMetricsFor(coll, displayFamily, &dispAsc, &dispDesc)) {
                wcscpy_s(displayFamily, bodyFamily);
                dispAsc = bodyAsc;
                dispDesc = bodyDesc;
            }
            if (!FontMetricsFor(coll, monoFamily, &monoAscent, &monoDescent)) {
                wcscpy_s(monoFamily, L"Consolas");
                FontMetricsFor(coll, monoFamily, &monoAscent, &monoDescent);
            }
            if (!FontMetricsFor(coll, iconFamily, nullptr, nullptr)) wcscpy_s(iconFamily, L"Segoe MDL2 Assets");
            coll->Release();
        }
    }
    for (int r = 0; r < R_COUNT; r++) {
        const RoleSpec& s = kRoles[r];
        size[r] = s.size;
        lineH[r] = s.lineH;
        bool mono = r == R_CODE;
        const wchar_t* fam = mono ? monoFamily : s.display ? displayFamily : bodyFamily;
        if (!from) {
            float a = mono ? monoAscent : s.display ? dispAsc : bodyAsc;
            float d = mono ? monoDescent : s.display ? dispDesc : bodyDesc;
            // CSS half-leading: the line box is lineH tall, content area (ascent+descent) centred in it
            baseline[r] = std::round(((s.lineH - (a + d) * s.size) * 0.5f + a * s.size) * 4.f) / 4.f;
        }
        if (FAILED(f->CreateTextFormat(fam, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       s.size, L"en-us", &fmt[r])))
            return false;
        fmt[r]->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, s.lineH, baseline[r]);
        fmt[r]->SetWordWrapping(mono ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
    }
    f->CreateTextFormat(bodyFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
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

float ImageDisplayHeight(const Image& im, float width) {
    if (im.w <= 0 || im.h <= 0) return 44.f;  // placeholder with alt text
    float w = std::min((float)im.w, width);
    return std::round(im.h * w / im.w);
}

// ------------------------------------------------------------------------------------------------ blocks
float BlockHeightEstimate(const Doc& d, const Block& b, float width, bool* exact) {
    *exact = false;
    float w = width - b.indent;
    switch (b.kind) {
    case BK_HR: *exact = true; return 4.f;
    case BK_CODE: {
        uint32_t lines = 1;
        const wchar_t* s = d.text.data() + b.textOff;
        for (uint32_t i = 0; i < b.textLen; i++) lines += s[i] == L'\n';
        *exact = true;
        return lines * kRoles[R_CODE].lineH + 2 * Metrics::kCodePad;
    }
    case BK_TABLE: {
        const Table& t = d.tables[b.aux];
        return t.rows * (kRoles[R_BODY].lineH + 2 * Metrics::kCellPadY + 1) + 1;
    }
    case BK_IMAGE: {
        const Image& im = d.images[b.aux];
        if (im.w >= 0) { *exact = true; return ImageDisplayHeight(im, w); }
        return 240.f;
    }
    default: {
        int role = b.heading ? b.heading : R_BODY;
        float charW = kRoles[role].size * 0.5f;
        float lines = std::ceil((b.textLen * charW + 1) / std::max(50.f, w));
        return std::max(1.f, lines) * kRoles[role].lineH + HeadingRuleExtra(b.heading);
    }
    }
}

static void ApplyRuns(const Doc& d, const Typography& t, IDWriteTextLayout* L, uint32_t textOff, uint32_t runOff,
                      uint32_t runCount, int role, bool heading) {
    IDWriteTextLayout1* L1 = nullptr;
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
        if (r.flags & F_CODE) {
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
            ApplyRuns(d, t, bl->text, b.textOff, b.runOff, b.runCount, role, b.heading != 0);
            DWRITE_TEXT_METRICS m{};
            bl->text->GetMetrics(&m);
            bl->height = m.height + HeadingRuleExtra(b.heading);
        }
        break;
    }
    case BK_CODE: {
        float inner = std::max(10.f, w - 2 * Metrics::kCodePad);
        if (SUCCEEDED(t.factory->CreateTextLayout(d.text.data() + b.textOff, b.textLen, t.fmt[R_CODE], inner, 1e7f, &bl->text))) {
            DWRITE_TEXT_METRICS m{};
            bl->text->GetMetrics(&m);
            bl->height = m.height + 2 * Metrics::kCodePad;
            bl->natural = std::ceil(m.widthIncludingTrailingWhitespace) + 2 * Metrics::kCodePad;
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
        bl->natural = iw > 0 ? std::min((float)iw, w) : std::min(w, 320.f);
        if (b.textLen)  // alt text for the placeholder / failed image
            t.factory->CreateTextLayout(d.text.data() + b.textOff, b.textLen, t.fmt[R_BODY], w, 1e7f, &bl->text);
        break;
    }
    }
    return bl;
}
