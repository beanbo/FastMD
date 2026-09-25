// Printing and PDF export (plan 3.4).
//
// The page is laid out by the same engine that draws the window: the document is re-wrapped once for the width of the
// paper, the pages are cut along block boundaries (and along lines or table rows inside a block too big for one page),
// and every page is drawn through a canvas that talks to the printer's DC. The window's own geometry is borrowed for
// the duration and handed back afterwards.
//
// PDF is the same path with the output sent to the "Microsoft Print to PDF" driver and a file name given up front, so
// Windows writes the file straight away instead of asking for it a second time.
#include "app.h"

#include <commdlg.h>

#include "canvas.h"

namespace {
constexpr float kMarginIn = 0.55f;   // from the edge of the paper
constexpr float kHeaderIn = 0.34f;   // band above the text: the file name
constexpr float kFooterIn = 0.34f;   // band below it: the page number
const wchar_t* kPdfPrinter = L"Microsoft Print to PDF";

// everything print borrows from the window
struct ViewState {
    int pxW, pxH;
    float dpi, zoom, textW, wideW, scrollY, targetY;
    uint32_t selA, selB;
    uint32_t anchor;   // block the window was scrolled to, and the offset into it: the reading place
    float anchorOff;
    bool caret, tocOpen, findOpen, dark;
    int hoverLink, hoverCode, hoverHeading, hoverTask, focusLink;
    Canvas* canvas;
};

void Save(ViewState& v) {
    v.pxW = g.pxW; v.pxH = g.pxH; v.dpi = g.dpi; v.zoom = g.cfg.zoom;
    v.textW = g.textW; v.wideW = g.wideW; v.scrollY = g.scrollY; v.targetY = g.targetY;
    v.selA = g.selAnchor; v.selB = g.selFocus; v.caret = g.caretOn;
    v.tocOpen = g.tocOpen; v.findOpen = g.findOpen; v.dark = PaletteIsDark();
    v.hoverLink = g.hoverLink; v.hoverCode = g.hoverCode; v.hoverHeading = g.hoverHeading; v.hoverTask = g.hoverTask;
    v.focusLink = g.focusLink;
    v.canvas = g.canvas;
    v.anchor = g.doc.blocks.empty() ? 0 : FirstVisible(g.scrollY);
    v.anchorOff = v.anchor < g.Y.size() ? g.scrollY - g.Y[v.anchor] : 0.f;
}

void Restore(const ViewState& v) {
    delete g.canvas;  // the print canvas; the window's own is put back below
    g.canvas = v.canvas;
    g.fitWide = false;
    g.pxW = v.pxW; g.pxH = v.pxH; g.dpi = v.dpi; g.cfg.zoom = v.zoom;
    g.scrollY = v.scrollY; g.targetY = v.targetY;
    g.selAnchor = v.selA; g.selFocus = v.selB; g.caretOn = v.caret;
    g.tocOpen = v.tocOpen; g.findOpen = v.findOpen;
    g.hoverLink = v.hoverLink; g.hoverCode = v.hoverCode; g.hoverHeading = v.hoverHeading; g.hoverTask = v.hoverTask;
    g.focusLink = v.focusLink;
    SetDarkPalette(v.dark);
    Relayout();  // back to the window's width; the print generation's measure jobs are dropped
    // Relayout anchors the view on the block at the top of the *paper* layout, which means nothing here: put the
    // reading place back on the block the window was actually showing.
    if (v.anchor < g.Y.size()) g.scrollY = g.Y[v.anchor] + v.anchorOff;
    g.scrollY = std::clamp(g.scrollY, 0.f, MaxScroll());
    g.targetY = std::clamp(g.scrollY + (v.targetY - v.scrollY), 0.f, MaxScroll());
    ForceFullRedraw();
    Invalidate();
}

// paper, in DIP: where the text stands and how tall one page is
struct Page {
    float pageH;            // height of the text area
    int mL, mT;             // margins in device pixels
    int areaW, areaH;       // text area in device pixels (header and footer excluded from areaH)
    int headerDev, footerDev;
};

// Cut points: the document y where each page starts. A block that does not fit moves to the next page whole; one that
// cannot fit on any page is split at the last line (or table row) that fits.
std::vector<float> Paginate(float pageH) {
    std::vector<float> tops{0.f};
    float cur = 0;
    size_t n = g.doc.blocks.size();
    for (size_t i = 0; i < n; i++) {
        const Block& b = g.doc.blocks[i];
        if (BlockHidden(b) || g.H[i] <= 0) continue;
        float top = g.Y[i], bot = top + g.H[i];
        if (bot <= cur + pageH) continue;
        float next;
        if (top <= cur + 0.5f) {  // starts on this page and runs past its bottom
            float room = cur + pageH - top;
            float cut = BlockSplitY((uint32_t)i, room);
            next = top + (cut > 4.f ? cut : std::max(room, 1.f));
        } else {
            next = top;  // the whole block starts the next page
        }
        if (next <= cur) next = cur + pageH;  // never stand still
        cur = next;
        tops.push_back(cur);
        if (tops.size() > 5000) break;  // a document that long is a mistake, not a print job
        i--;  // the same block may still not fit
    }
    return tops;
}

void DrawLabel(const std::wstring& s, float x, float y, float w, bool center, uint8_t pal) {
    IDWriteTextLayout* L = UiLayout(s, w);
    if (!L) return;
    if (center) L->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g.canvas->Text(L, x, y, pal);
    L->Release();
}

// One job: dc is a printer DC, outFile a path for "print to file" (PDF) or nullptr, [from, to] a 1-based page range.
bool PrintPages(HDC dc, const wchar_t* jobName, const wchar_t* outFile, int from, int to) {
    int dpiX = GetDeviceCaps(dc, LOGPIXELSX), dpiY = GetDeviceCaps(dc, LOGPIXELSY);
    int resX = GetDeviceCaps(dc, HORZRES), resY = GetDeviceCaps(dc, VERTRES);
    int offX = GetDeviceCaps(dc, PHYSICALOFFSETX), offY = GetDeviceCaps(dc, PHYSICALOFFSETY);
    if (dpiX <= 0 || dpiY <= 0 || resX <= 0 || resY <= 0) return false;
    // printers with different horizontal and vertical resolution: make one logical unit square, at the vertical one
    if (dpiX != dpiY) {
        SetMapMode(dc, MM_ANISOTROPIC);
        SetWindowExtEx(dc, dpiY, dpiY, nullptr);
        SetViewportExtEx(dc, dpiX, dpiY, nullptr);
        resX = MulDiv(resX, dpiY, dpiX);
        offX = MulDiv(offX, dpiY, dpiX);
    }
    const int dpi = dpiY;
    Page pg{};
    pg.mL = std::max(0, (int)(kMarginIn * dpi) - offX);
    pg.mT = std::max(0, (int)(kMarginIn * dpi) - offY);
    pg.headerDev = (int)(kHeaderIn * dpi);
    pg.footerDev = (int)(kFooterIn * dpi);
    pg.areaW = resX - 2 * pg.mL;
    pg.areaH = resY - 2 * pg.mT - pg.headerDev - pg.footerDev;
    if (pg.areaW < dpi || pg.areaH < dpi) return false;  // less than an inch of paper to write on

    ViewState saved;
    Save(saved);
    float s = (float)dpi / 96.f;
    g.canvas = CreatePrintCanvas(g.dwf, dc, pg.areaW, pg.areaH, s);
    g.dpi = (float)dpi;
    g.cfg.zoom = 1.f;      // paper has no zoom
    g.pxW = pg.areaW;
    g.pxH = pg.areaH;
    g.tocOpen = false;     // no outline panel, no selection, no search marks, no hover on paper
    g.findOpen = false;
    g.selAnchor = g.selFocus = 0;
    g.caretOn = false;
    g.hoverLink = g.hoverCode = g.hoverHeading = g.hoverTask = g.focusLink = -1;
    SetDarkPalette(false);  // dark pages are for screens
    g.fitWide = true;       // paper does not scroll: a wide diagram shrinks to the page instead
    pg.pageH = pg.areaH / s;

    ClearLayoutCache();
    g.gen++;  // measure jobs for the window's width stop here; nothing new is started while printing
    UpdateColumns();
    InitGeometry();
    for (uint32_t i = 0; i < g.doc.blocks.size(); i++) {  // exact heights: pages are cut by them
        EnsureLayout(i);
        if ((i & 255) == 255) TrimCache();
    }
    RecomputeY();

    std::vector<float> tops = Paginate(pg.pageH);
    int pages = (int)tops.size();
    if (to <= 0 || to > pages) to = pages;
    if (from < 1) from = 1;

    DOCINFOW di{sizeof(di)};
    di.lpszDocName = jobName;
    di.lpszOutput = outFile;
    bool started = StartDocW(dc, &di) > 0, ok = started;
    std::wstring name = FileNameOf(g.path);
    for (int p = from; ok && p <= to; p++) {
        if (StartPage(dc) <= 0) { ok = false; break; }
        SetViewportOrgEx(dc, pg.mL, pg.mT + pg.headerDev, nullptr);
        g.canvas->PushClip(0, 0, pg.areaW / s, pg.pageH);
        DrawDocumentPage(tops[p - 1], tops[p - 1] + pg.pageH);
        g.canvas->PopClip();
        SetViewportOrgEx(dc, 0, 0, nullptr);
        float w = pg.areaW / s;
        DrawLabel(name, pg.mL / s, pg.mT / s, w, false, P_MUTED);
        wchar_t num[64];
        swprintf_s(num, Tr(S_PRINT_PAGE_FMT), p, pages);
        DrawLabel(num, pg.mL / s, (pg.mT + pg.headerDev + pg.areaH + pg.footerDev * 0.25f) / s, w, true, P_MUTED);
        if (EndPage(dc) <= 0) { ok = false; break; }
    }
    // the paper's layouts go now, while the job and the print canvas they were drawn with are alive: released after
    // EndDoc and the canvas, DirectWrite read freed memory now and then (a PDF export crashed under load, 1.2.0 too)
    ClearLayoutCache();
    if (ok) EndDoc(dc);
    else if (started) AbortDoc(dc);
    Restore(saved);
    return ok;
}
}  // namespace

