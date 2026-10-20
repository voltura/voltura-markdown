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
    unsigned goalFailures = 0;
    auto goalCheck = [&](bool ok, const char *name) {
        if (!ok) {
            std::cerr << "GOAL AUDIT: " << name << '\n';
            ++goalFailures;
        }
    };
    goalCheck(
        has(L"/* SELECT is a comment */ SELECT 1;", L"sql", L"/* SELECT is a comment */", CodeColor::Comment),
        "015 SQL block comments are comments");
    goalCheck(has(L"echo 'path\\'; if true; then", L"bash", L"if", CodeColor::Keyword) &&
                  has(L"'path`'; return", L"powershell", L"return", CodeColor::Keyword) &&
                  has(L"SELECT 'path\\'; SELECT 1", L"sql", L"'path\\'", CodeColor::String),
              "016 literal strings do not escape their closing quote");
    goalCheck(has(L"var path = @\"C:\\\"; return;", L"csharp", L"return", CodeColor::Keyword) &&
                  has(L"var s = @\"a\"\"b\";", L"cs", L"\"a\"\"b\"", CodeColor::String),
              "032 C# verbatim strings retain backslashes and doubled quotes");
    for (auto prefix : {L"@", L"$@", L"@$"})
        goalCheck(has(std::wstring(prefix) + L"\"C:\\\"; return;", L"c#", L"return", CodeColor::Keyword),
                  "032 interpolated verbatim prefix variants retain their string boundary");
    goalCheck(has(L"echo path#name; if true; then", L"bash", L"if", CodeColor::Keyword) &&
                  has(L"echo \\#; if true; then", L"sh", L"if", CodeColor::Keyword) &&
                  has(L"echo 'path'#name; if true; then", L"shell", L"if", CodeColor::Keyword) &&
                  has(L"echo path\\ #name; if true; then", L"bash", L"if", CodeColor::Keyword),
              "033 shell hashes inside words are not comments");
    goalCheck(has(L"echo path # comment", L"bash", L"# comment", CodeColor::Comment) &&
                  has(L"echo path;# comment", L"sh", L"# comment", CodeColor::Comment) &&
                  has(L"echo \\\n# comment", L"bash", L"# comment", CodeColor::Comment),
              "033 shell comments still start at word boundaries");
    for (auto lang : {L"json", L"python", L"javascript", L"csharp", L"sql"})
        goalCheck(has(L"1.25e-3 + 2E+4", lang, L"1.25e-3", CodeColor::Number) &&
                      has(L"1.25e-3 + 2E+4", lang, L"2E+4", CodeColor::Number),
                  "034 signed exponents form complete numeric tokens");
    goalCheck(has(L"0x1.fp-3", L"cpp", L"0x1.fp-3", CodeColor::Number),
              "034 hexadecimal floating-point exponents retain their sign");
    goalCheck(has(L"0x1e-3", L"cpp", L"0x1e", CodeColor::Number) &&
                  has(L"12-3", L"javascript", L"12", CodeColor::Number) &&
                  !has(L"0x1e-3", L"cpp", L"0x1e-3", CodeColor::Number),
              "034 subtraction remains outside hexadecimal and ordinary numbers");
    const std::wstring rawCpp = L"auto s = R\"tag(first \" // literal\nsecond)tag\"; return;";
    goalCheck(has(rawCpp, L"cpp", L"\"tag(first \" // literal\nsecond)tag\"", CodeColor::String) &&
                  has(rawCpp, L"cpp", L"return", CodeColor::Keyword),
              "037 C++ raw strings retain embedded quotes and comment markers");
    for (auto prefix : {L"R", L"u8R", L"uR", L"UR", L"LR"})
        goalCheck(has(std::wstring(prefix) + L"\"(a\"b\\c)\"; return;", L"c++", L"\"(a\"b\\c)\"",
                      CodeColor::String),
                  "037 C++ raw string encodings accept empty delimiters");
    goalCheck(
        has(L"R\"abcdefghijklmnop(x)abcdefghijklmnop\"; return;", L"cpp", L"return", CodeColor::Keyword),
        "037 maximum-length raw delimiters terminate correctly");
    for (auto quote : {L'\'', L'"'}) {
        auto here = std::wstring(L"@") + quote + L"\ninner " + quote + L" # literal\n" + quote + L"@\nreturn";
        goalCheck(
            has(here, L"powershell", std::wstring_view(here).substr(1, here.size() - 8), CodeColor::String) &&
                has(here, L"powershell", L"return", CodeColor::Keyword),
            "040 PowerShell here-strings retain inner quotes and comment markers");
    }
    goalCheck(has(L"@'\r\n  '@ is body\r\n'@\r\nreturn", L"pwsh", L"return", CodeColor::Keyword) &&
                  has(L"@'\r\n  '@ is body\r\n'@\r\nreturn", L"pwsh", L"'\r\n  '@ is body\r\n'@",
                      CodeColor::String),
              "040 here-string terminators require line start and accept CRLF");
    goalCheck(has(L"auto n = 1'000; return n;", L"cpp", L"1'000", CodeColor::Number) &&
                  has(L"auto n = 1'000; return n;", L"cpp", L"return", CodeColor::Keyword),
              "047 numeric digit separators do not open character literals");
    goalCheck(has(L"0xAB'CD + 0b1010'0101", L"c++", L"0xAB'CD", CodeColor::Number) &&
                  has(L"0xAB'CD + 0b1010'0101", L"c++", L"0b1010'0101", CodeColor::Number),
              "047 hexadecimal and binary digit separators remain within numbers");
    goalCheck(has(L"1 + 'x'; return;", L"cpp", L"'x'", CodeColor::String) &&
                  has(L"1 + 'x'; return;", L"cpp", L"return", CodeColor::Keyword) &&
                  has(L"1'000'000 + 0.1'25e+1'0", L"cpp", L"0.1'25e+1'0", CodeColor::Number),
              "047 character literals and repeated fractional or exponent separators retain boundaries");
    for (auto lang : {L"cs", L"csharp", L"c#"}) {
        const std::wstring raw = L"\"\"\"text \" // literal\"\"\"";
        goalCheck(has(L"var s = " + raw + L"; return;", lang, raw, CodeColor::String) &&
                      has(L"var s = " + raw + L"; return;", lang, L"return", CodeColor::Keyword),
                  "057 C# raw strings retain embedded quotes and comment markers");
    }
    for (auto prefix : {L"", L"$", L"$$"}) {
        const std::wstring raw = L"\"\"\"\"\ntext \"\"\" // literal\\\n{{value}}\n\"\"\"\"";
        auto source = L"var s = " + std::wstring(prefix) + raw + L"; return;";
        goalCheck(has(source, L"cs", raw, CodeColor::String) &&
                      has(source, L"cs", L"return", CodeColor::Keyword),
                  "057 longer raw delimiters and interpolation prefixes retain multiline boundaries");
    }
    goalCheck(has(L"var s = \"\"\"unfinished \" // body", L"csharp", L"\"\"\"unfinished \" // body",
                  CodeColor::String) &&
                  has(L"var s = @\"\"\"quoted\"\"\"; return;", L"csharp", L"\"\"\"quoted\"\"\"",
                      CodeColor::String) &&
                  has(L"var s = @\"\"\"quoted\"\"\"; return;", L"csharp", L"return", CodeColor::Keyword),
              "057 unfinished raw strings stay bounded and verbatim doubled quotes keep their semantics");
    if (goalFailures)
        throw std::runtime_error("Goal code syntax regressions");
    for (auto input : {L"/", L"'", L"/*", L"#", L"\\", L"`", L"123", L"\"unterminated"})
        for (auto t : codeTokens(input, L"typescript"))
            check(t.length && t.start + t.length <= std::wstring_view(input).size());
    std::cout << "Code syntax checks passed\n";
}
