// Tiny single-pass syntax highlighter for fenced code blocks (GitHub light colours).
// Not a real grammar: comments, strings, numbers, keywords, types (Capitalized), calls ident(, $vars.
#include "doc.h"
#include <cwchar>
#include <cwctype>

namespace {
enum Lang : uint8_t { L_NONE, L_C, L_CPP, L_CS, L_JS, L_RUST, L_PY, L_PS, L_JSON, L_GO, L_JAVA, L_SH, L_SQL, L_CSS, L_XML, L_YAML, L_TOML };

struct LangSpec {
    const wchar_t* names;   // '|' separated aliases
    const wchar_t* line1;   // line comment
    const wchar_t* line2;
    const wchar_t* blockOpen;
    const wchar_t* blockClose;
    bool dollarVars, triple, backtick, singleQuoteStr, caseInsensitive;
    const wchar_t* keywords;  // space separated, sorted not required
    const wchar_t* consts;
};

const LangSpec kLangs[] = {
    {L"", nullptr, nullptr, nullptr, nullptr, false, false, false, false, false, L"", L""},
    {L"c|h", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" auto break case char const continue default do double else enum extern float for goto if inline int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while _Bool #include #define #if #ifdef #ifndef #endif #else #elif #pragma ",
     L" NULL true false stdin stdout stderr EOF "},
    {L"cpp|c++|cc|cxx|hpp|hh", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" alignas alignof auto bool break case catch char class concept const constexpr consteval const_cast continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern float for friend goto if inline int long mutable namespace new noexcept operator private protected public register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch template this thread_local throw try typedef typeid typename union unsigned using virtual void volatile while #include #define #if #ifdef #ifndef #endif #else #pragma ",
     L" nullptr true false NULL "},
    {L"csharp|cs|c#", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" abstract as async await base bool break byte case catch char checked class const continue decimal default delegate do double else enum event explicit extern finally fixed float for foreach get goto if implicit in init int interface internal is lock long namespace new object operator out override params private protected public readonly record ref return sbyte sealed set short sizeof stackalloc static string struct switch this throw try typeof uint ulong unchecked unsafe ushort using var virtual void volatile when where while yield ",
     L" null true false value "},
    {L"js|javascript|jsx|mjs|ts|typescript|tsx", L"//", nullptr, L"/*", L"*/", false, false, true, true, false,
     L" async await break case catch class const continue debugger default delete do else export extends finally for from function if import in instanceof let new of return static super switch this throw try typeof var void while with yield interface type enum implements private public protected readonly as ",
     L" null undefined true false NaN Infinity "},
    {L"rust|rs", L"//", nullptr, L"/*", L"*/", false, false, false, false, false,
     L" as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait type unsafe use where while ",
     L" true false None Some Ok Err "},
    {L"python|py|python3", L"#", nullptr, nullptr, nullptr, false, true, false, true, false,
     L" and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield ",
     L" None True False self "},
    {L"powershell|ps1|pwsh|ps", L"#", nullptr, L"<#", L"#>", true, false, false, true, true,
     L" begin break catch class continue data do dynamicparam else elseif end exit filter finally for foreach function if in param process return switch throw trap try until using while ",
     L" $true $false $null "},
    {L"json|jsonc|json5", L"//", nullptr, L"/*", L"*/", false, false, false, false, false, L"", L" true false null "},
    {L"go|golang", L"//", nullptr, L"/*", L"*/", false, false, true, true, false,
     L" break case chan const continue default defer else fallthrough for func go goto if import interface map package range return select struct switch type var ",
     L" nil true false iota "},
    {L"java|kotlin|kt|scala|swift|dart", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" abstract boolean break byte case catch char class const continue default do double else enum extends final finally float for fun func if implements import instanceof int interface let long native new override package private protected public return short static super switch synchronized this throw throws transient try val var void volatile when while ",
     L" null true false nil "},
    {L"bash|sh|shell|zsh|console", L"#", nullptr, nullptr, nullptr, true, false, false, true, false,
     L" if then else elif fi case esac for while until do done in function return export local readonly echo cd exit ",
     L" true false "},
    {L"sql", L"--", nullptr, L"/*", L"*/", false, false, false, true, true,
     L" select from where and or not insert into values update set delete create table index view drop alter add join left right inner outer on group by order having limit offset as distinct union all case when then else end primary key foreign references null is in like between exists ",
     L" true false "},
    {L"css|scss|less", nullptr, nullptr, L"/*", L"*/", false, false, false, true, false, L" @media @import @font-face !important ", L""},
    {L"html|xml|svg|xaml|htm", nullptr, nullptr, L"<!--", L"-->", false, false, false, true, false, L"", L""},
    {L"yaml|yml", L"#", nullptr, nullptr, nullptr, false, false, false, true, false, L"", L" true false null yes no "},
    {L"toml|ini|cfg", L"#", L";", nullptr, nullptr, false, false, false, true, false, L"", L" true false "},
};

bool NameMatches(const wchar_t* names, const wchar_t* lang, uint32_t n) {
    const wchar_t* p = names;
    while (*p) {
        const wchar_t* e = wcschr(p, L'|');
        size_t l = e ? (size_t)(e - p) : wcslen(p);
        if (l == n && _wcsnicmp(p, lang, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

bool InList(const wchar_t* list, const wchar_t* w, size_t n, bool ci) {
    if (!list || !*list || n == 0 || n > 40) return false;
    wchar_t buf[48];
    buf[0] = L' ';
    for (size_t i = 0; i < n; i++) buf[i + 1] = ci ? (wchar_t)towlower(w[i]) : w[i];
    buf[n + 1] = L' ';
    buf[n + 2] = 0;
    return wcsstr(list, buf) != nullptr;
}

inline bool IsIdStart(wchar_t c) { return iswalpha(c) || c == L'_' || c >= 0x80; }
inline bool IsId(wchar_t c) { return iswalnum(c) || c == L'_' || c >= 0x80; }
inline bool StartsWith(const wchar_t* s, size_t n, size_t i, const wchar_t* pat) {
    if (!pat) return false;
    size_t l = wcslen(pat);
    return i + l <= n && wcsncmp(s + i, pat, l) == 0;
}
}  // namespace

void Highlight(Doc& d, const wchar_t* langName, uint32_t langLen, uint32_t off, uint32_t len, uint8_t& langId) {
    langId = 0;
    if (!langName || !langLen) return;
    int li = 0;
    for (int i = 1; i < (int)(sizeof(kLangs) / sizeof(kLangs[0])); i++)
        if (NameMatches(kLangs[i].names, langName, langLen)) { li = i; break; }
    if (!li) return;
    langId = (uint8_t)li;
    const LangSpec& L = kLangs[li];
    const wchar_t* s = d.text.data() + off;
    size_t n = len;
    auto emit = [&](size_t a, size_t b, uint8_t col) {
        if (b <= a) return;
        if (!d.runs.empty()) {
            Run& r = d.runs.back();
            if (r.color == col && r.start + r.len == off + a && r.flags == 0 && r.start >= off) { r.len += (uint32_t)(b - a); return; }
        }
        d.runs.push_back(Run{off + (uint32_t)a, (uint32_t)(b - a), 0, col, 0, 0});
    };
    bool isJson = li == L_JSON, isXml = li == L_XML, isCss = li == L_CSS, isYaml = li == L_YAML;
    size_t i = 0;
    bool lineStart = true;
    while (i < n) {
        wchar_t c = s[i];
        if (c == L'\n') { i++; lineStart = true; continue; }
        // comments
        if (StartsWith(s, n, i, L.blockOpen)) {
            size_t end = i + wcslen(L.blockOpen);
            while (end < n && !StartsWith(s, n, end, L.blockClose)) end++;
            end = end < n ? end + wcslen(L.blockClose) : n;
            emit(i, end, P_COMMENT);
            i = end;
            continue;
        }
        if (StartsWith(s, n, i, L.line1) || StartsWith(s, n, i, L.line2)) {
            if (!(li == L_C || li == L_CPP) || c != L'#') {
                size_t end = i;
                while (end < n && s[end] != L'\n') end++;
                emit(i, end, P_COMMENT);
                i = end;
                continue;
            }
        }
        // strings
        if (c == L'"' || (c == L'\'' && L.singleQuoteStr) || (c == L'`' && L.backtick) ||
            (li == L_RUST && c == L'\'' && i + 2 < n && (s[i + 2] == L'\'' || (s[i + 1] == L'\\' && i + 3 < n)))) {
            size_t end = i + 1;
            bool triple = L.triple && i + 2 < n && s[i + 1] == c && s[i + 2] == c;
            if (triple) {
                end = i + 3;
                while (end + 2 < n && !(s[end] == c && s[end + 1] == c && s[end + 2] == c)) end++;
                end = end + 2 < n ? end + 3 : n;
            } else {
                while (end < n && s[end] != c && (s[end] != L'\n' || c == L'`')) {
                    if (s[end] == L'\\' && end + 1 < n) end++;
                    end++;
                }
                if (end < n && s[end] == c) end++;
            }
            uint8_t col = P_STRING;
            if (isJson) {  // keys: string followed by ':'
                size_t k = end;
                while (k < n && (s[k] == L' ' || s[k] == L'\t')) k++;
                if (k < n && s[k] == L':') col = P_CONST;
            }
            emit(i, end, col);
            i = end;
            lineStart = false;
            continue;
        }
        // xml/html tags
        if (isXml && c == L'<') {
            size_t end = i + 1;
            if (end < n && (s[end] == L'/' || s[end] == L'?' || s[end] == L'!')) end++;
            size_t nameStart = end;
            while (end < n && (IsId(s[end]) || s[end] == L':' || s[end] == L'-')) end++;
            emit(i, nameStart, P_TEXT);
            emit(nameStart, end, P_CONST);
            i = end;
            continue;
        }
        // numbers
        if (iswdigit(c) && (i == 0 || !IsId(s[i - 1]))) {
            size_t end = i + 1;
            while (end < n && (iswalnum(s[end]) || s[end] == L'.' || s[end] == L'_')) {
                if (s[end] == L'.' && (end + 1 >= n || !iswdigit(s[end + 1]))) break;
                end++;
            }
            emit(i, end, P_CONST);
            i = end;
            lineStart = false;
            continue;
        }
        // $variables
        if (L.dollarVars && c == L'$' && i + 1 < n && (IsIdStart(s[i + 1]) || s[i + 1] == L'{' || s[i + 1] == L'_')) {
            size_t end = i + 1;
            while (end < n && (IsId(s[end]) || s[end] == L':' || s[end] == L'{' || s[end] == L'}')) end++;
            bool isConst = InList(L.consts, s + i, end - i, true);
            emit(i, end, isConst ? P_CONST : P_VAR);
            i = end;
            continue;
        }
        // PowerShell -Parameters
        if (li == L_PS && c == L'-' && i + 1 < n && iswalpha(s[i + 1]) && (i == 0 || s[i - 1] == L' ')) {
            size_t end = i + 1;
            while (end < n && IsId(s[end])) end++;
            emit(i, end, P_CONST);
            i = end;
            continue;
        }
        // identifiers / keywords / preprocessor
        if (IsIdStart(c) || ((li == L_C || li == L_CPP) && c == L'#' ) || (isCss && (c == L'@' || c == L'.'))) {
            size_t end = i + 1;
            while (end < n && (IsId(s[end]) || (li == L_PS && s[end] == L'-' && end + 1 < n && iswalpha(s[end + 1])))) end++;
            size_t wl = end - i;
            uint8_t col = 0;
            if (InList(L.keywords, s + i, wl, L.caseInsensitive)) col = P_KEYWORD;
            else if (InList(L.consts, s + i, wl, L.caseInsensitive)) col = P_CONST;
            else if (isYaml && lineStart) {
                size_t k = end;
                while (k < n && s[k] == L' ') k++;
                if (k < n && s[k] == L':') col = P_TYPE;
            } else {
                size_t k = end;
                if (li == L_RUST && k < n && s[k] == L'!') col = P_FUNC;  // macros
                while (k < n && s[k] == L' ' && !(li == L_PS)) k++;
                if (!col && k < n && s[k] == L'(') col = P_FUNC;
                else if (!col && li == L_PS && wcschr(std::wstring(s + i, wl).c_str(), L'-')) col = P_FUNC;  // Verb-Noun
                else if (!col && iswupper(c) && li != L_JSON && li != L_SQL && wl > 1) col = P_TYPE;
            }
            if (col) emit(i, end, col);
            i = end;
            lineStart = false;
            continue;
        }
        if (c != L' ' && c != L'\t') lineStart = false;
        i++;
    }
}
