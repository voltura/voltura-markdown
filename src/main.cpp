#include "code-syntax.h"
#include "dialog-theme.h"
#include "document.h"
#include "editor.h"
#include "empty-state.h"
#include "image-layout.h"
#include "images.h"
#include "restart.h"
#include "search.h"
#include "shell.h"
#include "table-layout.h"
#include "updates.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <fstream>
#include <functional>
#ifdef VMD_TESTING
#include <iomanip>
#endif
#include <list>
#include <memory>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#ifdef VMD_TESTING
#define VMD_TEST_ONLY(...) __VA_ARGS__
#else
#define VMD_TEST_ONLY(...)
#endif
namespace {
using namespace vm;
constexpr UINT Loaded = WM_APP + 1, ImagesReady = WM_APP + 2, ScheduleAssociation = WM_APP + 4;
constexpr float bottom = 24;
struct Palette {
    D2D1_COLOR_F bg, fg, muted, panel, border, accent, selection, code;
};
D2D1_COLOR_F rgb(unsigned c, float a = 1) {
    return D2D1::ColorF(c, a);
}
Palette palette(bool dark) {
    return dark ? Palette{rgb(0x101319), rgb(0xe5e9f0), rgb(0x929eaf),       rgb(0x1c222d),
                          rgb(0x303a49), rgb(0x90b9ff), rgb(0x3865a0, .65f), rgb(0x181e28)}
                : Palette{rgb(0xfafbfd), rgb(0x202938), rgb(0x66758a),       rgb(0xffffff),
                          rgb(0xdce2eb), rgb(0x275fc7), rgb(0x94bbff, .55f), rgb(0xf0f3f8)};
}
struct Heights {
    std::vector<double> tree, values;
    void reset(size_t n) {
        tree.assign(n + 1, 0);
        values.assign(n, 0);
    }
    void set(size_t i, double v) {
        double delta = v - values[i];
        values[i] = v;
        for (++i; i < tree.size(); i += i & (~i + 1))
            tree[i] += delta;
    }
    double before(size_t i) const {
        double n = 0;
        for (; i; i -= i & (~i + 1))
            n += tree[i];
        return n;
    }
    double total() const {
        return before(values.size());
    }
    size_t find(double y) const {
        size_t at = 0;
        double sum = 0;
        size_t step = 1;
        while (step < tree.size())
            step <<= 1;
        for (; step; step >>= 1) {
            auto next = at + step;
            if (next < tree.size() && sum + tree[next] <= y) {
                at = next;
                sum += tree[next];
            }
        }
        return std::min(at, values.empty() ? size_t(0) : values.size() - 1);
    }
};
struct Part {
    ComPtr<IDWriteTextLayout> layout;
    ComPtr<ID2D1Bitmap> bitmap;
    std::vector<D2D1_RECT_F> keycaps, inlineCode;
    std::vector<CodeToken> codeTokens;
    uint32_t start, length;
    float x = 0, y = 0, width = 0, height = 0;
    float headingDividerWidth = 0;
};
struct Layout {
    std::vector<Part> parts;
    std::vector<D2D1_RECT_F> tableBorders, tableHeaders;
    std::vector<float> cellEdges;
    ComPtr<ID2D1Bitmap> bitmap;
    float height = 0, width = 0, tableGap = 0, left = 0;
    bool imagePending = false, plain = false;
    uint64_t used = 0;
    size_t cost = 0;
};
struct LoadedDocument {
    uint64_t generation;
    std::shared_ptr<Document> document;
    std::wstring error;
    bool restoreUnchanged = false;
    bool offerCreate = false, missing = false;
};
struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic_bool> cancel;
    std::shared_ptr<std::atomic_bool> done;
};
struct App {
    struct PreparedRenderer {
        std::atomic_bool ready = false;
        ComPtr<ID2D1Factory> factory;
        ComPtr<ID2D1HwndRenderTarget> target;
        HRESULT result = E_PENDING;
#ifdef VMD_TESTING
        double prepareMs = 0;
#endif
    };
#ifdef VMD_TESTING
    bool stagedRenderer = true;
#else
    static constexpr bool stagedRenderer = true;
#endif
    bool rendererStarted = false, hardwareActive = false;
    bool closeRequested = false, promotionPending = false;
#ifdef VMD_TESTING
    bool failRendererPreparation = false, failRendererFirstFrame = false, fallbackAwaitingFrame = false;
    bool failDeviceBrush = false;
#endif
    std::shared_ptr<PreparedRenderer> preparedRenderer;
    std::thread rendererWorker;
#ifdef VMD_TESTING
    double rendererPromotionMs = 0;
    std::chrono::steady_clock::time_point promotionBegan;
#endif
    ComPtr<ID2D1Factory> softwareFactory;
    ComPtr<ID2D1HwndRenderTarget> softwareTarget;
    ComPtr<ID2D1SolidColorBrush> softwareBrush;
    void finishRenderer() {
        if (!rendererWorker.joinable())
            return;
        // Window destruction must keep servicing cross-thread window messages until creation ends.
        HANDLE worker = rendererWorker.native_handle();
        bool quit = false;
        int quitCode = 0;
        while (WaitForSingleObject(worker, 0) == WAIT_TIMEOUT) {
            if (MsgWaitForMultipleObjects(1, &worker, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) {
                        quit = true;
                        quitCode = (int)message.wParam;
                        break;
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        }
        rendererWorker.join();
        if (quit)
            PostQuitMessage(quitCode);
    }
    ~App() {
        finishRenderer();
    }
    HWND hwnd = nullptr, canvas = nullptr, tooltip = nullptr, search = nullptr, backButton = nullptr;
    std::array<HWND, 7> buttons{};
    std::array<HWND, 3> zoomButtons{};
    float top = 78;
    HWND zoomPanel = nullptr;
    bool zoomExpanded = false, zoomFadeVisible = false;
    BYTE zoomAlpha = 0, zoomFadeStartAlpha = 0;
    ULONGLONG zoomUntil = 0, zoomFadeStarted = 0;
    float zoomFadeDuration = 150;
    Preferences prefs;
    Updates updates;
    bool updateTimerStarted = false, sessionEnding = false;
    bool dark = false, source = false, fullscreen = false, showControls = true, dragging = false,
         searching = false, loading = false;
    bool closing = false, searchMatch = false, launchedWithFile = false;
#ifdef VMD_TESTING
    bool selfTest = false;
    bool testAutomaticUpdates = false;
    std::filesystem::path updateFixture;
#endif
    float dpi = 96, zoom = 1, width = 900, height = 700, horizontal = 0, maxWidth = 0;
    float chromeWidth = 900, canvasLeft = 0;
    bool zoomLayoutPending = false;
    struct CodeButton {
        size_t block;
        D2D1_RECT_F bounds;
    };
    std::vector<CodeButton> codeButtons;
    float codePointerX = -1, codePointerY = -1;
    double codeButtonScroll = -1;
    float codeButtonHorizontal = -1;
    size_t pressedCode = SIZE_MAX, copiedCode = SIZE_MAX;
    ULONGLONG copiedCodeUntil = 0;
    unsigned viewportUpdates = 0;
    int scrollDrag = 0;
    float dragStart = 0;
    double dragScroll = 0;
    double scroll = 0;
    std::wstring hoveredLink;
    double hoverScroll = 0;
    float hoverHorizontal = 0;
    double scrollTarget = 0;
    bool scrollAnimating = false;
    std::chrono::steady_clock::time_point scrollTick;
    WINDOWPLACEMENT saved{sizeof(WINDOWPLACEMENT)};
    DWORD oldStyle = 0;
    uint32_t anchor = 0, caret = 0;
    uint64_t generation = 0, tick = 0;
    std::shared_ptr<Document> doc;
    std::unique_ptr<Editor> editor;
    std::shared_ptr<Document> editOriginal;
    std::function<void()> afterEditSave;
    bool leaveEdit(std::function<void()> action) {
        if (!editor)
            return true;
        auto choice = editor->leave();
        if (choice == Editor::Leave::Saving)
            afterEditSave = std::move(action);
        if (choice != Editor::Leave::Allow)
            return false;
        if (informationView)
            closeInformation();
        editor.reset();
        doc = std::move(editOriginal);
        source = false;
        anchor = caret = 0;
        searchMatch = false;
        ++generation;
        rawBlocks.clear();
        cancelSearch();
        resetLayout(false);
        positions();
        ShowWindow(canvas, SW_SHOWNA);
        SetWindowTextW(hwnd, doc && !doc->path.empty()
                                 ? (doc->path.filename().wstring() + L" — Voltura Markdown").c_str()
                                 : L"Voltura Markdown");
        invalidate();
        return true;
    }
    void finishEdit() {
        if (leaveEdit([this] { finishEdit(); }))
            SetFocus(hwnd);
    }
    void beginEdit(bool create = false, std::filesystem::path createPath = {}) {
        if (loading)
            return;
        if (create && !leaveEdit([this, createPath] { beginEdit(true, createPath); }))
            return;
        if (editor) {
            if (informationView)
                closeInformation();
            if (editor->isPreview())
                editor->togglePreview();
            editor->setVisible(true);
            editor->focus();
            return;
        }
        if (informationView)
            closeInformation();
        if (!create && (!doc || doc->plainSource))
            return;
        if (create) {
            doc = std::make_shared<Document>(parse(L""));
            doc->path = std::move(createPath);
            source = false;
            rawBlocks.clear();
            resetLayout(false);
        }
        cancelSearch();
        searching = false;
        ShowWindow(search, SW_HIDE);
        editOriginal = doc;
        editor = std::make_unique<Editor>(hwnd, *doc, dark, (unsigned)dpi, create);
        visibility(true);
        KillTimer(hwnd, 2);
        positions();
        editor->setVisible(true);
        editor->focus();
    }
    std::deque<Block> rawBlocks;
    Heights heights;
    std::unordered_map<size_t, Layout> cache;
    std::unordered_map<uint32_t, float> codeWidths;
    size_t cacheCost = 0;
    std::vector<Worker> workers;
    std::unique_ptr<Images> images;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> write;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> uiFormat;
    HFONT uiFont = nullptr;
    HICON largeIcon = nullptr, smallIcon = nullptr;
    std::wstring notice, query, lastError;
    std::filesystem::path pendingPath;
#ifdef VMD_TESTING
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now(), openStarted = started;
#endif
    bool firstPaint = false;
    enum class AssociationPhase { WaitingForPaint, Posted, Delay, Running, Finished };
    AssociationPhase associationPhase = AssociationPhase::WaitingForPaint;
    uint64_t associationReadyGeneration = UINT64_MAX;
    struct AssociationResult {
        std::atomic_bool done = false;
        MarkdownAssociation association = MarkdownAssociation::Unknown;
        bool registration = false, succeeded = false;
    };
    std::shared_ptr<AssociationResult> associationResult;
    HWND associationPanel = nullptr;
    std::array<HWND, 3> associationButtons{};
    std::wstring associationError;
#ifdef VMD_TESTING
    std::wstring testAssociation;
#endif
#ifdef VMD_TESTING
    std::wstring metricsFile;
    std::vector<std::pair<const char *, double>> startupPhases;
    void startupPhase(const char *name) {
        if (!metricsFile.empty() && !firstPaint)
            startupPhases.emplace_back(
                name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                          .count());
    }
    bool exitOnContent = false, scrollBenchmark = false, benchmarkAnimations = false;
    std::chrono::steady_clock::time_point heartbeatAt, scrollBenchmarkStarted;
    double loadingHeartbeatMaxMs = 0;
    unsigned loadingHeartbeatSamples = 0;
    bool verifyCloseFullscreen = false;
    std::wstring verifyFile;
    int verifyStage = 0, scrollFrames = 0;
    int scrollBenchmarkDirection = 1;
    unsigned scrollBenchmarkReversals = 0;
    std::array<uint64_t, 6> buttonPaints{};
    uint64_t toolbarBeforeScroll = 0, toolbarBeforeReload = 0;
    uint64_t spinnerPaints = 0, spinnerBeforeReload = 0;
    bool verificationReload = false;
    bool editorPreviewAwaitingPaint = false;
    uint64_t verificationCountGeneration = 0;
    bool checksPassed = true;
    std::vector<double> paintTimes;
#endif
    struct MenuRow {
        UINT id;
        std::wstring text;
        bool separator = false, enabled = true, checked = false;
    };
    std::vector<MenuRow> menuRows;
    struct SavedView {
        std::shared_ptr<Document> document;
        std::deque<Block> raw;
        bool source;
        double scroll;
        float horizontal, zoom;
        uint32_t anchor, caret;
        size_t blockIndex;
        double withinBlock;
    };
    std::unique_ptr<SavedView> informationView;
    std::optional<RestartView> restartView;
    std::filesystem::path restartFile;
#ifdef VMD_TESTING
    std::filesystem::path restartReport;
    bool restartMatched = false;
    void reportRestart(const char *suffix) {
        if (restartReport.empty())
            return;
        std::ofstream out(restartReport.wstring() + fromUtf8(suffix));
        size_t index = heights.values.empty() ? 0 : heights.find(scroll);
        out << std::setprecision(17) << index << ' '
            << (heights.values.empty() ? 0 : scroll - heights.before(index)) << ' ' << horizontal << ' '
            << zoom << ' ' << source << ' ' << fullscreen << ' ' << width << ' ' << height << ' '
            << restartMatched << '\n';
    }
#endif
    void prepareRestart() {
        if (informationView)
            closeInformation();
        stopScrolling();
        RestartView view;
        if (loading)
            view.document = std::filesystem::absolute(pendingPath);
        else if (doc && !doc->path.empty()) {
            view.document = std::filesystem::absolute(doc->path);
            view.digest = restartDigest(doc->source);
        }
        view.block = heights.values.empty() ? 0 : heights.find(scroll);
        view.within = heights.values.empty() ? 0 : scroll - heights.before((size_t)view.block);
        view.horizontal = horizontal;
        view.zoom = zoom;
        view.source = source;
        view.fullscreen = fullscreen;
        view.placement = saved;
        if (!fullscreen)
            GetWindowPlacement(hwnd, &view.placement);
        restartFile = saveRestartView(view);
        if (!restartFile.empty()) {
            auto command = L"--restore-update \"" + restartFile.wstring() + L"\"";
#ifdef VMD_TESTING
            if (!restartReport.empty()) {
                reportRestart(".before");
                command += L" --test-restart-report \"" + restartReport.wstring() + L"\"";
                wchar_t desktop[256]{};
                DWORD bytes = 0;
                if (GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop,
                                              sizeof(desktop), &bytes))
                    command += L" --test-restart-desktop \"" + std::wstring(desktop) + L"\"";
            }
#endif
            if (FAILED(RegisterApplicationRestart(command.c_str(),
                                                  RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT))) {
                DeleteFileW(restartFile.c_str());
                restartFile.clear();
            }
        }
    }
    bool menuOpen = false;
    POINT lastPointer{};
    bool pointerKnown = false;
    bool pointerMoved(POINT point) {
        bool moved = !pointerKnown || point.x != lastPointer.x || point.y != lastPointer.y;
        lastPointer = point;
        pointerKnown = true;
        return moved;
    }
    bool pointerOverControls() const {
        POINT point;
        if (!GetCursorPos(&point) || !ScreenToClient(hwnd, &point))
            return false;
        RECT client;
        GetClientRect(hwnd, &client);
        RECT zoomBounds{};
        if (zoomPanel && IsWindowVisible(zoomPanel)) {
            GetWindowRect(zoomPanel, &zoomBounds);
            MapWindowPoints(nullptr, hwnd, (POINT *)&zoomBounds, 2);
        }
        return point.x >= 0 && point.x < client.right && point.y >= 0 &&
               (point.y < top * scale() || PtInRect(&zoomBounds, point));
    }
    std::deque<Block> &blocks() {
        return source ? rawBlocks : doc->blocks;
    }
    const std::wstring &text() {
        return source || doc->plainSource ? doc->source : doc->text;
    }
    float scale() const {
        return dpi / 96.f;
    }
    float left() const {
        return source ? 28.f : std::max(28.f, (width - 860.f * zoom) / 2);
    }
    float documentLeft() const {
        return left() - horizontal;
    }
    float contentWidth() const {
        return std::max(100.f, width - 2 * left());
    }
    void invalidate() {
        if (canvas)
            InvalidateRect(canvas, nullptr, FALSE);
    }
    void invalidateHeader() {
        RECT r;
        GetClientRect(hwnd, &r);
        r.bottom = (LONG)(78 * scale());
        InvalidateRect(hwnd, &r, FALSE);
    }
    void paintHeader() {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        r.bottom = (LONG)(top * scale());
        RECT dirty;
        if (!IntersectRect(&dirty, &r, &ps.rcPaint)) {
            EndPaint(hwnd, &ps);
            return;
        }
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, dirty.right - dirty.left, dirty.bottom - dirty.top);
        auto oldBitmap = SelectObject(dc, bitmap);
        SetViewportOrgEx(dc, -dirty.left, -dirty.top, nullptr);
        HBRUSH background = CreateSolidBrush(dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
        FillRect(dc, &r, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dark ? RGB(146, 158, 175) : RGB(102, 117, 138));
        auto oldFont = SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));
        if (searching && showControls) {
            auto box = CreateSolidBrush(dark ? RGB(28, 34, 45) : RGB(255, 255, 255));
            auto oldBrush = SelectObject(dc, box);
            auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            float boxWidth = searchBoxWidth();
            RoundRect(dc, (int)(28 * scale()), (int)(20 * scale()), (int)((28 + boxWidth) * scale()),
                      (int)(54 * scale()), (int)(8 * scale()), (int)(8 * scale()));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(box);
            RECT countBounds{(LONG)((28 + boxWidth - searchCountWidth()) * scale()), (LONG)(20 * scale()),
                             (LONG)((20 + boxWidth) * scale()), (LONG)(54 * scale())};
            DrawTextW(dc, searchStatus.c_str(), (int)searchStatus.size(), &countBounds,
                      DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            if (showSearchHint()) {
                RECT hintBounds{(LONG)(28 * scale()), (LONG)(55 * scale()), (LONG)((20 + boxWidth) * scale()),
                                (LONG)(76 * scale())};
                DrawTextW(dc, L"Enter: next · Shift+Enter: previous", -1, &hintBounds,
                          DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            }
        }
        std::wstring title = doc ? doc->path.filename().wstring() : L"";
        if (editor && !informationView)
            title = (editor->path().empty() ? L"Untitled" : editor->path().filename().wstring()) +
                    std::wstring(editor->modified() ? L" *" : L"");
        if (showControls && !searching && loading) {
#ifdef VMD_TESTING
            if (!verifyFile.empty())
                ++spinnerPaints;
#endif
            float phase = (GetTickCount64() % 1200) / 1200.f * 6.2831853f;
            auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            for (int i = 0; i < 8; ++i) {
                float angle = phase + i * 6.2831853f / 8;
                float alpha = (i + 1) / 8.f;
                auto mix = [&](int background, int foreground) {
                    return (int)(background + (foreground - background) * alpha);
                };
                COLORREF color = dark ? RGB(mix(16, 144), mix(19, 185), mix(25, 255))
                                      : RGB(mix(250, 39), mix(251, 95), mix(253, 199));
                HBRUSH dot = CreateSolidBrush(color);
                auto previous = SelectObject(dc, dot);
                float x = 36 + cosf(angle) * 7, y = 37 + sinf(angle) * 7;
                Ellipse(dc, (int)((x - 1.5f) * scale()), (int)((y - 1.5f) * scale()),
                        (int)((x + 1.5f) * scale()), (int)((y + 1.5f) * scale()));
                SelectObject(dc, previous);
                DeleteObject(dot);
            }
            SelectObject(dc, oldPen);
        } else if (showControls && !searching && chromeWidth > 600) {
            RECT label{(LONG)(28 * scale()), (LONG)(26 * scale()), (LONG)((chromeWidth - 320) * scale()),
                       (LONG)(54 * scale())};
            DrawTextW(dc, title.c_str(), (int)title.size(), &label,
                      DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
        }
        BitBlt(screen, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top, dc,
               dirty.left, dirty.top, SRCCOPY);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
    }
    static LRESULT CALLBACK canvasProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
        auto a = (App *)GetWindowLongPtrW(w, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            a = (App *)((CREATESTRUCTW *)lp)->lpCreateParams;
            SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)a);
        }
        if (!a)
            return DefWindowProcW(w, msg, wp, lp);
        if (msg == WM_PAINT) {
            a->paint();
            return 0;
        }
        if (msg == WM_ERASEBKGND)
            return 1;
        if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            POINT point;
            if (GetCursorPos(&point) && ScreenToClient(a->hwnd, &point)) {
                SetCursor(a->documentCursor(point.x / a->scale() - a->canvasLeft, point.y / a->scale()));
                return TRUE;
            }
        }
        if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
            return a->message(msg, wp, lp);
        if (msg == WM_MOUSELEAVE) {
            a->codePointerX = a->codePointerY = -1;
            a->hoverLink(L"");
            a->invalidate();
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEMOVE) {
            int y = GET_Y_LPARAM(lp) + (int)(a->top * a->scale());
            return a->message(msg, wp, MAKELPARAM(GET_X_LPARAM(lp) + (int)(a->canvasLeft * a->scale()), y));
        }
        if (msg == WM_CAPTURECHANGED)
            return a->message(msg, wp, lp);
        return DefWindowProcW(w, msg, wp, lp);
    }
    void colour(D2D1_COLOR_F c) {
        brush->SetColor(c);
    }
    void fill(D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0) {
        colour(c);
        if (radius)
            target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
        else
            target->FillRectangle(r, brush.Get());
    }
    void line(float x, float y, float x2, float y2, D2D1_COLOR_F c, float stroke = 1) {
        colour(c);
        target->DrawLine({x, y}, {x2, y2}, brush.Get(), stroke);
    }
    void label(std::wstring_view s, D2D1_RECT_F r, D2D1_COLOR_F c, float size = 14) {
        ComPtr<IDWriteTextFormat> f;
        write->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
        colour(c);
        target->DrawTextW(s.data(), (UINT32)s.size(), f.Get(), r, brush.Get());
    }
    bool device() {
        if (target)
            return true;
        RECT r;
        GetClientRect(canvas, &r);
        HRESULT hr = d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(stagedRenderer && !hardwareActive ? D2D1_RENDER_TARGET_TYPE_SOFTWARE
                                                                           : D2D1_RENDER_TARGET_TYPE_DEFAULT),
            D2D1::HwndRenderTargetProperties(canvas, D2D1::SizeU(r.right, r.bottom),
                                             D2D1_PRESENT_OPTIONS_IMMEDIATELY),
            &target);
        if (FAILED(hr))
            return false;
        VMD_TEST_ONLY(startupPhase("render_target_ready");)
        target->SetDpi(dpi, dpi);
        target->SetTransform(D2D1::Matrix3x2F::Translation(0, -top));
        HRESULT brushResult;
#ifdef VMD_TESTING
        if (failDeviceBrush) {
            failDeviceBrush = false;
            brushResult = E_OUTOFMEMORY;
        } else
#endif
            brushResult = target->CreateSolidColorBrush(rgb(0), &brush);
        if (FAILED(brushResult)) {
            target.Reset();
            VMD_TEST_ONLY(if (!metricsFile.empty()) {
                std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
                out << "{\"brush_creation_failure\":true,\"target_released\":true}\n";
            })
            return false;
        }
        return true;
    }
    void prepareHardware() {
        if (!stagedRenderer || rendererStarted || closing || closeRequested)
            return;
        rendererStarted = true;
        auto prepared = std::make_shared<PreparedRenderer>();
        preparedRenderer = prepared;
        RECT bounds{};
        GetClientRect(canvas, &bounds);
        auto size = D2D1::SizeU(std::max(1L, bounds.right), std::max(1L, bounds.bottom));
        HWND destination = canvas;
#ifdef VMD_TESTING
        bool failPreparation = failRendererPreparation;
#endif
        // The factory is private to this worker until the release/acquire handoff.
        // No drawing occurs here; the software frame remains visible throughout preparation.
        rendererWorker = std::thread([prepared, destination, size VMD_TEST_ONLY(, failPreparation)] {
#ifdef VMD_TESTING
#ifdef VMD_TESTING
            auto began = std::chrono::steady_clock::now();
#endif
#endif
            HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            prepared->result =
                D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, prepared->factory.GetAddressOf());
#ifdef VMD_TESTING
            if (failPreparation)
                prepared->result = E_FAIL;
#endif
            if (SUCCEEDED(prepared->result))
                prepared->result = prepared->factory->CreateHwndRenderTarget(
                    D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE),
                    D2D1::HwndRenderTargetProperties(destination, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
                    &prepared->target);
#ifdef VMD_TESTING
            prepared->prepareMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
#endif
            if (SUCCEEDED(apartment))
                CoUninitialize();
            prepared->ready.store(true, std::memory_order_release);
        });
        SetTimer(hwnd, 15, 10, nullptr);
#ifdef VMD_TESTING
        if (!metricsFile.empty()) {
            std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
            out << "{\"renderer_preparing\":true}\n";
        }
#endif
    }
#ifdef VMD_TESTING
    void recordPromotion(bool success, HRESULT result) {
        if (metricsFile.empty())
            return;
        std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
        RECT bounds{};
        GetClientRect(canvas, &bounds);
        float targetDpiX = 0, targetDpiY = 0;
        if (target)
            target->GetDpi(&targetDpiX, &targetDpiY);
        auto pixels = target ? target->GetPixelSize() : D2D1::SizeU(0, 0);
        out << "{\"renderer_promotion\":" << (success ? "true" : "false")
            << ",\"prepare_ms\":" << preparedRenderer->prepareMs
            << ",\"ui_promotion_ms\":" << rendererPromotionMs << ",\"since_start_ms\":"
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
            << ",\"hresult\":" << result << ",\"failure_injection\":\""
            << (failRendererPreparation  ? "preparation"
                : failRendererFirstFrame ? "first_frame"
                                         : "none")
            << "\",\"viewport_matches\":"
            << (targetDpiX == dpi && targetDpiY == dpi && pixels.width == (UINT32)bounds.right &&
                        pixels.height == (UINT32)bounds.bottom
                    ? "true"
                    : "false")
            << "}\n";
    }
