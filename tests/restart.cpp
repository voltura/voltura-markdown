#include "../src/restart.cpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace vm;
    auto check = [](bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    std::filesystem::path owned;
    try {
        RestartView original;
        original.document = L"C:\\Documents with spaces\\läs mig.md";
        original.digest = restartDigest(L"unchanged document å");
        original.block = 791;
        original.within = 17.125;
        original.horizontal = 22.5f;
        original.zoom = 1.3f;
        original.source = true;
        original.fullscreen = true;
        original.placement.showCmd = SW_SHOWMAXIMIZED;
        original.placement.rcNormalPosition = {100, 80, 900, 780};
        owned = saveRestartView(original);
        check(!owned.empty(), "snapshot was not saved");
        auto restored = readRestartView(owned);
        check(restored && restored->document == original.document && restored->digest == original.digest &&
                  restored->block == original.block && restored->within == original.within &&
                  restored->horizontal == original.horizontal && restored->zoom == original.zoom &&
                  restored->source && restored->fullscreen &&
                  restored->placement.showCmd == SW_SHOWMAXIMIZED &&
                  restored->placement.rcNormalPosition.right == 900,
              "snapshot did not roundtrip");
        check(!std::filesystem::exists(owned) && !readRestartView(owned), "snapshot was not single use");
        check(restartDigest(L"changed document") != original.digest, "changed content matched");
        owned = saveRestartView(original);
        std::filesystem::resize_file(owned, 12);
        check(!readRestartView(owned), "truncated snapshot accepted");
        std::filesystem::remove(owned);
        original.zoom = std::nanf("");
        owned = saveRestartView(original);
        check(!readRestartView(owned), "non-finite snapshot accepted");
        std::filesystem::remove(owned);
        original.zoom = 1;
        owned = saveRestartView(original);
        {
            std::ofstream out(owned, std::ios::binary | std::ios::app);
            out.put('x');
        }
        check(!readRestartView(owned), "trailing snapshot data accepted");
        std::filesystem::remove(owned);
        check(!readRestartView(L"C:\\not-an-update-snapshot.view"), "foreign snapshot accepted");
        ManualSession session;
        wchar_t key[CCH_RM_SESSION_KEY + 1]{};
        check(RmStartSession(&session.handle, 0, key) == ERROR_SUCCESS, "test restart session");
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = sessionProc;
        cls.lpszClassName = L"VolturaManualSessionTest";
        RegisterClassW(&cls);
        HWND window = CreateWindowW(cls.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                    cls.hInstance, &session);
        check(window != nullptr, "test session window");
        check(SendMessageW(window, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF) == TRUE && session.ending,
              "sign-out query did not cancel manual helper");
        SendMessageW(window, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF);
        check(session.ending, "cancelled sign-out resumed installation");
        DestroyWindow(window);
        RmEndSession(session.handle);
        std::cout << "Restart snapshot roundtrip, exact offset, Unicode, content identity, single use, and "
                     "malformed state and simulated cancelled sign-out passed.\n";
        return 0;
    } catch (const std::exception &error) {
        if (!owned.empty())
            DeleteFileW(owned.c_str());
        std::cerr << error.what() << '\n';
        return 1;
    }
}
