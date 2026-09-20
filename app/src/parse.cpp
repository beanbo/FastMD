// Markdown → flat block model. md4c is compiled with MD4C_USE_UTF16, so it parses the UTF-16 buffer directly and
// text callbacks point into it (no per-run UTF-8 → UTF-16 conversion).
// md4c is pinned at master 7fc1815a (2026-09-17), which parses GitHub alerts (admonitions) and footnotes itself.
// Extras on top of it: YAML front matter (shown as a yaml code block), GitHub-style heading slugs for #anchors and
// the outline, footnote numbering into links that jump both ways, and our own alert parsing as a fallback.
#include "doc.h"
#ifndef MD4C_USE_UTF16
#define MD4C_USE_UTF16
#endif
#include "../third_party/md4c/md4c.h"
#include "emoji_table.h"
#include <cwchar>
#include <unordered_map>

// ------------------------------------------------------------------------------------------------ helpers
std::wstring GithubSlug(const wchar_t* s, size_t n) {
    // github-slugger: lower-case, keep letters / digits / marks / '_' / '-', spaces → '-', drop everything else
    std::wstring low = ToLower(std::wstring(s, n)), out;
    out.reserve(low.size());
    std::vector<WORD> t1(low.size() + 1), t3(low.size() + 1);
    if (!low.empty()) {
        GetStringTypeW(CT_CTYPE1, low.data(), (int)low.size(), t1.data());
        GetStringTypeW(CT_CTYPE3, low.data(), (int)low.size(), t3.data());
    }
    for (size_t i = 0; i < low.size(); i++) {
        wchar_t c = low[i];
        if (c == L' ') out.push_back(L'-');
        else if (c == L'-' || c == L'_') out.push_back(c);
        else if (c >= 0xD800 && c <= 0xDFFF) continue;  // astral (emoji etc.)
        else if ((t1[i] & (C1_ALPHA | C1_DIGIT)) || (t3[i] & C3_NONSPACING)) out.push_back(c);
    }
    return out;
}

uint32_t AlertColor(uint8_t alert) { return alert ? g_pal[P_ALERT_NOTE + alert - 1] : g_pal[P_BORDER]; }

static void AppendEntity(std::wstring& t, const MD_CHAR* s, MD_SIZE n) {
    auto put = [&](uint32_t cp) {
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            t.push_back((wchar_t)(0xD800 + (cp >> 10)));
            t.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
        } else t.push_back((wchar_t)cp);
    };
    if (n >= 4 && s[1] == L'#') {
        uint32_t v = 0;
        bool hex = (s[2] == L'x' || s[2] == L'X');
        for (MD_SIZE i = hex ? 3 : 2; i + 1 < n; i++) {
            wchar_t c = s[i];
            v = hex ? v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10) : v * 10 + (c - '0');
            if (v > 0x10FFFF) { v = 0xFFFD; break; }
        }
        put(v);
        return;
    }
    static const struct { const wchar_t* name; uint16_t cp; } tab[] = {
        {L"amp", '&'}, {L"lt", '<'}, {L"gt", '>'}, {L"quot", '"'}, {L"apos", '\''}, {L"nbsp", 0xA0},
        {L"copy", 0xA9}, {L"reg", 0xAE}, {L"trade", 0x2122}, {L"mdash", 0x2014}, {L"ndash", 0x2013},
        {L"hellip", 0x2026}, {L"laquo", 0xAB}, {L"raquo", 0xBB}, {L"times", 0xD7}, {L"middot", 0xB7},
        {L"bull", 0x2022}, {L"rarr", 0x2192}, {L"larr", 0x2190}, {L"uarr", 0x2191}, {L"darr", 0x2193},
        {L"deg", 0xB0}, {L"plusmn", 0xB1}, {L"para", 0xB6}, {L"sect", 0xA7}, {L"euro", 0x20AC},
        {L"lsquo", 0x2018}, {L"rsquo", 0x2019}, {L"ldquo", 0x201C}, {L"rdquo", 0x201D}, {L"check", 0x2713},
        {L"shy", 0xAD}, {L"ensp", 0x2002}, {L"emsp", 0x2003}, {L"thinsp", 0x2009}, {L"zwj", 0x200D},
    };
    if (n > 2) {
        for (auto& e : tab) {
            size_t l = wcslen(e.name);
            if (l == n - 2 && wcsncmp(e.name, s + 1, l) == 0) { put(e.cp); return; }
        }
    }
    t.append(s, n);
}

