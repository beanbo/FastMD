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
    // --- added in 2.5: the rest of the languages READMEs use; the lexer above is generic, a language only needs its
    // comment and string syntax plus a keyword list
    {L"php", L"//", L"#", L"/*", L"*/", true, false, false, true, false,
     L" abstract and array as break callable case catch class clone const continue declare default do echo else elseif "
     L"empty enum extends final finally fn for foreach function global if implements include include_once instanceof "
     L"interface isset list match namespace new or print private protected public readonly require require_once return "
     L"static switch throw trait try unset use var while yield ",
     L" null true false $this "},
    {L"ruby|rb|gemfile|rake", L"#", nullptr, L"=begin", L"=end", true, false, false, true, false,
     L" alias and begin break case class def defined? do else elsif end ensure for if in module next not or raise "
     L"redo rescue retry return self super then undef unless until when while yield attr_accessor attr_reader require "
     L"require_relative include extend ",
     L" nil true false "},
    {L"perl|pl|pm", L"#", nullptr, nullptr, nullptr, true, false, false, true, false,
     L" my our local sub if elsif else unless while until for foreach do last next redo return use no package require "
     L"bless ref defined undef print printf say open close die warn eval ",
     L" undef "},
    {L"lua", L"--", nullptr, L"--[[", L"]]", false, false, false, true, false,
     L" and break do else elseif end false for function goto if in local nil not or repeat return then true until "
     L"while ",
     L" nil true false self "},
    {L"r|rlang", L"#", nullptr, nullptr, nullptr, false, false, false, true, false,
     L" if else repeat while function for in next break library require return invisible ",
     L" TRUE FALSE NULL NA Inf NaN "},
    {L"haskell|hs", L"--", nullptr, L"{-", L"-}", false, false, false, true, false,
     L" case class data default deriving do else foreign if import in infix infixl infixr instance let module newtype "
     L"of then type where ",
     L" True False Nothing Just Left Right "},
    {L"elixir|ex|exs", L"#", nullptr, nullptr, nullptr, false, true, false, true, false,
     L" after alias and case catch cond def defmacro defmodule defp defstruct do else end fn for if import in not or "
     L"quote raise receive require rescue try unless unquote use when with ",
     L" nil true false :ok :error "},
    {L"erlang|erl", L"%", nullptr, nullptr, nullptr, false, false, false, true, false,
     L" after begin case catch cond end fun if let of receive try when andalso orelse module export import spec ",
     L" true false undefined ok "},
    {L"clojure|clj|cljs|lisp|scheme|elisp|racket", L";", nullptr, nullptr, nullptr, false, false, false, true, false,
     L" def defn defmacro let fn if when cond do loop recur ns require import case try catch finally throw quote "
     L"lambda define set! begin ",
     L" nil true false "},
    {L"groovy|gradle", L"//", nullptr, L"/*", L"*/", true, false, false, true, false,
     L" as assert break case catch class const continue def default do else enum extends final finally for goto if "
     L"implements import in instanceof interface new package return static super switch this throw throws trait try "
     L"while ",
     L" null true false it "},
    {L"julia|jl", L"#", nullptr, L"#=", L"=#", true, false, false, true, false,
     L" abstract baremodule begin break catch const continue do else elseif end export finally for function global if "
     L"import let local macro module mutable primitive quote return struct try type using while ",
     L" true false nothing missing "},
    {L"nim", L"#", nullptr, L"#[", L"]#", false, true, false, true, false,
     L" addr and as asm bind block break case cast concept const continue converter defer discard distinct div do "
     L"elif else end enum except export finally for from func if import in include interface is iterator let macro "
     L"method mixin mod nil not notin object of or out proc ptr raise ref return shl shr static template try tuple "
     L"type using var when while xor yield ",
     L" true false nil result "},
    {L"zig", L"//", nullptr, nullptr, nullptr, false, false, false, false, false,
     L" align allowzero and anyframe anytype asm async await break catch comptime const continue defer else enum "
     L"errdefer error export extern fn for if inline noalias nosuspend or orelse packed pub resume return struct "
     L"suspend switch test threadlocal try union unreachable usingnamespace var volatile while ",
     L" true false null undefined "},
    {L"vim|viml|vimscript", L"\"", nullptr, nullptr, nullptr, true, false, false, true, false,
     L" function endfunction if elseif else endif for endfor while endwhile try catch finally endtry let set setlocal "
     L"call return command autocmd augroup source normal echo echom nnoremap inoremap vnoremap map ",
     L" v:true v:false v:null "},
    {L"dockerfile|docker|containerfile", L"#", nullptr, nullptr, nullptr, true, false, false, true, true,
     L" from run cmd label maintainer expose env add copy entrypoint volume user workdir arg onbuild stopsignal "
     L"healthcheck shell as ",
     L" "},
    {L"makefile|make|mk|automake", L"#", nullptr, nullptr, nullptr, true, false, false, true, false,
     L" ifeq ifneq ifdef ifndef else endif include define endef export unexport override .PHONY .DEFAULT_GOAL "
     L"SHELL CC CXX CFLAGS LDFLAGS ",
     L" "},
    {L"cmake", L"#", nullptr, nullptr, nullptr, true, false, false, true, true,
     L" add_executable add_library add_subdirectory cmake_minimum_required project set if else elseif endif foreach "
     L"endforeach function endfunction macro endmacro include option target_link_libraries target_include_directories "
     L"target_compile_options install find_package message ",
     L" on off true false "},
    {L"graphql|gql", L"#", nullptr, nullptr, nullptr, true, false, false, true, false,
     L" query mutation subscription fragment on type input enum interface union scalar schema extend implements "
     L"directive ",
     L" true false null "},
    {L"protobuf|proto", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" syntax package import option message enum service rpc returns repeated optional required oneof map reserved "
     L"extend stream ",
     L" true false "},
    {L"hcl|terraform|tf|tfvars", L"#", L"//", L"/*", L"*/", true, false, false, true, false,
     L" resource provider variable output module data locals terraform provisioner for_each count depends_on "
     L"dynamic if for in ",
     L" true false null "},
    {L"nix", L"#", nullptr, L"/*", L"*/", true, false, false, true, false,
     L" let in with rec inherit if then else assert import or builtins derivation mkDerivation ",
     L" true false null "},
    {L"solidity|sol", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" pragma solidity contract interface library function modifier event struct enum mapping address uint uint256 "
     L"int bool string bytes memory storage calldata public private internal external view pure payable returns "
     L"return require revert assert emit constructor if else for while new delete import using is override virtual ",
     L" true false msg block now this "},
    {L"objectivec|objc|objective-c", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" @interface @implementation @end @property @synthesize @protocol @class @selector @autoreleasepool if else for "
     L"while do switch case break continue return void id self super const static extern typedef struct enum union "
     L"import include ",
     L" nil YES NO NULL "},
    {L"fortran|f90|f95|for", L"!", nullptr, nullptr, nullptr, false, false, false, true, true,
     L" program module subroutine function end do while if then else elseif endif implicit none integer real "
     L"double precision complex logical character parameter dimension allocate deallocate use contains call return "
     L"print write read ",
     L" .true. .false. "},
    {L"pascal|delphi|pas", L"//", nullptr, L"{", L"}", false, false, false, true, true,
     L" and array begin case const div do downto else end file for function goto if implementation in interface "
     L"label mod nil not of or packed procedure program record repeat set then to type unit until uses var while "
     L"with ",
     L" nil true false "},
    {L"ocaml|ml|fsharp|fs|fsx", L"//", nullptr, L"(*", L"*)", false, false, false, true, false,
     L" let rec in match with function fun if then else type module open begin end try raise exception of and as "
     L"when while do done for to downto mutable member override abstract interface inherit namespace ",
     L" true false None Some unit "},
    {L"vb|vbnet|vba|visualbasic", L"'", nullptr, nullptr, nullptr, false, false, false, false, true,
     L" dim as new if then else elseif end select case for each next while do loop function sub return class module "
     L"public private protected friend shared static imports namespace try catch finally throw with using property "
     L"get set ",
     L" nothing true false me "},
    {L"asm|nasm|x86asm|masm|gas", L";", nullptr, nullptr, nullptr, false, false, false, true, true,
     L" mov lea push pop call ret jmp je jne jz jnz jg jl add sub mul imul div idiv inc dec cmp test and or xor not "
     L"shl shr section global extern db dw dd dq resb equ proc endp ",
     L" eax ebx ecx edx rax rbx rcx rdx rsi rdi rsp rbp "},
    {L"batch|bat|cmd|dosbatch", L"::", L"rem", nullptr, nullptr, true, false, false, true, true,
     L" echo set if else for goto call exit rem setlocal endlocal shift pause start cd md rd del copy move ren type "
     L"findstr not exist defined errorlevel ",
     L" "},
    {L"awk|gawk", L"#", nullptr, nullptr, nullptr, true, false, false, true, false,
     L" BEGIN END if else while for do break continue next exit function print printf getline delete return ",
     L" NR NF FS OFS RS ORS "},
    {L"tex|latex", L"%", nullptr, nullptr, nullptr, false, false, false, true, false,
     L" \\begin \\end \\documentclass \\usepackage \\section \\subsection \\subsubsection \\chapter \\item \\label "
     L"\\ref \\cite \\textbf \\textit \\emph \\frac \\sum \\int \\newcommand \\includegraphics ",
     L" "},
    {L"matlab|octave", L"%", nullptr, L"%{", L"%}", false, false, false, true, false,
     L" function end if elseif else while for switch case otherwise break continue return try catch global "
     L"persistent classdef properties methods ",
     L" true false pi inf nan "},
    {L"prolog", L"%", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" is not module use_module dynamic discontiguous findall bagof setof assert asserta assertz retract ",
     L" true false fail "},
    {L"crystal|cr", L"#", nullptr, nullptr, nullptr, true, false, false, true, false,
     L" abstract alias as begin break case class def do else elsif end ensure enum extend for fun if in include "
     L"instance_sizeof is_a? lib macro module next of out pointerof private protected require rescue return select "
     L"self sizeof struct super then type typeof union unless until when while with yield ",
     L" nil true false "},
    {L"d|dlang", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" alias auto bool break byte case cast catch char class const continue default delegate do double else enum "
     L"export extern final finally float for foreach function goto if immutable import in int interface is long "
     L"module new nothrow out override package pragma private protected public pure ref return scope shared short "
     L"static struct switch template this throw try typeof ubyte uint ulong union ushort version void while with ",
     L" null true false "},
    {L"glsl|hlsl|wgsl|shader|metal", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" attribute uniform varying in out inout layout binding group vertex fragment compute void bool int uint float "
     L"double vec2 vec3 vec4 mat2 mat3 mat4 sampler2D texture return if else for while discard struct const ",
     L" true false gl_Position gl_FragColor "},
    {L"nginx|apache|conf|properties|env|dotenv", L"#", L";", nullptr, nullptr, true, false, false, true, true,
     L" server location listen root index proxy_pass upstream include ssl_certificate rewrite return if set "
     L"RewriteEngine RewriteRule Directory Location Options AllowOverride Require ",
     L" on off true false "},
    {L"v|vlang", L"//", nullptr, L"/*", L"*/", false, false, false, true, false,
     L" module import fn struct enum interface const mut pub if else for match return go defer or as in is type "
     L"union none assert unsafe ",
     L" true false none "},
    {L"ada", L"--", nullptr, nullptr, nullptr, false, false, false, true, true,
     L" abort abs abstract accept access aliased all and array at begin body case constant declare delay delta "
     L"digits do else elsif end entry exception exit for function generic goto if in interface is limited loop mod "
     L"new not null of or others out overriding package pragma private procedure protected raise range record rem "
     L"renames requeue return reverse select separate some subtype synchronized tagged task terminate then type "
     L"until use when while with xor ",
     L" true false null "},
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
