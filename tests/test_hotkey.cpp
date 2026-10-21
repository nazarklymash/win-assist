#include <doctest/doctest.h>

#include <windows.h>

#include "core/hotkey.h"

using namespace wa;

TEST_CASE("parses modifier combinations") {
    auto h = parse_hotkey("Alt+Space");
    REQUIRE(h);
    CHECK(h->modifiers == MOD_ALT);
    CHECK(h->vk == VK_SPACE);

    h = parse_hotkey("ctrl + alt + space");
    REQUIRE(h);
    CHECK(h->modifiers == (MOD_CONTROL | MOD_ALT));

    h = parse_hotkey("Win+Shift+F9");
    REQUIRE(h);
    CHECK(h->modifiers == (MOD_WIN | MOD_SHIFT));
    CHECK(h->vk == VK_F9);

    h = parse_hotkey("Ctrl+`");
    REQUIRE(h);
    CHECK(h->vk == VK_OEM_3);
}

TEST_CASE("rejects invalid hotkeys") {
    CHECK_FALSE(parse_hotkey(""));
    CHECK_FALSE(parse_hotkey("Ctrl+Alt"));
    CHECK_FALSE(parse_hotkey("Ctrl+A+B"));
    CHECK_FALSE(parse_hotkey("Hyper+Space"));
    CHECK_FALSE(parse_hotkey("F25"));
}

TEST_CASE("formats round-trip") {
    for (const char* text : {"Alt+Space", "Ctrl+Alt+Space", "Ctrl+Shift+K", "Win+F12"}) {
        auto h = parse_hotkey(text);
        REQUIRE(h);
        CHECK(format_hotkey(*h) == text);
    }
}
