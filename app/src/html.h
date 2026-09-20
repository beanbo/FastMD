// Tiny HTML reader for the raw HTML md4c hands over: a tag splitter and an entity-aware text reader. It understands
// only what READMEs actually use (see docs/PLAN.md 2.2); scripts, styles and iframes are never executed, and every
// tag we do not know is dropped while its text is kept.
#pragma once
#include "common.h"

struct HtmlAttr { std::wstring name, value; };

struct HtmlTag {
    std::wstring name;      // lower case, "" when the text was not a tag after all
    bool closing = false;   // </div>
    bool selfClose = false; // <br/>
    std::vector<HtmlAttr> attrs;
    const std::wstring* Attr(const wchar_t* n) const {
        for (const HtmlAttr& a : attrs)
            if (a.name == n) return &a.value;
        return nullptr;
    }
    int AttrInt(const wchar_t* n, int fallback = -1) const {
        const std::wstring* v = Attr(n);
        if (!v || v->empty()) return fallback;
        int x = _wtoi(v->c_str());
        return x > 0 ? x : fallback;
    }
};

// Reads one tag starting at s[0] == '<'. Returns the number of characters consumed (0 = not a tag).
size_t ParseHtmlTag(const wchar_t* s, size_t n, HtmlTag& out);

// HTML entities and numeric references → text (&amp; &#169; &#x2014; …).
void AppendHtmlText(std::wstring& out, const wchar_t* s, size_t n);
void AppendEntity(std::wstring& out, const wchar_t* s, size_t n);  // one entity, including '&' and ';'
