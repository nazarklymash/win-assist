#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace wa {

// Captures `hwnd` (including occluded/DirectComposition content) as a PNG,
// scaled so the longest edge is at most `max_edge` pixels, base64-encoded.
std::optional<std::string> capture_window_png_base64(HWND hwnd, int max_edge, std::string& error);

}  // namespace wa
