#include "editor-tools.h"
#include "images.h"
#include "md4c.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <richedit.h>
#include <richole.h>
#include <shlwapi.h>
#include <sstream>
#include <stdexcept>

namespace vm {
namespace {
std::wstring_view trim(std::wstring_view s) {
    while (!s.empty() && iswspace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && iswspace(s.back()))
        s.remove_suffix(1);
    return s;
}
std::wstring escape(std::wstring_view s) {
    std::wstring out;
    for (auto c : s) {
        if (c == '&')
            out += L"&amp;";
        else if (c == '<')
            out += L"&lt;";
        else if (c == '>')
            out += L"&gt;";
        else if (c == '"')
            out += L"&quot;";
        else
            out += c;
    }
    return out;
}
std::wstring safeUrl(std::wstring_view value) {
    std::wstring test(value);
    std::transform(test.begin(), test.end(), test.begin(), towlower);
    if (test.find_first_of(L"\r\n\t") != test.npos)
        return {};
    auto colon = test.find(L':');
    if (colon != test.npos && !test.starts_with(L"https:") && !test.starts_with(L"http:") &&
        !test.starts_with(L"mailto:") && !test.starts_with(L"file:"))
        return {};
    return escape(value);
}
std::wstring exportUrl(std::wstring_view value, const std::filesystem::path &document) {
    bool drive =
        value.size() > 2 && iswalpha(value[0]) && value[1] == ':' && (value[2] == '/' || value[2] == '\\');
    if (value.empty() || value[0] == '#' || (!drive && value.find(':') != value.npos) ||
        value.starts_with(L"//"))
        return safeUrl(value);
    auto split = value.find_first_of(L"#?");
    auto pathPart = std::wstring(value.substr(0, split));
    auto suffix = split == value.npos ? L"" : std::wstring(value.substr(split));
    std::wstring decoded(pathPart.size() + 1, L'\0');
    DWORD size = (DWORD)decoded.size();
    if (FAILED(UrlUnescapeW(pathPart.data(), decoded.data(), &size, URL_UNESCAPE_AS_UTF8)))
        return {};
    decoded.resize(size);
    std::filesystem::path path(decoded);
    if (path.is_relative()) {
        if (document.empty())
            return safeUrl(value);
        path = std::filesystem::absolute(document).parent_path() / path;
    }
    std::wstring url(32768, L'\0');
    size = (DWORD)url.size();
    if (FAILED(UrlCreateFromPathW(path.lexically_normal().c_str(), url.data(), &size, 0)))
        return {};
    url.resize(size);
    return safeUrl(url + suffix);
}
} // namespace
std::vector<HeadingEntry> sourceOutline(std::wstring_view source) {
    struct State {
        std::string bytes;
        std::vector<HeadingEntry> headings;
        bool heading = false;
        unsigned level = 0;
        size_t offset = SIZE_MAX, byte = 0, units = 0;
        std::wstring title;
        std::exception_ptr error;
    } state{toUtf8(source)};
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = [](MD_BLOCKTYPE type, void *detail, void *data) -> int {
        auto &s = *(State *)data;
        if (type == MD_BLOCK_H) {
            s.heading = true;
            s.level = ((MD_BLOCK_H_DETAIL *)detail)->level;
            s.offset = SIZE_MAX;
            s.title.clear();
        }
        return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void *, void *data) -> int {
        auto &s = *(State *)data;
        try {
            if (type == MD_BLOCK_H) {
                s.heading = false;
                if (s.offset != SIZE_MAX) {
                    s.units += fromUtf8(std::string_view(s.bytes).substr(s.byte, s.offset - s.byte)).size();
                    s.byte = s.offset;
                    s.headings.push_back({s.units, s.level, s.title});
                }
            }
        } catch (...) {
            s.error = std::current_exception();
            return 1;
        }
        return 0;
    };
    parser.enter_span = parser.leave_span = [](MD_SPANTYPE, void *, void *) -> int { return 0; };
    parser.text = [](MD_TEXTTYPE type, const char *text, MD_SIZE size, void *data) -> int {
        auto &s = *(State *)data;
        if (!s.heading)
            return 0;
        try {
            auto pointer = (uintptr_t)text, begin = (uintptr_t)s.bytes.data();
            if (s.offset == SIZE_MAX && pointer >= begin && pointer < begin + s.bytes.size()) {
                auto at = (size_t)(pointer - begin);
                auto newline = at ? s.bytes.rfind('\n', at - 1) : std::string::npos;
                s.offset = newline == std::string::npos ? 0 : newline + 1;
            }
            if (s.title.size() < 512) {
                auto value = fromUtf8(std::string_view(text, size));
                s.title += type == MD_TEXT_ENTITY ? entity(value) : value;
            }
        } catch (...) {
            s.error = std::current_exception();
            return 1;
        }
        return 0;
    };
    md_parse(state.bytes.data(), (MD_SIZE)state.bytes.size(), &parser, &state);
    if (state.error)
        std::rethrow_exception(state.error);
    return state.headings;
}
std::optional<TextEdit> continueMarkdown(std::wstring_view line, size_t caret) {
    caret = std::min(caret, line.size());
    auto prefix = line.substr(0, caret);
    size_t p = 0;
    while (p < prefix.size() && (prefix[p] == ' ' || prefix[p] == '\t'))
        ++p;
    size_t indent = p;
    while (p < prefix.size() && prefix[p] == '>') {
        ++p;
        if (p < prefix.size() && prefix[p] == ' ')
            ++p;
    }
    size_t quote = p;
    std::wstring marker;
    if (p + 1 < prefix.size() && (prefix[p] == '-' || prefix[p] == '+' || prefix[p] == '*') &&
        prefix[p + 1] == ' ') {
        marker = std::wstring(prefix.substr(p, 2));
        p += 2;
        if (p + 3 < prefix.size() && prefix[p] == '[' &&
            (prefix[p + 1] == ' ' || prefix[p + 1] == 'x' || prefix[p + 1] == 'X') && prefix[p + 2] == ']' &&
            prefix[p + 3] == ' ') {
            marker += L"[ ] ";
            p += 4;
        }
    } else {
        size_t digits = p;
        while (p < prefix.size() && iswdigit(prefix[p]))
            ++p;
        if (p > digits && p - digits <= 9 && p + 1 < prefix.size() &&
            (prefix[p] == '.' || prefix[p] == ')') && prefix[p + 1] == ' ') {
            marker = std::to_wstring(std::stoul(std::wstring(prefix.substr(digits, p - digits))) + 1) +
                     prefix[p] + L" ";
            p += 2;
        } else
            p = quote;
    }
    if (marker.empty() && quote == indent && !indent)
        return {};
    if (p > indent && trim(prefix.substr(p)).empty() && caret == line.size())
        return TextEdit{indent, caret - indent, L"", 0, 0};
    std::wstring next = L"\n" + std::wstring(prefix.substr(0, quote)) + marker;
    return TextEdit{caret, 0, next, next.size(), 0};
}
TextEdit indentMarkdown(std::wstring_view s, size_t start, size_t end, bool outdent) {
    start = std::min(start, s.size());
    end = std::clamp(end, start, s.size());
    size_t left = start ? s.rfind(L'\n', start - 1) : s.npos;
    left = left == s.npos ? 0 : left + 1;
    size_t right = s.find(L'\n', end > start && s[end - 1] == '\n' ? end - 1 : end);
    if (right == s.npos)
        right = s.size();
    TextEdit edit{left, right - left};
    for (size_t at = left; at <= right;) {
        size_t e = s.find(L'\n', at);
        if (e == s.npos || e > right)
            e = right;
        auto line = s.substr(at, e - at);
        if (outdent) {
            size_t n = 0;
            while (n < line.size() && n < 4 && line[n] == ' ')
                ++n;
            if (!n && !line.empty() && line[0] == '\t')
                n = 1;
            edit.replacement += line.substr(n);
        } else
            edit.replacement += L"    " + std::wstring(line);
        if (e == right)
            break;
        edit.replacement += L'\n';
        at = e + 1;
    }
    edit.selected = edit.replacement.size();
    return edit;
}
std::optional<TextEdit> editTable(std::wstring_view s, size_t caret, TableAction action) {
    caret = std::min(caret, s.size());
    size_t start = caret ? s.rfind(L'\n', caret - 1) : s.npos;
    start = start == s.npos ? 0 : start + 1;
    auto lineAt = [&](size_t at) {
        auto e = s.find(L'\n', at);
        return s.substr(at, (e == s.npos ? s.size() : e) - at);
    };
    if (lineAt(start).find('|') == s.npos)
        return {};
    while (start) {
        auto p = start > 1 ? s.rfind(L'\n', start - 2) : s.npos;
        p = p == s.npos ? 0 : p + 1;
        if (lineAt(p).find('|') == s.npos)
            break;
        start = p;
    }
    std::vector<std::vector<std::wstring>> rows;
    size_t end = start, active = 0, column = 0;
    while (end < s.size() && lineAt(end).find('|') != s.npos) {
        if (end - start > 1024 * 1024 || rows.size() >= 10000 || lineAt(end).size() > 32768)
            return {};
        auto raw = lineAt(end), line = trim(raw);
        std::vector<std::wstring> cells;
        size_t rawOffset = (size_t)(line.data() - raw.data());
        if (!line.empty() && line.front() == '|') {
            line.remove_prefix(1);
            ++rawOffset;
        }
        std::vector<size_t> pipes;
        size_t code = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && !code) {
                ++i;
                continue;
            }
            if (line[i] == '`') {
                auto begin = i;
                while (i + 1 < line.size() && line[i + 1] == '`')
                    ++i;
                auto run = i - begin + 1;
                if (!code) {
                    for (size_t q = i + 1; q < line.size(); ++q) {
                        if (line[q] != '`')
                            continue;
                        auto closingBegin = q;
                        while (q + 1 < line.size() && line[q + 1] == '`')
                            ++q;
                        if (q - closingBegin + 1 == run) {
                            code = run;
                            break;
                        }
                    }
                } else if (code == run)
                    code = 0;
                continue;
            }
            if (line[i] == '|' && !code)
                pipes.push_back(i);
        }
        size_t a = 0;
        for (auto pipe : pipes) {
            cells.emplace_back(trim(line.substr(a, pipe - a)));
            a = pipe + 1;
        }
        if (a < line.size() || pipes.empty())
            cells.emplace_back(trim(line.substr(a)));
        if (caret >= end && caret <= end + lineAt(end).size()) {
            active = rows.size();
            column = 0;
            for (auto pipe : pipes)
                if (end + rawOffset + pipe < caret && pipe + 1 < line.size())
                    ++column;
        }
        rows.push_back(std::move(cells));
        end += lineAt(end).size();
        if (end < s.size())
            ++end;
        else
            break;
    }
    if (rows.size() < 2 || rows[0].empty())
        return {};
    size_t cols = rows[0].size();
    if (rows[1].size() != cols)
        return {};
    for (auto &cell : rows[1]) {
        auto t = trim(cell);
        if (!t.empty() && t.front() == ':')
            t.remove_prefix(1);
        if (!t.empty() && t.back() == ':')
            t.remove_suffix(1);
        if (t.empty() || t.find_first_not_of('-') != t.npos)
            return {};
    }
    if (cols > 256 || rows.size() > 10000)
        return {};
    for (auto &row : rows) {
        if (row.size() > cols)
            return {};
        row.resize(cols);
    }
    column = std::min(column, cols - 1);
    switch (action) {
    case TableAction::AddRow:
        rows.insert(rows.begin() + std::max<size_t>(2, active + 1), std::vector<std::wstring>(cols, L"Cell"));
        break;
    case TableAction::DeleteRow:
        if (active < 2)
            return {};
        rows.erase(rows.begin() + active);
        break;
    case TableAction::AddColumn:
        if (cols == 256)
            return {};
        for (size_t i = 0; i < rows.size(); ++i)
            rows[i].insert(rows[i].begin() + column + 1, i == 1 ? L"---" : L"Cell");
        break;
    case TableAction::DeleteColumn:
        if (cols == 1)
            return {};
        for (auto &row : rows)
            row.erase(row.begin() + column);
        break;
    default:
        rows[1][column] = action == TableAction::Left     ? L":---"
                          : action == TableAction::Center ? L":---:"
                                                          : L"---:";
    }
    TextEdit edit{start, end - start};
    for (auto &row : rows) {
        edit.replacement += L"|";
        for (auto &cell : row)
            edit.replacement += L" " + cell + L" |";
        edit.replacement += L'\n';
    }
    edit.selection = std::min(caret - start, edit.replacement.size());
    return edit;
}
FenceState scanFenceLine(std::wstring_view line, FenceState state) {
    auto raw = line;
    unsigned quotes = 0, indent = 0;
    if (state.marker) {
        // Closing delimiters may shed only the containers of their opening delimiter.
        for (unsigned q = 0; q < state.quotes; ++q) {
            auto n = line.find_first_not_of(L" ");
            if (n == line.npos)
                return state;
            if (n > 3 || line[n] != '>')
                return scanFenceLine(raw, {});
            line.remove_prefix(n + 1);
            if (!line.empty() && line.front() == ' ')
                line.remove_prefix(1);
        }
        if (state.indent) {
            auto n = line.find_first_not_of(L" ");
            if (n == line.npos)
                return state;
            if (n < state.indent)
                return scanFenceLine(raw, {});
            line.remove_prefix(state.indent);
        }
    } else {
        for (;;) {
            auto n = line.find_first_not_of(L" ");
            if (n == line.npos)
                return {};
            if (n > 3)
                break;
            if (line[n] != '>')
                break;
            ++quotes;
            line.remove_prefix(n + 1);
            if (!line.empty() && line.front() == ' ')
                line.remove_prefix(1);
        }
        auto n = line.find_first_not_of(L" ");
        if (n == line.npos || n > 3)
            return {};
        line.remove_prefix(n);
        size_t marker = 0;
        if (line.size() > 2 && (line[0] == '-' || line[0] == '+' || line[0] == '*') && line[1] == ' ')
            marker = 2;
        else {
            size_t digits = 0;
            while (digits < line.size() && iswdigit(line[digits]))
                ++digits;
            if (digits && digits <= 9 && digits + 1 < line.size() &&
                (line[digits] == '.' || line[digits] == ')') && line[digits + 1] == ' ')
                marker = digits + 2;
        }
        if (marker) {
            line.remove_prefix(marker);
            indent = (unsigned)(n + marker);
        }
    }
    auto leading = line.find_first_not_of(L" ");
    if (leading == line.npos || leading > 3)
        return state;
    if (!state.marker && indent)
        indent += (unsigned)leading;
    line.remove_prefix(leading);
    if (line.front() != '`' && line.front() != '~')
        return state;
    size_t n = 0;
    while (n < line.size() && line[n] == line.front())
        ++n;
    if (n < 3)
        return state;
    if (!state.marker) {
        if (line.front() == '`' && line.substr(n).find('`') != line.npos)
            return {};
        return {line.front(), n, quotes, indent};
    }
    if (state.marker == line.front() && n >= state.length && trim(line.substr(n)).empty())
        return {};
    return state;
}
std::vector<FencePoint> sourceFences(std::wstring_view source) {
    std::vector<FencePoint> result{{0, {}}};
    FenceState state;
    for (size_t at = 0; at < source.size();) {
        auto end = source.find(L'\n', at);
        if (end == source.npos)
            end = source.size();
        auto next = scanFenceLine(source.substr(at, end - at), state);
        if (next != state) {
            auto opening = scanFenceLine(source.substr(at, end - at), {});
            bool implicitEnd = state.marker && !next.marker && !opening.marker;
            result.push_back({implicitEnd ? at : end + 1, next});
        }
        state = next;
        at = end + 1;
    }
    return result;
}
std::vector<SyntaxSpan> syntaxSpans(std::wstring_view s, FenceState state, bool partialFirstLine) {
    std::vector<SyntaxSpan> spans;
    for (size_t at = 0; at < s.size();) {
        size_t e = s.find(L'\n', at);
        if (e == s.npos)
            e = s.size();
        auto line = s.substr(at, e - at), t = trim(line);
        auto next = partialFirstLine && !at ? state : scanFenceLine(line, state);
        bool code = state.marker != 0 && next.marker != 0;
        if (next != state) {
            spans.push_back({at, e - at, 1});
        } else if (!code && (t.starts_with(L"#") || t.starts_with(L">") || t.starts_with(L"- ") ||
                             t.starts_with(L"* ")))
            spans.push_back({at, e - at, 1});
        for (size_t p = at; p < e; ++p) {
            if (s[p] == '"' || s[p] == '\'' || (!code && s[p] == '`')) {
                auto q = s.find(s[p], p + 1);
                if (q < e) {
                    spans.push_back({p, q - p + 1, 2});
                    p = q;
                }
            } else if (code && p + 1 < e && s[p] == '/' && s[p + 1] == '/') {
                spans.push_back({p, e - p, 3});
                break;
            } else if (code && iswdigit(s[p]) && (p == at || !iswalnum(s[p - 1]))) {
                auto q = p + 1;
                while (q < e && (iswdigit(s[q]) || s[q] == '.'))
                    ++q;
                spans.push_back({p, q - p, 2});
                p = q - 1;
            } else if (code && iswalpha(s[p])) {
                auto q = p + 1;
                while (q < e && (iswalnum(s[q]) || s[q] == '_'))
                    ++q;
                auto token = s.substr(p, q - p);
                constexpr std::wstring_view keys[] = {L"if",     L"else",   L"for",     L"while", L"return",
                                                      L"class",  L"const",  L"let",     L"var",   L"function",
                                                      L"def",    L"public", L"private", L"void",  L"int",
                                                      L"string", L"true",   L"false",   L"null",  L"import",
                                                      L"from",   L"using",  L"new",     L"async", L"await"};
                if (std::find(std::begin(keys), std::end(keys), token) != std::end(keys))
                    spans.push_back({p, q - p, 1});
                p = q - 1;
            } else if (!code && (s[p] == '[' || s[p] == '*' || s[p] == '_' || s[p] == '~'))
                spans.push_back({p, 1, 1});
        }
        state = next;
        at = e + 1;
    }
    return spans;
}
std::wstring replaceMatches(std::wstring_view source, std::wstring_view needle, std::wstring_view replacement,
                            bool matchCase, bool wholeWord, const std::atomic_bool &cancel) {
    if (needle.empty())
        return std::wstring(source);
    std::wstring haystack(source), query(needle);
    if (!matchCase) {
        CharUpperBuffW(haystack.data(), (DWORD)haystack.size());
        CharUpperBuffW(query.data(), (DWORD)query.size());
    }
    std::wstring out;
    out.reserve(source.size());
    size_t at = 0;
    auto word = [](wchar_t c) {
        WORD type = 0;
        GetStringTypeW(CT_CTYPE1, &c, 1, &type);
        return (type & (C1_ALPHA | C1_DIGIT)) || c == '_';
    };
    while (at < source.size()) {
        if (cancel.load())
            return {};
        auto found = haystack.find(query, at);
        if (found == haystack.npos) {
            out += source.substr(at);
            break;
        }
        bool match =
            !wholeWord || ((!found || !word(source[found - 1])) &&
                           (found + query.size() == source.size() || !word(source[found + query.size()])));
        out += source.substr(at, found - at);
        out += match ? replacement : source.substr(found, query.size());
        at = found + query.size();
        if (out.size() > 500000000)
            throw std::runtime_error("The replacement would make this document too large.");
    }
    return out;
}
std::wstring rebaseLocalLinks(std::wstring_view source, const std::filesystem::path &from,
                              const std::filesystem::path &to) {
    if (from.empty() ||
        std::filesystem::absolute(from).parent_path() == std::filesystem::absolute(to).parent_path())
        return std::wstring(source);
    struct State {
        std::string bytes;
        std::filesystem::path from, to;
        std::map<size_t, std::pair<size_t, std::string>> replacements;
        std::exception_ptr error;
    } state{toUtf8(source), std::filesystem::absolute(from).parent_path(),
            std::filesystem::absolute(to).parent_path()};
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = parser.leave_block = [](MD_BLOCKTYPE, void *, void *) -> int { return 0; };
    parser.leave_span = [](MD_SPANTYPE, void *, void *) -> int { return 0; };
    parser.text = [](MD_TEXTTYPE, const char *, MD_SIZE, void *) -> int { return 0; };
    parser.enter_span = [](MD_SPANTYPE type, void *detail, void *data) -> int {
        auto &s = *(State *)data;
        try {
            if (type != MD_SPAN_A && type != MD_SPAN_IMG)
                return 0;
            auto attr =
                type == MD_SPAN_A ? ((MD_SPAN_A_DETAIL *)detail)->href : ((MD_SPAN_IMG_DETAIL *)detail)->src;
            auto begin = (uintptr_t)s.bytes.data(), pointer = (uintptr_t)attr.text;
            if (pointer < begin || pointer + attr.size > begin + s.bytes.size())
                return 0;
            auto raw = entity(fromUtf8(std::string_view(attr.text, attr.size)));
            if (raw.empty() || raw[0] == '#' || raw.find(':') != raw.npos || raw.starts_with(L"//"))
                return 0;
            auto suffix = raw.find_first_of(L"#?");
            auto path = raw.substr(0, suffix), fragment = suffix == raw.npos ? L"" : raw.substr(suffix);
            std::wstring unescaped;
            for (size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '\\' && i + 1 < path.size() && iswpunct(path[i + 1]))
                    ++i;
                unescaped += path[i];
            }
            std::wstring decoded(unescaped.size() + 1, L'\0');
            DWORD n = (DWORD)decoded.size();
            if (FAILED(UrlUnescapeW(unescaped.data(), decoded.data(), &n, URL_UNESCAPE_AS_UTF8)))
                return 0;
            decoded.resize(n);
            std::filesystem::path target(decoded);
            if (target.is_absolute())
                return 0;
            target = (s.from / target).lexically_normal();
            auto relative = target.lexically_relative(s.to);
            auto destination = markdownFileDestination(relative.empty() ? target : relative);
            std::wstring encoded;
            for (auto c : destination) {
                if (c == ' ')
                    encoded += L"%20";
                else if (c == '(')
                    encoded += L"%28";
                else if (c == ')')
                    encoded += L"%29";
                else if (c == '<')
                    encoded += L"%3C";
                else if (c == '>')
                    encoded += L"%3E";
                else
                    encoded += c;
            }
            s.replacements[(size_t)(pointer - begin)] = {attr.size, toUtf8(encoded + fragment)};
        } catch (...) {
            s.error = std::current_exception();
            return 1;
        }
        return 0;
    };
    md_parse(state.bytes.data(), (MD_SIZE)state.bytes.size(), &parser, &state);
    if (state.error)
        std::rethrow_exception(state.error);
    std::string result;
    size_t at = 0;
    for (auto &[offset, replacement] : state.replacements) {
        if (offset < at)
            continue;
        result.append(state.bytes, at, offset - at);
        result += replacement.second;
        at = offset + replacement.first;
    }
    result.append(state.bytes, at);
    return fromUtf8(result);
}
std::string exportHtml(const Document &d) {
    std::wstring html =
        L"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
        L"content=\"width=device-width\"><meta http-equiv=\"Content-Security-Policy\" content=\"default-src "
        L"'none'; img-src https: http: file:; style-src 'unsafe-inline'; base-uri 'none'\"><title>" +
        escape(d.path.empty() ? L"Untitled" : d.path.filename().wstring()) +
        L"</title><style>body{font:17px/1.6 system-ui,sans-serif;max-width:900px;margin:40px auto;padding:0 "
        L"24px;color:#202938}p,td,th,blockquote{white-space:pre-wrap}pre,code{font-family:Consolas,monospace;"
        L"background:#f0f2f5}pre{padding:16px;"
        L"white-space:pre-wrap}table{border-collapse:collapse;width:100%}td,th{border:1px solid "
        L"#ccd2dc;padding:6px 12px}img{max-width:100%}blockquote{border-left:3px solid "
        L"#ccd2dc;margin-left:0;padding-left:20px}@media "
        L"print{body{max-width:none;margin:0}pre,table,img{break-inside:avoid}a{color:inherit}}</style></"
        L"head><body>";
    auto inlineText = [&](const Block &b, size_t start, size_t length) {
        std::wstring out;
        size_t end = std::min(start + length, d.text.size()), at = start;
        for (auto &run : b.runs) {
            size_t a = std::max<size_t>(start, run.start),
                   e = std::min<size_t>(end, (size_t)run.start + run.length);
            if (e <= a)
                continue;
            if (a > at)
                out += escape(std::wstring_view(d.text).substr(at, a - at));
            auto link = exportUrl(run.link, d.path);
            if (!link.empty())
                out += L"<a href=\"" + link + L"\">";
            if (run.style & Bold)
                out += L"<strong>";
            if (run.style & Italic)
                out += L"<em>";
            if (run.style & Underline)
                out += L"<u>";
            if (run.style & Strike)
                out += L"<del>";
            if (run.style & Mono)
                out += L"<code>";
            out += escape(std::wstring_view(d.text).substr(a, e - a));
            if (run.style & Mono)
                out += L"</code>";
            if (run.style & Strike)
                out += L"</del>";
            if (run.style & Underline)
                out += L"</u>";
            if (run.style & Italic)
                out += L"</em>";
            if (run.style & Bold)
                out += L"</strong>";
            if (!link.empty())
                out += L"</a>";
            at = e;
        }
        if (at < end)
            out += escape(std::wstring_view(d.text).substr(at, end - at));
        return out;
    };
    bool table = false;
    for (auto &b : d.blocks) {
        if (table && b.kind != Kind::Table) {
            html += L"</table>";
            table = false;
        }
        if (b.kind == Kind::Table) {
            if (!table) {
                html += L"<table>";
                table = true;
            }
            html += L"<tr>";
            for (auto &cell : b.cells) {
                auto tag = b.header ? L"th" : L"td";
                html += L"<" + std::wstring(tag) + L" style=\"text-align:" +
                        (cell.align == 3   ? L"right"
                         : cell.align == 2 ? L"center"
                                           : L"left") +
                        L"\">" + inlineText(b, cell.start, cell.length) + L"</" + tag + L">";
            }
            html += L"</tr>";
        } else if (b.kind == Kind::Rule)
            html += L"<hr>";
        else if (b.kind == Kind::Image && b.image) {
            auto url = exportUrl(*b.image, d.path);
            html += L"<p><img src=\"" + url + L"\" alt=\"" +
                    escape(std::wstring_view(d.text).substr(b.start, b.length)) + L"\"></p>";
        } else {
            auto tag = b.kind == Kind::Heading ? L"h" + std::to_wstring(std::clamp(b.level, 1u, 6u))
                       : b.kind == Kind::Code  ? L"pre"
                                               : L"p";
            if (b.quote)
                html += L"<blockquote>";
            html += L"<" + tag;
            if (b.anchor)
                html += L" id=\"" + escape(*b.anchor) + L"\"";
            html += L">" + inlineText(b, b.start, b.length) + L"</" + tag + L">";
            if (b.quote)
                html += L"</blockquote>";
        }
    }
    if (table)
        html += L"</table>";
    html += L"</body></html>";
    return toUtf8(html);
}
void printDocument(const Document &document, HDC printer, const std::filesystem::path &pdf,
                   const std::atomic_bool *cancel) {
    auto active = [&] {
        if (cancel && cancel->load())
            throw std::runtime_error("Printing cancelled.");
    };
    struct Com {
        HRESULT status = OleInitialize(nullptr);
        ~Com() {
            if (SUCCEEDED(status))
                OleUninitialize();
        }
    } com;
    HMODULE library = LoadLibraryW(L"Msftedit.dll");
    if (!library)
        throw std::runtime_error("Windows text printing is unavailable.");
    HWND text = CreateWindowExW(0, MSFTEDIT_CLASS, L"", ES_MULTILINE, 0, 0, 1, 1, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    bool started = false;
    try {
        if (!text)
            throw std::runtime_error("Could not prepare the print document.");
        class Storage final : public IRichEditOleCallback {
            ULONG refs = 1;

          public:
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
                if (!out)
                    return E_POINTER;
                *out = nullptr;
                if (id == __uuidof(IUnknown) || id == IID_IRichEditOleCallback) {
                    *out = this;
                    AddRef();
                    return S_OK;
                }
                return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override {
                return ++refs;
            }
            ULONG STDMETHODCALLTYPE Release() override {
                auto n = --refs;
                if (!n)
                    delete this;
                return n;
            }
            HRESULT STDMETHODCALLTYPE GetNewStorage(IStorage **out) override {
                ILockBytes *bytes = nullptr;
                auto result = CreateILockBytesOnHGlobal(nullptr, TRUE, &bytes);
                if (FAILED(result))
                    return result;
                result = StgCreateDocfileOnILockBytes(
                    bytes, STGM_SHARE_EXCLUSIVE | STGM_READWRITE | STGM_CREATE, 0, out);
                bytes->Release();
                return result;
            }
            HRESULT STDMETHODCALLTYPE GetInPlaceContext(IOleInPlaceFrame **, IOleInPlaceUIWindow **,
                                                        LPOLEINPLACEFRAMEINFO) override {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE ShowContainerUI(BOOL) override {
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE QueryInsertObject(LPCLSID, IStorage *, LONG) override {
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE DeleteObject(IOleObject *) override {
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE QueryAcceptData(IDataObject *, CLIPFORMAT *, DWORD, BOOL,
                                                      HGLOBAL) override {
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override {
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE GetClipboardData(CHARRANGE *, DWORD, IDataObject **) override {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE GetDragDropEffect(BOOL, DWORD, LPDWORD) override {
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE GetContextMenu(WORD, IOleObject *, CHARRANGE *, HMENU *) override {
                return E_NOTIMPL;
            }
        };
        auto storage = new Storage;
        SendMessageW(text, EM_SETOLECALLBACK, 0, (LPARAM)storage);
        storage->Release();
        SendMessageW(text, EM_SETTEXTMODE, TM_RICHTEXT, 0);
        SendMessageW(text, EM_EXLIMITTEXT, 0, 500000000);
        SETTEXTEX set{ST_DEFAULT, 1200};
        SendMessageW(text, EM_SETTEXTEX, (WPARAM)&set, (LPARAM)document.text.c_str());
        CHARFORMAT2W normal{sizeof(normal)};
        normal.dwMask = CFM_FACE | CFM_SIZE;
        normal.yHeight = 220;
        wcscpy_s(normal.szFaceName, L"Segoe UI");
        SendMessageW(text, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&normal);
        for (auto &block : document.blocks) {
            active();
            CHARRANGE range{(LONG)block.start, (LONG)(block.start + block.length)};
            SendMessageW(text, EM_EXSETSEL, 0, (LPARAM)&range);
            CHARFORMAT2W format = normal;
            format.dwMask |= CFM_BOLD;
            format.dwEffects = block.kind == Kind::Heading ? CFE_BOLD : 0;
            if (block.kind == Kind::Heading)
                format.yHeight = block.level == 1 ? 440 : block.level == 2 ? 340 : 280;
            if (block.kind == Kind::Code)
                wcscpy_s(format.szFaceName, L"Consolas");
            SendMessageW(text, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
            for (auto &run : block.runs) {
                range = {(LONG)run.start, (LONG)(run.start + run.length)};
                SendMessageW(text, EM_EXSETSEL, 0, (LPARAM)&range);
                format.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
                format.dwEffects = ((run.style & Bold) ? CFE_BOLD : 0) |
                                   ((run.style & Italic) ? CFE_ITALIC : 0) |
                                   ((run.style & Underline) ? CFE_UNDERLINE : 0) |
                                   ((run.style & Strike) ? CFE_STRIKEOUT : 0);
                if (block.kind == Kind::Heading)
                    format.dwEffects |= CFE_BOLD;
                SendMessageW(text, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
            }
        }
        auto rtfText = [](std::wstring_view value) {
            std::string out;
            for (auto c : value) {
                if (c == '\\' || c == '{' || c == '}') {
                    out += '\\';
                    out += (char)c;
                } else if (c == '\n' || c == '\r')
                    out += "\\line ";
                else if (c < 128)
                    out += (char)c;
                else
                    out += "\\u" + std::to_string((short)c) + "?";
            }
            return out;
        };
        auto streamRtf = [&](LONG start, LONG end, const std::string &rtf) {
            CHARRANGE range{start, end};
            SendMessageW(text, EM_EXSETSEL, 0, (LPARAM)&range);
            struct Buffer {
                const std::string &text;
                size_t offset = 0;
            } buffer{rtf};
            EDITSTREAM stream{};
            stream.dwCookie = (DWORD_PTR)&buffer;
            stream.pfnCallback = [](DWORD_PTR cookie, LPBYTE bytes, LONG count, LONG *written) -> DWORD {
                auto &b = *(Buffer *)cookie;
                *written = (LONG)std::min<size_t>(count, b.text.size() - b.offset);
                memcpy(bytes, b.text.data() + b.offset, *written);
                b.offset += *written;
                return 0;
            };
            SendMessageW(text, EM_STREAMIN, SF_RTF | SFF_SELECTION, (LPARAM)&stream);
            if (stream.dwError)
                throw std::runtime_error("Could not prepare formatted print content.");
        };
        // Replace from the end so original rendered offsets remain valid for earlier blocks.
        for (size_t i = document.blocks.size(); i-- > 0;) {
            active();
            auto &block = document.blocks[i];
            if (block.kind == Kind::Table) {
                size_t first = i;
                while (first && document.blocks[first - 1].kind == Kind::Table)
                    --first;
                std::string rtf = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Segoe UI;}}\\fs22 ";
                int pageWidth =
                    MulDiv(GetDeviceCaps(printer, HORZRES), 1440, GetDeviceCaps(printer, LOGPIXELSX)) - 1440;
                for (size_t row = first; row <= i; ++row) {
                    auto &b = document.blocks[row];
                    rtf += "\\trowd\\trgaph80 ";
                    for (size_t c = 0; c < b.cells.size(); ++c)
                        rtf += "\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10\\"
                               "clbrdrr\\brdrs\\brdrw10\\cellx" +
                               std::to_string((c + 1) * pageWidth / std::max<size_t>(1, b.cells.size()));
                    for (auto &cell : b.cells) {
                        rtf += "\\pard\\intbl";
                        rtf += cell.align == 3 ? "\\qr " : cell.align == 2 ? "\\qc " : "\\ql ";
                        if (b.header)
                            rtf += "\\b ";
                        rtf += rtfText(std::wstring_view(document.text).substr(cell.start, cell.length));
                        if (b.header)
                            rtf += "\\b0 ";
                        rtf += "\\cell ";
                    }
                    rtf += "\\row ";
                }
                rtf += "\\pard\\par}";
                streamRtf((LONG)document.blocks[first].start, (LONG)(block.start + block.length), rtf);
                i = first;
            } else if (block.kind == Kind::Image && block.image) {
                try {
                    std::atomic_bool running = false;
                    auto image =
                        imageForExport(*block.image, document.path.wstring(), cancel ? *cancel : running);
                    active();
                    size_t stride = ((size_t)image.width * 3 + 3) & ~size_t(3);
                    std::vector<unsigned char> dib(sizeof(BITMAPINFOHEADER) + stride * image.height, 0);
                    BITMAPINFOHEADER info{sizeof(info),
                                          (LONG)image.width,
                                          (LONG)image.height,
                                          1,
                                          24,
                                          BI_RGB,
                                          (DWORD)(stride * image.height),
                                          0,
                                          0,
                                          0,
                                          0};
                    memcpy(dib.data(), &info, sizeof(info));
                    for (size_t y = 0; y < image.height; ++y)
                        for (size_t x = 0; x < image.width; ++x) {
                            auto src = image.pixels.data() + (y * image.width + x) * 4;
                            auto dst = dib.data() + sizeof(info) + (image.height - 1 - y) * stride + x * 3;
                            for (unsigned c = 0; c < 3; ++c)
                                dst[c] = (BYTE)std::min(255u, (unsigned)src[c] + 255 - src[3]);
                        }
                    int width = MulDiv(GetDeviceCaps(printer, HORZRES), 1440,
                                       GetDeviceCaps(printer, LOGPIXELSX)) -
                                1440,
                        height = MulDiv(GetDeviceCaps(printer, VERTRES), 1440,
                                        GetDeviceCaps(printer, LOGPIXELSY)) -
                                 1840;
                    double scale =
                        std::min({15., (double)width / image.width, (double)height / image.height});
                    std::string rtf = "{\\rtf1{\\pict\\dibitmap0\\wbmbitspixel24\\wbmplanes1\\wbmwidthbytes" +
                                      std::to_string(stride) + "\\picw" + std::to_string(image.width) +
                                      "\\pich" + std::to_string(image.height) + "\\picwgoal" +
                                      std::to_string((int)(image.width * scale)) + "\\pichgoal" +
                                      std::to_string((int)(image.height * scale)) + " ";
                    constexpr char hex[] = "0123456789abcdef";
                    rtf.reserve(rtf.size() + dib.size() * 2 + 2);
                    for (auto b : dib) {
                        rtf += hex[b >> 4];
                        rtf += hex[b & 15];
                    }
                    rtf += "}}";
                    streamRtf((LONG)block.start, (LONG)(block.start + block.length), rtf);
                } catch (const std::exception &) {
                    // Preserve the existing alt text when an image is unavailable, as in reading mode.
                }
            }
        }
        DOCINFOW info{sizeof(info)};
        active();
        info.lpszDocName = L"Voltura Markdown";
        info.lpszOutput = pdf.empty() ? nullptr : pdf.c_str();
        if (StartDocW(printer, &info) <= 0)
            throw std::runtime_error("Printing was cancelled or the printer could not start.");
        started = true;
        int x = GetDeviceCaps(printer, LOGPIXELSX), y = GetDeviceCaps(printer, LOGPIXELSY);
        FORMATRANGE page{};
        page.hdc = page.hdcTarget = printer;
        page.rcPage = {0, 0, MulDiv(GetDeviceCaps(printer, HORZRES), 1440, x),
                       MulDiv(GetDeviceCaps(printer, VERTRES), 1440, y)};
        page.rc = {720, 720, page.rcPage.right - 720, page.rcPage.bottom - 720};
        page.chrg = {0, -1};
        if (page.rc.right <= page.rc.left || page.rc.bottom <= page.rc.top)
            throw std::runtime_error("The selected paper is too small.");
        GETTEXTLENGTHEX length{GTL_NUMCHARS, 1200};
        auto total = (LONG)SendMessageW(text, EM_GETTEXTLENGTHEX, (WPARAM)&length, 0);
        while (page.chrg.cpMin < total) {
            active();
            if (StartPage(printer) <= 0)
                throw std::runtime_error("Could not start a printed page.");
            LONG next = (LONG)SendMessageW(text, EM_FORMATRANGE, TRUE, (LPARAM)&page);
            if (EndPage(printer) <= 0 || next <= page.chrg.cpMin)
                throw std::runtime_error("Could not print this page.");
            page.chrg.cpMin = next;
        }
        if (EndDoc(printer) <= 0)
            throw std::runtime_error("Could not finish the print job.");
        started = false;
        SendMessageW(text, EM_FORMATRANGE, FALSE, 0);
        DestroyWindow(text);
        FreeLibrary(library);
    } catch (...) {
        if (started)
            AbortDoc(printer);
        if (text)
            DestroyWindow(text);
        FreeLibrary(library);
        throw;
    }
}
std::filesystem::path copyImageAsset(const std::filesystem::path &source,
                                     const std::filesystem::path &document) {
    if (document.empty())
        throw std::runtime_error("Save the Markdown file before adding managed images.");
    auto ext = source.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
    if (ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".gif" && ext != L".svg" &&
        ext != L".webp" && ext != L".bmp")
        throw std::runtime_error("Choose a PNG, JPEG, GIF, SVG, WebP, or BMP image.");
    if (std::filesystem::file_size(source) > 64 * 1024 * 1024)
        throw std::runtime_error("Choose an image smaller than 64 MiB.");
    auto folder =
        std::filesystem::absolute(document).parent_path() / (document.stem().wstring() + L".assets");
    std::filesystem::create_directories(folder);
    auto name = source.filename();
    for (unsigned n = 0; n < 10000; ++n) {
        auto target =
            folder / (n ? source.stem().wstring() + L"-" + std::to_wstring(n) + ext : name.wstring());
        std::error_code ec;
        if (std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, ec))
            return target;
        if (ec != std::errc::file_exists)
            throw std::runtime_error("Could not copy the image into the assets folder.");
    }
    throw std::runtime_error("Too many images share this filename.");
}
std::filesystem::path DraftRecovery::folder() {
    wchar_t path[32768]{};
    auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, 32768);
    if (!n || n >= 32768)
        throw std::runtime_error("Local application data is unavailable.");
    return std::filesystem::path(path) / L"Voltura" / L"Markdown" / L"Recovery";
}
DraftRecovery::DraftRecovery() {
    snapshot = folder() / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
                           L"-" + std::to_wstring((uintptr_t)this) + L".draft");
}
void DraftRecovery::write(std::wstring_view text, const std::filesystem::path &original) {
    std::lock_guard lock(mutex);
    if (closed)
        return;
    if (text.size() > 64 * 1024 * 1024)
        throw std::runtime_error(
            "Recovery is limited to 64 million UTF-16 characters. Save this large document explicitly.");
    std::filesystem::create_directories(snapshot.parent_path());
    auto temp = snapshot;
    temp += L".tmp";
    auto path = toUtf8(original.wstring()), body = toUtf8(text);
    if (path.size() + body.size() + 64 > 128 * 1024 * 1024)
        throw std::runtime_error(
            "Recovery snapshots are limited to 128 MiB. Save this large document explicitly.");
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << "VMD-DRAFT-1\n" << path.size() << '\n';
    out.write(path.data(), path.size());
    out.write(body.data(), body.size());
    out.close();
    if (!out)
        throw std::runtime_error("Could not write draft recovery data.");
    if (!MoveFileExW(temp.c_str(), snapshot.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not publish draft recovery data.");
}
void DraftRecovery::clear(bool close) {
    std::lock_guard lock(mutex);
    closed = closed || close;
    std::error_code ec;
    std::filesystem::remove(snapshot, ec);
    auto temp = snapshot;
    temp += L".tmp";
    std::filesystem::remove(temp, ec);
}
std::vector<DraftRecovery::Draft> DraftRecovery::list() {
    std::vector<Draft> result;
    std::error_code ec;
    auto dir = folder();
    for (auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (result.size() == 100)
            break;
        if (entry.path().extension() != L".draft" || entry.file_size() > 128 * 1024 * 1024)
            continue;
        auto filename = entry.path().filename().wstring();
        auto dash = filename.find('-');
        if (dash != filename.npos && dash < 11 &&
            filename.substr(0, dash).find_first_not_of(L"0123456789") == filename.npos) {
            DWORD pid = std::stoul(filename.substr(0, dash));
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (process) {
                auto live = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
                CloseHandle(process);
                if (live)
                    continue;
            }
        }
        std::ifstream in(entry.path(), std::ios::binary);
        std::string magic, length;
        if (!std::getline(in, magic) || magic != "VMD-DRAFT-1" || !std::getline(in, length) ||
            length.empty() || length.size() > 6 || length.find_first_not_of("0123456789") != length.npos)
            continue;
        auto n = std::stoul(length);
        if (n > 131072)
            continue;
        std::string path(n, '\0');
        if (!in.read(path.data(), n))
            continue;
        result.push_back({entry.path(), fromUtf8(path), {}});
    }
    return result;
}
DraftRecovery::Draft DraftRecovery::read(const std::filesystem::path &snapshot) {
    if (std::filesystem::file_size(snapshot) > 128 * 1024 * 1024)
        throw std::runtime_error("This recovery snapshot is too large.");
    std::ifstream in(snapshot, std::ios::binary);
    std::string magic, length;
    if (!std::getline(in, magic) || magic != "VMD-DRAFT-1" || !std::getline(in, length) || length.empty() ||
        length.size() > 6 || length.find_first_not_of("0123456789") != length.npos)
        throw std::runtime_error("This recovery snapshot is incomplete.");
    auto n = std::stoul(length);
    if (n > 131072)
        throw std::runtime_error("Invalid recovery path.");
    std::string path(n, '\0');
    if (!in.read(path.data(), n))
        throw std::runtime_error("This recovery snapshot is incomplete.");
    std::string text((std::istreambuf_iterator<char>(in)), {});
    return {snapshot, fromUtf8(path), fromUtf8(text)};
}
} // namespace vm
