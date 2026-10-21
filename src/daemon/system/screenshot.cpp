#include "daemon/system/screenshot.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <vector>

#include "core/base64.h"

using Microsoft::WRL::ComPtr;

namespace wa {
namespace {

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

}  // namespace

std::optional<std::string> capture_window_png_base64(HWND hwnd, int max_edge, std::string& error) {
    RECT rc{};
    if (!hwnd || !GetWindowRect(hwnd, &rc)) {
        error = "no window to capture";
        return std::nullopt;
    }
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        error = "window has no area";
        return std::nullopt;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bitmap);
    if (!PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)) {
        BitBlt(mem, 0, 0, width, height, screen, rc.left, rc.top, SRCCOPY);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    // PrintWindow leaves alpha undefined; force opaque pixels.
    auto* px = static_cast<uint32_t*>(bits);
    for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i) px[i] |= 0xFF000000u;

    std::optional<std::string> result;
    {
        ComScope com;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmap> wic_bitmap;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &wic_bitmap);

        double scale = 1.0;
        if (width > max_edge || height > max_edge) scale = static_cast<double>(max_edge) / (width > height ? width : height);
        const UINT out_w = static_cast<UINT>(width * scale);
        const UINT out_h = static_cast<UINT>(height * scale);
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(wic_bitmap.Get(), out_w, out_h, WICBitmapInterpolationModeFant);
        if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(out_w, out_h);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(scaler.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();

        HGLOBAL global = nullptr;
        if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream.Get(), &global);
        if (SUCCEEDED(hr)) {
            STATSTG stat{};
            stream->Stat(&stat, STATFLAG_NONAME);
            const void* data = GlobalLock(global);
            result = base64_encode(data, static_cast<size_t>(stat.cbSize.QuadPart));
            GlobalUnlock(global);
        } else {
            error = "PNG encoding failed";
        }
    }
    DeleteObject(bitmap);
    return result;
}

}  // namespace wa
