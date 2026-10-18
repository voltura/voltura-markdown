#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <windows.h>
namespace vm {
struct Preferences {
    int theme = -1;
    float zoom = 1;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    bool placed = false;
    bool startFullscreen = true;
    bool automaticUpdates = false;
};
Preferences readPreferences();
void savePreferences(const Preferences &p);
void saveAutomaticUpdates(bool enabled);
bool systemDark();
enum class MarkdownAssociation { Matching, Mismatch, Other, Unknown };
MarkdownAssociation classifyMarkdownAssociation(std::wstring_view progId, std::wstring_view command,
                                                const std::filesystem::path &currentExecutable);
MarkdownAssociation probeMarkdownAssociation();
std::wstring registerApplication(bool remove, bool integrateWithShell = true, bool *registered = nullptr);
bool openDefaultSettings();
std::filesystem::path markdownLinkPath(std::wstring_view link, const std::filesystem::path &base);
bool openLink(const std::wstring &link, const std::wstring &base);
std::wstring licenses();
bool copyText(HWND owner, std::wstring_view text);
HGLOBAL fileDrop(const std::filesystem::path &path);
bool copyFile(HWND owner, const std::filesystem::path &path);
bool openFileDirectory(HWND owner, const std::filesystem::path &path);
HRESULT editFileWith(HWND owner, const std::filesystem::path &path);
} // namespace vm
