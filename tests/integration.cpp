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
int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::filesystem::path temp;
    try {
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
        {
            Images images(nullptr);
            for (auto name : {L"test%20image.png", L"test.jpg", L"test.gif", L"test.svg"}) {
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