// ------------------------------------------------------------------------------------------------ emoji
// GitHub shortcodes: :rocket: → 🚀. The names come from the gemoji database (app/tools/gen_emoji.py), sorted, so a
// candidate is looked up with a binary search. Unknown names stay as they were typed, and code is never touched.
static const wchar_t* EmojiFor(const wchar_t* name, size_t n, uint32_t* len) {
    size_t lo = 0, hi = std::size(kEmoji);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const EmojiEntry& e = kEmoji[mid];
        const char* s = kEmojiNames + e.name;
        size_t k = n < e.nameLen ? n : e.nameLen;
        int cmp = 0;
        for (size_t i = 0; i < k && cmp == 0; i++) cmp = (int)name[i] - (int)(unsigned char)s[i];
        if (cmp == 0 && n != e.nameLen) cmp = n < e.nameLen ? -1 : 1;
        if (cmp == 0) { *len = e.charsLen; return kEmojiChars + e.chars; }
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return nullptr;
}

static void AppendWithEmoji(std::wstring& t, const MD_CHAR* s, MD_SIZE n) {
    for (MD_SIZE i = 0; i < n;) {
        if (s[i] == L':') {
            MD_SIZE j = i + 1;
            while (j < n && j - i <= 40) {
                wchar_t c = s[j];
                if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'+' || c == L'-')) break;
                j++;
            }
            uint32_t len = 0;
            const wchar_t* e = (j < n && j > i + 1 && s[j] == L':') ? EmojiFor(s + i + 1, j - i - 1, &len) : nullptr;
            if (e) {
                t.append(e, len);
                i = j + 1;
                continue;
            }
        }
        t.push_back(s[i++]);
    }
}

// ------------------------------------------------------------------------------------------------ builder
namespace {
enum CtxType : uint8_t { C_QUOTE, C_UL, C_OL, C_LI };
struct Ctx {
    uint8_t type;
    bool tight;
    bool nested;       // list inside a list item
    uint32_t next;     // next ordered number
    uint32_t items;    // number of LIs seen
    int quote;         // index into Doc::quotes
};

struct AlertSpec { const wchar_t* tag; const wchar_t* title; wchar_t icon; };
const AlertSpec kAlerts[] = {
    {L"[!NOTE]", L"Note", 0xE946},       // Segoe Fluent Icons: Info
    {L"[!TIP]", L"Tip", 0xEA80},         // Lightbulb
    {L"[!IMPORTANT]", L"Important", 0xE171},
    {L"[!WARNING]", L"Warning", 0xE7BA},
    {L"[!CAUTION]", L"Caution", 0xEA39},
};

struct Builder {
    Doc& d;
    std::vector<Ctx> stack;
    float indent = 0;
    int quoteDepth = 0, listDepth = 0;
    float pending = 0;      // collapsed margin waiting for the next leaf
    bool atStart = true;
    std::unordered_map<std::wstring, int> slugCount;

    // current leaf being collected
    bool collecting = false;
    uint8_t leafKind = BK_TEXT, leafHeading = 0;
    bool leafIsLiImplicit = false;
    uint32_t tStart = 0, rStart = 0;
    const MD_CHAR* codeLang = nullptr;
    uint32_t codeLangLen = 0;
    int bold = 0, italic = 0, code = 0, strike = 0, link = 0, img = 0;
    uint32_t curLink = 0;
    // image-only paragraph detection
    int paraImages = 0;
    bool paraOther = false;
    int paraImage = -1;
    // pending list marker (attached to the next leaf)
    uint8_t pendMarker = MK_NONE, pendLevel = 0;
    uint32_t pendNumber = 0;
    std::vector<std::wstring> pendAnchors;  // #targets for the next emitted block (footnote jumps)
    uint32_t fnId = 0;                      // footnote definition being collected
    // tables
    int tIndex = -1;
    uint32_t row = 0, col = 0;
    bool inCell = false;

    explicit Builder(Doc& doc) : d(doc) {}

    void Margin(float m) { if (m > pending) pending = m; }

    void StartLeaf(uint8_t kind, uint8_t heading) {
        collecting = true;
        leafKind = kind;
        leafHeading = heading;
        tStart = (uint32_t)d.text.size();
        rStart = (uint32_t)d.runs.size();
        paraImages = 0;
        paraOther = false;
        paraImage = -1;
    }

