#include "images.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <shlwapi.h>
#include <stdexcept>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>
#pragma warning(push)
#pragma warning(disable : 4244 4456 4702)
#define NANOSVG_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvgrast.h"
#pragma warning(pop)
using Microsoft::WRL::ComPtr;
#include "svg-text.h"
namespace vm {
namespace {
constexpr size_t limit = 32 * 1024 * 1024;
struct Http {
    HINTERNET h = nullptr;
    Http(HINTERNET h = nullptr) : h(h) {}
    Http(const Http &) = delete;
    Http &operator=(const Http &) = delete;
    ~Http() {
        if (h)
            WinHttpCloseHandle(h);
    }
    operator HINTERNET() const {
        return h;
    }
};
void require(bool ok) {
    if (!ok)
        throw std::runtime_error("Image unavailable");
}
std::vector<unsigned char> readImage(const std::wstring &path, const std::atomic_bool &cancel,
                                     Http &session) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto active = [&] { require(!cancel.load() && std::chrono::steady_clock::now() < deadline); };
    active();
    if (path.starts_with(L"https://")) {
        // Created lazily on the loading thread. Keeping the session alive lets
        // WinHTTP reuse HTTPS connections for badges from the same host.
        if (!session.h) {
            session.h = WinHttpOpen(L"VolturaMarkdown/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session.h)
                WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000);
        }
        require(session.h != nullptr);
        std::wstring url = path;
        for (int hop = 0; hop < 5; ++hop) {
            active();
            require(url.starts_with(L"https://"));
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            parts.dwHostNameLength = (DWORD)-1;
            parts.dwUrlPathLength = (DWORD)-1;
            parts.dwExtraInfoLength = (DWORD)-1;
            require(WinHttpCrackUrl(url.c_str(), 0, 0, &parts) != FALSE);
            std::wstring host(parts.lpszHostName, parts.dwHostNameLength),
                resource(parts.lpszUrlPath, parts.dwUrlPathLength);
            if (parts.dwExtraInfoLength)
                resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
            if (resource.empty())
                resource = L"/";
            Http connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
            Http request(WinHttpOpenRequest(connection, L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
            require(request.h != nullptr);
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            DWORD disable = WINHTTP_DISABLE_COOKIES;
            WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
            active();
            require(WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                       0, 0) != FALSE);
            active();
            require(WinHttpReceiveResponse(request, nullptr) != FALSE);
            active();
            DWORD status = 0, n = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                                &status, &n, nullptr);
            if (status >= 300 && status < 400) {
                DWORD bytes = 0;
                WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, nullptr, nullptr, &bytes, nullptr);
                require(bytes && bytes < 32768);
                std::wstring next(bytes / 2, 0);
                require(WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, nullptr, next.data(), &bytes,
                                            nullptr) != FALSE);
                next.resize(wcslen(next.c_str()));
                std::wstring combined(32768, 0);
                DWORD length = (DWORD)combined.size();
                require(SUCCEEDED(UrlCombineW(url.c_str(), next.c_str(), combined.data(), &length, 0)));
                combined.resize(length);
                url = std::move(combined);
                continue;
            }
            require(status == 200);
            std::vector<unsigned char> data;
            unsigned char buffer[16384];
            DWORD count = 0;
            for (;;) {
                active();
                require(WinHttpReadData(request, buffer, sizeof(buffer), &count) != FALSE);
                active();
                if (!count)
                    break;
                require(data.size() + count <= limit);
                data.insert(data.end(), buffer, buffer + count);
            }
            return data;
        }
        throw std::runtime_error("Too many image redirects");
    }
    std::ifstream f(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    require((bool)f);
    auto size = f.tellg();
    require(size > 0 && (uint64_t)size <= limit);
    std::vector<unsigned char> b((size_t)size);
    f.seekg(0);
    require((bool)f.read((char *)b.data(), size));
    return b;
}
Image decodeImage(std::vector<unsigned char> bytes, float rasterScale = 1,
                  std::shared_ptr<const std::vector<unsigned char>> source = {}) {
    Image out;
    std::string_view text((char *)bytes.data(), bytes.size());
    bool svg = text.find("<svg") != text.npos;
    if (svg) {
        out.svgSource =
            source ? std::move(source) : std::make_shared<const std::vector<unsigned char>>(bytes);
        bytes.push_back(0);
        std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(parseSvgWithText((char *)bytes.data()),
                                                                nsvgDelete);
        require(image && image->width > 0 && image->height > 0 && std::isfinite(image->width) &&
                std::isfinite(image->height));
        for (auto shape = image->shapes; shape; shape = shape->next) {
            require(std::isfinite(shape->strokeDashOffset));
            for (int i = 0; i < shape->strokeDashCount; ++i)
                require(std::isfinite(shape->strokeDashArray[i]) && shape->strokeDashArray[i] >= 0);
        }
        float baseScale = std::min(1.f, 2048.f / std::max(image->width, image->height));
        float scale = std::min(rasterScale * baseScale, 4096.f / std::max(image->width, image->height));
        out.width = std::max(1u, (unsigned)std::ceil(image->width * scale));
        out.height = std::max(1u, (unsigned)std::ceil(image->height * scale));
        out.pixels.resize((size_t)out.width * out.height * 4);
        std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> raster(nsvgCreateRasterizer(),
                                                                                nsvgDeleteRasterizer);
        require(raster != nullptr);
        nsvgRasterize(raster.get(), image.get(), 0, 0, scale, out.pixels.data(), out.width, out.height,
                      out.width * 4);
        require(!raster->failed);
        for (size_t i = 0; i < out.pixels.size(); i += 4) {
            auto r = out.pixels[i], b = out.pixels[i + 2], a = out.pixels[i + 3];
            out.pixels[i] = (b * a + 127) / 255;
            out.pixels[i + 1] = (out.pixels[i + 1] * a + 127) / 255;
            out.pixels[i + 2] = (r * a + 127) / 255;
        }
    } else {
        ComPtr<IWICImagingFactory> factory;
        require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&factory))));
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(bytes.data(), (UINT)bytes.size()));
        require(stream != nullptr);
        ComPtr<IWICBitmapDecoder> decoder;
        require(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                           WICDecodeMetadataCacheOnDemand, &decoder)));
        ComPtr<IWICBitmapFrameDecode> frame;
        require(SUCCEEDED(decoder->GetFrame(0, &frame)));
        UINT w, h;
        require(SUCCEEDED(frame->GetSize(&w, &h)) && w && h && w <= 65536 && h <= 65536 &&
                (uint64_t)w * h <= 64000000);
        double scale = std::min(1., 2048. / std::max(w, h));
        out.width = std::max(1u, (unsigned)(w * scale));
        out.height = std::max(1u, (unsigned)(h * scale));
        ComPtr<IWICBitmapScaler> scaler;
        require(SUCCEEDED(factory->CreateBitmapScaler(&scaler)));
        require(SUCCEEDED(
            scaler->Initialize(frame.Get(), out.width, out.height, WICBitmapInterpolationModeFant)));
        ComPtr<IWICFormatConverter> converter;
        require(SUCCEEDED(factory->CreateFormatConverter(&converter)));
        require(SUCCEEDED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                WICBitmapDitherTypeNone, nullptr, 0,
                                                WICBitmapPaletteTypeCustom)));
        out.pixels.resize((size_t)out.width * out.height * 4);
        require(SUCCEEDED(
            converter->CopyPixels(nullptr, out.width * 4, (UINT)out.pixels.size(), out.pixels.data())));
    }
    out.ready = true;
    return out;
}
} // namespace
Image imageForExport(const std::wstring &url, const std::wstring &base, const std::atomic_bool &cancel) {
    std::wstring key = url;
    if (!url.starts_with(L"https://")) {
        require(url.find(L"://") == url.npos && !url.starts_with(L"data:"));
        std::wstring decoded(key.size() + 1, L'\0');
        DWORD count = (DWORD)decoded.size();
        require(SUCCEEDED(UrlUnescapeW(key.data(), decoded.data(), &count, URL_UNESCAPE_AS_UTF8)));
        decoded.resize(count);
        std::filesystem::path path(decoded);
        if (path.is_relative())
            path = std::filesystem::path(base).parent_path() / path;
        path = std::filesystem::absolute(path).lexically_normal();
        auto root = path.root_name().wstring();
        require(root.size() == 2 && root[1] == L':' &&
                GetDriveTypeW(path.root_path().c_str()) != DRIVE_REMOTE);
        key = path.wstring();
    }
    Http session;
    auto bytes = readImage(key, cancel, session);
    require(!cancel.load());
    return decodeImage(std::move(bytes));
}
Images::Images(HWND h) : window(h) {}
#ifdef VMD_TESTING
Images::Images(HWND h, std::function<Image(const std::wstring &, const std::atomic_bool &)> loader)
    : window(h), testLoader(std::move(loader)) {}
