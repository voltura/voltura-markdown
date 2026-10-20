#include "document.h"
#include "lexbor/html/html.h"
#include "md4c.h"
extern "C" {
#include "entity.h"
}
#include <algorithm>
#ifdef VMD_TESTING
#include <chrono>
#endif
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace vm {
std::wstring fromUtf8(std::string_view s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}
std::string toUtf8(std::wstring_view s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring decode(std::string_view b, std::wstring *encoding) {
    auto label = [&](const wchar_t *s) {
        if (encoding)
            *encoding = s;
    };
    if (b.size() >= 2 &&
        ((uint8_t(b[0]) == 255 && uint8_t(b[1]) == 254) || (uint8_t(b[0]) == 254 && uint8_t(b[1]) == 255))) {
        bool le = uint8_t(b[0]) == 255;
        label(le ? L"UTF-16 LE" : L"UTF-16 BE");
        std::wstring s;
        s.reserve(b.size() / 2);
        for (size_t i = 2; i + 1 < b.size(); i += 2)
            s.push_back(wchar_t(le ? (uint8_t(b[i]) | (uint8_t(b[i + 1]) << 8))
                                   : ((uint8_t(b[i]) << 8) | uint8_t(b[i + 1]))));
        if (b.size() % 2)
            s.push_back(0xfffd);
        return s;
    }
    if (b.starts_with("\xef\xbb\xbf")) {
        b.remove_prefix(3);
        label(L"UTF-8");
        return fromUtf8(b);
    }
    if (b.empty()) {
        label(L"UTF-8");
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.data(), (int)b.size(), nullptr, 0);
    if (n) {
        label(L"UTF-8");
        return fromUtf8(b);
    }
    label(L"Windows ANSI fallback");
    n = MultiByteToWideChar(CP_ACP, 0, b.data(), (int)b.size(), nullptr, 0);
    std::wstring s(n, 0);
    MultiByteToWideChar(CP_ACP, 0, b.data(), (int)b.size(), s.data(), n);
    return s;
}
static void codepoint(std::wstring &s, unsigned c) {
    if (!c || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        c = 0xfffd;
    if (c < 0x10000)
        s.push_back(wchar_t(c));
    else {
        c -= 0x10000;
        s.push_back(wchar_t(0xd800 + (c >> 10)));
        s.push_back(wchar_t(0xdc00 + (c & 1023)));
    }
}
std::wstring entity(std::wstring_view s, const std::atomic_bool *cancel) {
    std::wstring out;
    size_t nextCheck = 0;
    for (size_t i = 0; i < s.size();) {
        if (i >= nextCheck) {
            if (cancel && cancel->load())
                throw std::runtime_error("Cancelled");
            nextCheck = i + 4096;
        }
        if (s[i] == L'&') {
            auto offset = s.substr(i, 40).find(L';');
            if (offset != s.npos) {
                auto end = i + offset;
                auto token = s.substr(i, end - i + 1);
                if (token.size() > 3 && token[1] == L'#') {
                    std::wstring n(token.substr(2, token.size() - 3));
                    wchar_t *tail = nullptr;
                    bool hex = !n.empty() && (n[0] == L'x' || n[0] == L'X');
                    auto digits = std::wstring_view(n).substr(hex ? 1 : 0);
                    bool valid = !digits.empty() &&
                                 digits.find_first_not_of(hex ? L"0123456789abcdefABCDEF" : L"0123456789") ==
                                     digits.npos;
                    unsigned long c = wcstoul(n.c_str() + (hex ? 1 : 0), &tail, hex ? 16 : 10);
                    if (valid && tail && *tail == 0) {
                        codepoint(out, c);
                        i = end + 1;
                        continue;
                    }
                }
                auto bytes = toUtf8(token);
                if (auto e = entity_lookup(bytes.data(), bytes.size())) {
                    codepoint(out, e->codepoints[0]);
                    if (e->codepoints[1])
                        codepoint(out, e->codepoints[1]);
                    i = end + 1;
                    continue;
                }
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}
namespace {
struct Parser {
    Document doc;
    bool sourceMap = false;
    const char *input = nullptr;
    size_t bytePosition = 0;
    size_t inputSize = 0;
    size_t textEnd = 0;
    uint32_t sourceCursor = 0;
    uint32_t sourcePosition = 0;
    std::vector<std::pair<uint32_t, uint32_t>> sourceAnchors;
    bool mappingHtml = false;
    struct HtmlSourceTags {
        std::vector<std::pair<size_t, size_t>> positions;
        size_t next = 0;
    };
    std::map<std::pair<std::wstring, bool>, HtmlSourceTags> htmlSourceTags;
    uint32_t htmlSource = 0;
    size_t htmlPosition = 0;
    bool currentLocated = false;
    void locate(const char *p) {
        if (!sourceMap || (uintptr_t)p < (uintptr_t)input || (uintptr_t)p > (uintptr_t)input + inputSize)
            return;
        size_t end = p - input;
        if (end < bytePosition) {
            uint32_t distance = 0;
            for (size_t i = end; i < bytePosition; ++i) {
                unsigned char c = input[i];
                if ((c & 0xc0) != 0x80)
                    distance += c >= 0xf0 ? 2 : 1;
            }
            sourcePosition = sourceCursor - distance;
            return;
        }
        while (bytePosition < end) {
            unsigned char c = input[bytePosition++];
            if ((c & 0xc0) != 0x80)
                sourceCursor += c >= 0xf0 ? 2 : 1;
        }
        sourcePosition = sourceCursor;
        if (current >= 0 && !currentLocated) {
            sourceAnchors.emplace_back(doc.blocks[current].start, sourcePosition);
            currentLocated = true;
        }
    }
    const std::atomic_bool *cancel;
    std::exception_ptr error;
    int current = -1;
    unsigned quote = 0, styles = 0;
    size_t headingContentStart = 0;
    std::map<std::wstring, unsigned> headingIds;
    struct List {
        bool ordered;
        int64_t next;
    };
    std::vector<List> lists;
    struct Span {
        MD_SPANTYPE type;
        unsigned old;
        std::wstring link;
    };
    std::vector<Span> spans;
    std::vector<std::pair<std::wstring, unsigned>> htmlStyles;
    struct HtmlBlock {
        std::wstring tag;
        unsigned heading, quote, styles;
        int alignment;
        size_t listDepth;
        bool inCode;
    };
    std::vector<HtmlBlock> htmlBlocks;
    struct Picture {
        std::wstring light, dark;
    };
    std::vector<Picture> pictures;
    std::wstring htmlBuffer;
    int alignment = 0;
    struct HtmlCell {
        int row;
        size_t cell, firstChild;
        int alignment;
        unsigned styles;
    };
    std::vector<HtmlCell> htmlCells;
    std::vector<std::pair<std::wstring, int>> tableAlignments;
    struct HtmlTable {
        size_t first;
        uint32_t group = 0;
        TableSection section = TableSection::Body;
        std::vector<TableColumn> columns;
        size_t colGroupFirst = 0;
        unsigned colGroupSpan = 1;
    };
    std::vector<HtmlTable> htmlTables;
    std::wstring link, prefix;
    uint64_t inlineGroup = 1;
    size_t markdownImages = 0;
    bool richTables = false;
    bool inCode = false, inImage = false, inHtml = false, separateRun = false;
    unsigned heading = 0;
    std::wstring attr(const MD_ATTRIBUTE &a) {
        return entity(fromUtf8({a.text, a.size}), cancel);
    }
    void check() {
        if (cancel && cancel->load())
            throw std::runtime_error("Cancelled");
    }
    Block &block() {
        if (current < 0)
            start();
        return doc.blocks[current];
    }
    void start(Kind kind = Kind::Paragraph) {
        check();
        Block b;
        b.kind = kind;
        b.start = (uint32_t)doc.text.size();
        b.codeStart = b.start;
        b.indent = (unsigned)lists.size();
        b.quote = quote;
        b.level = (uint8_t)heading;
        b.align = (uint8_t)alignment;
        doc.blocks.push_back(std::move(b));
        current = (int)doc.blocks.size() - 1;
        currentLocated = mappingHtml;
        if (sourceMap)
            sourceAnchors.emplace_back(doc.blocks.back().start, sourcePosition);
        if (!prefix.empty()) {
            // List markers are structural text, outside the first inline span.
            doc.text.append(prefix);
            prefix.clear();
        }
        if (kind == Kind::Heading)
            headingContentStart = doc.text.size();
    }
    void finish() {
        if (current < 0)
            return;
        auto &b = doc.blocks[current];
        b.length = (uint32_t)doc.text.size() - b.start;
        if (b.kind == Kind::Heading && !b.anchor) {
            std::wstring id = doc.text.substr(std::max<size_t>(b.start, headingContentStart));
            for (auto &c : id)
                c = (wchar_t)towlower(c);
            id.erase(std::remove_if(id.begin(), id.end(),
                                    [](wchar_t c) { return iswpunct(c) && c != L'-' && c != L'_'; }),
                     id.end());
            std::replace(id.begin(), id.end(), L' ', L'-');
            auto base = id;
            auto &next = headingIds[base];
            if (next) {
                do {
                    id = base + L"-" + std::to_wstring(next++);
                } while (headingIds.contains(id));
                headingIds.emplace(id, 1);
            } else
                next = 1;
            b.anchor = std::make_unique<std::wstring>(std::move(id));
        }
        doc.text.push_back(L'\n');
        current = -1;
    }
    void append(std::wstring_view s) {
        check();
        while (!s.empty()) {
            block();
            auto index = current;
            // Bound DirectWrite work even for an unbroken paragraph or code block.
            size_t used = doc.text.size() - doc.blocks[index].start;
            bool splitsSurrogate = used == 8191 && s.size() >= 2 && s[0] >= 0xd800 && s[0] <= 0xdbff &&
                                   s[1] >= 0xdc00 && s[1] <= 0xdfff;
            if ((used >= 8192 || splitsSurrogate) && doc.blocks[index].kind != Kind::Table && !inImage) {
                Block copy;
                copy.kind = doc.blocks[index].kind;
                copy.indent = doc.blocks[index].indent;
                copy.quote = quote;
                copy.level = (uint8_t)heading;
                copy.align = doc.blocks[index].align;
                copy.continuation = true;
                copy.codeStart = doc.blocks[index].codeStart;
                if (doc.blocks[index].language)
                    copy.language = std::make_unique<std::wstring>(*doc.blocks[index].language);
                doc.blocks[index].length = (uint32_t)used;
                current = -1;
                copy.start = (uint32_t)doc.text.size();
                doc.blocks.push_back(std::move(copy));
                current = (int)doc.blocks.size() - 1;
                used = 0;
            }
            auto &b = doc.blocks[current];
            size_t n =
                (b.kind == Kind::Table || inImage) ? s.size() : std::min<size_t>(s.size(), 8192 - used);
            if (n < s.size() && n > 1 && s[n - 1] >= 0xd800 && s[n - 1] <= 0xdbff)
                --n;
            if (!n)
                n = 1;
            uint32_t at = (uint32_t)doc.text.size();
            unsigned st = styles | (inCode ? Mono : 0);
            if (!separateRun && !b.runs.empty() && b.runs.back().start + b.runs.back().length == at &&
                b.runs.back().style == st && b.runs.back().link == link)
                b.runs.back().length += (uint32_t)n;
            else if (st || !link.empty())
                b.runs.push_back({at, (uint32_t)n, st, link});
            separateRun = false;
            doc.text.append(s.substr(0, n));
            s.remove_prefix(n);
        }
    }
    void enter(MD_BLOCKTYPE t, void *d) {
        check();
        ++inlineGroup;
        switch (t) {
        case MD_BLOCK_QUOTE:
            finish();
            ++quote;
            break;
        case MD_BLOCK_UL:
            finish();
            lists.push_back({false, 0});
            break;
        case MD_BLOCK_OL:
            finish();
            lists.push_back({true, ((MD_BLOCK_OL_DETAIL *)d)->start});
            break;
        case MD_BLOCK_LI: {
            finish();
            auto li = (MD_BLOCK_LI_DETAIL *)d;
            prefix = li->is_task ? (li->task_mark == L' ' ? L"☐  " : L"☑  ")
                                 : (!lists.empty() && lists.back().ordered
                                        ? std::to_wstring(lists.back().next++) + L".  "
                                        : L"•  ");
            break;
        }
        case MD_BLOCK_H:
            finish();
            heading = ((MD_BLOCK_H_DETAIL *)d)->level;
            start(Kind::Heading);
            break;
        case MD_BLOCK_CODE: {
            finish();
            inCode = true;
            start(Kind::Code);
            auto language = attr(((MD_BLOCK_CODE_DETAIL *)d)->lang);
            if (!language.empty())
                block().language = std::make_unique<std::wstring>(std::move(language));
            break;
        }
        case MD_BLOCK_HR:
            finish();
            start(Kind::Rule);
            finish();
            break;
        case MD_BLOCK_HTML:
            finish();
            inHtml = true;
            break;
        case MD_BLOCK_P:
            finish();
            // Start lazily when text arrives. An image can be the first inline;
            // eagerly creating a paragraph leaves an empty, full-height block.
            break;
        case MD_BLOCK_TABLE:
            finish();
            break;
        case MD_BLOCK_TR:
            finish();
            start(Kind::Table);
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            auto &b = block();
            if (t == MD_BLOCK_TH)
                b.header = true;
            if (!b.cells.empty())
                append(L"\t");
            block().cells.push_back({(uint32_t)doc.text.size(), 0, (int)((MD_BLOCK_TD_DETAIL *)d)->align});
            break;
        }
        case MD_BLOCK_FOOTNOTE_DEF: {
            finish();
            auto def = (MD_BLOCK_FOOTNOTE_DEF_DETAIL *)d;
            prefix = std::to_wstring(def->id) + L". ";
            start();
            block().anchor = std::make_unique<std::wstring>(L"fn-" + std::to_wstring(def->id));
            break;
        }
        case MD_BLOCK_ADMONITION:
            finish();
            ++quote;
            start();
            styles |= Bold;
            append(attr(((MD_BLOCK_ADMONITION_DETAIL *)d)->type));
            styles &= ~Bold;
            finish();
            break;
        default:
            break;
        }
    }
    void leave(MD_BLOCKTYPE t, void *) {
        ++inlineGroup;
        switch (t) {
        case MD_BLOCK_QUOTE:
            finish();
            if (quote)
                --quote;
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            finish();
            if (!lists.empty())
                lists.pop_back();
            break;
        case MD_BLOCK_LI:
            finish();
            prefix.clear();
            break;
        case MD_BLOCK_H:
            finish();
            heading = 0;
            break;
        case MD_BLOCK_CODE:
            finish();
            inCode = false;
            break;
        case MD_BLOCK_HTML:
            renderHtmlFragment();
            finish();
            inHtml = false;
            break;
        case MD_BLOCK_P:
        case MD_BLOCK_TR:
        case MD_BLOCK_FOOTNOTE_DEF:
            finish();
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            auto &b = block();
            if (!b.cells.empty())
                b.cells.back().length = (uint32_t)doc.text.size() - b.cells.back().start;
            break;
        }
        case MD_BLOCK_ADMONITION:
            finish();
            if (quote)
                --quote;
            break;
        default:
            break;
        }
    }
    void spanEnter(MD_SPANTYPE t, void *d) {
        spans.push_back({t, styles, link});
        switch (t) {
        case MD_SPAN_EM:
            styles |= Italic;
            break;
        case MD_SPAN_STRONG:
            styles |= Bold;
            break;
        case MD_SPAN_CODE:
            styles |= Mono;
            break;
        case MD_SPAN_DEL:
            styles |= Strike;
            break;
        case MD_SPAN_A:
            link = attr(((MD_SPAN_A_DETAIL *)d)->href);
            styles |= Underline;
            break;
        case MD_SPAN_IMG:
            finish();
            if (sourceMap) {
                // A reference image's URL may live at the end of the file.
                // Anchor the image syntax at its use, including empty alt text.
                auto marker = std::string_view(input, inputSize).find("![", textEnd);
                if (marker != std::string_view::npos)
                    locate(input + marker);
            }
            inImage = true;
            start(Kind::Image);
            block().image = std::make_unique<ImageReference>(attr(((MD_SPAN_IMG_DETAIL *)d)->src));
            block().image->inlineGroup = inlineGroup;
            ++markdownImages;
            break;
        case MD_SPAN_FOOTNOTE_REF: {
            auto ref = (MD_SPAN_FOOTNOTE_REF_DETAIL *)d;
            link = L"#fn-" + std::to_wstring(ref->id);
            styles |= Underline;
            append(L"[" + std::to_wstring(ref->id) + L"]");
            break;
        }
        default:
            break;
        }
    }
    void spanLeave(MD_SPANTYPE t) {
        if (t == MD_SPAN_IMG) {
            finish();
            inImage = false;
        }
        if (!spans.empty()) {
            styles = spans.back().old;
            link = std::move(spans.back().link);
            spans.pop_back();
        }
    }
    std::wstring attribute(std::wstring_view tag, std::wstring_view name) {
        size_t i = 1;
        while (i < tag.size() && !iswspace(tag[i]) && tag[i] != L'>')
            ++i;
        while (i < tag.size()) {
            while (i < tag.size() && (iswspace(tag[i]) || tag[i] == L'/'))
                ++i;
            size_t a = i;
            while (i < tag.size() && (iswalnum(tag[i]) || tag[i] == L'-' || tag[i] == L'_'))
                ++i;
            if (a == i) {
                ++i;
                continue;
            }
            std::wstring key(tag.substr(a, i - a));
            for (auto &c : key)
                c = (wchar_t)towlower(c);
            while (i < tag.size() && iswspace(tag[i]))
                ++i;
            if (i == tag.size() || tag[i] != L'=')
                continue;
            ++i;
            while (i < tag.size() && iswspace(tag[i]))
                ++i;
            wchar_t q = i < tag.size() && (tag[i] == L'\'' || tag[i] == L'"') ? tag[i++] : 0;
            size_t begin = i;
            while (i < tag.size() && (q ? tag[i] != q : !iswspace(tag[i]) && tag[i] != L'>'))
                ++i;
            if (key == name)
                return entity(tag.substr(begin, i - begin), cancel);
            if (q && i < tag.size())
                ++i;
        }
        return {};
    }
    static bool htmlSpace(wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f';
    }
    void renderHtmlFragment() {
        if (htmlBuffer.empty())
            return;
        if (sourceMap) {
            mappingHtml = true;
            htmlPosition = 0;
        }
        auto input = toUtf8(std::exchange(htmlBuffer, {}));
        std::unique_ptr<lxb_html_document_t, decltype(&lxb_html_document_destroy)> tree(
            lxb_html_document_create(), lxb_html_document_destroy);
        if (!tree || lxb_html_document_parse_chunk_begin(tree.get()) != LXB_STATUS_OK)
            throw std::runtime_error("HTML parser unavailable");
        // Observe the real tokenizer, then forward to its tree builder. This
        // excludes tag-like text in comments, attributes and raw-text elements.
        // Count UTF-16 offsets in one forward pass over the stable UTF-8 input.
        struct SourceIndex {
            Parser &parser;
            std::string_view input;
            lxb_html_tokenizer_t *tokenizer;
            lxb_html_tokenizer_token_f forward;
            void *context;
            size_t byte = 0, units = 0;
            size_t position(size_t end) {
                while (byte < end) {
                    if ((byte & 4095) == 0)
                        parser.check();
                    unsigned char c = input[byte++];
                    if ((c & 0xc0) != 0x80)
                        units += c >= 0xf0 ? 2 : 1;
                }
                return units;
            }
            static lxb_html_token_t *token(lxb_html_tokenizer_t *tkz, lxb_html_token_t *token, void *data) {
                auto &self = *(SourceIndex *)data;
                try {
                    self.parser.check();
                    if (token->tag_id >= LXB_TAG__BEGIN) {
                        bool close = (token->type & LXB_HTML_TOKEN_TYPE_CLOSE) != 0;
                        size_t name = (const char *)token->begin - self.input.data();
                        size_t end = (const char *)token->end - self.input.data();
                        auto tag = fromUtf8(self.input.substr(name, end - name));
                        for (auto &c : tag)
                            c = (wchar_t)towlower(c);
                        auto start = self.position(name) - (close ? 2 : 1);
                        auto limit = self.position(end);
                        self.parser.htmlSourceTags[{std::move(tag), close}].positions.emplace_back(start,
                                                                                                   limit);
                    }
                    return self.forward(tkz, token, self.context);
                } catch (...) {
                    self.parser.error = std::current_exception();
                    tkz->status = LXB_STATUS_ERROR;
                    return nullptr;
                }
            }
            ~SourceIndex() {
                lxb_html_tokenizer_callback_token_done_set(tokenizer, forward, context);
            }
        };
        auto tokenizer = ((lxb_html_parser_t *)tree->dom_document.parser)->tkz;
        SourceIndex index{*this, input, tokenizer, tokenizer->callback_token_done,
                          tokenizer->callback_token_ctx};
        if (sourceMap)
            lxb_html_tokenizer_callback_token_done_set(tokenizer, SourceIndex::token, &index);
        for (size_t at = 0; at < input.size(); at += 65536) {
            check();
            if (lxb_html_document_parse_chunk(tree.get(), (const lxb_char_t *)input.data() + at,
                                              std::min<size_t>(65536, input.size() - at)) != LXB_STATUS_OK) {
                if (error)
                    std::rethrow_exception(error);
                throw std::runtime_error("HTML parsing failed");
            }
        }
        if (lxb_html_document_parse_chunk_end(tree.get()) != LXB_STATUS_OK) {
            if (error)
                std::rethrow_exception(error);
            throw std::runtime_error("HTML parsing failed");
        }
        // Tree construction (including implied end tags and table foster parenting)
        // belongs to the HTML5 parser, not the native presentation adapter.
        std::string normalized;
        auto collect = [](const lxb_char_t *data, size_t size, void *context) -> lxb_status_t {
            try {
                ((std::string *)context)->append((const char *)data, size);
                return LXB_STATUS_OK;
            } catch (...) {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }
        };
        if (lxb_html_serialize_deep_cb(lxb_dom_interface_node(tree->body), collect, &normalized) !=
            LXB_STATUS_OK)
            throw std::runtime_error("HTML presentation failed");
        html(fromUtf8(normalized));
        mappingHtml = false;
        htmlSourceTags.clear();
    }
    int htmlAlignment(std::wstring_view token, int inherited) {
        auto value = attribute(token, L"align");
        for (auto &c : value)
            c = (wchar_t)towlower(c);
        return value == L"left" ? 1 : value == L"center" ? 2 : value == L"right" ? 3 : inherited;
    }
    // WHATWG non-negative-integer parsing, followed by the table model limits.
    unsigned tableSpan(std::wstring_view token, bool rows, std::wstring_view name = {}) {
        auto value = attribute(token, name.empty() ? rows ? L"rowspan" : L"colspan" : name);
        size_t at = 0;
        while (at < value.size() && htmlSpace(value[at]))
            ++at;
        bool negative = at < value.size() && value[at] == L'-';
        if (at < value.size() && (value[at] == L'+' || negative))
            ++at;
        if (at == value.size() || value[at] < L'0' || value[at] > L'9')
            return 1;
        unsigned result = 0, limit = rows ? 65534 : 1000;
        while (at < value.size() && value[at] >= L'0' && value[at] <= L'9') {
            check();
            result = std::min(limit, result * 10 + (unsigned)(value[at++] - L'0'));
        }
        if (negative && result)
            return 1;
        return result || rows ? result : 1;
    }
    bool formSpanningTable(size_t first, std::vector<TableColumn> declarations) {
        // Ordinary rows retain their incremental layout path. A span connects rows
        // and is represented by one sparse grid; never allocate rows * columns slots.
        bool needsGrid = !declarations.empty(), footerSeen = false;
        for (size_t i = first; i < doc.blocks.size(); ++i)
            if (doc.blocks[i].kind == Kind::Table) {
                // Footer-first source needs visual row coordinates even without
                // spans. Keep source/text order intact for search and selection.
                if ((TableSection)doc.blocks[i].level == TableSection::Foot)
                    footerSeen = true;
                else if (footerSeen)
                    needsGrid = true;
                for (auto &cell : doc.blocks[i].cells)
                    needsGrid |= cell.colSpan != 1 || cell.rowSpan != 1;
            }
        if (!needsGrid)
            return false;
        struct Group {
            std::vector<Block *> rows;
            TableSection section;
            uint32_t id;
        };
        std::vector<Group> groups;
        for (size_t i = first; i < doc.blocks.size(); ++i) {
            auto &row = doc.blocks[i];
            if (row.kind != Kind::Table)
                continue;
            if (groups.empty() || groups.back().id != row.codeStart)
                groups.push_back({{}, (TableSection)row.level, row.codeStart});
            groups.back().rows.push_back(&row);
        }
        if (groups.empty())
            return true; // A caption-only/empty table has no visible grid.
        // The HTML model processes footer groups after all other row groups.
        std::stable_partition(groups.begin(), groups.end(),
                              [](const Group &g) { return g.section != TableSection::Foot; });
        auto model = std::make_unique<TableModel>();
        model->columns = (unsigned)declarations.size();
        if (!declarations.empty())
            model->declaredColumns = std::make_shared<std::vector<TableColumn>>(std::move(declarations));
        for (auto &group : groups) {
            check();
            unsigned count = (unsigned)group.rows.size();
            for (unsigned y = 0; y < group.rows.size(); ++y)
                for (auto &cell : group.rows[y]->cells)
                    count = std::max(count, y + std::max<unsigned>(1, cell.rowSpan));
            if (count > 1000000 || model->rows > 1000000 - count)
                throw std::runtime_error("HTML table row limit");
            model->groups.push_back({model->rows, count, group.section, (uint32_t)model->groups.size()});
            std::vector<unsigned> occupiedUntil;
            for (unsigned y = 0; y < group.rows.size(); ++y) {
                unsigned x = 0;
                for (auto &cell : group.rows[y]->cells) {
                    check();
                    while (x < occupiedUntil.size() && occupiedUntil[x] > y)
                        ++x;
                    if (x + cell.colSpan > 65536)
                        throw std::runtime_error("HTML table column limit");
                    occupiedUntil.resize(std::max(occupiedUntil.size(), (size_t)x + cell.colSpan));
                    cell.row = model->rows + y;
                    cell.column = x;
                    cell.rows = cell.rowSpan ? cell.rowSpan : count - y;
                    for (unsigned col = x; col < x + cell.colSpan; ++col) {
                        model->overlap |= occupiedUntil[col] > y;
                        occupiedUntil[col] = std::max(occupiedUntil[col], y + cell.rows);
                    }
                    x += cell.colSpan;
                    model->columns = std::max(model->columns, x);
                }
            }
            model->rows += count;
        }
        Block merged;
        merged.kind = Kind::Table;
        merged.table = std::move(model);
        merged.start = UINT32_MAX;
        uint32_t end = 0;
        std::deque<Block> other;
        for (size_t i = first; i < doc.blocks.size(); ++i) {
            auto &row = doc.blocks[i];
            if (row.kind != Kind::Table) {
                merged.start = std::min(merged.start, row.start);
                end = std::max(end, row.start + row.length);
                merged.runs.insert(merged.runs.end(), row.runs.begin(), row.runs.end());
                other.push_back(std::move(row));
                continue;
            }
            merged.indent = row.indent;
            merged.quote = row.quote;
            merged.align = row.align;
            merged.start = std::min(merged.start, row.start);
            end = std::max(end, row.start + row.length);
            for (auto &cell : row.cells)
                merged.cells.push_back(std::move(cell));
            for (auto &run : row.runs)
                merged.runs.push_back(std::move(run));
        }
        doc.blocks.erase(doc.blocks.begin() + first, doc.blocks.end());
        if (!other.empty())
            merged.table->captions = std::make_shared<std::deque<Block>>(std::move(other));
        std::sort(merged.runs.begin(), merged.runs.end(),
                  [](const Run &a, const Run &b) { return a.start < b.start; });
        merged.length = end - merged.start;
        merged.table->tracks = std::make_shared<std::vector<TableColumn>>(merged.table->columns);
        if (merged.table->declaredColumns)
            std::copy(merged.table->declaredColumns->begin(), merged.table->declaredColumns->end(),
                      merged.table->tracks->begin());
        for (auto &cell : merged.cells) {
            check();
            for (unsigned col = cell.column; col < cell.column + cell.colSpan; ++col) {
                auto &track = (*merged.table->tracks)[col];
                track.minimum = std::max(track.minimum, 1.f / cell.colSpan);
                if (cell.percent)
                    track.percent = std::max(track.percent, (float)std::min(cell.width, 100u) / cell.colSpan);
                else
                    track.pixels = std::max(track.pixels, (float)cell.width / cell.colSpan);
            }
        }
        // Rendering bands preserve the standard table model while bounding
        // DirectWrite work to the connected rowspans near the viewport.
        bool ordered = std::is_sorted(merged.cells.begin(), merged.cells.end(),
                                      [](const Cell &a, const Cell &b) { return a.row < b.row; });
        bool laterCaption =
            merged.table->captions && !merged.cells.empty() &&
            std::any_of(merged.table->captions->begin(), merged.table->captions->end(),
                        [&](const Block &caption) { return caption.start >= merged.cells.front().start; });
        if (!ordered || merged.cells.empty() || laterCaption) {
            doc.blocks.push_back(std::move(merged));
            return true;
        }
        size_t begin = 0, firstGroup = 0;
        unsigned firstRow = 0;
        while (begin < merged.cells.size()) {
            check();
            unsigned lastRow = firstRow + 1;
            size_t endCell = begin;
            while (endCell < merged.cells.size() && merged.cells[endCell].row < lastRow) {
                lastRow = std::max(lastRow, merged.cells[endCell].row + merged.cells[endCell].rows);
                ++endCell;
            }
            // Empty rows are zero-height unless a spanning cell gives them height.
            if (endCell < merged.cells.size())
                lastRow = merged.cells[endCell].row;
            else
                lastRow = merged.table->rows;
            Block band;
            band.kind = Kind::Table;
            band.indent = merged.indent;
            band.quote = merged.quote;
            band.align = merged.align;
            band.start = begin ? merged.cells[begin].start : merged.start;
            uint32_t endText =
                endCell < merged.cells.size() ? merged.cells[endCell].start : merged.start + merged.length;
            band.length = endText - band.start;
            band.table = std::make_unique<TableModel>();
            band.table->rows = lastRow - firstRow;
            band.table->columns = merged.table->columns;
            band.table->rowOffset = firstRow;
            band.table->overlap = merged.table->overlap;
            band.table->continued = begin != 0;
            band.table->continues = endCell < merged.cells.size();
            band.table->tracks = merged.table->tracks;
            band.table->declaredColumns = merged.table->declaredColumns;
            if (!begin)
                band.table->captions = std::move(merged.table->captions);
            // Bands and groups are ordered. Skip expired groups once, and only
            // visit groups that overlap this band instead of rescanning all.
            auto &rowGroups = merged.table->groups;
            while (firstGroup < rowGroups.size() &&
                   rowGroups[firstGroup].first + rowGroups[firstGroup].rows <= firstRow)
                ++firstGroup;
            for (size_t g = firstGroup; g < rowGroups.size() && rowGroups[g].first < lastRow; ++g) {
                auto group = rowGroups[g];
                unsigned lo = std::max(group.first, firstRow),
                         hi = std::min(group.first + group.rows, lastRow);
                if (hi > lo) {
                    group.first = lo - firstRow;
                    group.rows = hi - lo;
                    band.table->groups.push_back(group);
                }
            }
            auto run = std::lower_bound(merged.runs.begin(), merged.runs.end(), band.start,
                                        [](const Run &r, uint32_t at) { return r.start < at; });
            while (run != merged.runs.end() && run->start < endText)
                band.runs.push_back(*run++);
            for (size_t cell = begin; cell < endCell; ++cell) {
                merged.cells[cell].row -= firstRow;
                band.cells.push_back(std::move(merged.cells[cell]));
            }
            doc.blocks.push_back(std::move(band));
            firstRow = lastRow;
            begin = endCell;
        }
        return true;
    }
    void pictureSource(std::wstring_view token) {
        if (pictures.empty())
            return;
        auto srcset = attribute(token, L"srcset");
        // Support a single resource, optionally marked 1x. Responsive candidate
        // lists and viewport media queries need a browser-style sizing policy.
        auto begin = srcset.find_first_not_of(L" \t\r\n\f");
        if (begin == srcset.npos)
            return;
        auto end = srcset.find_first_of(L" \t\r\n\f", begin);
        auto url = srcset.substr(begin, end - begin);
        if (url.find(L',') != url.npos)
            return;
        if (end != srcset.npos) {
            auto descriptor = srcset.substr(end);
            auto first = descriptor.find_first_not_of(L" \t\r\n\f");
            if (first != descriptor.npos &&
                descriptor.substr(first, descriptor.find_last_not_of(L" \t\r\n\f") - first + 1) != L"1x")
                return;
        }
        auto media = attribute(token, L"media");
        std::wstring query;
        for (auto c : media)
            if (!htmlSpace(c))
                query.push_back((wchar_t)towlower(c));
        auto type = attribute(token, L"type");
        for (auto &c : type)
            c = (wchar_t)towlower(c);
        if (!type.empty() && type != L"image/png" && type != L"image/jpeg" && type != L"image/gif" &&
            type != L"image/bmp" && type != L"image/tiff" && type != L"image/svg+xml")
            return;
        auto &picture = pictures.back();
        if ((query.empty() || query == L"all" || query == L"(prefers-color-scheme:light)") &&
            picture.light.empty())
            picture.light = url;
        if ((query.empty() || query == L"all" || query == L"(prefers-color-scheme:dark)") &&
            picture.dark.empty())
            picture.dark = url;
    }
    uint32_t imageDimension(std::wstring_view token, std::wstring_view name) {
        auto value = attribute(token, name);
        size_t begin = 0, end = value.size();
        while (begin < end && htmlSpace(value[begin]))
            ++begin;
        while (end > begin && htmlSpace(value[end - 1]))
            --end;
        uint32_t result = 0;
        for (size_t i = begin; i < end; ++i) {
            if (value[i] < L'0' || value[i] > L'9')
                return 0;
            unsigned digit = value[i] - L'0';
            if (result > (UINT32_MAX - digit) / 10)
                return 0;
            result = result * 10 + digit;
        }
        return result;
    }
    void htmlText(std::wstring_view text) {
        auto decoded = entity(text, cancel);
        if (inCode) {
            append(decoded);
            return;
        }
        std::wstring collapsed;
        bool space = current < 0 || (!doc.text.empty() && htmlSpace(doc.text.back()));
        for (wchar_t c : decoded) {
            if (htmlSpace(c)) {
                if (!space)
                    collapsed.push_back(L' ');
                space = true;
            } else {
                collapsed.push_back(c);
                space = false;
            }
        }
        append(collapsed);
    }
    void closeHtmlBlock() {
        finish();
        auto context = std::move(htmlBlocks.back());
        htmlBlocks.pop_back();
        heading = context.heading;
        quote = context.quote;
        styles = context.styles;
        alignment = context.alignment;
        lists.resize(context.listDepth);
        inCode = context.inCode;
        prefix.clear();
    }
    bool structuralHtml(const std::wstring &tag, std::wstring_view token, bool close) {
        bool isHeading = tag.size() == 2 && tag[0] == L'h' && tag[1] >= L'1' && tag[1] <= L'6';
        bool isList = tag == L"ul" || tag == L"ol";
        bool wrapper = tag == L"section" || tag == L"article" || tag == L"main" || tag == L"nav" ||
                       tag == L"aside" || tag == L"header" || tag == L"footer" || tag == L"details" ||
                       tag == L"p" || tag == L"div" || tag == L"figure" || tag == L"figcaption" ||
                       tag == L"center" || tag == L"address" || tag == L"dl" || tag == L"dt" ||
                       tag == L"dd" || tag == L"caption";
        if (!isHeading && !isList && !wrapper && tag != L"li" && tag != L"blockquote" && tag != L"summary" &&
            tag != L"pre")
            return false;
        if ((tag == L"li" && !close) || (isList && close))
            if (!htmlBlocks.empty() && htmlBlocks.back().tag == L"li")
                closeHtmlBlock();
        if (close) {
            if (htmlBlocks.empty() || htmlBlocks.back().tag != tag)
                return false;
            closeHtmlBlock();
            return true;
        }
        finish();
        htmlBlocks.push_back({tag, heading, quote, styles, alignment, lists.size(), inCode});
        alignment = tag == L"center" || tag == L"caption" ? 2 : htmlAlignment(token, alignment);
        if (isHeading) {
            heading = tag[1] - L'0';
            start(Kind::Heading);
            auto id = attribute(token, L"id");
            if (!id.empty()) {
                headingIds.try_emplace(id, 1);
                block().anchor = std::make_unique<std::wstring>(std::move(id));
            }
        } else if (tag == L"blockquote") {
            ++quote;
        } else if (isList) {
            int64_t number = 1;
            if (tag == L"ol") {
                auto value = attribute(token, L"start");
                wchar_t *tail = nullptr;
                auto parsed = wcstoll(value.c_str(), &tail, 10);
                if (!value.empty() && tail != value.c_str() && *tail == 0 && parsed >= INT32_MIN &&
                    parsed <= INT32_MAX)
                    number = parsed;
            }
            lists.push_back({tag == L"ol", number});
        } else if (tag == L"li") {
            if (!lists.empty() && lists.back().ordered) {
                auto value = attribute(token, L"value");
                wchar_t *tail = nullptr;
                auto parsed = wcstoll(value.c_str(), &tail, 10);
                if (!value.empty() && tail != value.c_str() && *tail == 0 && parsed >= INT32_MIN &&
                    parsed <= INT32_MAX)
                    lists.back().next = parsed;
            }
            prefix = !lists.empty() && lists.back().ordered ? std::to_wstring(lists.back().next++) + L".  "
                                                            : L"•  ";
            start();
        } else if (tag == L"summary") {
            styles |= Bold;
            start();
        } else if (tag == L"pre") {
            inCode = true;
            start(Kind::Code);
        }
        return true;
    }
    void html(std::wstring_view s) {
        size_t i = 0;
        while (i < s.size()) {
            check();
            if (s.substr(i, 4) == L"<!--") {
                auto commentEnd = s.find(L"-->", i + 4);
                i = commentEnd == s.npos ? s.size() : commentEnd + 3;
                continue;
            }
            if (s[i] != L'<') {
                auto end = s.find(L'<', i);
                if (end == s.npos)
                    end = s.size();
                htmlText(s.substr(i, end - i));
                i = end;
                continue;
            }
            size_t end = i + 1;
            wchar_t q = 0;
            for (; end < s.size(); ++end) {
                if (q) {
                    if (s[end] == q)
                        q = 0;
                } else if (s[end] == L'\'' || s[end] == L'"')
                    q = s[end];
                else if (s[end] == L'>')
                    break;
            }
            if (end == s.size()) {
                append(s.substr(i));
                break;
            }
            auto token = s.substr(i, end - i + 1);
            size_t at = 1;
            bool close = at < token.size() && token[at] == L'/';
            if (close)
                ++at;
            size_t a = at;
            while (at < token.size() &&
                   ((token[at] >= L'a' && token[at] <= L'z') || (token[at] >= L'A' && token[at] <= L'Z') ||
                    (token[at] >= L'0' && token[at] <= L'9') || token[at] == L'-' || token[at] == L'_' ||
                    token[at] == L':'))
                ++at;
            if (at < token.size() && !htmlSpace(token[at]) && token[at] != L'>' && token[at] != L'/') {
                append(token);
                i = end + 1;
                continue;
            }
            std::wstring tag(token.substr(a, at - a));
            for (auto &c : tag)
                c = (wchar_t)towlower(c);
            if (mappingHtml) {
                // Normalization can repair/reorder HTML. Match surviving tags in
                // source order; never let repaired markup move an anchor backwards.
                auto found = htmlSourceTags.find({tag, close});
                if (found != htmlSourceTags.end()) {
                    auto &tags = found->second;
                    while (tags.next < tags.positions.size() &&
                           tags.positions[tags.next].first < htmlPosition) {
                        check();
                        ++tags.next;
                    }
                    if (tags.next < tags.positions.size()) {
                        auto [match, endName] = tags.positions[tags.next++];
                        sourcePosition = htmlSource + (uint32_t)match;
                        htmlPosition = endName;
                    }
                }
            }
            if (structuralHtml(tag, token, close)) {
                i = end + 1;
                continue;
            }
            unsigned style = tag == L"b" || tag == L"strong" ? Bold
                             : tag == L"i" || tag == L"em" || tag == L"cite" || tag == L"dfn" || tag == L"var"
                                 ? Italic
                             : tag == L"code" || tag == L"samp" || tag == L"tt" ? Mono
                             : tag == L"s" || tag == L"del" || tag == L"strike" ? Strike
                             : tag == L"kbd"                                    ? Keycap | Mono
                             : tag == L"u" || tag == L"ins"                     ? Underline
                             : tag == L"sub"                                    ? Subscript
                             : tag == L"sup"                                    ? Superscript
                             : tag == L"small"                                  ? Small
                                                                                : 0;
            if (style) {
                if (tag == L"kbd")
                    separateRun = true;
                if (close) {
                    if (!htmlStyles.empty() && htmlStyles.back().first == tag) {
                        styles = htmlStyles.back().second;
                        htmlStyles.pop_back();
                    } else
                        append(token);
                } else {
                    htmlStyles.push_back({tag, styles});
                    styles |= style;
                }
            } else if (tag == L"picture") {
                if (close) {
                    if (!pictures.empty())
                        pictures.pop_back();
                } else
                    pictures.push_back({});
            } else if (tag == L"source" && !pictures.empty()) {
                if (!close)
                    pictureSource(token);
            } else if (tag == L"span" || tag == L"abbr" || tag == L"time" || tag == L"data" ||
                       tag == L"bdi" || tag == L"bdo" || tag == L"wbr") {
                // Harmless wrappers retain their content; CSS and attributes are ignored.
            } else if (tag == L"hr" && !close) {
                finish();
                start(Kind::Rule);
                finish();
            } else if (tag == L"br") {
                ++inlineGroup;
                if (current >= 0)
                    append(L"\n");
            } else if (tag == L"a") {
                if (close) {
                    link.clear();
                    styles &= ~Underline;
                } else {
                    link = attribute(token, L"href");
                    styles |= Underline;
                }
            } else if (tag == L"img" && !close) {
                ImageReference image{attribute(token, L"src")};
                image.width = imageDimension(token, L"width");
                image.height = imageDimension(token, L"height");
                if (!pictures.empty()) {
                    image.lightUrl = pictures.back().light;
                    image.darkUrl = pictures.back().dark;
                }
                finish();
                start(Kind::Image);
                block().image = std::make_unique<ImageReference>(std::move(image));
                append(attribute(token, L"alt"));
                finish();
            } else if (tag == L"colgroup" || tag == L"col") {
                if (!htmlTables.empty()) {
                    auto &table = htmlTables.back();
                    if (tag == L"colgroup") {
                        if (!close) {
                            table.colGroupFirst = table.columns.size();
                            table.colGroupSpan = tableSpan(token, false, L"span");
                        } else if (table.columns.size() == table.colGroupFirst) {
                            if (table.columns.size() + table.colGroupSpan > 65536)
                                throw std::runtime_error("HTML table column limit");
                            table.columns.resize(table.columns.size() + table.colGroupSpan);
                        }
                    } else if (!close) {
                        unsigned span = tableSpan(token, false, L"span");
                        if (table.columns.size() + span > 65536)
                            throw std::runtime_error("HTML table column limit");
                        TableColumn column;
                        auto width = attribute(token, L"width");
                        bool percent = !width.empty() && width.back() == L'%';
                        if (percent)
                            width.pop_back();
                        if (!width.empty() && width.size() <= 9 &&
                            std::all_of(width.begin(), width.end(),
                                        [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
                            auto hint = (float)wcstoul(width.c_str(), nullptr, 10);
                            if (percent)
                                column.percent = std::min(100.f, hint);
                            else
                                column.pixels = hint;
                        }
                        table.columns.insert(table.columns.end(), span, column);
                    }
                }
            } else if (tag == L"table" || tag == L"thead" || tag == L"tbody" || tag == L"tfoot") {
                finish();
                if (tag == L"table") {
                    if (!close)
                        htmlTables.push_back({doc.blocks.size()});
                    else if (!htmlTables.empty()) {
                        auto context = std::move(htmlTables.back());
                        auto first = context.first;
                        htmlTables.pop_back();
                        if (formSpanningTable(first, std::move(context.columns))) {
                            // Span coordinates and row groups now belong to the grid.
                        } else {
                            std::vector<Cell> columns;
                            for (size_t row = first; row < doc.blocks.size(); ++row) {
                                auto &b = doc.blocks[row];
                                if (b.kind != Kind::Table)
                                    continue;
                                columns.resize(
                                    std::max(columns.size(), std::min<size_t>(256, b.cells.size())));
                                for (size_t col = 0; col < std::min(columns.size(), b.cells.size()); ++col)
                                    if (!columns[col].width && b.cells[col].width) {
                                        columns[col].width = b.cells[col].width;
                                        columns[col].percent = b.cells[col].percent;
                                    }
                            }
                            for (size_t row = first; row < doc.blocks.size(); ++row) {
                                auto &b = doc.blocks[row];
                                if (b.kind != Kind::Table || b.cells.size() > 256)
                                    continue;
                                while (b.cells.size() < columns.size())
                                    b.cells.push_back({b.start + b.length, 0});
                                for (size_t col = 0; col < b.cells.size(); ++col) {
                                    b.cells[col].width = columns[col].width;
                                    b.cells[col].percent = columns[col].percent;
                                }
                                b.level = 0;
                            }
                        }
                    }
                } else if (!htmlTables.empty()) {
                    ++htmlTables.back().group;
                    htmlTables.back().section = !close && tag == L"thead"   ? TableSection::Head
                                                : !close && tag == L"tfoot" ? TableSection::Foot
                                                                            : TableSection::Body;
                }
                if (!close) {
                    tableAlignments.push_back({tag, alignment});
                    alignment = htmlAlignment(token, alignment);
                } else if (!tableAlignments.empty() && tableAlignments.back().first == tag) {
                    alignment = tableAlignments.back().second;
                    tableAlignments.pop_back();
                }
            } else if (tag == L"tr") {
                if (close && current >= 0 && block().kind == Kind::Table)
                    block().header =
                        !block().cells.empty() && std::all_of(block().cells.begin(), block().cells.end(),
                                                              [](const Cell &cell) { return cell.header; });
                finish();
                if (!close) {
                    tableAlignments.push_back({tag, alignment});
                    alignment = htmlAlignment(token, alignment);
                    start(Kind::Table);
                    if (!htmlTables.empty()) {
                        block().codeStart = htmlTables.back().group;
                        block().level = (uint8_t)htmlTables.back().section;
                    }
                } else if (!tableAlignments.empty() && tableAlignments.back().first == tag) {
                    alignment = tableAlignments.back().second;
                    tableAlignments.pop_back();
                }
            } else if (tag == L"th" || tag == L"td") {
                richTables = true;
                if (!close) {
                    if (block().kind != Kind::Table) {
                        finish();
                        start(Kind::Table);
                    }
                    if (!block().cells.empty())
                        append(L"\t");
                    auto cellAlign = htmlAlignment(token, alignment ? alignment : tag == L"th" ? 2 : 0);
                    auto width = attribute(token, L"width");
                    bool percent = !width.empty() && width.back() == L'%';
                    if (percent)
                        width.pop_back();
                    unsigned cellWidth = 0;
                    if (!width.empty() && width.size() <= 5 &&
                        std::all_of(width.begin(), width.end(),
                                    [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                        cellWidth = (unsigned)wcstoul(width.c_str(), nullptr, 10);
                    block().cells.push_back({(uint32_t)doc.text.size(), 0, cellAlign, cellWidth, percent});
                    block().cells.back().colSpan = (uint16_t)tableSpan(token, false);
                    block().cells.back().rowSpan = (uint16_t)tableSpan(token, true);
                    block().cells.back().header = tag == L"th";
                    block().header |= tag == L"th";
                    auto valign = attribute(token, L"valign");
                    for (auto &c : valign)
                        c = (wchar_t)towlower(c);
                    block().cells.back().verticalAlign = valign == L"top" ? 1 : valign == L"bottom" ? 3 : 2;
                    if (htmlCells.size() >= 64)
                        throw std::runtime_error("HTML table nesting limit");
                    htmlCells.push_back(
                        {current, block().cells.size() - 1, doc.blocks.size(), alignment, styles});
                    current = -1;
                    alignment = cellAlign;
                    if (tag == L"th")
                        styles |= Bold;

                } else if (!htmlCells.empty()) {
                    finish();
                    auto context = htmlCells.back();
                    htmlCells.pop_back();
                    auto content = std::make_shared<std::deque<Block>>();
                    for (size_t child = context.firstChild; child < doc.blocks.size(); ++child)
                        content->push_back(std::move(doc.blocks[child]));
                    doc.blocks.erase(doc.blocks.begin() + context.firstChild, doc.blocks.end());
                    current = context.row;
                    alignment = context.alignment;
                    styles = context.styles;
                    auto &cell = block().cells[context.cell];
                    cell.length = (uint32_t)doc.text.size() - cell.start;
                    cell.content = std::move(content);
                    for (auto &child : *cell.content)
                        block().runs.insert(block().runs.end(), child.runs.begin(), child.runs.end());
                }
            } else
                append(token);
            i = end + 1;
        }
    }
    void text(MD_TEXTTYPE t, const char *p, unsigned n) {
        locate(p);
        if (sourceMap && (uintptr_t)p >= (uintptr_t)input && (uintptr_t)p + n <= (uintptr_t)input + inputSize)
            textEnd = std::max(textEnd, (size_t)(p - input) + n);
        if (t == MD_TEXT_NULLCHAR) {
            append(L"�");
            return;
        }
        if (t == MD_TEXT_BR) {
            ++inlineGroup;
            append(L"\n");
            return;
        }
        if (t == MD_TEXT_SOFTBR) {
            append(L" ");
            return;
        }
        auto s = fromUtf8({p, n});
        if (t == MD_TEXT_ENTITY)
            append(entity(s, cancel));
        else if (t == MD_TEXT_HTML) {
            if (inHtml) {
                if (htmlBuffer.empty())
                    htmlSource = sourcePosition;
                htmlBuffer += s;
            } else
                html(s);
        } else if (t == MD_TEXT_NORMAL) {
            std::wstring collapsed;
            collapsed.reserve(s.size());
            bool space = current >= 0 && !doc.text.empty() && doc.text.back() == L' ';
            for (wchar_t c : s) {
                bool ws = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f';
                if (ws) {
                    if (!space)
                        collapsed.push_back(L' ');
                } else
                    collapsed.push_back(c);
                space = ws;
            }
            append(collapsed);
        } else
            append(s);
    }
    template <class F> static int guarded(void *u, F f) {
        auto &p = *(Parser *)u;
        try {
            p.check();
            f(p);
            return 0;
        } catch (...) {
            p.error = std::current_exception();
            return 1;
        }
    }
};
} // namespace
Document parse(std::wstring source, const std::atomic_bool *cancel, unsigned flags, bool sourceMap) {
#ifdef VMD_TESTING
    auto begin = std::chrono::steady_clock::now();
#endif
    if (source.size() > 500000000)
        throw std::runtime_error("Document exceeds the 500 million character safety limit.");
    Parser p{};
    p.cancel = cancel;
    p.doc.source = std::move(source);
    p.doc.text.reserve(p.doc.source.size());
    auto utf8 = toUtf8(p.doc.source);
    p.sourceMap = sourceMap;
    p.input = utf8.c_str();
    p.inputSize = utf8.size();
    MD_PARSER parser{};
    parser.flags = flags == 0xffffffff ? MD_DIALECT_GITHUB : flags;
    parser.enter_block = [](MD_BLOCKTYPE t, void *d, void *u) {
        return Parser::guarded(u, [&](Parser &p) { p.enter(t, d); });
    };
    parser.leave_block = [](MD_BLOCKTYPE t, void *d, void *u) {
        return Parser::guarded(u, [&](Parser &p) { p.leave(t, d); });
    };
    parser.enter_span = [](MD_SPANTYPE t, void *d, void *u) {
        return Parser::guarded(u, [&](Parser &p) { p.spanEnter(t, d); });
    };
    parser.leave_span = [](MD_SPANTYPE t, void *, void *u) {
        return Parser::guarded(u, [&](Parser &p) { p.spanLeave(t); });
    };
    parser.text = [](MD_TEXTTYPE t, const MD_CHAR *s, MD_SIZE n, void *u) {
        return Parser::guarded(u, [&](Parser &p) { p.text(t, s, n); });
    };
    int status = md_parse(utf8.data(), (MD_SIZE)utf8.size(), &parser, &p);
    if (p.error)
        std::rethrow_exception(p.error);
    if (status)
        throw std::runtime_error("Markdown parsing failed.");
    p.finish();
    // Soft line breaks between images are spaces in one Markdown paragraph.
    // Retain each image's offsets and link while sharing a wrapping layout.
    if (p.markdownImages > 1) {
        std::deque<Block> grouped;
        for (size_t i = 0; i < p.doc.blocks.size();) {
            auto &first = p.doc.blocks[i];
            size_t end = i + 1;
            if (first.image && first.image->inlineGroup) {
                size_t count = 1;
                for (size_t j = end; j < p.doc.blocks.size() && count < 256; ++j) {
                    p.check();
                    auto &next = p.doc.blocks[j];
                    if (next.start + next.length - first.start > 8192)
                        break;
                    if (next.image && next.image->inlineGroup == first.image->inlineGroup) {
                        end = j + 1;
                        ++count;
                    } else if (next.kind != Kind::Paragraph ||
                               !std::all_of(p.doc.text.begin() + next.start,
                                            p.doc.text.begin() + next.start + next.length,
                                            [](wchar_t c) { return c == L' ' || c == L'\t'; }))
                        break;
                }
            }
            if (end == i + 1) {
                grouped.push_back(std::move(first));
                ++i;
                continue;
            }
            Block row;
            row.start = first.start;
            row.indent = first.indent;
            row.quote = first.quote;
            row.align = first.align;
            auto &last = p.doc.blocks[end - 1];
            row.length = last.start + last.length - row.start;
            row.image = std::make_unique<ImageReference>();
            row.image->inlineImages = std::make_unique<std::deque<Block>>();
            for (; i < end; ++i) {
                auto &child = p.doc.blocks[i];
                if (!child.image)
                    continue;
                row.runs.insert(row.runs.end(), child.runs.begin(), child.runs.end());
                row.image->inlineImages->push_back(std::move(child));
            }
            grouped.push_back(std::move(row));
        }
        p.doc.blocks = std::move(grouped);
    }
    // Rich tables cannot split cell text independently of their native children.
    // Degrade oversized bands to those children in source order, preserving
    // images, links and formatting while keeping viewport layout work bounded.
    auto boundRichTables = [&](auto &&self, std::deque<Block> &blocks) -> void {
        std::deque<Block> bounded;
        for (auto &b : blocks) {
            p.check();
            size_t children = 0;
            for (auto &cell : b.cells)
                if (cell.content) {
                    self(self, *cell.content);
                    children += cell.content->size();
                }
            if (b.table && b.table->captions) {
                self(self, *b.table->captions);
                children += b.table->captions->size();
            }
            if (!children || (b.length <= 8192 && children <= 256 && b.cells.size() <= 256)) {
                bounded.push_back(std::move(b));
                continue;
            }
            std::vector<Block> content;
            if (b.table && b.table->captions)
                for (auto &child : *b.table->captions)
                    content.push_back(std::move(child));
            for (auto &cell : b.cells)
                if (cell.content)
                    for (auto &child : *cell.content)
                        content.push_back(std::move(child));
            std::stable_sort(content.begin(), content.end(),
                             [](const Block &a, const Block &b) { return a.start < b.start; });
            for (auto &child : content)
                bounded.push_back(std::move(child));
        }
        blocks = std::move(bounded);
    };
    if (p.richTables)
        boundRichTables(boundRichTables, p.doc.blocks);
    // Turn exceptionally tall table cells into bounded continuation rows. Keep
    // text/runs in the original logical order so selection and copying stay exact.
    bool needsSplit = std::any_of(p.doc.blocks.begin(), p.doc.blocks.end(), [](const Block &b) {
        return (!b.table && b.cells.size() > 256) ||
               std::any_of(b.cells.begin(), b.cells.end(),
                           [](const Cell &c) { return !c.content && c.length > 8192; });
    });
    if (needsSplit) {
        std::deque<Block> bounded;
        for (auto &b : p.doc.blocks) {
            if (!b.table && b.cells.size() > 256) {
                uint32_t position = b.start, limit = b.start + b.length;
                do {
                    p.check();
                    uint32_t end = std::min(limit, position + 8192);
                    if (end < limit && end > position && p.doc.text[end - 1] >= 0xd800 &&
                        p.doc.text[end - 1] <= 0xdbff && p.doc.text[end] >= 0xdc00 &&
                        p.doc.text[end] <= 0xdfff)
                        --end;
                    Block paragraph;
                    paragraph.start = position;
                    paragraph.length = end - position;
                    paragraph.indent = b.indent;
                    paragraph.quote = b.quote;
                    paragraph.header = b.header;
                    paragraph.continuation = position != b.start;
                    for (const auto &run : b.runs) {
                        auto lo = std::max(position, run.start), hi = std::min(end, run.start + run.length);
                        if (hi > lo) {
                            auto copy = run;
                            copy.start = lo;
                            copy.length = hi - lo;
                            paragraph.runs.push_back(std::move(copy));
                        }
                    }
                    bounded.push_back(std::move(paragraph));
                    position = end;
                } while (position < limit);
                continue;
            }
            size_t pieces = 1;
            for (auto &c : b.cells)
                if (!c.content)
                    pieces = std::max(pieces, ((size_t)c.length + 4095) / 4096);
            if (pieces == 1) {
                bounded.push_back(std::move(b));
                continue;
            }
            for (size_t part = 0; part < pieces; ++part) {
                if (cancel && cancel->load())
                    throw std::runtime_error("Cancelled");
                Block row;
                row.kind = Kind::Table;
                row.header = b.header;
                row.indent = b.indent;
                row.quote = b.quote;
                row.continuation = part != 0;
                row.start = UINT32_MAX;
                uint32_t end = 0;
                for (auto c : b.cells) {
                    auto boundary = [&](size_t requested) {
                        auto offset = std::min<size_t>(c.length, requested);
                        if (offset && offset < c.length && p.doc.text[c.start + offset] >= 0xdc00 &&
                            p.doc.text[c.start + offset] <= 0xdfff &&
                            p.doc.text[c.start + offset - 1] >= 0xd800 &&
                            p.doc.text[c.start + offset - 1] <= 0xdbff)
                            --offset;
                        return offset;
                    };
                    size_t offset = boundary(part * 4096), endOffset = boundary((part + 1) * 4096);
                    c.start += (uint32_t)offset;
                    c.length = (uint32_t)(endOffset - offset);
                    if (c.length) {
                        row.start = std::min(row.start, c.start);
                        end = std::max(end, c.start + c.length);
                    }
                    row.cells.push_back(c);
                }
                if (row.start == UINT32_MAX)
                    continue;
                row.length = end - row.start;
                for (auto &c : row.cells) {
                    if (!c.length)
                        c.start = row.start;
                    for (auto &r : b.runs) {
                        auto lo = std::max(c.start, r.start),
                             hi = std::min(c.start + c.length, r.start + r.length);
                        if (hi > lo) {
                            auto run = r;
                            run.start = lo;
                            run.length = hi - lo;
                            row.runs.push_back(std::move(run));
                        }
                    }
                }
                bounded.push_back(std::move(row));
            }
        }
        p.doc.blocks = std::move(bounded);
    }
#ifdef VMD_TESTING
    p.doc.parseMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
#endif
    if (sourceMap) {
        p.doc.sourceMapLength = (uint32_t)p.doc.source.size();
        std::stable_sort(p.sourceAnchors.begin(), p.sourceAnchors.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        uint32_t previous = 0;
        std::vector<uint32_t> lines{0};
        for (size_t i = 0; i < p.doc.source.size(); ++i)
            if (p.doc.source[i] == L'\n')
                lines.push_back((uint32_t)i + 1);
        for (const auto &b : p.doc.blocks) {
            auto found = std::upper_bound(p.sourceAnchors.begin(), p.sourceAnchors.end(), b.start,
                                          [](uint32_t at, const auto &a) { return at < a.first; });
            if (found != p.sourceAnchors.begin())
                previous = std::max(previous, std::prev(found)->second);
            auto line = std::upper_bound(lines.begin(), lines.end(), previous);
            p.doc.sourceStarts.push_back(*std::prev(line));
        }
    }
    return std::move(p.doc);
}
Document loadSource(const std::filesystem::path &path, const std::atomic_bool *cancel) {
    if (cancel && cancel->load())
        throw std::runtime_error("Cancelled");
#ifdef VMD_TESTING
    auto begin = std::chrono::steady_clock::now();
#endif
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("The file could not be opened.");
    auto size = f.tellg();
    if (size < 0 || size > 1000000000)
        throw std::runtime_error("The file is too large (1 GB byte limit).");
    if (cancel && cancel->load())
        throw std::runtime_error("Cancelled");
    std::string bytes((size_t)size, 0);
    f.seekg(0);
    size_t at = 0;
    while (at < bytes.size()) {
        if (cancel && cancel->load())
            throw std::runtime_error("Cancelled");
        auto n = std::min<size_t>(1024 * 1024, bytes.size() - at);
        if (!f.read(bytes.data() + at, n))
            throw std::runtime_error("The file changed or could not be read completely.");
        at += n;
    }
    if (cancel && cancel->load())
        throw std::runtime_error("Cancelled");
    Document doc;
    doc.source = decode(bytes, &doc.encoding);
    if (cancel && cancel->load())
        throw std::runtime_error("Cancelled");
    if (doc.source.size() > 500000000)
        throw std::runtime_error("Document exceeds the 500 million character safety limit.");
    doc.path = path;
#ifdef VMD_TESTING
    doc.readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
#endif
    return doc;
}
Document load(const std::filesystem::path &path, const std::atomic_bool *cancel) {
    auto source = loadSource(path, cancel);
    auto doc = parse(std::move(source.source), cancel);
    doc.path = std::move(source.path);
    doc.encoding = std::move(source.encoding);
#ifdef VMD_TESTING
    doc.readMs = source.readMs;
#endif
    return doc;
}
size_t blockAtPosition(const std::deque<Block> &blocks, uint32_t position) {
    auto after = std::upper_bound(blocks.begin(), blocks.end(), position,
                                  [](uint32_t at, const Block &block) { return at < block.start; });
    size_t candidate = after == blocks.begin() ? 0 : (size_t)(after - blocks.begin() - 1);
    if (blocks.empty() || blocks[candidate].kind != Kind::Table)
        return candidate;
    // Continuation rows interleave cells visually, so their text intervals overlap.
    for (size_t index = candidate + 1; index > 0;) {
        const auto &block = blocks[--index];
        if (block.kind != Kind::Table)
            break;
        for (const auto &cell : block.cells)
            if (position >= cell.start && position < cell.start + cell.length)
                return index;
    }
    return candidate;
}
std::deque<Block> sourceBlocks(std::wstring_view s) {
    std::deque<Block> blocks;
    size_t at = 0;
    while (at < s.size()) {
        size_t end = std::min(s.size(), at + 8192);
        if (end < s.size()) {
            auto lf = s.rfind(L'\n', end - 1);
            if (lf != s.npos && lf >= at)
                end = lf + 1;
            else if (s[end - 1] >= 0xd800 && s[end - 1] <= 0xdbff)
                --end;
        }
        Block b;
        b.kind = Kind::Code;
        b.start = (uint32_t)at;
        b.length = (uint32_t)(end - at);
        b.continuation = true;
        blocks.push_back(std::move(b));
        at = end;
    }
    return blocks;
}
} // namespace vm
