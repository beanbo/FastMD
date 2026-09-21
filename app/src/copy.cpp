// What the clipboard gets (plan 3.1, 3.2). Plain text is what every program understands; CF_HTML keeps the
// formatting for Word, Outlook and the web; RTF does the same for programs that do not read HTML; and "copy as
// Markdown" hands back the author's own source, found through the text → source map the parser recorded.
#include "app.h"

namespace {
uint32_t SelFrom() { return std::min(g.selAnchor, g.selFocus); }
uint32_t SelTo() { return std::max(g.selAnchor, g.selFocus); }

void Utf8(std::string& out, const wchar_t* s, size_t n) {
    if (!n) return;
    int len = WideCharToMultiByte(CP_UTF8, 0, s, (int)n, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return;
    size_t at = out.size();
    out.resize(at + len);
    WideCharToMultiByte(CP_UTF8, 0, s, (int)n, out.data() + at, len, nullptr, nullptr);
}

void HtmlEscape(std::string& out, const wchar_t* s, size_t n) {
    std::wstring esc;
    esc.reserve(n);
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case L'&': esc += L"&amp;"; break;
        case L'<': esc += L"&lt;"; break;
        case L'>': esc += L"&gt;"; break;
        case L'"': esc += L"&quot;"; break;
        case L'\xFFFC': break;  // the stand-in for a picture inside a line
        case L'\n': esc += L"<br>"; break;
        default: esc.push_back(s[i]);
        }
    }
    Utf8(out, esc.data(), esc.size());
}

void RtfText(std::string& out, const wchar_t* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        wchar_t c = s[i];
        if (c == L'\\' || c == L'{' || c == L'}') { out.push_back('\\'); out.push_back((char)c); }
        else if (c == L'\n') out += "\\line ";
        else if (c == L'\xFFFC') continue;
        else if (c < 0x80) out.push_back((char)c);
        else out += "\\u" + std::to_string((int)(int16_t)c) + "?";  // RTF wants a signed 16-bit code point
    }
}

// one inline run of a block, already cut down to the selection
struct Piece { uint32_t start, len; uint16_t flags; uint32_t link, image; };

void PiecesOf(uint32_t textOff, uint32_t runOff, uint32_t runCount, uint32_t from, uint32_t to,
              std::vector<Piece>& out) {
    uint32_t at = from;
    for (uint32_t k = 0; k < runCount && at < to; k++) {
        const Run& r = g.doc.runs[runOff + k];
        uint32_t rs = std::max(r.start, from), re = std::min(r.start + r.len, to);
        if (re <= rs) continue;
        if (rs > at) out.push_back(Piece{at, rs - at, 0, 0, 0});
        out.push_back(Piece{rs, re - rs, r.flags, r.link, r.image});
        at = re;
    }
    if (at < to) out.push_back(Piece{at, to - at, 0, 0, 0});
    (void)textOff;
}

void EmitPieces(const std::vector<Piece>& pieces, std::string& html, std::string& rtf) {
    const std::wstring& text = g.doc.text;
    for (const Piece& p : pieces) {
        const wchar_t* s = text.data() + p.start;
        bool link = (p.flags & F_LINK) && p.link < g.doc.links.size();
        if (link) {
            html += "<a href=\"";
            HtmlEscape(html, g.doc.links[p.link].data(), g.doc.links[p.link].size());
            html += "\">";
            rtf += "{\\field{\\*\\fldinst HYPERLINK \"";
            RtfText(rtf, g.doc.links[p.link].data(), g.doc.links[p.link].size());
            rtf += "\"}{\\fldrslt\\cf1\\ul ";
        }
        if (p.flags & F_BOLD) { html += "<strong>"; rtf += "\\b "; }
        if (p.flags & F_ITALIC) { html += "<em>"; rtf += "\\i "; }
        if (p.flags & F_STRIKE) { html += "<s>"; rtf += "\\strike "; }
        if (p.flags & F_SUP) html += "<sup>";
        if (p.flags & F_SUB) html += "<sub>";
        if (p.flags & (F_CODE | F_KBD)) { html += (p.flags & F_KBD) ? "<kbd>" : "<code>"; rtf += "\\f1 "; }
        // a formula is a picture on the page, but what belongs in another document is the source it was written from
        const Image* math = nullptr;
        if ((p.flags & F_IMAGE) && p.image < g.doc.images.size() && g.doc.images[p.image].mathKind &&
            !g.doc.images[p.image].alt.empty())
            math = &g.doc.images[p.image];
        if (math) {
            HtmlEscape(html, math->alt.data(), math->alt.size());
            RtfText(rtf, math->alt.data(), math->alt.size());
        } else {
            HtmlEscape(html, s, p.len);
            RtfText(rtf, s, p.len);
        }
        if (p.flags & (F_CODE | F_KBD)) { html += (p.flags & F_KBD) ? "</kbd>" : "</code>"; rtf += "\\f0 "; }
        if (p.flags & F_SUB) html += "</sub>";
        if (p.flags & F_SUP) html += "</sup>";
        if (p.flags & F_STRIKE) { html += "</s>"; rtf += "\\strike0 "; }
        if (p.flags & F_ITALIC) { html += "</em>"; rtf += "\\i0 "; }
        if (p.flags & F_BOLD) { html += "</strong>"; rtf += "\\b0 "; }
        if (link) {
            html += "</a>";
            rtf += "\\ul0\\cf0 }}";
        }
    }
}