    Block& Emit(uint8_t kind, float marginTop, float marginBottom) {
        Margin(marginTop);
        Block b{};
        b.kind = kind;
        b.gap = atStart ? 0.f : pending;
        atStart = false;
        pending = marginBottom;
        b.indent = indent;
        b.muted = quoteDepth > 0;
        b.marker = pendMarker;
        b.listLevel = pendLevel;
        b.number = pendNumber;
        b.textOff = (uint32_t)d.text.size();  // every block owns a [textOff, textOff+textLen) range, in order
        pendMarker = MK_NONE;
        d.blocks.push_back(b);
        for (std::wstring& a : pendAnchors) d.anchors.push_back(Anchor{std::move(a), (uint32_t)d.blocks.size() - 1});
        pendAnchors.clear();
        return d.blocks.back();
    }

    // the title line of a GitHub alert: icon + name, its own block in the alert's colour
    void EmitAlertTitle(uint8_t alert) {
        if (!alert || alert > std::size(kAlerts)) return;
        const AlertSpec& spec = kAlerts[alert - 1];
        uint32_t ts = (uint32_t)d.text.size(), rs = (uint32_t)d.runs.size();
        d.text.push_back(spec.icon);
        d.text += L"  ";
        d.text += spec.title;
        uint32_t T = (uint32_t)d.text.size() - ts;
        uint8_t pal = (uint8_t)(P_ALERT_NOTE + alert - 1);
        d.runs.push_back(Run{ts, 1, F_ICON, pal, 0, 0});
        d.runs.push_back(Run{ts + 1, T - 1, F_BOLD, pal, 0, 0});
        Block& tb = Emit(BK_TEXT, 0, 8);
        tb.alertTitle = alert;
        tb.textOff = ts;
        tb.textLen = T;
        tb.runOff = rs;
        tb.runCount = 2;
    }

    // The first paragraph of a blockquote starting with [!NOTE] etc. turns the quote into a GitHub alert: the tag is
    // replaced by a title line (icon + name) that becomes its own block, in document order.
    bool TryAlert(uint32_t& tEnd) {
        if (leafKind != BK_TEXT || leafHeading || stack.empty() || stack.back().type != C_QUOTE) return false;
        QuoteSpan& q = d.quotes[stack.back().quote];
        if (q.first != d.blocks.size() || q.alert) return false;  // not the quote's first leaf
        for (size_t a = 0; a < 5; a++) {
            size_t tl = wcslen(kAlerts[a].tag);
            if (tEnd - tStart < tl || _wcsnicmp(d.text.data() + tStart, kAlerts[a].tag, tl) != 0) continue;
            size_t k = tl;
            if (tStart + k < tEnd && !iswspace(d.text[tStart + k])) return false;  // "[!NOTE]x" is not an alert
            while (tStart + k < tEnd && iswspace(d.text[tStart + k])) k++;
            uint8_t alert = (uint8_t)(a + 1);
            q.alert = alert;
            std::wstring title;
            title.push_back(kAlerts[a].icon);
            title += L"  ";
            title += kAlerts[a].title;
            uint32_t T = (uint32_t)title.size();
            int32_t shift = (int32_t)T - (int32_t)k;
            d.text.replace(tStart, k, title);
            for (size_t r = rStart; r < d.runs.size(); r++) d.runs[r].start += shift;
            uint8_t pal = (uint8_t)(P_ALERT_NOTE + a);
            Run icon{tStart, 1, F_ICON, pal, 0, 0}, name{tStart + 1, T - 1, F_BOLD, pal, 0, 0};
            d.runs.insert(d.runs.begin() + rStart, {icon, name});
            Block& tb = Emit(BK_TEXT, 0, 8);
            tb.alertTitle = alert;
            tb.textOff = tStart;
            tb.textLen = T;
            tb.runOff = rStart;
            tb.runCount = 2;
            tEnd = (uint32_t)d.text.size();
            tStart += T;
            rStart += 2;
            return true;
        }
        return false;
    }

