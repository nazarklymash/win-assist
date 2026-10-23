#include <doctest/doctest.h>

#include "core/router.h"

using namespace wa;

namespace {

IntentDef make_intent(std::string name, std::vector<std::string> patterns) {
    IntentDef d;
    d.name = std::move(name);
    d.patterns = std::move(patterns);
    return d;
}

}  // namespace

TEST_CASE("normalize strips STT punctuation and whitespace") {
    CHECK(Router::normalize("  Open notepad.  ") == "Open notepad");
    CHECK(Router::normalize("\"What's up?\"") == "What's up");
    CHECK(Router::normalize("a   b\tc") == "a b c");
    CHECK(Router::normalize("...") == "");
}

TEST_CASE("normalize peels polite filler") {
    CHECK(Router::normalize("Can you open Discord for me please?") == "open Discord");
    CHECK(Router::normalize("hey could you launch Spotify") == "launch Spotify");
    CHECK(Router::normalize("будь ласка відкрий Discord") == "відкрий Discord");
}

TEST_CASE("captures parameters case-insensitively") {
    Router r;
    REQUIRE(r.add("open_app", make_intent("open_application", {"[please] (open|launch) [the] {app_name}"}), nullptr));
    auto m = r.match("Open Visual Studio Code.");
    REQUIRE(m);
    CHECK(m->plugin_id == "open_app");
    CHECK(m->parameters["app_name"] == "Visual Studio Code");

    m = r.match("please launch the calculator");
    REQUIRE(m);
    CHECK(m->parameters["app_name"] == "calculator");

    CHECK_FALSE(r.match("opening hours"));
    CHECK_FALSE(r.match("reopen notepad"));
}

TEST_CASE("optional alternation and literal-only patterns") {
    Router r;
    REQUIRE(r.add("builtin", make_intent("describe_screen", {"describe [my|the] screen", "what's on my screen"}), nullptr));
    CHECK(r.match("describe my screen"));
    CHECK(r.match("describe the screen"));
    CHECK(r.match("Describe screen"));
    CHECK(r.match("What's on my screen?"));
    CHECK_FALSE(r.match("describe your screen"));
}

TEST_CASE("more specific patterns win") {
    Router r;
    REQUIRE(r.add("a", make_intent("generic", {"{anything} up"}), nullptr));
    REQUIRE(r.add("b", make_intent("volume_up", {"(volume|sound) up"}), nullptr));
    auto m = r.match("volume up");
    REQUIRE(m);
    CHECK(m->intent == "volume_up");
}

TEST_CASE("soft match catches loose open phrasing") {
    Router r;
    REQUIRE(r.add("open_app", make_intent("open_application", {"[please] (open|launch) [the] {app_name}"}), nullptr));
    REQUIRE(r.add("file_search",
                  make_intent("find_file", {"[please] open [my|the] {query} (file|document|report|pdf)"}), nullptr));

    auto m = r.match("can you fire up Discord for me");
    REQUIRE(m);
    CHECK(m->plugin_id == "open_app");
    CHECK(m->parameters["app_name"] == "discord");

    m = r.match("find my invoice pdf");
    REQUIRE(m);
    CHECK(m->plugin_id == "file_search");
}

TEST_CASE("open app is not stolen by file search") {
    // file_search used to make the trailing "file|document|..." optional, so
    // "open Discord" matched find_file (more literal_chars) instead of open_app.
    Router r;
    REQUIRE(r.add("file_search",
                  make_intent("find_file", {"[please] open [my|the] {query} (file|document|report|pdf)",
                                            "відкрий [мій] (файл|документ) {query}"}),
                  nullptr));
    REQUIRE(r.add("open_app", make_intent("open_application", {"[please] (open|launch) [the] {app_name}",
                                                               "(відкрий|запусти) {app_name}"}),
                  nullptr));

    auto app = r.match("Open the Discord");
    REQUIRE(app);
    CHECK(app->plugin_id == "open_app");
    CHECK(app->parameters["app_name"] == "Discord");

    auto app2 = r.match("Open Discord.");
    REQUIRE(app2);
    CHECK(app2->plugin_id == "open_app");

    auto file = r.match("open my invoice file");
    REQUIRE(file);
    CHECK(file->plugin_id == "file_search");
    CHECK(file->parameters["query"] == "invoice");

    auto ua = r.match("відкрий Discord");
    REQUIRE(ua);
    CHECK(ua->plugin_id == "open_app");
}

TEST_CASE("multiple captures and trailing optional words") {
    Router r;
    REQUIRE(r.add("sys", make_intent("set_volume", {"set [the] volume to {level} [percent]"}), nullptr));
    auto m = r.match("Set the volume to 40 percent");
    REQUIRE(m);
    CHECK(m->parameters["level"] == "40");

    REQUIRE(r.add("t", make_intent("remind", {"remind me to {task} at {time}"}), nullptr));
    m = r.match("remind me to call mom at 5 pm");
    REQUIRE(m);
    CHECK(m->parameters["task"] == "call mom");
    CHECK(m->parameters["time"] == "5 pm");
}

TEST_CASE("regex metacharacters in literals are escaped") {
    Router r;
    REQUIRE(r.add("x", make_intent("calc", {"what is {a}+{b}"}), nullptr));
    auto m = r.match("what is 2+3");
    REQUIRE(m);
    CHECK(m->parameters["a"] == "2");
    CHECK(m->parameters["b"] == "3");
}

TEST_CASE("malformed patterns are rejected") {
    std::string error;
    CHECK_FALSE(compile_pattern("open {app", &error));
    CHECK_FALSE(compile_pattern("open [the app", &error));
    CHECK_FALSE(compile_pattern("open the) app", &error));
    CHECK_FALSE(compile_pattern("open {} now", &error));
}

TEST_CASE("unicode utterances") {
    Router r;
    REQUIRE(r.add("x", make_intent("open_application", {"(open|відкрий) {app_name}"}), nullptr));
    auto m = r.match("Відкрий Блокнот");
    REQUIRE(m);
    CHECK(m->parameters["app_name"] == "Блокнот");
}

TEST_CASE("selection and bilingual patterns") {
    Router r;
    REQUIRE(r.add("b", make_intent("summarize_selection",
                                   {"summarize [this|that|it|the selection]", "підсумуй [це|це виділення]"}),
                  nullptr));
    REQUIRE(r.add("b", make_intent("translate_selection",
                                   {"translate [this|that] [to|into] {language}", "переклади [це] [на|у] {language}"}),
                  nullptr));
    REQUIRE(r.add("s", make_intent("volume_up", {"louder", "гучніше"}), nullptr));

    auto a = r.match("Summarize this");
    REQUIRE(a);
    CHECK(a->intent == "summarize_selection");

    auto b = r.match("Підсумуй це");
    REQUIRE(b);
    CHECK(b->intent == "summarize_selection");

    auto c = r.match("переклади це на English");
    REQUIRE(c);
    CHECK(c->intent == "translate_selection");
    CHECK(c->parameters["language"] == "English");

    auto d = r.match("Гучніше");
    REQUIRE(d);
    CHECK(d->intent == "volume_up");
}
