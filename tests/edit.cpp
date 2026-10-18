#include "edit.h"
#include "editor.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace vm;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::wstring applyEdit(std::wstring text, const TextEdit &edit) {
    text.replace(edit.start, edit.length, edit.replacement);
    return text;
}
int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--list-recovery") {
        for (auto &draft : DraftRecovery::list())
            std::cout << toUtf8(DraftRecovery::read(draft.snapshot).text) << '\n';
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--print-pdf") {
        HDC printer = CreateDCW(L"WINSPOOL", L"Microsoft Print to PDF", nullptr, nullptr);
        if (!printer) {
            std::cerr << "Microsoft Print to PDF is unavailable\n";
            return 2;
        }
        try {
            auto output = std::filesystem::absolute(std::filesystem::path(argv[2]));
            auto image = output.parent_path() / L"print-fixture.svg";
            {
                std::ofstream svg(image);
                svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"160\" height=\"80\"><rect "
                       "width=\"160\" height=\"80\" fill=\"#0088cc\"/></svg>";
            }
            std::wstring source =
                L"# Voltura Markdown PDF\n\n**Bold**, *italic*, <u>underlined</u>, Unicode åäö.\n\n| Name | "
                L"Value |\n| --- | ---: |\n| First | 42 |\n| Second | 84 |\n\n";
            source += L"![Printed image](<" + markdownFileDestination(image) + L">)\n\n";
            for (unsigned i = 0; i < 180; ++i)
                source += L"Paragraph " + std::to_wstring(i) +
                          L": A printable Markdown document with multiple pages.\n\n";
            printDocument(parse(source), printer, output);
            DeleteDC(printer);
            return 0;
        } catch (const std::exception &e) {
            DeleteDC(printer);
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
    auto folder =
        std::filesystem::temp_directory_path() / (L"vmd-edit-test-" + std::to_wstring(GetCurrentProcessId()));
    try {
        std::filesystem::create_directory(folder);
        {
            auto outline = sourceOutline(
                L"# Heading    \n---\n\nå😀\n===\n\n```\n# hidden\n```\n\n> ## Quoted\n\n- ### Listed\n");
            require(outline.size() == 4 && outline[0].title == L"Heading" && outline[1].title == L"å😀" &&
                        outline[2].title == L"Quoted" && outline[3].title == L"Listed",
                    "outline follows Markdown headings including containers and Unicode");
            auto next = continueMarkdown(L"9. item", 7);
            require(next && next->replacement == L"\n10. ", "numbered list continuation");
            next = continueMarkdown(L"> - [x] done", 12);
            require(next && next->replacement == L"\n> - [ ] ", "task and quote continuation");
            next = continueMarkdown(L"- ", 2);
            require(next && next->replacement.empty() && next->start == 0 && next->length == 2,
                    "empty item exits list");
            require(!continueMarkdown(L"ordinary", 8), "ordinary Enter stays native");
            require(applyEdit(L"one\ntwo", indentMarkdown(L"one\ntwo", 0, 7, false)) == L"    one\n    two",
                    "indent selected lines");
            require(applyEdit(L"    one\n\ttwo", indentMarkdown(L"    one\n\ttwo", 0, 12, true)) ==
                        L"one\ntwo",
                    "outdent spaces and tabs");
            std::wstring table = L"| ``a|b`` | c\\|d | target |\n| --- | :---: | ---: |\n| 1 | 2 | 3 |\n";
            auto caret = table.find(L"target");
            auto edit = editTable(table, caret, TableAction::DeleteColumn);
            require(edit && edit->replacement.find(L"``a|b`` | c\\|d |") != edit->replacement.npos &&
                        edit->replacement.find(L"target") == edit->replacement.npos,
                    "table edits preserve escaped and code pipes");
            edit = editTable(table, caret, TableAction::Center);
            require(edit && edit->replacement.find(L"| --- | :---: | :---: |") != edit->replacement.npos,
                    "align selected column");
            require(!editTable(L"not a table", 0, TableAction::AddRow), "non table remains untouched");
            std::wstring literalTable = L"| A | B | C |\n| --- | --- | --- |\n| a` | b`` | c |\n";
            auto literal = editTable(literalTable, literalTable.find(L"b``"), TableAction::AddRow);
            require(literal && literal->replacement.find(L"| a` | b`` | c |") != literal->replacement.npos,
                    "unmatched backticks cannot swallow table columns");
            std::atomic_bool cancellation = false;
            require(replaceMatches(L"a A aa å Å", L"a", L"aa", false, true, cancellation) == L"aa aa aa å Å",
                    "replace all visits original whole word matches once");
            require(replaceMatches(L"å Å åä", L"å", L"x", true, true, cancellation) == L"x Å åä",
                    "case sensitive replacement");
            require(replaceMatches(L"å Å åä", L"å", L"x", false, true, cancellation) == L"x x åä",
                    "Unicode case folding and word boundaries");
            require(replaceMatches(L"unchanged", L"", L"x", true, false, cancellation) == L"unchanged",
                    "empty find is safe");
            auto fences = sourceFences(L"````cpp\nint x;\n```\nstill code\n````\nnormal\n");
            require(fences.size() == 3 && fences[1].state.length == 4 && fences[2].state.marker == 0,
                    "code context uses matching fence lengths");
            fences = sourceFences(L"```text\n- ```\n- literal\n```\n");
            require(fences.size() == 3 && fences[2].offset == 28,
                    "literal container-like lines do not close top-level fences");
            fences = sourceFences(L"> ```\n> code\n\n- item\n");
            require(fences.size() == 3 && !fences.back().state.marker,
                    "container code ends when its quote ends");
            fences = sourceFences(L"  - ```\n    code\n  - next\n");
            require(fences.size() == 3 && fences[1].state.indent == 4 && !fences.back().state.marker,
                    "indented sibling ends its list fence");
            require(!syntaxSpans(L"const value = 42; // comment\n", {L'`', 3}).empty(),
                    "code highlighting accepts viewport context");
            auto html = exportHtml(
                parse(L"# Heading\n\n**bold** <u>under</u> "
                      L"[bad](javascript:alert)\n\n|a|b|\n|:-|--:|\n|c|d|\n\n```\n<script>\n```\n"));
            require(html.find("<h1") != html.npos && html.find("<strong>bold</strong>") != html.npos &&
                        html.find("<u>under</u>") != html.npos && html.find("&lt;script&gt;") != html.npos &&
                        html.find("javascript:") == html.npos && html.find("text-align:right") != html.npos,
                    "HTML formatting and escaped inactive content");
            auto linked = parse(L"[file](folder/a%20%C3%A5.md#title)\n\n![image](C:/images/a.png)\n");
            linked.path = folder / L"source.md";
            html = exportHtml(linked);
            require(html.find("file:///") != html.npos && html.find("a%20") != html.npos &&
                        html.find("images/a.png") != html.npos,
                    "export resolves local links and Windows images");
            auto rebased = rebaseLocalLinks(
                L"[doc](other.md#title) ![image](source.assets/a%20%C3%A5.png) `![literal](a.png)`\n",
                folder / L"source.md", folder / L"sub" / L"copy.md");
            require(rebased.find(L"../other.md#title") != rebased.npos &&
                        rebased.find(L"../source.assets/a å.png") == rebased.npos &&
                        rebased.find(L"../source.assets/a%20å.png") != rebased.npos &&
                        rebased.find(L"`![literal](a.png)`") != rebased.npos,
                    "Save As rebases actual destinations only");
            auto image = folder / L"sample.png";
            {
                std::ofstream out(image);
                out << "image";
            }
            auto first = copyImageAsset(image, folder / L"source.md"),
                 second = copyImageAsset(image, folder / L"source.md");
            require(first != second && std::filesystem::exists(first) && std::filesystem::exists(second),
                    "managed assets never overwrite");
            wchar_t previous[32768]{};
            GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 32768);
            SetEnvironmentVariableW(L"LOCALAPPDATA", folder.c_str());
            auto state = std::make_shared<DraftRecovery>();
            state->write(L"recover å😀", folder / L"source.md");
            require(DraftRecovery::list().empty(), "live process drafts are not offered as crashed sessions");
            std::filesystem::path snapshot;
            for (auto &entry : std::filesystem::directory_iterator(DraftRecovery::folder()))
                if (entry.path().extension() == L".draft")
                    snapshot = entry.path();
            auto orphan = snapshot.parent_path() / L"4294967294-test.draft";
            std::filesystem::copy_file(snapshot, orphan);
            auto drafts = DraftRecovery::list();
            require(drafts.size() == 1 && drafts[0].text.empty() &&
                        DraftRecovery::read(drafts[0].snapshot).text == L"recover å😀" &&
                        drafts[0].original == folder / L"source.md",
                    "recovery preserves Unicode and original path");
            state->clear(true);
            state->write(L"stale", {});
            require(!std::filesystem::exists(snapshot),
                    "closed recovery writer cannot resurrect discarded draft");
            SetEnvironmentVariableW(L"LOCALAPPDATA", previous);
        }
        require(applyEdit(L"hello world", insertMarkdown(L"hello world", 6, 11, Insert::Bold)) ==
                    L"hello **world**",
                "bold selection");
        auto empty = insertMarkdown(L"", 0, 0, Insert::Italic);
        require(empty.replacement == L"*italic text*" && empty.selection == 1 && empty.selected == 11,
                "placeholder selection");
        for (auto kind : {Insert::Bold, Insert::Italic, Insert::Strike}) {
            unsigned style = kind == Insert::Bold ? Bold : kind == Insert::Italic ? Italic : Strike;
            for (auto selection : {L" hello ", L"\thello\n", L"hello "}) {
                std::wstring draft = L"Prefix:" + std::wstring(selection) + L"suffix";
                auto operation = insertMarkdown(draft, 7, 7 + wcslen(selection), kind);
                auto formatted = parse(applyEdit(draft, operation));
                bool found = false;
                for (auto &block : formatted.blocks)
                    for (auto &run : block.runs)
                        found |= (run.style & style) != 0;
                require(found, "formatting helpers keep whitespace outside delimiters");
                require(operation.replacement.substr(operation.selection, operation.selected) == L"hello",
                        "formatting selects inner content");
            }
            require(insertMarkdown(L" \t", 0, 2, kind).replacement == L" \t",
                    "whitespace-only selection unchanged");
        }
        auto spacedCode = parse(insertMarkdown(L" hi ", 0, 4, Insert::InlineCode).replacement);
        require(spacedCode.text == L" hi \n", "inline code retains selected outer spaces");
        auto underline = insertMarkdown(L"underlined", 0, 10, Insert::Underline);
        require(underline.replacement == L"<u>underlined</u>", "underline HTML syntax");
        auto underlined = parse(underline.replacement);
        require(!underlined.blocks.empty() && !underlined.blocks.front().runs.empty() &&
                    (underlined.blocks.front().runs.front().style & Underline),
                "underline preview");
        for (unsigned level = 1; level <= 6; ++level)
            require(insertMarkdown(L"title", 0, 5, Insert::Heading, level).replacement ==
                        std::wstring(level, L'#') + L" title",
                    "all heading levels");
        require(applyEdit(L"## One\nTwo", insertMarkdown(L"## One\nTwo", 0, 9, Insert::Heading, 3)) ==
                    L"### One\n### Two",
                "heading replaces existing prefix");
        require(applyEdit(L"## One", insertMarkdown(L"## One", 0, 6, Insert::Heading, 0)) == L"One",
                "normal text");
        require(applyEdit(L"one\ntwo\nthree", insertMarkdown(L"one\ntwo\nthree", 0, 8, Insert::Numbered)) ==
                    L"1. one\n2. two\nthree",
                "selected lines exclude trailing boundary");
        require(insertMarkdown(L"", 0, 0, Insert::Link, 0, 0, L"[å]", L"folder/a b(1).md").replacement ==
                    L"[\\[å\\]](<folder/a%20b(1).md>)",
                "link escaping");
        auto code = insertMarkdown(L"```\ninside", 0, 10, Insert::Code);
        require(code.replacement.starts_with(L"````\n"), "safe code fence");
        auto table = insertMarkdown(L"beforeafter", 6, 6, Insert::Table, 3, 2);
        auto parsed = parse(table.replacement);
        require(std::any_of(parsed.blocks.begin(), parsed.blocks.end(),
                            [](auto &b) { return b.kind == Kind::Table; }),
                "table renders");
        require(table.replacement.substr(table.selection, table.selected) == L"Header 1",
                "table placeholder selected");
        require(editText(L"a\r\nb\rc\n") == L"a\nb\nc\n", "editor normalization");
        std::atomic_bool cancel = false;
        auto warnings = [&](std::wstring source) {
            auto doc = parse(std::move(source));
            doc.path = folder / "draft.md";
            return validateDraft(doc, cancel);
        };
        require(
            warnings(L"# Hello\n\n[ok](#hello)\n`[code](missing.md)`\n\n```\n[ignored](missing.md)\n```\n")
                .empty(),
            "valid anchors and code exclusions");
        auto missing = warnings(L"# Å😀\n\n[bad](#absent)\n![image](missing.png)\n");
        require(missing.size() == 2 && missing[0].line == 3 && missing[1].line == 4,
                "Unicode diagnostic lines");
        require(missing[0].offset == std::wstring(L"# Å😀\n\n[bad](").size(), "UTF16 offset");
        require(warnings(L"```\nunclosed\n").size() == 1, "unclosed fence");
        require(warnings(L"```\n").size() == 1, "empty unclosed fence");
        require(warnings(L"```\n```\n").empty(), "empty closed fence");
        require(warnings(L"````x````\n\n```\nfoo\n```\n").empty(), "inline backticks before fenced code");
        require(warnings(L"> ```\n> code\n> ```\n").empty(), "quoted code fence");
        require(warnings(L"- ```\n  code\n  ```\n").empty(), "list code fence");
        require(warnings(L"~~~\ncode\n~~~\n\n```\nmore\n```\n").empty(), "multiple fences");
        require(warnings(L"    [ignored](missing.md)\n").empty(), "indented code excluded");
        auto untitled = parse(L"[deferred](missing.md)");
        require(validateDraft(untitled, cancel).empty(), "untitled relative checks deferred");
        {
            std::ofstream file(folder / "a b.md");
            file << "exists";
        }
        require(
            warnings(
                L"[local](a%20b.md)\n[remote](https://example.invalid)\n[mail](mailto:a@example.invalid)\n")
                .empty(),
            "percent paths and remote skips");
        require(warnings(L"[ref][id]\n\n[id]: missing.md\n").size() == 1, "reference destination checked");
        {
            std::ofstream file(folder / "a#b.md");
            file << "exists";
        }
        require(warnings(L"[local](a%23b.md)").empty(), "encoded filename fragment character");
        {
            auto filePath = folder / "image#1%20&copy;.svg";
            std::ofstream file(filePath);
            file << "existing image";
            file.close();
            auto image =
                insertMarkdown(L"", 0, 0, Insert::Image, 0, 0, L"Image", markdownFileDestination(filePath));
            require(image.replacement.find(L"image%231%2520%26copy;.svg") != std::wstring::npos,
                    "reserved file characters encoded");
            require(warnings(image.replacement).empty(), "helper destination resolves to original filename");
            auto url = insertMarkdown(L"", 0, 0, Insert::Link, 0, 0, L"web",
                                      L"https://example.invalid/a%20b#section");
            require(url.replacement.find(L"a%20b#section") != std::wstring::npos,
                    "URL escapes and fragments retained");
        }
        require(
            warnings(L"[absolute](<" + (folder / L"missing-image.png").generic_wstring() + L">)").size() == 1,
            "missing drive-absolute target");
        require(warnings(L"[absolute](<" + (folder / L"a%23b.md").generic_wstring() + L">)").empty(),
                "existing drive-absolute target");
        cancel = true;
        require(warnings(L"[bad](missing.md)").empty(), "validation cancellation");
        cancel = false;
        auto path = folder / L"å draft.md";
        FileBaseline none;
        auto saved = saveDraft(path, L"# New\n", none, none);
        require(saved.status == SaveResult::Saved && saved.baseline.bytes == "# New\r\n",
                "new UTF8 CRLF save");
        auto baseline = readBaseline(path);
        auto changed = saveDraft(path, L"next", none, none);
        require(changed.status == SaveResult::Conflict && readBaseline(path).bytes == baseline.bytes,
                "new-file race conflict");
        std::string mixed = "\xef\xbb\xbf"
                            "one\r\ntwo\nthree\rfour";
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << mixed;
        }
        baseline = readBaseline(path);
        saved = saveDraft(path, L"one\ntwo\ninserted\nthree\nfour", baseline, baseline);
        require(saved.status == SaveResult::Saved && saved.baseline.bytes ==
                                                         "\xef\xbb\xbf"
                                                         "one\r\ntwo\ninserted\r\nthree\rfour",
                "BOM mixed endings retained");
        auto external = readBaseline(path);
        {
            std::ofstream file(path);
            file << "external";
        }
        require(saveDraft(path, L"mine", external, external).status == SaveResult::Conflict,
                "external changes protected");
        require(readBaseline(path).bytes == "external", "conflict preserves original");
        for (bool le : {true, false}) {
            std::string bytes = le ? "\xff\xfe" : "\xfe\xff";
            for (auto c : std::wstring(L"å\r\n")) {
                bytes += (char)(le ? c & 255 : c >> 8);
                bytes += (char)(le ? c >> 8 : c & 255);
            }
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file << bytes;
            }
            baseline = readBaseline(path);
            saved = saveDraft(path, L"å\n", baseline, baseline);
            require(saved.status == SaveResult::Saved && saved.baseline.bytes == bytes, "UTF16 round trip");
        }
        if (GetACP() != CP_UTF8) {
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file << "\xe5\n";
            }
            baseline = readBaseline(path);
            require(saveDraft(path, L"😀\n", baseline, baseline).status == SaveResult::NeedsUtf8,
                    "ANSI conversion requires consent");
            require(readBaseline(path).bytes == baseline.bytes, "declined conversion retains bytes");
            saved = saveDraft(path, L"😀\n", baseline, baseline, true);
            require(saved.status == SaveResult::Saved && decode(saved.baseline.bytes) == L"😀\n",
                    "explicit UTF8 conversion");
        }
        baseline = readBaseline(path);
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
        require(saveDraft(path, L"blocked", baseline, baseline).status == SaveResult::Failed,
                "readonly failure");
        require(readBaseline(path).bytes == baseline.bytes, "failed save preserves original");
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        HWND owner = CreateWindowW(L"STATIC", L"Editor tests", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 700, nullptr,
                                   nullptr, GetModuleHandleW(nullptr), nullptr);
        {
            Editor editor(owner, parse(L""), false, 96);
            editor.verify();
        }
        DestroyWindow(owner);
        std::filesystem::remove_all(folder);
        std::cout << "PASS: insertion, validation, safe saving, and native editor checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        SetFileAttributesW((folder / L"å draft.md").c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove_all(folder);
        return 1;
    }
}
