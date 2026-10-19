#include "../src/code-syntax.h"
#include <iostream>
#include <stdexcept>
int main() {
    using namespace vm;
    auto has = [](std::wstring_view text, std::wstring_view lang, std::wstring_view token, CodeColor color) {
        for (auto t : codeTokens(text, lang))
            if (t.color == color && text.substr(t.start, t.length) == token)
                return true;
        return false;
    };
    auto check = [](bool ok) {
        if (!ok)
            throw std::runtime_error("Code highlighting regression");
    };
    for (auto lang : {L"ts", L"typescript", L"js", L"javascript", L"TSX"}) {
        check(has(L"const answer = 42; // hi", lang, L"const", CodeColor::Keyword));
        check(has(L"const answer = 42; // hi", lang, L"42", CodeColor::Number));
        check(has(L"const answer = 42; // hi", lang, L"// hi", CodeColor::Comment));
        check(has(L"let x = `hello`;", lang, L"`hello`", CodeColor::String));
    }
    for (auto lang : {L"cs", L"c#", L"csharp", L"cpp"}) {
        check(has(L"public string s = \"hello\\\" world\";", lang, L"public", CodeColor::Keyword));
        check(has(L"/* first\nsecond */ return 1;", lang, L"/* first\nsecond */", CodeColor::Comment));
    }
    for (auto lang : {L"powershell", L"pwsh", L"ps1"}) {
        check(has(L"IF ($value) { # hello", lang, L"IF", CodeColor::Keyword));
        check(has(L"$value = 'hello'", lang, L"$value", CodeColor::Variable));
        check(has(L"<# a\nb #>", lang, L"<# a\nb #>", CodeColor::Comment));
    }
    check(has(L"def test(): # hello", L"python", L"def", CodeColor::Keyword));
    check(has(L"SELECT * FROM t -- hello", L"sql", L"SELECT", CodeColor::Keyword));
    check(has(L"{\"ok\":true}", L"json", L"true", CodeColor::Keyword));
    check(has(L"if $value; then", L"bash", L"$value", CodeColor::Variable));
    check(codeTokens(L"const x = 42", L"unknown").empty());
    check(codeTokens(L"", L"ts").empty());
    check(codeTokens(L"https://example.com", L"plain").empty());
    for (auto input : {L"/", L"'", L"/*", L"#", L"\\", L"`", L"123", L"\"unterminated"})
        for (auto t : codeTokens(input, L"typescript"))
            check(t.length && t.start + t.length <= std::wstring_view(input).size());
    std::cout << "Code syntax checks passed\n";
}