#endif
    void promoteHardware() {
        if (closing || !preparedRenderer || !preparedRenderer->ready.load(std::memory_order_acquire))
            return;
        KillTimer(hwnd, 15);
        rendererWorker.join();
        if (closeRequested) {
            preparedRenderer.reset();
            DestroyWindow(hwnd);
            return;
        }
        auto began = std::chrono::steady_clock::now();
        HRESULT result = preparedRenderer->result;
        ComPtr<ID2D1SolidColorBrush> nextBrush;
        if (SUCCEEDED(result)) {
            RECT bounds{};
            GetClientRect(canvas, &bounds);
            result = preparedRenderer->target->Resize(
                D2D1::SizeU(std::max(1L, bounds.right), std::max(1L, bounds.bottom)));
        }
        if (SUCCEEDED(result))
            result = preparedRenderer->target->CreateSolidColorBrush(rgb(0), &nextBrush);
        if (FAILED(result)) {
            VMD_TEST_ONLY(recordPromotion(false, result);)
            preparedRenderer.reset();
            VMD_TEST_ONLY(fallbackAwaitingFrame = true;)
            invalidate();
#ifdef VMD_TESTING
            if (exitOnContent)
                SetTimer(hwnd, 9, 100, nullptr);
#endif
            return;
        }
        preparedRenderer->target->SetDpi(dpi, dpi);
        preparedRenderer->target->SetTransform(D2D1::Matrix3x2F::Translation(0, -top));
        for (auto i = cache.begin(); i != cache.end();) {
            if (i->second.bitmap) {
                cacheCost -= i->second.cost;
                i = cache.erase(i);
            } else
                ++i;
        }
        softwareBrush = std::move(brush);
        softwareTarget = std::move(target);
        softwareFactory = std::move(d2d);
        brush = std::move(nextBrush);
        target = std::move(preparedRenderer->target);
        d2d = std::move(preparedRenderer->factory);
        hardwareActive = true;
        promotionPending = true;
#ifdef VMD_TESTING
        promotionBegan = began;
        rendererPromotionMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
#endif
        invalidate();
    }
    void requestClose() {
        if (closing)
            return;
        if (!leaveEdit([this] { requestClose(); }))
            return;
        // Keep the HWND alive while the worker prepares its target, without blocking the UI thread.
        if (rendererWorker.joinable() && !preparedRenderer->ready.load(std::memory_order_acquire)) {
            closeRequested = true;
#ifdef VMD_TESTING
            if (!metricsFile.empty()) {
                std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
                out << "{\"renderer_close_deferred\":true}\n";
            }
#endif
            return;
        }
        if (rendererWorker.joinable())
            rendererWorker.join();
        DestroyWindow(hwnd);
    }
    void clearLayouts() {
        cache.clear();
        codeWidths.clear();
        cacheCost = 0;
        maxWidth = 0;
    }
    double estimate(const Block &b) {
        float size = (b.kind == Kind::Heading ? (b.level == 1   ? 34.f
                                                 : b.level == 2 ? 27.f
                                                                : 22.f)
                      : b.kind == Kind::Code  ? 14.f
                                              : 16.f) *
                     zoom;
        if (b.kind == Kind::Rule)
            return 24 * zoom;
        if (b.kind == Kind::Image)
            return 190 * zoom;
        double lines = 1;
        auto &s = text();
        if (source || b.kind == Kind::Code) {
            lines = (double)std::count(s.begin() + b.start, s.begin() + b.start + b.length, L'\n');
            if (!b.length || s[b.start + b.length - 1] != L'\n')
                ++lines;
            lines = std::max(1., lines);
        } else
            lines = std::max(
                1., std::ceil(b.length * size * .48 / std::max(80.f, contentWidth() - b.indent * 20)));
        return lines * size * 1.48 + (source ? 0 : 16 * zoom);
    }
    void resetLayout(bool keep = true) {
        codeButtons.clear();
        pressedCode = copiedCode = SIZE_MAX;
        KillTimer(hwnd, 19);
        hoverLink(L"");
        stopScrolling();
        double ratio = heights.total() > 0 ? scroll / heights.total() : 0;
        clearLayouts();
        if (!doc) {
            heights.reset(0);
            scroll = 0;
            horizontal = 0;
            invalidate();
            return;
        }
        auto &bs = blocks();
        heights.reset(bs.size());
        // Build the Fenwick index in linear time; later measured-height updates stay logarithmic.
        for (size_t i = 0; i < bs.size(); ++i) {
            heights.values[i] = estimate(bs[i]);
            heights.tree[i + 1] += heights.values[i];
            size_t parent = (i + 1) + ((i + 1) & (~(i + 1) + 1));
            if (parent < heights.tree.size())
                heights.tree[parent] += heights.tree[i + 1];
        }
        if (keep)
            scroll = ratio * heights.total();
        else
            scroll = 0;
        horizontal = 0;
        bars();
        invalidate();
    }
    void evict(size_t current) {
        if (cacheCost <= 32 * 1024 * 1024 && cache.size() <= 256)
            return;
        std::vector<std::pair<uint64_t, size_t>> order;
        for (auto &[i, l] : cache)
            if (i != current)
                order.emplace_back(l.used, i);
        std::sort(order.begin(), order.end());
        for (auto [_, i] : order) {
            cacheCost -= cache[i].cost;
            cache.erase(i);
            if (cacheCost < 24 * 1024 * 1024 && cache.size() < 192)
                break;
        }
    }
    Layout &plainLayout(size_t index) {
        const auto &block = blocks()[index];
        Layout out;
        out.plain = true;
        out.used = ++tick;
        float font = 16 * zoom;
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font, L"",
                                           &format)))
            throw std::runtime_error("Windows text rendering is unavailable.");
        format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, font * 1.5f, font * 1.16f);
        auto appendRange = [&](uint32_t start, uint32_t length) {
            uint32_t offset = 0;
            do {
                Part part;
                part.start = start + offset;
                part.length = std::min<uint32_t>(8192, length - offset);
                if (offset + part.length < length && part.length > 1 &&
                    text()[part.start + part.length - 1] >= 0xd800 &&
                    text()[part.start + part.length - 1] <= 0xdbff)
                    --part.length;
                part.y = out.height;
                part.width = contentWidth();
                if (FAILED(write->CreateTextLayout(text().data() + part.start, part.length, format.Get(),
                                                   part.width, 10000000, &part.layout)))
                    throw std::runtime_error("Windows text rendering is unavailable.");
                DWRITE_TEXT_METRICS metrics{};
                part.layout->GetMetrics(&metrics);
                part.height = metrics.height;
                out.height += metrics.height;
                out.width = std::max(out.width, metrics.widthIncludingTrailingWhitespace);
                out.cost += (size_t)part.length * 64 + 1024;
                offset += part.length;
                out.parts.push_back(std::move(part));
            } while (offset < length);
        };
        if (block.kind == Kind::Table && !block.table && !block.cells.empty()) {
            for (const auto &cell : block.cells)
                if (cell.length)
                    appendRange(cell.start, cell.length);
        } else {
            appendRange(block.start, block.length);
        }
        out.height += 14 * zoom;
        heights.set(index, out.height);
        maxWidth = std::max(maxWidth, out.width + left());
        cacheCost += out.cost;
        auto [found, inserted] = cache.emplace(index, std::move(out));
        evict(index);
        return found->second;
    }
    Layout &layout(size_t index) {
        try {
            return formattedLayout(index);
        } catch (const std::exception &) {
            auto found = cache.find(index);
            if (found != cache.end())
                return found->second;
            if (source)
                throw;
            return plainLayout(index);
        }
    }
    bool codeContinues(size_t index) const {
        return !source && index + 1 < doc->blocks.size() && doc->blocks[index + 1].kind == Kind::Code &&
               doc->blocks[index + 1].continuation &&
               doc->blocks[index + 1].codeStart == doc->blocks[index].codeStart;
    }
    Layout &formattedLayout(size_t index) {
        auto found = cache.find(index);
        if (found != cache.end()) {
            found->second.used = ++tick;
            return found->second;
        }
        auto out = nativeLayout(blocks()[index], contentWidth(), index);
        heights.set(index, out.height);
        maxWidth = std::max(maxWidth, out.width + left());
        cacheCost += out.cost;
        auto [it, _] = cache.emplace(index, std::move(out));
        evict(index);
        return it->second;
    }
    Layout nativeLayout(const Block &b, float containerWidth, size_t index = SIZE_MAX, unsigned depth = 0) {
        if (depth > 64)
            throw std::runtime_error("HTML layout nesting limit");
        Layout out;
        out.used = ++tick;
        float indent = source ? 0 : (float)(b.indent ? b.indent - 1 : 0) * 22 * zoom + b.quote * 18 * zoom;
        float available = std::max(depth ? 20 * zoom : 80.f, containerWidth - indent),
              padding = (!source && b.kind == Kind::Code) ? 14 * zoom : 0;
        out.left = indent;
        float topPadding = b.continuation ? 0 : padding;
        // A separate compact header keeps copy controls clear of selectable code.
        if (!source && b.kind == Kind::Code && !b.continuation)
            topPadding += 34 * zoom;
        float bottomPadding = (index != SIZE_MAX && codeContinues(index)) ? 0 : padding;
        float font = (b.kind == Kind::Heading ? (b.level == 1   ? 34.f
                                                 : b.level == 2 ? 27.f
                                                 : b.level == 3 ? 22.f
                                                                : 18.f)
                      : b.kind == Kind::Code  ? 14.f
                                              : 16.f) *
                     zoom;
        if (b.kind == Kind::Rule) {
            out.height = 24 * zoom;
            Part rule{};
            rule.start = b.start;
            rule.x = indent;
            rule.y = 8 * zoom;
            rule.width = available;
            out.parts.push_back(std::move(rule));
        } else if (b.image && b.image->inlineImages) {
            float x = 0, y = 0, rowHeight = 0;
            const float gap = 4 * zoom;
            for (auto &child : *b.image->inlineImages) {
                auto nested = nativeLayout(child, available + indent, SIZE_MAX, depth + 1);
                Part part;
                part.start = child.start;
                part.length = child.length;
                part.width = nested.width - nested.left;
                part.height = std::max(0.f, nested.height - 18 * zoom);
                part.bitmap = std::move(nested.bitmap);
                if (!part.bitmap) {
                    Block alt;
                    alt.start = child.start;
                    alt.length = child.length;
                    alt.runs = child.runs;
                    auto fallback = nativeLayout(alt, std::min(available, 160 * zoom), SIZE_MAX, depth + 1);
                    if (!fallback.parts.empty()) {
                        part = std::move(fallback.parts.front());
                        part.width = std::min(available, 160 * zoom);
                    }
                }
                if (x > 0 && x + part.width > available) {
                    x = 0;
                    y += rowHeight + gap;
                    rowHeight = 0;
                }
                part.x = indent + x;
                part.y = y;
                rowHeight = std::max(rowHeight, part.height);
                out.width = std::max(out.width, part.x + part.width);
                x += part.width + gap;
                out.parts.push_back(std::move(part));
                out.imagePending |= nested.imagePending;
                out.cost += nested.cost;
            }
            out.height = y + rowHeight + 14 * zoom;
        } else if (b.kind == Kind::Image) {
            auto image = images->get(b.image ? b.image->source(dark) : L"", doc->path.wstring());
            if (image && image->ready) {
                auto [w, h] = imageDisplaySize(b, image->width, image->height, available, zoom);
                image = images->raster(image, w * dpi / 96.f, h * dpi / 96.f, &out.imagePending);
                D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                if (SUCCEEDED(target->CreateBitmap(D2D1::SizeU(image->width, image->height),
                                                   image->pixels.data(), image->width * 4, props,
                                                   &out.bitmap))) {
                    float shift = b.align == 2 ? (available - w) / 2 : b.align == 3 ? available - w : 0;
                    out.left += shift;
                    out.width = out.left + w;
                    out.height = h + 18 * zoom;
                    out.cost += (size_t)image->width * image->height * 4;
                }
            }
            if (!out.bitmap) {
                out.imagePending = !image;
                out.height = 64 * zoom;
                out.width = indent + available;
            }
        } else {
            auto make = [&](uint32_t start, uint32_t count, float x, float w, int align) {
                Part p;
                p.start = start;
                p.length = count;
                p.x = x;
                p.y = topPadding;
                p.width = w;
                ComPtr<IDWriteTextFormat> format;
                bool mono = source || b.kind == Kind::Code;
                HRESULT hr = write->CreateTextFormat(
                    mono ? L"Consolas" : L"Segoe UI", nullptr,
                    b.kind == Kind::Heading || b.header ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                                        : DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font, L"", &format);
                if (FAILED(hr))
                    throw std::runtime_error("Text format unavailable");
                bool nowrap = source || b.kind == Kind::Code;
                format->SetWordWrapping(nowrap ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
                float lineHeight = informationView && !mono ? 1.65f : 1.5f;
                format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, font * lineHeight, font * 1.16f);
                if (align == 2)
                    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                else if (align == 3)
                    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                hr = write->CreateTextLayout(text().data() + start, count, format.Get(), std::max(20.f, w),
                                             10000000, &p.layout);
                if (FAILED(hr))
                    throw std::runtime_error("Text layout failed");
                if (!source && b.kind == Kind::Code && b.language)
                    p.codeTokens =
                        vm::codeTokens(std::wstring_view(text()).substr(start, count), *b.language);
                std::vector<DWRITE_TEXT_RANGE> keycapRanges, inlineCodeRanges;
                ComPtr<IDWriteTextLayout1> characterSpacing;
                if (!source) {
                    for (auto &r : b.runs) {
                        uint32_t begin = std::max(start, r.start),
                                 end = std::min(start + count, r.start + r.length);
                        if (end <= begin)
                            continue;
                        DWRITE_TEXT_RANGE range{begin - start, end - begin};
                        if (r.style & Bold)
                            p.layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
                        if (r.style & Italic)
                            p.layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
                        if (r.style & Mono)
                            p.layout->SetFontFamilyName(L"Consolas", range);
                        if (r.style & Strike)
                            p.layout->SetStrikethrough(TRUE, range);
                        if ((r.style & Underline) && r.link.empty())
                            p.layout->SetUnderline(TRUE, range);
                        if (r.style & (Subscript | Superscript | Small)) {
                            p.layout->SetFontSize(font * (r.style & Small ? .83f : .75f), range);
                            ComPtr<IDWriteTypography> typography;
                            if ((r.style & (Subscript | Superscript)) &&
                                SUCCEEDED(write->CreateTypography(&typography))) {
                                typography->AddFontFeature({r.style & Superscript
                                                                ? DWRITE_FONT_FEATURE_TAG_SUPERSCRIPT
                                                                : DWRITE_FONT_FEATURE_TAG_SUBSCRIPT,
                                                            1});
                                p.layout->SetTypography(typography.Get(), range);
                            }
                        }
                        if ((r.style & Mono) && !(r.style & Keycap) && b.kind != Kind::Code) {
                            if (!inlineCodeRanges.empty() &&
                                inlineCodeRanges.back().startPosition + inlineCodeRanges.back().length ==
                                    range.startPosition)
                                inlineCodeRanges.back().length += range.length;
                            else
                                inlineCodeRanges.push_back(range);
                        }
                        if (r.style & Keycap) {
                            p.layout->SetFontSize(font * .88f, range);
                            p.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
                            if (!characterSpacing)
                                p.layout.As(&characterSpacing);
                            if (characterSpacing) {
                                characterSpacing->SetCharacterSpacing(
                                    3 * zoom, range.length == 1 ? 3 * zoom : 0, 0, {range.startPosition, 1});
                                if (range.length > 1)
                                    characterSpacing->SetCharacterSpacing(
                                        0, 3 * zoom, 0, {range.startPosition + range.length - 1, 1});
                            }
                            keycapRanges.push_back(range);
                        }
                    }
                }
                // Reserve a little space at each end without changing the document text.
                for (auto range : inlineCodeRanges) {
                    if (!characterSpacing)
                        p.layout.As(&characterSpacing);
                    if (characterSpacing) {
                        characterSpacing->SetCharacterSpacing(3 * zoom, range.length == 1 ? 3 * zoom : 0, 0,
                                                              {range.startPosition, 1});
                        if (range.length > 1)
                            characterSpacing->SetCharacterSpacing(
                                0, 3 * zoom, 0, {range.startPosition + range.length - 1, 1});
                    }
                }
                DWRITE_TEXT_METRICS m{};
                p.layout->GetMetrics(&m);
                for (auto range : inlineCodeRanges) {
                    UINT32 metricsCount = 0;
                    p.layout->HitTestTextRange(range.startPosition, range.length, 0, 0, nullptr, 0,
                                               &metricsCount);
                    std::vector<DWRITE_HIT_TEST_METRICS> metrics(metricsCount);
                    if (metricsCount &&
                        SUCCEEDED(p.layout->HitTestTextRange(range.startPosition, range.length, 0, 0,
                                                             metrics.data(), metricsCount, &metricsCount))) {
                        for (auto &span : metrics) {
                            float h = std::min(span.height, font * 1.3f);
                            float y = span.top + (span.height - h) / 2;
                            p.inlineCode.push_back(D2D1::RectF(span.left, y, span.left + span.width, y + h));
                        }
                    }
                }
                for (auto range : keycapRanges) {
                    UINT32 metricsCount = 0;
                    p.layout->HitTestTextRange(range.startPosition, range.length, 0, 0, nullptr, 0,
                                               &metricsCount);
                    std::vector<DWRITE_HIT_TEST_METRICS> metrics(metricsCount);
                    if (metricsCount &&
                        SUCCEEDED(p.layout->HitTestTextRange(range.startPosition, range.length, 0, 0,
                                                             metrics.data(), metricsCount, &metricsCount))) {
                        for (auto &span : metrics) {
                            float capHeight = std::min(span.height, font * 1.3f);
                            float capTop = span.top + (span.height - capHeight) / 2;
                            p.keycaps.push_back(
                                D2D1::RectF(span.left, capTop, span.left + span.width, capTop + capHeight));
                        }
                    }
                }
                out.cost += (p.keycaps.size() + p.inlineCode.size()) * sizeof(D2D1_RECT_F) +
                            p.codeTokens.size() * sizeof(CodeToken);
                p.height = m.height;
                out.width = std::max(out.width, x + m.widthIncludingTrailingWhitespace);
                out.height = std::max(out.height, p.y + m.height + bottomPadding);
                out.cost += (size_t)count * 64 + 1024;
                out.parts.push_back(std::move(p));
            };
            if (b.kind == Kind::Table && !b.cells.empty()) {
                bool rich = std::any_of(b.cells.begin(), b.cells.end(),
                                        [](const Cell &cell) { return !!cell.content; });
                unsigned columns = b.table ? b.table->columns : (unsigned)b.cells.size();
                float rowWidth;
                std::vector<float> columnOffsets;
                if (b.table) {
                    columnOffsets = tableColumnOffsets(*b.table, available, zoom);
                    rowWidth = columnOffsets.back();
                    for (auto &x : columnOffsets)
                        x += indent;
                } else {
                    rowWidth = std::max(available, columns * (rich ? 40 : 140) * zoom);
                    std::vector<float> widths(columns);
                    for (size_t i = 0; i < b.cells.size(); ++i) {
                        auto &cell = b.cells[i];
                        if (cell.width)
                            widths[i] = cell.percent ? rowWidth * std::min(cell.width, 100u) / 100.f
                                                     : cell.width * zoom;
                    }
                    float specified = 0;
                    size_t automatic = 0;
                    for (auto columnWidth : widths) {
                        if (columnWidth)
                            specified += columnWidth;
                        else
                            ++automatic;
                    }
                    float remainder = automatic ? std::max(0.f, rowWidth - specified) / automatic : 0;
                    float total = 0;
                    for (size_t i = 0; i < widths.size(); ++i) {
                        if (!widths[i])
                            widths[i] = remainder;
                        widths[i] = std::max(b.table ? .001f : 40 * zoom, widths[i]);
                        total += widths[i];
                    }
                    columnOffsets.assign(columns + 1, indent);
                    double x = indent;
                    for (unsigned col = 0; col < columns; ++col) {
                        x += widths[col] * rowWidth / total;
                        columnOffsets[col + 1] = (float)x;
                    }
                }
                // Retain relative hints while fitting the available row width.
                struct CellPlacement {
                    size_t parts, borders, headers;
                    float height;
                };
                std::vector<CellPlacement> placements;
                std::vector<float> natural;
                out.height = 0;
                for (size_t i = 0; i < b.cells.size(); ++i) {
                    auto &c = b.cells[i];
                    unsigned column = b.table ? c.column : (unsigned)i;
                    unsigned span = b.table ? c.colSpan : 1;
                    float x = columnOffsets[column], w = columnOffsets[column + span] - x;
                    out.cellEdges.push_back(x);
                    placements.push_back(
                        {out.parts.size(), out.tableBorders.size(), out.tableHeaders.size(), 0});
                    float cy = 8 * zoom;
                    if (c.content) {
                        for (auto &child : *c.content) {
                            auto nested =
                                nativeLayout(child, std::max(20 * zoom, w - 20 * zoom), SIZE_MAX, depth + 1);
                            float cx = x + 10 * zoom;
                            if (child.kind == Kind::Image && !nested.bitmap) {
                                Part fallback;
                                fallback.start = child.start;
                                fallback.length = child.length;
                                fallback.width = w - 20 * zoom;
                                fallback.height = 48 * zoom;
                                ComPtr<IDWriteTextFormat> format;
                                if (SUCCEEDED(write->CreateTextFormat(
                                        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14 * zoom, L"",
                                        &format)))
                                    write->CreateTextLayout(text().data() + child.start, child.length,
                                                            format.Get(), fallback.width, fallback.height,
                                                            &fallback.layout);
                                if (fallback.layout)
                                    nested.parts.push_back(std::move(fallback));
                            }
                            if (nested.bitmap) {
                                Part imagePart;
                                imagePart.start = child.start;
                                imagePart.length = child.length;
                                imagePart.x = cx + nested.left;
                                imagePart.y = cy;
                                imagePart.width = nested.width - nested.left;
                                imagePart.height = nested.height - 18 * zoom;
                                imagePart.bitmap = std::move(nested.bitmap);
                                out.parts.push_back(std::move(imagePart));
                            }
                            for (auto &p : nested.parts) {
                                p.x += cx;
                                p.y += cy;
                                out.parts.push_back(std::move(p));
                            }
                            if (child.kind == Kind::Table && !child.table)
                                for (size_t edge = 0; edge < nested.cellEdges.size(); ++edge) {
                                    float right = edge + 1 < nested.cellEdges.size()
                                                      ? nested.cellEdges[edge + 1]
                                                      : nested.width;
                                    nested.tableBorders.push_back(D2D1::RectF(
                                        nested.cellEdges[edge], 0, right, nested.height - nested.tableGap));
                                }
                            for (auto rect : nested.tableBorders) {
                                rect.left += cx;
                                rect.right += cx;
                                rect.top += cy;
                                rect.bottom += cy;
                                out.tableBorders.push_back(rect);
                            }
                            for (auto rect : nested.tableHeaders) {
                                rect.left += cx;
                                rect.right += cx;
                                rect.top += cy;
                                rect.bottom += cy;
                                out.tableHeaders.push_back(rect);
                            }
                            out.imagePending |= nested.imagePending;
                            out.cost += nested.cost;
                            cy += nested.height;
                        }
                    } else {
                        make(c.start, c.length, x + 10 * zoom, w - 20 * zoom, c.align);
                        out.parts.back().y += 8 * zoom;
                        cy += out.parts.back().height;
                    }
                    placements.back().height = cy + 8 * zoom;
                    out.height = std::max(out.height, placements.back().height);
                    natural.push_back(placements.back().height);
                }
                auto rowOffsets = tableRowOffsets(b, natural);
                out.height = rowOffsets.back();
                for (size_t i = 0; i < placements.size(); ++i) {
                    auto &cell = b.cells[i];
                    unsigned row = b.table ? cell.row : 0;
                    unsigned span = b.table ? cell.rows : 1;
                    float extra = rowOffsets[row + span] - rowOffsets[row] - placements[i].height;
                    float dy = b.cells[i].verticalAlign == 1   ? 0
                               : b.cells[i].verticalAlign == 3 ? extra
                                                               : extra / 2;
                    dy += rowOffsets[row];
                    auto partEnd = i + 1 < placements.size() ? placements[i + 1].parts : out.parts.size();
                    auto borderEnd =
                        i + 1 < placements.size() ? placements[i + 1].borders : out.tableBorders.size();
                    for (size_t j = placements[i].parts; j < partEnd; ++j)
                        out.parts[j].y += dy;
                    for (size_t j = placements[i].borders; j < borderEnd; ++j) {
                        out.tableBorders[j].top += dy;
                        out.tableBorders[j].bottom += dy;
                    }
                    auto headerEnd =
                        i + 1 < placements.size() ? placements[i + 1].headers : out.tableHeaders.size();
                    for (size_t j = placements[i].headers; j < headerEnd; ++j) {
                        out.tableHeaders[j].top += dy;
                        out.tableHeaders[j].bottom += dy;
                    }
                }
                if (b.table) {
                    for (auto &cell : b.cells) {
                        auto rect = D2D1::RectF(columnOffsets[cell.column], rowOffsets[cell.row],
                                                columnOffsets[cell.column + cell.colSpan],
                                                rowOffsets[cell.row + cell.rows]);
                        out.tableBorders.push_back(rect);
                        if (cell.header)
                            out.tableHeaders.push_back(rect);
                    }
                    out.tableBorders.push_back(D2D1::RectF(indent, 0, indent + rowWidth, out.height));
                }
                out.width = indent + rowWidth;
            } else
                make(b.start, b.length, indent + padding, available - 2 * padding, b.align);
            if (!source && b.kind == Kind::Heading && b.level <= 2 && !out.parts.empty())
                out.parts.back().headingDividerWidth = available;
            if (!source && b.kind == Kind::Code) {
                out.width = std::max(containerWidth, out.width + padding);
                if (b.continuation || (index != SIZE_MAX && codeContinues(index))) {
                    auto &groupWidth = codeWidths[b.codeStart];
                    groupWidth = std::max(groupWidth, out.width);
                }
            }
            if (!source) {
                if (b.kind == Kind::Table) {
                    bool moreRows = !b.table && index != SIZE_MAX && index + 1 < blocks().size() &&
                                    blocks()[index + 1].kind == Kind::Table && !blocks()[index + 1].header;
                    out.tableGap = moreRows || (b.table && b.table->continues) ? 0 : 14 * zoom;
                    out.height += out.tableGap;
                } else if (b.kind == Kind::Code)
                    out.height +=
                        (index != SIZE_MAX && codeContinues(index)) ? 0 : (informationView ? 20 : 14) * zoom;
                else
                    out.height += b.continuation ? 0 : (informationView ? 20 : 14) * zoom;
            }
        }
        if (b.table && b.table->captions) {
            float captionHeight = 0;
            std::vector<Part> captions;
            for (auto &caption : *b.table->captions) {
                auto rendered = nativeLayout(caption, containerWidth, SIZE_MAX, depth + 1);
                for (auto &part : rendered.parts) {
                    part.y += captionHeight;
                    captions.push_back(std::move(part));
                }
                if (rendered.bitmap) {
                    Part part;
                    part.start = caption.start;
                    part.length = caption.length;
                    part.bitmap = std::move(rendered.bitmap);
                    part.x = rendered.left;
                    part.y = captionHeight;
                    part.width = rendered.width - rendered.left;
                    part.height = rendered.height - 18 * zoom;
                    captions.push_back(std::move(part));
                }
                out.imagePending |= rendered.imagePending;
                out.cost += rendered.cost;
                captionHeight += rendered.height;
            }
            for (auto &part : out.parts)
                part.y += captionHeight;
            for (auto &rect : out.tableBorders) {
                rect.top += captionHeight;
                rect.bottom += captionHeight;
            }
            for (auto &rect : out.tableHeaders) {
                rect.top += captionHeight;
                rect.bottom += captionHeight;
            }
            for (auto &part : captions)
                out.parts.push_back(std::move(part));
            out.height += captionHeight;
        }
        out.cost += out.tableBorders.size() * sizeof(D2D1_RECT_F) +
                    out.tableHeaders.size() * sizeof(D2D1_RECT_F) + out.cellEdges.size() * sizeof(float);
        out.height = std::max(out.height, 1.f);
        return out;
    }
    void bars() {
        double page = std::max(1.f, height - top - bottom);
        scroll = std::clamp(scroll, 0., std::max(0., heights.total() - page));
        // Layout arithmetic can put a fitting table a fraction of a pixel past the viewport.
        if (maxWidth > width && (maxWidth - width) * scale() < .5f)
            maxWidth = width;
        horizontal = std::clamp(horizontal, 0.f, std::max(0.f, maxWidth - width));
    }
    void stopScrolling() {
        scrollAnimating = false;
        KillTimer(hwnd, 4);
        scrollTarget = scroll;
    }
    bool editorSourceDrivesPreview = true;
    double previewSourcePosition() const {
        if (!doc || doc->sourceStarts.size() != heights.values.size() || heights.values.empty())
            return 0;
        size_t i = heights.find(scroll);
        double from = doc->sourceStarts[i];
        auto &starts = doc->sourceStarts;
        auto first = std::lower_bound(starts.begin(), starts.end(), starts[i]);
        auto next = std::upper_bound(first, starts.end(), starts[i]);
        i = (size_t)(first - starts.begin());
        double to = next != starts.end() ? (double)*next : (double)doc->sourceMapLength;
        double extent = heights.before((size_t)(next - starts.begin())) - heights.before(i);
        return from + (to - from) * std::clamp((scroll - heights.before(i)) / std::max(1., extent), 0., 1.);
    }
    void syncSourceToPreview() {
        editorSourceDrivesPreview = false;
        if (editor && editor->isLive())
            editor->previewScroll(previewSourcePosition());
    }
    void syncPreviewToSource() {
        if (!editor || !editor->canSynchronize() || !doc ||
            doc->sourceStarts.size() != heights.values.size() || heights.values.empty())
            return;
        double position = editor->sourcePosition();
        auto &starts = doc->sourceStarts;
        auto next = std::upper_bound(starts.begin(), starts.end(), position);
        size_t i = next == starts.begin() ? 0 : (size_t)(next - starts.begin() - 1);
        i = (size_t)(std::lower_bound(starts.begin(), starts.end(), starts[i]) - starts.begin());
        next = std::upper_bound(starts.begin() + i, starts.end(), starts[i]);
        // Measure only the destination block. Earlier images can remain lazy:
        // both directions use the same current height index.
        layout(i);
        double end = next != starts.end() ? (double)*next : (double)doc->sourceMapLength;
        double within = std::clamp((position - starts[i]) / std::max(1., end - starts[i]), 0., 1.);
        double extent = heights.before((size_t)(next - starts.begin())) - heights.before(i);
        scroll = heights.before(i) + within * extent;
        bars();
    }
    void scrollTo(double destination) {
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        scrollTarget = std::clamp(destination, 0., std::max(0., heights.total() - (height - top - bottom)));
#ifdef VMD_TESTING
        if (scrollBenchmark && benchmarkAnimations)
            animations = TRUE;
#endif
        if (!animations) {
            scroll = scrollTarget;
            stopScrolling();
            bars();
            if (editor && editor->isLive())
                syncSourceToPreview();
            invalidate();
            return;
        }
        if (!scrollAnimating) {
            scroll += (scrollTarget - scroll) * .18;
            invalidate();
            scrollAnimating = true;
            scrollTick = std::chrono::steady_clock::now();
            SetTimer(hwnd, 4, 10, nullptr);
        }
    }
    void animateScroll() {
        if (!scrollAnimating)
            return;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - scrollTick).count();
        scrollTick = now;
        scrollTarget = std::clamp(scrollTarget, 0., std::max(0., heights.total() - (height - top - bottom)));
        scroll += (scrollTarget - scroll) * (1 - std::exp(-std::min(dt, .1) / .032));
        if (editor && editor->isLive())
            syncSourceToPreview();
        if (std::abs(scrollTarget - scroll) < .2) {
            scroll = scrollTarget;
            stopScrolling();
        }
        bars();
        invalidate();
    }
    D2D1_RECT_F verticalThumb() {
        float page = std::max(1.f, height - top - bottom), track = page;
        float thumb =
            std::clamp((float)(page / std::max<double>(page, heights.total()) * track), 28.f, track);
        float pos = top + (float)(scroll / std::max(1., heights.total() - page)) * (track - thumb);
        return D2D1::RectF(width + 3, pos, width + 9, pos + thumb);
    }
    D2D1_RECT_F horizontalThumb() {
        float track = std::max(1.f, width - 32),
              thumb = std::clamp(width / std::max(width, maxWidth) * track, 28.f, track);
        float pos = 16 + horizontal / std::max(1.f, maxWidth - width) * (track - thumb);
        return D2D1::RectF(pos, height + 3, pos + thumb, height + 9);
    }
    int scrollbarAt(float x, float y) const {
        if (!doc)
            return 0;
        if (x >= width && x <= width + 12 && y >= top && y <= height - bottom &&
            heights.total() > height - top - bottom)
            return 1;
        if (y >= height && y <= height + 12 && x >= 0 && x <= width && maxWidth > width)
            return 2;
        return 0;
    }
    void hoverLink(std::wstring destination) {
        if (hoveredLink == destination && hoverScroll == scroll && hoverHorizontal == horizontal)
            return;
        hoveredLink = std::move(destination);
        hoverScroll = scroll;
        hoverHorizontal = horizontal;
        invalidate();
    }
    std::wstring linkAt(float x, float y) {
        if (source || !doc || blocks().empty() || x < 0 || x >= width || y < top || y >= height - bottom ||
            scrollDrag || dragging)
            return {};
        size_t i = heights.find(scroll + y - top);
        auto &b = blocks()[i];
        if (std::none_of(b.runs.begin(), b.runs.end(), [](const auto &r) { return !r.link.empty(); }))
            return {};
        auto &l = layout(i);
        for (auto &p : l.parts) {
            BOOL trailing = FALSE, inside = FALSE;
            DWRITE_HIT_TEST_METRICS m{};
            if (!p.layout) {
                if (!p.bitmap)
                    continue;
                float px = x - documentLeft() - p.x;
                float py = y - top - (float)(heights.before(i) - scroll) - p.y;
                if (px >= 0 && px <= p.width && py >= 0 && py <= p.height)
                    for (auto &run : b.runs)
                        if (!run.link.empty() && run.start <= p.start &&
                            run.start + run.length >= p.start + p.length)
                            return run.link;
                continue;
            }
            p.layout->HitTestPoint(x - documentLeft() - p.x,
                                   y - top - (float)(heights.before(i) - scroll) - p.y, &trailing, &inside,
                                   &m);
            if (!inside)
                continue;
            auto position = p.start + m.textPosition;
            for (auto &r : b.runs)
                if (position >= r.start && position < r.start + r.length && !r.link.empty())
                    return r.link;
        }
        return {};
    }
    size_t codeButtonAt(float x, float y) const {
        if (source || !doc || codeButtonScroll != scroll || codeButtonHorizontal != horizontal || x < 0 ||
            x >= width || y < top || y >= height - bottom)
            return SIZE_MAX;
        for (auto &button : codeButtons) {
            auto r = button.bounds;
            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom)
                return button.block;
        }
        return SIZE_MAX;
    }
    std::wstring_view codeText(size_t index) const {
        auto &first = doc->blocks[index];
        size_t end = index;
        while (end + 1 < doc->blocks.size() && doc->blocks[end + 1].kind == Kind::Code &&
               doc->blocks[end + 1].continuation && doc->blocks[end + 1].codeStart == first.codeStart)
            ++end;
        auto &last = doc->blocks[end];
        return std::wstring_view(doc->text).substr(first.start, last.start + last.length - first.start);
    }
    HCURSOR documentCursor(float x, float y) {
        bool textArea = doc && x >= 0 && x < width && y >= top && y < height - bottom;
        if (scrollDrag || scrollbarAt(x, y) || !textArea)
            return LoadCursorW(nullptr, IDC_ARROW);
        return LoadCursorW(nullptr,
                           codeButtonAt(x, y) != SIZE_MAX || !linkAt(x, y).empty() ? IDC_HAND : IDC_IBEAM);
    }
    void drawScrollbars(const Palette &colors) {
        if (heights.total() > height - top - bottom) {
            auto r = verticalThumb();
            fill(r, scrollDrag == 1 ? colors.accent : colors.muted, 3);
        }
        if (maxWidth > width) {
            auto r = horizontalThumb();
            fill(r, scrollDrag == 2 ? colors.accent : colors.muted, 3);
        }
    }
    void drawSelection(const Part &p, float x, float y, uint32_t from, uint32_t to, D2D1_COLOR_F c) {
        auto begin = std::max(from, p.start), end = std::min(to, p.start + p.length);
        if (end <= begin)
            return;
        UINT32 n = 0;
        if (!p.layout)
            return;
        p.layout->HitTestTextRange(begin - p.start, end - begin, x, y, nullptr, 0, &n);
        if (!n)
            return;
        std::vector<DWRITE_HIT_TEST_METRICS> hits(n);
        if (SUCCEEDED(p.layout->HitTestTextRange(begin - p.start, end - begin, x, y, hits.data(), n, &n)))
            for (auto &h : hits)
                fill(D2D1::RectF(h.left, h.top, h.left + h.width, h.top + h.height), c, 2);
    }
    D2D1_RECT_F blockBounds(size_t index, const Layout &l, float y) {
        auto kind = blocks()[index].kind;
        float gap = kind == Kind::Table ? l.tableGap : (kind == Kind::Image ? 18 : 14) * zoom;
        float right = l.width;
        if (kind == Kind::Code) {
            gap = codeContinues(index) ? 0 : (informationView ? 20 : 14) * zoom;
            auto found = codeWidths.find(blocks()[index].codeStart);
            if (found != codeWidths.end())
                right = std::max(right, found->second);
        }
        return D2D1::RectF(documentLeft() + l.left, y, documentLeft() + right, y + l.height - gap);
    }
    D2D1_RECT_F imageLabelBounds(const D2D1_RECT_F &bounds) {
        float inset = std::min(14 * zoom, (bounds.right - bounds.left) / 4);
        return D2D1::RectF(bounds.left + inset, bounds.top + 12 * zoom, bounds.right - inset,
                           bounds.top + 46 * zoom);
    }
    void paint() {
#ifdef VMD_TESTING
        auto paintStarted = std::chrono::steady_clock::now();
#endif
        PAINTSTRUCT ps;
        BeginPaint(canvas, &ps);
        if (!device()) {
            EndPaint(canvas, &ps);
            return;
        }
        if (zoomLayoutPending) {
            zoomLayoutPending = false;
            // Publish the new zoom with measured look-ahead extents, so the next timer
            // cannot immediately resize the scrollbar from the estimated heights.
            measureAhead(false);
        }
        auto colors = palette(dark);
        ComPtr<ID2D1SolidColorBrush> linkBrush;
        target->BeginDraw();
        target->Clear(colors.bg);
        codeButtons.clear();
        codeButtonScroll = scroll;
        codeButtonHorizontal = horizontal;
        std::array<ComPtr<ID2D1SolidColorBrush>, 5> syntaxBrushes;
        const unsigned darkSyntax[] = {0xc5a5ff, 0xa8d8a0, 0x93a18c, 0xf0be88, 0x8cd5ef};
        const unsigned lightSyntax[] = {0x7436a8, 0x256330, 0x5c6b52, 0x955014, 0x176480};
        bool paintedDocument = false;
        if (doc && !blocks().empty()) {
            target->PushAxisAlignedClip(D2D1::RectF(0, top, width, height - bottom),
                                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            try {
                size_t i = heights.find(scroll);
                double y = top + heights.before(i) - scroll;
                // Measure only this viewport before drawing, so visible pieces of the same
                // code block use one right edge even when a later piece is wider.
                double measuredY = y;
                for (size_t n = i; n < blocks().size() && measuredY < height - bottom; ++n)
                    measuredY += layout(n).height;
                // Layout only visible content. Timer fills a small amount of look-ahead work.
                for (; i < blocks().size() && y < height - bottom; ++i) {
                    auto &b = blocks()[i];
                    auto &l = layout(i);
                    float x = documentLeft();
                    if (y + l.height >= top) {
                        paintedDocument = true;
                        if (!source && !l.plain && b.kind == Kind::Code)
                            fill(blockBounds(i, l, (float)y), colors.code,
                                 b.continuation || codeContinues(i) ? 0.f : 8.f);
                        if (b.quote && !source && !l.plain)
                            line(x + b.quote * 18 * zoom - 10 * zoom, (float)y,
                                 x + b.quote * 18 * zoom - 10 * zoom, (float)y + l.height - 12 * zoom,
                                 colors.accent, 3);
                        if (!l.plain && b.kind == Kind::Table && !b.table) {
                            auto bounds = blockBounds(i, l, (float)y);
                            float rowBottom = (float)y + l.height - l.tableGap;
                            if (b.header)
                                fill(bounds, colors.code);
                            if (b.header || i == 0 || blocks()[i - 1].kind != Kind::Table)
                                line(bounds.left, (float)y, bounds.right, (float)y, colors.border);
                            line(bounds.left, rowBottom, bounds.right, rowBottom, colors.border);
                            for (auto edge : l.cellEdges)
                                line(x + edge, (float)y, x + edge, rowBottom, colors.border);
                            line(x + l.width, (float)y, x + l.width, rowBottom, colors.border);
                        }
                        if (!l.plain && b.kind == Kind::Image) {
                            auto bounds = blockBounds(i, l, (float)y);
                            if (l.bitmap)
                                target->DrawBitmap(l.bitmap.Get(), bounds);
                            else {
                                bounds.bottom = bounds.top + 48 * zoom;
                                fill(bounds, colors.code, 6);
                                target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                                label(b.length ? std::wstring_view(text()).substr(b.start, b.length)
                                               : L"Image",
                                      imageLabelBounds(bounds), colors.muted, 14 * zoom);
                                target->PopAxisAlignedClip();
                            }
                        }
                        for (auto rect : l.tableHeaders) {
                            rect.left += x;
                            rect.right += x;
                            rect.top += (float)y;
                            rect.bottom += (float)y;
                            fill(rect, colors.code);
                        }
                        for (auto rect : l.tableBorders) {
                            rect.left += x;
                            rect.right += x;
                            rect.top += (float)y;
                            rect.bottom += (float)y;
                            colour(colors.border);
                            target->DrawRectangle(rect, brush.Get());
                        }
                        for (auto &p : l.parts) {
                            if (p.bitmap) {
                                target->DrawBitmap(p.bitmap.Get(),
                                                   D2D1::RectF(x + p.x, (float)y + p.y, x + p.x + p.width,
                                                               (float)y + p.y + p.height));
                                continue;
                            }
                            if (!p.layout) {
                                line(x + p.x, (float)y + p.y, x + p.x + p.width, (float)y + p.y,
                                     colors.border);
                                continue;
                            }
                            float px = x + p.x, py = (float)y + p.y;
                            for (auto rect : p.inlineCode) {
                                rect.left += px;
                                rect.right += px;
                                rect.top += py;
                                rect.bottom += py;
                                fill(rect, colors.code, 4 * zoom);
                            }
                            for (auto cap : p.keycaps) {
                                cap.left += px;
                                cap.right += px;
                                cap.top += py;
                                cap.bottom += py;
                                fill(cap, colors.panel, 3 * zoom);
                                colour(colors.border);
                                target->DrawRoundedRectangle(D2D1::RoundedRect(cap, 3 * zoom, 3 * zoom),
                                                             brush.Get(), zoom);
                            }
                            if (searchMatch)
                                drawSelection(p, px, py, anchor, caret, rgb(0xe7b54b, .35f));
                            if (anchor != caret)
                                drawSelection(p, px, py, std::min(anchor, caret), std::max(anchor, caret),
                                              colors.selection);
                            colour(colors.fg);
                            if (!source && !l.plain) {
                                for (auto token : p.codeTokens) {
                                    auto slot = (size_t)token.color;
                                    if (!syntaxBrushes[slot])
                                        target->CreateSolidColorBrush(
                                            rgb(dark ? darkSyntax[slot] : lightSyntax[slot]),
                                            &syntaxBrushes[slot]);
                                    p.layout->SetDrawingEffect(syntaxBrushes[slot].Get(),
                                                               {token.start, token.length});
                                }
                                for (auto &run : b.runs) {
                                    auto begin = std::max(p.start, run.start);
                                    auto end = std::min(p.start + p.length, run.start + run.length);
                                    if (run.link.empty() || end <= begin)
                                        continue;
                                    if (!linkBrush)
                                        target->CreateSolidColorBrush(colors.accent, &linkBrush);
                                    p.layout->SetDrawingEffect(linkBrush.Get(),
                                                               {begin - p.start, end - begin});
                                }
                            }
                            target->DrawTextLayout({px, py}, p.layout.Get(), brush.Get(),
                                                   D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                            p.layout->SetDrawingEffect(nullptr, {0, p.length});
                            if (p.headingDividerWidth > 0) {
                                float dividerY = py + p.height + 6 * zoom;
                                line(px, dividerY, px + p.headingDividerWidth, dividerY, colors.border);
                            }
                        }
                    }
                    if (!source && !l.plain && b.kind == Kind::Code && !b.continuation &&
                        y + l.height >= top) {
                        auto bounds = blockBounds(i, l, (float)y);
                        float right = std::min(bounds.right, width - 8) - 8 * zoom;
                        auto r =
                            D2D1::RectF(right - 30 * zoom, (float)y + 6 * zoom, right, (float)y + 36 * zoom);
                        codeButtons.push_back({i, r});
                        bool hovered = codeButtonAt(codePointerX, codePointerY) == i;
                        fill(r, hovered ? colors.border : colors.panel, 5 * zoom);
                        colour(colors.muted);
                        auto back = D2D1::RoundedRect(D2D1::RectF(r.left + 12 * zoom, r.top + 7 * zoom,
                                                                  r.left + 23 * zoom, r.top + 19 * zoom),
                                                      1, 1);
                        target->DrawRoundedRectangle(back, brush.Get(), 1.4f * zoom);
                        auto front = D2D1::RoundedRect(D2D1::RectF(r.left + 7 * zoom, r.top + 12 * zoom,
                                                                   r.left + 18 * zoom, r.top + 24 * zoom),
                                                       1, 1);
                        fill(front.rect, colors.panel, zoom);
                        colour(colors.muted);
                        target->DrawRoundedRectangle(front, brush.Get(), 1.4f * zoom);
                        bool copied = copiedCode == i && GetTickCount64() < copiedCodeUntil;
                        if (hovered || copied) {
                            auto tip = D2D1::RectF(std::max(0.f, r.left - 183 * zoom), r.top,
                                                   r.left - 5 * zoom, r.bottom);
                            fill(tip, colors.panel, 5 * zoom);
                            label(copied ? L"Copied!" : L"Copy code to clipboard",
                                  D2D1::RectF(tip.left + 8 * zoom, tip.top + 7 * zoom, tip.right, tip.bottom),
                                  colors.fg, 12 * zoom);
                        } else if (b.language) {
                            label(*b.language,
                                  D2D1::RectF(bounds.left + 14 * zoom, r.top + 7 * zoom, r.left - 6 * zoom,
                                              r.bottom),
                                  colors.muted, 12 * zoom);
                        }
                    }
                    y += l.height;
                }
            } catch (const std::exception &) {
                PostMessageW(hwnd, WM_COMMAND, 110, 0);
            }
            target->PopAxisAlignedClip();
            bars();
        } else if (!loading) {
            drawEmptyState(target.Get(), D2D1::RectF(0, top, width, height - bottom), dark);
        }
        if (!notice.empty()) {
            fill(D2D1::RectF(20, height - 43, width - 20, height - 8), colors.panel, 6);
            label(notice, D2D1::RectF(30, height - 35, width - 55, height - 10), colors.muted, 12);
            label(L"×", D2D1::RectF(width - 43, height - 37, width - 23, height - 10), colors.muted, 18);
        }
        if (!hoveredLink.empty() && hoverScroll == scroll && hoverHorizontal == horizontal && !source &&
            notice.empty()) {
            float right = std::min(width - 8, 20 + (float)hoveredLink.size() * 7);
            fill(D2D1::RectF(0, height - bottom - 30, right, height - bottom), colors.panel, 3);
            ComPtr<IDWriteTextFormat> format;
            write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, 12, L"", &format);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            write->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
            format->SetTrimming(&trimming, ellipsis.Get());
            colour(colors.fg);
            target->DrawTextW(hoveredLink.data(), (UINT32)hoveredLink.size(), format.Get(),
                              D2D1::RectF(8, height - bottom - 25, right - 8, height - bottom - 3),
                              brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        drawScrollbars(colors);
        HRESULT hr = target->EndDraw();
        // Exercise the recovery from an EndDraw device-loss result without requiring a real GPU reset.
#ifdef VMD_TESTING
        if (promotionPending && failRendererFirstFrame)
            hr = D2DERR_RECREATE_TARGET;
#endif
        bool retryPaint = hr == D2DERR_RECREATE_TARGET;
#ifdef VMD_TESTING
        if (editorPreviewAwaitingPaint && editor && (editor->isPreview() || editor->isLive()) &&
            paintedDocument && SUCCEEDED(hr)) {
            editorPreviewAwaitingPaint = false;
            if (!metricsFile.empty()) {
                std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
                out << "{\"edit_preview_painted\":true,\"source\":" << source
                    << ",\"first_kind\":" << (int)doc->blocks.front().kind
                    << ",\"source_units\":" << doc->source.size() << ",\"live\":" << editor->isLive()
                    << ",\"source_focus\":" << editor->sourceFocused() << "}\n";
            }
        }
#endif
        if (hr == D2DERR_RECREATE_TARGET) {
            target.Reset();
            brush.Reset();
            clearLayouts();
        }
        if (promotionPending) {
#ifdef VMD_TESTING
            rendererPromotionMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - promotionBegan)
                    .count();
#endif
            VMD_TEST_ONLY(recordPromotion(SUCCEEDED(hr), hr);)
            if (FAILED(hr)) {
                target = std::move(softwareTarget);
                brush = std::move(softwareBrush);
                d2d = std::move(softwareFactory);
                hardwareActive = false;
                RECT bounds{};
                GetClientRect(canvas, &bounds);
                if (FAILED(target->Resize(
                        D2D1::SizeU(std::max(1L, bounds.right), std::max(1L, bounds.bottom))))) {
                    brush.Reset();
                    target.Reset();
                } else {
                    target->SetDpi(dpi, dpi);
                    target->SetTransform(D2D1::Matrix3x2F::Translation(0, -top));
                }
                clearLayouts();
                retryPaint = true;
                VMD_TEST_ONLY(fallbackAwaitingFrame = true;)
            } else {
                softwareBrush.Reset();
                softwareTarget.Reset();
                softwareFactory.Reset();
            }
            promotionPending = false;
            preparedRenderer.reset();
#ifdef VMD_TESTING
            if (exitOnContent)
                SetTimer(hwnd, 9, 100, nullptr);
#endif
        }
        if (paintedDocument && SUCCEEDED(hr) && !firstPaint) {
            firstPaint = true;
            VMD_TEST_ONLY(recordMetric();)
            VMD_TEST_ONLY(if (!restartFile.empty()) reportRestart(".after");)
#ifdef VMD_TESTING
            if (exitOnContent && !stagedRenderer)
                SetTimer(hwnd, 9, 100, nullptr);
#endif
        }
#ifdef VMD_TESTING
        if (fallbackAwaitingFrame && paintedDocument && SUCCEEDED(hr) && !hardwareActive) {
            fallbackAwaitingFrame = false;
#ifdef VMD_TESTING
            if (!metricsFile.empty()) {
                std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
                out << "{\"software_fallback_frame\":true,\"blocks\":" << doc->blocks.size() << "}\n";
            }
#endif
        }
#endif
        EndPaint(canvas, &ps);
#ifdef VMD_TESTING
        if (!metricsFile.empty() && searchMatch && paintedFindGeneration != findGeneration && SUCCEEDED(hr)) {
            paintedFindGeneration = findGeneration;
            std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
            out << "{\"search_match_paint_ms\":"
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - findStarted)
                       .count()
                << ",\"count_started\":" << (countStarted ? "true" : "false")
                << ",\"search_anchor\":" << anchor << "}\n";
        }
#endif
        if (SUCCEEDED(hr) && paintedDocument && countPending) {
            countPending = false;
            startCount(countStart, countBackwards);
        }
        // No shell work, worker, or notice controls exist on the path to first content.
        if (SUCCEEDED(hr) && !loading && (firstPaint || !doc || blocks().empty())) {
            if (prefs.automaticUpdates && !updateTimerStarted && !sessionEnding) {
                updateTimerStarted = true;
                SetTimer(hwnd, 18, 5000, nullptr);
            }
            associationReadyGeneration = generation;
            if (associationPhase == AssociationPhase::WaitingForPaint) {
                associationPhase = AssociationPhase::Posted;
                PostMessageW(hwnd, ScheduleAssociation, (WPARAM)generation, 0);
            }
        }
        if (retryPaint)
            invalidate();
        if (SUCCEEDED(hr) && (firstPaint || (!doc && !loading)))
            prepareHardware();
#ifdef VMD_TESTING
        if (paintedDocument && !verifyFile.empty())
            paintTimes.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - paintStarted)
                    .count());
        if (paintTimes.size() > 1000)
            paintTimes.erase(paintTimes.begin(), paintTimes.begin() + 500);
