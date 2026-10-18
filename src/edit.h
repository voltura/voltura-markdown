#pragma once
#include "document.h"
#include <optional>

namespace vm {
enum class Insert {
    Bold,
    Italic,
    Underline,
    Strike,
    InlineCode,
    Heading,
    Bullet,
    Numbered,
    Task,
    Quote,
    Code,
    Rule,
    Table,
    Link,
    Image
};
struct TextEdit {
    size_t start = 0, length = 0;
    std::wstring replacement;
    size_t selection = 0, selected = 0;
};
TextEdit insertMarkdown(std::wstring_view source, size_t start, size_t end, Insert kind, unsigned first = 0,
                        unsigned second = 0, std::wstring_view label = {},
                        std::wstring_view destination = {});
std::wstring editText(std::wstring_view source);
std::wstring markdownFileDestination(const std::filesystem::path &path);
struct FileBaseline {
    std::string bytes;
    bool exists = false;
};
FileBaseline readBaseline(const std::filesystem::path &path);
struct SaveResult {
    enum Status { Saved, Conflict, NeedsUtf8, Failed } status = Failed;
    FileBaseline baseline;
    std::wstring error;
};
SaveResult saveDraft(const std::filesystem::path &path, std::wstring_view text, const FileBaseline &expected,
                     const FileBaseline &original, bool utf8 = false);
struct Diagnostic {
    size_t offset = 0, length = 0, line = 1;
    std::wstring message;
};
std::vector<Diagnostic> validateDraft(const Document &document, const std::atomic_bool &cancel);
} // namespace vm
