#include "document.h"
#include "image-layout.h"
#include "md4c-html.h"
#include "search.h"
#include "table-layout.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <psapi.h>
#include <stdexcept>
#include <thread>
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
        {
            std::wstring source = L"# Å \U0001f642\n\nBefore.\n\n![](large.png)\n\n"
                                  L"<div>\n<p>Caption</p>\n<img src='large.png'>\n"
                                  L"<p>After HTML image</p>\n</div>\n\n## After\n\nTail.\n";
            auto mapped = parse(source, nullptr, 0xffffffff, true);
            require(mapped.sourceStarts.size() == mapped.blocks.size(), "preview source map size");
            require(std::is_sorted(mapped.sourceStarts.begin(), mapped.sourceStarts.end()),
                    "preview source anchors are monotone");
            for (size_t i = 0; i < mapped.blocks.size(); ++i) {
                auto &b = mapped.blocks[i];
                auto rendered = mapped.text.substr(b.start, b.length);
                if (b.kind == Kind::Heading && b.level == 2)
                    require(mapped.sourceStarts[i] == source.find(L"## After"),
                            "heading stays aligned after Markdown and HTML images");
                if (rendered == L"After HTML image")
                    require(mapped.sourceStarts[i] == source.find(L"<p>After HTML"),
                            "individual HTML blocks have source anchors");
                if (b.kind == Kind::Image)
                    require(mapped.sourceStarts[i] == source.find(L"![]") ||
                                mapped.sourceStarts[i] == source.find(L"<img"),
                            "empty-alt image source anchor");
            }
            require(parse(source).sourceStarts.empty(), "reader does not collect editor source maps");
            auto reference =
                parse(L"![][picture]\n\n## Following\n\n[picture]: image.png\n", nullptr, 0xffffffff, true);
            for (size_t i = 0; i < reference.blocks.size(); ++i) {
                if (reference.blocks[i].kind == Kind::Image)
                    require(reference.sourceStarts[i] == 0, "reference image source location");
                if (reference.blocks[i].kind == Kind::Heading)
                    require(reference.sourceStarts[i] == 14, "heading after a reference image");
            }
            std::wstring fragments =
                L"<DIV>\n<table><tr><td>First</table>\n<p>After repaired table</p>\n</DIV>\n\n"
                L"<article>\n<p>After second fragment</p>\n</article>\n";
            auto repaired = parse(fragments, nullptr, 0xffffffff, true);
            for (size_t i = 0; i < repaired.blocks.size(); ++i) {
                auto &b = repaired.blocks[i];
                auto rendered = repaired.text.substr(b.start, b.length);
                if (rendered == L"After repaired table" || rendered == L"After second fragment")
                    require(repaired.sourceStarts[i] == fragments.find(L"<p>" + rendered),
                            "indexed anchors survive repaired tags and separate HTML fragments");
            }
        }
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
        {
            std::wstring comment = L"<!--\n<p>Hidden example</p>\n";
            for (unsigned i = 0; i < 6000; ++i)
                comment += L"Hidden Å \U0001f642\n";
            comment += L"-->\n";
            for (auto prefix :
                 {L"<div>\n" + comment, std::wstring(L"<div data-example=\"\n<p>Hidden attribute</p>\n\">\n"),
                  L"<!--" + std::wstring(65524, L'x') + L"-->\n<section>\n"}) {
                auto source = prefix + L"<p>Visible paragraph</p>\n" +
                              (prefix.ends_with(L"<section>\n") ? L"</section>\n" : L"</div>\n");
                auto mapped = parse(source, nullptr, 0xffffffff, true);
                validate(mapped);
                require(mapped.sourceStarts.size() == mapped.blocks.size(), "token source map size");
                auto found = std::find_if(mapped.blocks.begin(), mapped.blocks.end(), [&](const Block &b) {
                    return mapped.text.substr(b.start, b.length) == L"Visible paragraph";
                });
                require(found != mapped.blocks.end(), "visible HTML paragraph survives token indexing");
                require(mapped.sourceStarts[found - mapped.blocks.begin()] == source.find(L"<p>Visible"),
                        "source anchors ignore comments and quoted markup across UTF-8 chunks");
                require(mapped.text == parse(source).text,
                        "source token observation preserves reader content");
            }
        }
        {
            // Each group produces an independent rendering band. Traversing all
            // groups per band would make an otherwise modest table quadratic.
            std::wstring source = L"<table>\n";
            std::vector<uint32_t> rows;
            for (unsigned i = 0; i < 40000; ++i) {
                rows.push_back((uint32_t)source.size());
                source += L"<tbody><tr><td colspan='2'>x</td></tr></tbody>\n";
            }
            source += L"</table>\n";
            for (bool sourceMap : {false, true}) {
                auto begin = std::chrono::steady_clock::now();
                auto table = parse(source, nullptr, 0xffffffff, sourceMap);
                auto elapsed = std::chrono::steady_clock::now() - begin;
                require(elapsed < std::chrono::seconds(1),
                        "many table row groups parse without quadratic scans");
                validate(table);
                require(table.blocks.size() == rows.size(), "all table rendering bands retained");
                for (size_t i = 0; i < table.blocks.size(); ++i) {
                    auto &b = table.blocks[i];
                    require(b.table && b.table->rowOffset == i && b.table->rows == 1 &&
                                b.table->groups.size() == 1 && b.table->groups[0].first == 0 &&
                                b.table->groups[0].rows == 1 && b.table->groups[0].identity == i &&
                                b.cells.size() == 1 && b.cells[0].colSpan == 2,
                            "forward group traversal preserves band grid and group identities");
                    if (sourceMap)
                        require(table.sourceStarts[i] == rows[i], "table bands keep accurate source anchors");
                }
                std::cout << "HTML table (40000 row groups, source map " << sourceMap
                          << "): " << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";
            }
        }
        {
            // Lexbor inserts tbody around every row. Missing source tags must
            // not rescan the remaining fragment on each preview refresh.
            std::wstring source = L"<div>\n";
            std::vector<uint32_t> rows;
            for (unsigned i = 0; i < 4000; ++i) {
                rows.push_back((uint32_t)source.size());
                source += L"<table><tr><td>Row " + std::to_wstring(i) + L"</td></tr></table>\n";
            }
            source += L"</div>\n\n## Following\n\nTail.\n";
            auto begin = std::chrono::steady_clock::now();
            auto mapped = parse(source, nullptr, 0xffffffff, true);
            auto elapsed = std::chrono::steady_clock::now() - begin;
            require(elapsed < std::chrono::seconds(1), "implied HTML tags keep preview mapping fast");
            validate(mapped);
            auto reader = parse(source);
            require(mapped.text == reader.text && mapped.blocks.size() == reader.blocks.size(),
                    "source mapping preserves formatted content");
            require(mapped.sourceStarts.size() == mapped.blocks.size() &&
                        std::is_sorted(mapped.sourceStarts.begin(), mapped.sourceStarts.end()),
                    "large repaired HTML source anchors remain monotone");
            size_t row = 0;
            for (size_t i = 0; i < mapped.blocks.size(); ++i) {
                auto &block = mapped.blocks[i];
                if (block.kind == Kind::Table) {
                    require(row < rows.size() && mapped.sourceStarts[i] == rows[row++],
                            "each table keeps its own source line after implied tbody tags");
                } else if (block.kind == Kind::Heading)
                    require(mapped.sourceStarts[i] == source.find(L"## Following"),
                            "Markdown after indexed HTML keeps its source anchor");
            }
            require(row == rows.size(), "all indexed HTML rows remain visible");
            std::cout << "HTML preview mapping (4000 tables): "
                      << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";
        }
        {
            std::atomic_bool cancel = false;
            const std::wstring text = L"Ångström one åNGSTRÖM two Ångström";
            auto count = countMatches(text, L"ångström", 13, cancel);
            require(count.total == 3 && count.beforeStart == 1, "ordinal Unicode search count");
            require(findMatch(text, L"ångström", 0, false, cancel) == 0, "first Unicode match");
            require(findMatch(text, L"ångström", text.size(), false, cancel) == 0, "forward search wraps");
            require(findMatch(text, L"ångström", 0, true, cancel) == 26, "backward search wraps");
            require(countMatches(L"aaaaa", L"aa", 2, cancel).total == 4 &&
                        findMatch(L"aaaaa", L"aa", 1, false, cancel) == 1,
                    "overlapping matches count and navigate consistently");
            require(countMatches(text, L"missing", 0, cancel).total == 0 &&
                        countMatches(text, L"", 0, cancel).total == 0 &&
                        findMatch(text, L"", 0, false, cancel) == -1,
                    "empty and missing searches");
            cancel = true;
            require(countMatches(text, L"one", 0, cancel).total == 0 &&
                        findMatch(text, L"one", 0, false, cancel) == -1,
                    "cancelled search stops promptly");
            cancel = false;
            std::wstring dense(1024 * 1024, L'a');
            require(findMatch(dense, L"a", 0, false, cancel) == 0 &&
                        countMatches(dense, L"a", 0, cancel).total == dense.size(),
                    "dense results use constant-size count state");
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
        require(std::any_of(
                    d.blocks.begin(), d.blocks.end(),
                    [](auto &b) { return b.kind == Kind::Image && b.image && b.image->url == L"photo.png"; }),
                "image");
        auto sizedImage = parse(L"<img src=\"assets/icon-master.png\" width=\"96\" height=\"96\" "
                                L"alt=\"Voltura Markdown document and lightning icon\">\n");
        validate(sizedImage);
        auto &icon = sizedImage.blocks.front();
        require(icon.kind == Kind::Image && icon.image->width == 96 && icon.image->height == 96 &&
                    imageDisplaySize(icon, 1024, 1024, 800, 1) == std::pair{96.f, 96.f},
                "README icon respects HTML width and height");
        require(imageDisplaySize(icon, 1024, 1024, 800, 1.5f) == std::pair{144.f, 144.f} &&
                    imageDisplaySize(icon, 1024, 1024, 48, 1) == std::pair{48.f, 48.f},
                "explicit image dimensions follow zoom and fit narrow viewports");
        for (auto attributes :
             {L"WIDTH='96'", L"height=48", L"width=' 96 ' height='48'", L"width='&#57;6' height=48"}) {
            auto sized = parse(L"<img src='photo.png' " + std::wstring(attributes) + L">\n");
            require(imageDisplaySize(sized.blocks.front(), 400, 200, 800, 1) == std::pair{96.f, 48.f},
                    "single HTML dimension retains aspect ratio; both dimensions set the size");
        }
        auto stretched = parse(L"<img src='photo.png' width=96 height=96>\n");
        require(imageDisplaySize(stretched.blocks.front(), 400, 200, 800, 1) == std::pair{96.f, 96.f},
                "both dimensions override intrinsic aspect ratio");
        for (auto value :
             {L"0", L"-1", L"96px", L"50%", L"1.5", L"", L"4294967296", L"999999999999999999999999999999"}) {
            auto invalid = parse(L"<img src='photo.png' width='" + std::wstring(value) + L"' height='" +
                                 value + L"'>\n");
            require(invalid.blocks.front().image->width == 0 && invalid.blocks.front().image->height == 0 &&
                        imageDisplaySize(invalid.blocks.front(), 400, 200, 100, 1) == std::pair{100.f, 50.f},
                    "invalid dimensions fall back to intrinsic size with viewport fit");
        }
        require(imageDisplaySize(d.blocks.back(), 40, 20, 800, 1) == std::pair{40.f, 20.f},
                "images without HTML dimensions retain their intrinsic size");
        require(imageDisplaySize(d.blocks.back(), 40, 20, 800, 2) == std::pair{80.f, 40.f},
                "intrinsic image dimensions follow document zoom");
        auto picture =
            parse(L"<p>\n<picture>\n<source media=\"(prefers-color-scheme: dark)\" "
                  L"srcset=\"apps/public-site/assets/voltura-air-host-dark.png\">\n"
                  L"<img src=\"apps/public-site/assets/voltura-air-host.png\" alt=\"Host &amp; pairing\" "
                  L"width=900>\n"
                  L"</picture>\n<br><sub>Windows host pairing screen</sub>\n</p>\n\n![next](next.png)\n");
        validate(picture);
        auto &host = *picture.blocks.front().image;
        require(host.source(false) == L"apps/public-site/assets/voltura-air-host.png" &&
                    host.source(true) == L"apps/public-site/assets/voltura-air-host-dark.png" &&
                    host.width == 900,
                "picture uses theme-specific source and retains img fallback and dimensions");
        require(picture.text.find(L"<picture>") == picture.text.npos &&
                    picture.text.find(L"<source") == picture.text.npos &&
                    picture.text.find(L"<sub>") == picture.text.npos &&
                    picture.text.find(L"Host & pairing") != picture.text.npos &&
                    picture.blocks.back().image->source(true) == L"next.png",
                "picture markup stays hidden and sources do not leak to later images");
        require(std::any_of(picture.blocks.begin(), picture.blocks.end(),
                            [](auto &b) {
                                return std::any_of(b.runs.begin(), b.runs.end(),
                                                   [](auto &r) { return r.style & Subscript; });
                            }),
                "sub captions keep searchable styled text");
        auto nestedSub = parse(L"before <sub>small <b>bold</b></sub> after\n");
        require(nestedSub.text.find(L"small bold") != nestedSub.text.npos &&
                    std::none_of(nestedSub.blocks.front().runs.begin(), nestedSub.blocks.front().runs.end(),
                                 [&](auto &r) {
                                     return (r.style & Subscript) &&
                                            r.start + r.length > nestedSub.text.find(L" after");
                                 }),
                "sub styles restore after nested inline formatting");
        auto firstImage = [](const Document &document) -> const ImageReference & {
            auto found = std::find_if(document.blocks.begin(), document.blocks.end(),
                                      [](auto &b) { return b.image != nullptr; });
            require(found != document.blocks.end(), "picture contains an image");
            return *found->image;
        };
        for (auto source :
             {L"media='(min-width: 900px)' srcset='wide.png'", L"type='image/unsupported' srcset='bad.png'",
              L"srcset='one.png 1x, two.png 2x'", L"srcset=''"}) {
            auto fallback =
                parse(L"<picture><source " + std::wstring(source) + L"><img src='fallback.png'></picture>\n");
            require(firstImage(fallback).source(false) == L"fallback.png" &&
                        firstImage(fallback).source(true) == L"fallback.png",
                    "unsupported source conditions retain img fallback");
        }
        auto orderedSources = parse(
            L"<PICTURE><SOURCE MEDIA=' ( prefers-color-scheme : LIGHT ) ' SRCSET='light.png 1x'>"
            L"<source srcset='first.png'><source srcset='later.png'><img src='fallback.png'></PICTURE>\n");
        require(firstImage(orderedSources).source(false) == L"light.png" &&
                    firstImage(orderedSources).source(true) == L"first.png",
                "first matching source wins for each theme with case-insensitive HTML attributes");
        bool bold = false, italic = false, link = false;
        for (auto &b : d.blocks)
            for (auto &r : b.runs) {
                bold |= (r.style & Bold) != 0;
                italic |= (r.style & Italic) != 0;
                link |= r.link == L"https://example.com";
            }
        require(bold && italic && link, "inline styles");
        for (auto images :
             {L"![Visitors](visitors.svg)", L"[![Visitors](visitors.svg)](https://example.com/visitors)\n"
                                            L"[![Stars](stars.svg)](https://example.com/stars)"}) {
            std::wstring source = L"## Statistics\n\n" + std::wstring(images) + L"\n";
            auto statistics = parse(source, nullptr, 0xffffffff, true);
            validate(statistics);
            require(statistics.blocks.size() == 2 && statistics.blocks[0].kind == Kind::Heading &&
                        statistics.blocks[1].image,
                    "badges immediately follow their heading without an empty paragraph");
            require(statistics.sourceStarts.size() == 2 && statistics.sourceStarts[0] == 0 &&
                        statistics.sourceStarts[1] == source.find(images),
                    "badge spacing fix preserves heading and image source anchors");
        }
        for (auto separator : {L" ", L"\n"}) {
            auto badges =
                parse(L"[![Visitors](visitors.svg)](https://example.com/visitors)" + std::wstring(separator) +
                      L"[![Stars](stars.svg)](https://example.com/stars)\n");
            validate(badges);
            auto row = std::find_if(badges.blocks.begin(), badges.blocks.end(), [](const Block &b) {
                return b.image && b.image->inlineImages != nullptr;
            });
            require(row != badges.blocks.end() && row->image->inlineImages->size() == 2 &&
                        row->runs.size() == 2,
                    "adjacent linked images share a paragraph across spaces and soft breaks");
            require(row->runs[0].link == L"https://example.com/visitors" &&
                        row->runs[1].link == L"https://example.com/stars" &&
                        row->image->inlineImages->front().image->url == L"visitors.svg" &&
                        row->image->inlineImages->back().image->url == L"stars.svg",
                    "inline images retain independent sources and link targets");
        }
        for (auto separator : {L"\n\n", L"  \n", L"<br>", L" text "}) {
            auto separated =
                parse(L"![First](first.svg)" + std::wstring(separator) + L"![Second](second.svg)\n");
            require(std::none_of(separated.blocks.begin(), separated.blocks.end(),
                                 [](const Block &b) { return b.image && b.image->inlineImages != nullptr; }),
                    "paragraphs, hard breaks, and intervening prose keep separate image blocks");
        }
        for (auto source :
             {L"- [Contributing](CONTRIBUTING.md)\n", L"1. [Contributing](CONTRIBUTING.md)\n",
              L"- [ ] [Contributing](CONTRIBUTING.md)\n", L"- parent\n  - [Contributing](CONTRIBUTING.md)\n",
              L"- **[Contributing](CONTRIBUTING.md)**\n"}) {
            auto listed = parse(source);
            validate(listed);
            auto label = listed.text.find(L"Contributing");
            bool foundLink = false;
            for (auto &block : listed.blocks)
                for (auto &run : block.runs)
                    if (!run.link.empty()) {
                        foundLink = true;
                        require(run.link == L"CONTRIBUTING.md" && run.start == label &&
                                    run.length == std::wstring_view(L"Contributing").size(),
                                "list marker and spacing are outside the link");
                    }
            require(foundLink, "list item retains its link");
        }
        // Expected structure follows WHATWG tree construction and default rendering:
        // https://html.spec.whatwg.org/multipage/parsing.html
        // https://html.spec.whatwg.org/multipage/rendering.html
        auto richTable =
            parse(L"<div align='center'><table><tr><td width='34%'><p>First<br>line</p>"
                  L"<picture><source media='(prefers-color-scheme: dark)' srcset='dark.png'>"
                  L"<img src='light.png' alt='Photo' width='320'></picture><br><sub>Caption</sub>"
                  L"<td width='66%'><ul><li>One<li>Two</ul>"
                  L"<table><tr><td>Inner<td>Next</table></table></div>\n");
        validate(richTable);
        require(richTable.blocks.size() == 1 && richTable.blocks.front().cells.size() == 2,
                "HTML5 omitted cell/row ends produce one outer table row");
        auto &richRow = richTable.blocks.front();
        require(richRow.cells[0].width == 34 && richRow.cells[0].percent && richRow.cells[1].width == 66,
                "HTML cell width hints retain their units");
        auto &leftContent = *richRow.cells[0].content;
        require(leftContent.size() == 3 && leftContent[0].align == 2 && leftContent[1].kind == Kind::Image &&
                    leftContent[1].image->width == 320 && leftContent[1].image->source(true) == L"dark.png" &&
                    leftContent[2].runs.front().style & Subscript,
                "cell retains paragraph, theme picture, and caption in order with inherited alignment");
        require(richTable.text.find(L"First\nline") != richTable.text.npos &&
                    richRow.cells[1].content->back().kind == Kind::Table &&
                    richRow.cells[1].content->back().cells.size() == 2,
                "line breaks, lists, and nested HTML tables retain their structure");
        for (auto &cell : richRow.cells)
            for (auto &child : *cell.content)
                require(child.start >= cell.start && child.start + child.length <= cell.start + cell.length,
                        "nested content retains searchable source ranges");
        auto grid = parse(L"<table><tr><th rowspan='2'>Tall</th><td colspan='2'>Wide</td></tr>"
                          L"<tr><td>Left</td><td>Right</td></tr></table>\n");
        validate(grid);
        auto &gridBlock = grid.blocks.front();
        require(grid.blocks.size() == 1 && gridBlock.table && gridBlock.table->rows == 2 &&
                    gridBlock.table->columns == 3 && gridBlock.cells.size() == 4,
                "spanning cells form one sparse HTML table grid");
        require(gridBlock.cells[0].row == 0 && gridBlock.cells[0].column == 0 &&
                    gridBlock.cells[0].rows == 2 && gridBlock.cells[0].header &&
                    gridBlock.cells[1].column == 1 && gridBlock.cells[1].colSpan == 2 &&
                    gridBlock.cells[2].row == 1 && gridBlock.cells[2].column == 1 &&
                    gridBlock.cells[3].column == 2,
                "table cells skip occupied slots and retain combined spans and header semantics");
        auto offsets = tableRowOffsets(gridBlock, {100, 30, 40, 20});
        require(offsets.size() == 3 && offsets[1] == 45 && offsets[2] == 100,
                "rowspan height satisfies both spanning and single-row minimum heights");
        auto groups = parse(L"<table><tfoot><tr><td colspan='2'>Foot</tfoot>"
                            L"<tbody><tr><td rowspan='0'>Group</td><td>A</tr><tr><td>B</tbody>"
                            L"<tbody><tr><td>C</td><td>D</tbody></table>\n");
        auto &grouped = groups.blocks.front();
        require(grouped.table && grouped.table->groups.size() == 3 && grouped.cells[0].row == 3 &&
                    grouped.cells[1].row == 0 && grouped.cells[1].rows == 2 &&
                    grouped.cells[1].rowSpan == 0 && grouped.cells[4].column == 0 &&
                    grouped.table->groups.back().section == TableSection::Foot,
                "rowspan zero stops at its own tbody and footer groups follow body groups");
        for (auto span : {L"", L" colspan='2'"}) {
            std::wstring footer = L"<tfoot><tr><td" + std::wstring(span) + L">Footer å</td></tr></tfoot>\n";
            std::wstring body =
                L"<tbody><tr><td" + std::wstring(span) + L">Body \U0001f642</td></tr></tbody>\n";
            auto source = L"<table>\n" + footer + body + L"</table>\n\n## After footer\n";
            auto mapped = parse(source, nullptr, 0xffffffff, true);
            validate(mapped);
            require(mapped.blocks.size() == 2 && mapped.blocks[0].table && mapped.blocks[0].cells.size() == 2,
                    "footer-first tables share the grid model with or without spans");
            auto &table = mapped.blocks[0];
            require(table.cells[0].row == 1 && table.cells[1].row == 0 &&
                        mapped.text.substr(table.cells[0].start, 8) == L"Footer å" &&
                        mapped.text.substr(table.cells[1].start, 7) == L"Body \U0001f642",
                    "footer moves below body while cell text stays in original source order");
            auto rows = tableRowOffsets(table, {21, 37});
            require(rows[table.cells[0].row] == 37 && rows[table.cells[1].row] == 0,
                    "native footer geometry follows the body with and without spans");
            require(mapped.source == source && mapped.text == parse(source).text &&
                        mapped.sourceStarts.size() == mapped.blocks.size() &&
                        mapped.sourceStarts[0] == source.find(L"<tfoot>") &&
                        mapped.sourceStarts[1] == source.find(L"## After footer") &&
                        blockAtPosition(mapped.blocks, table.cells[0].start) == 0 &&
                        blockAtPosition(mapped.blocks, table.cells[1].start) == 0,
                    "footer reordering preserves source anchors and searchable cell offsets");
        }
        auto alreadyOrdered = parse(L"<table><tbody><tr><td>Body</td></tr></tbody>"
                                    L"<tfoot><tr><td>Footer</td></tr></tfoot></table>\n");
        require(alreadyOrdered.blocks.size() == 2 && !alreadyOrdered.blocks[0].table &&
                    !alreadyOrdered.blocks[1].table,
                "already ordered tables without spans retain incremental row layout");
        auto implicitRows = parse(L"<table><tbody><tr><td rowspan='3'>Three</tbody>"
                                  L"<tbody><tr><td colspan='2'>New group</tbody></table>\n");
        require(implicitRows.blocks.size() == 2 && implicitRows.blocks.front().table->rows == 3 &&
                    implicitRows.blocks.back().table->rowOffset == 3,
                "positive rowspans grow implied rows before the next row group");
        auto overlapping = parse(L"<table><tr><td>A<td rowspan=2>B<tr><td colspan=2>C</table>\n");
        require(overlapping.blocks.front().table->overlap && overlapping.blocks.front().cells.size() == 3,
                "HTML table-model overlaps are recorded without dropping text or shifting author spans");
        for (auto value : {L"", L"bad", L"-1", L"+bad"}) {
            auto d = parse(L"<table><tr><td colspan='" + std::wstring(value) + L"' rowspan='" + value +
                           L"'>X</table>\n");
            require(d.blocks.front().cells.front().colSpan == 1 &&
                        d.blocks.front().cells.front().rowSpan == 1,
                    "missing or invalid span integers default to one");
        }
        auto limits = parse(L"<table><tr><td colspan=' +999999999999999999999tail' "
                            L"rowspan='999999999999999'>Limit</table>\n");
        require(limits.blocks.front().cells[0].colSpan == 1000 &&
                    limits.blocks.front().cells[0].rowSpan == 65534 &&
                    limits.blocks.front().table->rows == 65534 &&
                    limits.blocks.front().table->columns == 1000,
                "HTML span limits saturate without numeric overflow or a dense slot allocation");
        auto columnLimits = tableColumnOffsets(*limits.blocks.front().table, 860, 1);
        require(columnLimits.size() == 1001 && std::abs(columnLimits.back() - 860) < .01f,
                "a large colspan uses logical slots without forcing an artificially wide table");
        auto narrowHints =
            parse(L"<table><tr><td width='99%'>Wide<td>Narrow<tr><td colspan=2>Combined</table>\n");
        auto narrowColumns = tableColumnOffsets(*narrowHints.blocks.front().table, 80, 1);
        require(narrowColumns.size() == 3 && narrowColumns[1] >= 39.99f &&
                    narrowColumns[2] - narrowColumns[1] >= 39.99f,
                "column constraints preserve native cell minima under extreme width hints");
        auto percentHint = parse(L"<table><tr><td rowspan=2 width='34%'>Picture<td>A<tr><td>B</table>\n");
        auto percentColumns = tableColumnOffsets(*percentHint.blocks.front().table, 860, 1);
        require(std::abs(percentColumns[1] - 860 * .34f) < .01f &&
                    std::abs(percentColumns.back() - 860) < .01f,
                "percentage hints reserve the remaining width for automatic columns");
        auto boundedOffsets = tableRowOffsets(limits.blocks.front(), {120});
        require(boundedOffsets.size() == 65535 && std::abs(boundedOffsets.back() - 120) < .01f,
                "maximum rowspans retain bounded sparse height calculation");
        auto prefix = parse(L"<table><tr><td colspan='2suffix' rowspan='-0suffix'>Prefix</table>\n");
        require(prefix.blocks.front().cells[0].colSpan == 2 && prefix.blocks.front().cells[0].rowSpan == 0,
                "span values follow HTML non-negative-integer prefix parsing including negative zero");
        auto captioned = parse(L"<table><caption>Table caption</caption><tr><td colspan=2>Body</table>\n");
        require(captioned.blocks.size() == 1 && captioned.blocks[0].table->captions &&
                    captioned.text.find(L"Table caption") != captioned.text.npos,
                "table captions stay in the model with searchable original text ranges");
        auto nestedSpans = parse(L"<table><tr><td colspan=2><table><tr><td rowspan=2>Inner<tr></table>"
                                 L"<tr><td>Outer A<td>Outer B</table>\n");
        require(nestedSpans.blocks.front().table->columns == 2 &&
                    nestedSpans.blocks.front().cells.front().content->front().table->rows == 2,
                "nested tables build independent span grids");
        // Independent small dense-grid oracle for WHATWG slot placement. Production
        // uses only column expiry ranges; this checks mixed spans and model errors.
        uint32_t random = 71;
        for (unsigned trial = 0; trial < 120; ++trial) {
            auto next = [&] {
                random = random * 1664525 + 1013904223;
                return random;
            };
            unsigned rows = 1 + next() % 6, extent = rows;
            struct Expected {
                unsigned row, column, colspan, rowspan, height;
            };
            std::vector<Expected> expected;
            std::wstring source = L"<table><tbody>";
            for (unsigned row = 0; row < rows; ++row) {
                source += L"<tr>";
                unsigned cells = 1 + next() % 4;
                for (unsigned cell = 0; cell < cells; ++cell) {
                    unsigned colspan = 1 + next() % 3, rowspan = next() % 4;
                    if (!row && !cell)
                        colspan = 2;
                    expected.push_back({row, 0, colspan, rowspan, 0});
                    extent = std::max(extent, row + std::max(1u, rowspan));
                    source += L"<td colspan='" + std::to_wstring(colspan) + L"' rowspan='" +
                              std::to_wstring(rowspan) + L"'>X";
                }
                source += L"</tr>";
            }
            source += L"</tbody></table>\n";
            bool slots[12][100]{};
            bool collision = false;
            unsigned previous = UINT32_MAX, x = 0, columns = 0;
            for (auto &cell : expected) {
                if (cell.row != previous) {
                    x = 0;
                    previous = cell.row;
                }
                while (slots[cell.row][x])
                    ++x;
                cell.column = x;
                cell.height = cell.rowspan ? cell.rowspan : extent - cell.row;
                for (unsigned y = cell.row; y < cell.row + cell.height; ++y)
                    for (unsigned col = x; col < x + cell.colspan; ++col) {
                        collision |= slots[y][col];
                        slots[y][col] = true;
                    }
                x += cell.colspan;
                columns = std::max(columns, x);
            }
            auto actual = parse(std::move(source));
            size_t cellIndex = 0;
            unsigned totalRows = 0;
            for (auto &table : actual.blocks) {
                require(table.table && table.table->columns == columns && table.table->overlap == collision,
                        "sparse table bands agree with independent grid oracle");
                totalRows += table.table->rows;
                std::vector<float> heights;
                for (size_t i = 0; i < table.cells.size(); ++i, ++cellIndex) {
                    auto &e = expected[cellIndex];
                    auto &c = table.cells[i];
                    require(c.row + table.table->rowOffset == e.row && c.column == e.column &&
                                c.rows == e.height,
                            "mixed colspan/rowspan anchors agree with HTML slot placement");
                    heights.push_back((float)(10 + next() % 120));
                }
                auto y = tableRowOffsets(table, heights);
                for (size_t i = 0; i < table.cells.size(); ++i)
                    require(y[table.cells[i].row + table.cells[i].rows] - y[table.cells[i].row] + .02f >=
                                heights[i],
                            "every cell fits the shared row-height constraints");
            }
            require(totalRows == extent && cellIndex == expected.size(),
                    "table bands preserve every row and cell");
        }
        std::wstring manyRows = L"<table><caption>Large table</caption><tbody>";
        for (unsigned row = 0; row < 400; ++row)
            manyRows += L"<tr><td rowspan=2>Group</td><td>A</td></tr><tr><td>B</td></tr>";
        manyRows += L"</tbody></table>\n";
        auto bands = parse(std::move(manyRows));
        require(bands.blocks.size() == 400 && bands.blocks.front().table->captions &&
                    !bands.blocks.front().table->continued && !bands.blocks.back().table->continues,
                "large spanning tables retain captions and incremental connected-row bands");
        uint32_t previousEnd = 0;
        for (auto &band : bands.blocks) {
            require(band.table && band.table->rows == 2 && band.cells.size() == 3 &&
                        band.table->tracks == bands.blocks.front().table->tracks && band.start >= previousEnd,
                    "table bands share column constraints and preserve ordered searchable text ranges");
            previousEnd = band.start + band.length;
        }
        auto nestedHeader =
            parse(L"<table><tr><td><table><tr><th colspan=2>Nested heading</table></table>\n");
        require(nestedHeader.blocks.front().cells[0].content->front().cells[0].align == 2,
                "default data-cell alignment does not override nested header-cell centering");
        auto declaredColumns =
            parse(L"<table><colgroup span=8><col span=2 width='25%'><col width='50%'></colgroup>"
                  L"<tr><td colspan=2>A<td>B</table>\n");
        auto &declared = declaredColumns.blocks.front();
        require(declared.table && declared.table->columns == 3 &&
                    declared.table->declaredColumns->size() == 3 &&
                    declaredColumns.text.find(L"<col") == declaredColumns.text.npos,
                "col elements declare table slots and override their enclosing colgroup span");
        auto declaredWidths = tableColumnOffsets(*declared.table, 800, 1);
        require(std::abs(declaredWidths[1] - 200) < .01f && std::abs(declaredWidths[2] - 400) < .01f &&
                    std::abs(declaredWidths[3] - 800) < .01f,
                "declared column widths participate in the shared native table constraints");
        auto emptyColumns = parse(L"<table><colgroup span='3'></colgroup><tr><td>A</table>\n");
        require(emptyColumns.blocks.front().table->columns == 3 &&
                    emptyColumns.blocks.front().cells.size() == 1,
                "empty colgroups contribute declared columns without creating fake text cells");
        auto implied = parse(L"<div><p>First<p>Second</div>\n\nNormal\n");
        require(implied.blocks.size() == 3 && implied.text.find(L"</p>") == implied.text.npos,
                "HTML5 paragraph end tags are implied by tree construction");
        auto fostered = parse(L"<table>Before<tr><td>Cell</table>\n");
        require(fostered.blocks.size() == 2 && fostered.blocks[0].kind == Kind::Paragraph &&
                    fostered.blocks[1].kind == Kind::Table &&
                    fostered.text.find(L"Before") < fostered.text.find(L"Cell"),
                "HTML5 foster parenting places stray table text before the table");
        auto formatting = parse(L"<div><b>Bold <i>both</b> italic</i><!-- hidden -->"
                                L"<small>small</small><sup>2</sup></div>\n");
        validate(formatting);
        require(formatting.text.find(L"hidden") == formatting.text.npos &&
                    formatting.text.find(L"<") == formatting.text.npos &&
                    std::any_of(formatting.blocks[0].runs.begin(), formatting.blocks[0].runs.end(),
                                [](const Run &run) { return run.style & Superscript; }),
                "HTML5 repairs misnested formatting and omits comments");
        auto longRichCell = parse(L"<table><tr><td><p>" + std::wstring(20000, L'x') + L"</p></table>\n");
        require(longRichCell.blocks.size() == 3 &&
                    std::all_of(longRichCell.blocks.begin(), longRichCell.blocks.end(),
                                [](const Block &b) { return b.kind == Kind::Paragraph && b.length <= 8192; }),
                "oversized rich cells expose bounded children to viewport layout");
        auto largeCell = parse(L"<table><tr><td>" + std::wstring(2 * 1024 * 1024, L'x') +
                                   L"<a href='https://example.com'>Link</a><img src='cell.png' alt='Photo'>"
                                   L"</td></tr></table>\n",
                               nullptr, 0xffffffff, true);
        validate(largeCell);
        require(largeCell.blocks.size() > 250 && largeCell.blocks.back().image &&
                    largeCell.blocks.back().image->url == L"cell.png" &&
                    largeCell.text.find(L"Link") != largeCell.text.npos &&
                    std::all_of(largeCell.blocks.begin(), largeCell.blocks.end(),
                                [](const Block &b) { return b.length <= 8192; }) &&
                    largeCell.sourceStarts.size() == largeCell.blocks.size(),
                "large HTML cells preserve bounded text, links, images and source anchors");
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
        auto malformedHtml = parse(
            L"<div><ol start='invalid'><li>Default one<li>Default two</ol><kbd-custom>literal</kbd-custom>"
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
        bool nestedKey = false;
        auto plainAt = nestedKeys.text.find(L"plain");
        bool plainRestored = plainAt != std::wstring::npos;
        for (auto &block : nestedKeys.blocks)
            for (auto &run : block.runs) {
                auto label = nestedKeys.text.substr(run.start, run.length);
                nestedKey |= label == L"Shift" && (run.style & (Keycap | Bold)) == (Keycap | Bold);
                plainRestored &= !(run.start <= plainAt && plainAt < run.start + run.length &&
                                   (run.style & (Keycap | Bold)));
            }
        require(nestedKey && plainRestored, "nested keyboard styling restores surrounding formatting");
        auto ordinary = parse(L"A plain paragraph.\n\nAnother plain paragraph.");
        require(ordinary.blocks.size() == 2 && ordinary.blocks[0].runs.empty() &&
                    ordinary.blocks[1].runs.empty() && ordinary.text.find(L"Another") != ordinary.text.npos,
                "plain text retains content without allocating formatting records");
        auto separated = parse(L"**one** plain **two**");
        require(separated.blocks[0].runs.size() == 2 &&
                    separated.blocks[0].runs[0].start + separated.blocks[0].runs[0].length <
                        separated.blocks[0].runs[1].start,
                "implicit plain ranges do not merge separated formatting");
        auto longKey = parse(L"<kbd>" + std::wstring(20000, L'x') + L"</kbd>");
        validate(longKey);
        require(longKey.blocks.size() >= 3, "long keyboard content retains bounded layout blocks");
        auto longCode = parse(L"```cpp\n" + std::wstring(20000, L'x') + L"\n```");
        static_assert(sizeof(Block) == 104, "Code grouping must not enlarge every document block");
        validate(longCode);
        require(longCode.blocks.size() >= 3 && std::all_of(longCode.blocks.begin(), longCode.blocks.end(),
                                                           [](const Block &block) {
                                                               return block.kind == Kind::Code &&
                                                                      block.language &&
                                                                      *block.language == L"cpp";
                                                           }),
                "segmented code preserves optional language metadata");
        auto separateCode = parse(L"```\n" + std::wstring(20000, L'x') + L"\n```\n\n```\nshort\n```");
        require(separateCode.blocks.size() == 4 &&
                    separateCode.blocks[0].codeStart == separateCode.blocks[1].codeStart &&
                    separateCode.blocks[1].codeStart == separateCode.blocks[2].codeStart &&
                    separateCode.blocks[2].codeStart != separateCode.blocks[3].codeStart,
                "bounded code pieces share identity without joining adjacent fences");
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
        auto entityStart = std::chrono::steady_clock::now();
        std::wstring ampersands(800000, L'&');
        auto htmlEntities = parse(L"<div>\n" + ampersands + L"&amp;\n</div>\n");
        require(htmlEntities.text.find(ampersands + L"&") != std::wstring::npos,
                "malformed entities preserve text and subsequent valid entities");
        require(std::chrono::steady_clock::now() - entityStart < std::chrono::seconds(3),
                "entity decoding remains linear for long HTML lines");
        bool entityCancelled = false;
        try {
            entity(ampersands, &cancel);
        } catch (const std::runtime_error &) {
            entityCancelled = true;
        }
        require(entityCancelled, "entity preprocessing obeys cancellation");
        std::wstring validEntities = L"x";
        validEntities.reserve(4000001);
        for (int i = 0; i < 1000000; ++i)
            validEntities += L"&lt;";
        std::atomic_bool duringDecode = false;
        std::thread stopDecode([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            duringDecode.store(true);
        });
        bool interruptedEntities = false;
        try {
            entity(validEntities, &duringDecode);
        } catch (const std::runtime_error &) {
            interruptedEntities = true;
        }
        stopDecode.join();
        require(interruptedEntities, "valid entity token jumps cannot skip cancellation checkpoints");
        std::cout << "PASS: adapter, styles, HTML subset, tables, extensions, Unicode, encodings, source, "
                     "cancellation, bounded blocks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