#endif
    }
#ifdef VMD_TESTING
    void recordMetric() {
        if (metricsFile.empty())
            return;
        auto now = std::chrono::steady_clock::now();
        if (loadingHeartbeatSamples)
            loadingHeartbeatMaxMs = std::max(
                loadingHeartbeatMaxMs, std::chrono::duration<double, std::milli>(now - heartbeatAt).count());
        KillTimer(hwnd, 14);
        PROCESS_MEMORY_COUNTERS_EX memory{};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory));
        std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
        double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        out << "{\"first_content_ms\":" << ms << ",\"open_ms\":"
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - openStarted)
                   .count()
            << ",\"read_ms\":" << doc->readMs << ",\"parse_ms\":" << doc->parseMs
            << ",\"blocks\":" << doc->blocks.size() << ",\"characters\":" << doc->source.size()
            << ",\"peak_working_set_bytes\":" << memory.PeakWorkingSetSize
            << ",\"loading_heartbeat_samples\":" << loadingHeartbeatSamples
            << ",\"loading_heartbeat_max_ms\":" << loadingHeartbeatMaxMs << ",\"startup_phases_ms\":{";
        bool first = true;
        for (auto [name, elapsed] : startupPhases) {
            if (!first)
                out << ',';
            first = false;
            out << '\"' << name << "\":" << elapsed;
        }
        out << "}}\n";
    }
#endif
#ifdef VMD_TESTING
    void associationTrace(const char *event) {
        if (metricsFile.empty())
            return;
        std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
        out << "{\"association\":\"" << event << "\",\"elapsed_ms\":"
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
            << ",\"generation\":" << generation << "}\n";
    }
