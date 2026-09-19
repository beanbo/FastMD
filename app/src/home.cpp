// Start screen (FastMD started without a file): the last documents from positions.bin, filtered by name as you type.
// Up / Down + Enter or a click opens one; the list comes from the doc thread, so it is in the first frame.
#include "app.h"

namespace {
const float kColW = 600.f, kItemH = 54.f, kFilterH = 38.f;
const size_t kShown = 10;
float g_left = 0, g_listTop = 0, g_colW = kColW;  // geometry of the last frame (hit-testing)

void Geometry(float* left, float* titleY, float* filterY, float* listTop, float* colW) {
    *colW = std::min(kColW, ViewW() - 48.f);
    *left = std::floor((ViewW() - *colW) * 0.5f);
    *titleY = std::max(28.f, std::floor(ViewH() * 0.1f));
    *filterY = *titleY + 52.f;
    *listTop = *filterY + kFilterH + 14.f;
}

void Centered(IDWriteTextLayout* L, float y, uint8_t pal) {
    if (!L) return;
    L->SetMaxWidth(ViewW());
    L->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g.canvas->Text(L, 0, y, pal);
    L->Release();
}

void DrawEmpty() {
    float cy = ViewH() * 0.42f;
    Centered(UiLayout(Tr(S_EMPTY_TITLE), ViewW(), g.typo.fmt[R_H2]), cy - 40.f, P_TEXT);
    Centered(UiLayout(Tr(S_EMPTY_HINT), ViewW()), cy + 4.f, P_MUTED);
}

IDWriteTextLayout* Trimmed(const std::wstring& s, float maxW, IDWriteTextFormat* fmt) {
    IDWriteTextLayout* L = UiLayout(s, maxW, fmt);
    if (!L) return nullptr;
    L->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* ell = nullptr;
    if (SUCCEEDED(g.dwf->CreateEllipsisTrimmingSign(L, &ell))) {
        L->SetTrimming(&tr, ell);
        ell->Release();
    }
    return L;
}
}  // namespace

void HomeRebuild() {
    g.recentShown.clear();
    std::wstring f = ToLower(g.recentFilter);
    for (size_t i = 0; i < g.recentAll.size() && g.recentShown.size() < kShown; i++) {
        if (!f.empty() && ToLower(FileNameOf(g.recentAll[i].path)).find(f) == std::wstring::npos) continue;
        g.recentShown.push_back((int)i);
    }
    g.recentSel = std::clamp(g.recentSel, 0, std::max(0, (int)g.recentShown.size() - 1));
    g.recentHover = -1;
}

