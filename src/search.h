#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <windows.h>

namespace vm {
struct MatchCount {
    uint64_t total = 0, beforeStart = 0;
};
inline bool matchesAt(std::wstring_view text, std::wstring_view query, size_t at) {
    return !query.empty() && at <= text.size() && query.size() <= text.size() - at &&
           CompareStringOrdinal(text.data() + at, (int)query.size(), query.data(), (int)query.size(), TRUE) ==
               CSTR_EQUAL;
}
inline int findMatch(std::wstring_view text, std::wstring_view query, size_t start, bool backwards,
                     const std::atomic_bool &cancel) {
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
            if (matchesAt(text, query, at))
                return (int)at;
        }
    }
    return -1;
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