    void EndLeaf() {
        if (!collecting) return;
        collecting = false;
        uint32_t tEnd = (uint32_t)d.text.size();
        if (leafKind == BK_CODE) {
            while (tEnd > tStart && (d.text[tEnd - 1] == L'\n' || d.text[tEnd - 1] == L'\r')) tEnd--;
            d.text.resize(tEnd);
        }
        if (leafKind == BK_TEXT && !leafHeading && paraImages == 1 && !paraOther && paraImage >= 0) {
            // paragraph that is just an image → image block (alt text kept for the placeholder)
            d.runs.resize(rStart);
            Block& b = Emit(BK_IMAGE, 0, 16);
            b.aux = (uint32_t)paraImage;
            b.textOff = tStart;
            b.textLen = tEnd - tStart;
            return;
        }
        if (TryAlert(tEnd) && tEnd == tStart) {  // alert whose content starts in the next paragraph
            leafIsLiImplicit = false;
            return;
        }
        float mt = 0, mb = 16;
        if (leafHeading) { mt = 24; mb = 16; }
        else if (leafIsLiImplicit) { mt = 0; mb = 0; }
        else if (leafKind == BK_TEXT && !stack.empty() && stack.back().type == C_LI) { mt = 16; }  // li > p
        Block& b = Emit(leafKind, mt, mb);
        b.heading = leafHeading;
        if (leafHeading == 6) b.muted = 1;
        b.textOff = tStart;
        b.textLen = tEnd - tStart;
        b.runOff = rStart;
        b.runCount = (uint32_t)d.runs.size() - rStart;
        if (leafKind == BK_CODE) {
            uint8_t lang = 0;
            Highlight(d, codeLang, codeLangLen, tStart, tEnd - tStart, lang);
            b.runCount = (uint32_t)d.runs.size() - rStart;
            b.lang = lang;
        }
        if (leafHeading) {
            std::wstring slug = GithubSlug(d.text.data() + tStart, tEnd - tStart);
            int& cnt = slugCount[slug];
            if (cnt) slug += L"-" + std::to_wstring(cnt);
            cnt++;
            d.headings.push_back(Heading{(uint32_t)d.blocks.size() - 1, leafHeading, std::move(slug)});
        }
        leafIsLiImplicit = false;
    }

    // implicit text leaf: md4c sends the text of tight list items and of footnote definitions without a P block
    void EnsureLeaf() {
        if (collecting || inCell) return;
        if (fnId || (!stack.empty() && stack.back().type == C_LI)) {
            StartLeaf(BK_TEXT, 0);
            leafIsLiImplicit = true;
        }
    }

    void AppendText(const MD_CHAR* s, MD_SIZE n, bool isEntity) {
        EnsureLeaf();
        if (!collecting && !inCell) return;
        uint32_t start = (uint32_t)d.text.size();
        bool raw = code || (leafKind == BK_CODE && collecting);  // code keeps :colons: and every character as typed
        if (isEntity) AppendEntity(d.text, s, n);
        else if (raw) d.text.append(s, n);
        else AppendWithEmoji(d.text, s, n);
        uint32_t len = (uint32_t)d.text.size() - start;
        if (!len) return;
        if (img) return;  // alt text: not styled as a run, but kept (shown if not an image block)
        if (!paraOther) {
            for (uint32_t i = start; i < start + len; i++)
                if (d.text[i] != L' ' && d.text[i] != L'\n' && d.text[i] != L'\t') { paraOther = true; break; }
        }
        uint16_t fl = (bold ? F_BOLD : 0) | (italic ? F_ITALIC : 0) | (code ? F_CODE : 0) | (strike ? F_STRIKE : 0) |
                      (link ? F_LINK : 0);
        if (leafKind == BK_CODE && collecting) fl = 0;
        if (!fl) return;
        uint8_t color = (fl & F_LINK) ? P_LINK : (fl & F_STRIKE) ? P_MUTED : P_DEFAULT;
        uint32_t lnk = (fl & F_LINK) ? curLink : 0;
        if (!d.runs.empty() && d.runs.size() > rStart) {
            Run& r = d.runs.back();
            if (r.start + r.len == start && r.flags == fl && r.color == color && r.link == lnk) {
                r.len += len;
                return;
            }
        }
        d.runs.push_back(Run{start, len, fl, color, 0, lnk});
    }