#endif
    void startAssociationWork(bool registration = false) {
        auto result = std::make_shared<AssociationResult>();
        result->registration = registration;
#ifdef VMD_TESTING
        auto test = testAssociation;
#endif
        // The worker owns only its result, never App or an HWND. Closing never joins it.
        try {
            std::thread([result VMD_TEST_ONLY(, test)] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                try {
                    if (result->registration) {
#ifdef VMD_TESTING
                        if (test.empty())
#endif
                        {
                            registerApplication(false, false, &result->succeeded);
                            if (result->succeeded)
                                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr,
                                               nullptr);
                        }
                    }
#ifdef VMD_TESTING
                    else if (!test.empty()) {
                        if (test == L"slow")
                            Sleep(15000);
                        result->association =
                            test == L"unknown" ? MarkdownAssociation::Unknown : MarkdownAssociation::Mismatch;
                    }
#endif
                    else
                        result->association = probeMarkdownAssociation();
                } catch (...) {
                    // Optional shell integration must not affect reading or terminate the process.
                }
                result->done.store(true);
            }).detach();
        } catch (const std::exception &) {
            associationPhase = AssociationPhase::Finished;
            if (registration)
                associationFailed();
            return;
        }
        associationResult = std::move(result);
        associationPhase = AssociationPhase::Running;
        SetTimer(hwnd, 17, 100, nullptr);
        VMD_TEST_ONLY(associationTrace(registration ? "registration_started" : "probe_started");)
    }
    void associationFailed() {
        associationError = L"Could not register this location. Try again.";
        EnableWindow(associationButtons[0], TRUE);
        positionAssociation();
        InvalidateRect(associationPanel, nullptr, TRUE);
        VMD_TEST_ONLY(associationTrace("registration_failed");)
    }
    void pollAssociation() {
        if (!associationResult || !associationResult->done.load())
            return;
        // A later document also gets its first paint before any notice work.
        if (loading || associationReadyGeneration != generation)
            return;
        KillTimer(hwnd, 17);
        auto result = std::move(associationResult);
        associationPhase = AssociationPhase::Finished;
        if (result->registration) {
            if (result->succeeded) {
                if (openDefaultSettings()) {
                    VMD_TEST_ONLY(associationTrace("settings_opened");)
                    dismissAssociation();
                } else {
                    associationError = L"Could not open Default apps. Try Windows Settings.";
                    EnableWindow(associationButtons[0], TRUE);
                    positionAssociation();
                }
            } else
                associationFailed();
        } else {
            VMD_TEST_ONLY(associationTrace("probe_finished");)
            if (result->association == MarkdownAssociation::Mismatch)
                showAssociation();
        }
    }
    static constexpr auto associationTitle = L"Use this copy for .md files?";
    static constexpr auto associationText =
        L"Windows points to Voltura Markdown at another location. Choose this copy in Windows Default apps.";
    int associationTextHeight(HDC dc, int panelWidth) {
        RECT text{0, 0, panelWidth - (LONG)(32 * scale()), 0};
        DrawTextW(dc, associationText, -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        return text.bottom;
    }
    void positionAssociation() {
        if (!associationPanel)
            return;
        auto s = scale();
        int panelWidth = (int)(std::min(540.f, chromeWidth - 40) * s);
        auto dc = GetDC(associationPanel);
        auto old = SelectObject(dc, uiFont);
        int textHeight = associationTextHeight(dc, panelWidth);
        SelectObject(dc, old);
        ReleaseDC(associationPanel, dc);
        int buttonY = (int)(54 * s) + textHeight + (associationError.empty() ? 0 : (int)(28 * s));
        int panelHeight = buttonY + (int)(48 * s);
        POINT origin{(LONG)(20 * s), std::max((LONG)(top * s), (LONG)((height - 80) * s) - panelHeight)};
        ClientToScreen(hwnd, &origin);
        SetWindowPos(associationPanel, nullptr, origin.x, origin.y, panelWidth, panelHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        MoveWindow(associationButtons[0], (int)(16 * s), buttonY, (int)(170 * s), (int)(32 * s), TRUE);
        MoveWindow(associationButtons[1], (int)(194 * s), buttonY, (int)(94 * s), (int)(32 * s), TRUE);
        MoveWindow(associationButtons[2], (int)(296 * s), buttonY, (int)(86 * s), (int)(32 * s), TRUE);
        InvalidateRect(associationPanel, nullptr, FALSE);
    }
    static LRESULT CALLBACK associationProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto app = (App *)GetWindowLongPtrW(window, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            app = (App *)((CREATESTRUCTW *)lp)->lpCreateParams;
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)app);
        }
        if (!app)
            return DefWindowProcW(window, msg, wp, lp);
        if (msg == WM_ERASEBKGND)
            return 1;
        if (msg == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (msg == WM_DRAWITEM) {
            app->drawButton(*(DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        if (msg == WM_COMMAND) {
            app->command(LOWORD(wp));
            return 0;
        }
        if (msg == WM_TIMER && wp == 1) {
            KillTimer(window, 1);
            SetWindowTextW(app->associationButtons[1], L"Copy path");
            InvalidateRect(app->associationButtons[1], nullptr, FALSE);
            return 0;
        }
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            auto screen = BeginPaint(window, &ps);
            RECT bounds;
            GetClientRect(window, &bounds);
            auto dc = CreateCompatibleDC(screen);
            auto bitmap = CreateCompatibleBitmap(screen, bounds.right, bounds.bottom);
            auto oldBitmap = SelectObject(dc, bitmap);
            auto background = CreateSolidBrush(app->dark ? RGB(28, 34, 45) : RGB(255, 255, 255));
            FillRect(dc, &bounds, background);
            DeleteObject(background);
            auto border = CreateSolidBrush(app->dark ? RGB(48, 58, 73) : RGB(220, 226, 235));
            FrameRect(dc, &bounds, border);
            DeleteObject(border);
            auto oldFont = SelectObject(dc, app->uiFont);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->dark ? RGB(229, 233, 240) : RGB(32, 41, 56));
            auto s = app->scale();
            RECT title{(LONG)(16 * s), (LONG)(14 * s), bounds.right - (LONG)(16 * s), (LONG)(38 * s)};
            DrawTextW(dc, associationTitle, -1, &title, DT_SINGLELINE | DT_NOPREFIX);
            RECT text{(LONG)(16 * s), (LONG)(44 * s), bounds.right - (LONG)(16 * s), bounds.bottom};
            DrawTextW(dc, associationText, -1, &text, DT_WORDBREAK | DT_NOPREFIX);
            if (!app->associationError.empty()) {
                text.top += app->associationTextHeight(dc, bounds.right) + (LONG)(8 * s);
                DrawTextW(dc, app->associationError.c_str(), -1, &text, DT_SINGLELINE | DT_NOPREFIX);
            }
            SelectObject(dc, oldFont);
            BitBlt(screen, 0, 0, bounds.right, bounds.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(dc);
            EndPaint(window, &ps);
            return 0;
        }
        if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
            return app->message(msg, wp, lp);
        return DefWindowProcW(window, msg, wp, lp);
    }
    void showAssociation() {
        VMD_TEST_ONLY(auto previousFocus = GetFocus();)
        WNDCLASSEXW c{sizeof(c)};
        c.lpfnWndProc = associationProc;
        c.hInstance = GetModuleHandleW(nullptr);
        c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        c.lpszClassName = L"VolturaMarkdownAssociation";
        RegisterClassExW(&c);
        associationPanel =
            CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, c.lpszClassName, associationTitle,
                            WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, hwnd, nullptr, c.hInstance, this);
        if (!associationPanel)
            return;
        const wchar_t *names[] = {L"Open Default apps", L"Copy path", L"Dismiss"};
        const int ids[] = {150, 152, 151};
        for (size_t i = 0; i < associationButtons.size(); ++i) {
            associationButtons[i] =
                CreateWindowExW(0, L"BUTTON", names[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                                0, 1, 1, associationPanel, (HMENU)(INT_PTR)ids[i], nullptr, nullptr);
            SetWindowSubclass(associationButtons[i], childProc, 1, (DWORD_PTR)this);
        }
        positionAssociation();
        ShowWindow(associationPanel, SW_SHOWNOACTIVATE);
        VMD_TEST_ONLY(associationTrace(previousFocus == GetFocus() ? "notice_focus_preserved"
                                                                   : "notice_focus_changed");)
        VMD_TEST_ONLY(associationTrace("notice_shown");)
    }
    void dismissAssociation() {
        KillTimer(associationPanel, 1);
        if (associationPanel && IsChild(associationPanel, GetFocus()))
            SetFocus(hwnd);
        ShowWindow(associationPanel, SW_HIDE);
        VMD_TEST_ONLY(associationTrace("notice_dismissed");)
    }
    void zoomVisibility() {
        bool visible = showControls && zoomExpanded;
        if (visible == zoomFadeVisible)
            return;
        zoomFadeVisible = visible;
        zoomFadeStartAlpha = zoomAlpha;
        zoomFadeStarted = GetTickCount64();
        BOOL animate = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
        zoomFadeDuration = animate ? 150.f : 1.f;
        if (visible) {
            positionZoomOverlay();
            ShowWindow(zoomPanel, SW_SHOWNOACTIVATE);
        }
        SetTimer(hwnd, 11, 16, nullptr);
    }
    void zoomFade() {
        float progress = std::min(1.f, (GetTickCount64() - zoomFadeStarted) / zoomFadeDuration);
        float eased = progress * progress * (3 - 2 * progress);
        zoomAlpha = (BYTE)std::lround(zoomFadeStartAlpha +
                                      ((zoomFadeVisible ? 255 : 0) - zoomFadeStartAlpha) * eased);
        SetLayeredWindowAttributes(zoomPanel, RGB(255, 0, 255), zoomAlpha, LWA_COLORKEY | LWA_ALPHA);
        if (progress == 1) {
            KillTimer(hwnd, 11);
            if (!zoomFadeVisible)
                ShowWindow(zoomPanel, SW_HIDE);
        }
    }
    static LRESULT CALLBACK zoomProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto app = (App *)GetWindowLongPtrW(window, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            app = (App *)((CREATESTRUCTW *)lp)->lpCreateParams;
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)app);
        }
        if (!app)
            return DefWindowProcW(window, msg, wp, lp);
        if (msg == WM_ERASEBKGND)
            return 1;
        if (msg == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            auto dc = BeginPaint(window, &ps);
            auto background = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(dc, &ps.rcPaint, background);
            DeleteObject(background);
            EndPaint(window, &ps);
            return 0;
        }
        if (msg == WM_DRAWITEM) {
            app->drawButton(*(DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        if (msg == WM_COMMAND) {
            app->command(LOWORD(wp));
            return 0;
        }
        if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
            return app->message(msg, wp, lp);
        return DefWindowProcW(window, msg, wp, lp);
    }
    HWND nextTabControl(HWND current, bool reverse) {
        auto first = GetNextDlgTabItem(hwnd, nullptr, FALSE);
        auto last = GetNextDlgTabItem(hwnd, nullptr, TRUE);
        if (IsWindowVisible(associationPanel)) {
            std::vector<HWND> controls;
            auto control = first;
            if (control)
                do {
                    if (IsWindowVisible(control) && IsWindowEnabled(control))
                        controls.push_back(control);
                    control = GetNextDlgTabItem(hwnd, control, FALSE);
                } while (control && control != first);
            for (auto button : associationButtons)
                if (IsWindowEnabled(button))
                    controls.push_back(button);
            if (IsWindowVisible(zoomPanel))
                controls.insert(controls.end(), zoomButtons.begin(), zoomButtons.end());
            auto found = std::find(controls.begin(), controls.end(), current);
            if (found == controls.end())
                return reverse ? controls.back() : controls.front();
            auto index = (size_t)(found - controls.begin());
            return controls[(index + controls.size() + (reverse ? -1 : 1)) % controls.size()];
        }
        if (!current && reverse && IsWindowVisible(zoomPanel))
            return zoomButtons.back();
        auto zoomControl = std::find(zoomButtons.begin(), zoomButtons.end(), current);
        if (zoomControl != zoomButtons.end()) {
            auto index = (int)(zoomControl - zoomButtons.begin()) + (reverse ? -1 : 1);
            return index >= 0 && index < (int)zoomButtons.size() ? zoomButtons[index]
                   : reverse                                     ? last
                                                                 : first;
        }
        if (IsWindowVisible(zoomPanel) && (current == (reverse ? first : last)))
            return reverse ? zoomButtons.back() : zoomButtons.front();
        return GetNextDlgTabItem(hwnd, current, reverse);
    }
    void positionZoomOverlay() {
        if (!zoomPanel)
            return;
        POINT origin{(LONG)((canvasLeft + width - 158) * scale()), (LONG)((height - 52) * scale())};
        ClientToScreen(hwnd, &origin);
        SetWindowPos(zoomPanel, nullptr, origin.x, origin.y, (int)(134 * scale()), (int)(34 * scale()),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        const float offsets[] = {0, 38, 100};
        for (size_t i = 0; i < zoomButtons.size(); ++i)
            MoveWindow(zoomButtons[i], (int)(offsets[i] * scale()), 0, (int)((i == 1 ? 58 : 34) * scale()),
                       (int)(34 * scale()), TRUE);
    }
    void updateZoomControls() {
        auto label = std::to_wstring((int)std::lround(zoom * 100)) + L"%";
        SetWindowTextW(zoomButtons[1], label.c_str());
        for (auto button : zoomButtons)
            InvalidateRect(button, nullptr, FALSE);
        zoomExpanded = std::abs(zoom - 1.f) > .001f || GetTickCount64() < zoomUntil;
        zoomVisibility();
    }
    void positions() {
        RECT r;
        GetClientRect(hwnd, &r);
        width = std::max(1.f, r.right / scale() - 12);
        chromeWidth = width;
        canvasLeft = 0;
        height = std::max(1.f, r.bottom / scale() - 12);
        top = 78;
        float start = width - (fullscreen ? 7 : 6) * 38 - 20;
        for (int i = 0; i < 7; ++i)
            MoveWindow(buttons[i], (int)((start + i * 38) * scale()), (int)(20 * scale()),
                       (int)(34 * scale()), (int)(34 * scale()), TRUE);
        positionZoomOverlay();
        positionAssociation();
        if (canvas)
            MoveWindow(canvas, 0, (int)(top * scale()), r.right,
                       std::max<LONG>(1, r.bottom - (int)(top * scale())), FALSE);
        if (editor && !informationView) {
            editor->position((int)(78 * scale()));
            bool visible = editor->isPreview() || editor->isLive();
            ShowWindow(canvas, visible ? SW_SHOWNA : SW_HIDE);
            if (visible) {
                auto bounds = editor->previewBounds((int)(78 * scale()));
                canvasLeft = bounds.left / scale();
                top = bounds.top / scale();
                width = std::max(1.f, (bounds.right - bounds.left) / scale() - 12);
                SetWindowPos(canvas, HWND_TOP, bounds.left, bounds.top,
                             std::max(1L, bounds.right - bounds.left),
                             std::max(1L, bounds.bottom - bounds.top), SWP_NOACTIVATE);
            }
        }
        if (target) {
            // Mode changes resize the canvas without resizing the parent window.
            // Keep its backing buffer in sync or D2D stretches the old preview frame.
            RECT bounds{};
            GetClientRect(canvas, &bounds);
            auto size = D2D1::SizeU(bounds.right, bounds.bottom);
            auto current = target->GetPixelSize();
            if (current.width != size.width || current.height != size.height)
                target->Resize(size);
            target->SetTransform(D2D1::Matrix3x2F::Translation(0, -top));
        }
        positionZoomOverlay();
        invalidateHeader();
        positionSearch();
        if (backButton)
            MoveWindow(backButton, (int)(20 * scale()), (int)(20 * scale()), (int)(34 * scale()),
                       (int)(34 * scale()), TRUE);
    }
    float searchBoxWidth() const {
        return std::max(100.f, chromeWidth - (fullscreen ? 7 : 6) * 38 - 20 - 48);
    }
    float searchCountWidth() const {
        return std::min(searchBoxWidth() * .55f,
                        std::clamp((float)searchStatus.size() * 8 + 12, 96.f, 180.f));
    }
    bool showSearchHint() const {
        return searching && countReady && searchTotal > 1 && searchBoxWidth() >= 350;
    }
    void positionSearch() {
        if (!search)
            return;
        HDC dc = GetDC(search);
        auto oldFont = SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, oldFont);
        ReleaseDC(search, dc);
        int fieldHeight = metrics.tmHeight + 2 * GetSystemMetricsForDpi(SM_CYBORDER, (UINT)dpi);
        MoveWindow(search, (int)(36 * scale()), (int)(37 * scale()) - fieldHeight / 2,
                   (int)(std::max(24.f, searchBoxWidth() - searchCountWidth() - 24) * scale()), fieldHeight,
                   TRUE);
    }
    void visibility(bool show) {
        if (show == showControls)
            return;
        showControls = show;
        // A visibility change can synthesize WM_MOUSEMOVE without physical movement.
        // Remember the cursor before hiding so that message cannot reveal us again.
        if (GetCursorPos(&lastPointer))
            pointerKnown = true;
        auto batch = BeginDeferWindowPos((int)buttons.size() + 2);
        auto stage = [&](HWND control, bool visible) {
            if (control && batch)
                batch = DeferWindowPos(batch, control, nullptr, 0, 0, 0, 0,
                                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                                           (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        };
        for (size_t i = 0; i < buttons.size(); ++i)
            stage(buttons[i], show && (i != 6 || fullscreen));
        stage(backButton, show && informationView && !searching);
        stage(search, show && searching);
        if (batch)
            EndDeferWindowPos(batch);
        else {
            for (size_t i = 0; i < buttons.size(); ++i)
                ShowWindow(buttons[i], show && (i != 6 || fullscreen) ? SW_SHOWNA : SW_HIDE);
            ShowWindow(backButton, show && informationView && !searching ? SW_SHOWNA : SW_HIDE);
            ShowWindow(search, show && searching ? SW_SHOWNA : SW_HIDE);
        }
        zoomVisibility();
        invalidateHeader();
    }
    void themeTooltip() {
        if (!tooltip)
            return;
        // System visual styles ignore TTM_SETTIP*COLOR; disable only for our tooltip.
        SetWindowTheme(tooltip, L"", L"");
        SendMessageW(tooltip, TTM_SETTIPBKCOLOR, dark ? RGB(28, 34, 45) : RGB(255, 255, 255), 0);
        SendMessageW(tooltip, TTM_SETTIPTEXTCOLOR, dark ? RGB(229, 233, 240) : RGB(32, 41, 56), 0);
        SendMessageW(tooltip, WM_SETFONT, (WPARAM)uiFont, FALSE);
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)(340 * scale()));
        RECT margins{(LONG)(8 * scale()), (LONG)(5 * scale()), (LONG)(8 * scale()), (LONG)(5 * scale())};
        SendMessageW(tooltip, TTM_SETMARGIN, 0, (LPARAM)&margins);
        SendMessageW(tooltip, TTM_POP, 0, 0);
    }
    void theme() {
        bool previousDark = dark;
        dark = prefs.theme < 0 ? systemDark() : prefs.theme == 1;
        dialogDark = dark;
        if (previousDark != dark)
            clearLayouts();
        if (editor)
            editor->theme(dark, (unsigned)dpi);
        themeTooltip();
        BOOL use = dark;
        DwmSetWindowAttribute(hwnd, 20, &use, sizeof(use));
        COLORREF caption = dark ? RGB(16, 19, 25) : RGB(250, 251, 253);
        DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
        for (auto b : buttons)
            InvalidateRect(b, nullptr, FALSE);
        for (auto button : zoomButtons)
            InvalidateRect(button, nullptr, FALSE);
        if (backButton)
            InvalidateRect(backButton, nullptr, FALSE);
        if (search)
            InvalidateRect(search, nullptr, FALSE);
        if (associationPanel) {
            InvalidateRect(associationPanel, nullptr, FALSE);
            for (auto button : associationButtons)
                InvalidateRect(button, nullptr, FALSE);
        }
        invalidateHeader();
        invalidate();
    }
    void refreshDpiAssets() {
        if (editor)
            editor->theme(dark, (unsigned)dpi);
        auto nextFont = CreateFontW(-(int)(14 * dpi / 96.f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (search)
            SendMessageW(search, WM_SETFONT, (WPARAM)nextFont, TRUE);
        if (uiFont)
            DeleteObject(uiFont);
        uiFont = nextFont;
        themeTooltip();
        positionSearch();
        positionAssociation();
        auto instance = GetModuleHandleW(nullptr);
        auto big = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     GetSystemMetricsForDpi(SM_CXICON, (UINT)dpi),
                                     GetSystemMetricsForDpi(SM_CYICON, (UINT)dpi), 0);
        auto smallHandle = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetricsForDpi(SM_CXSMICON, (UINT)dpi),
                                             GetSystemMetricsForDpi(SM_CYSMICON, (UINT)dpi), 0);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallHandle);
        if (largeIcon)
            DestroyIcon(largeIcon);
        if (smallIcon)
            DestroyIcon(smallIcon);
        largeIcon = big;
        smallIcon = smallHandle;
    }
    void initialise() {
        dpi = (float)GetDpiForWindow(hwnd);
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            (IUnknown **)write.GetAddressOf());
        if (!d2d || !write)
            throw std::runtime_error("Windows text rendering could not be initialized.");
        VMD_TEST_ONLY(startupPhase("graphics_factories_ready");)
#ifdef VMD_TESTING
        prefs = selfTest ? Preferences{} : readPreferences();
        if (testAutomaticUpdates) {
            prefs.automaticUpdates = true;
            if (!updateFixture.empty())
                updates.fixture(updateFixture);
        }
#else
        prefs = readPreferences();
#endif
        zoom = prefs.zoom;
        zoomExpanded = std::abs(zoom - 1.f) > .001f;
        images = std::make_unique<Images>(hwnd);
        WNDCLASSEXW canvasClass{sizeof(canvasClass)};
        canvasClass.style = CS_DBLCLKS;
        canvasClass.lpfnWndProc = canvasProc;
        canvasClass.hInstance = GetModuleHandleW(nullptr);
        canvasClass.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        canvasClass.lpszClassName = L"VolturaMarkdownCanvas";
        RegisterClassExW(&canvasClass);
        canvas = CreateWindowExW(0, canvasClass.lpszClassName, L"Read-only document", WS_CHILD | WS_VISIBLE,
                                 0, 0, 1, 1, hwnd, nullptr, canvasClass.hInstance, this);
        const wchar_t *names[] = {
            L"Open file (Ctrl+O)",        L"Find (Ctrl+F)",    L"Show source / formatted view (Ctrl+Shift+V)",
            L"Toggle light / dark theme", L"Fullscreen (F11)", L"More options",
            L"Close viewer (Alt+F4)"};
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                  WS_POPUP | TTS_ALWAYSTIP | TTS_NOFADE | TTS_NOANIMATE, 0, 0, 0, 0, hwnd,
                                  nullptr, nullptr, nullptr);
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 300);
        for (int i = 0; i < 7; ++i) {
            buttons[i] =
                CreateWindowExW(0, L"BUTTON", names[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                                0, 0, 0, hwnd, (HMENU)(INT_PTR)(i < 6 ? 100 + i : 107), nullptr, nullptr);
            SetWindowSubclass(buttons[i], childProc, 1, (DWORD_PTR)this);
            TOOLINFOW info{sizeof(info)};
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = hwnd;
            info.uId = (UINT_PTR)buttons[i];
            info.lpszText = const_cast<wchar_t *>(names[i]);
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, (LPARAM)&info);
        }
        WNDCLASSEXW zoomClass{sizeof(zoomClass)};
        zoomClass.lpfnWndProc = zoomProc;
        zoomClass.hInstance = GetModuleHandleW(nullptr);
        zoomClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        zoomClass.lpszClassName = L"VolturaMarkdownZoom";
        RegisterClassExW(&zoomClass);
        zoomPanel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                    zoomClass.lpszClassName, L"Zoom controls", WS_POPUP | WS_CLIPCHILDREN, 0,
                                    0, 1, 1, hwnd, nullptr, zoomClass.hInstance, this);
        SetLayeredWindowAttributes(zoomPanel, RGB(255, 0, 255), 0, LWA_COLORKEY | LWA_ALPHA);
        const wchar_t *zoomNames[] = {L"Zoom out (- or Ctrl+-)", L"100%", L"Zoom in (+ or Ctrl++)"};
        const wchar_t *zoomTips[] = {L"Zoom out (- or Ctrl+-)", L"Reset zoom to 100% (Ctrl+0)",
                                     L"Zoom in (+ or Ctrl++)"};
        for (size_t i = 0; i < zoomButtons.size(); ++i) {
            zoomButtons[i] =
                CreateWindowExW(0, L"BUTTON", zoomNames[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                0, 0, 1, 1, zoomPanel, (HMENU)(INT_PTR)(140 + i), nullptr, nullptr);
            SetWindowSubclass(zoomButtons[i], childProc, 1, (DWORD_PTR)this);
            TOOLINFOW info{sizeof(info)};
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = hwnd;
            info.uId = (UINT_PTR)zoomButtons[i];
            info.lpszText = const_cast<wchar_t *>(zoomTips[i]);
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, (LPARAM)&info);
        }
        VMD_TEST_ONLY(startupPhase("buttons_ready");)
        updateZoomControls();
        search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                                 (HMENU)200, nullptr, nullptr);
        SendMessageW(search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Find in document");
        SendMessageW(search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(search, EM_SETLIMITTEXT, 4096, 0);
        SetWindowSubclass(search, childProc, 1, (DWORD_PTR)this);
        backButton = CreateWindowExW(0, L"BUTTON", L"Back (Esc)", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                                     1, 1, hwnd, (HMENU)106, nullptr, nullptr);
        SetWindowSubclass(backButton, childProc, 1, (DWORD_PTR)this);
        TOOLINFOW backTip{sizeof(backTip)};
        backTip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        backTip.hwnd = hwnd;
        backTip.uId = (UINT_PTR)backButton;
        backTip.lpszText = const_cast<wchar_t *>(L"Back to document (Esc)");
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, (LPARAM)&backTip);
        SetTimer(hwnd, 1, 100, nullptr);
        ShowWindow(buttons[6], SW_HIDE);
        DragAcceptFiles(hwnd, TRUE);
        positions();
        refreshDpiAssets();
        theme();
        VMD_TEST_ONLY(startupPhase("controls_ready");)
        if (prefs.placed) {
            auto p = prefs.placement;
            if (p.showCmd == SW_SHOWMINIMIZED)
                p.showCmd = SW_SHOWNORMAL;
            RECT r = p.rcNormalPosition;
            if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
                p.showCmd = SW_HIDE;
                SetWindowPlacement(hwnd, &p);
            }
        }
    }
    static LRESULT CALLBACK childProc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto a = (App *)data;
        if (msg == WM_ERASEBKGND)
            return 1;
        if (msg == WM_GETDLGCODE && w == a->search)
            return DLGC_WANTALLKEYS;
        if (msg == WM_CHAR && w != a->search && !(a->associationPanel && IsChild(a->associationPanel, w)) &&
            a->zoomCharacter(wp))
            return 0;
        // TranslateMessage queues WM_CHAR before WM_KEYDOWN is dispatched. Enter
        // already navigates below; the single-line edit would beep on its character.
        if (w == a->search && msg == WM_CHAR && (wp == L'\r' || wp == L'\n'))
            return 0;
        if (msg == WM_KEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (a->associationPanel && IsChild(a->associationPanel, w)) {
                if (wp == VK_RETURN) {
                    a->command(GetDlgCtrlID(w));
                    return 0;
                }
                if (wp == VK_ESCAPE) {
                    a->dismissAssociation();
                    return 0;
                }
            }
            if (w == a->search && wp == VK_RETURN) {
                a->find(true, GetKeyState(VK_SHIFT) < 0);
                return 0;
            }
            if (wp == VK_F1 || wp == VK_ESCAPE || wp == VK_F11 || wp == VK_F3 ||
                (ctrl && (wp == 'O' || wp == 'F' || w != a->search))) {
                a->key(wp);
                return 0;
            }
        }
        if (msg == WM_SETFOCUS) {
            a->visibility(true);
            KillTimer(a->hwnd, 2);
        }
        if (msg == WM_MOUSEMOVE) {
            POINT point;
            if (GetCursorPos(&point))
                a->pointerMoved(point);
            if (!GetPropW(w, L"VolturaHover")) {
                SetPropW(w, L"VolturaHover", (HANDLE)1);
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, w, 0};
                TrackMouseEvent(&track);
                InvalidateRect(w, nullptr, FALSE);
            }
            if (a->fullscreen)
                SetTimer(a->hwnd, 2, 1800, nullptr);
        }
        if (msg == WM_MOUSELEAVE)
            RemovePropW(w, L"VolturaHover");
        if (msg == WM_MOUSELEAVE || msg == WM_KILLFOCUS)
            InvalidateRect(w, nullptr, FALSE);
        return DefSubclassProc(w, msg, wp, lp);
    }
    void openDialog() {
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = hwnd;
        dialog.lpstrFilter = L"Markdown files\0*.md;*.markdown;*.mdown;*.mkd\0All files\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = 32768;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (themedOpenFile(&dialog))
            open(path);
    }
    void reap() {
        for (auto i = workers.begin(); i != workers.end();) {
            if (i->done->load()) {
                i->thread.join();
                i = workers.erase(i);
            } else
                ++i;
        }
    }
    void open(std::filesystem::path path, bool offerCreate = false) {
        if (!leaveEdit([this, path] { open(path); }))
            return;
        cancelSearch();
        if (informationView)
            closeInformation();
        for (auto &w : workers)
            w.cancel->store(true);
        reap();
        ++generation;
        loading = true;
        SetTimer(hwnd, 5, 33, nullptr);
        notice.clear();
        invalidateHeader();
        firstPaint = false;
#ifdef VMD_TESTING
        openStarted = std::chrono::steady_clock::now();
#endif
        pendingPath = path;
        invalidate();
        auto cancel = std::make_shared<std::atomic_bool>(false),
             done = std::make_shared<std::atomic_bool>(false);
        auto gen = generation;
        HWND destination = hwnd;
        workers.reserve(workers.size() + 1);
        auto result = std::make_unique<LoadedDocument>();
        result->offerCreate = offerCreate;
        auto restore = restartView;
        result->error = L"The document could not be loaded.";
        workers.push_back({std::thread([path, gen, destination, cancel, done, restore,
                                        result = std::move(result)]() mutable {
                               result->generation = gen;
                               try {
                                   result->document = std::make_shared<Document>(load(path, cancel.get()));
                               } catch (const std::exception &e) {
                                   try {
                                       result->error = fromUtf8(e.what());
                                   } catch (...) {
                                   }
                                   if (!cancel->load()) {
                                       try {
                                           result->document =
                                               std::make_shared<Document>(loadSource(path, cancel.get()));
                                           result->document->plainSource = true;
                                           result->document->blocks = sourceBlocks(result->document->source);
                                           for (auto &block : result->document->blocks)
                                               block.kind = Kind::Paragraph;
                                       } catch (...) {
                                       }
                                   }
                               }
                               if (restore && result->document && !cancel->load()) {
                                   try {
                                       result->restoreUnchanged =
                                           restartDigest(result->document->source) == restore->digest;
                                   } catch (...) {
                                   }
                               }
                               if (!result->document && result->offerCreate) {
                                   auto attributes = GetFileAttributesW(path.c_str());
                                   auto error = GetLastError();
                                   result->missing =
                                       attributes == INVALID_FILE_ATTRIBUTES &&
                                       (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
                               }
                               if (!cancel->load()) {
                                   auto pointer = result.release();
                                   if (!PostMessageW(destination, Loaded, 0, (LPARAM)pointer))
                                       delete pointer;
                               }
                               done->store(true);
                           }),
                           cancel, done});
    }
    void loaded(std::unique_ptr<LoadedDocument> r) {
        if (r->generation != generation)
            return;
        loading = false;
        KillTimer(hwnd, 5);
        invalidateHeader();
        if (!r->document) {
            if (r->offerCreate && r->missing) {
                auto message = L"This file does not exist:\n\n" + pendingPath.wstring() +
                               L"\n\nCreate a new Markdown document here?";
                if (themedMessageBox(hwnd, message.c_str(), L"Create Markdown file",
                                     MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    beginEdit(true, pendingPath);
                } else {
                    launchedWithFile = false;
                    if (fullscreen)
                        toggleFullscreen();
                }
                notice.clear();
                invalidate();
                return;
            }
            VMD_TEST_ONLY(if (!restartFile.empty()) reportRestart(".after");)
            notice = r->error;
            invalidate();
            return;
        }
        VMD_TEST_ONLY(startupPhase("document_delivered");)
        images->cancelPending(true);
        doc = std::move(r->document);
        source = false;
        rawBlocks.clear();
        anchor = caret = 0;
        searchMatch = false;
        notice.clear();
        SetWindowTextW(hwnd, (doc->path.filename().wstring() + L" — Voltura Markdown").c_str());
        resetLayout(false);
        if (restartView) {
            auto view = *restartView;
            restartView.reset();
            if (r->restoreUnchanged) {
                VMD_TEST_ONLY(restartMatched = true;)
                source = view.source;
                if (source)
                    rawBlocks = sourceBlocks(doc->source);
                zoom = view.zoom;
                updateZoomControls();
                resetLayout(false);
                if (view.block < blocks().size() && device()) {
                    auto &current = layout((size_t)view.block);
                    scroll =
                        heights.before((size_t)view.block) + std::min(view.within, (double)current.height);
                }
                horizontal = view.horizontal;
                bars();
            }
        }
        invalidateHeader();
        if (searching)
            find(false);
#ifdef VMD_TESTING
        if (scrollBenchmark) {
            paintTimes.clear();
            scrollFrames = 0;
            scrollBenchmarkDirection = 1;
            scrollBenchmarkReversals = 0;
            scrollTarget = scroll;
            scrollBenchmarkStarted = std::chrono::steady_clock::now();
            SetTimer(hwnd, 13, 10, nullptr);
        } else if (!verifyFile.empty())
            SetTimer(hwnd, 10, 120, nullptr);
#endif
    }
    void toggleSource() {
        if (editor && !informationView) {
            editor->togglePreview();
            return;
        }
        if (!doc)
            return;
        cancelSearch();
        double ratio = heights.total() > 0 ? scroll / heights.total() : 0;
        if (!source && rawBlocks.empty())
            rawBlocks = sourceBlocks(doc->source);
        source = !source;
        anchor = caret = 0;
        searchMatch = false;
        resetLayout(false);
        scroll = ratio * std::max(0., heights.total() - (height - top - bottom));
        bars();
        invalidate();
        SetFocus(hwnd);
    }
    bool zoomCharacter(WPARAM character) {
        // Use translated characters so Shift and keyboard layouts produce the actual symbol.
        if ((character != L'+' && character != L'-') || GetKeyState(VK_CONTROL) < 0 ||
            GetKeyState(VK_MENU) < 0)
            return false;
        zoomBy(zoom + (character == L'+' ? .1f : -.1f));
        return true;
    }
    void zoomBy(float next) {
        float previous = zoom;
        zoom = std::clamp(std::round(next * 10) / 10, .5f, 3.f);
        prefs.zoom = zoom;
        zoomUntil = GetTickCount64() + 2200;
        visibility(true);
        updateZoomControls();
        SetTimer(hwnd, 12, 2200, nullptr);
        if (zoom != previous) {
            resetLayout();
            zoomLayoutPending = true;
        }
    }
    void toggleFullscreen() {
        stopScrolling();
        if (!fullscreen) {
            saved.length = sizeof(saved);
            GetWindowPlacement(hwnd, &saved);
            if (saved.showCmd == SW_HIDE)
                saved.showCmd = prefs.placed && prefs.placement.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED
                                                                                            : SW_SHOWNORMAL;
            oldStyle = GetWindowLongW(hwnd, GWL_STYLE);
            fullscreen = true;
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongW(hwnd, GWL_STYLE, oldStyle & ~WS_OVERLAPPEDWINDOW);
            MONITORINFO mi = monitor;
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED);
            SetTimer(hwnd, 2, 1800, nullptr);
        } else {
            fullscreen = false;
            KillTimer(hwnd, 2);
            SetWindowLongW(hwnd, GWL_STYLE, oldStyle);
            SetWindowPlacement(hwnd, &saved);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            visibility(true);
        }
        ShowWindow(buttons[6], fullscreen && showControls ? SW_SHOW : SW_HIDE);
        SetFocus(hwnd);
    }
    HMENU createMenu() {
        bool hasFile = doc && !doc->path.empty() && !loading;
        menuRows = {{160, L"New Markdown\tCtrl+N", false, !loading},
                    {161, L"Edit Markdown\tCtrl+E", false, !loading && doc && !doc->plainSource},
                    {162, L"Save\tCtrl+S", false, editor != nullptr},
                    {163, L"Save As…\tCtrl+Shift+S", false, editor != nullptr},
                    {164, L"Switch to view mode", false, editor != nullptr},
                    {0, L"", true},
                    {130, L"Copy file", false, hasFile},
                    {131, L"Copy file name", false, hasFile},
                    {132, L"Copy full path", false, hasFile},
                    {133, L"Open file directory", false, hasFile},
                    {134, L"Edit file with…", false, hasFile},
                    {0, L"", true},
                    {120, L"Register for .md files…"},
                    {121, L"Unregister application"},
                    {0, L"", true},
                    {124, L"Start fullscreen", false, true, prefs.startFullscreen},
                    {126, L"Automatic updates", false, true, prefs.automaticUpdates},
                    {122, L"Use Windows theme", false, true, prefs.theme < 0},
                    {125, L"Help\tF1", false, !loading},
                    {123, L"About / open-source licenses", false, !loading}};
        HMENU popup = CreatePopupMenu();
        MENUINFO info{sizeof(info)};
        info.fMask = MIM_STYLE;
        info.dwStyle = MNS_NOCHECK;
        SetMenuInfo(popup, &info);
        for (auto &row : menuRows) {
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_DATA | MIIM_STRING;
            item.wID = row.id;
            item.fType = MFT_OWNERDRAW | (row.separator ? MFT_SEPARATOR : 0);
            item.fState = (row.enabled ? MFS_ENABLED : MFS_DISABLED) | (row.checked ? MFS_CHECKED : 0);
            item.dwItemData = (ULONG_PTR)&row;
            item.dwTypeData = row.text.data();
            InsertMenuItemW(popup, GetMenuItemCount(popup), TRUE, &item);
        }
        return popup;
    }
    void measureMenu(MEASUREITEMSTRUCT &item) {
        auto row = reinterpret_cast<MenuRow *>(item.itemData);
        HDC dc = GetDC(hwnd);
        auto old = SelectObject(dc, uiFont);
        auto tab = row->text.find(L'\t');
        SIZE label{}, shortcut{};
        GetTextExtentPoint32W(dc, row->text.c_str(),
                              (int)(tab == std::wstring::npos ? row->text.size() : tab), &label);
        if (tab != std::wstring::npos)
            GetTextExtentPoint32W(dc, row->text.c_str() + tab + 1, (int)(row->text.size() - tab - 1),
                                  &shortcut);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        item.itemWidth = label.cx + (UINT)(56 * scale());
        if (tab != std::wstring::npos)
            item.itemWidth += shortcut.cx + (UINT)(40 * scale());
        item.itemHeight = (UINT)((row->separator ? 10 : 34) * scale());
    }
    void drawMenu(const DRAWITEMSTRUCT &item) {
        auto &row = *reinterpret_cast<MenuRow *>(item.itemData);
        RECT r = item.rcItem;
        HDC dc = CreateCompatibleDC(item.hDC);
        auto bitmap = CreateCompatibleBitmap(item.hDC, r.right - r.left, r.bottom - r.top);
        auto oldBitmap = SelectObject(dc, bitmap);
        SetViewportOrgEx(dc, -r.left, -r.top, nullptr);
        COLORREF background = dark ? RGB(28, 34, 45) : RGB(255, 255, 255);
        auto fill = CreateSolidBrush(background);
        FillRect(dc, &r, fill);
        DeleteObject(fill);
        if (row.separator) {
            auto pen = CreatePen(PS_SOLID, 1, dark ? RGB(48, 58, 73) : RGB(220, 226, 235));
            auto previous = SelectObject(dc, pen);
            MoveToEx(dc, r.left + (int)(12 * scale()), (r.top + r.bottom) / 2, nullptr);
            LineTo(dc, r.right - (int)(12 * scale()), (r.top + r.bottom) / 2);
            SelectObject(dc, previous);
            DeleteObject(pen);
        } else {
            if ((item.itemState & ODS_SELECTED) && row.enabled) {
                auto hover = CreateSolidBrush(dark ? RGB(46, 59, 78) : RGB(230, 237, 250));
                auto oldBrush = SelectObject(dc, hover);
                auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
                RoundRect(dc, r.left + (int)(4 * scale()), r.top + (int)(2 * scale()),
                          r.right - (int)(4 * scale()), r.bottom - (int)(2 * scale()), (int)(8 * scale()),
                          (int)(8 * scale()));
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(hover);
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, !row.enabled ? (dark ? RGB(108, 119, 136) : RGB(148, 157, 170))
                                          : (dark ? RGB(229, 233, 240) : RGB(32, 41, 56)));
            auto font = SelectObject(dc, uiFont);
            RECT label = r;
            label.left += (LONG)(34 * scale());
            label.right -= (LONG)(16 * scale());
            auto tab = row.text.find(L'\t');
            if (tab != std::wstring::npos) {
                const wchar_t *shortcut = row.text.c_str() + tab + 1;
                int length = (int)(row.text.size() - tab - 1);
                SIZE size{};
                GetTextExtentPoint32W(dc, shortcut, length, &size);
                RECT cap{label.right - size.cx - (LONG)(16 * scale()),
                         (r.top + r.bottom - size.cy) / 2 - (LONG)(2 * scale()), label.right,
                         (r.top + r.bottom + size.cy) / 2 + (LONG)(2 * scale())};
                auto capBrush = CreateSolidBrush(dark ? RGB(36, 44, 57) : RGB(245, 247, 250));
                auto capPen = CreatePen(PS_SOLID, std::max(1, (int)scale()),
                                        dark ? RGB(71, 83, 101) : RGB(190, 199, 212));
                auto oldBrush = SelectObject(dc, capBrush);
                auto oldPen = SelectObject(dc, capPen);
                RoundRect(dc, cap.left, cap.top, cap.right, cap.bottom, (int)(6 * scale()),
                          (int)(6 * scale()));
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(capBrush);
                DeleteObject(capPen);
                DrawTextW(dc, shortcut, length, &cap, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                label.right = cap.left - (LONG)(24 * scale());
            }
            DrawTextW(dc, row.text.c_str(), (int)(tab == std::wstring::npos ? row.text.size() : tab), &label,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, font);
            if (row.checked) {
                auto pen = CreatePen(PS_SOLID, std::max(1, (int)(1.7f * scale())),
                                     dark ? RGB(144, 185, 255) : RGB(39, 95, 199));
                auto previous = SelectObject(dc, pen);
                LONG x = r.left + (LONG)(14 * scale()), y = (r.top + r.bottom) / 2;
                POINT points[] = {{x, y},
                                  {x + (LONG)(3 * scale()), y + (LONG)(3 * scale())},
                                  {x + (LONG)(9 * scale()), y - (LONG)(4 * scale())}};
                Polyline(dc, points, 3);
                SelectObject(dc, previous);
                DeleteObject(pen);
            }
        }
        BitBlt(item.hDC, r.left, r.top, r.right - r.left, r.bottom - r.top, dc, r.left, r.top, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    void menu() {
        auto popup = createMenu();
        HBRUSH background = CreateSolidBrush(dark ? RGB(28, 34, 45) : RGB(255, 255, 255));
        MENUINFO info{sizeof(info)};
        info.fMask = MIM_BACKGROUND;
        info.hbrBack = background;
        SetMenuInfo(popup, &info);
        RECT button;
        GetWindowRect(buttons[5], &button);
        menuOpen = true;
        auto id = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_NOANIMATION, button.right,
                                 button.bottom + (int)(6 * scale()), 0, hwnd, nullptr);
        menuOpen = false;
        DestroyMenu(popup);
        DeleteObject(background);
        if (id)
            command(id);
    }
    void command(int id) {
        if (id == 160) {
            beginEdit(true);
            return;
        }
        if (id == 161) {
            beginEdit();
            return;
        }
        if (id == 162 || id == 163) {
            if (editor)
                editor->save(id == 163);
            return;
        }
        if (id == 164) {
            finishEdit();
            return;
        }
        switch (id) {
        case 150:
            if (associationResult || !IsWindowVisible(associationPanel))
                break;
            associationError.clear();
            EnableWindow(associationButtons[0], FALSE);
            startAssociationWork(true);
            break;
        case 151:
            dismissAssociation();
            break;
        case 152: {
            if (!IsWindowVisible(associationPanel))
                break;
            bool copied = false;
            try {
                std::wstring path(32768, L'\0');
                auto length = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
                if (length && length < path.size()) {
                    path.resize(length);
#ifdef VMD_TESTING
                    if (!testAssociation.empty()) {
                        copied = testAssociation != L"copy-failure";
                        std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
                        out << "{\"copied_executable\":" << std::quoted(toUtf8(path)) << "}\n";
                    } else
#endif
                        copied = copyText(hwnd, path);
                }
            } catch (const std::exception &) {
                // Optional clipboard work must leave the open document usable.
            }
            SetWindowTextW(associationButtons[1], copied ? L"Copied" : L"Copy failed");
            InvalidateRect(associationButtons[1], nullptr, FALSE);
            SetTimer(associationPanel, 1, 2000, nullptr);
            break;
        }
        case 130:
        case 131:
        case 132:
        case 133:
        case 134:
            if (doc && !doc->path.empty() && !loading) {
                std::error_code error;
                auto path = std::filesystem::absolute(doc->path, error).lexically_normal();
                bool success = !error;
                if (success) {
                    if (id == 130)
                        success = copyFile(hwnd, path);
                    else if (id == 131)
                        success = copyText(hwnd, path.filename().wstring());
                    else if (id == 132)
                        success = copyText(hwnd, path.wstring());
                    else if (id == 133)
                        success = openFileDirectory(hwnd, path);
                    else {
                        HRESULT result = editFileWith(hwnd, path);
                        success = SUCCEEDED(result) || result == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
                                  result == E_ABORT;
                    }
                }
                if (!success) {
                    notice = id == 133   ? L"The file directory could not be opened."
                             : id == 134 ? L"The editor chooser could not be opened."
                                         : L"The clipboard is unavailable. Try again.";
                    invalidate();
                }
            }
            break;
        case 140:
            zoomBy(zoom - .1f);
            break;
        case 141:
            zoomBy(1);
            break;
        case 142:
            zoomBy(zoom + .1f);
            break;
        case 100:
            openDialog();
            break;
        case 101:
            showSearch();
            break;
        case 102:
            toggleSource();
            break;
        case 103:
            prefs.theme = dark ? 0 : 1;
            theme();
            break;
        case 104:
            toggleFullscreen();
            break;
        case 105:
            menu();
            break;
        case 106:
            closeInformation();
            break;
        case 107:
            requestClose();
            break;
        case 110:
            if (notice != L"Windows could not draw part of this document.") {
                notice = L"Windows could not draw part of this document.";
                invalidate();
            }
            break;
        case 120:
        case 121: {
            notice = registerApplication(id == 121);
            invalidate();
            break;
        }
        case 124:
            prefs.startFullscreen = !prefs.startFullscreen;
            break;
        case 126:
            prefs.automaticUpdates = !prefs.automaticUpdates;
            if (!prefs.automaticUpdates) {
                KillTimer(hwnd, 18);
                updateTimerStarted = false;
                updates.cancel();
            } else
                invalidate(); // Schedule only after a successful content/empty-window paint.
#ifdef VMD_TESTING
            if (!selfTest)
#endif
                saveAutomaticUpdates(prefs.automaticUpdates);
            break;
        case 122:
            prefs.theme = -1;
            theme();
            break;
        case 125:
            showHelp();
            break;
        case 123: {
            showAbout();
            break;
        }
        }
    }
    void showAbout() {
        showInformation(L"About", licenses());
    }
    void showHelp() {
        showInformation(L"Help", LR"md(# Using Voltura Markdown

Read Markdown files, or choose Edit Markdown (Ctrl+E) to edit source. Ctrl+N creates a new draft; Ctrl+S saves explicitly. A missing full file path supplied at startup offers to create a draft there.

## Edit mode

- **Heading, Bold, Italic, Underline, Link, Code, List, Image, Insert:** insert Markdown around a selection or use editable placeholders. Insert includes tables, images, lists, quotes, code, and rules.
- **Live:** edit source on the left and see the formatted document update on the right after a typing pause. This is the default for wide edit windows. Smaller windows keep the source/preview toggle; enabling Live stacks the panes.
- **Code:** wrap selected text in a fenced code block, or insert a selected placeholder.
- **Image (Ctrl+Shift+I):** enter alt text and a file path or HTTPS URL, browse local images, or insert a placeholder to fill in later.
- **More edit shortcuts:** Ctrl+Shift+1–6 for headings; Ctrl+Shift+0 for normal text; Ctrl+Shift+7/8/9 for numbered/bulleted/task lists; Ctrl+Shift+C/K for code block/inline code; Ctrl+Shift+Q/X for quote/strikethrough; Ctrl+Shift+L for live preview. Undo/Redo are also in Insert.
- **Table:** choose body rows and columns; the header row is added automatically.
- **Preview / Source (Ctrl+Shift+V):** preview the unsaved draft and return without losing undo history.
- **Ctrl+B / Ctrl+I / Ctrl+U / Ctrl+K:** bold, italic, underline, or insert a link. Underline uses supported `<u>` HTML tags. Ctrl+Z/Ctrl+Y undo/redo; Ctrl+F finds source text.
- **Save (Ctrl+S) / Save As (Ctrl+Shift+S):** write explicitly. Unsaved changes have an asterisk; leaving asks to save, discard, or cancel.
- **Warnings:** live checks after a typing pause identify unclosed fences, missing local files, and broken heading links. Select a warning to jump to source. Warnings allow saving; remote targets are not checked.
- **View mode:** leave the editor and return to reading. Escape returns from preview first, then switches to view mode. Tab inserts a tab in source; Ctrl+Tab moves to the toolbar. Editor buttons stay visible in fullscreen.
- **Find/Replace (Ctrl+H), Outline (Ctrl+Shift+O):** replace source text or jump to a heading. Enter continues lists; Shift+Tab outdents selected lines.
- **Insert tools:** edit existing tables, copy images into an assets folder, paste clipboard images, link to a heading, toggle source highlighting or synchronized scrolling, export HTML, and print/PDF. Save a new document before copying images. Ctrl+click a preview heading to jump to source.
- **Recover draft:** restore a background crash checkpoint as an unsaved document. Checkpoints never overwrite the original file. Explicit save or discard clears obsolete checkpoints.
- Files changed elsewhere require an explicit overwrite choice; Insert includes Reload from disk. Updates wait while modified drafts are open. Use Cancel pending action to stop a background image, replacement, or export operation.

Use the **back arrow** or <kbd>Escape</kbd> to return to your reading position.

## Open and read

- **Open a file:** use the folder icon or <kbd>Ctrl</kbd>+<kbd>O</kbd>.
- **Drag and drop:** drop a Markdown file into the window.
- **Reveal controls:** move the pointer. Hover over an icon to see its name.
- **Scroll:** <kbd>↑</kbd>/<kbd>↓</kbd> move vertically; <kbd>←</kbd>/<kbd>→</kbd> move horizontally across wide tables and code.
- **Links:** colored link text shows its destination on hover. Click a heading link to navigate within the document; synchronized Live preview follows the destination in source too.
- **Code blocks:** click the copy icon in the upper-right corner to copy the complete block, including indentation and line breaks. Hover for **Copy code to clipboard**; **Copied!** confirms success.
- Fenced code with a recognized language label uses basic syntax coloring: PowerShell, JavaScript/TypeScript, C#, C/C++, Python, JSON/JSONC, SQL, and shell/Bash. Other labels remain plain text. Inline code has a subtle background.
- HTML tables support merged cells, captions, nested tables, images, and alignment hints. HTML images respect width/height, pictures can select light/dark sources, and simple SVG badges retain sharp text at different zoom levels. Adjacent Markdown badges wrap together.
- Small text, subscripts, superscripts, and keyboard keys keep their formatting. HTML uses a native presentation subset; CSS and scripts are not executed. Exceptionally large rich table bands show readable child blocks instead of table geometry.

## Find, copy and zoom

- **Find text:** press <kbd>Ctrl</kbd>+<kbd>F</kbd>.
- **Next / previous match:** press <kbd>Enter</kbd> / <kbd>Shift</kbd>+<kbd>Enter</kbd>, or <kbd>F3</kbd> / <kbd>Shift</kbd>+<kbd>F3</kbd>.

The search field shows your current match and total, such as **3 of 12**. The first hit appears before background counting finishes. A navigation hint appears for multiple hits when there is room, and matches keep two text lines of context above them where possible.
- **Copy text:** drag to select, then press <kbd>Ctrl</kbd>+<kbd>C</kbd>. <kbd>Ctrl</kbd>+<kbd>A</kbd> selects all.
- **Zoom in:** press <kbd>+</kbd> or <kbd>Ctrl</kbd>+<kbd>+</kbd>.
- **Zoom out:** press <kbd>-</kbd> or <kbd>Ctrl</kbd>+<kbd>-</kbd>.
- **Change text size with the mouse:** use <kbd>Ctrl</kbd> + mouse wheel.
- While typing in **Find**, <kbd>+</kbd> and <kbd>-</kbd> enter search text.
- **Reset text size:** press <kbd>Ctrl</kbd>+<kbd>0</kbd>, or click the current zoom percentage.

Zoom controls appear in the lower-right corner when you change text size: **−**, **percentage**, and **+**. They fade away after returning to **100%**.

## Change the view

- **Formatted / source:** use the code-brackets icon.
- **Light / dark:** use the sun / moon icon.
- **Fullscreen / window:** use the fullscreen icon or <kbd>F11</kbd>.

Fullscreen controls disappear while you read. Move the pointer to bring them back.

## Go back or close

<kbd>Escape</kbd> takes one step at a time:

1. Close search, or return from Help / About.
2. If launched with a file path (including Explorer), close the viewer directly.
3. Otherwise, leave fullscreen or close the normal window.

Use <kbd>F11</kbd> whenever you want to switch between fullscreen and a normal window.

The <kbd>×</kbd> in the fullscreen toolbar closes the viewer directly.

## File actions

Open the **three-dot menu** for these options:

- **Copy file:** paste a copy into Explorer.
- **Copy file name / full path:** put that text on the clipboard.
- **Open file directory:** show the document in Explorer.
- **Edit file with…:** choose another app to edit your document.

## Make it your default reader

1. Keep the app in its permanent location.
2. Choose **Register for .md files…** from the three-dot menu.
3. Windows opens this app's default settings where supported; otherwise, find **Voltura Markdown** in Default apps. Confirm it for **.md** files.

Setup also offers **Choose as default Markdown reader**. You confirm the final choice in Windows.

## Startup and theme

In the **three-dot menu**:

- **Start fullscreen:** choose how future windows open when launched with a document. Without a document, the app starts in a normal window.
- **Use Windows theme:** follow your system's light / dark setting.
- **Automatic updates:** download updates and install on close. Off by default.
- **Manual updates:** open windows return to their documents and reading positions.
)md");
    }
    void showInformation(const wchar_t *title, const std::wstring &content) {
        if (loading)
            return;
        auto information = std::make_shared<Document>(parse(content));
        if (informationView)
            closeInformation();
        if (editor)
            editor->setVisible(false);
        size_t index = heights.values.empty() ? 0 : heights.find(scroll);
        double within = heights.values.empty() ? 0 : scroll - heights.before(index);
        informationView = std::make_unique<SavedView>(SavedView{
            doc, std::move(rawBlocks), source, scroll, horizontal, zoom, anchor, caret, index, within});
        cancelSearch();
        searching = false;
        KillTimer(hwnd, 3);
        ShowWindow(search, SW_HIDE);
        doc = std::move(information);
        positions();
        ShowWindow(canvas, SW_SHOWNA);
        source = false;
        rawBlocks.clear();
        anchor = caret = 0;
        searchMatch = false;
        notice.clear();
        resetLayout(false);
        visibility(true);
        ShowWindow(backButton, SW_SHOW);
        SetWindowTextW(hwnd, (std::wstring(title) + L" — Voltura Markdown").c_str());
        invalidateHeader();
        SetFocus(hwnd);
    }
    void closeInformation() {
        if (!informationView)
            return;
        auto savedView = std::move(informationView);
        cancelSearch();
        searching = false;
        KillTimer(hwnd, 3);
        ShowWindow(search, SW_HIDE);
        ShowWindow(backButton, SW_HIDE);
        doc = std::move(savedView->document);
        if (editor && (editor->isPreview() || editor->isLive()) && editor->previewDocument())
            doc = editor->previewDocument();
        if (editor)
            editor->setVisible(true);
        positions();
        rawBlocks = std::move(savedView->raw);
        source = savedView->source;
        zoom = savedView->zoom;
        prefs.zoom = zoom;
        updateZoomControls();
        resetLayout(false);
        scroll = savedView->scroll;
        if (doc && savedView->blockIndex < blocks().size() && device()) {
            auto &current = layout(savedView->blockIndex);
            scroll = heights.before(savedView->blockIndex) +
                     std::min(savedView->withinBlock, (double)current.height);
        }
        horizontal = savedView->horizontal;
        anchor = savedView->anchor;
        caret = savedView->caret;
        searchMatch = false;
        notice.clear();
        bars();
        SetWindowTextW(hwnd, doc ? (doc->path.filename().wstring() + L" — Voltura Markdown").c_str()
                                 : L"Voltura Markdown");
        invalidateHeader();
        invalidate();
        SetFocus(hwnd);
        if (editor) {
            editor->setVisible(true);
            editor->focus();
        }
    }
    void showSearch() {
        if (editor && !informationView) {
            editor->find();
            return;
        }
        searching = true;
        if (informationView)
            ShowWindow(backButton, SW_HIDE);
        visibility(true);
        ShowWindow(search, SW_SHOW);
        SetFocus(search);
        SendMessageW(search, EM_SETSEL, 0, -1);
        invalidateHeader();
        invalidate();
    }
    void cancelCount() {
        ++countGeneration;
        if (countCancel)
            countCancel->store(true);
        countCancel.reset();
        countStarted = countReady = countPending = false;
        searchTotal = 0;
        searchIndex = 0;
        countQuery.clear();
        if (!searchStatus.empty()) {
            float previousWidth = searchCountWidth();
            searchStatus.clear();
            if (previousWidth != searchCountWidth())
                positionSearch();
            invalidateHeader();
        }
    }
    void cancelSearch(bool resetCount = true) {
        ++findGeneration;
        KillTimer(hwnd, 3);
        if (findCancel)
            findCancel->store(true);
        if (resetCount)
            cancelCount();
    }
    void refreshSearchStatus() {
        float previousWidth = searchCountWidth();
        int64_t total = (int64_t)searchTotal;
        int64_t current = searchMatch && total ? (searchIndex % total + total) % total + 1 : 0;
        searchStatus =
            countReady ? std::to_wstring(current) + L" of " + std::to_wstring(searchTotal) : L"Counting…";
        if (previousWidth != searchCountWidth())
            positionSearch();
        invalidateHeader();
    }
    void find(bool advance, bool backwards = false) {
#ifdef VMD_TESTING
        if (!metricsFile.empty())
            findStarted = std::chrono::steady_clock::now();
#endif
        cancelSearch(false);
        if (!doc)
            return;
        int n = GetWindowTextLengthW(search);
        query.resize(n + 1);
        GetWindowTextW(search, query.data(), n + 1);
        query.resize(n);
        if (query.empty()) {
            cancelCount();
            if (notice == L"No matches")
                notice.clear();
            searchMatch = false;
            anchor = caret = 0;
            invalidate();
            return;
        }
        bool continuing = advance && searchMatch && countCancel && query == countQuery;
        uint32_t start = advance ? (backwards    ? std::min(anchor, caret)
                                    : continuing ? anchor + 1
                                                 : std::max(anchor, caret))
                                 : 0;
        if (!continuing) {
            cancelCount();
            countQuery = query;
            countCancel = std::make_shared<std::atomic_bool>(false);
#ifdef VMD_TESTING
            if (!metricsFile.empty())
                countBegan = findStarted;
#endif
            refreshSearchStatus();
        }
        auto document = doc;
        bool raw = source;
        auto q = query;
        auto gen = findGeneration;
        auto countGen = countGeneration;
        int step = continuing ? (backwards ? -1 : 1) : 0;
        findCancel = std::make_shared<std::atomic_bool>(false);
        auto cancel = findCancel;
        auto done = std::make_shared<std::atomic_bool>(false);
        HWND destination = hwnd;
        auto documentGen = generation;
        reap();
        workers.reserve(workers.size() + 1);
        auto result = std::make_unique<SearchResult>(
            SearchResult{gen, documentGen, raw, -1, (uint32_t)q.size(), step, start, backwards, countGen});
        workers.push_back({std::thread([document, raw, q, start, gen, cancel, done, destination, documentGen,
                                        backwards, step, countGen, result = std::move(result)]() mutable {
                               auto &s = raw || document->plainSource ? document->source : document->text;
                               int found = findMatch(s, q, start, backwards, *cancel);
                               if (!cancel->load()) {
                                   result->found = found;
                                   auto r = result.release();
                                   if (!PostMessageW(destination, WM_APP + 3, 0, (LPARAM)r))
                                       delete r;
                               }
                               done->store(true);
                           }),
                           cancel, done});
    }
    struct SearchResult {
        uint64_t gen, documentGen;
        bool source;
        int found;
        uint32_t length;
        int step = 0;
        uint32_t start = 0;
        bool backwards = false;
        uint64_t countGen = 0;
    };
    struct CountResult {
        uint64_t gen, documentGen;
        bool source;
        MatchCount count;
        bool backwards;
    };
    uint64_t findGeneration = 0, countGeneration = 0, searchTotal = 0;
#ifdef VMD_TESTING
    uint64_t paintedFindGeneration = 0;
    std::chrono::steady_clock::time_point findStarted, countBegan;
#endif
    int64_t searchIndex = 0;
    bool countStarted = false, countReady = false, countPending = false, countBackwards = false;
    uint32_t countStart = 0;
    std::wstring countQuery, searchStatus;
    std::shared_ptr<std::atomic_bool> findCancel, countCancel;
    void startCount(uint32_t start, bool backwards) {
        if (countStarted || !countCancel)
            return;
        countStarted = true;
        auto document = doc;
        auto q = countQuery;
        auto cancel = countCancel;
        auto done = std::make_shared<std::atomic_bool>(false);
        auto gen = countGeneration, documentGen = generation;
        bool raw = source;
        HWND destination = hwnd;
        workers.reserve(workers.size() + 1);
        auto result = std::make_unique<CountResult>(CountResult{gen, documentGen, raw, {}, backwards});
        workers.push_back({std::thread([document, q, cancel, done, gen, documentGen, raw, destination, start,
                                        backwards, result = std::move(result)]() mutable {
                               SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                               auto &s = raw || document->plainSource ? document->source : document->text;
                               auto count = countMatches(s, q, start, *cancel);
                               if (!cancel->load()) {
                                   result->count = count;
                                   auto r = result.release();
                                   if (!PostMessageW(destination, WM_APP + 5, 0, (LPARAM)r))
                                       delete r;
                               }
                               done->store(true);
                           }),
                           cancel, done});
    }
    void countResult(std::unique_ptr<CountResult> r) {
        if (r->gen != countGeneration || r->documentGen != generation || r->source != source || !countCancel)
            return;
        searchTotal = r->count.total;
        searchIndex += (int64_t)r->count.beforeStart - (r->backwards ? 1 : 0);
        countReady = true;
        refreshSearchStatus();
#ifdef VMD_TESTING
        if (!metricsFile.empty()) {
            std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
            out << "{\"search_total\":" << searchTotal << ",\"search_count_ms\":"
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - countBegan)
                       .count()
                << "}\n";
        }
#endif
    }
    void searchResult(std::unique_ptr<SearchResult> r) {
        if (r->gen != findGeneration || r->documentGen != generation || r->source != source)
            return;
        if (r->found < 0) {
            notice = L"No matches";
            searchMatch = false;
            searchTotal = 0;
            countReady = true;
            refreshSearchStatus();
            invalidate();
            return;
        }
        notice.clear();
        anchor = (uint32_t)r->found;
        caret = anchor + r->length;
        searchMatch = true;
        searchIndex += r->step;
        jump(anchor, true);
        refreshSearchStatus();
        invalidate();
        // Begin the lower-priority count only after the first highlighted frame is presented.
        if (!countStarted && !countPending) {
            countStart = r->start;
            countBackwards = r->backwards;
            countPending = true;
        }
    }
    void jump(uint32_t position, bool context = false) {
        stopScrolling();
        if (!doc || blocks().empty())
            return;
        auto &bs = blocks();
        size_t i = blockAtPosition(bs, position);
        auto &l = layout(i);
        scroll = heights.before(i);
        for (auto &p : l.parts)
            if (position >= p.start && position < p.start + p.length) {
                FLOAT x, y;
                DWRITE_HIT_TEST_METRICS m{};
                if (!p.layout) {
                    scroll += p.y;
                    break;
                }
                p.layout->HitTestTextPosition(position - p.start, FALSE, &x, &y, &m);
                scroll += p.y + y;
                if (context)
                    scroll -= std::min<double>(2 * m.height, std::max(0.f, height - top - bottom - m.height));
                if (context && i == 0 && position == bs.front().start)
                    scroll = 0; // Keep the first code line's copy header visible as well.
                horizontal = std::max(0.f, left() + p.x + x - width / 2);
                break;
            }
        bars();
        if (editor && editor->isLive())
            syncSourceToPreview();
    }
    void copy() {
        if (!doc || anchor == caret)
            return;
        size_t begin = std::min(anchor, caret),
               end = std::min<size_t>(std::max(anchor, caret), text().size());
        auto s = text().substr(begin, end - begin);
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            auto memory = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
            if (memory) {
                auto p = GlobalLock(memory);
                if (p) {
                    memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(memory);
                    if (!SetClipboardData(CF_UNICODETEXT, memory))
                        GlobalFree(memory);
                } else
                    GlobalFree(memory);
            }
            CloseClipboard();
        }
    }
    uint32_t hit(float x, float y) {
        if (!doc || blocks().empty())
            return 0;
        size_t i = heights.find(scroll + y - top);
        auto &b = blocks()[i];
        auto &l = layout(i);
        float originX = documentLeft(), originY = top + (float)(heights.before(i) - scroll);
        if (l.parts.empty())
            return b.start;
        const Part *chosen = &l.parts.front();
        float distance = 1e20f;
        for (auto &p : l.parts) {
            float dx = std::max({p.x - (x - originX), 0.f, x - originX - p.x - p.width}),
                  dy = std::max({p.y - (y - originY), 0.f, y - originY - p.y - p.height});
            float d = dx * dx + dy * dy;
            if (d < distance) {
                distance = d;
                chosen = &p;
            }
        }
        BOOL trailing, inside;
        DWRITE_HIT_TEST_METRICS m{};
        if (!chosen->layout)
            return chosen->start;
        chosen->layout->HitTestPoint(x - originX - chosen->x, y - originY - chosen->y, &trailing, &inside,
                                     &m);
        return std::min(chosen->start + chosen->length,
                        chosen->start + m.textPosition + (trailing ? m.length : 0));
    }
    void activateLink(uint32_t position) {
        if (source || !doc)
            return;
        for (auto &b : doc->blocks)
            if (position >= b.start && position <= b.start + b.length) {
                for (auto &r : b.runs)
                    if (position >= r.start && position < r.start + r.length && !r.link.empty()) {
                        if (r.link.starts_with(L"#")) {
                            auto id = r.link.substr(1);
                            std::wstring decoded(id.size() + 1, L'\0');
                            DWORD length = (DWORD)decoded.size();
                            if (SUCCEEDED(
                                    UrlUnescapeW(id.data(), decoded.data(), &length, URL_UNESCAPE_AS_UTF8))) {
                                decoded.resize(length);
                                id = std::move(decoded);
                            }
                            visitBlocks(doc->blocks, [&](const Block &target) {
                                if (target.anchor && *target.anchor == id) {
                                    jump(target.start);
                                    invalidate();
                                    return false;
                                }
                                return true;
                            });
                        } else if (!openLink(r.link, doc->path.wstring())) {
                            notice = L"This link cannot be opened by the reader.";
                            invalidate();
                        }
                        return;
                    }
            }
    }
    void key(WPARAM key) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (ctrl && key == 'N') {
            beginEdit(true);
            return;
        }
        if (ctrl && key == 'E') {
            beginEdit();
            return;
        }
        if (editor && !informationView && editor->key(key))
            return;
        if (key == VK_F1) {
            showHelp();
            return;
        }
        if (key == VK_F11) {
            toggleFullscreen();
            return;
        }
        if (key == VK_ESCAPE) {
            if (searching) {
                cancelSearch();
                searching = false;
                ShowWindow(search, SW_HIDE);
                if (informationView)
                    ShowWindow(backButton, SW_SHOW);
                SetFocus(hwnd);
                invalidateHeader();
            } else if (informationView)
                closeInformation();
            else if (fullscreen && !launchedWithFile)
                toggleFullscreen();
            else
                requestClose();
            return;
        }
        if (!ctrl && !shift && (key == VK_LEFT || key == VK_RIGHT)) {
            horizontal = std::clamp(horizontal + (key == VK_LEFT ? -54.f : 54.f) * zoom, 0.f,
                                    std::max(0.f, maxWidth - width));
            bars();
            invalidate();
            return;
        }
        if (doc && (key == VK_LEFT || key == VK_RIGHT ||
                    (shift && (key == VK_HOME || key == VK_END || key == VK_UP || key == VK_DOWN)))) {
            uint32_t next = caret;
            auto &s = text();
            if (key == VK_LEFT) {
                if (next)
                    --next;
                if (next && s[next] >= 0xdc00 && s[next] <= 0xdfff)
                    --next;
                if (ctrl)
                    while (next && iswalnum(s[next - 1]))
                        --next;
            } else if (key == VK_RIGHT) {
                if (next < s.size())
                    ++next;
                if (next < s.size() && s[next] >= 0xdc00 && s[next] <= 0xdfff)
                    ++next;
                if (ctrl)
                    while (next < s.size() && iswalnum(s[next]))
                        ++next;
            } else if (key == VK_HOME && ctrl)
                next = 0;
            else if (key == VK_END && ctrl)
                next = (uint32_t)s.size();
            else if (key == VK_HOME) {
                while (next && s[next - 1] != L'\n')
                    --next;
            } else if (key == VK_END) {
                while (next < s.size() && s[next] != L'\n')
                    ++next;
            } else {
                if (next < s.size()) {
                    jump(next);
                    auto i = heights.find(scroll);
                    auto &l = layout(i);
                    for (auto &p : l.parts)
                        if (next >= p.start && next <= p.start + p.length) {
                            FLOAT x, y;
                            DWRITE_HIT_TEST_METRICS m{};
                            if (!p.layout)
                                continue;
                            p.layout->HitTestTextPosition(next - p.start, FALSE, &x, &y, &m);
                            float originX = documentLeft();
                            next = hit(originX + p.x + x,
                                       top + (float)(heights.before(i) - scroll) + p.y + y +
                                           (key == VK_UP ? -m.height * .5f : m.height * 1.5f));
                            break;
                        }
                }
            }
            caret = next;
            if (!shift)
                anchor = caret;
            searchMatch = false;
            invalidate();
            return;
        }
        if (ctrl) {
            if (key == 'O')
                openDialog();
            else if (key == 'F')
                showSearch();
            else if (key == 'C')
                copy();
            else if (key == 'A' && doc) {
                anchor = 0;
                caret = (uint32_t)text().size();
                invalidate();
            } else if (key == 'V' && shift)
                toggleSource();
            else if (key == '0' || key == VK_NUMPAD0)
                zoomBy(1);
            else if (key == VK_OEM_PLUS || key == VK_ADD)
                zoomBy(zoom + .1f);
            else if (key == VK_OEM_MINUS || key == VK_SUBTRACT)
                zoomBy(zoom - .1f);
            else if (key == VK_HOME) {
                scroll = 0;
                bars();
                invalidate();
            } else if (key == VK_END) {
                scroll = heights.total();
                bars();
                invalidate();
            }
            return;
        }
        if (key == VK_F3) {
            if (!searching)
                showSearch();
            find(true, shift);
            return;
        }
        if (key == VK_TAB) {
            visibility(true);
            SetFocus(nextTabControl(nullptr, shift));
            return;
        }
        double delta = key == VK_NEXT || key == VK_SPACE ? height - top - bottom - 24
                       : key == VK_PRIOR                 ? -(height - top - bottom - 24)
                       : key == VK_DOWN                  ? 40
                       : key == VK_UP                    ? -40
                                                         : 0;
        if (delta)
            scrollTo((scrollAnimating ? scrollTarget : scroll) + delta);
        else if (key == VK_HOME) {
            stopScrolling();
            scroll = 0;
            bars();
            invalidate();
        } else if (key == VK_END) {
            stopScrolling();
            scroll = heights.total();
            bars();
            invalidate();
        }
    }
    void drawButton(const DRAWITEMSTRUCT &item) {
#ifdef VMD_TESTING
        if (!verifyFile.empty() && item.CtlID >= 100 && item.CtlID < 106)
            ++buttonPaints[item.CtlID - 100];
#endif
        RECT r = item.rcItem;
        HDC dc = CreateCompatibleDC(item.hDC);
        HBITMAP buffer = CreateCompatibleBitmap(item.hDC, r.right, r.bottom);
        auto previousBitmap = SelectObject(dc, buffer);
        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(item.hwndItem, &cursor);
        bool hover = PtInRect(&r, cursor), focus = (item.itemState & ODS_FOCUS) != 0;
        bool active = (item.CtlID == 102 && source) || (item.CtlID == 104 && fullscreen);
        COLORREF bg = dark ? RGB(28, 34, 45) : RGB(255, 255, 255),
                 fg = dark ? RGB(210, 221, 238) : RGB(57, 75, 98);
        if (hover || active)
            bg = dark ? RGB(46, 59, 78) : RGB(230, 237, 250);
        if (item.itemState & ODS_SELECTED)
            bg = dark ? RGB(63, 82, 112) : RGB(209, 223, 248);
        bool associationButton = item.CtlID >= 150 && item.CtlID <= 152;
        auto background = CreateSolidBrush(item.CtlID >= 140 && item.CtlID <= 142 ? RGB(255, 0, 255)
                                           : associationButton ? (dark ? RGB(28, 34, 45) : RGB(255, 255, 255))
                                           : dark              ? RGB(16, 19, 25)
                                                               : RGB(250, 251, 253));
        FillRect(dc, &r, background);
        DeleteObject(background);
        auto b = CreateSolidBrush(bg);
        auto pen = CreatePen(PS_SOLID, 1,
                             focus               ? (dark ? RGB(144, 185, 255) : RGB(39, 95, 199))
                             : associationButton ? (dark ? RGB(48, 58, 73) : RGB(220, 226, 235))
                                                 : bg);
        auto ob = SelectObject(dc, b), op = SelectObject(dc, pen);
        RoundRect(dc, 1, 1, r.right - 1, r.bottom - 1, (int)(10 * scale()), (int)(10 * scale()));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(pen);
        int cx = r.right / 2, cy = r.bottom / 2;
        float s = scale();
        auto path = [&](std::initializer_list<POINT> pts) {
            std::vector<POINT> p;
            for (auto v : pts)
                p.push_back({cx + (LONG)(v.x * s), cy + (LONG)(v.y * s)});
            Polyline(dc, p.data(), (int)p.size());
        };
        pen = CreatePen(PS_SOLID, std::max(1, (int)(1.6f * s)), fg);
        op = SelectObject(dc, pen);
        ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        switch (item.CtlID) {
        case 150:
        case 151:
        case 152: {
            wchar_t label[64]{};
            GetWindowTextW(item.hwndItem, label, 64);
            auto font = SelectObject(dc, uiFont);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(
                dc, IsWindowEnabled(item.hwndItem) ? fg : (dark ? RGB(146, 158, 175) : RGB(102, 117, 138)));
            DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, font);
            break;
        }
        case 140:
        case 142:
            path({{-6, 0}, {6, 0}});
            if (item.CtlID == 142)
                path({{0, -6}, {0, 6}});
            break;
        case 141: {
            wchar_t label[16]{};
            GetWindowTextW(item.hwndItem, label, 16);
            auto font = SelectObject(dc, uiFont);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, fg);
            DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, font);
            break;
        }
        case 100:
            path({{-9, 6}, {-9, -6}, {-2, -6}, {1, -3}, {9, -3}, {9, 7}, {-9, 7}});
            path({{-9, -1}, {9, -1}});
            break;
        case 101:
            Ellipse(dc, cx - (int)(8 * s), cy - (int)(8 * s), cx + (int)(4 * s), cy + (int)(4 * s));
            path({{2, 2}, {8, 8}});
            break;
        case 102:
            path({{-5, -6}, {-10, 0}, {-5, 6}});
            path({{5, -6}, {10, 0}, {5, 6}});
            path({{2, -9}, {-2, 9}});
            break;
        case 103:
            Ellipse(dc, cx - (int)(5 * s), cy - (int)(5 * s), cx + (int)(5 * s), cy + (int)(5 * s));
            for (int i = 0; i < 8; ++i) {
                double a = i * 3.14159265 / 4;
                POINT p[]{{cx + (LONG)(cos(a) * 8 * s), cy + (LONG)(sin(a) * 8 * s)},
                          {cx + (LONG)(cos(a) * 11 * s), cy + (LONG)(sin(a) * 11 * s)}};
                Polyline(dc, p, 2);
            }
            break;
        case 104:
            path({{-3, -8}, {-8, -8}, {-8, -3}});
            path({{3, -8}, {8, -8}, {8, -3}});
            path({{-8, 3}, {-8, 8}, {-3, 8}});
            path({{8, 3}, {8, 8}, {3, 8}});
            break;
        case 105:
            for (int x : {-6, 0, 6}) {
                auto dot = CreateSolidBrush(fg);
                SelectObject(dc, dot);
                Ellipse(dc, cx + (int)((x - 1) * s), cy - (int)s, cx + (int)((x + 1) * s) + 1,
                        cy + (int)s + 1);
                SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                DeleteObject(dot);
            }
            break;
        case 106:
            path({{5, -7}, {-3, 0}, {5, 7}});
            break;
        case 107:
            path({{-6, -6}, {6, 6}});
            path({{6, -6}, {-6, 6}});
            break;
        }
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
        BitBlt(item.hDC, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previousBitmap);
        DeleteObject(buffer);
        DeleteDC(dc);
    }
    void timer() {
        reap();
        measureAhead();
    }
    void measureAhead(bool bounded = true) {
        if (!doc || loading || !target || blocks().empty())
            return;
        auto start = std::chrono::steady_clock::now();
        size_t i = heights.find(scroll);
        double y = heights.before(i), end = scroll + 2 * (height - top);
        bool changed = false;
        for (; i < blocks().size() && y <= end; ++i) {
            if (!cache.contains(i)) {
                layout(i);
                changed = true;
            }
            y += heights.values[i];
            if (bounded && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(3))
                break;
        }
        if (changed) {
            if (editorSourceDrivesPreview)
                syncPreviewToSource();
            bars();
            invalidate();
        }
    }
