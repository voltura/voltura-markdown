#pragma once
#include "edit.h"
#include <mutex>

namespace vm {
struct HeadingEntry {
    size_t offset = 0;
    unsigned level = 1;
    std::wstring title;
};
std::vector<HeadingEntry> sourceOutline(std::wstring_view source);
std::optional<TextEdit> continueMarkdown(std::wstring_view line, size_t caret);
TextEdit indentMarkdown(std::wstring_view source, size_t start, size_t end, bool outdent);
enum class TableAction { AddRow, DeleteRow, AddColumn, DeleteColumn, Left, Center, Right };
std::optional<TextEdit> editTable(std::wstring_view source, size_t caret, TableAction action);
struct SyntaxSpan {
    size_t start, length;
    unsigned color;
};
struct FenceState {
    wchar_t marker = 0;
    size_t length = 0;
    unsigned quotes = 0, indent = 0;
    bool operator==(const FenceState &) const = default;
};
struct FencePoint {
    size_t offset;
    FenceState state;
};
FenceState scanFenceLine(std::wstring_view line, FenceState state);
std::vector<FencePoint> sourceFences(std::wstring_view source);
std::vector<SyntaxSpan> syntaxSpans(std::wstring_view source, FenceState initial = {},
                                    bool partialFirstLine = false);
std::wstring replaceMatches(std::wstring_view source, std::wstring_view needle, std::wstring_view replacement,
                            bool matchCase, bool wholeWord, const std::atomic_bool &cancel);
std::string exportHtml(const Document &document);
std::wstring rebaseLocalLinks(std::wstring_view source, const std::filesystem::path &from,
                              const std::filesystem::path &to);
void printDocument(const Document &document, HDC printer, const std::filesystem::path &pdf = {},
                   const std::atomic_bool *cancel = nullptr);
std::filesystem::path copyImageAsset(const std::filesystem::path &source,
                                     const std::filesystem::path &document);
// One independent writer per editor session. The mutex orders cleanup after any in-flight write.
class DraftRecovery {
    std::mutex mutex;
    bool closed = false;
    std::filesystem::path snapshot;

  public:
    DraftRecovery();
    void write(std::wstring_view text, const std::filesystem::path &original);
    void clear(bool close = false);
    struct Draft {
        std::filesystem::path snapshot, original;
        std::wstring text;
    };
    static std::vector<Draft> list();
    static Draft read(const std::filesystem::path &snapshot);
    static std::filesystem::path folder();
};
} // namespace vm