    // --------------------------------------------------------------------------------------- blocks
    int Enter(MD_BLOCKTYPE t, void* det) {
        switch (t) {
        case MD_BLOCK_DOC: break;
        case MD_BLOCK_QUOTE:
        case MD_BLOCK_ADMONITION: {  // md4c reports > [!NOTE] … as an admonition; a plain quote has no alert
            EndLeaf();
            uint8_t alert = AL_NONE;
            if (t == MD_BLOCK_ADMONITION) {
                const MD_ATTRIBUTE& ty = ((MD_BLOCK_ADMONITION_DETAIL*)det)->type;
                for (size_t a = 0; a < std::size(kAlerts); a++)
                    if (ty.size == wcslen(kAlerts[a].title) && _wcsnicmp(ty.text, kAlerts[a].title, ty.size) == 0)
                        alert = (uint8_t)(a + 1);
            }
            QuoteSpan q{indent, (uint32_t)d.blocks.size(), UINT32_MAX, alert};
            d.quotes.push_back(q);
            stack.push_back(Ctx{C_QUOTE, false, false, 0, 0, (int)d.quotes.size() - 1});
            indent += 20.f;  // 4 px bar + 16 px padding (GitHub: padding 0 1em, border-left .25em)
            quoteDepth++;
            EmitAlertTitle(alert);
            break;
        }
        // footnotes: the definitions md4c collected at the end of the document, numbered and linked back
        case MD_BLOCK_FOOTNOTE_DEF_SECTION: EndLeaf(); Emit(BK_HR, 24, 16); break;
        case MD_BLOCK_FOOTNOTE_DEF: {
            EndLeaf();
            fnId = ((MD_BLOCK_FOOTNOTE_DEF_DETAIL*)det)->id;
            pendMarker = MK_NUMBER;
            pendNumber = fnId;
            pendLevel = 1;
            pendAnchors.push_back(L"fn-" + std::to_wstring(fnId));
            indent += 32.f;
            break;
        }
        case MD_BLOCK_UL:
        case MD_BLOCK_OL: {
            EndLeaf();
            bool nested = !stack.empty() && stack.back().type == C_LI;
            Ctx c{};
            c.type = t == MD_BLOCK_UL ? C_UL : C_OL;
            c.nested = nested;
            if (t == MD_BLOCK_UL) c.tight = ((MD_BLOCK_UL_DETAIL*)det)->is_tight != 0;
            else { auto* o = (MD_BLOCK_OL_DETAIL*)det; c.tight = o->is_tight != 0; c.next = o->start; }
            stack.push_back(c);
            indent += 32.f;  // padding-left: 2em
            listDepth++;
            break;
        }
        case MD_BLOCK_LI: {
            EndLeaf();
            auto* li = (MD_BLOCK_LI_DETAIL*)det;
            Ctx& list = stack.back();
            if (list.items++ > 0) Margin(4);  // li + li { margin-top: .25em }
            pendLevel = (uint8_t)listDepth;
            if (li->is_task) pendMarker = (li->task_mark == L' ') ? MK_TASK_OPEN : MK_TASK_DONE;
            else if (list.type == C_OL) { pendMarker = MK_NUMBER; pendNumber = list.next++; }
            else pendMarker = MK_BULLET;
            stack.push_back(Ctx{C_LI, list.tight, false, 0, 0, -1});
            break;
        }
        case MD_BLOCK_HR: EndLeaf(); Emit(BK_HR, 8, 24); break;
        case MD_BLOCK_H: EndLeaf(); StartLeaf(BK_TEXT, (uint8_t)((MD_BLOCK_H_DETAIL*)det)->level); break;
        case MD_BLOCK_CODE: {
            EndLeaf();
            auto* c = (MD_BLOCK_CODE_DETAIL*)det;
            codeLang = c->lang.text;
            codeLangLen = c->lang.size;
            StartLeaf(BK_CODE, 0);
            break;
        }
        case MD_BLOCK_HTML: EndLeaf(); StartLeaf(BK_TEXT, 0); code = 0; break;  // raw HTML: tags stripped, text kept
        case MD_BLOCK_P: EndLeaf(); StartLeaf(BK_TEXT, 0); break;
        case MD_BLOCK_TABLE: {
            EndLeaf();
            auto* td = (MD_BLOCK_TABLE_DETAIL*)det;
            Table tb{};
            tb.cols = td->col_count;
            tb.rows = td->head_row_count + td->body_row_count;
            tb.cellOff = (uint32_t)d.cells.size();
            tb.alignOff = (uint32_t)d.aligns.size();
            d.cells.resize(d.cells.size() + (size_t)tb.cols * tb.rows, Cell{0, 0, 0, 0});
            d.aligns.resize(d.aligns.size() + tb.cols, 0);
            d.tables.push_back(tb);
            tIndex = (int)d.tables.size() - 1;
            row = 0;
            col = 0;
            break;
        }
        case MD_BLOCK_THEAD: case MD_BLOCK_TBODY: break;
        case MD_BLOCK_TR: col = 0; break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            inCell = true;
            tStart = (uint32_t)d.text.size();
            rStart = (uint32_t)d.runs.size();
            if (row == 0 && tIndex >= 0) {
                Table& tb = d.tables[tIndex];
                if (col < tb.cols) d.aligns[tb.alignOff + col] = (uint8_t)((MD_BLOCK_TD_DETAIL*)det)->align;
            }
            break;
        }
        default: break;
        }
        return 0;
    }

