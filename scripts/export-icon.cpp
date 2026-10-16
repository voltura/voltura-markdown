// Development-only exporter. Links the exact native empty-state renderer into a WIC PNG writer.
#include "../src/empty-state.h"
#include <cstdio>
#include <cwchar>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return 3;
    const auto run = [&]() -> HRESULT {
        ComPtr<IWICImagingFactory> wic;
        HRESULT result =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (FAILED(result))
            return result;
        ComPtr<IWICBitmap> bitmap;
        result = wic->CreateBitmap(2048, 2048, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
        if (FAILED(result))
            return result;
        ComPtr<ID2D1Factory> factory;
        result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
        if (FAILED(result))
            return result;
        ComPtr<ID2D1RenderTarget> target;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        result = factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target);
        if (FAILED(result))
            return result;
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0, 0.f));
        target->SetTransform(D2D1::Matrix3x2F::Scale(10, 10));
        vm::drawEmptyState(target.Get(), D2D1::RectF(0, 0, 204.8f, 204.8f), true);
        result = target->EndDraw();
        if (FAILED(result))
            return result;
        ComPtr<IWICStream> stream;
        result = wic->CreateStream(&stream);
        if (FAILED(result))
            return result;
        result = stream->InitializeFromFilename(argv[1], GENERIC_WRITE);
        if (FAILED(result))
            return result;
        ComPtr<IWICBitmapEncoder> encoder;
        result = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(result))
            return result;
        result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(result))
            return result;
        ComPtr<IWICBitmapFrameEncode> frame;
        result = encoder->CreateNewFrame(&frame, nullptr);
        if (FAILED(result))
            return result;
        result = frame->Initialize(nullptr);
        if (FAILED(result))
            return result;
        result = frame->SetSize(2048, 2048);
        if (FAILED(result))
            return result;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        result = frame->SetPixelFormat(&format);
        if (FAILED(result))
            return result;
        result = frame->WriteSource(bitmap.Get(), nullptr);
        if (FAILED(result))
            return result;
        result = frame->Commit();
        if (FAILED(result))
            return result;
        return encoder->Commit();
    };
    hr = run();
    CoUninitialize();
    if (FAILED(hr))
        std::fwprintf(stderr, L"Icon export failed: 0x%08lx\n", hr);
    return FAILED(hr) ? 1 : 0;
}