// Ctrl+P: the system dialog picks the printer, the paper and the pages.
void PrintDocument() {
    if (g.path.empty() || g.doc.blocks.empty()) return;
    // the dialog and the job (whose driver may show a dialog of its own in StartDocW, after the pages are cut): no
    // reload, no picture changes the model before the last page is out
    ModalScope modal;
    PRINTDLGW pd{sizeof(pd)};
    pd.hwndOwner = g.hwnd;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
    pd.nMinPage = 1;
    pd.nMaxPage = 9999;
    pd.nFromPage = 1;
    pd.nToPage = 9999;
    pd.nCopies = 1;
    if (!PrintDlgW(&pd)) return;  // cancelled, or no printer at all
    int from = (pd.Flags & PD_PAGENUMS) ? pd.nFromPage : 1;
    int to = (pd.Flags & PD_PAGENUMS) ? pd.nToPage : 0;
    std::wstring name = FileNameOf(g.path);
    bool ok = pd.hDC && PrintPages(pd.hDC, name.c_str(), nullptr, from, to);
    if (pd.hDC) DeleteDC(pd.hDC);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    ShowToast(Tr(ok ? S_PRINTED : S_PRINT_FAILED));
}

// "Экспорт в PDF": our own save dialog, then the same job through the PDF driver with the file name set up front.
void ExportPdf() {
    if (g.path.empty() || g.doc.blocks.empty()) return;
    ModalScope modal;  // the save dialog and the job, as PrintDocument
    std::wstring path = SavePdfDialog();
    if (path.empty()) return;
    HDC dc = CreateDCW(L"WINSPOOL", kPdfPrinter, nullptr, nullptr);
    if (!dc) {
        ShowToast(Tr(S_PDF_NO_PRINTER), 2600);
        return;
    }
    std::wstring name = FileNameOf(g.path);
    bool ok = PrintPages(dc, name.c_str(), path.c_str(), 1, 0);
    DeleteDC(dc);
    ShowToast(Tr(ok ? S_PDF_SAVED : S_PRINT_FAILED));
}
