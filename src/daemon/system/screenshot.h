#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wa {

// Captures `hwnd` (including occluded/DirectComposition content) as a PNG,
// scaled so the longest edge is at most `max_edge` pixels.
std::optional<std::vector<uint8_t>> capture_window_png(HWND hwnd, int max_edge, std::string& error);

// Encodes a GDI bitmap as PNG, scaled to `max_edge`.
std::optional<std::vector<uint8_t>> encode_bitmap_png(HBITMAP bitmap, int width, int height, int max_edge,
                                                      std::string& error);

// Same capture, base64-encoded for the vision API.
std::optional<std::string> capture_window_png_base64(HWND hwnd, int max_edge, std::string& error);

}  // namespace wa
