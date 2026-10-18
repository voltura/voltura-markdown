#include "restart.h"
#include <algorithm>
#include <atomic>
#include <bcrypt.h>
#include <cmath>
#include <fstream>
#include <restartmanager.h>
#include <shlobj.h>
#include <thread>
#include <tlhelp32.h>
#include <vector>

namespace vm {
namespace {
std::filesystem::path executable() {
    std::wstring value(32768, 0);
    auto count = GetModuleFileNameW(nullptr, value.data(), (DWORD)value.size());
    value.resize(count);
    return value;
}
bool samePath(const std::filesystem::path &a, const std::filesystem::path &b) {
    return _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
}
std::filesystem::path sessionDirectory() {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        return {};
    auto result = std::filesystem::path(local) / L"Voltura" / L"Markdown" / L"UpdateSessions";
    CoTaskMemFree(local);
    return result;
}
struct DiskView {
    DWORD magic = 0x31524d56;
    DWORD bytes = sizeof(DiskView);
    FILETIME created{};
    std::array<unsigned char, 32> digest{};
    WINDOWPLACEMENT placement{};
    double within = 0;
    uint64_t block = 0;
    float horizontal = 0, zoom = 1;
    DWORD source = 0, fullscreen = 0;
    DWORD documentLength = 0, executableLength = 0;
};
bool ordinary(const std::filesystem::path &path) {
    for (auto current = path; !current.empty();) {
        auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return false;
        auto parent = current.parent_path();
        if (parent == current)
            break;
        current = std::move(parent);
    }
    return true;
}
struct ManualSession {
    DWORD handle = 0;
    std::atomic_bool ending = false, done = false;
};
LRESULT CALLBACK sessionProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto state = (ManualSession *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        state = (ManualSession *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state);
    }
    if (state && (message == WM_QUERYENDSESSION || message == WM_ENDSESSION)) {
        state->ending = true;
        RmCancelCurrentTask(state->handle);
        return message == WM_QUERYENDSESSION ? TRUE : 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
std::vector<RM_UNIQUE_PROCESS> readers(const std::filesystem::path &target) {
    std::vector<RM_UNIQUE_PROCESS> result;
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        throw std::runtime_error("process snapshot");
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, target.filename().c_str()))
            continue;
        auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        std::wstring path(32768, 0);
        DWORD length = (DWORD)path.size();
        FILETIME created{}, exited{}, kernel{}, user{};
        if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
            path.resize(length);
            if (samePath(path, target) && GetProcessTimes(process, &created, &exited, &kernel, &user))
                result.push_back({entry.th32ProcessID, created});
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return result;
}
} // namespace

