#pragma once
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace vm {
enum class CodeColor : uint8_t { Keyword, String, Comment, Number, Variable };
struct CodeToken {
    uint32_t start, length;
    CodeColor color;
};
// Deliberately a small lexical highlighter: no grammar engine or startup work.
// Input is one bounded renderer part; unknown language labels remain plain text.
inline std::vector<CodeToken> codeTokens(std::wstring_view text, std::wstring_view language) {
    std::wstring lang(language);
    for (auto &c : lang)
        c = (wchar_t)towlower(c);
    bool ps = lang == L"powershell" || lang == L"pwsh" || lang == L"ps1";
    bool py = lang == L"python" || lang == L"py";
    bool sh = lang == L"bash" || lang == L"sh" || lang == L"shell";
    bool sql = lang == L"sql";
    bool json = lang == L"json" || lang == L"jsonc";
    bool cs = lang == L"cs" || lang == L"csharp" || lang == L"c#";
    bool js = lang == L"javascript" || lang == L"js" || lang == L"jsx" || lang == L"typescript" ||
              lang == L"ts" || lang == L"tsx";
    bool cpp = lang == L"c" || lang == L"cpp" || lang == L"c++" || lang == L"h";
    if (!(ps || py || sh || sql || json || cs || js || cpp))
        return {};
    constexpr std::wstring_view common = L" true false null ";
    constexpr std::wstring_view cwords =
        L" if else for while do return break continue switch case default try catch finally throw class enum "
        L"public private protected static new void const this namespace using async await interface abstract "
        L"override readonly virtual sealed struct bool int string double float decimal var object get set "
        L"null true false typeof sizeof out ref in is as params yield record required init global base "
        L"operator implicit explicit event delegate lock fixed unsafe volatile extern partial internal "
        L"stackalloc checked unchecked char long short byte uint ulong ushort sbyte dynamic ";
    constexpr std::wstring_view jswords =
        L" function let const var export import from default extends implements type keyof infer declare "
        L"module constructor readonly async await undefined of in instanceof delete typeof void new this "
        L"super debugger with satisfies unknown never any number string boolean interface enum class if else "
        L"for while do return break continue switch case try catch finally throw yield null true false ";
    constexpr std::wstring_view pswords =
        L" function param begin process end if else elseif foreach for while do until switch return break "
        L"continue try catch finally throw trap filter class enum in exit true false null ";
    constexpr std::wstring_view pywords =
        L" def class if elif else for while return break continue try except finally raise import from as "
        L"with pass lambda yield async await and or not in is None True False global nonlocal assert del ";
    constexpr std::wstring_view sqlwords =
        L" select from where insert into update delete create alter drop table values set join left right "
        L"inner outer on as and or not null is in order by group having limit distinct union all case when "
        L"then else end asc desc primary key references default true false ";
    constexpr std::wstring_view shwords =
        L" if then else elif fi for do done while until case esac function in export local return exit ";
    auto words = ps     ? pswords
                 : py   ? pywords
                 : sh   ? shwords
                 : sql  ? sqlwords
                 : json ? common
                 : js   ? jswords
                        : cwords;
    std::vector<CodeToken> result;
    auto word = [](wchar_t c) { return iswalnum(c) || c == L'_' || c == L'$'; };
    for (size_t i = 0; i < text.size();) {
        size_t start = i;
        CodeColor color;
        wchar_t c = text[i];
        auto next = [&](wchar_t n) { return i + 1 < text.size() && text[i + 1] == n; };
        bool blockComment =
            (!py && !sh && !ps && !sql && c == L'/' && next(L'*')) || (ps && c == L'<' && next(L'#'));
        if (blockComment) {
            auto end = text.find(ps ? L"#>" : L"*/", i + 2);
            i = end == text.npos ? text.size() : end + 2;
            color = CodeColor::Comment;
        } else if (((ps || py || sh) && c == L'#') || (!(ps || py || sh || sql) && c == L'/' && next(L'/')) ||
                   (sql && c == L'-' && next(L'-'))) {
            auto end = text.find(L'\n', i);
            i = end == text.npos ? text.size() : end;
            color = CodeColor::Comment;
        } else if (c == L'\'' || c == L'"' || (js && c == L'`')) {
            bool triple = py && i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c;
            i += triple ? 3 : 1;
            while (i < text.size()) {
                if (text[i] == (ps ? L'`' : L'\\') && i + 1 < text.size()) {
                    i += 2;
                    continue;
                }
                if (text[i] == c &&
                    (!triple || (i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c))) {
                    i += triple ? 3 : 1;
                    break;
                }
                ++i;
            }
            color = CodeColor::String;
        } else if (iswdigit(c) && (i == 0 || !word(text[i - 1]))) {
            ++i;
            while (i < text.size() && (iswalnum(text[i]) || text[i] == L'.' || text[i] == L'_'))
                ++i;
            color = CodeColor::Number;
        } else if (word(c)) {
            ++i;
            while (i < text.size() && (word(text[i]) || (ps && text[i] == L'-')))
                ++i;
            std::wstring token(text.substr(start, i - start));
            if (ps || sql)
                for (auto &ch : token)
                    ch = (wchar_t)towlower(ch);
            if ((ps || sh) && c == L'$')
                color = CodeColor::Variable;
            else if (words.find(L" " + token + L" ") != words.npos)
                color = CodeColor::Keyword;
            else
                continue;
        } else {
            ++i;
            continue;
        }
        result.push_back({(uint32_t)start, (uint32_t)(i - start), color});
    }
    return result;
}
} // namespace vm