const int kHeadSize[7] = {22, 36, 30, 26, 24, 22, 20};  // RTF half-points: body 11 pt, h1 18 pt …
}  // namespace

// Builds both rich formats for the current selection in one walk over the blocks.
static void BuildRich(std::string& html, std::string& rtf) {
    uint32_t from = SelFrom(), to = SelTo();
    rtf = "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fswiss Segoe UI;}{\\f1\\fmodern Consolas;}}"
          "{\\colortbl;\\red9\\green105\\blue218;}\\fs22 ";
    bool listOpen = false;
    for (size_t i = BlockOfPos(from); i < g.doc.blocks.size(); i++) {
        const Block& b = g.doc.blocks[i];
        if (b.textOff >= to && b.textLen) break;
        if (b.textOff > to) break;
        uint32_t bs = std::max(from, b.textOff), be = std::min(to, b.textOff + b.textLen);
        if (BlockHidden(b) || b.kind == BK_HR || be <= bs) {
            if (b.kind == BK_HR && bs <= be) { html += "<hr>"; rtf += "\\par\\brdrb\\brdrs\\par "; }
            continue;
        }
        bool item = b.marker != MK_NONE;
        if (item && !listOpen) { html += "<ul>"; listOpen = true; }
        if (!item && listOpen) { html += "</ul>"; listOpen = false; }
        if (b.kind == BK_CODE) {
            html += "<pre><code>";
            rtf += "\\par\\f1 ";
            HtmlEscape(html, g.doc.text.data() + bs, be - bs);
            RtfText(rtf, g.doc.text.data() + bs, be - bs);
            html += "</code></pre>";
            rtf += "\\f0\\par ";
            continue;
        }
        if (b.kind == BK_TABLE) {
            const Table& t = g.doc.tables[b.aux];
            html += "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\">";
            rtf += "\\par ";
            for (uint32_t r = 0; r < t.rows; r++) {
                html += "<tr>";
                for (uint32_t c = 0; c < t.cols; c++) {
                    const Cell& cell = g.doc.cells[t.cellOff + r * t.cols + c];
                    uint32_t cs = std::max(bs, cell.textOff), ce = std::min(be, cell.textOff + cell.textLen);
                    html += r == 0 ? "<th>" : "<td>";
                    if (ce > cs) {
                        std::vector<Piece> pieces;
                        PiecesOf(cell.textOff, cell.runOff, cell.runCount, cs, ce, pieces);
                        EmitPieces(pieces, html, rtf);
                    }
                    html += r == 0 ? "</th>" : "</td>";
                    if (c + 1 < t.cols) rtf += "\\tab ";
                }
                html += "</tr>";
                rtf += "\\par ";
            }
            html += "</table>";
            continue;
        }
        std::vector<Piece> pieces;
        PiecesOf(b.textOff, b.runOff, b.runCount, bs, be, pieces);
        int h = b.heading;
        if (h) {
            html += "<h" + std::to_string(h) + ">";
            rtf += "\\par\\b\\fs" + std::to_string(kHeadSize[h]) + " ";
        } else if (item) {
            html += "<li>";
            rtf += "\\par\\bullet\\tab ";
        } else {
            html += "<p>";
            rtf += "\\par ";
        }
        EmitPieces(pieces, html, rtf);
        if (h) {
            html += "</h" + std::to_string(h) + ">";
            rtf += "\\b0\\fs22 ";
        } else if (item) {
            html += "</li>";
        } else {
            html += "</p>";
        }
    }
    if (listOpen) html += "</ul>";
    rtf += "}";
}