#endif
void Images::cancelPending() {
    auto next = std::make_shared<std::atomic_bool>(false);
    std::lock_guard lock(mutex);
    generationCancel->store(true);
    generationCancel = std::move(next);
    jobs.clear();
    for (auto it = cache.begin(); it != cache.end();) {
        if (!it->second->ready && !it->second->failed)
            it = cache.erase(it);
        else
            ++it;
    }
}
Images::~Images() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
        generationCancel->store(true);
        jobs.clear();
    }
    cv.notify_all();
    for (auto &t : workers)
        t.join();
}
void Images::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Http session;
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return stopping || !jobs.empty(); });
            if (stopping)
                break;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        Image result;
        try {
#ifdef VMD_TESTING
            if (testLoader)
                result = testLoader(job.key, *job.cancel);
            else
#endif
            {
                auto bytes = job.svgSource ? *job.svgSource : readImage(job.key, *job.cancel, session);
                require(!job.cancel->load());
                result = decodeImage(std::move(bytes), job.rasterScale, job.svgSource);
            }
        } catch (...) {
            result.failed = true;
            result.svgSource = job.svgSource;
        }
        {
            std::lock_guard lock(mutex);
            if (stopping.load())
                break;
            auto pending = cache.find(job.key);
            if (job.cancel->load() || pending == cache.end() || pending->second != job.pending)
                continue;
            size_t total = result.pixels.size() + (result.svgSource ? result.svgSource->size() : 0);
            for (const auto &[key, image] : cache)
                total += image->pixels.size() + (image->svgSource ? image->svgSource->size() : 0);
            for (auto it = cache.begin(); it != cache.end() && total > 64 * 1024 * 1024;) {
                if (it->first != job.key && it->second.use_count() == 1 && it->second->ready) {
                    total -= it->second->pixels.size() +
                             (it->second->svgSource ? it->second->svgSource->size() : 0);
                    it = cache.erase(it);
                } else
                    ++it;
            }
            // Pending images have never been returned to the UI. Publish under the
            // same lock without another allocation on this worker's failure path.
            *job.pending = std::move(result);
        }
        PostMessageW(window, WM_APP + 2, 0, 0);
    }
    CoUninitialize();
}
std::shared_ptr<Image> Images::get(const std::wstring &url, const std::wstring &base) {
    std::wstring key = url.substr(0, url.find(L'#'));
    if (!url.starts_with(L"https://")) {
        if (url.find(L"://") != url.npos || url.starts_with(L"data:"))
            return {};
        std::wstring decoded(key.size() + 1, 0);
        DWORD length = (DWORD)decoded.size();
        if (SUCCEEDED(UrlUnescapeW(key.data(), decoded.data(), &length, URL_UNESCAPE_AS_UTF8))) {
            decoded.resize(length);
            key = std::move(decoded);
        }
        auto path = std::filesystem::path(key);
        if (path.is_relative())
            path = std::filesystem::path(base).parent_path() / path;
        std::error_code error;
        path = std::filesystem::absolute(path, error).lexically_normal();
        auto root = path.root_name().wstring();
        if (error || root.size() != 2 || root[1] != L':' ||
            GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE) {
            auto failed = std::make_shared<Image>();
            failed->failed = true;
            return failed;
        }
        key = path.wstring();
    }
    std::lock_guard lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end()) {
        if (!it->second->ready && !it->second->failed)
            return {};
        return it->second;
    }
    if (jobs.size() >= 16)
        return {};
    // Network waits should not serialize an entire row of remote badges.
    // Local-only documents keep the existing two-worker footprint.
    size_t workerCount = url.starts_with(L"https://") ? 6 : 2;
    while (workers.size() < workerCount)
        workers.emplace_back([this] { run(); });
    size_t total = 0;
    for (auto &[k, v] : cache)
        total += v->pixels.size() + (v->svgSource ? v->svgSource->size() : 0);
    if (total > 64 * 1024 * 1024 || cache.size() >= 128) {
        for (auto i = cache.begin(); i != cache.end();) {
            if (i->second.use_count() == 1)
                i = cache.erase(i);
            else
                ++i;
        }
    }
    auto result = std::make_shared<Image>();
    cache.emplace(key, result);
    jobs.push_back({key, result, generationCancel});
    cv.notify_one();
    return {};
}
std::shared_ptr<Image> Images::raster(const std::shared_ptr<Image> &image, float pixelWidth,
                                      float pixelHeight, bool *pending) {
    if (pending)
        *pending = false;
    if (!image || !image->svgSource || !image->ready || !std::isfinite(pixelWidth) ||
        !std::isfinite(pixelHeight) || pixelWidth <= 0 || pixelHeight <= 0)
        return image;
    // Small buckets avoid rerasterizing for every fractional resize step. Always
    // round up so the destination never needs to magnify a low-resolution badge.
    float scale = std::ceil(std::max(pixelWidth / image->width, pixelHeight / image->height) * 8) / 8;
    scale = std::min(scale, 4096.f / std::max(image->width, image->height));
    // Keep the URL-addressed base and one raster together under the cache's
    // 64 MiB limit. Otherwise they evict each other on every subsequent paint.
    double remaining = 64. * 1024 * 1024 - image->pixels.size() - 2. * image->svgSource->size();
    double area = (double)image->width * image->height * 4;
    float budgetScale = (float)(std::floor(std::sqrt(std::max(0., remaining) / area) * 8) / 8);
    scale = std::min(scale, budgetScale);
    // Ceil-rounded raster dimensions must also fit (not just their float area).
    while (scale > 1 && std::ceil(image->width * scale) * std::ceil(image->height * scale) * 4 > remaining)
        scale -= .125f;
    if (scale <= 1)
        return image;
    auto key = L"svg:" + std::to_wstring((uintptr_t)image->svgSource.get()) + L":" + std::to_wstring(scale);
    std::lock_guard lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end()) {
        if (pending)
            *pending = !it->second->ready && !it->second->failed;
        return it->second->ready ? it->second : image;
    }
    if (pending)
        *pending = true;
    if (jobs.size() >= 16)
        return image;
    if (cache.size() >= 128) {
        for (auto i = cache.begin(); i != cache.end();) {
            if (i->second.use_count() == 1 && (i->second->ready || i->second->failed))
                i = cache.erase(i);
            else
                ++i;
        }
    }
    auto result = std::make_shared<Image>();
    cache.emplace(key, result);
    jobs.push_back({key, result, generationCancel, image->svgSource, scale, image});
    cv.notify_one();
    return image;
}
} // namespace vm