    int Leave(MD_BLOCKTYPE t, void*) {
        switch (t) {
        case MD_BLOCK_QUOTE:
        case MD_BLOCK_ADMONITION: {
            EndLeaf();
            Ctx c = stack.back();
            stack.pop_back();
            QuoteSpan& q = d.quotes[c.quote];
            q.last = d.blocks.size() > q.first ? (uint32_t)d.blocks.size() - 1 : UINT32_MAX;
            if (q.alert && q.last != UINT32_MAX)  // alerts keep the normal text colour (plain quotes are muted)
                for (uint32_t k = q.first; k <= q.last; k++) d.blocks[k].muted = quoteDepth > 1;
            indent -= 20.f;
            quoteDepth--;
            Margin(16);
            break;
        }
        case MD_BLOCK_UL:
        case MD_BLOCK_OL: {
            EndLeaf();
            bool nested = stack.back().nested;
            stack.pop_back();
            indent -= 32.f;
            listDepth--;
            if (!nested) Margin(16);
            break;
        }
        case MD_BLOCK_LI: {
            EndLeaf();
            if (pendMarker != MK_NONE) {  // empty item: still show its marker
                StartLeaf(BK_TEXT, 0);
                leafIsLiImplicit = true;
                EndLeaf();
            }
            stack.pop_back();
            break;
        }
        case MD_BLOCK_H: case MD_BLOCK_CODE: case MD_BLOCK_P: EndLeaf(); break;
        case MD_BLOCK_FOOTNOTE_DEF: {
            EndLeaf();
            indent -= 32.f;
            // back to the place that referenced it: an arrow appended to the definition's last line
            if (fnId && !d.blocks.empty()) {
                Block& b = d.blocks.back();
                if (b.kind == BK_TEXT && b.textOff + b.textLen == d.text.size()) {
                    uint32_t start = (uint32_t)d.text.size();
                    d.text += L" \x21A9";  // ↩
                    d.links.push_back(L"#fnref-" + std::to_wstring(fnId));
                    d.runs.push_back(Run{start + 1, 1, F_LINK, P_LINK, 0, (uint32_t)d.links.size() - 1});
                    b.textLen += 2;
                    b.runCount = (uint32_t)d.runs.size() - b.runOff;
                }
            }
            fnId = 0;
            Margin(4);
            break;
        }
        case MD_BLOCK_HTML: {
            // drop empty HTML blocks (comments, lone tags)
            bool empty = true;
            for (size_t i = tStart; i < d.text.size(); i++)
                if (!iswspace(d.text[i])) { empty = false; break; }
            if (empty) { d.text.resize(tStart); d.runs.resize(rStart); collecting = false; }
            else EndLeaf();
            break;
        }
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            inCell = false;
            if (tIndex >= 0) {
                Table& tb = d.tables[tIndex];
                if (row < tb.rows && col < tb.cols) {
                    uint32_t tEnd = (uint32_t)d.text.size();
                    if (t == MD_BLOCK_TH) {  // header cells: bold over the whole cell
                        d.runs.resize(rStart);
                        if (tEnd > tStart) d.runs.push_back(Run{tStart, tEnd - tStart, F_BOLD, P_DEFAULT, 0, 0});
                    }
                    d.cells[tb.cellOff + row * tb.cols + col] =
                        Cell{tStart, tEnd - tStart, rStart, (uint32_t)d.runs.size() - rStart};
                }
            }
            col++;
            break;
        }
        case MD_BLOCK_TR: row++; break;
        case MD_BLOCK_TABLE: {
            const Table& tb = d.tables[tIndex];
            uint32_t first = (uint32_t)d.text.size(), last = first;
            if (tb.rows * tb.cols) {
                first = d.cells[tb.cellOff].textOff;
                const Cell& lc = d.cells[tb.cellOff + tb.rows * tb.cols - 1];
                last = std::max(first, lc.textOff + lc.textLen);
            }
            Block& b = Emit(BK_TABLE, 0, 16);
            b.aux = (uint32_t)tIndex;
            b.textOff = first;
            b.textLen = last - first;
            tIndex = -1;
            break;
        }
        default: break;
        }
        return 0;
    }

    int SpanEnter(MD_SPANTYPE t, void* det) {
        EnsureLeaf();
        switch (t) {
        case MD_SPAN_EM: italic++; break;
        case MD_SPAN_STRONG: bold++; break;
        case MD_SPAN_CODE: code++; break;
        case MD_SPAN_DEL: strike++; break;
        // formulas are shown as code until they are typeset for real (plan 4.1)
        case MD_SPAN_LATEXMATH: case MD_SPAN_LATEXMATH_DISPLAY: code++; break;
        case MD_SPAN_A: {
            auto* a = (MD_SPAN_A_DETAIL*)det;
            d.links.emplace_back(a->href.text, a->href.size);
            curLink = (uint32_t)d.links.size() - 1;
            link++;
            break;
        }
        case MD_SPAN_FOOTNOTE_REF: {  // self-contained: md4c sends no text for it, the number is ours to draw
            if (!collecting && !inCell) break;
            auto* f = (MD_SPAN_FOOTNOTE_REF_DETAIL*)det;
            std::wstring tag = L"[" + std::to_wstring(f->id) + L"]";
            uint32_t start = (uint32_t)d.text.size();
            d.text += tag;
            d.links.push_back(L"#fn-" + std::to_wstring(f->id));
            d.runs.push_back(Run{start, (uint32_t)tag.size(), F_LINK, P_LINK, 0, (uint32_t)d.links.size() - 1});
            if (f->ref_id == 1) pendAnchors.push_back(L"fnref-" + std::to_wstring(f->id));
            paraOther = true;
            break;
        }
        case MD_SPAN_IMG: {
            auto* im = (MD_SPAN_IMG_DETAIL*)det;
            Image image;
            std::wstring s(im->src.text, im->src.size);
            bool remote = s.find(L"://") != std::wstring::npos || s.rfind(L"data:", 0) == 0;
            if (!remote && !s.empty()) {
                std::wstring p;
                for (size_t i = 0; i < s.size(); i++) {  // %xx decoding + slashes
                    wchar_t c = s[i];
                    if (c == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
                        p.push_back((wchar_t)wcstol(s.substr(i + 1, 2).c_str(), nullptr, 16));
                        i += 2;
                    } else p.push_back(c == L'/' ? L'\\' : c);
                }
                image.path = (p.size() > 1 && p[1] == L':') ? p : d.baseDir + p;
            }
            d.images.push_back(image);
            paraImages++;
            paraImage = (int)d.images.size() - 1;
            img++;
            break;
        }
        default: break;
        }
        return 0;
    }

    int SpanLeave(MD_SPANTYPE t, void*) {
        switch (t) {
        case MD_SPAN_EM: italic--; break;
        case MD_SPAN_STRONG: bold--; break;
        case MD_SPAN_CODE: code--; break;
        case MD_SPAN_DEL: strike--; break;
        case MD_SPAN_LATEXMATH: case MD_SPAN_LATEXMATH_DISPLAY: code--; break;
        case MD_SPAN_A: link--; break;
        case MD_SPAN_IMG: img--; break;
        default: break;
        }
        return 0;
    }

    int OnText(MD_TEXTTYPE t, const MD_CHAR* s, MD_SIZE n) {
        switch (t) {
        case MD_TEXT_NORMAL: case MD_TEXT_CODE: case MD_TEXT_LATEXMATH: AppendText(s, n, false); break;
        case MD_TEXT_NULLCHAR: AppendText(L"\xFFFD", 1, false); break;
        case MD_TEXT_BR: AppendText(L"\n", 1, false); break;
        case MD_TEXT_SOFTBR: AppendText(L" ", 1, false); break;
        case MD_TEXT_ENTITY: AppendText(s, n, true); break;
        case MD_TEXT_HTML: {
            // inline / block HTML: strip tags, keep <br> as a line break
            if (n >= 3 && s[0] == L'<' && (s[1] | 32) == L'b' && (s[2] | 32) == L'r') { AppendText(L"\n", 1, false); break; }
            if (n && s[0] == L'<') break;
            AppendText(s, n, false);
            break;
        }
        default: break;
        }
        return 0;
    }

    // YAML front matter at the very top: "---" … "---" / "..." → yaml code block. Returns chars consumed.
    size_t FrontMatter(const wchar_t* s, size_t n) {
        auto lineEnd = [&](size_t i) { while (i < n && s[i] != L'\n') i++; return i; };
        auto isDelim = [&](size_t ls, size_t le, bool closing) {
            while (le > ls && (s[le - 1] == L'\r' || s[le - 1] == L' ' || s[le - 1] == L'\t')) le--;
            if (le - ls != 3) return false;
            return wcsncmp(s + ls, L"---", 3) == 0 || (closing && wcsncmp(s + ls, L"...", 3) == 0);
        };
        size_t first = lineEnd(0);
        if (first >= n || !isDelim(0, first, false)) return 0;
        size_t i = first + 1, bodyStart = i;
        for (int lines = 0; i < n && lines < 400; lines++) {
            size_t le = lineEnd(i);
            if (isDelim(i, le, true)) {
                if (i == bodyStart) return 0;  // "---\n---" is two thematic breaks, not front matter
                StartLeaf(BK_CODE, 0);
                for (size_t k = bodyStart; k < i; k++)
                    if (s[k] != L'\r') d.text.push_back(s[k]);
                static const wchar_t kYaml[] = L"yaml";
                codeLang = kYaml;
                codeLangLen = 4;
                EndLeaf();
                d.blocks.back().muted = 1;
                return le < n ? le + 1 : n;
            }
            i = le + 1;
        }
        return 0;
    }
};

