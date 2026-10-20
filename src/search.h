#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>
#include <windows.h>

namespace vm {
struct SearchOptions {
    bool matchCase = false, wholeWord = false;
};
inline bool searchWordCharacter(wchar_t c) {
    WORD type = 0, marks = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &type);
    GetStringTypeW(CT_CTYPE3, &c, 1, &marks);
    return (type & (C1_ALPHA | C1_DIGIT)) || (marks & (C3_NONSPACING | C3_VOWELMARK)) || c == '_';
}
struct MatchIndex {
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> lines;
    uint32_t totalLines = 1;
    uint32_t length = 0;
};
struct MatchCount {
    uint64_t total = 0, beforeStart = 0;
};
inline bool matchesAt(std::wstring_view text, std::wstring_view query, size_t at,
                      SearchOptions options = {}) {
    return !query.empty() && at <= text.size() && query.size() <= text.size() - at &&
           CompareStringOrdinal(text.data() + at, (int)query.size(), query.data(), (int)query.size(),
                                !options.matchCase) == CSTR_EQUAL &&
           (!options.wholeWord ||
            ((!at || !searchWordCharacter(text[at - 1])) &&
             (at + query.size() == text.size() || !searchWordCharacter(text[at + query.size()]))));
}
inline int findMatch(std::wstring_view text, std::wstring_view query, size_t start, bool backwards,
                     const std::atomic_bool &cancel, SearchOptions options = {}) {
    if (query.empty())
        return -1;
    start = std::min(start, text.size());
    for (int pass = 0; pass < 2 && !cancel.load(); ++pass) {
        size_t begin = backwards ? (pass ? text.size() : start) : (pass ? 0 : start);
        size_t end = backwards ? (pass ? start : 0) : (pass ? start : text.size());
        for (size_t i = begin; backwards ? i > end : i < end;) {
            size_t at = backwards ? --i : i++;
            if ((at & 4095) == 0 && cancel.load())
                return -1;
            if (matchesAt(text, query, at, options))
                return (int)at;
        }
    }
    return -1;
}
inline MatchIndex indexMatches(std::wstring_view text, std::wstring_view query,
                               const std::atomic_bool &cancel, SearchOptions options = {}) {
    MatchIndex result;
    result.length = (uint32_t)query.size();
    if (query.empty())
        return result;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((i & 4095) == 0 && cancel.load())
            return {};
        if (matchesAt(text, query, i, options)) {
            result.offsets.push_back((uint32_t)i);
            if (result.lines.empty() || result.lines.back() != result.totalLines - 1)
                result.lines.push_back(result.totalLines - 1);
        }
        if (text[i] == L'\n' || (text[i] == L'\r' && (i + 1 == text.size() || text[i + 1] != L'\n')))
            ++result.totalLines;
    }
    return result;
}
inline MatchCount countMatches(std::wstring_view text, std::wstring_view query, size_t start,
                               const std::atomic_bool &cancel) {
    MatchCount result;
    if (query.empty())
        return result;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((i & 4095) == 0 && cancel.load())
            break;
        if (matchesAt(text, query, i)) {
            ++result.total;
            if (i < start)
                ++result.beforeStart;
        }
    }
    return result;
}
} // namespace vm
