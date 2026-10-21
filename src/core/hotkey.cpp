#include "core/hotkey.h"

#include <windows.h>

#include <array>
#include <utility>

#include "common/strings.h"

namespace wa {
namespace {

struct NamedKey {
    const char* name;
    unsigned vk;
};

constexpr std::array kNamedKeys = {
    NamedKey{"space", VK_SPACE},       NamedKey{"enter", VK_RETURN},     NamedKey{"return", VK_RETURN},
    NamedKey{"tab", VK_TAB},           NamedKey{"backspace", VK_BACK},   NamedKey{"insert", VK_INSERT},
    NamedKey{"delete", VK_DELETE},     NamedKey{"home", VK_HOME},        NamedKey{"end", VK_END},
    NamedKey{"pageup", VK_PRIOR},      NamedKey{"pagedown", VK_NEXT},    NamedKey{"up", VK_UP},
    NamedKey{"down", VK_DOWN},         NamedKey{"left", VK_LEFT},        NamedKey{"right", VK_RIGHT},
    NamedKey{"pause", VK_PAUSE},       NamedKey{"scrolllock", VK_SCROLL}, NamedKey{"escape", VK_ESCAPE},
    NamedKey{"esc", VK_ESCAPE},        NamedKey{"`", VK_OEM_3},          NamedKey{"backquote", VK_OEM_3},
    NamedKey{";", VK_OEM_1},           NamedKey{"/", VK_OEM_2},          NamedKey{".", VK_OEM_PERIOD},
    NamedKey{",", VK_OEM_COMMA},       NamedKey{"-", VK_OEM_MINUS},      NamedKey{"=", VK_OEM_PLUS},
};

std::optional<unsigned> key_from_name(const std::string& lower) {
    if (lower.size() == 1) {
        char c = lower[0];
        if (c >= 'a' && c <= 'z') return static_cast<unsigned>('A' + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c);
    }
    if (lower.size() >= 2 && lower[0] == 'f') {
        int n = 0;
        for (size_t i = 1; i < lower.size(); ++i) {
            if (lower[i] < '0' || lower[i] > '9') return std::nullopt;
            n = n * 10 + (lower[i] - '0');
        }
        if (n >= 1 && n <= 24) return static_cast<unsigned>(VK_F1 + n - 1);
        return std::nullopt;
    }
    for (const auto& key : kNamedKeys) {
        if (lower == key.name) return key.vk;
    }
    return std::nullopt;
}

}  // namespace

std::optional<HotkeySpec> parse_hotkey(std::string_view text) {
    HotkeySpec spec;
    size_t start = 0;
    bool have_key = false;
    while (start <= text.size()) {
        size_t plus = text.find('+', start);
        // A trailing "+" token (e.g. "Ctrl++") is not supported; "=" is the plus key.
        std::string token = to_lower_ascii(trim(text.substr(start, plus == std::string_view::npos ? text.npos : plus - start)));
        if (token.empty()) return std::nullopt;
        if (token == "ctrl" || token == "control") {
            spec.modifiers |= MOD_CONTROL;
        } else if (token == "alt") {
            spec.modifiers |= MOD_ALT;
        } else if (token == "shift") {
            spec.modifiers |= MOD_SHIFT;
        } else if (token == "win" || token == "super") {
            spec.modifiers |= MOD_WIN;
        } else {
            if (have_key) return std::nullopt;
            auto vk = key_from_name(token);
            if (!vk) return std::nullopt;
            spec.vk = *vk;
            have_key = true;
        }
        if (plus == std::string_view::npos) break;
        start = plus + 1;
    }
    if (!have_key) return std::nullopt;
    return spec;
}

std::string format_hotkey(const HotkeySpec& spec) {
    std::string out;
    if (spec.modifiers & MOD_CONTROL) out += "Ctrl+";
    if (spec.modifiers & MOD_ALT) out += "Alt+";
    if (spec.modifiers & MOD_SHIFT) out += "Shift+";
    if (spec.modifiers & MOD_WIN) out += "Win+";
    if (spec.vk >= 'A' && spec.vk <= 'Z') return out + static_cast<char>(spec.vk);
    if (spec.vk >= '0' && spec.vk <= '9') return out + static_cast<char>(spec.vk);
    if (spec.vk >= VK_F1 && spec.vk <= VK_F24) return out + "F" + std::to_string(spec.vk - VK_F1 + 1);
    for (const auto& key : kNamedKeys) {
        if (key.vk == spec.vk) {
            std::string name = key.name;
            if (!name.empty() && name[0] >= 'a' && name[0] <= 'z') name[0] = static_cast<char>(name[0] - 32);
            return out + name;
        }
    }
    return out + "VK" + std::to_string(spec.vk);
}

}  // namespace wa
