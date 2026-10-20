#pragma once
#include "editor-tools.h"
#include <iostream>
#include <stdexcept>

inline void goalBugRegressions(const std::filesystem::path &folder) {
    using namespace vm;
    unsigned failures = 0;
    auto check = [&](bool ok, const char *name) {
        if (!ok) {
            std::cerr << "GOAL AUDIT: " << name << '\n';
            ++failures;
        }
    };
    auto next = continueMarkdown(L"1. [x] done", 11);
    check(next && next->replacement == L"\n2. [ ] ", "001 ordered tasks continue unchecked");
    next = continueMarkdown(L"1.   item", 9);
    check(next && next->replacement == L"\n2.   ", "002 ordered lists retain marker spacing");
    check(rebaseLocalLinks(L"[download](?download=1)", folder / L"source.md", folder / L"copy.md") ==
              L"[download](source.md?download=1)",
          "003 same-folder Save As preserves query-only link target");
    auto doc = parse(L"[local](other.md#part:one)");
    doc.path = folder / L"source.md";
    check(exportHtml(doc).find("other.md#part:one") != std::string::npos &&
              rebaseLocalLinks(doc.source, doc.path, folder / L"sub/copy.md") ==
                  L"[local](../other.md#part:one)",
          "004 colons in local fragments are not URL schemes");
    const std::wstring table = L"Prose with a | character\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n";
    const std::wstring adjacent = L"Prose with a | character\n| A | B |\n| --- | --- |\n| 1 | 2 |\n";
    check(editTable(table, table.find(L"A"), TableAction::Center).has_value() &&
              editTable(adjacent, adjacent.find(L"A"), TableAction::Center).has_value(),
          "005 table commands find headers after pipe-containing prose");
    auto tableEdit = editTable(adjacent, adjacent.find(L"A"), TableAction::Center);
    check(tableEdit && tableEdit->start == adjacent.find(L"| A") &&
              !editTable(adjacent, 2, TableAction::Center),
          "005 table editing preserves preceding prose");
    auto html = exportHtml(parse(L"Press <kbd>Ctrl</kbd>.\n"));
    check(html.find("<kbd>") != html.npos, "006 HTML export retains keyboard key semantics");
    html = exportHtml(parse(L"> ![alt](image.png)\n\n> ---\n"));
    auto image = html.find("<img"), rule = html.find("<hr");
    auto quotedAt = [&](size_t at) {
        auto open = html.rfind("<blockquote>", at), close = html.rfind("</blockquote>", at);
        return open != html.npos && (close == html.npos || close < open);
    };
    check(image != html.npos && rule != html.npos && quotedAt(image) && quotedAt(rule),
          "007 HTML export retains quotes around images and rules");
    doc = parse(std::wstring(9000, L'a'));
    html = exportHtml(doc);
    auto paragraph = html.find("<p>");
    check(paragraph != html.npos && html.find("<p>", paragraph + 1) == html.npos &&
              html.find(std::string(9000, 'a')) != html.npos,
          "008 HTML export rejoins layout pieces of one paragraph");
    doc = parse(L"# " + std::wstring(9000, L'a'));
    check(doc.blocks.size() > 1 && doc.blocks.front().anchor &&
              *doc.blocks.front().anchor == std::wstring(9000, L'a') && !doc.blocks.back().anchor,
          "009 long heading anchors use the whole title and target its first piece");
    html = exportHtml(doc);
    auto heading = html.find("<h1");
    check(heading != html.npos && html.find("<h1", heading + 1) == html.npos,
          "008 long headings also export as one element");
    doc = parse(L"<h2 id='explicit'>" + std::wstring(9000, L'a') + L"</h2>\n");
    check(doc.blocks.front().anchor && *doc.blocks.front().anchor == L"explicit" && !doc.blocks.back().anchor,
          "009 explicit long-heading identifiers remain on the first piece");
    check(indentMarkdown(L"  \ttext", 0, 7, true).replacement == L"text",
          "010 outdent removes one four-column level of mixed indentation");
    doc = parse(L"prefix\n\n[missing](missing\\(file\\).md)");
    doc.path = folder / L"source.md";
    std::atomic_bool cancel = false;
    auto diagnostics = validateDraft(doc, cancel);
    check(diagnostics.size() == 1 && diagnostics[0].offset == doc.source.find(L"missing\\(") &&
              diagnostics[0].length == std::wstring_view(L"missing\\(file\\).md").size(),
          "011 escaped link diagnostics select the raw destination");
    doc = parse(L"[Jump](#nested)\n\n<table><tr><td><h2 id='nested'>Nested</h2></td></tr></table>\n");
    check(validateDraft(doc, cancel).empty(), "017 heading links inside table cells are valid targets");
    doc = parse(L"<p><br>Text</p>\n");
    check(doc.text == L"\nText\n", "018 leading HTML line breaks retain their blank line");
    doc = parse(L"<p><u>Before <a href='https://example.com'>Link</a> After</u></p>\n");
    auto after = doc.text.find(L"After");
    bool underlined = false;
    visitBlocks(doc.blocks, [&](const Block &block) {
        for (const auto &run : block.runs)
            if (run.start <= after && after < run.start + run.length)
                underlined |= (run.style & Underline) != 0;
        return true;
    });
    check(underlined, "019 closing a link preserves enclosing underline styling");
    doc = parse(L"<ol start=' 3 '><li>Three</li><li value=' +7suffix'>Seven</li><li>Eight</li></ol>\n");
    check(doc.text.find(L"3.  Three") != doc.text.npos && doc.text.find(L"7.  Seven") != doc.text.npos &&
              doc.text.find(L"8.  Eight") != doc.text.npos,
          "020 HTML list numbering accepts integer prefixes and trailing whitespace");
    auto tabFence = scanFenceLine(L"-\t```", {});
    check(tabFence.marker == L'`' && tabFence.indent == 4 &&
              scanFenceLine(L"\t- literal", tabFence).marker == L'`',
          "021 tab-indented list fences protect their code contents");
    auto opened = scanFenceLine(L"```", {});
    check(scanFenceLine(L"```\v", opened).marker == L'`',
          "022 nonblank whitespace cannot close a code fence");
    const std::wstring fencedAfterTable =
        L"| A | B |\n| --- | --- |\n| 1 | 2 |\n```lang|option\nliteral\n```\n";
    auto boundedTable = editTable(fencedAfterTable, 2, TableAction::Center);
    check(boundedTable && boundedTable->start + boundedTable->length == fencedAfterTable.find(L"```") &&
              boundedTable->replacement.find(L"```") == std::wstring::npos,
          "026 table commands do not consume a following code fence with a pipe");
    check(replaceMatches(L"e\u0301 e", L"e", L"x", true, true, cancel) == L"e\u0301 x",
          "027 whole-word replacement preserves combining-mark words");
    check(!editTable(fencedAfterTable, fencedAfterTable.find(L"lang"), TableAction::Center),
          "026 table commands reject a caret in the following fence");
    check(replaceMatches(L"e^ e", L"e", L"x", true, true, cancel) == L"x^ x",
          "027 whole-word replacement retains punctuation boundaries");
    auto outline = sourceOutline(std::wstring(L"# Before\0After\n", 15));
    check(outline.size() == 1 && outline[0].title == L"Before\uFFFDAfter",
          "028 outline replaces embedded nulls instead of truncating labels");
    for (auto markup : {L"[![](image.png)](https://example.com/)\n",
                        L"<a href='https://example.com/'><img src='image.png' alt=''></a>\n"}) {
        doc = parse(markup);
        html = exportHtml(doc);
        bool linkedImage = false;
        visitBlocks(doc.blocks, [&](const Block &block) {
            if (block.kind == Kind::Image)
                for (const auto &run : block.runs)
                    linkedImage |= run.link == L"https://example.com/";
            return true;
        });
        check(linkedImage && html.find("<a href=\"https://example.com/\"><img") != html.npos,
              "029 empty-alt images retain their enclosing link for hit testing and export");
    }
    html = exportHtml(parse(L"<h2>First<br>Second</h2>\n"));
    check(html.find("First<br>Second") != html.npos, "030 exported headings preserve explicit line breaks");
    html = exportHtml(parse(L"<h2><em>First<br>Second</em></h2>\n"));
    check(html.find("First<br>Second") != html.npos, "030 styled heading breaks retain their markup");
    html = exportHtml(parse(L"```\nFirst\nSecond\n```\n"));
    check(html.find("First\nSecond") != html.npos && html.find("<br>") == html.npos,
          "030 code export retains literal newlines");
    const std::wstring nestedTable = L"- item\n\n  | A | B |\n  | --- | --- |\n  | 1 | 2 |\n";
    auto nestedEdit = editTable(nestedTable, nestedTable.find(L"A"), TableAction::AddRow);
    check(nestedEdit && nestedEdit->replacement.starts_with(L"  |") &&
              nestedEdit->replacement.find(L"\n|") == std::wstring::npos,
          "031 table edits retain list-continuation indentation");
    if (nestedEdit) {
        auto changed = nestedTable;
        changed.replace(nestedEdit->start, nestedEdit->length, nestedEdit->replacement);
        doc = parse(changed);
        bool nestedRows = false;
        for (const auto &block : doc.blocks)
            if (block.kind == Kind::Table) {
                nestedRows = true;
                check(block.indent == 1, "031 edited table remains inside its list item");
            }
        check(nestedRows, "031 edited table still parses as a table");
    }
    auto insertedLink =
        insertMarkdown(L"", 0, 0, Insert::Link, 0, 0, L"First\n\nSecond", L"https://example.com/");
    doc = parse(insertedLink.replacement);
    bool multilineLink = false;
    for (const auto &block : doc.blocks)
        for (const auto &run : block.runs)
            multilineLink |= run.link == L"https://example.com/";
    check(multilineLink, "inserting a multiline label produces a working link");
    doc = parse(L"# Before ![middle](image.png) After\n\n# Before middle After\n");
    check(doc.blocks.front().anchor && *doc.blocks.front().anchor == L"before-middle-after",
          "035 image-containing heading anchors include alt text and following text");
    check(doc.blocks.back().anchor && *doc.blocks.back().anchor == L"before-middle-after-1",
          "035 complete image-containing heading titles participate in duplicate numbering");
    doc = parse(L"- # ![Title](image.png)\n\n[go](#title)\n");
    check(doc.blocks.front().anchor && *doc.blocks.front().anchor == L"title" &&
              validateDraft(doc, cancel).empty(),
          "035 image-only heading links resolve without including the list marker");
    doc = parse(L"# ![Title](image.png)\n\n[go](#title)\n");
    html = exportHtml(doc);
    check(html.find("id=\"title\"") != html.npos,
          "035 exporting an image-only heading retains its link destination");
    doc = parse(L"# Ångström École\n\n[go](#ångström-école)\n");
    check(doc.blocks.front().anchor && *doc.blocks.front().anchor == L"ångström-école" &&
              validateDraft(doc, cancel).empty(),
          "036 non-ASCII heading capitals produce lowercase identifiers");
    doc = parse(L"# ΑΛΦΑ\n\n# αλφα\n\n<h2 id='ÅExact'>Explicit</h2>\n");
    check(doc.blocks[0].anchor && *doc.blocks[0].anchor == L"αλφα" && doc.blocks[1].anchor &&
              *doc.blocks[1].anchor == L"αλφα-1" && doc.blocks.back().anchor &&
              *doc.blocks.back().anchor == L"ÅExact",
          "036 Unicode case variants share duplicate numbering while explicit IDs retain their case");
    doc = parse(L"[go](#sample)\n\n<pre id='sample'>code</pre>\n");
    html = exportHtml(doc);
    check(validateDraft(doc, cancel).empty() && html.find("<pre id=\"sample\">") != html.npos,
          "038 code-block anchor destinations survive parsing and HTML export");
    doc = parse(L"<pre id='sample'>" + std::wstring(9000, L'x') + L"</pre>\n\n# sample\n");
    html = exportHtml(doc);
    const auto codeId = html.find("id=\"sample\"");
    check(codeId != html.npos && html.find("id=\"sample\"", codeId + 1) == html.npos &&
              doc.blocks.back().anchor && *doc.blocks.back().anchor == L"sample-1",
          "038 chunked code keeps one anchor and reserves its identifier against generated headings");
    const std::wstring headedTable =
        L"# Before | table\n| A | B |\n| --- | --- |\n| 1 | 2 |\n# After | table\n";
    auto headingBoundary = editTable(headedTable, headedTable.find(L"| A") + 2, TableAction::Center);
    check(headingBoundary && headingBoundary->start == headedTable.find(L"| A") &&
              headingBoundary->start + headingBoundary->length == headedTable.find(L"# After"),
          "026 table edits preserve adjacent pipe-containing headings");
    const std::wstring hashCells = L"A | B\n--- | ---\n#tag | value\n####### text | value\n";
    auto hashEdit = editTable(hashCells, 0, TableAction::Center);
    check(hashEdit && hashEdit->length == hashCells.size() &&
              hashEdit->replacement.find(L"#tag") != std::wstring::npos,
          "026 hash-prefixed table cells that are not headings remain editable");
    check(rebaseLocalLinks(L"<a href='notes.md'>Notes</a><img src=\"image.png\">\n", folder / L"source.md",
                           folder / L"sub/copy.md") ==
              L"<a href='../notes.md'>Notes</a><img src=\"../image.png\">\n",
          "039 Save As rebases relative HTML links and images");
    auto pictureRebased =
        rebaseLocalLinks(L"<picture>\n<source srcset='dark.png 1x'>\n<IMG\n SRC='a&amp;b.png'>\n</picture>",
                         folder / L"source.md", folder / L"sub/copy.md");
    check(pictureRebased ==
              L"<picture>\n<source srcset='../dark.png 1x'>\n<IMG\n SRC='../a%26b.png'>\n</picture>",
          "039 Save As preserves supported picture sources and multiline entity-bearing attributes");
    const std::wstring literalHtml =
        L"<!-- <img src='comment.png'> -->\n\n<script>\nlet s = \"<img src='script.png'>\";\n</script>\n\n"
        L"```html\n<img src='code.png'>\n```\n\n`<img src='inline.png'>`\n\n"
        L"<![CDATA[<img src='data.png'>]]>\n";
    check(rebaseLocalLinks(literalHtml, folder / L"source.md", folder / L"sub/copy.md") == literalHtml,
          "039 Save As leaves HTML comments, raw script text, and code examples unchanged");
    check(rebaseLocalLinks(L"<a href='?x=1&amp;y=2'>Q</a><img src=image.png>", folder / L"source.md",
                           folder / L"copy.md") ==
              L"<a href='source.md?x=1&amp;y=2'>Q</a><img src=image.png>",
          "039 same-folder HTML query targets preserve the original document");
    check(rebaseLocalLinks(L"> <img\n> src='image.png'>\n", folder / L"source.md", folder / L"sub/copy.md") ==
              L"> <img\n> src='../image.png'>\n",
          "039 Save As preserves multiline HTML inside blockquotes");
    check(rebaseLocalLinks(L"- <img\n  src='image.png'>\n", folder / L"source.md", folder / L"sub/copy.md") ==
              L"- <img\n  src='../image.png'>\n",
          "039 Save As preserves list prefixes around multiline HTML");
    auto normalEmpty = insertMarkdown(L"", 0, 0, Insert::Heading, 0);
    check(normalEmpty.replacement.empty() && normalEmpty.selection == 0 && normalEmpty.selected == 0,
          "041 normal-text command does not insert a heading placeholder into an empty document");
    auto normalBlank = insertMarkdown(L"Before\n\nAfter", 7, 7, Insert::Heading, 0);
    check(normalBlank.start == 7 && normalBlank.length == 0 && normalBlank.replacement.empty(),
          "041 normal-text command preserves an empty line between paragraphs");
    check(insertMarkdown(L"", 0, 0, Insert::Heading, 2).replacement == L"## Heading",
          "041 actual heading insertion retains its placeholder");
    check(insertMarkdown(L"> # Title", 5, 5, Insert::Heading, 0).replacement == L"> Title" &&
              insertMarkdown(L"> > Title", 5, 5, Insert::Heading, 2).replacement == L"> > ## Title",
          "042 heading commands operate inside existing blockquotes");
    auto quotedHeading = insertMarkdown(L"> > Title", 5, 5, Insert::Heading, 2);
    doc = parse(quotedHeading.replacement);
    check(doc.blocks.front().kind == Kind::Heading && doc.blocks.front().level == 2 &&
              doc.blocks.front().quote == 2,
          "042 heading conversion retains nested quote depth");
    check(insertMarkdown(L">\t  # literal", 8, 8, Insert::Heading, 0).replacement == L">\t  # literal",
          "042 normal-text conversion does not strip a code literal after tab-indented quote padding");
    for (auto thematic : {L"* * *", L"- - -", L"*  **"}) {
        doc = parse(thematic);
        check(doc.blocks.front().kind == Kind::Rule, "044 fixture parses as a thematic break");
        check(!continueMarkdown(thematic, std::wstring_view(thematic).size()),
              "044 Enter after a thematic break does not invent a list item");
    }
    auto quotedRule = continueMarkdown(L"> * * *", 7);
    check(quotedRule && quotedRule->replacement == L"\n> ",
          "044 Enter after a quoted thematic break retains only the quote prefix");
    doc = parse(L"<ol reversed><li>First</li><li>Second</li><li>Third</li></ol>");
    check(doc.text.find(L"3.  First") != doc.text.npos && doc.text.find(L"2.  Second") != doc.text.npos &&
              doc.text.find(L"1.  Third") != doc.text.npos,
          "046 reversed HTML lists count down from the number of direct items");
    doc = parse(L"<ol reversed start='7'><li>First</li><li value='3'>Reset</li><li>Last</li></ol>");
    check(doc.text.find(L"7.  First") != doc.text.npos && doc.text.find(L"3.  Reset") != doc.text.npos &&
              doc.text.find(L"2.  Last") != doc.text.npos,
          "046 reversed HTML lists respect explicit starts and value resets");
    doc = parse(L"<ol reversed='false' start='invalid'><li>Outer<ol><li>Inner</li></ol></li>"
                L"<li>End</li></ol>");
    check(doc.text.find(L"2.  Outer") != doc.text.npos && doc.text.find(L"1.  Inner") != doc.text.npos &&
              doc.text.find(L"1.  End") != doc.text.npos,
          "046 boolean reversed attributes count direct children and preserve nested forward lists");
    doc = parse(L"<ol reversed start='-2147483648'><li>Min</li><li>Next</li></ol>");
    check(doc.text.find(L"-2147483649.  Next") != doc.text.npos,
          "046 reversed list counters do not wrap at the signed HTML attribute boundary");
    auto htmlBreakOutline = sourceOutline(L"# First<br>Second\n\n## Third<BR class='break'/>Fourth\n");
    check(htmlBreakOutline.size() == 2 && htmlBreakOutline[0].title == L"First Second" &&
              htmlBreakOutline[1].title == L"Third Fourth",
          "050 heading outline retains word boundaries at inline HTML line breaks");
    auto literalBreakOutline = sourceOutline(L"# `First<br>Second` and A<strong>B</strong>\n");
    check(literalBreakOutline.size() == 1 && literalBreakOutline[0].title == L"First<br>Second and AB",
          "050 outline preserves literal code and does not add spaces for formatting tags");
    doc = parse(L"# First<br>Second\n\n[go](#first-second)\n");
    check(doc.blocks.front().anchor && *doc.blocks.front().anchor == L"first-second" &&
              validateDraft(doc, cancel).empty(),
          "051 heading line breaks produce valid whitespace-free link identifiers");
    check(exportHtml(doc).find("id=\"first-second\"") != std::string::npos,
          "051 exported heading identifiers retain normalized line-break boundaries");
    doc = parse(L"# First\tSecond\n\n[go](#first-second)\n");
    check(doc.blocks.front().anchor && *doc.blocks.front().anchor == L"first-second" &&
              validateDraft(doc, cancel).empty(),
          "051 heading tabs use the same separator as spaces");
    doc = parse(L"# First<br>Second\n\n# First Second\n\n<h2 id='Explicit-ID'>First<br>Second</h2>\n");
    check(doc.blocks[1].anchor && *doc.blocks[1].anchor == L"first-second-1" && doc.blocks.back().anchor &&
              *doc.blocks.back().anchor == L"Explicit-ID",
          "051 normalized titles share duplicate numbering while explicit identifiers stay unchanged");
    const std::wstring attributePipeTable = L"| <span title='a|b'>A</span> | B |\n| --- | --- |\n| 1 | 2 |\n";
    doc = parse(attributePipeTable);
    check(!doc.blocks.empty() && doc.blocks.front().kind == Kind::Table &&
              doc.blocks.front().cells.size() == 2,
          "052 table renderer ignores pipes inside inline HTML attributes");
    auto attributePipeEdit =
        editTable(attributePipeTable, attributePipeTable.find(L"B |"), TableAction::Right);
    check(attributePipeEdit && attributePipeEdit->replacement.find(L"| --- | ---: |") != std::wstring::npos,
          "052 table editing uses the rendered column boundaries around inline HTML");
    for (auto firstCell :
         {L"Å\U0001F600 <span title='> | `'>A</span>", L"A <!-- x|y -->", L"`<span title='a|b'>`"}) {
        auto source = std::wstring(L"| ") + firstCell + L" | B |\n| --- | --- |\n| 1 | 2 |\n";
        auto edit = editTable(source, source.find(L"B |"), TableAction::DeleteColumn);
        check(edit && edit->replacement.find(firstCell) != std::wstring::npos &&
                  edit->replacement.find(L" B |") == std::wstring::npos,
              "052 HTML-aware table edits preserve Unicode, comments, quoted delimiters, and code");
    }
    for (auto firstCell :
         {L"[A](https://example.test/a|b)", L"[A](target.md \"a|b\")", L"<https://example.test/a|b>",
          L"[<span title='x|y'>Å</span>](target.md \"a|b\")"}) {
        auto source = std::wstring(L"| ") + firstCell + L" | B |\n| --- | --- |\n| 1 | 2 |\n";
        doc = parse(source);
        check(!doc.blocks.empty() && doc.blocks.front().kind == Kind::Table &&
                  doc.blocks.front().cells.size() == 2,
              "052 renderer accepts pipes inside link destinations and titles");
        auto edit = editTable(source, source.find(L"B |"), TableAction::Right);
        check(edit && edit->replacement.find(L"| --- | ---: |") != std::wstring::npos,
              "052 table commands preserve link destination and title boundaries");
    }
    const std::wstring imageTitleTable = L"| ![A](image.png \"a|b\") | B |\n| --- | --- |\n| 1 | 2 |\n";
    auto imageTitleEdit = editTable(imageTitleTable, imageTitleTable.find(L"B |"), TableAction::Right);
    check(imageTitleEdit && imageTitleEdit->replacement.find(L"| --- | ---: |") != std::wstring::npos,
          "052 table editing protects image title delimiters");
    doc = parse(L"| Image | Text |\n| --- | --- |\n| Before ![Alt](image.png) After | Neighbor |\n");
    check(doc.blocks.size() == 2 && doc.blocks[0].kind == Kind::Table && doc.blocks[1].kind == Kind::Table &&
              doc.blocks[1].cells.size() == 2 && doc.blocks[1].cells[0].content,
          "053 Markdown images remain inside their table cell without destroying the row");
    html = exportHtml(doc);
    auto imageAt = html.find("<img ");
    auto neighborAt = html.find("Neighbor");
    check(imageAt != html.npos && neighborAt != html.npos && html.rfind("<td", imageAt) != html.npos &&
              html.find("</td>", imageAt) < neighborAt && html.find("</table>", imageAt) > neighborAt,
          "053 exported images and neighboring cells retain their table structure");
    doc = parse(L"| [![](image.png)](https://example.com/) | Heading |\n| --- | --- |\n| X | Y |\n\n"
                L"| <img src='image.png'> | Other |\n| --- | --- |\n| Z | W |\n");
    html = exportHtml(doc);
    auto firstTable = html.find("<table>");
    check(firstTable != html.npos && html.find("<table>", firstTable + 1) != html.npos &&
              doc.blocks.front().cells.front().content && doc.blocks.front().cells.front().header &&
              html.find("<a href=\"https://example.com/\"><img") != html.npos,
          "053 image headers retain header status, empty-alt links, and separate exported tables");
    doc = parse(L"| A | B |\n| --- | --- |\n| " + std::wstring(9000, L'x') +
                L" ![Alt](image.png) | Keep neighbor |\n");
    bool retainedNeighbor = false, retainedImage = false, boundedChildren = true;
    visitBlocks(doc.blocks, [&](const Block &block) {
        retainedNeighbor |=
            doc.text.substr(block.start, block.length).find(L"Keep neighbor") != std::wstring::npos;
        retainedImage |= block.kind == Kind::Image && block.image;
        boundedChildren &= block.kind == Kind::Table || block.length <= 8192;
        return true;
    });
    check(retainedNeighbor && retainedImage && boundedChildren,
          "053 oversized rich rows retain all cells and bounded text chunks");
    doc = parse(L"| A | B |\n| --- | --- |\n| Plain | Text |\n");
    check(!doc.blocks.front().cells.front().content && !doc.blocks.back().cells.front().content,
          "053 ordinary Markdown tables retain the plain-cell path");
    doc = parse(L"![Outer ![Inner](inner.png) Tail](outer.png)\n");
    check(doc.blocks.size() == 1 && doc.blocks.front().kind == Kind::Image && doc.blocks.front().image &&
              doc.blocks.front().image->url == L"outer.png" &&
              doc.text.substr(doc.blocks.front().start, doc.blocks.front().length) == L"Outer Inner Tail",
          "054 nested image labels remain plain alt text of the outer image");
    doc = parse(L"![A [label](https://inner.example/) <b>B</b>](outer.png)\n");
    html = exportHtml(doc);
    check(doc.blocks.size() == 1 && doc.blocks.front().image &&
              html.find("alt=\"A label &lt;b&gt;B&lt;/b&gt;\"") != html.npos &&
              html.find("href=\"https://inner.example/\"") == html.npos,
          "054 alt-label formatting and links remain plain text");
    doc = parse(L"[![A <b>B</b>](outer.png)](https://outer.example/)\n");
    html = exportHtml(doc);
    check(doc.blocks.size() == 1 && html.find("href=\"https://outer.example/\"") != html.npos &&
              html.find("alt=\"A &lt;b&gt;B&lt;/b&gt;\"") != html.npos,
          "054 linked images retain their outer hyperlink around literal alt HTML");
    doc = parse(L"![A ![B ![C](third.png) D](second.png) E  \nF](first.png)\n\nAfter");
    check(doc.blocks.size() == 2 && doc.blocks.front().image &&
              doc.blocks.front().image->url == L"first.png" &&
              doc.text.substr(doc.blocks.front().start, doc.blocks.front().length) == L"A B C D E F" &&
              doc.blocks.back().kind == Kind::Paragraph,
          "054 nested alt depth and hard breaks do not leak into following document content");
    auto altOutline = sourceOutline(L"# ![A <b>B</b><br>C](image.png)\n");
    check(altOutline.size() == 1 && altOutline.front().title == L"A <b>B</b><br>C",
          "054 heading outlines retain literal HTML in image alt labels");
    doc = parse(L"![A ![B](missing-inner.png) [C](missing-inner.md)](https://example.com/outer.png)\n");
    doc.path = folder / L"source.md";
    check(validateDraft(doc, cancel).empty(),
          "054 validation does not check unused destinations inside image alt text");
    doc = parse(L"<div><script>const example = \"<img src='literal.png'>\";</script>"
                L"<style>/* <h1>Not a heading</h1> */</style><p>After</p></div>");
    bool rawImage = false, rawHeading = false;
    visitBlocks(doc.blocks, [&](const Block &block) {
        rawImage |= block.kind == Kind::Image;
        rawHeading |= block.kind == Kind::Heading;
        return true;
    });
    check(!rawImage && !rawHeading && doc.text.find(L"<img src='literal.png'>") != doc.text.npos,
          "055 literal HTML inside raw-text elements cannot create images or headings");
    for (auto tag : {L"script", L"style", L"xmp", L"iframe", L"noembed", L"noframes"}) {
        doc = parse(std::wstring(L"<div><") + tag + L">&lt;img&gt; &amp; <img src='literal.png'></" + tag +
                    L"><h2>Actual heading</h2></div>");
        unsigned images = 0, headings = 0;
        visitBlocks(doc.blocks, [&](const Block &block) {
            images += block.kind == Kind::Image;
            headings += block.kind == Kind::Heading;
            return true;
        });
        check(!images && headings == 1 && doc.text.find(L"&lt;img&gt; &amp;") != doc.text.npos,
              "055 raw-text serialization preserves literal entities and resumes after the element");
    }
    doc = parse(L"<div><textarea>&lt;b&gt;literal&lt;/b&gt;</textarea><p>After &amp; text</p></div>");
    check(doc.text.find(L"<b>literal</b>") != doc.text.npos &&
              doc.text.find(L"After & text") != doc.text.npos,
          "055 ordinary escaped HTML text retains its existing entity decoding");
    const std::wstring shortRowTable = L"| A | B |\n| --- | --- |\nSingle\n| X | Y |\n\nAfter\n";
    doc = parse(shortRowTable);
    check(doc.blocks.size() == 4 && doc.blocks[1].kind == Kind::Table && doc.blocks[1].cells.size() == 2 &&
              doc.text.substr(doc.blocks[1].cells[0].start, doc.blocks[1].cells[0].length) == L"Single",
          "056 pipe-free table body rows are rendered as padded table cells");
    auto shortRowEdit = editTable(shortRowTable, shortRowTable.find(L"Single"), TableAction::DeleteRow);
    check(shortRowEdit && shortRowEdit->replacement.find(L"Single") == std::wstring::npos &&
              shortRowEdit->replacement.find(L"| X | Y |") != std::wstring::npos &&
              shortRowEdit->length == shortRowTable.find(L"\nAfter"),
          "056 table commands include pipe-free body rows without swallowing following paragraphs");
    auto wholeTableEdit = editTable(shortRowTable, 2, TableAction::AddColumn);
    check(wholeTableEdit && wholeTableEdit->replacement.find(L"| Single | Cell |  |") != std::wstring::npos &&
              wholeTableEdit->replacement.find(L"| X | Cell | Y |") != std::wstring::npos,
          "056 column edits update rows on both sides of a pipe-free body row");
    for (auto boundary : {L"\nAfter", L"---\nAfter", L"- Item\n", L"> Quote\n", L"```\ncode\n```\n"}) {
        std::wstring source = L"Before\n| A | B |\n| --- | --- |\nSingle\n";
        auto end = source.size();
        source += boundary;
        auto edit = editTable(source, source.find(L"Single"), TableAction::Center);
        check(edit && edit->start == source.find(L"| A") && edit->start + edit->length == end &&
                  edit->replacement.find(L"| Single |  |") != std::wstring::npos,
              "056 table edits preserve preceding prose and following block boundaries");
    }
    check(!editTable(shortRowTable, shortRowTable.find(L"After"), TableAction::AddRow),
          "056 commands outside the table remain inactive");
    check(rebaseLocalLinks(L"![A ![B](inner.png) <img src='literal.png'>](outer.png)", folder / L"source.md",
                           folder / L"sub/copy.md") ==
              L"![A ![B](inner.png) <img src='literal.png'>](../outer.png)",
          "054 Save As preserves literal nested destinations and HTML inside image alt text");
    check(rebaseLocalLinks(
              L"![A ![B ![C](third.png)](inner.png) [D](literal.md)](https://example.com/a.png)\n\n"
              L"[Real](notes.md) <img src='real.png'>",
              folder / L"source.md", folder / L"sub/copy.md") ==
              L"![A ![B ![C](third.png)](inner.png) [D](literal.md)](https://example.com/a.png)\n\n"
              L"[Real](../notes.md) <img src='../real.png'>",
          "054 Save As exits nested alt context before subsequent real links and images");
    check(rebaseLocalLinks(L"[![A <img src='literal.png'>](outer.png)](notes.md)", folder / L"source.md",
                           folder / L"sub/copy.md") ==
              L"[![A <img src='literal.png'>](../outer.png)](../notes.md)",
          "054 Save As rebases both real destinations of linked images");
    auto setextEdit = insertMarkdown(L"Title\n=====\n\nAfter", 2, 2, Insert::Heading, 0);
    auto setextResult = std::wstring(L"Title\n=====\n\nAfter");
    setextResult.replace(setextEdit.start, setextEdit.length, setextEdit.replacement);
    check(setextResult == L"Title\n\nAfter",
          "058 Normal text removes a Setext underline belonging to the edited heading");
    for (auto source : {L"Title\n---", L"Title\n===", L"Title\n  ===  "}) {
        auto edit = insertMarkdown(source, std::wstring_view(source).size() - 1,
                                   std::wstring_view(source).size() - 1, Insert::Heading, 2);
        std::wstring result(source);
        result.replace(edit.start, edit.length, edit.replacement);
        check(result == L"## Title", "058 heading changes from the underline remove the old delimiter");
    }
    for (auto source : {L"> Title\n> ===\n", L"First\nSecond\n---\n"}) {
        auto edit = insertMarkdown(source, 2, 2, Insert::Heading, 0);
        std::wstring result(source);
        result.replace(edit.start, edit.length, edit.replacement);
        check(result == (source[0] == L'>' ? L"> Title\n" : L"First\nSecond\n"),
              "058 quoted and multiline Setext headings become ordinary text");
    }
    for (auto source : {L"Title\n\n---\n", L"```\nTitle\n===\n```\n"}) {
        auto caret = std::wstring_view(source).find(L"Title");
        auto edit = insertMarkdown(source, caret, caret, Insert::Heading, 0);
        check(edit.replacement.find(L'\n') == std::wstring::npos,
              "058 heading conversion does not consume separate rules or code lines");
    }
    for (auto title : {L"**Title**", L"[Title](notes.md)", L"![Title](image.png)", L"![](image.png)",
                       L"`Title`", L"&#65;"}) {
        std::wstring source = std::wstring(title) + L"\n===\n";
        auto edit = insertMarkdown(source, 1, 1, Insert::Heading, 0);
        source.replace(edit.start, edit.length, edit.replacement);
        check(source == std::wstring(title) + L"\n",
              "058 formatted heading content is preserved when its underline is removed");
    }
    const std::wstring referenceHeading = L"å😀 Before\n\n![][pic]\n===\n\n[pic]: image.png\n";
    auto referenceHeadingEdit = insertMarkdown(referenceHeading, referenceHeading.find(L"!["),
                                               referenceHeading.find(L"!["), Insert::Heading, 0);
    auto referenceHeadingResult = referenceHeading;
    referenceHeadingResult.replace(referenceHeadingEdit.start, referenceHeadingEdit.length,
                                   referenceHeadingEdit.replacement);
    check(referenceHeadingResult == L"å😀 Before\n\n![][pic]\n\n[pic]: image.png\n",
          "058 empty reference-image headings retain definitions and Unicode source offsets");
    auto severalHeadings = insertMarkdown(L"One\n===\n\nTwo\n---\n", 0, 17, Insert::Heading, 0);
    std::wstring severalResult = L"One\n===\n\nTwo\n---\n";
    severalResult.replace(severalHeadings.start, severalHeadings.length, severalHeadings.replacement);
    check(severalResult == L"One\n\nTwo\n",
          "058 a selection spanning multiple headings removes each underline exactly once");
    const std::wstring imageHeadingSource =
        L"å😀 Before\n\n![](image.png)\nTitle\n===\n\n#\n\n## ![](image.png)\n";
    auto imageHeadingOutline = sourceOutline(imageHeadingSource);
    check(imageHeadingOutline.size() == 3 &&
              imageHeadingOutline[0].offset == imageHeadingSource.find(L"![]") &&
              imageHeadingOutline[1].offset == imageHeadingSource.find(L"#\n") &&
              imageHeadingOutline[2].offset == imageHeadingSource.find(L"## "),
          "059 outline includes text-free headings and targets the first line of image-led headings");
    const std::wstring emptyHeadingSource = L"#\n\n> ##\n\n- ###\n\n![][pic]\n===\n\n[pic]: image.png\n";
    auto emptyOutline = sourceOutline(emptyHeadingSource);
    check(emptyOutline.size() == 4 && emptyOutline[0].offset == 0 && emptyOutline[1].offset == 3 &&
              emptyOutline[2].offset == 9 && emptyOutline[3].offset == emptyHeadingSource.find(L"![]") &&
              emptyOutline[0].title.empty() && emptyOutline[3].title.empty(),
          "059 empty nested and reference-image headings retain source positions without title text");
    auto tabOutline = sourceOutline(L"# Before\tAfter\n\n## `Code\tTab`\n\n# Entity&#9;Tab\n");
    check(tabOutline.size() == 3 && tabOutline[0].title == L"Before After" &&
              tabOutline[1].title == L"Code Tab" && tabOutline[2].title == L"Entity Tab",
          "060 heading tabs stay in the outline label instead of becoming keyboard-shortcut badges");
    const std::wstring headingRule = L"# Title\n---\nAfter";
    auto headingRuleEdit = insertMarkdown(headingRule, 2, 2, Insert::Heading, 0);
    auto headingRuleResult = headingRule;
    headingRuleResult.replace(headingRuleEdit.start, headingRuleEdit.length, headingRuleEdit.replacement);
    auto headingRuleDoc = parse(headingRuleResult);
    check(headingRuleDoc.blocks.size() == 3 && headingRuleDoc.blocks[0].kind == Kind::Paragraph &&
              headingRuleDoc.blocks[1].kind == Kind::Rule,
          "061 Normal text preserves the following horizontal rule instead of creating a Setext heading");
    for (auto source : {L"# Title\n---\n", L"> # Title\n> ---\n", L"Title\n===\n---\n"}) {
        for (bool selectAll : {false, true}) {
            std::wstring result(source);
            auto edit =
                insertMarkdown(result, selectAll ? 0 : 2, selectAll ? result.size() : 2, Insert::Heading, 0);
            result.replace(edit.start, edit.length, edit.replacement);
            auto parsed = parse(result);
            check(parsed.blocks.size() == 2 && parsed.blocks[0].kind == Kind::Paragraph &&
                      parsed.blocks[1].kind == Kind::Rule,
                  "061 selected, quoted, and Setext conversions retain the following rule");
        }
    }
    for (auto source : {L"# Title\n- - -\n", L"# Title\n***\n", L"# Title\nText\n"}) {
        auto edit = insertMarkdown(source, 2, 2, Insert::Heading, 0);
        check(edit.replacement == L"Title", "061 unambiguous following blocks need no extra separator");
    }
    for (auto label : {L"First\n\nSecond", L"First\n# Second"}) {
        auto insertion = insertMarkdown(L"", 0, 0, Insert::Link, 0, 0, label, L"https://example.com/");
        auto linked = parse(insertion.replacement);
        check(linked.blocks.size() == 1 && !linked.blocks.front().runs.empty() &&
                  linked.blocks.front().runs.front().link == L"https://example.com/" &&
                  linked.text.find(L"First") != linked.text.npos &&
                  linked.text.find(L"Second") != linked.text.npos,
              "064 multiline inserted labels cannot escape their link into separate Markdown blocks");
    }
    for (auto kind : {Insert::Link, Insert::Image}) {
        const std::wstring label = L"A\r\n\r\n# B & &#10; [C]";
        auto insertion = insertMarkdown(L"", 0, 0, kind, 0, 0, label, L"https://example.com/a.png");
        auto parsed = parse(insertion.replacement);
        check(parsed.blocks.size() == 1 &&
                  parsed.text.substr(parsed.blocks[0].start, parsed.blocks[0].length) == editText(label),
              "064 encoded label newlines preserve CRLF text, punctuation, and literal entity syntax");
    }
    auto breakHeading = parse(L"# First<br><br>Second\n");
    auto headingLabel = breakHeading.text.substr(breakHeading.blocks[0].start, breakHeading.blocks[0].length);
    auto headingLink = parse(
        insertMarkdown(L"", 0, 0, Insert::Link, 0, 0, headingLabel, L"#" + *breakHeading.blocks[0].anchor)
            .replacement);
    check(headingLink.blocks.size() == 1 && !headingLink.blocks[0].runs.empty() &&
              headingLink.blocks[0].runs[0].link == L"#" + *breakHeading.blocks[0].anchor,
          "064 Link to heading retains links for headings with consecutive explicit breaks");
    if (failures)
        throw std::runtime_error(std::to_string(failures) + " goal audit regressions");
}
