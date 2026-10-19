#include "document.h"
#include "images.h"
#include "shell.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <shellapi.h>
#include <shlwapi.h>
#include <stdexcept>
#include <thread>
#include <wincodec.h>
#include <winioctl.h>
#include <wrl/client.h>
using namespace vm;
using Microsoft::WRL::ComPtr;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void check(HRESULT hr) {
    require(SUCCEEDED(hr), "WIC fixture setup failed");
}
void raster(const std::filesystem::path &path, REFGUID container) {
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(&stream));
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(container, nullptr, &encoder));
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr));
    check(frame->Initialize(nullptr));
    check(frame->SetSize(8, 8));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&format));
    std::vector<BYTE> pixels(8 * 8 * 3, 180);
    ComPtr<IWICBitmap> bitmap;
    check(factory->CreateBitmapFromMemory(8, 8, GUID_WICPixelFormat24bppBGR, 24, (UINT)pixels.size(),
                                          pixels.data(), &bitmap));
    check(frame->WriteSource(bitmap.Get(), nullptr));
    check(frame->Commit());
    check(encoder->Commit());
}
int wmain(int argc, wchar_t **argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::filesystem::path temp;
    try {
        // Opt-in live measurement; normal CTest runs never need network access.
        if (argc > 2 && std::wstring_view(argv[1]) == L"--benchmark-images") {
            Images images(nullptr);
            for (int pass = 0; pass < 2; ++pass) {
                auto start = std::chrono::steady_clock::now();
                std::vector<bool> done(argc - 2);
                int remaining = argc - 2;
                while (remaining) {
                    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(30),
                            "image benchmark timed out");
                    for (int i = 2; i < argc; ++i) {
                        if (done[i - 2])
                            continue;
                        auto image = images.get(argv[i], L"");
                        if (!image)
                            continue;
                        require(image->ready && !image->failed, "benchmark image failed to decode");
                        if (!pass)
                            std::cout << "image_" << i - 2 << "_ms="
                                      << std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - start)
                                             .count()
                                      << '\n';
                        done[i - 2] = true;
                        --remaining;
                    }
                    if (remaining)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                std::cout << (pass ? "cached_ms=" : "cold_ms=")
                          << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                       start)
                                 .count()
                          << '\n';
            }
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--decode-image") {
            std::atomic_bool cancel = false;
            auto image = imageForExport(argv[2], L"", cancel);
            std::ofstream output(std::filesystem::path(argv[3]), std::ios::binary);
            output.write((const char *)&image.width, sizeof(image.width));
            output.write((const char *)&image.height, sizeof(image.height));
            output.write((const char *)image.pixels.data(), image.pixels.size());
            require((bool)output, "image pixel output");
            return 0;
        }
        // Override HKCU only in this test process: no real association or Settings UI.
        std::wstring root = L"Software\\VolturaMarkdownTest-" + std::to_wstring(GetCurrentProcessId());
        HKEY hive = nullptr;
        require(RegCreateKeyExW(HKEY_CURRENT_USER, root.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                KEY_ALL_ACCESS, nullptr, &hive, nullptr) == ERROR_SUCCESS,
                "test hive");
        require(RegOverridePredefKey(HKEY_CURRENT_USER, hive) == ERROR_SUCCESS, "isolated HKCU override");
        bool registrationSucceeded = false;
        auto registered = registerApplication(false, false, &registrationSucceeded);
        bool okay = registrationSucceeded && registered.find(L"is registered") != registered.npos;
        wchar_t command[32768]{};
        DWORD size = sizeof(command);
        okay &= RegGetValueW(hive, L"Software\\Classes\\Voltura.Markdown\\shell\\open\\command", nullptr,
                             RRF_RT_REG_SZ, nullptr, command, &size) == ERROR_SUCCESS;
        okay &= std::wstring(command).ends_with(L"\" \"%1\"");
        HKEY classes = nullptr;
        require(RegOpenKeyExW(hive, L"Software\\Classes", 0, KEY_ALL_ACCESS, &classes) == ERROR_SUCCESS,
                "isolated association classes");
        require(RegOverridePredefKey(HKEY_CLASSES_ROOT, classes) == ERROR_SUCCESS, "isolated HKCR override");
        wchar_t resolved[32768]{};
        DWORD resolvedSize = (DWORD)std::size(resolved);
        okay &= SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_FIXED_PROGID | ASSOCF_NOFIXUPS, ASSOCSTR_COMMAND,
                                            L"Voltura.Markdown", L"open", resolved, &resolvedSize));
        okay &= std::wstring(resolved) == command;
        HKEY openCommand = nullptr;
        require(RegOpenKeyExW(classes, L"Voltura.Markdown\\shell\\open\\command", 0, KEY_SET_VALUE,
                              &openCommand) == ERROR_SUCCESS,
                "isolated command key");
        const wchar_t moved[] = L"\"C:\\Missing portable copy\\vmd.exe\" \"%1\"";
        RegSetValueExW(openCommand, nullptr, 0, REG_SZ, (const BYTE *)moved, sizeof(moved));
        RegCloseKey(openCommand);
        resolvedSize = (DWORD)std::size(resolved);
        okay &= SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_FIXED_PROGID | ASSOCF_NOFIXUPS, ASSOCSTR_COMMAND,
                                            L"Voltura.Markdown", L"open", resolved, &resolvedSize));
        okay &= std::wstring(resolved) == moved;
        require(RegCreateKeyExW(classes, L"Applications\\vmd.exe\\shell\\open\\command", 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &openCommand, nullptr) == ERROR_SUCCESS,
                "isolated Open with command");
        RegSetValueExW(openCommand, nullptr, 0, REG_SZ, (const BYTE *)moved, sizeof(moved));
        RegCloseKey(openCommand);
        resolvedSize = (DWORD)std::size(resolved);
        okay &= SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_BYEXENAME | ASSOCF_NOFIXUPS, ASSOCSTR_COMMAND,
                                            L"vmd.exe", L"open", resolved, &resolvedSize));
        okay &= std::wstring(resolved) == moved;
        RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
        RegCloseKey(classes);
        registerApplication(true, false);
        HKEY probe = nullptr;
        okay &= RegOpenKeyExW(hive, L"Software\\Classes\\Voltura.Markdown", 0, KEY_READ, &probe) ==
                ERROR_FILE_NOT_FOUND;
        if (probe)
            RegCloseKey(probe);
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegCloseKey(hive);
        RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
        require(okay, "isolated file association roundtrip");
        const std::filesystem::path current = LR"(C:\Readers\Voltura Markdown\vmd.exe)";
        auto association = [&](std::wstring_view prog, std::wstring_view command) {
            return classifyMarkdownAssociation(prog, command, current);
        };
        require(association(L"Voltura.Markdown", LR"("C:\Readers\Voltura Markdown\vmd.exe" "%1")") ==
                    MarkdownAssociation::Matching,
                "association matches quoted executable");
        require(association(L"voltura.markdown",
                            LR"("c:/READERS/Voltura Markdown/../Voltura Markdown/vmd.exe" "%1")") ==
                    MarkdownAssociation::Matching,
                "association normalizes case separators and parent segments");
        require(association(L"Voltura.Markdown", LR"("\\?\C:\Readers\Voltura Markdown\vmd.exe" "%1")") ==
                    MarkdownAssociation::Matching,
                "association normalizes extended path prefix");
        require(association(L"Voltura.Markdown", LR"("C:\Missing old copy\vmd.exe" "%1")") ==
                    MarkdownAssociation::Mismatch,
                "association retains missing executable target");
        require(classifyMarkdownAssociation(L"Voltura.Markdown", LR"("C:\Läsare\日本語\vmd.exe" "%1")",
                                            LR"(C:\Läsare\日本語\vmd.exe)") == MarkdownAssociation::Matching,
                "association preserves Unicode executable paths");
        wchar_t running[32768]{};
        GetModuleFileNameW(nullptr, running, (DWORD)std::size(running));
        require(association(L"Voltura.Markdown", L"\"" + std::wstring(running) + L"\" \"%1\"") ==
                    MarkdownAssociation::Mismatch,
                "association warns for another existing executable");
        require(association(L"Applications\\vmd.exe", LR"("C:\Old\vmd.exe" "%1")") ==
                    MarkdownAssociation::Mismatch,
                "association recognizes Windows Open with registration");
        for (auto command : {L"", L"vmd.exe \"%1\"", L"C:\\Program Files\\vmd.exe \"%1\"",
                             L"\"C:\\Readers\\bad.txt\" \"%1\""})
            require(association(L"Voltura.Markdown", command) == MarkdownAssociation::Unknown,
                    "inconclusive association stays quiet");
        require(association(L"Other.Markdown", LR"("C:\Old\vmd.exe" "%1")") == MarkdownAssociation::Other,
                "another default application stays quiet");
        require(association(L"", L"") == MarkdownAssociation::Unknown, "no default stays quiet");
        temp = std::filesystem::temp_directory_path() /
               (L"VolturaMarkdownTests-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(temp);
        raster(temp / L"test image.png", GUID_ContainerFormatPng);
        raster(temp / L"test.jpg", GUID_ContainerFormatJpeg);
        raster(temp / L"test.gif", GUID_ContainerFormatGif);
        std::ofstream(temp / L"test.svg") << "<svg xmlns='http://www.w3.org/2000/svg' width='16' "
                                             "height='8'><rect width='16' height='8' fill='#ff6600'/></svg>";
        std::ofstream(temp / L"classes.svg")
            << "<svg width='16' height='8'><style>.a{class:b;style:class:a;fill:#ff6600;}"
               ".b{class:a;}</style><rect class='a' style='class:a;' width='16' height='8'/></svg>";
        std::ofstream(temp / L"tiny-dash.svg")
            << "<svg width='200' height='200'><path d='M100 100 L101 100' stroke='black' "
               "stroke-dasharray='0.000002'/></svg>";
        std::ofstream(temp / L"dashes.svg")
            << "<svg width='200' height='200'><path d='M10 10 L100 10' stroke='black' "
               "stroke-dasharray='0 2 3' stroke-dashoffset='-2'/></svg>";
        std::ofstream(temp / L"badge.svg")
            << "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='20'>"
               "<clipPath id='clip'><rect width='100' height='20' fill='red'/></clipPath>"
               "<rect width='100' height='20' fill='#555'/>"
               "<g fill='white' text-anchor='middle' font-family='Verdana,sans-serif' font-size='110'>"
               "<text x='250' y='140' transform='scale(.1)' textLength='400'>visitors</text>"
               "<g transform='scale(.1)'><text x='750' y='140' textLength='300'>123</text></g>"
               "</g><rect width='100' height='20' fill='rgba(0,0,0,0)'/></svg>";
        {
            std::atomic_bool cancel = false;
            auto badge = imageForExport(L"badge.svg", (temp / L"document.md").wstring(), cancel);
            size_t labelPixels = 0, valuePixels = 0, redPixels = 0;
            for (unsigned y = 0; y < badge.height; ++y)
                for (unsigned x = 0; x < badge.width; ++x) {
                    auto pixel = badge.pixels.data() + ((size_t)y * badge.width + x) * 4;
                    if (pixel[0] > 200 && pixel[1] > 200 && pixel[2] > 200)
                        (x < 50 ? labelPixels : valuePixels)++;
                    if (pixel[2] > 200 && pixel[1] < 40)
                        ++redPixels;
                }
            require(badge.width == 100 && badge.height == 20 && labelPixels > 30 && valuePixels > 20 &&
                        redPixels == 0,
                    "SVG badge text, anchors, nested transforms and intrinsic size");
            std::ofstream(temp / L"entities.svg")
                << "<svg width='120' height='20'><text y='15' font-size='14' fill='white'>A &amp; B "
                   "&#49;</text></svg>";
            auto entities = imageForExport(L"entities.svg", (temp / L"document.md").wstring(), cancel);
            require(std::any_of(entities.pixels.begin(), entities.pixels.end(), [](auto c) { return c > 0; }),
                    "SVG text entities render as glyphs");
            std::ofstream(temp / L"embedded.svg")
                << "<svg width='30' height='20'><image x='5' y='5' width='10' height='10' "
                   "href='data:image/svg+xml;base64,PHN2ZyB3aWR0aD0nMTAnIGhlaWdodD0nMTAnPjxyZWN0IHdpZHRo"
                   "PScxMCcgaGVpZ2h0PScxMCcgZmlsbD0nI2ZmMDAwMCcvPjwvc3ZnPg=='/>"
                   "<image href='file:///must-not-open.svg' width='20' height='20'/></svg>";
            auto embedded = imageForExport(L"embedded.svg", (temp / L"document.md").wstring(), cancel);
            auto pixel = embedded.pixels.data() + (10 * embedded.width + 10) * 4;
            require(pixel[2] == 255 && pixel[0] == 0 && pixel[3] == 255 && embedded.pixels[3] == 0,
                    "embedded base64 SVG icon renders at its placement; external resources stay ignored");
            std::ofstream(temp / L"oversized-text.svg")
                << "<svg width='100' height='20'><text y='15'>" << std::string(4097, 'a') << "</text></svg>";
            bool rejected = false;
            try {
                imageForExport(L"oversized-text.svg", (temp / L"document.md").wstring(), cancel);
            } catch (const std::exception &) {
                rejected = true;
            }
            require(rejected, "SVG text work is bounded before font layout");
        }
        {
            Images images(nullptr);
            for (auto name : {L"test%20image.png", L"test.jpg", L"test.gif", L"test.svg", L"classes.svg",
                              L"dashes.svg"}) {
                std::shared_ptr<Image> image;
                for (int i = 0; i < 300; ++i) {
                    image = images.get(name, (temp / L"document.md").wstring());
                    if (image)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                require(image && image->ready && !image->failed && image->width > 0 && image->height > 0 &&
                            image->pixels.size() == image->width * image->height * 4,
                        "local PNG/JPEG/GIF/SVG decode");
            }
            auto base = (temp / L"document.md").wstring();
            std::shared_ptr<Image> badge;
            for (int i = 0; i < 300 && !badge; ++i) {
                badge = images.get(L"badge.svg", base);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(badge && badge->ready && badge->svgSource, "badge vector source retained");
            // Removing the file proves DPI/zoom changes reuse source without I/O.
            std::filesystem::remove(temp / L"badge.svg");
            for (float dpi : {96.f, 120.f, 144.f, 192.f}) {
                for (float zoom : {.75f, 1.f, 1.5f, 2.f}) {
                    float scale = dpi / 96 * zoom;
                    std::shared_ptr<Image> sharp;
                    bool pending = true;
                    for (int i = 0; i < 300; ++i) {
                        sharp = images.raster(badge, 100 * scale, 20 * scale, &pending);
                        if (!pending)
                            break;
                        require(sharp == badge, "original badge stays available while rasterizing");
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    require(!pending && sharp && sharp->ready && sharp->width >= 100 * scale &&
                                sharp->height >= 20 * scale,
                            "SVG raster covers Windows DPI and document zoom");
                    require(images.raster(badge, 100 * scale, 20 * scale) == sharp,
                            "repeat layout reuses raster cache");
                    require(badge->width == 100 && badge->height == 20,
                            "raster resolution never changes intrinsic layout size");
                }
            }
            std::shared_ptr<Image> bounded;
            bool pending = true;
            for (int i = 0; i < 300 && pending; ++i) {
                bounded = images.raster(badge, 100000, 20000, &pending);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(!pending && bounded->width <= 4096 && bounded->height <= 4096,
                    "extreme SVG magnification remains bounded");
            auto png = images.get(L"test%20image.png", base);
            require(images.raster(png, 100, 100) == png, "raster images retain their existing decode");
            for (auto url : {L"\\\\server\\share\\image.png", L"%5C%5Cserver%5Cshare%5Cimage.png",
                             L"\\\\?\\C:\\image.png", L"//server/share/image.png"}) {
                auto rejected = images.get(url, (temp / L"document.md").wstring());
                require(rejected && rejected->failed, "network and device image paths are rejected");
            }
            std::shared_ptr<Image> missing;
            for (int i = 0; i < 300; ++i) {
                missing = images.get(L"missing.png", (temp / L"document.md").wstring());
                if (missing)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(missing && missing->failed, "missing image fallback");
            std::shared_ptr<Image> tiny;
            for (int i = 0; i < 300 && !tiny; ++i) {
                tiny = images.get(L"tiny-dash.svg", (temp / L"document.md").wstring());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(tiny && tiny->failed, "non-progressing SVG dashes fail without hanging");
        }
        {
            // Observe worker completion without retaining any UI image pointer.
            // The old eviction loop only appeared once layouts released the base.
            HWND window = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                        GetModuleHandleW(nullptr), nullptr);
            require(window != nullptr, "image cache completion window");
            {
                Images images(window);
                auto base = (temp / L"document.md").wstring();
                std::ofstream(temp / L"large-square.svg")
                    << "<svg width='2048' height='2048'><rect width='2048' height='2048' fill='blue'/></svg>";
                auto completed = [&] {
                    MSG message{};
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (!PeekMessageW(&message, window, WM_APP + 2, WM_APP + 2, PM_REMOVE)) {
                        require(std::chrono::steady_clock::now() < deadline, "SVG cache worker completes");
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                };
                images.get(L"large-square.svg", base);
                completed();
                std::weak_ptr<Image> original;
                {
                    auto image = images.get(L"large-square.svg", base);
                    require(image && image->ready, "large SVG base decodes");
                    original = image;
                }
                std::filesystem::remove(temp / L"large-square.svg");
                for (float size : {3840.f, 4096.f, 3072.f, 3840.f}) {
                    bool pending = false;
                    {
                        auto image = images.get(L"large-square.svg", base);
                        require(image && image == original.lock(), "large SVG never reloads its source");
                        images.raster(image, size, size, &pending);
                    }
                    if (pending)
                        completed();
                    auto begin = std::chrono::steady_clock::now();
                    for (unsigned repeat = 0; repeat < 100; ++repeat) {
                        auto image = images.get(L"large-square.svg", base);
                        require(image && image == original.lock(), "raster publication retains the URL base");
                        auto raster = images.raster(image, size, size, &pending);
                        require(!pending && raster && raster->ready && raster != image,
                                "repeat large SVG layouts reuse a completed raster");
                        require(image->pixels.size() + raster->pixels.size() + 2 * image->svgSource->size() <=
                                    64 * 1024 * 1024,
                                "base and enlarged raster fit the bounded image cache together");
                        require(image->width == 2048 && image->height == 2048,
                                "bounded raster does not change intrinsic layout size");
                    }
                    require(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100),
                            "cached large SVG layouts stay fast");
                }
            }
            DestroyWindow(window);
        }
        for (int workerCount : {2, 6}) {
            std::atomic_int started = 0, cancelledJobs = 0;
            Images images(nullptr, [&](const std::wstring &, const std::atomic_bool &cancel) {
                int ordinal = ++started;
                if (ordinal <= workerCount) {
                    while (!cancel.load())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    ++cancelledJobs;
                }
                Image image;
                image.width = image.height = 1;
                image.pixels.assign(4, ordinal <= workerCount ? 0 : 255);
                image.ready = true;
                return image;
            });
            auto base = (temp / L"document.md").wstring();
            std::wstring prefix = workerCount == 6 ? L"https://badges.invalid/" : L"";
            images.get(prefix + L"same.png", base);
            for (int i = 1; i < workerCount; ++i)
                images.get(prefix + L"slow" + std::to_wstring(i) + L".png", base);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (started < workerCount && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(started == workerCount, "local/remote image workers run concurrently");
            for (int i = 0; i < 16; ++i)
                images.get(prefix + L"queued" + std::to_wstring(i) + L".png", base);
            require(started == workerCount, "image worker concurrency remains bounded");
            images.cancelPending();
            std::shared_ptr<Image> current;
            while ((!current || cancelledJobs < workerCount) && std::chrono::steady_clock::now() < deadline) {
                current = images.get(prefix + L"same.png", base);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(current && current->ready && current->pixels[0] == 255 && started == workerCount + 1 &&
                        cancelledJobs == workerCount,
                    "document change cancels obsolete queue and rejects stale same-URL results");
        }
        auto path = temp / L"Unicode åäö 日本語.md";
        {
            std::ofstream file(path, std::ios::binary);
            file << "# Unicode\n\nHello";
        }
        auto oversizedPath = temp / L"oversized.md";
        HANDLE sparse = CreateFileW(oversizedPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        require(sparse != INVALID_HANDLE_VALUE, "sparse size-limit fixture");
        DWORD returned = 0;
        bool sparseSupported =
            DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
        CloseHandle(sparse);
        if (sparseSupported) {
            std::filesystem::resize_file(oversizedPath, 1000000001);
            bool oversizedRejected = false;
            try {
                loadSource(oversizedPath);
            } catch (const std::runtime_error &error) {
                oversizedRejected = std::string(error.what()).find("1 GB") != std::string::npos;
            }
            require(oversizedRejected, "oversized source rejected before allocation");
        } else {
            std::cout << "SKIP: filesystem does not support sparse size-limit fixture\n";
        }
        auto decodedSource = loadSource(path);
        require(decodedSource.source == L"# Unicode\n\nHello" && decodedSource.path == path &&
                    decodedSource.blocks.empty(),
                "bounded source reader preserves metadata");
        std::atomic_bool cancelled = true;
        bool sourceCancelled = false;
        try {
            loadSource(path, &cancelled);
        } catch (const std::runtime_error &) {
            sourceCancelled = true;
        }
        require(sourceCancelled, "source reader obeys cancellation");
        require(markdownLinkPath(L"My%20Notes.md#section", path) == temp / L"My Notes.md",
                "relative Markdown links decode spaces and separate fragments");
        require(markdownLinkPath(L"notes%23one.md", path) == temp / L"notes#one.md",
                "escaped filename hash remains part of filename");
        require(markdownLinkPath(L"%5C%5Cserver%5Cshare%5Cfile.md", path).empty() &&
                    markdownLinkPath(L"evil%3Afile.md", path).empty(),
                "encoded absolute and protocol links cannot become file launch paths");
        for (auto link : {L"x%22%20--configure-defaults%20%22x.md", L"x\" --configure-defaults \"x.md",
                          L"x%00.md", L"x%0a.md", L"x%3f.md", L"x%7c.md"})
            require(markdownLinkPath(link, path).empty(), "invalid decoded file names cannot inject options");
        require(markdownLinkPath(L"../other/My%20Notes.md", path) ==
                    (temp.parent_path() / L"other/My Notes.md"),
                "parent-relative document links remain supported");
        auto document = load(path);
        require(document.text.find(L"Hello") != document.text.npos, "Unicode file path");
        HGLOBAL drop = fileDrop(path);
        require(drop != nullptr, "Explorer file clipboard allocation");
        wchar_t dropped[32768]{};
        require(DragQueryFileW((HDROP)drop, 0xffffffff, nullptr, 0) == 1,
                "Explorer clipboard contains exactly one file");
        DragQueryFileW((HDROP)drop, 0, dropped, 32768);
        bool exactPath = std::filesystem::path(dropped) == std::filesystem::absolute(path);
        GlobalFree(drop);
        require(exactPath, "Explorer clipboard preserves full Unicode path");
        bool missing = false;
        try {
            load(temp / L"missing.md");
        } catch (...) {
            missing = true;
        }
        require(missing, "missing document");
        std::filesystem::remove_all(temp);
        CoUninitialize();
        std::cout << "PASS: isolated registry roundtrip, PNG/JPEG/GIF/SVG images, percent-escaped paths, "
                     "missing images, Unicode paths, missing files\n";
        return 0;
    } catch (const std::exception &e) {
        if (!temp.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(temp, ec);
        }
        std::cerr << "FAIL: " << e.what() << "\n";
        CoUninitialize();
        return 1;
    }
}
