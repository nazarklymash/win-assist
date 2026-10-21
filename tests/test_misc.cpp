#include <doctest/doctest.h>

#include <windows.h>

#include "common/pipe.h"
#include "core/base64.h"
#include "core/config.h"
#include "daemon/plugins/action.h"

using namespace wa;

TEST_CASE("base64 round trip and validation") {
    CHECK(base64_encode("", 0) == "");
    CHECK(base64_encode("f", 1) == "Zg==");
    CHECK(base64_encode("fo", 2) == "Zm8=");
    CHECK(base64_encode("foobar", 6) == "Zm9vYmFy");
    auto d = base64_decode("Zm9v\r\nYmE=");
    REQUIRE(d);
    CHECK(std::string(d->begin(), d->end()) == "fooba");
    CHECK_FALSE(base64_decode("Zm9v!"));
    CHECK_FALSE(base64_decode("Z"));
}

TEST_CASE("config fills missing keys with defaults") {
    std::string error;
    Config c = parse_config(R"({ "ollama": { "model": "llama3.2" }, /* comment */ "tts": { "enabled": false } })", &error);
    CHECK(error.empty());
    CHECK(c.ollama.model == "llama3.2");
    CHECK(c.ollama.host == "http://127.0.0.1:11434");
    CHECK_FALSE(c.tts.enabled);
    CHECK(c.hotkeys.assistant == "Alt+Space");

    parse_config("{ not json", &error);
    CHECK_FALSE(error.empty());
}

TEST_CASE("plugin output parsing") {
    auto r = ActionResult::from_output("log line\n{\"status\":\"ok\",\"display\":\"Opening Notepad\"}\n");
    CHECK(r.ok);
    CHECK(r.display == "Opening Notepad");

    r = ActionResult::from_output(R"({"status":"error","display":"nope"})");
    CHECK_FALSE(r.ok);

    r = ActionResult::from_output("just some text\n");
    CHECK(r.ok);
    CHECK(r.display == "just some text");
}

TEST_CASE("pipe framing over an anonymous pipe") {
    HANDLE read = nullptr, write = nullptr;
    REQUIRE(CreatePipe(&read, &write, nullptr, 1 << 16));
    CHECK(ipc::write_frame(write, std::string_view("{\"op\":\"x\"}")));
    std::string big(40000, 'z');
    CHECK(ipc::write_frame(write, big));
    CHECK(ipc::write_frame(write, std::string_view()));
    std::string frame;
    REQUIRE(ipc::read_frame(read, frame));
    CHECK(frame == "{\"op\":\"x\"}");
    REQUIRE(ipc::read_frame(read, frame));
    CHECK(frame == big);
    REQUIRE(ipc::read_frame(read, frame));
    CHECK(frame.empty());
    CloseHandle(write);
    CHECK_FALSE(ipc::read_frame(read, frame));
    CloseHandle(read);
}