// The source of the selection, as the author wrote it: the map says where each piece of text came from.
std::wstring SelectionMarkdown() {
    if (!HasSelection() || g.src.empty() || g.doc.srcMap.empty()) return SelectionText();
    uint32_t from = SelFrom(), to = SelTo();
    auto srcOf = [&](uint32_t pos, bool end) -> size_t {
        auto it = std::upper_bound(g.doc.srcMap.begin(), g.doc.srcMap.end(), std::pair<uint32_t, uint32_t>{pos, UINT32_MAX});
        if (it == g.doc.srcMap.begin()) return end ? g.src.size() : 0;
        --it;
        size_t off = it->second + (pos - it->first);  // inside a chunk text and source move together
        return std::min(off, g.src.size());
    };
    size_t a = srcOf(from, false), b = srcOf(to, true);
    if (b <= a) return SelectionText();
    while (a > 0 && g.src[a - 1] != L'\n' && a - 1 > 0 && b - a < 4000) a--;  // start at the line the selection began on
    while (b < g.src.size() && g.src[b] != L'\n') b++;
    std::wstring out;
    for (size_t i = a; i < b; i++) {  // the clipboard wants CRLF
        if (g.src[i] == L'\n' && (i == a || g.src[i - 1] != L'\r')) out.push_back(L'\r');
        if (g.src[i] != L'\r') out.push_back(g.src[i]);
        else out.push_back(L'\r');
    }
    return out;
}

// The three formats of a selection: plain text, CF_HTML (with its header of byte offsets) and RTF. The clipboard and
// a drag out of the window both hand over exactly these.
void SelectionRichFormats(std::wstring& text, std::string& cfHtml, std::string& rtf) {
    text.clear();
    cfHtml.clear();
    rtf.clear();
    if (!HasSelection()) return;
    text = SelectionText();
    if (text.empty()) return;
    std::string html;
    BuildRich(html, rtf);
    std::string head = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
                       "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
    std::string open = "<html><body>\r\n<!--StartFragment-->";
    std::string close = "<!--EndFragment-->\r\n</body></html>";
    size_t startHtml = head.size(), startFrag = startHtml + open.size();
    size_t endFrag = startFrag + html.size(), endHtml = endFrag + close.size();
    auto put = [&](const char* name, size_t value) {
        size_t at = head.find(name);
        if (at == std::string::npos) return;
        char buf[11];
        sprintf_s(buf, "%010zu", value);
        head.replace(at + strlen(name), 10, buf);
    };
    put("StartHTML:", startHtml);
    put("EndHTML:", endHtml);
    put("StartFragment:", startFrag);
    put("EndFragment:", endFrag);
    cfHtml = head + open + html + close;
}

// text + CF_HTML + RTF in one go
void CopySelectionRich() {
    std::wstring text;
    std::string cfHtml, rtf;
    SelectionRichFormats(text, cfHtml, rtf);
    if (text.empty()) return;
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    auto set = [](UINT fmt, const void* data, size_t bytes) {
        if (!fmt || !bytes) return;
        if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            memcpy(GlobalLock(h), data, bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(fmt, h)) GlobalFree(h);
        }
    };
    set(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    static UINT fmtHtml = RegisterClipboardFormatW(L"HTML Format");
    static UINT fmtRtf = RegisterClipboardFormatW(L"Rich Text Format");
    set(fmtHtml, cfHtml.c_str(), cfHtml.size() + 1);
    set(fmtRtf, rtf.c_str(), rtf.size() + 1);
    CloseClipboard();
}
