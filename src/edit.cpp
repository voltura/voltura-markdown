#include "edit.h"
#include "md4c.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <set>
#include <shlwapi.h>
#include <stdexcept>

namespace vm {
std::wstring markdownFileDestination(const std::filesystem::path &path) {
    std::wstring result;
    for (auto c : path.generic_wstring()) {
        if (c == L'%')
            result += L"%25";
        else if (c == L'&')
            result += L"%26";
        else if (c == L'#')
            result += L"%23";
        else if (c == L'?')
            result += L"%3F";
        else
            result += c;
    }
    return result;
}
std::wstring editText(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            out += L'\n';
            if (i + 1 < text.size() && text[i + 1] == L'\n')
                ++i;
        } else
            out += text[i];
    }
    return out;
}
TextEdit insertMarkdown(std::wstring_view s, size_t start, size_t end, Insert kind, unsigned first,
                        unsigned second, std::wstring_view label, std::wstring_view destination) {
    start = std::min(start, s.size());
    end = std::clamp(end, start, s.size());
    TextEdit result{start, end - start};
    auto selected = std::wstring(s.substr(start, end - start));
    auto wrap = [&](std::wstring before, std::wstring after, std::wstring placeholder) {
        if (!selected.empty() && (kind == Insert::Bold || kind == Insert::Italic || kind == Insert::Strike)) {
            size_t left = 0, right = selected.size();
            while (left < right && iswspace(selected[left]))
                ++left;
            while (right > left && iswspace(selected[right - 1]))
                --right;
            if (left == right) {
                result.replacement = selected;
                result.selected = selected.size();
                return;
            }
            result.selection = left + before.size();
            result.selected = right - left;
            result.replacement = selected.substr(0, left) + before + selected.substr(left, right - left) +
                                 after + selected.substr(right);
            return;
        }
        if (selected.empty())
            selected = std::move(placeholder);
        result.selection = before.size();
        result.selected = selected.size();
        result.replacement = before + selected + after;
    };
    auto block = [&](std::wstring body) {
        std::wstring before, after;
        if (start && s[start - 1] != L'\n')
            before = L"\n\n";
        else if (start > 1 && s[start - 2] != L'\n')
            before = L"\n";
        if (end < s.size() && s[end] != L'\n')
            after = L"\n\n";
        else if (end + 1 < s.size() && s[end + 1] != L'\n')
            after = L"\n";
        result.selection += before.size();
        result.replacement = before + body + after;
    };
    switch (kind) {
    case Insert::Bold:
        wrap(L"**", L"**", L"bold text");
        break;
    case Insert::Italic:
        wrap(L"*", L"*", L"italic text");
        break;
    case Insert::Underline:
        wrap(L"<u>", L"</u>", L"underlined text");
        break;
    case Insert::Strike:
        wrap(L"~~", L"~~", L"text");
        break;
    case Insert::InlineCode: {
        size_t longest = 0, run = 0;
        for (auto c : selected) {
            run = c == L'`' ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        std::wstring fence(longest + 1, L'`');
        bool pad = !selected.empty() && (selected.front() == L'`' || selected.back() == L'`' ||
                                         (selected.front() == L' ' && selected.back() == L' ' &&
                                          selected.find_first_not_of(L' ') != selected.npos));
        wrap(fence + (pad ? L" " : L""), (pad ? L" " : L"") + fence, L"code");
        break;
    }
    case Insert::Link:
    case Insert::Image: {
        std::wstring escapedLabel, escapedDestination;
        auto title = label.empty() ? (selected.empty() ? L"text" : selected) : std::wstring(label);
        for (auto c : title) {
            if (c == L'[' || c == L']' || c == L'\\')
                escapedLabel += L'\\';
            escapedLabel += c;
        }
        for (auto c : destination) {
            if (c == L' ')
                escapedDestination += L"%20";
            else if (c == L'<')
                escapedDestination += L"%3C";
            else if (c == L'>')
                escapedDestination += L"%3E";
            else if (c == L'\n' || c == L'\r')
                continue;
            else
                escapedDestination += c == L'\\' ? L'/' : c;
        }
        result.replacement =
            (kind == Insert::Image ? L"![" : L"[") + escapedLabel + L"](<" + escapedDestination + L">)";
        result.selection = kind == Insert::Image ? 2 : 1;
        result.selected = escapedLabel.size();
        break;
    }
    case Insert::Rule:
        block(L"---\n");
        break;
    case Insert::Table: {
        unsigned rows = std::clamp(first, 1u, 100u), columns = std::clamp(second, 1u, 256u);
        std::wstring table = L"|";
        for (unsigned c = 0; c < columns; ++c)
            table += L" Header " + std::to_wstring(c + 1) + L" |";
        table += L"\n|";
        for (unsigned c = 0; c < columns; ++c)
            table += L" --- |";
        for (unsigned r = 0; r < rows; ++r) {
            table += L"\n|";
            for (unsigned c = 0; c < columns; ++c)
                table += L" Cell |";
        }
        table += L'\n';
        result.selection = 2;
        result.selected = 8;
        block(std::move(table));
        break;
    }
    case Insert::Code: {
        size_t longest = 2, run = 0;
        for (auto c : selected) {
            run = c == L'`' ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        std::wstring fence(longest + 1, L'`');
        if (selected.empty())
            selected = L"code";
        result.selection = fence.size() + 1;
        result.selected = selected.size();
        block(fence + L"\n" + selected + (selected.back() == L'\n' ? L"" : L"\n") + fence + L"\n");
        break;
    }
    default: {
        auto left = start ? s.rfind(L'\n', start - 1) : s.npos;
        result.start = left == s.npos ? 0 : left + 1;
        auto right = s.find(L'\n', end > start && s[end - 1] == L'\n' ? end - 1 : end);
        if (right == s.npos)
            right = s.size();
        result.length = right - result.start;
        std::wstring lines(s.substr(result.start, result.length));
        if (lines.empty())
            lines = kind == Insert::Heading ? L"Heading" : L"item";
        size_t at = 0;
        unsigned number = 1;
        while (at <= lines.size()) {
            auto next = lines.find(L'\n', at);
            if (next == lines.npos)
                next = lines.size();
            auto line = lines.substr(at, next - at);
            std::wstring prefix;
            if (kind == Insert::Heading) {
                size_t n = 0;
                while (n < line.size() && line[n] == L'#')
                    ++n;
                if (n >= 1 && n <= 6 && (n == line.size() || line[n] == L' ')) {
                    if (n < line.size())
                        ++n;
                    line.erase(0, n);
                }
                if (first)
                    prefix = std::wstring(std::min(first, 6u), L'#') + L" ";
            } else if (kind == Insert::Bullet)
                prefix = L"- ";
            else if (kind == Insert::Numbered)
                prefix = std::to_wstring(number++) + L". ";
            else if (kind == Insert::Task)
                prefix = L"- [ ] ";
            else if (kind == Insert::Quote)
                prefix = L"> ";
            if (result.replacement.empty())
                result.selection = prefix.size();
            result.replacement += prefix + line;
            if (next == lines.size())
                break;
            result.replacement += L'\n';
            at = next + 1;
        }
        result.selected = start == end ? result.replacement.size() - result.selection : 0;
        break;
    }
    }
    return result;
}
FileBaseline readBaseline(const std::filesystem::path &path) {
    if (path.empty())
        return {};
    std::error_code error;
    bool exists = std::filesystem::exists(path, error);
    if (error)
        throw std::runtime_error("Could not inspect the destination file.");
    if (!exists)
        return {};
    auto size = std::filesystem::file_size(path);
    if (size > 1000000000)
        throw std::runtime_error("The file is too large to edit.");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not read the file for safe saving.");
    FileBaseline result{std::string((size_t)size, '\0'), true};
    if (size && !input.read(result.bytes.data(), (std::streamsize)size))
        throw std::runtime_error("Could not read the complete file.");
    return result;
}
namespace {
struct Line {
    std::wstring text, ending;
};
std::vector<Line> splitLines(std::wstring_view text) {
    std::vector<Line> lines;
    size_t at = 0;
    while (at < text.size()) {
        auto end = text.find_first_of(L"\r\n", at);
        if (end == text.npos)
            end = text.size();
        size_t next = end;
        if (next < text.size() && text[next++] == L'\r' && next < text.size() && text[next] == L'\n')
            ++next;
        lines.push_back(
            {std::wstring(text.substr(at, end - at)), std::wstring(text.substr(end, next - end))});
        at = next;
    }
    return lines;
}
std::wstring restoreEndings(std::wstring_view text, const FileBaseline &original) {
    auto before = splitLines(decode(original.bytes));
    auto after = splitLines(editText(text));
    std::map<std::wstring, std::vector<size_t>> matches;
    size_t crlf = 0, lf = 0, cr = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        matches[before[i].text].push_back(i);
        if (before[i].ending == L"\r\n")
            ++crlf;
        else if (before[i].ending == L"\n")
            ++lf;
        else if (before[i].ending == L"\r")
            ++cr;
    }
    std::wstring fallback = !original.exists || crlf >= std::max(lf, cr) ? L"\r\n" : lf >= cr ? L"\n" : L"\r";
    std::wstring out;
    size_t cursor = 0;
    for (auto &line : after) {
        out += line.text;
        if (line.ending.empty())
            continue;
        auto found = matches.find(line.text);
        const Line *old = nullptr;
        if (found != matches.end()) {
            auto at = std::lower_bound(found->second.begin(), found->second.end(), cursor);
            if (at != found->second.end()) {
                old = &before[*at];
                cursor = *at + 1;
            }
        }
        out += old && !old->ending.empty() ? old->ending : fallback;
    }
    return out;
}
bool same(const FileBaseline &a, const FileBaseline &b) {
    return a.exists == b.exists && a.bytes == b.bytes;
}
} // namespace
SaveResult saveDraft(const std::filesystem::path &path, std::wstring_view text, const FileBaseline &expected,
                     const FileBaseline &original, bool utf8) {
    SaveResult result;
    std::filesystem::path temporary;
    bool ownsTemporary = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    try {
        if (!same(readBaseline(path), expected)) {
            result.status = SaveResult::Conflict;
            return result;
        }
        std::wstring encoding;
        auto originalText = decode(original.bytes, &encoding);
        bool unchanged = original.exists && editText(originalText) == text;
        auto source = unchanged ? originalText : restoreEndings(text, original);
        std::string bytes;
        if (unchanged && !utf8)
            bytes = original.bytes;
        else if (!utf8 && encoding.starts_with(L"UTF-16")) {
            bool le = encoding == L"UTF-16 LE";
            bytes = le ? "\xff\xfe" : "\xfe\xff";
            for (auto c : source) {
                bytes += (char)(le ? c & 255 : c >> 8);
                bytes += (char)(le ? c >> 8 : c & 255);
            }
        } else if (!utf8 && encoding == L"Windows ANSI fallback") {
            BOOL substituted = FALSE;
            int count = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, source.data(), (int)source.size(),
                                            nullptr, 0, nullptr, &substituted);
            bytes.resize(count);
            if (count)
                WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, source.data(), (int)source.size(),
                                    bytes.data(), count, nullptr, &substituted);
            if (substituted || (!source.empty() && !count)) {
                result.status = SaveResult::NeedsUtf8;
                return result;
            }
        } else {
            if (!utf8 && original.bytes.starts_with("\xef\xbb\xbf"))
                bytes = "\xef\xbb\xbf";
            bytes += toUtf8(source);
        }
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            temporary = path.parent_path() /
                        (L".vmd-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                         std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt) + L".tmp");
            file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                ownsTemporary = true;
                break;
            }
            if (GetLastError() != ERROR_FILE_EXISTS)
                throw std::runtime_error("Could not create a temporary save file.");
        }
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Could not create a temporary save file.");
        size_t at = 0;
        while (at < bytes.size()) {
            DWORD written = 0, count = (DWORD)std::min<size_t>(bytes.size() - at, 1024 * 1024);
            if (!WriteFile(file, bytes.data() + at, count, &written, nullptr) || written != count)
                throw std::runtime_error("Could not write the complete file.");
            at += written;
        }
        if (!FlushFileBuffers(file))
            throw std::runtime_error("Could not flush the saved file.");
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (!same(readBaseline(path), expected))
            result.status = SaveResult::Conflict;
        else {
            BOOL replaced = expected.exists
                                ? ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)
                                : MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
            if (!replaced)
                throw std::runtime_error("Windows could not replace the destination. Check its permissions "
                                         "and whether another app has locked it.");
            result.status = SaveResult::Saved;
            ownsTemporary = false;
            result.baseline = {std::move(bytes), true};
        }
    } catch (const std::exception &e) {
        result.error = fromUtf8(e.what());
    }
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (ownsTemporary)
        DeleteFileW(temporary.c_str());
    return result;
}

