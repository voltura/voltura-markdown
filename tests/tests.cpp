#include "document.h"
#include "md4c-html.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <psapi.h>
#include <stdexcept>
using namespace vm;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void validate(const Document &d) {
    for (auto &b : d.blocks) {
        require((size_t)b.start + b.length <= d.text.size(), "block outside text");
        for (auto &r : b.runs)
            require(r.start >= b.start && (size_t)r.start + r.length <= b.start + b.length,
                    "run outside block");
        for (auto &c : b.cells)
            require(c.start >= b.start && (size_t)c.start + c.length <= b.start + b.length,
                    "cell outside block");
    }
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc > 1 && std::wstring(argv[1]) == L"--html") {
            std::string s((std::istreambuf_iterator<char>(std::cin)), {}), out;
            int result = md_html(
                s.data(), (MD_SIZE)s.size(),
                [](const MD_CHAR *text, MD_SIZE size, void *data) {
                    ((std::string *)data)->append(text, size);
                },
                &out, 0, 0);
            // Exercise the actual native document adapter on every spec example as well.
            validate(parse(fromUtf8(s), nullptr, 0));
            std::cout << out;
            return result;
        }
        if (argc > 2 && std::wstring(argv[1]) == L"--benchmark") {
            auto begin = std::chrono::steady_clock::now();
            auto d = load(argv[2]);
            validate(d);
            PROCESS_MEMORY_COUNTERS_EX memory{};
            GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory));
            std::cout
                << "{\"read_ms\":" << d.readMs << ",\"parse_ms\":" << d.parseMs << ",\"total_ms\":"
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count()
                << ",\"source_chars\":" << d.source.size() << ",\"blocks\":" << d.blocks.size()
                << ",\"working_set_bytes\":" << memory.WorkingSetSize
                << ",\"peak_working_set_bytes\":" << memory.PeakWorkingSetSize << "}\n";
            return 0;
        }
        auto d = parse(
            L"# Hello &amp; world\n\nA **bold** and *italic* [link](https://example.com).\n\n> Quote\n\n- "
            L"[x] Done\n- Open\n\n| A | B |\n| :- | -: |\n| one | two |\n\n```cpp\nint main() "
            L"{}\n```\n\nFootnote[^x].\n\n[^x]: Body\n\n> [!NOTE]\n> Alert\n\n![alt](photo.png)\n");
        validate(d);
        require(d.text.find(L"Hello & world") != d.text.npos, "entities");
        require(d.text.find(L"☑") != d.text.npos, "tasks");
        require(d.text.find(L"Body") != d.text.npos, "footnotes");
        require(d.text.find(L"Alert") != d.text.npos, "admonitions");
        require(std::any_of(d.blocks.begin(), d.blocks.end(),
                            [](auto &b) { return b.kind == Kind::Table && b.cells.size() == 2 && b.header; }),
                "table header");
        require(std::any_of(d.blocks.begin(), d.blocks.end(),
                            [](auto &b) { return b.kind == Kind::Image && b.image == L"photo.png"; }),
                "image");
        bool bold = false, italic = false, link = false;
        for (auto &b : d.blocks)
            for (auto &r : b.runs) {
                bold |= (r.style & Bold) != 0;
                italic |= (r.style & Italic) != 0;
                link |= r.link == L"https://example.com";
            }
        require(bold && italic && link, "inline styles");
        auto html = parse(L"A <strong>bold</strong> <em>word</em><br>next &copy; "
                          L"<script>alert(1)</script>\n\n<table><tr><th>X</th><th>Y</th></tr><tr><td>A</"
                          L"td><td>B</td></tr></table>\n");
        validate(html);
        require(html.text.find(L"<script>") != html.text.npos, "unknown HTML must remain visible");
        require(html.text.find(L"©") != html.text.npos, "named entity");
        require(std::any_of(html.blocks.begin(), html.blocks.end(),
                            [](auto &b) { return b.kind == Kind::Table && b.cells.size() == 2; }),
                "HTML table");
        auto structure =
            parse(L"<section><H1>HTML title</H1><h6>Small heading</h6><blockquote><p>Quoted text</p>"
                  L"</blockquote><ol start='4'><li>Four</li><li>Five<ul><li>Nested</li></ul></li></ol>"
                  L"<hr><details><summary>More information</summary><p><span style='display:none'>Always "
                  L"expanded</span></p></details>"
                  L"<pre>first\n  second</pre></section>\n\n# Markdown after HTML\n");
        validate(structure);
        bool htmlHeading = false, smallHeading = false, quoteBlock = false, nestedList = false, rule = false,
             summary = false, preformatted = false, markdownAfter = false;
        for (const auto &block : structure.blocks) {
            auto label = structure.text.substr(block.start, block.length);
            htmlHeading |= block.kind == Kind::Heading && block.level == 1 && label == L"HTML title";
            smallHeading |= block.kind == Kind::Heading && block.level == 6;
            quoteBlock |= block.quote == 1 && label.find(L"Quoted text") != label.npos;
            nestedList |= block.indent == 2 && label.find(L"Nested") != label.npos;
            rule |= block.kind == Kind::Rule;
            preformatted |= block.kind == Kind::Code && label == L"first\n  second";
            markdownAfter |= block.kind == Kind::Heading && label == L"Markdown after HTML" && !block.quote &&
                             !block.indent;
            for (const auto &run : block.runs)
                summary |=
                    (run.style & Bold) && structure.text.substr(run.start, run.length) == L"More information";
        }
        require(htmlHeading && smallHeading && quoteBlock && nestedList && rule && summary && preformatted &&
                    markdownAfter,
                "structural HTML maps to scoped native blocks and returns to Markdown formatting");
        require(structure.text.find(L"4.  Four") != structure.text.npos &&
                    structure.text.find(L"5.  Five") != structure.text.npos &&
                    structure.text.find(L"Always expanded") != structure.text.npos,
                "ordered HTML start values and expanded details remain readable");
        auto malformedHtml =
            parse(L"<div><ol start='-7'><li>Default one<li>Default two</ol><kbd-custom>literal</kbd-custom>"
                  L"<kbd@invalid>unknown</kbd@invalid><script>no execution</script></div>");
        validate(malformedHtml);
        require(malformedHtml.text.find(L"1.  Default one") != malformedHtml.text.npos &&
                    malformedHtml.text.find(L"2.  Default two") != malformedHtml.text.npos &&
                    malformedHtml.text.find(L"<kbd-custom>literal</kbd-custom>") != malformedHtml.text.npos &&
                    malformedHtml.text.find(L"<kbd@invalid>unknown</kbd@invalid>") !=
                        malformedHtml.text.npos &&
                    malformedHtml.text.find(L"<script>no execution</script>") != malformedHtml.text.npos,
                "optional list closures, invalid starts, and unknown HTML stay safe and readable");
        std::wstring shortcut =
            L"Press <kbd>Ctrl</kbd>+<kbd>O</kbd>, then <kbd>A</kbd><kbd>B</kbd> normally.";
        auto keys = parse(shortcut);
        validate(keys);
        std::vector<std::wstring> labels;
        for (auto &block : keys.blocks)
            for (auto &run : block.runs)
                if (run.style & Keycap)
                    labels.push_back(keys.text.substr(run.start, run.length));
        require(labels == std::vector<std::wstring>{L"Ctrl", L"O", L"A", L"B"},
                "keyboard tags retain distinct keycaps, including adjacent keys");
        require(keys.source == shortcut && keys.text.find(L"Ctrl+O") != keys.text.npos &&
                    keys.text.find(L"<kbd>") == keys.text.npos,
                "keyboard styling preserves source and copied text without synthetic padding");
        auto nestedKeys = parse(L"<strong><kbd>Shift</kbd></strong> plain <kbd><kbd>F1</kbd></kbd>");
        validate(nestedKeys);
        bool nestedKey = false, plainRestored = false;
        for (auto &block : nestedKeys.blocks)
            for (auto &run : block.runs) {
                auto label = nestedKeys.text.substr(run.start, run.length);
                nestedKey |= label == L"Shift" && (run.style & (Keycap | Bold)) == (Keycap | Bold);
                plainRestored |= label.find(L"plain") != label.npos && !(run.style & Keycap);
            }
        require(nestedKey && plainRestored, "nested keyboard styling restores surrounding formatting");
        auto longKey = parse(L"<kbd>" + std::wstring(20000, L'x') + L"</kbd>");
        validate(longKey);
        require(longKey.blocks.size() >= 3, "long keyboard content retains bounded layout blocks");
        std::wstring huge(200000, L'a');
        auto large = parse(huge);
        validate(large);
        require(large.blocks.size() > 20, "bounded paragraph layout");
        require(large.text.substr(0, huge.size()) == huge, "long paragraph content preserved");
        for (auto &b : large.blocks)
            require(b.length <= 8192, "bounded block");
        auto unicodeBoundary = parse(std::wstring(8191, L'a') + L"😀b");
        validate(unicodeBoundary);
        require(unicodeBoundary.blocks.size() == 2, "Unicode boundary continuation count");
        for (const auto &block : unicodeBoundary.blocks) {
            require(!block.length || unicodeBoundary.text[block.start] < 0xdc00 ||
                        unicodeBoundary.text[block.start] > 0xdfff,
                    "block starts inside surrogate pair");
            require(!block.length || unicodeBoundary.text[block.start + block.length - 1] < 0xd800 ||
                        unicodeBoundary.text[block.start + block.length - 1] > 0xdbff,
                    "block ends inside surrogate pair");
        }
        auto table = parse(L"| A | B |\n| --- | --- |\n| " + std::wstring(4095, L'a') + L"😀" +
                           std::wstring(5903, L'a') + L" | " + std::wstring(10000, L'b') + L" |\n");
        validate(table);
        for (const auto &block : table.blocks)
            for (const auto &cell : block.cells) {
                require(!cell.length || table.text[cell.start] < 0xdc00 || table.text[cell.start] > 0xdfff,
                        "table cell starts inside surrogate pair");
                require(!cell.length || table.text[cell.start + cell.length - 1] < 0xd800 ||
                            table.text[cell.start + cell.length - 1] > 0xdbff,
                        "table cell ends inside surrogate pair");
            }
        auto firstB = (uint32_t)table.text.find(L"bbbb");
        auto matchRow = blockAtPosition(table.blocks, firstB);
        require(std::any_of(table.blocks[matchRow].cells.begin(), table.blocks[matchRow].cells.end(),
                            [&](const Cell &cell) {
                                return firstB >= cell.start && firstB < cell.start + cell.length;
                            }),
                "find resolves the actual table continuation cell");
        std::wstring header = L"|", separator = L"|", row = L"|";
        for (int i = 0; i < 257; ++i) {
            header += L" h |";
            separator += L" --- |";
            row += L" a |";
        }
        auto wide = parse(L"# Before wide table\n\n" + header + L"\n" + separator + L"\n" + row +
                          L"\n\n# After wide table\n");
        validate(wide);
        unsigned preservedHeadings = 0, flattenedRows = 0;
        for (const auto &block : wide.blocks) {
            preservedHeadings += block.kind == Kind::Heading;
            flattenedRows += block.kind == Kind::Paragraph;
            require(block.cells.size() <= 256 && block.length <= 8192,
                    "wide table degrades locally to bounded paragraphs");
        }
        require(preservedHeadings == 2 && flattenedRows == 2 && wide.text.find(L"a\ta\ta") != wide.text.npos,
                "wide table keeps surrounding formatting and original cell copy order");
        auto unicodeWide = parse(header + L"\n" + separator + L"\n| " + std::wstring(8191, L'x') + L"😀 |" +
                                 row.substr(1) + L"\n");
        validate(unicodeWide);
        for (const auto &block : unicodeWide.blocks) {
            require(block.length <= 8192, "long flattened wide rows retain bounded layout");
            require(!block.length || unicodeWide.text[block.start] < 0xdc00 ||
                        unicodeWide.text[block.start] > 0xdfff,
                    "flattened wide row starts outside a surrogate pair");
            require(!block.length || unicodeWide.text[block.start + block.length - 1] < 0xd800 ||
                        unicodeWide.text[block.start + block.length - 1] > 0xdbff,
                    "flattened wide row ends outside a surrogate pair");
        }
        auto raw = sourceBlocks(L"one\r\ntwo\n三 😀\n");
        require(raw.size() == 1 && raw[0].length == 14, "source CRLF Unicode preserved");
        require(decode("\xef\xbb\xbfhello") == L"hello", "UTF-8 BOM");
        require(decode(std::string("\xff\xfeh\0i\0", 6)) == L"hi", "UTF-16 LE");
        require(decode(std::string("\xfe\xff\0h\0i", 6)) == L"hi", "UTF-16 BE");
        require(parse(L"").blocks.empty(), "empty document");
        std::atomic_bool cancel = true;
        bool cancelled = false;
        try {
            parse(huge, &cancel);
        } catch (...) {
            cancelled = true;
        }
        require(cancelled, "cancellation");
        auto legacy = parse(L"Title\n=====\n\n    indented\n\n[ref]: "
                            L"https://example.org\n\n[Reference][ref]\n\nline  \nbreak\n");
        validate(legacy);
        require(legacy.blocks.front().kind == Kind::Heading, "Setext");
        require(legacy.text.find(L"line\nbreak") != legacy.text.npos, "hard break");
        require(entity(L"&#x1F600; &NotEqualTilde; &bogus;") == L"😀 ≂̸ &bogus;",
                "numeric and two-codepoint entities");
        std::cout << "PASS: adapter, styles, HTML subset, tables, extensions, Unicode, encodings, source, "
                     "cancellation, bounded blocks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
