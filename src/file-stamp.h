#pragma once
#include <cstdint>
#include <filesystem>
#include <windows.h>

namespace vm {
struct FileStamp {
    DWORD error = ERROR_SUCCESS;
    uint64_t writeTime = 0, size = 0;
    bool operator==(const FileStamp &) const = default;
};

inline FileStamp fileStamp(const std::filesystem::path &path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return {GetLastError()};
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return {ERROR_DIRECTORY};
    return {ERROR_SUCCESS,
            (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime,
            (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow};
}
} // namespace vm
