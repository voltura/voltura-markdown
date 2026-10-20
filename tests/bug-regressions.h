#pragma once
#include "editor-tools.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

inline void bugRegressions(const std::filesystem::path &folder) {
    using namespace vm;
    unsigned failures = 0;
    auto check = [&](bool ok, const char *name) {
        if (!ok) {
            std::cerr << "AUDIT: " << name << '\n';
            ++failures;
        }
    };
    check(entity(L"&# +65; &#x+41; &#-1;") == L"&# +65; &#x+41; &#-1;",
          "numeric entities reject signs and whitespace");
    auto doc = parse(L"# Same\n\n# Same\n\n# Same-1\n\n# Same\n");
    check(doc.blocks.size() == 4 && *doc.blocks[0].anchor == L"same" && *doc.blocks[1].anchor == L"same-1" &&
              *doc.blocks[2].anchor == L"same-1-1" && *doc.blocks[3].anchor == L"same-2",
          "repeated heading anchors are unique");
    doc = parse(L"- # Title\n");
    check(!doc.blocks.empty() && doc.blocks[0].anchor && *doc.blocks[0].anchor == L"title",
          "list heading anchor excludes structural list marker");
    doc = parse(L"<h2 id='custom'>Title</h2>\n");
    check(!doc.blocks.empty() && doc.blocks[0].anchor && *doc.blocks[0].anchor == L"custom",
          "explicit HTML heading id is retained");
    doc = parse(L"<ol start='0'><li>Zero</li><li>One</li></ol>\n");
    check(doc.text.find(L"0.  Zero") != doc.text.npos && doc.text.find(L"1.  One") != doc.text.npos,
          "HTML ordered list accepts zero start");
    doc = parse(L"<ol start='-2'><li>Minus two</li><li>Minus one</li></ol>\n");
    check(doc.text.find(L"-2.  Minus two") != doc.text.npos &&
              doc.text.find(L"-1.  Minus one") != doc.text.npos,
          "HTML ordered list accepts negative start");
    doc = parse(L"<ol><li value='7'>Seven</li><li>Eight</li></ol>\n");
    check(doc.text.find(L"7.  Seven") != doc.text.npos && doc.text.find(L"8.  Eight") != doc.text.npos,
          "HTML list item value resets subsequent numbering");
    auto heading = insertMarkdown(L"## Title ##", 0, 11, Insert::Heading, 0);
    check(heading.replacement == L"Title", "heading removal strips optional closing hashes");
    const std::wstring codeTable = L"```\n| A | B |\n| --- | --- |\n| 1 | 2 |\n```\n";
    check(!editTable(codeTable, codeTable.find(L"A"), TableAction::DeleteColumn),
          "table editing does not modify fenced code");
    const std::wstring afterQuote = L"> ```\n> code\n\n| A |\n| --- |\n";
    check(editTable(afterQuote, afterQuote.find(L"A"), TableAction::Center).has_value(),
          "an implicitly closed quoted fence does not hide a following table");
    const std::wstring eofTable = L"| A | B |\n| --- | --- |\n| 1 | 2 |";
    auto table = editTable(eofTable, eofTable.find(L"A"), TableAction::Center);
    check(table && !table->replacement.ends_with(L"\n"), "table edit preserves missing final newline");
    auto next = continueMarkdown(L">  > item", 9);
    check(next && next->replacement == L"\n>  > ", "nested quote continuation preserves spaced markers");
    next = continueMarkdown(L"-   [x] done", 12);
    check(next && next->replacement == L"\n-   [ ] ", "task continuation accepts extra marker spacing");
    std::ofstream(folder / L"literal&copy;.md") << "target";
    doc = parse(L"[file](literal\\&copy;.md)");
    doc.path = folder / L"source.md";
    std::atomic_bool cancel = false;
    check(validateDraft(doc, cancel).empty(), "validation preserves escaped entity-like filenames");
    auto html = exportHtml(parse(L"| A |\n| --- |\n| 1 |\n\n| B |\n| --- |\n| 2 |\n"));
    auto first = html.find("<table>");
    check(first != html.npos && html.find("<table>", first + 1) != html.npos,
          "HTML export keeps separate Markdown tables separate");
    html = exportHtml(parse(L"<table><tr><th>A</th></tr><tr><th>B</th></tr></table>\n"));
    first = html.find("<table>");
    check(first != html.npos && html.find("<table>", first + 1) == html.npos,
          "multiple HTML header rows stay in one table");
    auto longCode = parse(L"```\n" + std::wstring(9000, L'a') + L"\n```\n");
    html = exportHtml(longCode);
    first = html.find("<pre>");
    check(first != html.npos && html.find("<pre>", first + 1) == html.npos,
          "HTML export rejoins bounded pieces of one code block");
    auto preEnd = html.find("</pre>", first);
    check(first != html.npos && preEnd != html.npos &&
              parse(fromUtf8(html.substr(first, preEnd + 6 - first))).text == longCode.text,
          "joined code pieces preserve every character through HTML round trip");
    html = exportHtml(parse(L"<table><tr><td valign='bottom'>A</td></tr></table>\n"));
    check(html.find("vertical-align:bottom") != html.npos,
          "HTML export preserves ordinary table vertical alignment");
    html = exportHtml(parse(L"> > Nested\n"));
    check(html.find("<blockquote><blockquote>") != html.npos, "HTML export preserves nested quote depth");
    next = continueMarkdown(L"> - ", 4);
    check(next && next->start == 2 && next->length == 2 && next->replacement.empty(),
          "empty quoted list exits only the list");
    check(!editTable(L"    | A | B |\n    | --- | --- |\n", 6, TableAction::DeleteColumn),
          "table editing does not modify indented code");
    doc = parse(L"[download](?download=1)");
    doc.path = folder / L"source.md";
    html = exportHtml(doc);
    auto rebased = rebaseLocalLinks(doc.source, doc.path, folder / L"sub/copy.md");
    check(html.find("source.md?download=1") != html.npos && rebased == L"[download](../source.md?download=1)",
          "query-only local links keep referring to the source document");
    std::wstring encoding;
    check(decode("\xef\xbb\xbf\xc3(", &encoding) == L"\ufffd(" && encoding == L"UTF-8",
          "UTF-8 BOM prevents ANSI reinterpretation of damaged UTF-8");
    if (failures)
        throw std::runtime_error(std::to_string(failures) + " audit regressions");
}
