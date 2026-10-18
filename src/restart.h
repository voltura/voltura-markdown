#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <windows.h>

namespace vm {
struct RestartView {
    std::filesystem::path document;
    std::array<unsigned char, 32> digest{};
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    double within = 0;
    uint64_t block = 0;
    float horizontal = 0, zoom = 1;
    bool source = false, fullscreen = false;
};
std::array<unsigned char, 32> restartDigest(std::wstring_view source);
std::filesystem::path saveRestartView(const RestartView &view);
std::optional<RestartView> readRestartView(const std::filesystem::path &file);
int manualUpdateHelper(int argc, wchar_t **argv);
} // namespace vm