#ifdef VMD_TESTING
    static LRESULT CALLBACK searchCharProbe(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                            DWORD_PTR data) {
        if (msg == WM_CHAR)
            ++*(unsigned *)data;
        return DefSubclassProc(window, msg, wp, lp);
    }
    void verifyUi() {
        auto check = [&](bool ok, const char *name) {
            checksPassed &= ok;
            std::ofstream out(std::filesystem::path(verifyFile), std::ios::app);
            out << "{\"check\":\"" << name << "\",\"passed\":" << (ok ? "true" : "false") << "}\n";
        };
        switch (verifyStage++) {
        case 0:
            check(doc && !source && !loading, "formatted_default");
            for (size_t i = 0; i < blocks().size(); ++i) {
                auto &b = blocks()[i];
                if (b.kind == Kind::Table && doc->path.filename() == L"markdown-table-images.md") {
                    unsigned expectedImages = 0;
                    bool waiting = false;
                    for (const auto &cell : b.cells)
                        if (cell.content)
                            visitBlocks(*cell.content, [&](const Block &child) {
                                if (child.image) {
                                    ++expectedImages;
                                    waiting |= !images->get(child.image->source(dark), doc->path.wstring());
                                }
                                return true;
                            });
                    if (waiting) {
                        --verifyStage;
                        return;
                    }
                    auto &rendered = layout(i);
                    unsigned bitmaps = 0;
                    bool links = true;
                    double savedScroll = scroll;
                    scroll = heights.before(i);
                    for (const auto &part : rendered.parts)
                        if (part.bitmap) {
                            ++bitmaps;
                            links &= linkAt(documentLeft() + part.x + part.width / 2,
                                            top + part.y + part.height / 2) == L"https://example.com/table";
                        }
                    scroll = savedScroll;
                    check(expectedImages == 1 && bitmaps == expectedImages,
                          "markdown_table_images_render_inside_their_rows");
                    check(links, "markdown_table_image_links_hit_their_destinations");
                }
                if (b.kind == Kind::Heading && text().substr(b.start, b.length) == L"Statistics") {
                    auto &heading = layout(i);
                    bool adjacent = i + 1 < blocks().size() && blocks()[i + 1].image;
                    check(adjacent, "statistics_badges_immediately_follow_heading");
                    if (adjacent) {
                        auto &badges = layout(i + 1);
                        float firstY = badges.parts.empty() ? 0.f : badges.parts.front().y;
                        check(heading.parts.size() == 1 && firstY == 0.f &&
                                  std::abs(heading.height - heading.parts.front().height - 14 * zoom) < .01f,
                              "statistics_badges_have_only_normal_heading_spacing");
                    }
                }
                if (b.image && b.image->inlineImages && doc->path.filename() == L"inline-images.md") {
                    for (auto &child : *b.image->inlineImages)
                        if (!images->get(child.image->source(dark), doc->path.wstring())) {
                            --verifyStage;
                            return;
                        }
                    auto wide = nativeLayout(b, 1200);
                    auto narrow = nativeLayout(b, 240);
                    check(wide.parts.size() == 7 &&
                              std::all_of(wide.parts.begin(), wide.parts.end(),
                                          [](const Part &p) { return p.bitmap && p.y == 0; }),
                          "linked_badges_share_one_row_when_they_fit");
                    check(narrow.parts.size() == 7 && narrow.parts.back().y > 0 &&
                              std::all_of(
                                  narrow.parts.begin(), narrow.parts.end(),
                                  [](const Part &p) { return p.bitmap && p.x >= 0 && p.x + p.width <= 240; }),
                          "linked_badges_wrap_within_available_width");
                    auto &rendered = layout(i);
                    double savedScroll = scroll;
                    scroll = heights.before(i);
                    bool links = rendered.parts.size() == b.image->inlineImages->size();
                    for (size_t badge = 0; links && badge < rendered.parts.size(); ++badge) {
                        auto &part = rendered.parts[badge];
                        auto &child = (*b.image->inlineImages)[badge];
                        links &= !child.runs.empty() &&
                                 linkAt(documentLeft() + part.x + part.width / 2,
                                        top + part.y + part.height / 2) == child.runs.front().link;
                    }
                    check(links, "each_inline_badge_hits_its_own_link");
                    scroll = savedScroll;
                    float savedZoom = zoom, savedDpi = dpi;
                    bool sharp = true, waiting = false;
                    for (float testDpi : {96.f, 120.f, 144.f, 192.f}) {
                        for (float factor : {.75f, 1.f, 1.5f, 2.f}) {
                            dpi = testDpi;
                            zoom = factor;
                            auto badge = nativeLayout(b.image->inlineImages->front(), 1200);
                            waiting |= badge.imagePending;
                            if (badge.bitmap) {
                                auto pixels = badge.bitmap->GetPixelSize();
                                sharp &= pixels.width >= (badge.width - badge.left) * dpi / 96.f &&
                                         pixels.height >= (badge.height - 18 * zoom) * dpi / 96.f;
                            } else
                                sharp = false;
                        }
                    }
                    zoom = savedZoom;
                    dpi = savedDpi;
                    if (waiting) {
                        --verifyStage;
                        return;
                    }
                    check(sharp, "svg_badge_bitmap_covers_dpi_and_zoom_without_changing_layout_size");
                }
                if (b.image && (!b.image->lightUrl.empty() || !b.image->darkUrl.empty())) {
                    auto image = images->get(b.image->source(dark), doc->path.wstring());
                    if (!image) {
                        --verifyStage;
                        return;
                    }
                    auto &l = layout(i);
                    check(image->ready && l.bitmap, "picture_selected_source_decodes_and_renders");
                }
                if (b.kind != Kind::Image || !b.image || !b.image->width || !b.image->height)
                    continue;
                auto image = images->get(b.image->source(dark), doc->path.wstring());
                if (!image) {
                    --verifyStage;
                    return;
                }
                float savedZoom = zoom;
                for (float factor : {.5f, 1.f, 2.f}) {
                    zoom = factor;
                    clearLayouts();
                    auto &l = layout(i);
                    auto bounds = blockBounds(i, l, 0);
                    float fit =
                        std::min(1.f, std::max(80.f, contentWidth() - l.left) / (b.image->width * zoom));
                    check(l.bitmap &&
                              std::abs(bounds.right - bounds.left - b.image->width * zoom * fit) < .01f &&
                              std::abs(bounds.bottom - bounds.top - b.image->height * zoom * fit) < .01f,
                          "html_image_bitmap_bounds_respect_dimensions_and_zoom");
                }
                zoom = savedZoom;
                resetLayout(false);
            }
            {
                float right = 0;
                clearLayouts();
                for (size_t i = 0; i < std::min<size_t>(64, blocks().size()); ++i)
                    right = std::max(right, left() + layout(i).width);
                bars();
                check(right > width || maxWidth <= width,
                      "fitting_document_does_not_scroll_empty_right_margin");
            }
            {
                auto popup = createMenu();
                check(GetMenuItemCount(popup) == 20, "file_operations_menu_items");
                auto automatic = std::find_if(menuRows.begin(), menuRows.end(),
                                              [](const MenuRow &row) { return row.id == 126; });
                check(automatic != menuRows.end() && automatic->text == L"Automatic updates" &&
                          !automatic->checked,
                      "automatic_updates_default_unchecked");
                bool rowsValid = true;
                for (auto &row : menuRows) {
                    MEASUREITEMSTRUCT measure{};
                    measure.itemData = (ULONG_PTR)&row;
                    measureMenu(measure);
                    rowsValid &= measure.itemHeight > 0;
                    if (row.id >= 130 && row.id <= 134)
                        rowsValid &= row.enabled;
                }
                check(rowsValid, "menu_dpi_measurements_and_enabled_file_actions");
                DestroyMenu(popup);
                command(126);
                popup = createMenu();
                automatic = std::find_if(menuRows.begin(), menuRows.end(),
                                         [](const MenuRow &row) { return row.id == 126; });
                check(prefs.automaticUpdates && automatic != menuRows.end() && automatic->checked,
                      "automatic_updates_menu_toggles_on");
                DestroyMenu(popup);
                command(126);
                check(!prefs.automaticUpdates && !updateTimerStarted,
                      "automatic_updates_unticking_cancels_schedule");
                auto current = doc;
                doc.reset();
                popup = createMenu();
                bool disabled = true;
                for (auto &row : menuRows)
                    if (row.id >= 130 && row.id <= 134)
                        disabled &= !row.enabled;
                check(disabled, "file_actions_disabled_without_document");
                DestroyMenu(popup);
                ValidateRect(canvas, nullptr);
                message(WM_SIZE, SIZE_RESTORED, 0);
                check(GetUpdateRect(canvas, nullptr, FALSE) != FALSE,
                      "empty_view_resize_requests_document_repaint");
                doc = std::move(current);
                resetLayout();
            }
            for (size_t i = 0; i < blocks().size(); ++i) {
                auto &row = blocks()[i];
                if (row.kind != Kind::Table || std::none_of(row.cells.begin(), row.cells.end(),
                                                            [](const Cell &cell) { return !!cell.content; }))
                    continue;
                size_t readyImages = 0;
                bool pending = false;
                std::function<void(const Block &)> inspect = [&](const Block &child) {
                    if (child.image) {
                        auto image = images->get(child.image->source(dark), doc->path.wstring());
                        pending |= !image;
                        readyImages += image && image->ready ? 1 : 0;
                    }
                    for (auto &cell : child.cells)
                        if (cell.content)
                            for (auto &nested : *cell.content)
                                inspect(nested);
                };
                inspect(row);
                if (pending) {
                    --verifyStage;
                    return;
                }
                auto &rendered = layout(i);
                auto bitmaps = std::count_if(rendered.parts.begin(), rendered.parts.end(),
                                             [](const Part &part) { return !!part.bitmap; });
                check((size_t)bitmaps == readyImages, "html_cell_images_render_inside_their_table");
                check(rendered.cellEdges.size() == row.cells.size(), "html_table_keeps_cell_boundaries");
                bool contained = true;
                for (auto &part : rendered.parts)
                    contained &= part.x >= rendered.left - .1f &&
                                 part.x + part.width <= rendered.width + .1f && part.y >= 0 &&
                                 part.y + part.height <= rendered.height + .1f;
                check(contained, "html_nested_content_stays_inside_table_bounds");
                if (row.table && row.cells.size() <= 256) {
                    bool geometry = rendered.tableBorders.size() >= row.cells.size() + 1;
                    auto first = rendered.tableBorders.size() - row.cells.size() - 1;
                    for (size_t a = 0; geometry && a < row.cells.size(); ++a) {
                        auto rect = rendered.tableBorders[first + a];
                        geometry &= rect.left >= rendered.left - .1f && rect.right <= rendered.width + .1f &&
                                    rect.top >= 0 && rect.bottom <= rendered.height + .1f;
                        for (size_t b = a + 1; b < row.cells.size(); ++b) {
                            auto other = rendered.tableBorders[first + b];
                            if (row.cells[a].row == row.cells[b].row)
                                geometry &= std::abs(rect.top - other.top) < .1f;
                            if (row.cells[a].column == row.cells[b].column)
                                geometry &= std::abs(rect.left - other.left) < .1f;
                            if (!row.table->overlap)
                                geometry &=
                                    std::min(rect.right, other.right) - std::max(rect.left, other.left) <
                                        .1f ||
                                    std::min(rect.bottom, other.bottom) - std::max(rect.top, other.top) < .1f;
                        }
                    }
                    check(geometry, "html_spans_share_grid_boundaries_without_internal_cell_overlap");
                    if (geometry) {
                        float footerTop = 1e20f, bodyBottom = 0;
                        for (size_t a = 0; a < row.cells.size(); ++a) {
                            auto &cell = row.cells[a];
                            auto group =
                                std::find_if(row.table->groups.begin(), row.table->groups.end(),
                                             [&](const TableRowGroup &g) {
                                                 return cell.row >= g.first && cell.row < g.first + g.rows;
                                             });
                            auto rect = rendered.tableBorders[first + a];
                            if (group != row.table->groups.end() && group->section == TableSection::Foot)
                                footerTop = std::min(footerTop, rect.top);
                            else
                                bodyBottom = std::max(bodyBottom, rect.bottom);
                        }
                        if (footerTop < 1e20f)
                            check(footerTop >= bodyBottom - .1f, "html_footer_renders_below_body");
                    }
                }
            }
            check(fullscreen, "fullscreen_default");
            if (fullscreen)
                toggleFullscreen();
            check(!(GetWindowLongW(hwnd, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL)),
                  "no_native_scrollbar_resize_cycle");
            check(IsWindowVisible(buttons[0]) != FALSE, "toolbar_visible");
            {
                RECT originalBounds;
                GetWindowRect(hwnd, &originalBounds);
                auto originalDpi = dpi;
                auto originalDocument = doc;
                auto originalTheme = dark;
                auto originalAnchor = anchor, originalCaret = caret;
                anchor = 1;
                caret = 3;
                for (bool full : {false, true}) {
                    if (fullscreen != full)
                        toggleFullscreen();
                    for (UINT testDpi : {96u, 120u, 144u, 192u, 216u, 288u, 96u}) {
                        LONG x = testDpi == 144 ? -1280 : 80;
                        RECT suggested{x, 80, x + 1200, 900};
                        auto previousUpdates = viewportUpdates;
                        SendMessageW(hwnd, WM_DPICHANGED, MAKELONG(testDpi, testDpi), (LPARAM)&suggested);
                        RECT client, control, zoomBounds;
                        GetClientRect(hwnd, &client);
                        GetWindowRect(buttons[0], &control);
                        GetWindowRect(zoomPanel, &zoomBounds);
                        POINT zoomPoint{zoomBounds.right, zoomBounds.bottom};
                        ScreenToClient(hwnd, &zoomPoint);
                        float targetX = 0, targetY = 0;
                        if (target)
                            target->GetDpi(&targetX, &targetY);
                        check(dpi == testDpi && viewportUpdates > previousUpdates &&
                                  std::abs(width - (client.right / scale() - 12)) < .01f &&
                                  control.right - control.left == (int)(34 * scale()) &&
                                  zoomPoint.x < client.right && zoomPoint.y < client.bottom &&
                                  (!target || (targetX == testDpi && targetY == testDpi)),
                              full ? "simulated_fullscreen_dpi_geometry" : "simulated_windowed_dpi_geometry");
                        check(doc == originalDocument && anchor == 1 && caret == 3 && dark == originalTheme,
                              "simulated_dpi_preserves_document_selection_and_theme");
                        RECT searchBounds;
                        GetWindowRect(search, &searchBounds);
                        MapWindowPoints(nullptr, hwnd, (POINT *)&searchBounds, 2);
                        check(searchBounds.left >= 28 * scale() &&
                                  searchBounds.right <=
                                      (28 + searchBoxWidth() - searchCountWidth()) * scale() &&
                                  searchBounds.top >= 20 * scale() && searchBounds.bottom <= 54 * scale(),
                              "search_edit_right_edge_stays_inside_rounded_field_at_each_dpi");
                        double sum = 0;
                        bool indexValid = true;
                        for (size_t i = 0; i < heights.values.size(); ++i) {
                            indexValid &= std::abs(heights.before(i) - sum) < .001;
                            sum += heights.values[i];
                        }
                        check(indexValid && std::abs(heights.total() - sum) < .001,
                              "linear_height_index_matches_prefix_sums_after_dpi_change");
                    }
                }
                if (fullscreen)
                    toggleFullscreen();
                SendMessageW(hwnd, WM_DPICHANGED, MAKELONG((UINT)originalDpi, (UINT)originalDpi),
                             (LPARAM)&originalBounds);
                anchor = originalAnchor;
                caret = originalCaret;
            }

            command(102);
            break;
        case 1:
            check(source && text() == doc->source, "exact_readonly_source");
            SendMessageW(hwnd, WM_CHAR, 'X', 0);
            check(text() == doc->source, "typing_does_not_edit");
            command(102);
            command(103);
            break;
        case 2:
            check(!source && dark == (prefs.theme == 1), "view_and_theme_toggle");
            for (size_t i = 0; i < blocks().size(); ++i) {
                auto &b = blocks()[i];
                if (b.image && (!b.image->lightUrl.empty() || !b.image->darkUrl.empty())) {
                    auto image = images->get(b.image->source(dark), doc->path.wstring());
                    if (!image) {
                        --verifyStage;
                        return;
                    }
                    check(image->ready && layout(i).bitmap, "picture_renders_after_theme_switch");
                }
            }
            check((COLORREF)SendMessageW(tooltip, TTM_GETTIPBKCOLOR, 0, 0) ==
                          (dark ? RGB(28, 34, 45) : RGB(255, 255, 255)) &&
                      (COLORREF)SendMessageW(tooltip, TTM_GETTIPTEXTCOLOR, 0, 0) ==
                          (dark ? RGB(229, 233, 240) : RGB(32, 41, 56)) &&
                      (HFONT)SendMessageW(tooltip, WM_GETFONT, 0, 0) == uiFont,
                  "tooltip_uses_current_theme_and_dpi_font");
            {
                auto original = doc;
                bool originalTheme = dark;
                doc = std::make_shared<Document>(
                    parse(L"[Example link](https://example.com/path) plain <u>underlined</u>\n"));
                for (bool theme : {false, true}) {
                    dark = theme;
                    resetLayout(false);
                    scroll = 0;
                    horizontal = 0;
                    auto &part = layout(0).parts.front();
                    BOOL underlined = TRUE, explicitUnderline = FALSE;
                    part.layout->GetUnderline(0, &underlined);
                    part.layout->GetUnderline((UINT32)doc->text.find(L"underlined"), &explicitUnderline);
                    check(!underlined && explicitUnderline,
                          theme ? "dark_links_have_no_underline_and_preserve_explicit_underline"
                                : "light_links_have_no_underline_and_preserve_explicit_underline");
                    FLOAT x = 0, y = 0;
                    DWRITE_HIT_TEST_METRICS metrics{};
                    part.layout->HitTestTextPosition(0, FALSE, &x, &y, &metrics);
                    x += documentLeft() + part.x + metrics.width / 2;
                    y += top + part.y + metrics.height / 2;
                    check(linkAt(x, y) == L"https://example.com/path" &&
                              documentCursor(x, y) == LoadCursorW(nullptr, IDC_HAND) &&
                              linkAt(width - 2, y).empty(),
                          theme ? "dark_link_hover_hits_glyphs_only" : "light_link_hover_hits_glyphs_only");
                    hoverLink(linkAt(x, y));
                    check(hoveredLink == L"https://example.com/path", "link_hover_shows_destination");
                    invalidate();
                    UpdateWindow(canvas);
                    canvasProc(canvas, WM_MOUSELEAVE, 0, 0);
                    check(hoveredLink.empty(), "link_destination_clears_on_mouse_leave");
                    source = true;
                    check(linkAt(x, y).empty(), "source_view_does_not_preview_links");
                    source = false;
                }
                dark = originalTheme;
                doc = original;
                resetLayout(false);
            }
            {
                auto original = doc;
                const double originalScroll = 10;
                doc =
                    std::make_shared<Document>(parse(L"<table><tr><td>Before<hr>After</td></tr></table>\n"));
                resetLayout(false);
                auto &ruledCell = layout(0);
                check(std::count_if(ruledCell.parts.begin(), ruledCell.parts.end(),
                                    [](const Part &p) { return !p.layout && !p.bitmap && p.width > 0; }) == 1,
                      "rich_cell_retains_horizontal_rule_geometry");
                std::wstring mappedSource;
                for (unsigned line = 0; line < 300; ++line)
                    mappedSource += L"Paragraph.\n\n";
                mappedSource += std::wstring(6000, L'x');
                doc = std::make_shared<Document>(parse(mappedSource, nullptr, 0xffffffff, true));
                resetLayout(false);
                auto lastBlock = doc->blocks.size() - 1;
                layout(lastBlock);
                scroll = heights.before(lastBlock) + heights.values[lastBlock] / 2;
                double mappedPosition = previewSourcePosition();
                doc->source.clear();
                for (wchar_t c : mappedSource) {
                    if (c == L'\n')
                        doc->source += L'\r';
                    doc->source += c;
                }
                check(previewSourcePosition() == mappedPosition &&
                          doc->sourceMapLength == mappedSource.size(),
                      "saved_crlf_source_preserves_editor_coordinate_mapping");
                doc = std::make_shared<Document>(
                    parse(L"Normal <sub>Windows host pairing screen</sub> after\n"));
                resetLayout(false);
                auto &caption = layout(0).parts.front();
                float normalSize = 0, captionSize = 0, afterSize = 0;
                caption.layout->GetFontSize(0, &normalSize);
                caption.layout->GetFontSize(7, &captionSize);
                caption.layout->GetFontSize((UINT32)doc->text.find(L"after"), &afterSize);
                check(std::abs(captionSize - normalSize * .75f) < .01f && afterSize == normalSize,
                      "sub_caption_is_smaller_and_restores_following_text_size");
                doc = original;
                clearLayouts();
                auto &readableBlock = plainLayout(0);
                uint32_t readableCharacters = 0;
                for (auto &part : readableBlock.parts)
                    readableCharacters += part.length;
                check(readableBlock.plain && !source && readableCharacters == doc->blocks.front().length,
                      "local_plain_fallback_preserves_formatted_view_and_block_text");
                auto tableFallback = std::make_shared<Document>();
                tableFallback->text = L"first omitted second";
                Block tableBlock;
                tableBlock.kind = Kind::Table;
                tableBlock.length = (uint32_t)tableFallback->text.size();
                tableBlock.cells = {{0, 5}, {14, 6}};
                tableFallback->blocks.push_back(std::move(tableBlock));
                doc = tableFallback;
                resetLayout(false);
                auto &readableTable = plainLayout(0);
                check(readableTable.parts.size() == 2 && readableTable.parts[0].start == 0 &&
                          readableTable.parts[0].length == 5 && readableTable.parts[1].start == 14 &&
                          readableTable.parts[1].length == 6,
                      "table_fallback_preserves_disjoint_cell_ranges");
                auto emergency = std::make_shared<Document>();
                emergency->source = L"# Original title\n\nComplete readable content.";
                emergency->plainSource = true;
                emergency->blocks = sourceBlocks(emergency->source);
                for (auto &block : emergency->blocks)
                    block.kind = Kind::Paragraph;
                doc = emergency;
                resetLayout(false);
                check(!source && text() == emergency->source && !layout(0).parts.empty(),
                      "parser_fallback_stays_in_reading_view_with_complete_text");
                doc = original;
                resetLayout(false);
                {
                    float savedWidth = width, savedZoom = zoom;
                    width = 900;
                    zoom = 1;
                    doc = std::make_shared<Document>(
                        parse(L"Full installer: `VolturaAir-Setup-<version>-win-x64-full.exe` includes "
                              L"runtimes.\n"));
                    resetLayout(false);
                    auto &inlinePart = layout(0).parts.front();
                    check(inlinePart.inlineCode.size() == 1 && inlinePart.keycaps.empty(),
                          "single_backtick_code_has_one_background_inside_prose");
                    auto originalText = doc->text;
                    source = true;
                    resetLayout(false);
                    check(layout(0).parts.front().inlineCode.empty(),
                          "source_view_has_no_inline_code_background");
                    source = false;
                    width = 320;
                    zoom = 1.5f;
                    resetLayout(false);
                    auto &wrappedInline = layout(0).parts.front();
                    bool bounded = wrappedInline.inlineCode.size() > 1;
                    for (auto rect : wrappedInline.inlineCode)
                        bounded &= rect.left >= -.01f && rect.right <= wrappedInline.width + .1f &&
                                   rect.top >= 0 && rect.bottom <= wrappedInline.height + .1f;
                    check(bounded && doc->text == originalText,
                          "inline_code_background_wraps_at_zoom_without_changing_text");
                    width = 900;
                    zoom = 1;
                    doc = std::make_shared<Document>(
                        parse(L"```typescript\nconst value = 42; // sample\n```\n"));
                    resetLayout(false);
                    check(layout(0).parts.front().inlineCode.empty(),
                          "fenced_code_keeps_its_existing_block_background");
                    resetLayout(false);
                    invalidate();
                    UpdateWindow(canvas);
                    check(codeButtons.size() == 1 && !layout(0).parts.front().codeTokens.empty(),
                          "code_copy_button_and_syntax_tokens_render");
                    if (!codeButtons.empty()) {
                        auto r = codeButtons.front().bounds;
                        float x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
                        check(documentCursor(x, y) == LoadCursorW(nullptr, IDC_HAND),
                              "code_copy_hand_cursor");
                        auto point = MAKELPARAM((int)((x + canvasLeft) * scale()), (int)(y * scale()));
                        message(WM_MOUSEMOVE, 0, point);
                        message(WM_LBUTTONDOWN, 0, point);
                        message(WM_LBUTTONUP, 0, point);
                        bool exact = false;
                        if (OpenClipboard(hwnd)) {
                            if (auto data = GetClipboardData(CF_UNICODETEXT)) {
                                if (auto value = (const wchar_t *)GlobalLock(data)) {
                                    exact = std::wstring_view(value) == codeText(0);
                                    GlobalUnlock(data);
                                }
                            }
                            CloseClipboard();
                        }
                        check(exact && copiedCode == 0 && !dragging,
                              "code_button_copies_exact_text_and_confirms");
                        source = true;
                        check(codeButtonAt(x, y) == SIZE_MAX, "source_view_has_no_code_copy_hit_target");
                        source = false;
                        canvasProc(canvas, WM_MOUSELEAVE, 0, 0);
                        check(codePointerX == -1, "code_copy_tooltip_clears_on_leave");
                    }
                    width = 1700;
                    zoom = 1;
                    doc = std::make_shared<Document>(parse(L"```\n" + std::wstring(114, L'x') + L"\n```\n"));
                    resetLayout(false);
                    float right = left() + layout(0).width;
                    bars();
                    check(right > width - left() && right < width && maxWidth <= width,
                          "code_fitting_in_centering_margin_needs_no_horizontal_scrollbar");
                    for (float factor : {1.f, 2.f, 3.f}) {
                        width = 900;
                        zoom = factor;
                        resetLayout(false);
                        auto &code = layout(0);
                        DWRITE_TEXT_METRICS metrics{};
                        code.parts.front().layout->GetMetrics(&metrics);
                        float textRight = code.parts.front().x + metrics.widthIncludingTrailingWhitespace;
                        check(code.width >= contentWidth() && code.width >= textRight + 14 * zoom,
                              "code_background_contains_unwrapped_text_and_zoomed_padding");
                        horizontal = maxWidth;
                        bars();
                        check(documentLeft() + code.width <= width + .01f,
                              "code_right_padding_is_reachable_by_horizontal_scrolling");
                    }
                    width = savedWidth;
                    zoom = savedZoom;
                    doc = original;
                    resetLayout(false);
                }
                {
                    float savedZoom = zoom;
                    for (float factor : {.5f, 1.f, 2.f, 3.f}) {
                        zoom = factor;
                        doc = std::make_shared<Document>(
                            parse(L"> | A | B |\n> | --- | --- |\n> | first | second |\n\n"
                                  L"> ![Missing image](missing-layout-test.png)\n"));
                        resetLayout(false);
                        auto &table = layout(0);
                        auto tableBounds = blockBounds(0, table, 0);
                        check(std::abs(tableBounds.left - (documentLeft() + table.parts[0].x - 10 * zoom)) <
                                  .01f,
                              "nested_table_background_aligns_with_column_borders");
                        size_t imageIndex = 0;
                        while (imageIndex < blocks().size() && blocks()[imageIndex].kind != Kind::Image)
                            ++imageIndex;
                        check(imageIndex < blocks().size(), "quoted_image_fixture_contains_image");
                        if (imageIndex == blocks().size())
                            throw std::runtime_error("Image fixture missing");
                        auto &image = layout(imageIndex);
                        auto imageBounds = blockBounds(imageIndex, image, 0);
                        check(std::abs(imageBounds.left - documentLeft() - 18 * zoom) < .01f,
                              "quoted_image_respects_text_indentation");
                        auto labelBounds = imageLabelBounds(imageBounds);
                        check(std::abs(labelBounds.left - imageBounds.left - 14 * zoom) < .01f &&
                                  std::abs(labelBounds.top - imageBounds.top - 12 * zoom) < .01f,
                              "image_placeholder_label_scales_with_document_zoom");
                        auto narrowLabel = imageLabelBounds(D2D1::RectF(0, 0, 80, 48 * zoom));
                        check(narrowLabel.right > narrowLabel.left,
                              "narrow_image_placeholder_retains_positive_label_width");
                        std::wstring code = L"```\n";
                        for (int line = 0; line < 130; ++line)
                            code += std::wstring(63, L'x') + L"\n";
                        code += std::wstring(120, L'y') + L"\n```\n";
                        doc = std::make_shared<Document>(parse(std::move(code)));
                        resetLayout(false);
                        for (size_t n = 0; n < blocks().size(); ++n)
                            layout(n);
                        auto first = blockBounds(0, layout(0), 0);
                        auto second = blockBounds(1, layout(1), layout(0).height);
                        check(std::abs(first.right - second.right) < .01f,
                              "segmented_code_background_has_consistent_right_edge");
                        check(std::abs(first.bottom - second.top) < .01f,
                              "segmented_code_background_has_no_internal_gap");
                        check(codeText(0) == std::wstring_view(doc->text).substr(
                                                 0, doc->blocks.back().start + doc->blocks.back().length),
                              "code_copy_includes_all_segmented_parts");
                    }
                    zoom = savedZoom;
                    doc = original;
                    resetLayout(false);
                }
                std::wstring wideTable = L"|";
                for (int column = 0; column < 16; ++column)
                    wideTable += L" column " + std::to_wstring(column) + L" |";
                wideTable += L"\n|";
                for (int column = 0; column < 16; ++column)
                    wideTable += L" --- |";
                wideTable += L"\n";
                doc = std::make_shared<Document>(parse(std::move(wideTable)));
                resetLayout(false);
                jump(doc->blocks.front().cells.back().start);
                check(horizontal > 0, "search_reveals_match_in_horizontally_scrolled_table");
                auto arrow = LoadCursorW(nullptr, IDC_ARROW), ibeam = LoadCursorW(nullptr, IDC_IBEAM);
                check(scrollbarAt(30, height + 6) == 2 && documentCursor(30, height + 6) == arrow,
                      "horizontal_scrollbar_uses_arrow_cursor");
                check(documentCursor(30, top + 20) == ibeam, "document_text_keeps_selection_cursor");
                scrollDrag = 2;
                check(documentCursor(30, top + 20) == arrow, "scrollbar_drag_keeps_arrow_outside_track");
                scrollDrag = 0;
                horizontal = 0;
                auto savedCaret = caret;
                key(VK_RIGHT);
                check(horizontal > 0 && caret == savedCaret, "right_arrow_scrolls_wide_content");
                key(VK_LEFT);
                check(horizontal == 0, "left_arrow_scrolls_back_and_clamps_at_start");
                for (int n = 0; n < 100; ++n)
                    key(VK_RIGHT);
                check(horizontal <= std::max(0.f, maxWidth - width),
                      "horizontal_arrow_scrolling_clamps_at_end");
                doc = std::make_shared<Document>(
                    parse(L"Paragraph before the table.\n\n| A | B |\n| --- | --- |\n| first | second |\n"));
                resetLayout(false);
                layout(0);
                layout(1);
                maxWidth = width + 100;
                float originalLeft = documentLeft();
                key(VK_RIGHT);
                check(std::abs(documentLeft() - (originalLeft - horizontal)) < .001f,
                      "paragraphs_and_tables_share_horizontal_document_origin");
                const auto &paragraphPart = layout(0).parts.front();
                FLOAT hitX, hitY;
                DWRITE_HIT_TEST_METRICS hitMetrics{};
                paragraphPart.layout->HitTestTextPosition(8, FALSE, &hitX, &hitY, &hitMetrics);
                check(hit(documentLeft() + paragraphPart.x + hitX + hitMetrics.width * .25f,
                          top + paragraphPart.y + hitY + hitMetrics.height * .5f) == paragraphPart.start + 8,
                      "paragraph_selection_tracks_horizontal_scroll");
                jump(paragraphPart.start);
                check(horizontal == 0, "search_reveals_paragraph_after_horizontal_scroll");
                doc = std::make_shared<Document>(
                    parse(L"| A | B |\n| --- | --- |\n| first | second |\n| last | row |\n"));
                resetLayout(false);
                check(layout(0).tableGap == 0 && layout(1).tableGap == 0 && layout(2).tableGap == 14 * zoom,
                      "table_rows_join_continuously_with_only_a_final_margin");
                doc = original;
                resetLayout(false);
                scroll = originalScroll;
                showAbout();
                check(informationView && !source && doc->path.empty() &&
                          doc->text.find(L"Open-source software") != std::wstring::npos,
                      "about_is_formatted_themed_document");
                check(heights.total() > height - top - bottom && IsWindowVisible(backButton),
                      "about_has_scrollbar_and_back_control");
                key(VK_ESCAPE);
                check(!informationView && doc == original &&
                          std::abs(scroll -
                                   std::clamp(originalScroll, 0.,
                                              std::max(0., heights.total() - (height - top - bottom)))) < 1,
                      "about_escape_restores_document_and_position");
                key(VK_F1);
                check(informationView && !source && doc->path.empty() &&
                          doc->text.find(L"Using Voltura Markdown") != std::wstring::npos &&
                          doc->text.find(L"Find, copy and zoom") != std::wstring::npos &&
                          IsWindowVisible(backButton),
                      "f1_opens_formatted_help_with_back_control");
                bool keycapGeometry = false;
                for (size_t i = 0; i < blocks().size() && !keycapGeometry; ++i)
                    for (auto &part : layout(i).parts)
                        keycapGeometry |= !part.keycaps.empty();
                check(keycapGeometry && doc->text.find(L"<kbd>") == std::wstring::npos,
                      "help_keyboard_shortcuts_have_cached_keycap_geometry");
                check(doc->text.find(L"Zoom in: press +") != std::wstring::npos &&
                          doc->text.find(L"Zoom out: press -") != std::wstring::npos,
                      "help_documents_plain_plus_and_minus_shortcuts");
                zoomBy(1);
                SendMessageW(hwnd, WM_CHAR, L'+', 1);
                check(std::abs(zoom - 1.1f) < .001f, "help_zoom_character_handled");
                showAbout();
                key(VK_ESCAPE);
                check(!informationView && doc == original &&
                          std::abs(scroll -
                                   std::clamp(originalScroll, 0.,
                                              std::max(0., heights.total() - (height - top - bottom)))) < 1,
                      "switching_help_to_about_preserves_original_reading_position");
            }
            {
                // Exercise the same translation and dispatch sequence as the message loop.
                // Modifier state is local to this test thread and restored after every input.
                auto press = [&](HWND window, UINT vk, bool ctrl = false, bool shift = false,
                                 bool alt = false) {
                    BYTE savedKeys[256]{}, state[256]{};
                    GetKeyboardState(savedKeys);
                    state[VK_CONTROL] = ctrl ? 0x80 : 0;
                    state[VK_SHIFT] = shift ? 0x80 : 0;
                    state[VK_MENU] = alt ? 0x80 : 0;
                    SetKeyboardState(state);
                    MSG input{};
                    input.hwnd = window;
                    input.message = WM_KEYDOWN;
                    input.wParam = vk;
                    input.lParam = 1 | (MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
                    TranslateMessage(&input);
                    DispatchMessageW(&input);
                    MSG character{};
                    while (PeekMessageW(&character, window, WM_CHAR, WM_CHAR, PM_REMOVE))
                        DispatchMessageW(&character);
                    SetKeyboardState(savedKeys);
                };
                zoomBy(1);
                for (wchar_t symbol : {L'+', L'-'}) {
                    SHORT mapping = VkKeyScanW(symbol);
                    check(mapping != -1 && !(HIBYTE(mapping) & 6), "zoom_symbol_has_keyboard_mapping");
                    if (mapping == -1 || (HIBYTE(mapping) & 6))
                        continue;
                    press(hwnd, LOBYTE(mapping), false, (HIBYTE(mapping) & 1) != 0);
                    check(std::abs(zoom - (symbol == L'+' ? 1.1f : 1.f)) < .001f,
                          symbol == L'+' ? "translated_main_keyboard_plus"
                                         : "translated_main_keyboard_minus");
                }
                press(hwnd, VK_ADD);
                check(std::abs(zoom - 1.1f) < .001f, "numeric_keypad_plus");
                press(hwnd, VK_SUBTRACT);
                check(zoom == 1.f, "numeric_keypad_minus");
                press(hwnd, VK_OEM_PLUS, true, true);
                check(std::abs(zoom - 1.1f) < .001f, "ctrl_plus_zooms_exactly_once");
                press(hwnd, VK_SUBTRACT, true);
                check(zoom == 1.f, "ctrl_keypad_minus_zooms_exactly_once");
                press(hwnd, VK_ADD, true);
                check(std::abs(zoom - 1.1f) < .001f, "ctrl_keypad_plus_zooms_exactly_once");
                press(hwnd, VK_OEM_MINUS, true);
                check(zoom == 1.f, "ctrl_minus_zooms_exactly_once");
                zoomBy(1.2f);
                press(hwnd, '0', true);
                check(zoom == 1.f, "ctrl_zero_still_resets_zoom");
                {
                    BYTE savedKeys[256]{}, state[256]{};
                    GetKeyboardState(savedKeys);
                    state[VK_MENU] = 0x80;
                    SetKeyboardState(state);
                    SendMessageW(hwnd, WM_CHAR, L'+', 1);
                    SendMessageW(hwnd, WM_CHAR, L'-', 1);
                    SetKeyboardState(savedKeys);
                    check(zoom == 1.f, "alt_characters_do_not_zoom");
                }
                SendMessageW(hwnd, WM_CHAR, L'=', 1);
                check(zoom == 1.f, "equals_is_not_a_zoom_alias");
                for (auto control : {buttons[0], zoomButtons[0], backButton}) {
                    SetFocus(control);
                    SendMessageW(control, WM_CHAR, L'+', 1);
                    check(std::abs(zoom - 1.1f) < .001f, "control_focus_plus_zooms");
                    SendMessageW(control, WM_CHAR, L'-', 1);
                }
                toggleSource();
                for (auto control : {hwnd, buttons[2], zoomButtons[0]}) {
                    SetFocus(control);
                    zoomBy(1);
                    auto originalHeight = layout(0).parts.front().height;
                    auto originalWidth = layout(0).width;
                    for (wchar_t symbol : {L'+', L'-'}) {
                        SHORT mapping = VkKeyScanW(symbol);
                        if (mapping == -1 || (HIBYTE(mapping) & 6))
                            continue;
                        press(control, LOBYTE(mapping), false, (HIBYTE(mapping) & 1) != 0);
                        check(source && std::abs(zoom - (symbol == L'+' ? 1.1f : 1.f)) < .001f,
                              "source_translated_keyboard_zoom_once_with_document_or_control_focus");
                        auto &measured = layout(0);
                        check(symbol == L'+'
                                  ? measured.parts.front().height > originalHeight &&
                                        measured.width > originalWidth
                                  : std::abs(measured.parts.front().height - originalHeight) < .01f &&
                                        std::abs(measured.width - originalWidth) < .01f,
                              "source_keyboard_zoom_resizes_measured_text");
                    }
                    press(control, VK_ADD);
                    check(std::abs(zoom - 1.1f) < .001f, "source_keypad_plus_zooms_once");
                    press(control, VK_SUBTRACT);
                    check(zoom == 1.f, "source_keypad_minus_zooms_once");
                    press(control, VK_ADD, true);
                    check(std::abs(zoom - 1.1f) < .001f, "source_ctrl_plus_zooms_once");
                    press(control, VK_SUBTRACT, true);
                    check(zoom == 1.f, "source_ctrl_minus_zooms_once");
                    zoomBy(1.2f);
                    press(control, '0', true);
                    check(zoom == 1.f, "source_ctrl_zero_resets_zoom");
                }
                zoomBy(2.9f);
                for (int i = 0; i < 3; ++i)
                    press(hwnd, VK_ADD);
                check(zoom == 3.f, "source_repeated_plus_respects_upper_limit");
                zoomBy(.6f);
                for (int i = 0; i < 3; ++i)
                    press(hwnd, VK_SUBTRACT);
                check(zoom == .5f, "source_repeated_minus_respects_lower_limit");
                zoomBy(1);
                for (int i = 0; i < 3; ++i)
                    press(hwnd, VK_ADD);
                check(std::abs(zoom - 1.3f) < .001f, "source_repeated_plus_applies_one_step_per_character");
                showSearch();
                SetWindowTextW(search, L"");
                press(search, VK_ADD);
                press(search, VK_SUBTRACT);
                wchar_t sourceSearchSymbols[8]{};
                GetWindowTextW(search, sourceSearchSymbols, 8);
                check(std::wstring(sourceSearchSymbols) == L"+-" && std::abs(zoom - 1.3f) < .001f,
                      "source_find_accepts_zoom_symbols_without_zooming");
                key(VK_ESCAPE);
                toggleSource();
                check(!source && std::abs(zoom - 1.3f) < .001f, "view_toggle_preserves_shared_zoom");
                zoomBy(2.9f);
                for (int i = 0; i < 3; ++i)
                    press(hwnd, VK_ADD);
                check(zoom == 3.f, "repeated_plus_respects_upper_zoom_limit");
                zoomBy(.6f);
                for (int i = 0; i < 3; ++i)
                    press(hwnd, VK_SUBTRACT);
                check(zoom == .5f, "repeated_minus_respects_lower_zoom_limit");
                zoomBy(1);
                for (int i = 0; i < 3; ++i)
                    press(hwnd, VK_ADD);
                check(std::abs(zoom - 1.3f) < .001f, "repeated_plus_applies_one_step_per_character");
                showSearch();
                SetWindowTextW(search, L"");
                press(search, VK_ADD);
                press(search, VK_SUBTRACT);
                SendMessageW(search, WM_CHAR, L'=', 1);
                wchar_t searchSymbols[8]{};
                GetWindowTextW(search, searchSymbols, 8);
                check(std::wstring(searchSymbols) == L"+-=" && std::abs(zoom - 1.3f) < .001f,
                      "find_accepts_zoom_symbols_without_zooming");
                key(VK_ESCAPE);
            }
            zoomBy(1.2f);
            check(zoom > 1.19f, "zoom");
            {
                auto original = doc;
                std::wstring paragraphs;
                for (int i = 0; i < 100; ++i)
                    paragraphs += L"A short paragraph for zoom layout measurement.\n\n";
                doc = std::make_shared<Document>(parse(std::move(paragraphs)));
                resetLayout(false);
                bool stable = true;
                for (bool sourceView : {false, true}) {
                    source = sourceView;
                    if (source)
                        rawBlocks = sourceBlocks(doc->source);
                    resetLayout(false);
                    for (float factor : {.5f, .9f, 1.1f, 1.3f, 2.f, 3.f}) {
                        for (bool atEnd : {false, true}) {
                            zoomBy(factor == 3.f ? 2.9f : factor + .1f);
                            scroll = atEnd ? heights.total() : 0;
                            bars();
                            zoomBy(factor);
                            RedrawWindow(canvas, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                            double firstHeight = heights.total();
                            auto firstThumb = verticalThumb();
                            for (int i = 0; i < 20; ++i)
                                timer();
                            RedrawWindow(canvas, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                            auto settledThumb = verticalThumb();
                            stable &= std::abs(heights.total() - firstHeight) < .001 &&
                                      std::abs(firstThumb.top - settledThumb.top) < .001f &&
                                      std::abs(firstThumb.bottom - settledThumb.bottom) < .001f;
                        }
                    }
                }
                check(stable, "zoom_scrollbar_is_stable_after_first_frame");
                maxWidth = width + .001f;
                bars();
                check(maxWidth <= width && horizontal == 0,
                      "subpixel_overflow_does_not_show_horizontal_scrollbar");
                maxWidth = width + 100;
                horizontal = 50;
                bars();
                check(maxWidth > width && horizontal == 50, "real_horizontal_overflow_remains_scrollable");
                source = false;
                rawBlocks.clear();
                doc = std::move(original);
                zoomBy(1.2f);
            }
            {
                RECT client;
                GetClientRect(hwnd, &client);
                check(std::abs(height - (client.bottom / scale() - 12)) < .001f,
                      "zoom_preserves_full_document_viewport_height");
            }
            {
                wchar_t label[16]{};
                GetWindowTextW(zoomButtons[1], label, 16);
                check(zoomExpanded && top == 78 && IsWindowVisible(zoomButtons[0]) &&
                          std::wstring(label) == L"120%",
                      "zoom_overlay_shows_percentage_without_reducing_viewport");
                RECT zoomBounds, lastActionBounds, client;
                GetWindowRect(zoomButtons.back(), &zoomBounds);
                GetWindowRect(buttons[fullscreen ? 6 : 5], &lastActionBounds);
                MapWindowPoints(nullptr, hwnd, (POINT *)&zoomBounds, 2);
                GetClientRect(hwnd, &client);
                check(zoomBounds.bottom < (height + 3) * scale() &&
                          zoomBounds.right < (width + 3) * scale() && zoomBounds.bottom < client.bottom,
                      "zoom_overlay_clears_both_scrollbars");
                MapWindowPoints(hwnd, nullptr, (POINT *)&zoomBounds, 2);
                check(std::abs(zoomBounds.right - lastActionBounds.right) <= 1,
                      "zoom_overlay_aligns_with_right_toolbar_edge");
                auto lastControl = GetNextDlgTabItem(hwnd, nullptr, TRUE);
                check(nextTabControl(lastControl, false) == zoomButtons.front() &&
                          nextTabControl(zoomButtons.front(), false) == zoomButtons[1] &&
                          nextTabControl(zoomButtons.front(), true) == lastControl,
                      "floating_zoom_controls_remain_in_keyboard_tab_order");
                command(140);
                check(std::abs(zoom - 1.1f) < .001f, "zoom_minus_button");
                command(142);
                check(std::abs(zoom - 1.2f) < .001f, "zoom_plus_button");
                wchar_t resetLabel[16]{}, resetTip[256]{};
                GetWindowTextW(zoomButtons[1], resetLabel, 16);
                TOOLINFOW resetTool{sizeof(resetTool)};
                resetTool.hwnd = hwnd;
                resetTool.uId = (UINT_PTR)zoomButtons[1];
                resetTool.lpszText = resetTip;
                SendMessageW(tooltip, TTM_GETTEXTW, 256, (LPARAM)&resetTool);
                check(std::wstring(resetLabel) == L"120%" && !GetDlgItem(zoomPanel, 143) &&
                          std::wstring(resetTip) == L"Reset zoom to 100% (Ctrl+0)",
                      "zoom_percentage_has_reset_tooltip_without_duplicate_button");
                command(141);
                check(zoom == 1.f, "zoom_percentage_resets_to_actual_size");
                zoomUntil = 0;
                message(WM_TIMER, 12, 0);
                zoomFadeStarted = GetTickCount64() - 200;
                zoomFade();
                check(!zoomExpanded && top == 78 && !IsWindowVisible(zoomButtons[0]),
                      "zoom_group_fades_away_at_actual_size");
                zoomBy(1.2f);
            }
            toggleFullscreen();
            break;
        case 3:
            check(fullscreen && !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION), "borderless_fullscreen");
            visibility(false);
            check(!IsWindowVisible(buttons[0]), "fullscreen_controls_hide");
            zoomFadeStarted = GetTickCount64() - 200;
            zoomFade();
            check(!IsWindowVisible(zoomButtons[0]), "zoom_group_hides_with_fullscreen_toolbar");
            POINT pointer;
            GetCursorPos(&pointer);
            ScreenToClient(hwnd, &pointer);
            message(WM_MOUSEMOVE, 0, MAKELPARAM(pointer.x, pointer.y));
            check(!showControls && !IsWindowVisible(buttons[0]),
                  "synthetic_mousemove_does_not_reveal_hidden_toolbar");
            visibility(true);
            check(IsWindowVisible(buttons[0]) != FALSE, "fullscreen_controls_reveal");
            {
                bool fileLaunch = launchedWithFile;
                launchedWithFile = false;
                key(VK_ESCAPE);
                check(!fullscreen, "escape_restores_window_without_command_line_document");
                launchedWithFile = fileLaunch;
            }
            zoomBy(1);
            showSearch();
            {
                unsigned forwarded = 0;
                RemoveWindowSubclass(search, childProc, 1);
                SetWindowSubclass(search, searchCharProbe, 2, (DWORD_PTR)&forwarded);
                SetWindowSubclass(search, childProc, 1, (DWORD_PTR)this);
                auto before = findGeneration;
                SendMessageW(search, WM_CHAR, L'\r', 0);
                SendMessageW(search, WM_CHAR, L'\n', 0);
                check(forwarded == 0 && before == findGeneration,
                      "search_enter_characters_never_reach_beeping_native_edit_or_navigate_twice");
                SendMessageW(search, WM_CHAR, L'Z', 0);
                check(forwarded == 1, "search_printable_characters_still_reach_native_edit");
                RemoveWindowSubclass(search, searchCharProbe, 2);
            }
            {
                RECT input, control;
                GetWindowRect(search, &input);
                GetWindowRect(buttons[0], &control);
                check(std::abs(input.top + input.bottom - control.top - control.bottom) <= 2,
                      "search_text_box_vertically_centered");
                wchar_t hint[128]{};
                SendMessageW(search, EM_GETCUEBANNER, (WPARAM)hint, 128);
                check(std::wstring(hint) == L"Find in document", "search_hint_available_on_focus");
            }
            SetWindowTextW(search, L"Voltura");
            find(false);
            break;
        case 4:
            if (!searchMatch || !countReady) {
                --verifyStage;
                return;
            }
            check(caret > anchor && doc->text.substr(anchor, caret - anchor) == L"Voltura", "async_search");
            {
                std::atomic_bool cancel = false;
                auto expected = countMatches(doc->text, L"Voltura", 0, cancel).total;
                check(searchTotal == expected && searchStatus == L"1 of " + std::to_wstring(expected),
                      "background_count_shows_current_match_and_total");
                check(showSearchHint() == (expected > 1 && searchBoxWidth() >= 350),
                      "search_shortcut_hint_only_for_multiple_hits_with_room");
                verificationCountGeneration = countGeneration;
                find(true);
            }
            break;
        case 5:
            if (!searchMatch || searchIndex != 1) {
                --verifyStage;
                return;
            }
            check(countReady && countGeneration == verificationCountGeneration &&
                      searchStatus ==
                          std::to_wstring(searchTotal > 1 ? 2 : 1) + L" of " + std::to_wstring(searchTotal),
                  "next_match_updates_ordinal_without_recounting");
            find(true, true);
            break;
        case 6:
            if (searchIndex != 0) {
                --verifyStage;
                return;
            }
            check(countGeneration == verificationCountGeneration &&
                      searchStatus == L"1 of " + std::to_wstring(searchTotal),
                  "previous_match_updates_ordinal_without_recounting");
            {
                auto savedAnchor = anchor, savedCaret = caret;
                auto savedScroll = scroll;
                countResult(std::make_unique<CountResult>(
                    CountResult{countGeneration - 1, generation, source, {999, 0}, false}));
                check(searchTotal != 999 && anchor == savedAnchor && caret == savedCaret &&
                          scroll == savedScroll,
                      "stale_count_cannot_change_current_search_or_position");
            }
            {
                auto staleGeneration = findGeneration;
                auto staleCountGeneration = countGeneration;
                SetWindowTextW(search, L"");
                find(false);
                searchResult(
                    std::make_unique<SearchResult>(SearchResult{staleGeneration, generation, source, 0, 7}));
                countResult(std::make_unique<CountResult>(
                    CountResult{staleCountGeneration, generation, source, {12, 0}, false}));
                check(!searchMatch && anchor == 0 && caret == 0 && searchStatus.empty(),
                      "cleared_search_rejects_pending_result");
                auto dismissedGeneration = findGeneration;
                key(VK_ESCAPE);
                searchResult(std::make_unique<SearchResult>(
                    SearchResult{dismissedGeneration, generation, source, -1, 7}));
                check(!searching && notice != L"No matches" && findGeneration != dismissedGeneration,
                      "dismissed_search_rejects_pending_result");
            }
            {
                countCancel = std::make_shared<std::atomic_bool>(false);
                countStarted = countReady = true;
                searchTotal = 12;
                searchMatch = true;
                searchIndex = 2;
                refreshSearchStatus();
                check(searchStatus == L"3 of 12", "search_status_formats_current_and_total");
                searchIndex = 12;
                refreshSearchStatus();
                check(searchStatus == L"1 of 12", "search_status_wraps_forward");
                searchIndex = -1;
                refreshSearchStatus();
                check(searchStatus == L"12 of 12", "search_status_wraps_backwards");
                auto savedWidth = width;
                width = 460;
                check(!showSearchHint(), "shortcut_hint_yields_space_in_narrow_window");
                width = savedWidth;
                searchTotal = 1;
                check(!showSearchHint(), "shortcut_hint_hidden_for_single_match");
                cancelSearch();
                searchResult(
                    std::make_unique<SearchResult>(SearchResult{findGeneration, generation, source, -1, 7}));
                check(searchStatus == L"0 of 0" && !searchMatch && !showSearchHint(),
                      "no_results_show_zero_count_without_shortcut_hint");
                cancelSearch();
            }
            {
                auto original = doc;
                auto savedRaw = std::move(rawBlocks);
                auto savedZoom = zoom;
                doc = std::make_shared<Document>(parse(L"```\n" + std::wstring(40, L'\n') + L"target\n" +
                                                       std::wstring(100, L'\n') + L"```\n"));
                for (bool raw : {false, true}) {
                    source = raw;
                    if (raw)
                        rawBlocks = sourceBlocks(doc->source);
                    for (float factor : {.5f, 1.f, 3.f}) {
                        zoom = factor;
                        resetLayout(false);
                        uint32_t position = (uint32_t)text().find(L"target");
                        auto index = blockAtPosition(blocks(), position);
                        auto &l = layout(index);
                        auto &part = l.parts.front();
                        FLOAT x, y;
                        DWRITE_HIT_TEST_METRICS metrics{};
                        part.layout->HitTestTextPosition(position - part.start, FALSE, &x, &y, &metrics);
                        jump(position, true);
                        double gap = heights.before(index) + part.y + y - scroll;
                        check(std::abs(gap - 2 * metrics.height) < .01,
                              "search_match_keeps_two_rows_of_context_at_each_zoom");
                        jump(part.start, true);
                        check(scroll == 0, "first_line_match_clamps_context_to_document_start");
                    }
                }
                doc = original;
                source = false;
                rawBlocks = std::move(savedRaw);
                zoom = savedZoom;
                resetLayout(false);
            }
            searchMatch = false;
            anchor = caret = 0;
            paintTimes.clear();
            SetTimer(hwnd, 10, 16, nullptr);
            break;
        default:
            if (!scrollFrames)
                for (auto count : buttonPaints)
                    toolbarBeforeScroll += count;
            if (scrollFrames++ < 90) {
                scrollTo(scrollFrames % 45 / 44. * std::max(0., heights.total() - (height - top - bottom)));
                return;
            }
            if (!verificationReload) {
                uint64_t total = 0;
                for (auto count : buttonPaints)
                    total += count;
                check(total == toolbarBeforeScroll, "toolbar_not_repainted_by_scrolling");
                toolbarBeforeReload = total;
                spinnerBeforeReload = spinnerPaints;
                verificationReload = true;
                open(doc->path);
                return;
            }
            if (loading)
                return;
            {
                uint64_t total = 0;
                for (auto count : buttonPaints)
                    total += count;
                check(total == toolbarBeforeReload, "toolbar_not_repainted_by_loading");
                if (doc->source.size() > 1024 * 1024)
                    check(spinnerPaints > spinnerBeforeReload + 1, "spinner_animates_in_isolated_region");
            }
            check(std::isfinite(scroll) && scroll >= 0 &&
                      scroll <= std::max(0., heights.total() - (height - top - bottom)),
                  "scroll_bounds");
            auto thumb = verticalThumb();
            check(std::isfinite(thumb.top) && thumb.bottom >= thumb.top, "themed_thumb_geometry");
            check(heights.total() <= height - top - bottom ||
                      (scrollbarAt(width + 6, top + 10) == 1 &&
                       documentCursor(width + 6, top + 10) == LoadCursorW(nullptr, IDC_ARROW)),
                  "vertical_scrollbar_uses_arrow_cursor");
            {
                anchor = 0;
                caret = (uint32_t)text().size();
                beginEdit(true);
                finishEdit();
                check(anchor == 0 && caret == 0 && doc && doc->text.empty(),
                      "new_draft_finish_resets_reader_selection");
                key(VK_LEFT);
                check(caret == 0, "shorter_document_navigation_stays_in_bounds");
            }
            {
                auto samples = paintTimes;
                std::sort(samples.begin(), samples.end());
                double mean = 0;
                for (auto v : samples)
                    mean += v;
                mean /= std::max<size_t>(1, samples.size());
                double p95 = samples.empty() ? 0 : samples[(samples.size() - 1) * 95 / 100];
                std::ofstream out(std::filesystem::path(verifyFile), std::ios::app);
                out << "{\"passed\":" << (checksPassed ? "true" : "false")
                    << ",\"scroll_paint_samples\":" << samples.size() << ",\"paint_mean_ms\":" << mean
                    << ",\"paint_p95_ms\":" << p95 << "}\n";
            }
            KillTimer(hwnd, 10);
            if (verifyCloseFullscreen) {
                toggleFullscreen();
                check(fullscreen && IsWindowVisible(buttons[6]), "fullscreen_close_icon_visible");
                command(107);
                check(!IsWindow(hwnd), "fullscreen_x_closes_process_without_restoring_window");
            } else {
                key(VK_ESCAPE);
                check(!IsWindow(hwnd), "escape_closes_normal_window");
            }
            break;
        }
    }
#endif
    void close() {
        KillTimer(hwnd, 18);
        updates.close(prefs.automaticUpdates && !sessionEnding VMD_TEST_ONLY(&&!selfTest));
        stopScrolling();
        closing = true;
        KillTimer(hwnd, 16);
        KillTimer(hwnd, 17);
        associationResult.reset();
        finishRenderer();
        for (auto &w : workers)
            w.cancel->store(true);
        for (auto &w : workers)
            if (w.thread.joinable())
                w.thread.join();
        workers.clear();
        MSG message;
        while (PeekMessageW(&message, hwnd, Loaded, Loaded, PM_REMOVE))
            delete (LoadedDocument *)message.lParam;
        while (PeekMessageW(&message, hwnd, WM_APP + 3, WM_APP + 3, PM_REMOVE))
            delete (SearchResult *)message.lParam;
        while (PeekMessageW(&message, hwnd, WM_APP + 5, WM_APP + 5, PM_REMOVE))
            delete (CountResult *)message.lParam;
        images.reset();
#ifdef VMD_TESTING
        if (!selfTest)
#endif
        {
            prefs.zoom = informationView ? informationView->zoom : zoom;
            if (fullscreen)
                prefs.placement = saved;
            else {
                prefs.placement.length = sizeof(WINDOWPLACEMENT);
                GetWindowPlacement(hwnd, &prefs.placement);
            }
            prefs.placed = true;
            savePreferences(prefs);
        }
        if (uiFont)
            DeleteObject(uiFont);
        if (largeIcon)
            DestroyIcon(largeIcon);
        if (smallIcon)
            DestroyIcon(smallIcon);
    }
    LRESULT message(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            initialise();
            return 0;
        case ScheduleAssociation:
            if (associationPhase == AssociationPhase::Posted && !closing) {
                if (loading || wp != generation || associationReadyGeneration != generation) {
                    associationPhase = AssociationPhase::WaitingForPaint;
                    if (!loading)
                        invalidate();
                    return 0;
                }
                associationPhase = AssociationPhase::Delay;
                SetTimer(hwnd, 16, 3000, nullptr);
                VMD_TEST_ONLY(associationTrace("delay_started");)
            }
            return 0;
        case WM_PAINT:
            paintHeader();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOVE:
            positionZoomOverlay();
            positionAssociation();
            return 0;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED)
                return 0;
            ++viewportUpdates;
            positions();
            if (doc)
                resetLayout();
            // Resizing the D2D target does not schedule a paint. The empty view
            // needs this too, otherwise its artwork remains at fullscreen coordinates.
            invalidate();
            return 0;
        case WM_DPICHANGED: {
            dpi = (float)HIWORD(wp);
            refreshDpiAssets();
            if (target)
                target->SetDpi(dpi, dpi);
            auto r = (RECT *)lp;
            MONITORINFO monitor{sizeof(monitor)};
            if (fullscreen && GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &monitor))
                r = &monitor.rcMonitor;
            auto previousUpdate = viewportUpdates;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            // A fullscreen move can change DPI without changing pixel dimensions.
            // Windows then need not send WM_SIZE, but every DIP-based viewport
            // measurement and the child render target still need rebuilding.
            if (viewportUpdates == previousUpdate)
                message(WM_SIZE, SIZE_RESTORED, 0);
            return 0;
        }
        case WM_GETMINMAXINFO:
            ((MINMAXINFO *)lp)->ptMinTrackSize = {(LONG)(460 * scale()), (LONG)(280 * scale())};
            return 0;
        case WM_SETTINGCHANGE:
            if (prefs.theme < 0)
                theme();
            return 0;
        case WM_DRAWITEM:
            if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU)
                drawMenu(*(DRAWITEMSTRUCT *)lp);
            else
                drawButton(*(DRAWITEMSTRUCT *)lp);
            return TRUE;
        case WM_MEASUREITEM:
            if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
                measureMenu(*(MEASUREITEMSTRUCT *)lp);
                return TRUE;
            }
            break;
        case WM_CTLCOLOREDIT: {
            auto dc = (HDC)wp;
            SetTextColor(dc, dark ? RGB(229, 233, 240) : RGB(32, 41, 56));
            SetBkColor(dc, dark ? RGB(28, 34, 45) : RGB(255, 255, 255));
            static HBRUSH darkBrush = CreateSolidBrush(RGB(28, 34, 45)),
                          lightBrush = CreateSolidBrush(RGB(255, 255, 255));
            return (LRESULT)(dark ? darkBrush : lightBrush);
        }
        case WM_COMMAND:
            if (LOWORD(wp) == 200 && HIWORD(wp) == EN_CHANGE) {
                cancelSearch();
                SetTimer(hwnd, 3, 180, nullptr);
                return 0;
            }
            command(LOWORD(wp));
            return 0;
        case EditorChanged:
            invalidateHeader();
            return 0;
        case EditorScroll:
            if (editor && editor->isLive() && !informationView) {
                stopScrolling();
                editorSourceDrivesPreview = true;
                syncPreviewToSource();
                bars();
                invalidate();
            }
            return 0;