int cbEnter(MD_BLOCKTYPE t, void* det, void* u) { return ((Builder*)u)->Enter(t, det); }
int cbLeave(MD_BLOCKTYPE t, void* det, void* u) { return ((Builder*)u)->Leave(t, det); }
int cbSpanEnter(MD_SPANTYPE t, void* det, void* u) { return ((Builder*)u)->SpanEnter(t, det); }
int cbSpanLeave(MD_SPANTYPE t, void* det, void* u) { return ((Builder*)u)->SpanLeave(t, det); }
int cbText(MD_TEXTTYPE t, const MD_CHAR* s, MD_SIZE n, void* u) { return ((Builder*)u)->OnText(t, s, n); }
}  // namespace

bool ParseMarkdown(Doc& d, const wchar_t* src, size_t n) {
    d.text.reserve(n);
    d.runs.reserve(n / 24 + 16);
    d.blocks.reserve(n / 60 + 16);
    Builder b(d);
    size_t skip = b.FrontMatter(src, n);
    MD_PARSER p{};
    p.abi_version = 0;
    p.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;  // $…$ and $$…$$ as on GitHub
    p.enter_block = cbEnter;
    p.leave_block = cbLeave;
    p.enter_span = cbSpanEnter;
    p.leave_span = cbSpanLeave;
    p.text = cbText;
    int rc = md_parse(src + skip, (MD_SIZE)(n - skip), &p, &b);
    b.EndLeaf();
    for (auto& q : d.quotes)  // empty quotes are marked with first = UINT32_MAX and skipped when drawing
        if (q.last == UINT32_MAX || q.last < q.first || q.first >= d.blocks.size()) q.first = q.last = UINT32_MAX;
    return rc == 0;
}

