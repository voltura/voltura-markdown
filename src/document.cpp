#include "document.h"
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
#include <memory>
#include <stdexcept>

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
    if (b.starts_with("\xef\xbb\xbf"))
        b.remove_prefix(3);
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
                    unsigned long c = wcstoul(n.c_str() + (hex ? 1 : 0), &tail, hex ? 16 : 10);
                    if (tail && *tail == 0 && tail != n.c_str() + (hex ? 1 : 0)) {
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
    const std::atomic_bool *cancel;
    std::exception_ptr error;
    int current = -1;
    unsigned quote = 0, styles = 0;
    struct List {
        bool ordered;
        unsigned next;
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
        size_t listDepth;
        bool inCode;
    };
    std::vector<HtmlBlock> htmlBlocks;
    std::wstring link, prefix;
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
        b.indent = (unsigned)lists.size();
        b.quote = quote;
        b.level = heading;
        doc.blocks.push_back(std::move(b));
        current = (int)doc.blocks.size() - 1;
        if (!prefix.empty()) {
            auto p = std::move(prefix);
            prefix.clear();
            append(p);
        }
    }
    void finish() {
        if (current < 0)
            return;
        auto &b = doc.blocks[current];
        b.length = (uint32_t)doc.text.size() - b.start;
        if (b.kind == Kind::Heading) {
            std::wstring id = doc.text.substr(b.start, b.length);
            for (auto &c : id)
                c = (wchar_t)towlower(c);
            id.erase(std::remove_if(id.begin(), id.end(),
                                    [](wchar_t c) { return iswpunct(c) && c != L'-' && c != L'_'; }),
                     id.end());
            std::replace(id.begin(), id.end(), L' ', L'-');
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
                copy.level = heading;
                copy.continuation = true;
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
            start();
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
            inImage = true;
            start(Kind::Image);
            block().image = std::make_unique<std::wstring>(attr(((MD_SPAN_IMG_DETAIL *)d)->src));
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
        lists.resize(context.listDepth);
        inCode = context.inCode;
        prefix.clear();
    }
    bool structuralHtml(const std::wstring &tag, std::wstring_view token, bool close) {
        bool isHeading = tag.size() == 2 && tag[0] == L'h' && tag[1] >= L'1' && tag[1] <= L'6';
        bool isList = tag == L"ul" || tag == L"ol";
        bool wrapper = tag == L"section" || tag == L"article" || tag == L"main" || tag == L"nav" ||
                       tag == L"aside" || tag == L"header" || tag == L"footer" || tag == L"details";
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
        htmlBlocks.push_back({tag, heading, quote, styles, lists.size(), inCode});
        if (isHeading) {
            heading = tag[1] - L'0';
            start(Kind::Heading);
        } else if (tag == L"blockquote") {
            ++quote;
        } else if (isList) {
            unsigned number = 1;
            if (tag == L"ol") {
                auto value = attribute(token, L"start");
                wchar_t *tail = nullptr;
                unsigned long parsed = wcstoul(value.c_str(), &tail, 10);
                if (!value.empty() && value.front() != L'-' && tail && *tail == 0 && parsed > 0 &&
                    parsed < UINT32_MAX)
                    number = (unsigned)parsed;
            }
            lists.push_back({tag == L"ol", number});
        } else if (tag == L"li") {
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
            if (structuralHtml(tag, token, close)) {
                i = end + 1;
                continue;
            }
            unsigned style = tag == L"b" || tag == L"strong" ? Bold
                             : tag == L"i" || tag == L"em"   ? Italic
                             : tag == L"code"                ? Mono
                             : tag == L"s" || tag == L"del"  ? Strike
                             : tag == L"kbd"                 ? Keycap | Mono
                             : tag == L"u"                   ? Underline
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
            } else if (tag == L"span") {
                // Harmless wrappers retain their content; CSS and attributes are ignored.
            } else if (tag == L"hr" && !close) {
                finish();
                start(Kind::Rule);
                finish();
            } else if (tag == L"br") {
                append(L"\n");
            } else if (tag == L"p" || tag == L"div") {
                finish();
            } else if (tag == L"a") {
                if (close) {
                    link.clear();
                    styles &= ~Underline;
                } else {
                    link = attribute(token, L"href");
                    styles |= Underline;
                }
            } else if (tag == L"img" && !close) {
                finish();
                start(Kind::Image);
                block().image = std::make_unique<std::wstring>(attribute(token, L"src"));
                append(attribute(token, L"alt"));
                finish();
            } else if (tag == L"table" || tag == L"thead" || tag == L"tbody") {
                finish();
            } else if (tag == L"tr") {
                finish();
                if (!close)
                    start(Kind::Table);
            } else if (tag == L"th" || tag == L"td") {
                if (!close) {
                    if (block().kind != Kind::Table) {
                        finish();
                        start(Kind::Table);
                    }
                    if (!block().cells.empty())
                        append(L"\t");
                    block().cells.push_back({(uint32_t)doc.text.size(), 0, 0});
                    if (tag == L"th")
                        block().header = true;
                } else if (current >= 0 && !block().cells.empty())
                    block().cells.back().length = (uint32_t)doc.text.size() - block().cells.back().start;
            } else
                append(token);
            i = end + 1;
        }
    }
    void text(MD_TEXTTYPE t, const char *p, unsigned n) {
        if (t == MD_TEXT_NULLCHAR) {
            append(L"�");
            return;
        }
        if (t == MD_TEXT_BR) {
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
        else if (t == MD_TEXT_HTML)
            html(s);
        else if (t == MD_TEXT_NORMAL) {
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
Document parse(std::wstring source, const std::atomic_bool *cancel, unsigned flags) {
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
    // Turn exceptionally tall table cells into bounded continuation rows. Keep
    // text/runs in the original logical order so selection and copying stay exact.
    bool needsSplit = std::any_of(p.doc.blocks.begin(), p.doc.blocks.end(), [](const Block &b) {
        return b.cells.size() > 256 ||
               std::any_of(b.cells.begin(), b.cells.end(), [](const Cell &c) { return c.length > 8192; });
    });
    if (needsSplit) {
        std::deque<Block> bounded;
        for (auto &b : p.doc.blocks) {
            if (b.cells.size() > 256) {
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