#ifdef VMD_TESTING
        case WM_APP + 40:
            // Private-desktop scroll regression probe; absent from vmd.exe.
            return (LRESULT)std::llround(previewSourcePosition()) + 1;
        case WM_APP + 41: {
            // Check the backing buffer, since D2D stretches stale buffers to fit the HWND.
            if (!target)
                return 0;
            RECT bounds{};
            GetClientRect(canvas, &bounds);
            auto pixels = target->GetPixelSize();
            return pixels.width == (UINT32)bounds.right && pixels.height == (UINT32)bounds.bottom;
        }
#endif
        case EditorLayout:
            if (editor) {
                message(WM_SIZE, SIZE_RESTORED, 0);
                if (!editor->isPreview())
                    editor->focus();
            }
            return 0;
        case EditorPreview:
            if (editor && (editor->isPreview() || editor->isLive()) && !informationView &&
                editor->previewDocument()) {
                cancelSearch();
                ++generation;
                images->cancelPending();
                doc = editor->previewDocument();
                source = false;
                VMD_TEST_ONLY(editorPreviewAwaitingPaint = true;)
                rawBlocks.clear();
                anchor = caret = 0;
                message(WM_SIZE, SIZE_RESTORED, 0);
                resetLayout(editor->isLive());
                syncPreviewToSource();
                invalidate();
                if (!editor->isLive())
                    SetFocus(hwnd);
            }
            return 0;
        case EditorSaved:
            if (editor && !wp) {
                editOriginal = editor->previewDocument();
                if (informationView) {
                    informationView->document = editOriginal;
                    informationView->raw.clear();
                    informationView->source = false;
                    informationView->anchor = informationView->caret = 0;
                } else {
                    cancelSearch();
                    ++generation;
                    doc = editOriginal;
                    source = false;
                    rawBlocks.clear();
                    anchor = caret = 0;
                    searchMatch = false;
                    resetLayout(editor->isPreview() || editor->isLive());
                    invalidate();
                }
#ifdef VMD_TESTING
                if (!metricsFile.empty()) {
                    auto savedDocument = informationView ? informationView->document : doc;
                    std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
                    out << "{\"edit_saved_path_matches\":"
                        << (savedDocument && savedDocument->path == editor->path()) << "}\n";
                }
#endif
            }
            if (afterEditSave) {
                auto action = std::move(afterEditSave);
                afterEditSave = {};
                if (!wp)
                    action();
            }
            invalidateHeader();
            return 0;
        case EditorFinish:
            finishEdit();
            return 0;
        case Loaded:
            loaded(std::unique_ptr<LoadedDocument>((LoadedDocument *)lp));
            return 0;
        case WM_APP + 5:
            countResult(std::unique_ptr<CountResult>((CountResult *)lp));
            return 0;
        case WM_APP + 3:
            searchResult(std::unique_ptr<SearchResult>((SearchResult *)lp));
            return 0;
        case ImagesReady: {
            for (auto i = cache.begin(); i != cache.end();) {
                if (i->second.imagePending) {
                    cacheCost -= i->second.cost;
                    i = cache.erase(i);
                } else
                    ++i;
            }
            if (editorSourceDrivesPreview)
                syncPreviewToSource();
            invalidate();
            return 0;
        }
        case WM_TIMER:
            if (wp == 19) {
                KillTimer(hwnd, 19);
                copiedCode = SIZE_MAX;
                invalidate();
                return 0;
            }
            if (wp == 18) {
                if (prefs.automaticUpdates && !sessionEnding && !closing) {
#ifdef VMD_TESTING
                    if (!metricsFile.empty()) {
                        std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
                        out << "{\"update_check_ms\":"
                            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                         started)
                                   .count()
                            << "}\n";
                    }
#endif
                    updates.check();
                    SetTimer(hwnd, 18, 60 * 60 * 1000, nullptr);
                } else
                    KillTimer(hwnd, 18);
                return 0;
            }
            if (wp == 16) {
                KillTimer(hwnd, 16);
                if (associationPhase == AssociationPhase::Delay) {
#ifdef VMD_TESTING
                    if (testAssociation == L"defer" && generation == 1)
                        open(pendingPath);
#endif
                    if (loading || associationReadyGeneration != generation) {
                        associationPhase = AssociationPhase::WaitingForPaint;
                        VMD_TEST_ONLY(associationTrace("deferred");)
                    } else
                        startAssociationWork();
                }
                return 0;
            }
            if (wp == 17) {
                pollAssociation();
                return 0;
            }
            if (wp == 15) {
                promoteHardware();
                return 0;
            }
            if (wp == 1)
                timer();
            else if (wp == 2) {
                if (!editor && fullscreen && !loading && !menuOpen && !dragging && !searching &&
                    !pointerOverControls() &&
                    (GetFocus() == hwnd || (!IsChild(hwnd, GetFocus()) && !IsChild(zoomPanel, GetFocus())))) {
                    visibility(false);
                    KillTimer(hwnd, 2);
                }
            } else if (wp == 3) {
                KillTimer(hwnd, 3);
                find(false);
            } else if (wp == 4)
                animateScroll();
            else if (wp == 5 && loading && showControls && !searching) {
                RECT spinner{(LONG)(24 * scale()), (LONG)(25 * scale()), (LONG)(48 * scale()),
                             (LONG)(49 * scale())};
                InvalidateRect(hwnd, &spinner, FALSE);
            }