size_t FindPrefixCut(const wchar_t* s, size_t n, size_t minChars) {
    bool inFence = false, prevBlank = true;
    wchar_t fenceCh = 0;
    size_t fenceLen = 0, i = 0;
    while (i < n) {
        size_t ls = i;
        while (i < n && s[i] != L'\n') i++;
        size_t le = i;
        if (i < n) i++;
        size_t k = ls;
        int ind = 0;
        while (k < le && s[k] == L' ' && ind < 4) { k++; ind++; }
        bool blank = true;
        for (size_t j = ls; j < le; j++)
            if (s[j] != L' ' && s[j] != L'\t' && s[j] != L'\r') { blank = false; break; }
        if (ind < 4 && k + 2 < le && (s[k] == L'`' || s[k] == L'~') && s[k + 1] == s[k] && s[k + 2] == s[k]) {
            size_t cnt = 0;
            while (k + cnt < le && s[k + cnt] == s[k]) cnt++;
            if (!inFence) { inFence = true; fenceCh = s[k]; fenceLen = cnt; }
            else if (s[k] == fenceCh && cnt >= fenceLen) inFence = false;
        } else if (!inFence && ls >= minChars && prevBlank && ind == 0 && le > ls && s[ls] == L'#') {
            return ls;
        }
        prevBlank = blank;
    }
    return n;
}
