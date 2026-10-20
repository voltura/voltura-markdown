#pragma once
#include "images.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

inline void goalImageRegressions(const std::filesystem::path &folder) {
    using namespace vm;
    unsigned failures = 0;
    auto check = [&](bool ok, const char *message) {
        if (!ok) {
            std::cerr << "GOAL AUDIT: " << message << '\n';
            ++failures;
        }
    };
    auto waitImage = [&](Images &images, const std::wstring &url) {
        std::shared_ptr<Image> image;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        do {
            image = images.get(url, (folder / L"document.md").wstring());
            if (image)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        return image && image->ready && !image->failed;
    };
    Images local(nullptr);
    const auto lateImage = folder / L"late-image.svg";
    check(!waitImage(local, L"late-image.svg"), "065 absent local image initially fails");
    auto missingImage = local.get(L"late-image.svg", (folder / L"document.md").wstring());
    check(missingImage && missingImage->failed, "065 failure is cached before generation changes");
    {
        std::ofstream out(lateImage);
        out << "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='4'><rect width='8' "
               "height='4'/></svg>";
    }
    local.cancelPending();
    check(waitImage(local, L"late-image.svg"),
          "065 a new document generation retries an image that previously failed");
    auto firstImage = local.get(L"late-image.svg", (folder / L"document.md").wstring());
    local.cancelPending();
    check(local.get(L"late-image.svg", (folder / L"document.md").wstring()) == firstImage,
          "065 preview edits preserve successfully loaded images");
    {
        std::ofstream out(lateImage);
        out << "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='4'><rect width='16' "
               "height='4'/></svg>";
    }
    local.cancelPending(true);
    check(waitImage(local, L"late-image.svg") &&
              local.get(L"late-image.svg", (folder / L"document.md").wstring())->width == 16 && firstImage &&
              firstImage->width == 8,
          "065 document reload refreshes images without mutating existing snapshots");
    bool exported = true;
    std::atomic_bool cancel = false;
    for (auto name : {L"test.svg?v=1", L"test.svg#icon"}) {
        try {
            exported &= imageForExport(name, (folder / L"document.md").wstring(), cancel).ready;
        } catch (...) {
            exported = false;
        }
    }
    check(waitImage(local, L"test.svg?v=1#icon") && exported,
          "013 local image URL suffixes work in reader and export");
    Images remote(nullptr, [](const std::wstring &key, const std::atomic_bool &) {
        Image image;
        image.ready = key == L"https://images.invalid/CaseSensitive.svg?q=A";
        image.failed = !image.ready;
        return image;
    });
    check(waitImage(remote, L"HTTPS://images.invalid/CaseSensitive.svg?q=A#icon"),
          "014 HTTPS image schemes are case insensitive without changing paths");
    {
        std::ifstream file(folder / L"test image.png", std::ios::binary);
        std::string png((std::istreambuf_iterator<char>(file)), {});
        std::string payload = std::string("Comment\0", 8) + "<svg width='32' height='32'></svg>";
        std::string chunk;
        auto append32 = [&](uint32_t value) {
            for (int shift = 24; shift >= 0; shift -= 8)
                chunk += (char)(value >> shift);
        };
        append32((uint32_t)payload.size());
        chunk += "tEXt";
        chunk += payload;
        uint32_t crc = 0xffffffff;
        for (size_t i = 4; i < chunk.size(); ++i) {
            crc ^= (unsigned char)chunk[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
        }
        append32(~crc);
        png.insert(png.size() - 12, chunk); // Insert a valid tEXt chunk before IEND.
        std::ofstream output(folder / L"svg-comment.png", std::ios::binary);
        output.write(png.data(), png.size());
        output.close();
        bool decoded = false;
        try {
            auto image = imageForExport(L"svg-comment.png", (folder / L"document.md").wstring(), cancel);
            decoded = image.ready && image.width == 8 && image.height == 8 && !image.svgSource;
        } catch (...) {
        }
        check(decoded && waitImage(local, L"svg-comment.png"),
              "048 PNG metadata mentioning SVG does not change the selected image decoder");
        std::ofstream(folder / L"prefixed.svg", std::ios::binary)
            << "\xef\xbb\xbf<?xml version='1.0' encoding='UTF-8'?>\n<!-- SVG fixture -->\n"
               "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='8'>"
               "<rect width='16' height='8' fill='red'/></svg>";
        auto vector = imageForExport(L"prefixed.svg", (folder / L"document.md").wstring(), cancel);
        check(vector.ready && vector.svgSource && vector.width == 16 && vector.height == 8 &&
                  waitImage(local, L"prefixed.svg"),
              "048 XML declarations, comments, and UTF-8 BOMs retain SVG decoding");
    }
    const unsigned corners[][4] = {{0, 7, 24, 31}, {7, 0, 31, 24}, {31, 24, 7, 0}, {24, 31, 0, 7},
                                   {0, 24, 7, 31}, {24, 0, 31, 7}, {31, 7, 24, 0}, {7, 31, 0, 24}};
    for (unsigned orientation = 1; orientation <= 8; ++orientation) {
        auto name = L"orientation-" + std::to_wstring(orientation) + L".tif";
        auto image = imageForExport(name, (folder / L"document.md").wstring(), cancel);
        unsigned width = orientation >= 5 ? 4 : 8, height = orientation >= 5 ? 8 : 4;
        bool correct = image.width == width && image.height == height;
        const unsigned offsets[] = {0, width - 1, width * (height - 1), width * height - 1};
        for (unsigned i = 0; i < 4; ++i)
            correct &= image.pixels[offsets[i] * 4] == corners[orientation - 1][i] * 5;
        check(correct, "049 EXIF orientation transforms dimensions and all four image corners");
        check(waitImage(local, name), "049 oriented images load asynchronously in the reader");
        auto reader = local.get(name, (folder / L"document.md").wstring());
        check(reader && reader->width == image.width && reader->height == image.height &&
                  reader->pixels == image.pixels,
              "049 reader and export use the same oriented pixels");
    }
    auto photo = imageForExport(L"orientation-6.jpg", (folder / L"document.md").wstring(), cancel);
    check(photo.width == 4 && photo.height == 8, "049 JPEG APP1 orientation rotates portrait photos");
    auto invalid = imageForExport(L"orientation-9.tif", (folder / L"document.md").wstring(), cancel);
    check(invalid.width == 8 && invalid.height == 4 && invalid.pixels.front() == 0,
          "049 invalid orientation metadata retains the stored image orientation");
    bool nulRejected = false;
    try {
        imageForExport(L"test.svg%00different.png", (folder / L"document.md").wstring(), cancel);
    } catch (...) {
        nulRejected = true;
    }
    check(nulRejected && !waitImage(local, L"test.svg%00different.png"),
          "encoded NUL in an image URL cannot silently open a truncated filename");
    if (failures)
        throw std::runtime_error(std::to_string(failures) + " image goal audit regressions");
}