#ifdef VMD_TESTING
            else if (wp == 9) {
                KillTimer(hwnd, 9);
                requestClose();
            } else if (wp == 13) {
                double limit = std::max(0., heights.total() - (height - top - bottom));
                if (limit <= 0) {
                    KillTimer(hwnd, 13);
                    checksPassed = false;
                    std::ofstream out(std::filesystem::path(verifyFile), std::ios::app);
                    out << "{\"passed\":false,\"check\":\"scrollable_content_required\"}\n";
                    out.close();
                    requestClose();
                    return 0;
                }
                if (std::chrono::steady_clock::now() - scrollBenchmarkStarted < std::chrono::seconds(10)) {
                    if ((scrollBenchmarkDirection > 0 && scroll >= limit - .2) ||
                        (scrollBenchmarkDirection < 0 && scroll <= .2)) {
                        scrollBenchmarkDirection = -scrollBenchmarkDirection;
                        ++scrollBenchmarkReversals;
                    }
                    ++scrollFrames;
                    scrollTo(scrollTarget + scrollBenchmarkDirection * 48);
                } else {
                    KillTimer(hwnd, 13);
                    auto samples = paintTimes;
                    std::sort(samples.begin(), samples.end());
                    double mean = 0;
                    for (auto value : samples)
                        mean += value;
                    BOOL animations = TRUE;
                    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
                    std::ofstream out(std::filesystem::path(verifyFile), std::ios::app);
                    out << "{\"passed\":true,\"system_animations_enabled\":"
                        << (animations ? "true" : "false")
                        << ",\"benchmark_forces_animation\":" << (benchmarkAnimations ? "true" : "false")
                        << ",\"scroll_input_samples\":" << scrollFrames
                        << ",\"scroll_boundary_reversals\":" << scrollBenchmarkReversals
                        << ",\"scroll_paint_samples\":" << samples.size()
                        << ",\"paint_mean_ms\":" << mean / std::max<size_t>(1, samples.size())
                        << ",\"paint_p95_ms\":"
                        << (samples.empty() ? 0 : samples[(samples.size() - 1) * 95 / 100]) << "}\n";
                    out.close();
                    requestClose();
                }
            } else if (wp == 14) {
                auto now = std::chrono::steady_clock::now();
                loadingHeartbeatMaxMs =
                    std::max(loadingHeartbeatMaxMs,
                             std::chrono::duration<double, std::milli>(now - heartbeatAt).count());
                heartbeatAt = now;
                ++loadingHeartbeatSamples;
            } else if (wp == 10)
                verifyUi();
