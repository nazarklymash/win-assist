#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wa {

struct HotkeySpec {
    unsigned modifiers = 0;  // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
    unsigned vk = 0;
};

// Parses strings like "Alt+Space", "Ctrl+Alt+Space", "Win+Shift+F9".
std::optional<HotkeySpec> parse_hotkey(std::string_view text);
std::string format_hotkey(const HotkeySpec& spec);

}  // namespace wa