std::array<unsigned char, 32> restartDigest(std::wstring_view source) {
    std::array<unsigned char, 32> digest{};
    if (source.size() > MAXDWORD / sizeof(wchar_t) ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)source.data(),
                   (ULONG)(source.size() * sizeof(wchar_t)), digest.data(), (ULONG)digest.size()) < 0)
        throw std::runtime_error("restart digest");
    return digest;
}
std::filesystem::path saveRestartView(const RestartView &view) {
    auto directory = sessionDirectory();
    if (directory.empty())
        return {};
    std::filesystem::create_directories(directory);
    if (!ordinary(directory))
        return {};
    // Only manual updates visit this cache. Keep abandoned snapshots bounded.
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    auto ticks = [](FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    size_t retained = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (entry.path().extension() != L".view" || !ordinary(entry.path()) ||
            !GetFileAttributesExW(entry.path().c_str(), GetFileExInfoStandard, &data) ||
            data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (ticks(now) - ticks(data.ftLastWriteTime) > 7ULL * 86400 * 10000000)
            DeleteFileW(entry.path().c_str());
        else
            ++retained;
    }
    if (retained >= 64)
        return {};
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        return {};
    wchar_t name[40]{};
    StringFromGUID2(id, name, 40);
    auto file = directory / (std::wstring(name) + L".view");
    auto app = executable().wstring(), document = view.document.wstring();
    if (app.size() > 32767 || document.size() > 32767)
        return {};
    DiskView disk;
    disk.created = now;
    disk.digest = view.digest;
    disk.placement = view.placement;
    disk.within = view.within;
    disk.block = view.block;
    disk.horizontal = view.horizontal;
    disk.zoom = view.zoom;
    disk.source = view.source;
    disk.fullscreen = view.fullscreen;
    disk.documentLength = (DWORD)document.size();
    disk.executableLength = (DWORD)app.size();
    std::ofstream output(file, std::ios::binary);
    output.write((const char *)&disk, sizeof(disk));
    output.write((const char *)app.data(), app.size() * sizeof(wchar_t));
    output.write((const char *)document.data(), document.size() * sizeof(wchar_t));
    output.close();
    if (!output) {
        DeleteFileW(file.c_str());
        return {};
    }
    return file;
}
std::optional<RestartView> readRestartView(const std::filesystem::path &file) {
    if (!file.is_absolute() || !samePath(file.parent_path(), sessionDirectory()) ||
        file.extension() != L".view" || !ordinary(file))
        return {};
    std::ifstream input(file, std::ios::binary);
    DiskView disk{};
    input.read((char *)&disk, sizeof(disk));
    if (!input || disk.magic != 0x31524d56 || disk.bytes != sizeof(disk) || disk.documentLength > 32767 ||
        !disk.executableLength || disk.executableLength > 32767 ||
        disk.placement.length != sizeof(WINDOWPLACEMENT) || !std::isfinite(disk.within) || disk.within < 0 ||
        !std::isfinite(disk.horizontal) || disk.horizontal < 0 || !std::isfinite(disk.zoom) ||
        disk.zoom < .5f || disk.zoom > 3 || disk.source > 1 || disk.fullscreen > 1)
        return {};
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER current{{now.dwLowDateTime, now.dwHighDateTime}},
        created{{disk.created.dwLowDateTime, disk.created.dwHighDateTime}};
    if (current.QuadPart < created.QuadPart || current.QuadPart - created.QuadPart > 86400ULL * 10000000)
        return {};
    std::wstring app(disk.executableLength, 0), document(disk.documentLength, 0);
    input.read((char *)app.data(), app.size() * sizeof(wchar_t));
    input.read((char *)document.data(), document.size() * sizeof(wchar_t));
    if (!input || input.peek() != EOF || !samePath(app, executable()) ||
        document.find(L'\0') != std::wstring::npos || app.find(L'\0') != std::wstring::npos)
        return {};
    input.close();
    DeleteFileW(file.c_str());
    return RestartView{document,        disk.digest, disk.placement,   disk.within,         disk.block,
                       disk.horizontal, disk.zoom,   disk.source != 0, disk.fullscreen != 0};
}
int manualUpdateHelper(int argc, wchar_t **argv) {
    if (argc < 2 || std::wstring_view(argv[1]) != L"--manual-update")
        return -1;
    if (argc != 4)
        return 1;
    std::filesystem::path target = argv[2], staged = argv[3];
    if (!target.is_absolute() || target.filename() != L"vmd.exe" ||
        staged != target.parent_path() / L"vmd.update-new.exe" || !ordinary(staged) ||
        samePath(executable(), target) || GetSystemMetrics(SM_SHUTTINGDOWN))
        return 1;
    ManualSession state;
    wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&state.handle, 0, key) != ERROR_SUCCESS)
        return 1;
    WNDCLASSW cls{};
    cls.lpfnWndProc = sessionProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"VolturaMarkdownManualUpdate";
    RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                cls.hInstance, &state);
    if (!window) {
        RmEndSession(state.handle);
        return 1;
    }
    int result = 1;
    std::thread worker([&] {
        bool shutdownRequested = false;
        try {
            auto processes = readers(target);
            bool closed = processes.empty();
            if (!closed && !state.ending &&
                RmRegisterResources(state.handle, 0, nullptr, (UINT)processes.size(), processes.data(), 0,
                                    nullptr) == ERROR_SUCCESS) {
                shutdownRequested = true;
                closed = RmShutdown(state.handle, 0, nullptr) == ERROR_SUCCESS;
            }
            if (closed && !state.ending && !GetSystemMetrics(SM_SHUTTINGDOWN) && readers(target).empty()) {
                auto backup = target.parent_path() / L"vmd.update-old.exe";
                if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES &&
                    ReplaceFileW(target.c_str(), staged.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
                    DeleteFileW(backup.c_str());
                    result = 0;
                } else if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
                    MoveFileW(backup.c_str(), target.c_str());
            }
        } catch (...) {
        }
        // Also restore readers if replacement failed; the original binary remains usable.
        if (shutdownRequested && !state.ending && !GetSystemMetrics(SM_SHUTTINGDOWN))
            RmRestart(state.handle, 0, nullptr);
        state.done = true;
        PostMessageW(window, WM_NULL, 0, 0);
    });
    while (!state.done) {
        MSG message{};
        if (GetMessageW(&message, nullptr, 0, 0) <= 0) {
            state.ending = true;
            RmCancelCurrentTask(state.handle);
            break;
        }
        DispatchMessageW(&message);
    }
    worker.join();
    DestroyWindow(window);
    RmEndSession(state.handle);
    return result;
}
} // namespace vm
