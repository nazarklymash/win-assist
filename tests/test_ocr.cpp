#include <doctest/doctest.h>

#include <windows.h>

#include <string>

#include "daemon/system/ocr.h"
#include "daemon/system/screenshot.h"

using namespace wa;

namespace {

// Renders `text` as black-on-white into a PNG so OCR has something to read.
std::vector<uint8_t> render_text_png(const wchar_t* text, std::string& error) {
    constexpr int kW = 900;
    constexpr int kH = 220;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = kW;
    bmi.bmiHeader.biHeight = -kH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bitmap = SelectObject(mem, bitmap);

    RECT rc{0, 0, kW, kH};
    FillRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HFONT font = CreateFontW(96, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(mem, font);
    SetTextColor(mem, RGB(0, 0, 0));
    SetBkMode(mem, TRANSPARENT);
    DrawTextW(mem, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, old_font);
    DeleteObject(font);

    auto* px = static_cast<uint32_t*>(bits);
    for (size_t i = 0, n = static_cast<size_t>(kW) * kH; i < n; ++i) px[i] |= 0xFF000000u;

    SelectObject(mem, old_bitmap);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    auto png = encode_bitmap_png(bitmap, kW, kH, 2000, error);
    DeleteObject(bitmap);
    return png ? *png : std::vector<uint8_t>{};
}

}  // namespace

TEST_CASE("OCR reads rendered text") {
    if (!ocr_available()) {
        WARN("no Windows OCR language pack installed; skipping");
        return;
    }
    std::string error;
    auto png = render_text_png(L"HELLO WORLD", error);
    REQUIRE_MESSAGE(!png.empty(), error);

    auto text = ocr_png(png, error);
    REQUIRE_MESSAGE(text, error);
    std::string upper;
    for (char c : *text) upper += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    CHECK(upper.find("HELLO") != std::string::npos);
    CHECK(upper.find("WORLD") != std::string::npos);
}

TEST_CASE("OCR rejects an empty image") {
    std::string error;
    CHECK_FALSE(ocr_png({}, error));
    CHECK_FALSE(error.empty());
}
