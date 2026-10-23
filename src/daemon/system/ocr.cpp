#include "daemon/system/ocr.h"

#include <winrt/Windows.Foundation.h>
// Needed for range-for over OcrResult::Lines().
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include "common/log.h"
#include "common/strings.h"
#include "daemon/system/screenshot.h"

namespace wa {
namespace {

using namespace winrt::Windows::Globalization;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Storage::Streams;

// The OCR engine rejects very large bitmaps; stay under its documented limit.
constexpr int kMaxEdge = 2000;

OcrEngine make_engine() {
    if (OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages()) return engine;
    // User profile languages may have no OCR pack; English usually does.
    try {
        return OcrEngine::TryCreateFromLanguage(Language(L"en-US"));
    } catch (const winrt::hresult_error&) {
        return nullptr;
    }
}

}  // namespace

bool ocr_available() {
    try {
        return make_engine() != nullptr;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

std::optional<std::string> ocr_png(const std::vector<uint8_t>& png, std::string& error) {
    if (png.empty()) {
        error = "no image to read";
        return std::nullopt;
    }
    try {
        OcrEngine engine = make_engine();
        if (!engine) {
            error = "Windows has no OCR language pack installed.";
            return std::nullopt;
        }

        InMemoryRandomAccessStream stream;
        DataWriter writer(stream);
        writer.WriteBytes(winrt::array_view<const uint8_t>(png.data(), png.data() + png.size()));
        writer.StoreAsync().get();
        writer.FlushAsync().get();
        writer.DetachStream();
        stream.Seek(0);

        BitmapDecoder decoder = BitmapDecoder::CreateAsync(stream).get();
        SoftwareBitmap bitmap =
            decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied).get();

        OcrResult result = engine.RecognizeAsync(bitmap).get();
        std::string text;
        for (const OcrLine& line : result.Lines()) {
            text += to_utf8(std::wstring(line.Text()));
            text += '\n';
        }
        return trim(text);
    } catch (const winrt::hresult_error& e) {
        error = to_utf8(std::wstring(e.message()));
        log::warn("ocr: {}", error);
        return std::nullopt;
    }
}

std::optional<std::string> ocr_window(HWND hwnd, std::string& error) {
    auto png = capture_window_png(hwnd, kMaxEdge, error);
    if (!png) return std::nullopt;
    return ocr_png(*png, error);
}

}  // namespace wa
