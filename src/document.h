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
enum class Kind : uint8_t { Paragraph, Heading, Code, Rule, Table, Image };
enum Style : unsigned {
    Bold = 1,
    Italic = 2,
    Mono = 4,
    Strike = 8,
    Underline = 16,
    Keycap = 32,
    Subscript = 64,
    Superscript = 128,
    Small = 256
};
struct Run {
    uint32_t start = 0, length = 0;
    unsigned style = 0;
    std::wstring link;
};
struct Block;
struct Cell {
    uint32_t start = 0, length = 0;
    int align = 0;
    unsigned width = 0;
    bool percent = false;
    uint8_t verticalAlign = 2;
    bool header = false;
    uint16_t colSpan = 1, rowSpan = 1;
    uint32_t row = 0, column = 0, rows = 1;
    std::shared_ptr<std::deque<Block>> content;
};
enum class TableSection : uint8_t { Body, Head, Foot };
struct TableRowGroup {
    uint32_t first = 0, rows = 0;
    TableSection section = TableSection::Body;
    uint32_t identity = 0;
};
struct TableColumn {
    float pixels = 0, percent = 0, minimum = 0;
};
struct TableModel {
    uint32_t rows = 0, columns = 0;
    bool overlap = false;
    // Bands share column constraints and logical row-group identities. A band
    // ends only where no rowspan crosses the boundary.
    uint32_t rowOffset = 0;
    bool continued = false, continues = false;
    std::shared_ptr<std::vector<TableColumn>> tracks;
    std::shared_ptr<std::vector<TableColumn>> declaredColumns;
    std::vector<TableRowGroup> groups;
    std::shared_ptr<std::deque<Block>> captions;
};
struct ImageReference {
    std::wstring url;
    uint32_t width = 0, height = 0;
    std::wstring lightUrl, darkUrl;
    uint64_t inlineGroup = 0;
    std::unique_ptr<std::deque<Block>> inlineImages;
    const std::wstring &source(bool dark) const {
        const auto &selected = dark ? darkUrl : lightUrl;
        return selected.empty() ? url : selected;
    }
};
struct Block {
    Kind kind = Kind::Paragraph;
    uint8_t level = 0, align = 0;
    bool continuation : 1 = false;
    bool header : 1 = false;
    uint32_t start = 0, length = 0;
    unsigned indent = 0, quote = 0;
    // Shared identity for bounded pieces of one fenced/indented code block.
    uint32_t codeStart = 0;
    std::vector<Run> runs;
    std::vector<Cell> cells;
    std::unique_ptr<TableModel> table;
    // Most blocks have no metadata; keep their hot record compact.
    std::unique_ptr<ImageReference> image;
    std::unique_ptr<std::wstring> language, anchor;
};
struct Document {
    std::wstring text;
    std::wstring source;
    std::deque<Block> blocks;
    // Optional UTF-16 source anchors, collected only for editable previews.
    std::vector<uint32_t> sourceStarts;
    uint32_t sourceMapLength = 0;
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
Document parse(std::wstring source, const std::atomic_bool *cancel = nullptr, unsigned flags = 0xffffffff,
               bool sourceMap = false);
Document loadSource(const std::filesystem::path &path, const std::atomic_bool *cancel = nullptr);
Document load(const std::filesystem::path &path, const std::atomic_bool *cancel = nullptr);
size_t blockAtPosition(const std::deque<Block> &blocks, uint32_t position);
std::deque<Block> sourceBlocks(std::wstring_view source);
std::wstring entity(std::wstring_view text, const std::atomic_bool *cancel = nullptr);
} // namespace vm
