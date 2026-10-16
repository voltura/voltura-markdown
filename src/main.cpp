#include "document.h"
#include "empty-state.h"
#include "images.h"
#include "shell.h"
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
#include <list>
#include <memory>
#include <shellapi.h>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace {
using namespace vm;
constexpr UINT Loaded = WM_APP + 1, ImagesReady = WM_APP + 2;
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
    std::vector<D2D1_RECT_F> keycaps;
    uint32_t start, length;
    float x = 0, y = 0, width = 0, height = 0;
};
struct Layout {
    std::vector<Part> parts;
    ComPtr<ID2D1Bitmap> bitmap;
    float height = 0, width = 0, tableGap = 0;
    bool imagePending = false, plain = false;
    uint64_t used = 0;
    size_t cost = 0;
};
struct LoadedDocument {
    uint64_t generation;
    std::shared_ptr<Document> document;
    std::wstring error;
};
struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic_bool> cancel;
    std::shared_ptr<std::atomic_bool> done;
};
struct App {
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
    bool dark = false, source = false, fullscreen = false, showControls = true, dragging = false,
         searching = false, loading = false;
    bool closing = false, selfTest = false, searchMatch = false, launchedWithFile = false;
    float dpi = 96, zoom = 1, width = 900, height = 700, horizontal = 0, maxWidth = 0;
    unsigned viewportUpdates = 0;
    int scrollDrag = 0;
    float dragStart = 0;
    double dragScroll = 0;
    double scroll = 0;
    double scrollTarget = 0;
    bool scrollAnimating = false;
    std::chrono::steady_clock::time_point scrollTick;
    WINDOWPLACEMENT saved{sizeof(WINDOWPLACEMENT)};
    DWORD oldStyle = 0;
    uint32_t anchor = 0, caret = 0;
    uint64_t generation = 0, tick = 0;
    std::shared_ptr<Document> doc;
    std::vector<Block> rawBlocks;
    Heights heights;
    std::unordered_map<size_t, Layout> cache;
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
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now(), openStarted = started;
    bool firstPaint = false;
    std::wstring metricsFile;
    bool exitOnContent = false;
    bool verifyCloseFullscreen = false;
    std::wstring verifyFile;
    int verifyStage = 0, scrollFrames = 0;
    std::array<uint64_t, 6> buttonPaints{};
    uint64_t toolbarBeforeScroll = 0, toolbarBeforeReload = 0;
    uint64_t spinnerPaints = 0, spinnerBeforeReload = 0;
    bool verificationReload = false;
    bool checksPassed = true;
    std::vector<double> paintTimes;
    struct MenuRow {
        UINT id;
        std::wstring text;
        bool separator = false, enabled = true, checked = false;
    };
    std::vector<MenuRow> menuRows;
    struct SavedView {
        std::shared_ptr<Document> document;
        std::vector<Block> raw;
        bool source;
        double scroll;
        float horizontal, zoom;
        uint32_t anchor, caret;
        size_t blockIndex;
        double withinBlock;
    };
    std::unique_ptr<SavedView> informationView;
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
    std::vector<Block> &blocks() {
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
        r.bottom = (LONG)(top * scale());
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
            float boxWidth = std::max(100.f, width - 7 * 38 - 20 - 48);
            RoundRect(dc, (int)(28 * scale()), (int)(20 * scale()), (int)((28 + boxWidth) * scale()),
                      (int)(54 * scale()), (int)(8 * scale()), (int)(8 * scale()));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(box);
        }
        std::wstring title = doc ? doc->path.filename().wstring() : L"";
        if (showControls && !searching && loading) {
            if (!verifyFile.empty())
                ++spinnerPaints;
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
        } else if (showControls && !searching && width > 600) {
            RECT label{(LONG)(28 * scale()), (LONG)(26 * scale()), (LONG)((width - 320) * scale()),
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
        if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
            return a->message(msg, wp, lp);
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEMOVE) {
            int y = GET_Y_LPARAM(lp) + (int)(a->top * a->scale());
            return a->message(msg, wp, MAKELPARAM(GET_X_LPARAM(lp), y));
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
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(canvas, D2D1::SizeU(r.right, r.bottom),
                                             D2D1_PRESENT_OPTIONS_IMMEDIATELY),
            &target);
        if (FAILED(hr))
            return false;
        target->SetDpi(dpi, dpi);
        target->SetTransform(D2D1::Matrix3x2F::Translation(0, -top));
        return SUCCEEDED(target->CreateSolidColorBrush(rgb(0), &brush));
    }
    void clearLayouts() {
        cache.clear();
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
        for (size_t i = 0; i < bs.size(); ++i)
            heights.set(i, estimate(bs[i]));
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
        if (block.kind == Kind::Table && !block.cells.empty()) {
            for (const auto &cell : block.cells)
                if (cell.length)
                    appendRange(cell.start, cell.length);
        } else {
            appendRange(block.start, block.length);
        }
        out.height += 14 * zoom;
        heights.set(index, out.height);
        maxWidth = std::max(maxWidth, out.width + left() * 2);
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
    Layout &formattedLayout(size_t index) {
        auto found = cache.find(index);
        if (found != cache.end()) {
            found->second.used = ++tick;
            return found->second;
        }
        auto &b = blocks()[index];
        Layout out;
        out.used = ++tick;
        float indent = source ? 0 : (float)(b.indent ? b.indent - 1 : 0) * 22 * zoom + b.quote * 18 * zoom;
        float available = std::max(80.f, contentWidth() - indent),
              padding = (!source && b.kind == Kind::Code) ? 14 * zoom : 0;
        float font = (b.kind == Kind::Heading ? (b.level == 1   ? 34.f
                                                 : b.level == 2 ? 27.f
                                                 : b.level == 3 ? 22.f
                                                                : 18.f)
                      : b.kind == Kind::Code  ? 14.f
                                              : 16.f) *
                     zoom;
        if (b.kind == Kind::Rule) {
            out.height = 24 * zoom;
        } else if (b.kind == Kind::Image) {
            auto image = images->get(b.image, doc->path.wstring());
            if (image && image->ready) {
                float f = std::min(1.f, available / image->width);
                float w = image->width * f, h = image->height * f;
                D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                if (SUCCEEDED(target->CreateBitmap(D2D1::SizeU(image->width, image->height),
                                                   image->pixels.data(), image->width * 4, props,
                                                   &out.bitmap))) {
                    out.width = w;
                    out.height = h + 18 * zoom;
                    out.cost += (size_t)image->width * image->height * 4;
                }
            }
            if (!out.bitmap) {
                out.imagePending = !image;
                out.height = 64 * zoom;
                out.width = available;
            }
        } else {
            auto make = [&](uint32_t start, uint32_t count, float x, float w, int align) {
                Part p;
                p.start = start;
                p.length = count;
                p.x = x;
                p.y = padding;
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
                std::vector<DWRITE_TEXT_RANGE> keycapRanges;
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
                        if (r.style & Underline)
                            p.layout->SetUnderline(TRUE, range);
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
                DWRITE_TEXT_METRICS m{};
                p.layout->GetMetrics(&m);
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
                out.cost += p.keycaps.size() * sizeof(D2D1_RECT_F);
                p.height = m.height;
                out.width = std::max(out.width, x + m.widthIncludingTrailingWhitespace);
                out.height = std::max(out.height, p.y + m.height + padding);
                out.cost += (size_t)count * 64 + 1024;
                out.parts.push_back(std::move(p));
            };
            if (b.kind == Kind::Table && !b.cells.empty()) {
                float w = std::max(available, (float)b.cells.size() * 140 * zoom) / (float)b.cells.size();
                for (size_t i = 0; i < b.cells.size(); ++i) {
                    auto &c = b.cells[i];
                    make(c.start, c.length, indent + i * w + 10 * zoom, w - 20 * zoom, c.align);
                }
                out.width = indent + w * b.cells.size();
                out.height += 16 * zoom;
                for (auto &p : out.parts)
                    p.y += 8 * zoom;
            } else
                make(b.start, b.length, indent + padding, available - 2 * padding, 0);
            if (!source) {
                if (b.kind == Kind::Table) {
                    bool moreRows = index + 1 < blocks().size() && blocks()[index + 1].kind == Kind::Table &&
                                    !blocks()[index + 1].header;
                    out.tableGap = moreRows ? 0 : 14 * zoom;
                    out.height += out.tableGap;
                } else
                    out.height += b.continuation ? 0 : (informationView ? 20 : 14) * zoom;
            }
        }
        out.height = std::max(out.height, 1.f);
        heights.set(index, out.height);
        maxWidth = std::max(maxWidth, out.width + left() * 2);
        cacheCost += out.cost;
        auto [it, _] = cache.emplace(index, std::move(out));
        evict(index);
        return it->second;
    }
    void bars() {
        double page = std::max(1.f, height - top - bottom);
        scroll = std::clamp(scroll, 0., std::max(0., heights.total() - page));
        horizontal = std::clamp(horizontal, 0.f, std::max(0.f, maxWidth - width));
    }
    void stopScrolling() {
        scrollAnimating = false;
        KillTimer(hwnd, 4);
        scrollTarget = scroll;
    }
    void scrollTo(double destination) {
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        scrollTarget = std::clamp(destination, 0., std::max(0., heights.total() - (height - top - bottom)));
        if (!animations) {
            scroll = scrollTarget;
            stopScrolling();
            bars();
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
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - scrollTick).count();
        scrollTick = now;
        scrollTarget = std::clamp(scrollTarget, 0., std::max(0., heights.total() - (height - top - bottom)));
        scroll += (scrollTarget - scroll) * (1 - std::exp(-std::min(dt, .1) / .032));
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
        p.layout->HitTestTextRange(begin - p.start, end - begin, x, y, nullptr, 0, &n);
        if (!n)
            return;
        std::vector<DWRITE_HIT_TEST_METRICS> hits(n);
        if (SUCCEEDED(p.layout->HitTestTextRange(begin - p.start, end - begin, x, y, hits.data(), n, &n)))
            for (auto &h : hits)
                fill(D2D1::RectF(h.left, h.top, h.left + h.width, h.top + h.height), c, 2);
    }
    void paint() {
        auto paintStarted = std::chrono::steady_clock::now();
        PAINTSTRUCT ps;
        BeginPaint(canvas, &ps);
        if (!device()) {
            EndPaint(canvas, &ps);
            return;
        }
        auto colors = palette(dark);
        target->BeginDraw();
        target->Clear(colors.bg);
        bool paintedDocument = false;
        if (doc && !blocks().empty()) {
            target->PushAxisAlignedClip(D2D1::RectF(0, top, width, height - bottom),
                                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            try {
                size_t i = heights.find(scroll);
                double y = top + heights.before(i) - scroll;
                // Layout only visible content. Timer fills a small amount of look-ahead work.
                for (; i < blocks().size() && y < height - bottom; ++i) {
                    auto &b = blocks()[i];
                    auto &l = layout(i);
                    float x = documentLeft();
                    if (y + l.height >= top) {
                        paintedDocument = true;
                        if (!source && !l.plain && b.kind == Kind::Code)
                            fill(
                                D2D1::RectF(x, (float)y, x + contentWidth(), (float)y + l.height - 14 * zoom),
                                colors.code, 8);
                        if (b.quote && !source && !l.plain)
                            line(x + b.quote * 18 * zoom - 10 * zoom, (float)y,
                                 x + b.quote * 18 * zoom - 10 * zoom, (float)y + l.height - 12 * zoom,
                                 colors.accent, 3);
                        if (!l.plain && b.kind == Kind::Rule)
                            line(x, (float)y + 8 * zoom, x + contentWidth(), (float)y + 8 * zoom,
                                 colors.border);
                        if (!l.plain && b.kind == Kind::Table) {
                            float rowBottom = (float)y + l.height - l.tableGap;
                            if (b.header)
                                fill(D2D1::RectF(x, (float)y, x + l.width, rowBottom), colors.code);
                            if (b.header || i == 0 || blocks()[i - 1].kind != Kind::Table)
                                line(x, (float)y, x + l.width, (float)y, colors.border);
                            line(x, rowBottom, x + l.width, rowBottom, colors.border);
                            for (auto &p : l.parts)
                                line(x + p.x - 10 * zoom, (float)y, x + p.x - 10 * zoom, rowBottom,
                                     colors.border);
                            line(x + l.width, (float)y, x + l.width, rowBottom, colors.border);
                        }
                        if (!l.plain && b.kind == Kind::Image) {
                            if (l.bitmap)
                                target->DrawBitmap(
                                    l.bitmap.Get(),
                                    D2D1::RectF(x, (float)y, x + l.width, (float)y + l.height - 18 * zoom));
                            else {
                                fill(D2D1::RectF(x, (float)y, x + contentWidth(), (float)y + 48 * zoom),
                                     colors.code, 6);
                                label(b.length ? std::wstring_view(text()).substr(b.start, b.length)
                                               : L"Image",
                                      D2D1::RectF(x + 14, (float)y + 12, x + contentWidth() - 14,
                                                  (float)y + 46),
                                      colors.muted);
                            }
                        }
                        for (auto &p : l.parts) {
                            float px = x + p.x, py = (float)y + p.y;
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
                            target->DrawTextLayout({px, py}, p.layout.Get(), brush.Get(),
                                                   D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
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
        drawScrollbars(colors);
        HRESULT hr = target->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            target.Reset();
            brush.Reset();
            clearLayouts();
        }
        if (paintedDocument && !firstPaint) {
            firstPaint = true;
            recordMetric();
            if (exitOnContent)
                SetTimer(hwnd, 9, 100, nullptr);
        }
        EndPaint(canvas, &ps);
        if (paintedDocument && !verifyFile.empty())
            paintTimes.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - paintStarted)
                    .count());
        if (paintTimes.size() > 1000)
            paintTimes.erase(paintTimes.begin(), paintTimes.begin() + 500);
    }
    void recordMetric() {
        if (metricsFile.empty())
            return;
        std::ofstream out(std::filesystem::path(metricsFile), std::ios::app);
        double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        out << "{\"first_content_ms\":" << ms << ",\"open_ms\":"
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - openStarted)
                   .count()
            << ",\"read_ms\":" << doc->readMs << ",\"parse_ms\":" << doc->parseMs
            << ",\"blocks\":" << doc->blocks.size() << ",\"characters\":" << doc->source.size() << "}\n";
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
        POINT origin{(LONG)((width - 158) * scale()), (LONG)((height - 52) * scale())};
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
        dpi = (float)GetDpiForWindow(hwnd);
        width = std::max(1.f, r.right / scale() - 12);
        height = std::max(1.f, r.bottom / scale() - 12);
        float start = width - (fullscreen ? 7 : 6) * 38 - 20;
        for (int i = 0; i < 7; ++i)
            MoveWindow(buttons[i], (int)((start + i * 38) * scale()), (int)(20 * scale()),
                       (int)(34 * scale()), (int)(34 * scale()), TRUE);
        positionZoomOverlay();
        if (canvas)
            MoveWindow(canvas, 0, (int)(top * scale()), r.right,
                       std::max<LONG>(1, r.bottom - (int)(top * scale())), FALSE);
        invalidateHeader();
        positionSearch();
        if (backButton)
            MoveWindow(backButton, (int)(20 * scale()), (int)(20 * scale()), (int)(34 * scale()),
                       (int)(34 * scale()), TRUE);
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
        float start = width - (fullscreen ? 7 : 6) * 38 - 20;
        MoveWindow(search, (int)(36 * scale()), (int)(37 * scale()) - fieldHeight / 2,
                   (int)((std::max(100.f, start - 48) - 16) * scale()), fieldHeight, TRUE);
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
        dark = prefs.theme < 0 ? systemDark() : prefs.theme == 1;
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
        invalidateHeader();
        invalidate();
    }
    void refreshDpiAssets() {
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
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            (IUnknown **)write.GetAddressOf());
        if (!d2d || !write)
            throw std::runtime_error("Windows text rendering could not be initialized.");
        prefs = selfTest ? Preferences{} : readPreferences();
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
        const wchar_t *zoomNames[] = {L"Zoom out (Ctrl+-)", L"100%", L"Zoom in (Ctrl++)"};
        const wchar_t *zoomTips[] = {L"Zoom out (Ctrl+-)", L"Reset zoom to 100% (Ctrl+0)",
                                     L"Zoom in (Ctrl++)"};
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
        if (msg == WM_KEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
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
        if (GetOpenFileNameW(&dialog))
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
    void open(std::filesystem::path path) {
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
        openStarted = std::chrono::steady_clock::now();
        pendingPath = path;
        invalidate();
        auto cancel = std::make_shared<std::atomic_bool>(false),
             done = std::make_shared<std::atomic_bool>(false);
        auto gen = generation;
        HWND destination = hwnd;
        workers.push_back({std::thread([path, gen, destination, cancel, done] {
                               auto result = std::make_unique<LoadedDocument>();
                               result->generation = gen;
                               try {
                                   result->document = std::make_shared<Document>(load(path, cancel.get()));
                               } catch (const std::exception &e) {
                                   result->error = fromUtf8(e.what());
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
            notice = r->error;
            invalidate();
            return;
        }
        doc = std::move(r->document);
        source = false;
        rawBlocks = sourceBlocks(doc->source);
        anchor = caret = 0;
        searchMatch = false;
        notice.clear();
        SetWindowTextW(hwnd, (doc->path.filename().wstring() + L" — Voltura Markdown").c_str());
        resetLayout(false);
        invalidateHeader();
        if (searching)
            find(false);
        if (!verifyFile.empty())
            SetTimer(hwnd, 10, 120, nullptr);
    }
    void toggleSource() {
        if (!doc)
            return;
        cancelSearch();
        double ratio = heights.total() > 0 ? scroll / heights.total() : 0;
        source = !source;
        anchor = caret = 0;
        searchMatch = false;
        resetLayout(false);
        scroll = ratio * std::max(0., heights.total() - (height - top - bottom));
        bars();
        invalidate();
        SetFocus(hwnd);
    }
    void zoomBy(float next) {
        float previous = zoom;
        zoom = std::clamp(std::round(next * 10) / 10, .5f, 3.f);
        prefs.zoom = zoom;
        zoomUntil = GetTickCount64() + 2200;
        visibility(true);
        updateZoomControls();
        SetTimer(hwnd, 12, 2200, nullptr);
        if (zoom != previous)
            resetLayout();
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
        menuRows = {{130, L"Copy file", false, hasFile},
                    {131, L"Copy file name", false, hasFile},
                    {132, L"Copy full path", false, hasFile},
                    {133, L"Open file directory", false, hasFile},
                    {134, L"Edit file with…", false, hasFile},
                    {0, L"", true},
                    {120, L"Register for .md files…"},
                    {121, L"Unregister application"},
                    {0, L"", true},
                    {124, L"Start fullscreen", false, true, prefs.startFullscreen},
                    {122, L"Use Windows theme", false, true, prefs.theme < 0},
                    {125, L"Help (F1)", false, !loading},
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
        SIZE text{};
        GetTextExtentPoint32W(dc, row->text.c_str(), (int)row->text.size(), &text);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        item.itemWidth = text.cx + (UINT)(56 * scale());
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
            DrawTextW(dc, row.text.c_str(), (int)row.text.size(), &label,
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
        switch (id) {
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
            DestroyWindow(hwnd);
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

Open and read Markdown files. Select and copy text; the viewer does not edit or save documents.

Use the **back arrow** or <kbd>Escape</kbd> to return to your reading position.

## Open and read

- **Open a file:** use the folder icon or <kbd>Ctrl</kbd>+<kbd>O</kbd>.
- **Drag and drop:** drop a Markdown file into the window.
- **Reveal controls:** move the pointer. Hover over an icon to see its name.
- **Scroll:** <kbd>↑</kbd>/<kbd>↓</kbd> move vertically; <kbd>←</kbd>/<kbd>→</kbd> move horizontally across wide tables and code.

## Find, copy and zoom

- **Find text:** press <kbd>Ctrl</kbd>+<kbd>F</kbd>.
- **Next / previous match:** press <kbd>Enter</kbd> / <kbd>Shift</kbd>+<kbd>Enter</kbd>.
- **Copy text:** drag to select, then press <kbd>Ctrl</kbd>+<kbd>C</kbd>. <kbd>Ctrl</kbd>+<kbd>A</kbd> selects all.
- **Change text size:** use <kbd>Ctrl</kbd> + mouse wheel or <kbd>Ctrl</kbd>+<kbd>+</kbd> / <kbd>Ctrl</kbd>+<kbd>−</kbd>.
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
3. In Windows Default apps, select **Voltura Markdown** for **.md**.

Setup also offers **Choose as default Markdown reader**. You confirm the final choice in Windows.

## Startup and theme

In the **three-dot menu**:

- **Start fullscreen:** choose how future windows open.
- **Use Windows theme:** follow your system's light / dark setting.
)md");
    }
    void showInformation(const wchar_t *title, const std::wstring &content) {
        if (loading)
            return;
        auto information = std::make_shared<Document>(parse(content));
        if (informationView)
            closeInformation();
        size_t index = heights.values.empty() ? 0 : heights.find(scroll);
        double within = heights.values.empty() ? 0 : scroll - heights.before(index);
        informationView = std::make_unique<SavedView>(SavedView{
            doc, std::move(rawBlocks), source, scroll, horizontal, zoom, anchor, caret, index, within});
        cancelSearch();
        searching = false;
        KillTimer(hwnd, 3);
        ShowWindow(search, SW_HIDE);
        doc = std::move(information);
        source = false;
        rawBlocks = sourceBlocks(doc->source);
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
    }
    void showSearch() {
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
    void cancelSearch() {
        ++findGeneration;
        KillTimer(hwnd, 3);
        if (findCancel)
            findCancel->store(true);
    }
    void find(bool advance, bool backwards = false) {
        cancelSearch();
        if (!doc)
            return;
        int n = GetWindowTextLengthW(search);
        query.resize(n + 1);
        GetWindowTextW(search, query.data(), n + 1);
        query.resize(n);
        if (query.empty()) {
            if (notice == L"No matches")
                notice.clear();
            searchMatch = false;
            anchor = caret = 0;
            invalidate();
            return;
        }
        // Invariant ordinal, case-insensitive matching; scanning runs off the UI thread for large documents.
        auto document = doc;
        bool raw = source;
        auto q = query;
        uint32_t start = advance ? (backwards ? std::min(anchor, caret) : std::max(anchor, caret)) : 0;
        auto gen = findGeneration;
        findCancel = std::make_shared<std::atomic_bool>(false);
        auto cancel = findCancel;
        auto done = std::make_shared<std::atomic_bool>(false);
        HWND destination = hwnd;
        auto documentGen = generation;
        reap();
        workers.push_back(
            {std::thread([document, raw, q, start, gen, cancel, done, destination, documentGen, backwards] {
                 auto &s = raw || document->plainSource ? document->source : document->text;
                 int found = -1;
                 size_t count = s.size();
                 auto match = [&](size_t at) {
                     return at + q.size() <= count &&
                            CompareStringOrdinal(s.data() + at, (int)q.size(), q.data(), (int)q.size(),
                                                 TRUE) == CSTR_EQUAL;
                 };
                 if (!backwards) {
                     for (size_t pass = 0; pass < 2 && found < 0 && !cancel->load(); ++pass) {
                         size_t begin = pass ? 0 : std::min<size_t>(start, count),
                                end = pass ? std::min<size_t>(start, count) : count;
                         for (size_t i = begin; i < end; ++i) {
                             if ((i & 4095) == 0 && cancel->load())
                                 break;
                             if (match(i)) {
                                 found = (int)i;
                                 break;
                             }
                         }
                     }
                 } else {
                     for (size_t pass = 0; pass < 2 && found < 0 && !cancel->load(); ++pass) {
                         size_t begin = pass ? count : std::min<size_t>(start, count),
                                end = pass ? std::min<size_t>(start, count) : 0;
                         for (size_t i = begin; i > end;) {
                             --i;
                             if ((i & 4095) == 0 && cancel->load())
                                 break;
                             if (match(i)) {
                                 found = (int)i;
                                 break;
                             }
                         }
                     }
                 }
                 if (!cancel->load()) {
                     auto r = new SearchResult{gen, documentGen, raw, found, (uint32_t)q.size()};
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
    };
    uint64_t findGeneration = 0;
    std::shared_ptr<std::atomic_bool> findCancel;
    void searchResult(std::unique_ptr<SearchResult> r) {
        if (r->gen != findGeneration || r->documentGen != generation || r->source != source)
            return;
        if (r->found < 0) {
            notice = L"No matches";
            searchMatch = false;
            invalidate();
            return;
        }
        notice.clear();
        anchor = (uint32_t)r->found;
        caret = anchor + r->length;
        searchMatch = true;
        jump(anchor);
        invalidate();
    }
    void jump(uint32_t position) {
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
                p.layout->HitTestTextPosition(position - p.start, FALSE, &x, &y, &m);
                scroll += y;
                horizontal = std::max(0.f, left() + p.x + x - width / 2);
                break;
            }
        bars();
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
                            for (size_t i = 0; i < doc->blocks.size(); ++i)
                                if (doc->blocks[i].anchor == id) {
                                    scroll = heights.before(i);
                                    bars();
                                    invalidate();
                                    break;
                                }
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
                DestroyWindow(hwnd);
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
            SetFocus(shift && IsWindowVisible(zoomPanel) ? zoomButtons.back() : buttons[shift ? 5 : 0]);
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
        if (!verifyFile.empty() && item.CtlID >= 100 && item.CtlID < 106)
            ++buttonPaints[item.CtlID - 100];
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
        auto background = CreateSolidBrush(item.CtlID >= 140 && item.CtlID <= 142 ? RGB(255, 0, 255)
                                           : dark                                 ? RGB(16, 19, 25)
                                                                                  : RGB(250, 251, 253));
        FillRect(dc, &r, background);
        DeleteObject(background);
        auto b = CreateSolidBrush(bg);
        auto pen = CreatePen(PS_SOLID, 1, focus ? (dark ? RGB(144, 185, 255) : RGB(39, 95, 199)) : bg);
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
        if (!doc || loading || !target || blocks().empty())
            return;
        auto start = std::chrono::steady_clock::now();
        size_t i = heights.find(scroll), end = heights.find(scroll + 2 * (height - top));
        bool changed = false;
        for (; i <= end && i < blocks().size(); ++i) {
            if (!cache.contains(i)) {
                layout(i);
                changed = true;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(3))
                break;
        }
        if (changed) {
            bars();
            invalidate();
        }
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
            {
                auto popup = createMenu();
                check(GetMenuItemCount(popup) == 13, "file_operations_menu_items");
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
            check(fullscreen, "fullscreen_default");
            if (fullscreen)
                toggleFullscreen();
            check(!(GetWindowLongW(hwnd, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL)),
                  "no_native_scrollbar_resize_cycle");
            check(IsWindowVisible(buttons[0]) != FALSE, "toolbar_visible");
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
            check((COLORREF)SendMessageW(tooltip, TTM_GETTIPBKCOLOR, 0, 0) ==
                          (dark ? RGB(28, 34, 45) : RGB(255, 255, 255)) &&
                      (COLORREF)SendMessageW(tooltip, TTM_GETTIPTEXTCOLOR, 0, 0) ==
                          (dark ? RGB(229, 233, 240) : RGB(32, 41, 56)) &&
                      (HFONT)SendMessageW(tooltip, WM_GETFONT, 0, 0) == uiFont,
                  "tooltip_uses_current_theme_and_dpi_font");
            {
                auto original = doc;
                const double originalScroll = 10;
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
                check(!informationView && doc == original && std::abs(scroll - originalScroll) < 1,
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
                showAbout();
                key(VK_ESCAPE);
                check(!informationView && doc == original && std::abs(scroll - originalScroll) < 1,
                      "switching_help_to_about_preserves_original_reading_position");
            }
            zoomBy(1.2f);
            check(zoom > 1.19f, "zoom");
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
            if (!searchMatch) {
                --verifyStage;
                return;
            }
            check(caret > anchor && doc->text.substr(anchor, caret - anchor) == L"Voltura", "async_search");
            {
                auto staleGeneration = findGeneration;
                SetWindowTextW(search, L"");
                find(false);
                searchResult(
                    std::make_unique<SearchResult>(SearchResult{staleGeneration, generation, source, 0, 7}));
                check(!searchMatch && anchor == 0 && caret == 0, "cleared_search_rejects_pending_result");
                auto dismissedGeneration = findGeneration;
                key(VK_ESCAPE);
                searchResult(std::make_unique<SearchResult>(
                    SearchResult{dismissedGeneration, generation, source, -1, 7}));
                check(!searching && notice != L"No matches" && findGeneration != dismissedGeneration,
                      "dismissed_search_rejects_pending_result");
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
    void close() {
        closing = true;
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
        images.reset();
        if (!selfTest) {
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
        case WM_PAINT:
            paintHeader();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOVE:
            positionZoomOverlay();
            return 0;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED)
                return 0;
            ++viewportUpdates;
            positions();
            if (target && canvas) {
                RECT r;
                GetClientRect(canvas, &r);
                target->Resize(D2D1::SizeU(r.right, r.bottom));
            }
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
        case Loaded:
            loaded(std::unique_ptr<LoadedDocument>((LoadedDocument *)lp));
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
            invalidate();
            return 0;
        }
        case WM_TIMER:
            if (wp == 1)
                timer();
            else if (wp == 2) {
                if (fullscreen && !loading && !menuOpen && !dragging && !searching &&
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
            } else if (wp == 9) {
                KillTimer(hwnd, 9);
                DestroyWindow(hwnd);
            } else if (wp == 10)
                verifyUi();
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
            float x = GET_X_LPARAM(lp) / scale(), y = GET_Y_LPARAM(lp) / scale();
            SetFocus(hwnd);
            stopScrolling();
            if (doc && x >= width && y >= top && y <= height - bottom &&
                heights.total() > height - top - bottom) {
                auto r = verticalThumb();
                if (y < r.top || y > r.bottom) {
                    scroll += (y < r.top ? -1 : 1) * (height - top - bottom);
                    bars();
                } else {
                    scrollDrag = 1;
                    dragStart = y;
                    dragScroll = scroll;
                    SetCapture(hwnd);
                }
                invalidate();
                return 0;
            }
            if (doc && y >= height && maxWidth > width) {
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
            if (doc) {
                caret = hit(x, y);
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
            float x = GET_X_LPARAM(lp) / scale(), y = GET_Y_LPARAM(lp) / scale();
            if (scrollDrag == 1) {
                auto r = verticalThumb();
                scroll = dragScroll + (y - dragStart) *
                                          std::max(0., heights.total() - (height - top - bottom)) /
                                          std::max(1.f, height - top - bottom - (r.bottom - r.top));
                bars();
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
                caret = hit(GET_X_LPARAM(lp) / scale(), GET_Y_LPARAM(lp) / scale());
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
            open(path);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
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
        MessageBoxW(hwnd, fromUtf8(e.what()).c_str(), L"Voltura Markdown", MB_ICONERROR);
        if (msg == WM_CREATE)
            return -1;
        return 0;
    }
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    App app;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitCommonControls();
    int argc = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path path;
    UINT exitAfter = 0;
    bool configureDefaults = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = args[i];
        if (a == L"--configure-defaults")
            configureDefaults = true;
        else if (a == L"--verify-close-fullscreen")
            app.verifyCloseFullscreen = true;
        else if (a == L"--metrics" && i + 1 < argc)
            app.metricsFile = args[++i];
        else if (a == L"--benchmark") {
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
    if (!path.empty())
        app.open(path);
    if (app.prefs.startFullscreen)
        app.toggleFullscreen();
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    if (exitAfter)
        SetTimer(hwnd, 9, exitAfter, nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB && GetFocus() != hwnd) {
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
    return 0;
}