void DrawHome() {
    if (g.recentAll.empty()) { DrawEmpty(); return; }
    float left, titleY, filterY, listTop, colW;
    Geometry(&left, &titleY, &filterY, &listTop, &colW);
    g_left = left;
    g_listTop = listTop;
    g_colW = colW;
    if (IDWriteTextLayout* t = UiLayout(Tr(S_RECENT_TITLE), colW, g.typo.fmt[R_H3])) {
        g.canvas->Text(t, left, titleY, P_TEXT);
        t->Release();
    }
    // filter box (typing goes here: the start screen has no other text input)
    g.canvas->FillRoundRect(left, filterY, left + colW, filterY + kFilterH, 6.f, P_BG);
    g.canvas->StrokeRoundRect(left, filterY, left + colW, filterY + kFilterH, 6.f, 1.5f, P_ACCENT);
    static const wchar_t kSearch = 0xE721;  // Segoe Fluent Icons: Search
    IDWriteTextLayout* icon = nullptr;
    if (SUCCEEDED(g.dwf->CreateTextLayout(&kSearch, 1, g.typo.uiIcon, 32.f, kFilterH, &icon))) {
        icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g.canvas->Text(icon, left + 4.f, filterY, P_MUTED);
        icon->Release();
    }
    bool empty = g.recentFilter.empty();
    if (IDWriteTextLayout* ft = UiLayout(empty ? std::wstring(Tr(S_RECENT_FILTER)) : g.recentFilter, colW - 60.f)) {
        ft->SetFontSize(14.f, DWRITE_TEXT_RANGE{0, 1000});
        DWRITE_TEXT_METRICS m{};
        ft->GetMetrics(&m);
        float tx = left + 38.f, ty = filterY + (kFilterH - m.height) * 0.5f;
        g.canvas->Text(ft, tx, ty, empty ? P_MUTED : P_TEXT);
        float cx = empty ? tx : tx + m.widthIncludingTrailingWhitespace + 1.f;
        g.canvas->FillRect(cx, ty + 2.f, cx + 1.2f, ty + m.height - 2.f, P_TEXT);
        ft->Release();
    }
    // list
    float y = listTop;
    if (g.recentShown.empty()) {
        if (IDWriteTextLayout* nt = UiLayout(Tr(S_RECENT_NONE), colW)) {
            g.canvas->Text(nt, left + 14.f, y + 12.f, P_MUTED);
            nt->Release();
        }
        y += kItemH;
    }
    for (size_t k = 0; k < g.recentShown.size(); k++, y += kItemH) {
        const PosEntry& e = g.recentAll[g.recentShown[k]];
        if ((int)k == g.recentSel) g.canvas->FillRoundRect(left, y, left + colW, y + kItemH - 4.f, 6.f, P_CURRENT);
        else if ((int)k == g.recentHover) g.canvas->FillRoundRect(left, y, left + colW, y + kItemH - 4.f, 6.f, P_HOVER);
        if (IDWriteTextLayout* nl = Trimmed(FileNameOf(e.path), colW - 28.f, g.typo.fmt[R_BODY])) {
            g.canvas->Text(nl, left + 14.f, y + 4.f, P_TEXT);
            nl->Release();
        }
        std::wstring dir = DirOf(e.path);
        if (dir.size() > 1 && dir.back() == L'\\') dir.pop_back();
        if (IDWriteTextLayout* dl = Trimmed(dir, colW - 28.f, nullptr)) {
            g.canvas->Text(dl, left + 14.f, y + 28.f, P_MUTED);
            dl->Release();
        }
    }
    if (IDWriteTextLayout* h = UiLayout(Tr(S_EMPTY_HINT), colW)) {
        g.canvas->Text(h, left + 14.f, y + 14.f, P_MUTED);
        h->Release();
    }
}

int HomeItemAt(float x, float y) {
    if (!g.path.empty() || g.recentShown.empty() || x < g_left || x > g_left + g_colW || y < g_listTop) return -1;
    int k = (int)std::floor((y - g_listTop) / kItemH);
    return k >= 0 && k < (int)g.recentShown.size() && y - g_listTop - k * kItemH < kItemH - 4.f ? k : -1;
}

void HomeOpen(int k) {
    if (k < 0 || k >= (int)g.recentShown.size()) return;
    std::wstring path = g.recentAll[g.recentShown[k]].path;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {  // gone: drop it from the list
        ShowToast(Tr(S_FILE_NOT_FOUND) + FileNameOf(path), 2500);
        PositionsRemove(path);
        g.recentAll.erase(g.recentAll.begin() + g.recentShown[k]);
        HomeRebuild();
        Invalidate();
        return;
    }
    OpenDocument(path, false, 0, true);
}

bool HomeKey(WPARAM vk) {
    if (!g.path.empty() || g.recentAll.empty()) return false;
    int n = (int)g.recentShown.size();
    switch (vk) {
    case VK_DOWN: if (n) g.recentSel = (g.recentSel + 1) % n; break;
    case VK_UP: if (n) g.recentSel = (g.recentSel - 1 + n) % n; break;
    case VK_HOME: g.recentSel = 0; break;
    case VK_END: g.recentSel = std::max(0, n - 1); break;
    case VK_RETURN: HomeOpen(g.recentSel); return true;
    case VK_BACK:
        if (g.recentFilter.empty()) return true;
        g.recentFilter.pop_back();
        HomeRebuild();
        break;
    case VK_ESCAPE:
        if (g.recentFilter.empty()) return false;  // → close the window
        g.recentFilter.clear();
        HomeRebuild();
        break;
    default: return false;
    }
    Invalidate();
    return true;
}

bool HomeChar(wchar_t c) {
    if (!g.path.empty() || g.recentAll.empty() || c < 32 || c == 127) return false;
    g.recentFilter.push_back(c);
    g.recentSel = 0;
    HomeRebuild();
    Invalidate();
    return true;
}
