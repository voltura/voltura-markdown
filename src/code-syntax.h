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
    bool shellWord = false;
    auto word = [](wchar_t c) { return iswalnum(c) || c == L'_' || c == L'$'; };
    for (size_t i = 0; i < text.size();) {
        size_t start = i;
        CodeColor color;
        wchar_t c = text[i];
        auto next = [&](wchar_t n) { return i + 1 < text.size() && text[i + 1] == n; };
        if (sh && c == L'\\' && i + 1 < text.size()) {
            // A continuation removes the newline; other escapes stay in the same word.
            if (text[i + 1] != L'\n')
                shellWord = true;
            i += 2;
            continue;
        }
        bool blockComment = (!py && !sh && !ps && c == L'/' && next(L'*')) || (ps && c == L'<' && next(L'#'));
        if (blockComment) {
            auto end = text.find(ps ? L"#>" : L"*/", i + 2);
            i = end == text.npos ? text.size() : end + 2;
            color = CodeColor::Comment;
        } else if (((ps || py || (sh && !shellWord)) && c == L'#') ||
                   (!(ps || py || sh || sql) && c == L'/' && next(L'/')) ||
                   (sql && c == L'-' && next(L'-'))) {
            auto end = text.find(L'\n', i);
            i = end == text.npos ? text.size() : end;
            color = CodeColor::Comment;
        } else if (c == L'\'' || c == L'"' || (js && c == L'`')) {
            if (ps && i && text[i - 1] == L'@') {
                auto newline = text.find_first_not_of(L" \t\r", i + 1);
                if (newline != text.npos && text[newline] == L'\n') {
                    auto closing = text.find(std::wstring(L"\n") + c + L"@", newline);
                    i = closing == text.npos ? text.size() : closing + 3;
                    result.push_back({(uint32_t)start, (uint32_t)(i - start), CodeColor::String});
                    continue;
                }
            }
            if (cpp && lang != L"c" && c == L'"' && i && text[i - 1] == L'R') {
                size_t prefixStart = i;
                while (prefixStart && word(text[prefixStart - 1]))
                    --prefixStart;
                auto prefix = text.substr(prefixStart, i - prefixStart);
                bool rawPrefix = prefix == L"R" || prefix == L"u8R" || prefix == L"uR" || prefix == L"UR" ||
                                 prefix == L"LR";
                size_t open = i + 1;
                while (open < text.size() && open - i <= 17 && text[open] != L'(' && text[open] > L' ' &&
                       text[open] < 127 && text[open] != L')' && text[open] != L'\\')
                    ++open;
                if (rawPrefix && open < text.size() && open - i <= 17 && text[open] == L'(') {
                    auto closing = L")" + std::wstring(text.substr(i + 1, open - i - 1)) + L"\"";
                    auto end = text.find(closing, open + 1);
                    i = end == text.npos ? text.size() : end + closing.size();
                    result.push_back({(uint32_t)start, (uint32_t)(i - start), CodeColor::String});
                    continue;
                }
            }
            bool triple = py && i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c;
            bool verbatim =
                cs && c == L'"' &&
                ((i && text[i - 1] == L'@') || (i >= 2 && text[i - 1] == L'$' && text[i - 2] == L'@'));
            if (cs && c == L'"' && !verbatim) {
                size_t quotes = 1;
                while (i + quotes < text.size() && text[i + quotes] == L'"')
                    ++quotes;
                if (quotes >= 3) {
                    auto closing = text.find(text.substr(i, quotes), i + quotes);
                    i = closing == text.npos ? text.size() : closing + quotes;
                    result.push_back({(uint32_t)start, (uint32_t)(i - start), CodeColor::String});
                    continue;
                }
            }
            bool escapes = !sql && !verbatim && !((ps || sh) && c == L'\'');
            i += triple ? 3 : 1;
            while (i < text.size()) {
                if (escapes && text[i] == (ps ? L'`' : L'\\') && i + 1 < text.size()) {
                    i += 2;
                    continue;
                }
                if ((sql || verbatim || (ps && c == L'\'')) && text[i] == c && i + 1 < text.size() &&
                    text[i + 1] == c) {
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
            bool hex = c == L'0' && (next(L'x') || next(L'X'));
            ++i;
            while (i < text.size()) {
                auto digit = [&](wchar_t value) {
                    return (value >= L'0' && value <= L'9') ||
                           (hex && ((value >= L'a' && value <= L'f') || (value >= L'A' && value <= L'F')));
                };
                if (cpp && text[i] == L'\'' && i + 1 < text.size() && digit(text[i - 1]) &&
                    digit(text[i + 1])) {
                    ++i;
                    continue;
                }
                if (iswalnum(text[i]) || text[i] == L'.' || text[i] == L'_') {
                    ++i;
                    continue;
                }
                bool exponent = hex ? (text[i - 1] == L'p' || text[i - 1] == L'P')
                                    : (text[i - 1] == L'e' || text[i - 1] == L'E');
                if ((text[i] == L'+' || text[i] == L'-') && exponent && i + 1 < text.size() &&
                    iswdigit(text[i + 1])) {
                    ++i;
                    continue;
                }
                break;
            }
            color = CodeColor::Number;
        } else if (word(c)) {
            shellWord = true;
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
            if (sh)
                shellWord = !iswspace(c) && std::wstring_view(L";|&()<>").find(c) == std::wstring_view::npos;
            ++i;
            continue;
        }
        if (sh && color != CodeColor::Comment)
            shellWord = true;
        result.push_back({(uint32_t)start, (uint32_t)(i - start), color});
    }
    return result;
}
} // namespace vm
