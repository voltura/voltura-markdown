#include "shell.h"
#include <algorithm>
#include <filesystem>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <vector>
#include <wrl/client.h>
namespace vm {
bool copyText(HWND owner, std::wstring_view text) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!memory)
        return false;
    auto destination = static_cast<wchar_t *>(GlobalLock(memory));
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    destination[text.size()] = 0;
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    bool success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    if (!success)
        GlobalFree(memory);
    CloseClipboard();
    return success;
}
HGLOBAL fileDrop(const std::filesystem::path &path) {
    std::error_code error;
    auto full = std::filesystem::absolute(path, error).lexically_normal().wstring();
    if (error || full.empty())
        return nullptr;
    HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (full.size() + 2) * sizeof(wchar_t));
    if (!memory)
        return nullptr;
    auto drop = static_cast<DROPFILES *>(GlobalLock(memory));
    if (!drop) {
        GlobalFree(memory);
        return nullptr;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy(reinterpret_cast<BYTE *>(drop) + sizeof(DROPFILES), full.c_str(),
           (full.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    return memory;
}
bool copyFile(HWND owner, const std::filesystem::path &path) {
    auto drop = fileDrop(path);
    auto effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    UINT format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (!drop || !effect || !format) {
        if (drop)
            GlobalFree(drop);
        if (effect)
            GlobalFree(effect);
        return false;
    }
    auto value = static_cast<DWORD *>(GlobalLock(effect));
    if (!value) {
        GlobalFree(drop);
        GlobalFree(effect);
        return false;
    }
    *value = DROPEFFECT_COPY;
    GlobalUnlock(effect);
    if (!OpenClipboard(owner)) {
        GlobalFree(drop);
        GlobalFree(effect);
        return false;
    }
    bool success = false;
    if (EmptyClipboard() && SetClipboardData(CF_HDROP, drop)) {
        drop = nullptr; // The clipboard now owns the file list.
        if (SetClipboardData(format, effect)) {
            effect = nullptr;
            success = true;
        }
    }
    if (drop)
        GlobalFree(drop);
    if (effect)
        GlobalFree(effect);
    CloseClipboard();
    return success;
}
bool openFileDirectory(HWND owner, const std::filesystem::path &path) {
    std::error_code error;
    auto full = std::filesystem::absolute(path, error).lexically_normal();
    if (error || full.empty())
        return false;
    PIDLIST_ABSOLUTE item = nullptr;
    if (SUCCEEDED(SHParseDisplayName(full.c_str(), nullptr, &item, 0, nullptr))) {
        auto result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        CoTaskMemFree(item);
        if (SUCCEEDED(result))
            return true;
    }
    return (INT_PTR)ShellExecuteW(owner, L"open", full.parent_path().c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL) > 32;
}
HRESULT editFileWith(HWND owner, const std::filesystem::path &path) {
    std::error_code error;
    auto full = std::filesystem::absolute(path, error).lexically_normal().wstring();
    if (error || full.empty())
        return E_INVALIDARG;
    OPENASINFO info{};
    info.pcszFile = full.c_str();
    // The Windows 10/11 chooser executes this file once; it does not change
    // the default .md association. No editor paths are hardcoded.
    info.oaifInFlags = OAIF_EXEC;
    return SHOpenWithDialog(owner, &info);
}
static std::wstring config() {
    PWSTR root = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &root))) {
        path = root;
        CoTaskMemFree(root);
    } else
        return {};
    path += L"\\Voltura\\Markdown";
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path + L"\\preferences.ini";
}
Preferences readPreferences() {
    Preferences p;
    auto path = config();
    if (path.empty())
        return p;
    p.theme = GetPrivateProfileIntW(L"reader", L"theme", -1, path.c_str());
    p.theme = std::clamp(p.theme, -1, 1);
    p.zoom = std::clamp(GetPrivateProfileIntW(L"reader", L"zoom", 100, path.c_str()) / 100.f, .5f, 3.f);
    p.startFullscreen = GetPrivateProfileIntW(L"reader", L"startFullscreen", 1, path.c_str()) != 0;
    p.placed = GetPrivateProfileStructW(L"reader", L"placement", &p.placement, sizeof(p.placement),
                                        path.c_str()) != 0;
    if (p.placement.length != sizeof(WINDOWPLACEMENT))
        p.placed = false;
    return p;
}
void savePreferences(const Preferences &p) {
    auto path = config();
    if (path.empty())
        return;
    WritePrivateProfileStringW(L"reader", L"theme", std::to_wstring(p.theme).c_str(), path.c_str());
    WritePrivateProfileStringW(L"reader", L"zoom", std::to_wstring((int)(p.zoom * 100 + .5f)).c_str(),
                               path.c_str());
    WritePrivateProfileStringW(L"reader", L"startFullscreen", p.startFullscreen ? L"1" : L"0", path.c_str());
    if (p.placed)
        WritePrivateProfileStructW(L"reader", L"placement", (void *)&p.placement, sizeof(p.placement),
                                   path.c_str());
}
bool systemDark() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}
static bool sameText(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) == CSTR_EQUAL;
}
static std::wstring normalizedExecutable(const std::filesystem::path &path) {
    if (!path.is_absolute())
        return {};
    auto text = path.lexically_normal().wstring();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    // Resolve short names when available, but retain missing targets for moved portable copies.
    std::vector<wchar_t> full(32768);
    auto size = GetLongPathNameW(text.c_str(), full.data(), (DWORD)full.size());
    if (size && size < full.size())
        text.assign(full.data(), size);
    if (text.starts_with(L"\\\\?\\UNC\\"))
        text = L"\\\\" + text.substr(8);
    else if (text.starts_with(L"\\\\?\\"))
        text.erase(0, 4);
    return text;
}
MarkdownAssociation classifyMarkdownAssociation(std::wstring_view progId, std::wstring_view command,
                                                const std::filesystem::path &currentExecutable) {
    bool ownClass = sameText(progId, L"Voltura.Markdown");
    bool executableClass = sameText(progId, L"Applications\\vmd.exe");
    auto currentName = currentExecutable.filename().wstring();
    executableClass |= sameText(progId, L"Applications\\" + currentName);
    if (!ownClass && !executableClass)
        return progId.empty() ? MarkdownAssociation::Unknown : MarkdownAssociation::Other;
    if (command.empty())
        return MarkdownAssociation::Unknown;
    int count = 0;
    auto args = CommandLineToArgvW(std::wstring(command).c_str(), &count);
    if (!args)
        return MarkdownAssociation::Unknown;
    std::filesystem::path target = count ? args[0] : L"";
    LocalFree(args);
    auto registered = normalizedExecutable(target), current = normalizedExecutable(currentExecutable);
    if (registered.empty() || current.empty() || !sameText(target.extension().wstring(), L".exe"))
        return MarkdownAssociation::Unknown;
    return sameText(registered, current) ? MarkdownAssociation::Matching : MarkdownAssociation::Mismatch;
}
MarkdownAssociation probeMarkdownAssociation() {
    // Called only by the delayed worker. COM and all shell/path work stay off the UI thread.
    auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return MarkdownAssociation::Unknown;
    MarkdownAssociation result = MarkdownAssociation::Unknown;
    {
        Microsoft::WRL::ComPtr<IApplicationAssociationRegistration> associations;
        PWSTR progId = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationAssociationRegistration, nullptr,
                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&associations))) &&
            SUCCEEDED(associations->QueryCurrentDefault(L".md", AT_FILEEXTENSION, AL_EFFECTIVE, &progId))) {
            std::vector<wchar_t> exe(32768), command(32768);
            auto size = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
            DWORD length = (DWORD)command.size();
            bool application = std::wstring_view(progId).size() > 13 &&
                               sameText(std::wstring_view(progId).substr(0, 13), L"Applications\\");
            auto flags = (application ? ASSOCF_INIT_BYEXENAME : ASSOCF_INIT_FIXED_PROGID) | ASSOCF_NOFIXUPS;
            auto hr = AssocQueryStringW(flags, ASSOCSTR_COMMAND, application ? progId + 13 : progId, L"open",
                                        command.data(), &length);
            if (size && size < exe.size())
                result = classifyMarkdownAssociation(progId, SUCCEEDED(hr) ? command.data() : L"",
                                                     std::filesystem::path(exe.data()));
        }
        CoTaskMemFree(progId);
    }
    CoUninitialize();
    return result;
}
static LONG value(const std::wstring &key, const wchar_t *name, const std::wstring &data) {
    HKEY k;
    LONG result =
        RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)data.c_str(),
                                (DWORD)((data.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    return result;
}
std::wstring registerApplication(bool remove, bool integrateWithShell, bool *registered) {
    if (registered)
        *registered = false;
    const std::wstring app = L"Software\\Voltura\\Markdown";
    const std::wstring prog = L"Software\\Classes\\Voltura.Markdown";
    if (remove) {
        RegDeleteTreeW(HKEY_CURRENT_USER, prog.c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, (app + L"\\Capabilities").c_str());
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &k) ==
            ERROR_SUCCESS) {
            RegDeleteValueW(k, L"Voltura Markdown");
            RegCloseKey(k);
        }
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.md\\OpenWithProgids", 0, KEY_SET_VALUE,
                          &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, L"Voltura.Markdown");
            RegCloseKey(k);
        }
        if (integrateWithShell)
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return L"Registration removed. Your documents have not been changed.";
    }
    std::vector<wchar_t> buffer(32768);
    GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
    std::wstring exe = buffer.data();
    LONG error = 0;
    auto set = [&](const std::wstring &key, const wchar_t *name, const std::wstring &data) {
        auto r = value(key, name, data);
        if (r != ERROR_SUCCESS && !error)
            error = r;
    };
    set(prog, nullptr, L"Markdown document");
    set(prog + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
    set(prog + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"");
    set(prog, L"FriendlyTypeName", L"Markdown document");
    set(app + L"\\Capabilities", L"ApplicationName", L"Voltura Markdown");
    set(app + L"\\Capabilities", L"ApplicationDescription", L"Fast, read-only Markdown reader");
    set(app + L"\\Capabilities", L"ApplicationIcon", exe + L",0");
    set(app + L"\\Capabilities\\FileAssociations", L".md", L"Voltura.Markdown");
    set(L"Software\\RegisteredApplications", L"Voltura Markdown", app + L"\\Capabilities");
    HKEY k;
    auto created = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.md\\OpenWithProgids", 0, nullptr,
                                   0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (created == ERROR_SUCCESS) {
        auto r = RegSetValueExW(k, L"Voltura.Markdown", 0, REG_NONE, nullptr, 0);
        if (r && !error)
            error = r;
        RegCloseKey(k);
    } else if (!error)
        error = created;
    if (error)
        return L"Windows could not finish registration (error " + std::to_wstring(error) + L").";
    if (registered)
        *registered = true;
    if (integrateWithShell) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        openDefaultSettings();
    }
    return L"Voltura Markdown is registered. Windows opens this app's default settings where "
           L"supported; otherwise, find Voltura Markdown in Default apps. Confirm it for .md files. "
           L"Keep the executable at this location, or register it again after moving it.";
}
bool openDefaultSettings() {
    auto result =
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Voltura%20Markdown",
                      nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
        result = ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)result > 32;
}
std::filesystem::path markdownLinkPath(std::wstring_view link, const std::filesystem::path &base) {
    auto fragment = link.find(L'#');
    std::wstring encoded(link.substr(0, fragment));
    if (encoded.empty() || encoded.find(L'?') != encoded.npos)
        return {};
    std::wstring decoded(encoded.size() + 1, 0);
    DWORD length = (DWORD)decoded.size();
    if (FAILED(UrlUnescapeW(encoded.data(), decoded.data(), &length, URL_UNESCAPE_AS_UTF8)))
        return {};
    decoded.resize(length);
    if (decoded.find_first_of(L"\":<>|?*") != decoded.npos ||
        std::any_of(decoded.begin(), decoded.end(), [](wchar_t c) { return c < 32; }) ||
        decoded.starts_with(L"\\") || decoded.starts_with(L"/"))
        return {};
    auto path = (base.parent_path() / decoded).lexically_normal();
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension == L".md" || extension == L".markdown" ? path : std::filesystem::path{};
}
bool openLink(const std::wstring &link, const std::wstring &base) {
    std::wstring lower = link;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    if (lower.starts_with(L"https://") || lower.starts_with(L"http://") || lower.starts_with(L"mailto:"))
        return (INT_PTR)ShellExecuteW(nullptr, L"open", link.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
    auto path = markdownLinkPath(link, base);
    if (!path.empty()) {
        // Another document opens at its start; fragment navigation is local to this window.
        std::vector<wchar_t> exe(32768);
        GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
        auto args = L"\"" + path.wstring() + L"\"";
        return (INT_PTR)ShellExecuteW(nullptr, L"open", exe.data(), args.c_str(), nullptr, SW_SHOWNORMAL) >
               32;
    }
    return false;
}
std::wstring licenses() {
    std::wstring text =
        L"# Voltura Markdown\n\n**Version 1.1.5**\n\n"
        L"A fast, read-only Markdown reader for Windows.\n\n"
        L"## Created by\n\n- **Developer:** Joakim Skoglund\n"
        L"- **Company:** Voltura AB\n- **Website:** [markdown.voltura.se](https://markdown.voltura.se/)\n\n"
        L"## Help and navigation\n\n- <kbd>F1</kbd>: open Help for controls and shortcuts.\n"
        L"- **Back arrow** / <kbd>Escape</kbd>: return to your reading position.\n\n"
        L"## Open-source software\n\n"
        L"- **Voltura Markdown:** application and artwork · MIT license.\n"
        L"- **MD4C:** Markdown parser · MIT license.\n"
        L"- **NanoSVG:** static SVG renderer · zlib license.\n\n"
        L"The complete license notices follow.\n\n---\n\n";
    for (int id : {4, 2, 3}) {
        auto r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (r) {
            auto data = (const char *)LockResource(LoadResource(nullptr, r));
            auto size = SizeofResource(nullptr, r);
            int n = MultiByteToWideChar(CP_UTF8, 0, data, size, nullptr, 0);
            std::wstring s(n, 0);
            MultiByteToWideChar(CP_UTF8, 0, data, size, s.data(), n);
            text += id == 4   ? L"### Voltura Markdown\n\nApplication and artwork · MIT license\n\n```text\n"
                    : id == 2 ? L"### MD4C\n\nMarkdown parser · MIT license\n\n```text\n"
                              : L"### NanoSVG\n\nStatic SVG renderer · zlib license\n\n```text\n";
            text += s + L"\n```\n\n";
        }
    }
    return text;
}
} // namespace vm
