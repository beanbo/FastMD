// Tag splitting and entity decoding for the raw HTML inside Markdown. No DOM is built here: the document builder
// (parse.cpp) walks the tags as they come and keeps its own small stack of what is open.
#include "html.h"
#include <cwchar>

namespace {
void PutCodepoint(std::wstring& t, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        t.push_back((wchar_t)(0xD800 + (cp >> 10)));
        t.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
    } else {
        t.push_back((wchar_t)cp);
    }
}
}  // namespace

// one entity, including the & and the ; — the named set is the one READMEs actually use
void AppendEntity(std::wstring& t, const wchar_t* s, size_t n) {
    if (n >= 4 && s[1] == L'#') {
        uint32_t v = 0;
        bool hex = (s[2] == L'x' || s[2] == L'X');
        for (size_t i = hex ? 3 : 2; i + 1 < n; i++) {
            wchar_t c = s[i];
            uint32_t d = (c >= L'0' && c <= L'9')                  ? (uint32_t)(c - L'0')
                         : hex && ((c | 32) >= L'a' && (c | 32) <= L'f') ? (uint32_t)((c | 32) - L'a' + 10)
                                                                  : 0xFFFFFFFF;
            if (d == 0xFFFFFFFF) { v = 0xFFFD; break; }
            v = hex ? v * 16 + d : v * 10 + d;
            if (v > 0x10FFFF) { v = 0xFFFD; break; }
        }
        PutCodepoint(t, v);
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
            if (l == n - 2 && wcsncmp(e.name, s + 1, l) == 0) { PutCodepoint(t, e.cp); return; }
        }
    }
    t.append(s, n);
}

void AppendHtmlText(std::wstring& out, const wchar_t* s, size_t n) {
    for (size_t i = 0; i < n;) {
        if (s[i] == L'&') {
            size_t j = i + 1;
            while (j < n && j - i < 12 && s[j] != L';' && !iswspace(s[j])) j++;
            if (j < n && s[j] == L';') {
                AppendEntity(out, s + i, j - i + 1);
                i = j + 1;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
}

size_t ParseHtmlTag(const wchar_t* s, size_t n, HtmlTag& out) {
    out = HtmlTag{};
    if (n < 2 || s[0] != L'<') return 0;
    if (n >= 4 && s[1] == L'!' && s[2] == L'-' && s[3] == L'-') {  // comment: skipped whole
        for (size_t i = 4; i + 2 < n; i++)
            if (s[i] == L'-' && s[i + 1] == L'-' && s[i + 2] == L'>') return i + 3;
        return n;
    }
    size_t i = 1;
    if (s[i] == L'/') { out.closing = true; i++; }
    if (i >= n || !(iswalpha(s[i]) || s[i] == L'!')) return 0;
    size_t nameStart = i;
    while (i < n && (iswalnum(s[i]) || s[i] == L'-' || s[i] == L'!')) i++;
    out.name.assign(s + nameStart, i - nameStart);
    for (wchar_t& c : out.name) c = (wchar_t)towlower(c);
    while (i < n && s[i] != L'>') {
        while (i < n && iswspace(s[i])) i++;
        if (i < n && s[i] == L'/') { out.selfClose = true; i++; continue; }
        if (i >= n || s[i] == L'>') break;
        size_t as = i;
        while (i < n && !iswspace(s[i]) && s[i] != L'=' && s[i] != L'>' && s[i] != L'/') i++;
        if (i == as) { i++; continue; }  // stray character: skip it
        HtmlAttr a;
        a.name.assign(s + as, i - as);
        for (wchar_t& c : a.name) c = (wchar_t)towlower(c);
        while (i < n && iswspace(s[i])) i++;
        if (i < n && s[i] == L'=') {
            i++;
            while (i < n && iswspace(s[i])) i++;
            if (i < n && (s[i] == L'"' || s[i] == L'\'')) {
                wchar_t q = s[i++];
                size_t vs = i;
                while (i < n && s[i] != q) i++;
                AppendHtmlText(a.value, s + vs, i - vs);
                if (i < n) i++;
            } else {
                size_t vs = i;
                while (i < n && !iswspace(s[i]) && s[i] != L'>' && s[i] != L'/') i++;
                AppendHtmlText(a.value, s + vs, i - vs);
            }
        }
        out.attrs.push_back(std::move(a));
    }
    if (i < n && s[i] == L'>') i++;
    return i;
}