std::vector<Diagnostic> validateDraft(const Document &doc, const std::atomic_bool &cancel) {
    struct Validation {
        const Document &doc;
        const std::atomic_bool &cancel;
        std::string bytes;
        std::vector<Diagnostic> issues;
        std::set<std::wstring> anchors;
        std::map<std::filesystem::path, std::pair<bool, bool>> localTargets;
        size_t omitted = 0;
        std::exception_ptr error;
        size_t position = 0;
        bool code = false;
        wchar_t fence = 0;
        size_t fenceStart = 0, fenceLength = 0, codeEnd = 0;
        void locate(const char *s, size_t n) {
            auto address = (uintptr_t)s, base = (uintptr_t)bytes.data();
            if (address >= base && address <= base + bytes.size() && n <= base + bytes.size() - address)
                position = (size_t)(address - base);
        }
        void issue(size_t byte, size_t count, std::wstring message) {
            if (issues.size() >= 500) {
                ++omitted;
                return;
            }
            issues.push_back({byte, count, 0, std::move(message)});
        }
        size_t lineStart(size_t at) const {
            auto p = at ? bytes.rfind('\n', at - 1) : bytes.npos;
            return p == bytes.npos ? 0 : p + 1;
        }
        void beginCode(MD_BLOCK_CODE_DETAIL &detail) {
            code = true;
            fence = detail.fence_char;
            codeEnd = 0;
            if (!fence)
                return;
            // A text callback can stop before an inline code span's closing delimiters.
            // Block fences start on a later line, possibly after list/quote prefixes.
            size_t at = std::min(position, bytes.size());
            if (at && bytes[at - 1] != '\n') {
                auto end = bytes.find('\n', at);
                at = end == bytes.npos ? bytes.size() : end + 1;
            }
            fenceStart = bytes.npos;
            while (at < bytes.size()) {
                auto end = bytes.find('\n', at);
                if (end == bytes.npos)
                    end = bytes.size();
                auto candidate = bytes.find(std::string(3, (char)fence), at);
                if (candidate < end && bytes.find_first_not_of(" \t>+-*0123456789.)", at) == candidate) {
                    fenceStart = candidate;
                    break;
                }
                at = end == bytes.size() ? end : end + 1;
            }
            if (fenceStart == bytes.npos) {
                fence = 0;
                return;
            }
            fenceLength = 0;
            while (fenceStart + fenceLength < bytes.size() && bytes[fenceStart + fenceLength] == fence)
                ++fenceLength;
        }
        void endCode() {
            if (fence) {
                auto openingEnd = bytes.find('\n', fenceStart);
                size_t candidate = codeEnd                    ? codeEnd
                                   : openingEnd == bytes.npos ? bytes.size()
                                                              : openingEnd + 1;
                bool closed = false;
                if (candidate < bytes.size()) {
                    auto end = bytes.find('\n', candidate);
                    if (end == bytes.npos)
                        end = bytes.size();
                    auto close = bytes.find(std::string(fenceLength, (char)fence), candidate);
                    if (close < end) {
                        auto trailing = close;
                        while (trailing < end && bytes[trailing] == fence)
                            ++trailing;
                        closed = bytes.find_first_not_of(" \t\r", trailing) >= end;
                        if (closed)
                            position = end == bytes.size() ? end : end + 1;
                    }
                }
                if (!closed)
                    issue(fenceStart, fenceLength, L"Code fence has no closing fence.");
            }
            code = false;
            fence = 0;
        }
        void link(const MD_ATTRIBUTE &attribute, bool image) {
            auto previousPosition = position;
            locate(attribute.text, attribute.size);
            size_t location = position;
            position = previousPosition;
            auto target = entity(fromUtf8(std::string_view(attribute.text, attribute.size)));
            std::wstring unescaped;
            for (size_t i = 0; i < target.size(); ++i) {
                if (target[i] == L'\\' && i + 1 < target.size() && iswpunct(target[i + 1]))
                    ++i;
                unescaped += target[i];
            }
            target = std::move(unescaped);
            bool fragmentOnly = target.starts_with(L"#");
            if (!fragmentOnly) {
                auto fragment = target.find_first_of(L"#?");
                if (fragment != target.npos)
                    target.resize(fragment);
            }
            std::wstring decoded(target.size() + 1, L'\0');
            DWORD size = (DWORD)decoded.size();
            if (SUCCEEDED(UrlUnescapeW(target.data(), decoded.data(), &size, URL_UNESCAPE_AS_UTF8)))
                target.assign(decoded.data(), size);
            if (fragmentOnly) {
                if (!image && target.size() > 1 && !anchors.contains(target.substr(1)))
                    issue(location, attribute.size, L"Heading link has no target: " + target);
                return;
            }
            bool drivePath = target.size() >= 3 && iswalpha(target[0]) && target[1] == L':' &&
                             (target[2] == L'/' || target[2] == L'\\');
            if (target.empty() || target.starts_with(L"//") || target.starts_with(L"\\\\") ||
                (!drivePath && target.find(L':') != target.npos))
                return;
            auto path = std::filesystem::path(target);
            if (path.empty() || (path.is_relative() && doc.path.empty()))
                return;
            if (path.is_relative())
                path = doc.path.parent_path() / path;
            if (path.wstring().starts_with(L"\\\\"))
                return;
            auto root = path.root_path();
            if (!root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE)
                return;
            auto [cached, inserted] = localTargets.try_emplace(path.lexically_normal());
            if (inserted) {
                std::error_code ec;
                bool exists = std::filesystem::exists(path, ec);
                cached->second = {exists, (bool)ec};
            }
            if (cancel.load())
                return;
            if (cached->second.second)
                issue(location, attribute.size, L"Could not check local target: " + target);
            else if (!cached->second.first)
                issue(location, attribute.size, L"Local target does not exist: " + target);
        }
    } state{doc, cancel, toUtf8(doc.source)};
    for (auto &block : doc.blocks)
        if (block.anchor)
            state.anchors.insert(*block.anchor);
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB;
    // All C callbacks contain exceptions so they cannot unwind through the C parser.
    parser.enter_block = [](MD_BLOCKTYPE type, void *detail, void *data) -> int {
        auto &v = *(Validation *)data;
        if (v.cancel.load())
            return 1;
        try {
            if (type == MD_BLOCK_CODE)
                v.beginCode(*(MD_BLOCK_CODE_DETAIL *)detail);
        } catch (...) {
            v.error = std::current_exception();
            return 1;
        }
        return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void *, void *data) -> int {
        auto &v = *(Validation *)data;
        try {
            if (type == MD_BLOCK_CODE)
                v.endCode();
        } catch (...) {
            v.error = std::current_exception();
            return 1;
        }
        return v.cancel.load() ? 1 : 0;
    };
    parser.enter_span = [](MD_SPANTYPE type, void *detail, void *data) -> int {
        auto &v = *(Validation *)data;
        if (v.cancel.load())
            return 1;
        try {
            if (type == MD_SPAN_A)
                v.link(((MD_SPAN_A_DETAIL *)detail)->href, false);
            if (type == MD_SPAN_IMG)
                v.link(((MD_SPAN_IMG_DETAIL *)detail)->src, true);
        } catch (...) {
            v.error = std::current_exception();
            return 1;
        }
        return 0;
    };
    parser.leave_span = [](MD_SPANTYPE, void *, void *data) {
        return ((Validation *)data)->cancel.load() ? 1 : 0;
    };
    parser.text = [](MD_TEXTTYPE, const char *text, MD_SIZE size, void *data) -> int {
        auto &v = *(Validation *)data;
        v.locate(text, size);
        v.position += size;
        if (v.code)
            v.codeEnd = v.position;
        return v.cancel.load() ? 1 : 0;
    };
    md_parse(state.bytes.data(), (MD_SIZE)state.bytes.size(), &parser, &state);
    if (state.error)
        std::rethrow_exception(state.error);
    std::sort(state.issues.begin(), state.issues.end(), [](auto &a, auto &b) { return a.offset < b.offset; });
    size_t byte = 0, offset = 0, line = 1;
    for (auto &issue : state.issues) {
        while (byte < issue.offset && byte < state.bytes.size()) {
            unsigned char c = (unsigned char)state.bytes[byte++];
            if (c == '\n')
                ++line;
            if ((c & 0xc0) != 0x80)
                offset += c >= 0xf0 ? 2 : 1;
        }
        issue.length = fromUtf8(std::string_view(state.bytes).substr(issue.offset, issue.length)).size();
        issue.offset = offset;
        issue.line = line;
    }
    if (state.omitted)
        state.issues.push_back(
            {0, 0, 1, std::to_wstring(state.omitted) + L" additional warnings; showing the first 500."});
    return std::move(state.issues);
}
} // namespace vm
