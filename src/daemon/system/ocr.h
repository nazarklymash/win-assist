#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wa {

// Reads text from a PNG image with the offline Windows OCR engine.
// Returns nullopt when no OCR language pack is installed.
std::optional<std::string> ocr_png(const std::vector<uint8_t>& png, std::string& error);

// Captures `hwnd` and runs OCR over it.
std::optional<std::string> ocr_window(HWND hwnd, std::string& error);

// True when Windows has an OCR engine available for this user.
bool ocr_available();

}  // namespace wa
