#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace vm {
enum class Kind { Paragraph, Heading, Code, Rule, Table, Image };
enum Style : unsigned { Bold = 1, Italic = 2, Mono = 4, Strike = 8, Underline = 16, Keycap = 32 };
struct Run {
    uint32_t start = 0, length = 0;
    unsigned style = 0;
    std::wstring link;
};
struct Cell {
    uint32_t start = 0, length = 0;
    int align = 0;
};
struct Block {
    Kind kind = Kind::Paragraph;
    uint32_t start = 0, length = 0;
    unsigned level = 0, indent = 0, quote = 0;
    bool continuation = false, header = false;
    // Shared identity for bounded pieces of one fenced/indented code block.
    uint32_t codeStart = 0;
    std::vector<Run> runs;
    std::vector<Cell> cells;
    // Most blocks have no metadata; keep their hot record compact.
    std::unique_ptr<std::wstring> image, language, anchor;
};
struct Document {
    std::wstring text;
    std::wstring source;
    std::deque<Block> blocks;
    std::filesystem::path path;
    std::wstring encoding;
    bool plainSource = false;
#ifdef VMD_TESTING
    double readMs = 0, parseMs = 0;
#endif
};
std::wstring fromUtf8(std::string_view s);
std::string toUtf8(std::wstring_view s);
std::wstring decode(std::string_view bytes, std::wstring *encoding = nullptr);
Document parse(std::wstring source, const std::atomic_bool *cancel = nullptr, unsigned flags = 0xffffffff);
Document loadSource(const std::filesystem::path &path, const std::atomic_bool *cancel = nullptr);
Document load(const std::filesystem::path &path, const std::atomic_bool *cancel = nullptr);
size_t blockAtPosition(const std::deque<Block> &blocks, uint32_t position);
std::deque<Block> sourceBlocks(std::wstring_view source);
std::wstring entity(std::wstring_view text, const std::atomic_bool *cancel = nullptr);
} // namespace vm
