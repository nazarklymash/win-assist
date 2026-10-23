#include <doctest/doctest.h>

#include "daemon/system/triggers.h"

using namespace wa;

namespace {

TriggerRule rule(std::string when, std::string match, int threshold = 20, int cooldown = 300) {
    TriggerRule r;
    r.name = when + ":" + match;
    r.when = std::move(when);
    r.match = std::move(match);
    r.threshold = threshold;
    r.cooldown_sec = cooldown;
    r.notify = "fired";
    return r;
}

SystemSnapshot snapshot(std::vector<std::string> processes, std::string title = {}, int battery = -1,
                        bool on_ac = true) {
    SystemSnapshot s;
    s.processes = std::move(processes);
    s.foreground_title = std::move(title);
    s.battery_percent = battery;
    s.on_ac = on_ac;
    return s;
}

}  // namespace

TEST_CASE("the first snapshot only establishes a baseline") {
    TriggerEngine engine;
    engine.set_rules({rule("app_launch", "code.exe")});
    CHECK(engine.evaluate(snapshot({"code.exe"}), 0).empty());
}

TEST_CASE("app launch and exit fire on the edge only") {
    TriggerEngine engine;
    engine.set_rules({rule("app_launch", "code.exe"), rule("app_exit", "code.exe")});
    engine.evaluate(snapshot({"explorer.exe"}), 0);

    auto launched = engine.evaluate(snapshot({"explorer.exe", "code.exe"}), 10);
    REQUIRE(launched.size() == 1);
    CHECK(launched[0].rule == "app_launch:code.exe");
    CHECK(launched[0].notify == "fired");

    CHECK(engine.evaluate(snapshot({"explorer.exe", "code.exe"}), 20).empty());

    auto exited = engine.evaluate(snapshot({"explorer.exe"}), 30);
    REQUIRE(exited.size() == 1);
    CHECK(exited[0].rule == "app_exit:code.exe");
}

TEST_CASE("process and title matching is case-insensitive") {
    TriggerEngine engine;
    engine.set_rules({rule("app_launch", "Code.EXE"), rule("window_title", "Pull Request")});
    engine.evaluate(snapshot({}, "inbox"), 0);
    auto fires = engine.evaluate(snapshot({"code.exe"}, "review pull request #12"), 10);
    CHECK(fires.size() == 2);
}

TEST_CASE("cooldown suppresses a repeat within the window") {
    TriggerEngine engine;
    engine.set_rules({rule("app_launch", "code.exe", 20, 100)});
    engine.evaluate(snapshot({}), 0);
    CHECK(engine.evaluate(snapshot({"code.exe"}), 10).size() == 1);
    engine.evaluate(snapshot({}), 20);
    CHECK(engine.evaluate(snapshot({"code.exe"}), 30).empty());   // 20 s < 100 s cooldown
    engine.evaluate(snapshot({}), 40);
    CHECK(engine.evaluate(snapshot({"code.exe"}), 200).size() == 1);
}

TEST_CASE("battery thresholds fire once per crossing") {
    TriggerEngine engine;
    engine.set_rules({rule("battery_below", "", 20)});
    engine.evaluate(snapshot({}, "", 50, false), 0);
    CHECK(engine.evaluate(snapshot({}, "", 19, false), 10).size() == 1);
    CHECK(engine.evaluate(snapshot({}, "", 15, false), 1000).empty());  // still below, no new edge
    engine.evaluate(snapshot({}, "", 80, true), 2000);
    CHECK(engine.evaluate(snapshot({}, "", 5, false), 3000).size() == 1);
}

TEST_CASE("a machine without a battery never fires battery rules") {
    TriggerEngine engine;
    engine.set_rules({rule("battery_below", "", 20)});
    engine.evaluate(snapshot({}), 0);
    CHECK(engine.evaluate(snapshot({}), 10).empty());
}

TEST_CASE("power source changes fire on transition") {
    TriggerEngine engine;
    engine.set_rules({rule("on_battery", ""), rule("on_ac", "")});
    engine.evaluate(snapshot({}, "", 90, true), 0);
    auto unplugged = engine.evaluate(snapshot({}, "", 90, false), 10);
    REQUIRE(unplugged.size() == 1);
    CHECK(unplugged[0].rule == "on_battery:");
    auto plugged = engine.evaluate(snapshot({}, "", 90, true), 20);
    REQUIRE(plugged.size() == 1);
    CHECK(plugged[0].rule == "on_ac:");
}

TEST_CASE("disabled, actionless and unknown rules are skipped") {
    TriggerEngine engine;
    TriggerRule disabled = rule("app_launch", "code.exe");
    disabled.enabled = false;
    TriggerRule actionless = rule("app_launch", "code.exe");
    actionless.notify.clear();
    TriggerRule unknown = rule("full_moon", "code.exe");
    engine.set_rules({disabled, actionless, unknown});
    engine.evaluate(snapshot({}), 0);
    CHECK(engine.evaluate(snapshot({"code.exe"}), 10).empty());
}

TEST_CASE("a snapshot of the real machine has plausible fields") {
    const SystemSnapshot now = take_snapshot();
    CHECK(now.processes.size() > 5);
    CHECK(now.battery_percent <= 100);
}