#endif
            else if (wp == 11)
                zoomFade();
            else if (wp == 12) {
                KillTimer(hwnd, 12);
                auto now = GetTickCount64();
                if (now < zoomUntil)
                    SetTimer(hwnd, 12, (UINT)(zoomUntil - now), nullptr);
                else if (std::abs(zoom - 1.f) < .001f) {
                    auto focus = GetFocus();
                    for (auto button : zoomButtons)
                        if (focus == button)
                            SetFocus(hwnd);
                    zoomExpanded = false;
                    zoomVisibility();
                }
            }
            return 0;
        case WM_CHAR:
            if (zoomCharacter(wp))
                return 0;
            break;
        case WM_KEYDOWN:
            key(wp);
            return 0;
        case WM_MOUSEWHEEL:
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)
                zoomBy(zoom + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? .1f : -.1f));
            else {
                UINT lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                double amount = lines == WHEEL_PAGESCROLL ? height - top - bottom : lines * 18 * zoom;
                scrollTo((scrollAnimating ? scrollTarget : scroll) -
                         GET_WHEEL_DELTA_WPARAM(wp) / 120. * amount);
            }
            return 0;
        case WM_MOUSEHWHEEL:
            horizontal += GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 54 * zoom;
            bars();
            invalidate();
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO s{sizeof(s), SIF_TRACKPOS};
            GetScrollInfo(hwnd, SB_VERT, &s);
            switch (LOWORD(wp)) {
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
                scroll = s.nTrackPos;
                break;
            case SB_LINEUP:
                scroll -= 40;
                break;
            case SB_LINEDOWN:
                scroll += 40;
                break;
            case SB_PAGEUP:
                scroll -= height - top - bottom;
                break;
            case SB_PAGEDOWN:
                scroll += height - top - bottom;
                break;
            case SB_TOP:
                scroll = 0;
                break;
            case SB_BOTTOM:
                scroll = heights.total();
                break;
            }
            bars();
            if (editor && editor->isLive())
                syncSourceToPreview();
            invalidate();
            return 0;
        }
        case WM_HSCROLL: {
            SCROLLINFO s{sizeof(s), SIF_TRACKPOS};
            GetScrollInfo(hwnd, SB_HORZ, &s);
            switch (LOWORD(wp)) {
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
                horizontal = (float)s.nTrackPos;
                break;
            case SB_LINELEFT:
                horizontal -= 40;
                break;
            case SB_LINERIGHT:
                horizontal += 40;
                break;
            case SB_PAGELEFT:
                horizontal -= width / 2;
                break;
            case SB_PAGERIGHT:
                horizontal += width / 2;
                break;
            }
            bars();
            invalidate();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            float x = GET_X_LPARAM(lp) / scale() - canvasLeft, y = GET_Y_LPARAM(lp) / scale();
            SetFocus(hwnd);
            stopScrolling();
            if (scrollbarAt(x, y) == 1) {
                auto r = verticalThumb();
                if (y < r.top || y > r.bottom) {
                    scroll += (y < r.top ? -1 : 1) * (height - top - bottom);
                    bars();
                    if (editor && editor->isLive())
                        syncSourceToPreview();
                } else {
                    scrollDrag = 1;
                    dragStart = y;
                    dragScroll = scroll;
                    SetCapture(hwnd);
                }
                invalidate();
                return 0;
            }
            if (scrollbarAt(x, y) == 2) {
                auto r = horizontalThumb();
                if (x < r.left || x > r.right) {
                    horizontal += (x < r.left ? -1 : 1) * width * .8f;
                    bars();
                } else {
                    scrollDrag = 2;
                    dragStart = x;
                    dragScroll = horizontal;
                    SetCapture(hwnd);
                }
                invalidate();
                return 0;
            }
            if (!notice.empty() && y > height - 44) {
                notice.clear();
                invalidate();
                return 0;
            }
            if (y < top || y > height - bottom)
                return 0;
            if (auto code = codeButtonAt(x, y); code != SIZE_MAX) {
                pressedCode = code;
                SetCapture(hwnd);
                return 0;
            }
            if (doc) {
                caret = hit(x, y);
                if ((wp & MK_CONTROL) && editor && editor->isLive() && doc == editor->previewDocument()) {
                    auto block = blockAtPosition(doc->blocks, caret);
                    if (block < doc->blocks.size() && doc->blocks[block].kind == Kind::Heading) {
                        size_t heading = 0;
                        for (size_t i = 0; i < block; ++i)
                            if (doc->blocks[i].kind == Kind::Heading)
                                ++heading;
                        editor->sourceAtHeading(heading);
                        return 0;
                    }
                }
                if (!(wp & MK_SHIFT))
                    anchor = caret;
                searchMatch = false;
                dragging = true;
                SetCapture(hwnd);
                invalidate();
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT point;
            bool moved = GetCursorPos(&point) && pointerMoved(point);
            if (fullscreen && moved) {
                if (!showControls)
                    visibility(true);
                SetTimer(hwnd, 2, 1800, nullptr);
            }
            float x = GET_X_LPARAM(lp) / scale() - canvasLeft, y = GET_Y_LPARAM(lp) / scale();
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, canvas, 0};
            TrackMouseEvent(&tracking);
            auto previousCode = codeButtonAt(codePointerX, codePointerY);
            codePointerX = x;
            codePointerY = y;
            if (previousCode != codeButtonAt(x, y))
                invalidate();
            hoverLink(codeButtonAt(x, y) == SIZE_MAX ? linkAt(x, y) : L"");
            // Captured mouse movement does not reliably generate WM_SETCURSOR.
            SetCursor(documentCursor(x, y));
            if (scrollDrag == 1) {
                auto r = verticalThumb();
                scroll = dragScroll + (y - dragStart) *
                                          std::max(0., heights.total() - (height - top - bottom)) /
                                          std::max(1.f, height - top - bottom - (r.bottom - r.top));
                bars();
                if (editor && editor->isLive())
                    syncSourceToPreview();
                invalidate();
            } else if (scrollDrag == 2) {
                auto r = horizontalThumb();
                horizontal = (float)dragScroll + (x - dragStart) * std::max(0.f, maxWidth - width) /
                                                     std::max(1.f, width - 32 - (r.right - r.left));
                bars();
                invalidate();
            } else if (dragging) {
                if (y < top)
                    scroll -= 15;
                if (y > height - bottom)
                    scroll += 15;
                bars();
                caret = hit(x, y);
                invalidate();
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (pressedCode != SIZE_MAX) {
                auto code = pressedCode;
                pressedCode = SIZE_MAX;
                ReleaseCapture();
                if (codeButtonAt(GET_X_LPARAM(lp) / scale() - canvasLeft, GET_Y_LPARAM(lp) / scale()) ==
                    code) {
                    if (copyText(hwnd, codeText(code))) {
                        copiedCode = code;
                        copiedCodeUntil = GetTickCount64() + 1800;
                        SetTimer(hwnd, 19, 1800, nullptr);
                    } else
                        notice = L"The clipboard is unavailable. Try again.";
                }
                invalidate();
                return 0;
            }
            if (scrollDrag) {
                scrollDrag = 0;
                ReleaseCapture();
                invalidate();
            } else if (dragging) {
                dragging = false;
                ReleaseCapture();
                if (anchor == caret)
                    activateLink(caret);
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (doc) {
                caret = hit(GET_X_LPARAM(lp) / scale() - canvasLeft, GET_Y_LPARAM(lp) / scale());
                anchor = caret;
                auto &s = text();
                while (anchor && iswalnum(s[anchor - 1]))
                    --anchor;
                while (caret < s.size() && iswalnum(s[caret]))
                    ++caret;
                searchMatch = false;
                invalidate();
            }
            return 0;
        case WM_CAPTURECHANGED:
            pressedCode = SIZE_MAX;
            dragging = false;
            scrollDrag = 0;
            return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT n = DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring path(n + 1, 0);
            DragQueryFileW(drop, 0, path.data(), n + 1);
            path.resize(n);
            DragFinish(drop);
            if (editor && !informationView && editor->addImage(path))
                return 0;
            open(path);
            return 0;
        }
        case WM_QUERYENDSESSION:
            if (editor && (editor->modified() || editor->busy()))
                return FALSE;
            if ((lp & ENDSESSION_CLOSEAPP) && !(lp & ENDSESSION_LOGOFF)) {
                try {
                    prepareRestart();
                } catch (...) {
                    return FALSE; // Keep this reader open if its state cannot be preserved.
                }
                if (restartFile.empty())
                    return FALSE;
            }
            sessionEnding = true; // A cancelled shutdown still suppresses installation for this run.
            KillTimer(hwnd, 18);
            updates.sessionEnd();
            return TRUE;
        case WM_ENDSESSION:
            sessionEnding = true;
            KillTimer(hwnd, 18);
            updates.sessionEnd();
            if (!wp && !restartFile.empty()) {
                UnregisterApplicationRestart();
                DeleteFileW(restartFile.c_str());
                restartFile.clear();
            }
            if (wp)
                requestClose();
            return 0;
        case WM_CLOSE:
            requestClose();
            return 0;
        case WM_DESTROY:
            close();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App *app = (App *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        app = (App *)((CREATESTRUCTW *)lp)->lpCreateParams;
        app->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
    }
    if (!app)
        return DefWindowProcW(hwnd, msg, wp, lp);
    try {
        return app->message(msg, wp, lp);
    } catch (const std::exception &e) {
        themedMessageBox(hwnd, fromUtf8(e.what()).c_str(), L"Voltura Markdown", MB_ICONERROR);
        if (msg == WM_CREATE)
            return -1;
        return 0;
    }
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!args)
        return 1;
#ifdef VMD_TESTING
    // Restart Manager chooses the interactive desktop. Tests move back before creating any HWND.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring_view(args[i]) == L"--test-restart-desktop") {
            HDESK desktop = OpenDesktopW(args[i + 1], 0, FALSE, GENERIC_ALL);
            if (!desktop || !SetThreadDesktop(desktop)) {
                LocalFree(args);
                return 1;
            }
            break;
        }
    }
#endif
    int manualResult = manualUpdateHelper(argc, args);
    if (manualResult >= 0) {
        LocalFree(args);
        return manualResult;
    }
    int helperResult = updateHelper(argc, args);
    if (helperResult >= 0) {
        LocalFree(args);
        return helperResult;
    }
    if (!updateReaderMayStart()) {
        LocalFree(args);
        return 0;
    }
    App app;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitCommonControls();
    std::filesystem::path path;
#ifdef VMD_TESTING
    UINT exitAfter = 0;
#endif
    bool configureDefaults = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = args[i];
        if (a == L"--configure-defaults")
            configureDefaults = true;
        else if (a == L"--restore-update" && i + 1 < argc) {
            auto snapshot = std::filesystem::path(args[++i]);
            app.restartView = readRestartView(snapshot);
            if (app.restartView)
                path = app.restartView->document;
            app.restartFile = snapshot;
        }
#ifdef VMD_TESTING
        else if (a == L"--test-restart-desktop" && i + 1 < argc)
            ++i;
        else if (a == L"--test-restart-report" && i + 1 < argc) {
            app.restartReport = args[++i];
            app.selfTest = true;
        } else if (a == L"--test-automatic-updates") {
            app.testAutomaticUpdates = true;
            app.selfTest = true;
        } else if (a == L"--test-update-fixture" && i + 1 < argc) {
            app.testAutomaticUpdates = true;
            app.selfTest = true;
            app.updateFixture = args[++i];
        } else if (a == L"--test-association" && i + 1 < argc) {
            app.testAssociation = args[++i];
            app.selfTest = true;
        } else if (a == L"--verify-close-fullscreen")
            app.verifyCloseFullscreen = true;
        else if (a == L"--metrics" && i + 1 < argc)
            app.metricsFile = args[++i];
        else if (a == L"--benchmark-animations")
            app.benchmarkAnimations = true;
        else if (a == L"--hardware-renderer")
            app.stagedRenderer = false;
        else if (a == L"--staged-renderer")
            app.stagedRenderer = true;
        else if (a == L"--test-renderer-fail-preparation")
            app.failRendererPreparation = true;
        else if (a == L"--test-renderer-fail-brush")
            app.failDeviceBrush = true;
        else if (a == L"--test-renderer-fail-first-frame")
            app.failRendererFirstFrame = true;
        else if (a == L"--scroll-benchmark" && i + 1 < argc) {
            app.scrollBenchmark = true;
            app.verifyFile = args[++i];
            app.selfTest = true;
        } else if (a == L"--benchmark") {
            app.exitOnContent = true;
            app.selfTest = true;
        } else if (a == L"--verify-ui" && i + 1 < argc) {
            app.verifyFile = args[++i];
            app.selfTest = true;
        } else if (a == L"--exit-after" && i + 1 < argc) {
            exitAfter = (UINT)_wtoi(args[++i]);
            app.selfTest = true;
        } else if (a == L"--no-preferences")
            app.selfTest = true;
#endif
        else if (!a.starts_with(L"--"))
            path = a;
    }
    LocalFree(args);
    app.launchedWithFile = !path.empty();
    if (configureDefaults) {
        bool registered = false;
        registerApplication(false, true, &registered);
        CoUninitialize();
        return registered ? 0 : 1;
    }
    WNDCLASSEXW c{sizeof(c)};
    c.style = CS_DBLCLKS;
    c.lpfnWndProc = windowProc;
    c.hInstance = instance;
    c.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    c.hIconSm = c.hIcon;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.lpszClassName = L"VolturaMarkdownWindow";
    RegisterClassExW(&c);
    auto initialDpi = GetDpiForSystem();
    int initialWidth = std::min((int)(1040 * initialDpi / 96), GetSystemMetrics(SM_CXSCREEN) - 80),
        initialHeight = std::min((int)(800 * initialDpi / 96), GetSystemMetrics(SM_CYSCREEN) - 100);
    HWND hwnd = CreateWindowExW(0, c.lpszClassName, L"Voltura Markdown",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                initialWidth, initialHeight, nullptr, nullptr, instance, &app);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }
    VMD_TEST_ONLY(app.startupPhase("window_created");)
    if (app.restartView)
        SetWindowPlacement(hwnd, &app.restartView->placement);
    if (!path.empty())
        app.open(path, path.is_absolute() && !app.restartView);
    if (app.restartView ? app.restartView->fullscreen : app.launchedWithFile && app.prefs.startFullscreen)
        app.toggleFullscreen();
    ShowWindow(hwnd, app.restartView ? app.restartView->placement.showCmd : show);
    UpdateWindow(hwnd);
    VMD_TEST_ONLY(if (path.empty() && app.restartView) app.reportRestart(".after");)
    VMD_TEST_ONLY(app.startupPhase("window_shown");)
#ifdef VMD_TESTING
    if (!app.metricsFile.empty()) {
        app.heartbeatAt = std::chrono::steady_clock::now();
        SetTimer(hwnd, 14, 16, nullptr);
    }
    if (exitAfter)
        SetTimer(hwnd, 9, exitAfter, nullptr);
#endif
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (app.editor && app.editor->dialogMessage(message))
            continue;
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB && app.editor && !app.informationView) {
            if (app.editor->tab(GetKeyState(VK_SHIFT) < 0))
                continue;
        } else if (message.message == WM_KEYDOWN && message.wParam == VK_TAB && GetFocus() != hwnd) {
            HWND next = app.nextTabControl(GetFocus(), GetKeyState(VK_SHIFT) < 0);
            if (next)
                SetFocus(next);
            else
                SetFocus(hwnd);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
#ifdef VMD_TESTING
    return app.scrollBenchmark && !app.checksPassed ? 1 : 0;
#else
    return 0;
#endif
}
