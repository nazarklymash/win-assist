#include <doctest/doctest.h>

#include "core/manifest.h"
#include "core/tools.h"

using namespace wa;
using nlohmann::json;

namespace {

const char* kManifest = R"({
  "id": "open_app",
  "name": "Open Application",
  "type": "script",
  "runtime": "powershell",
  "entry": "open_app.ps1",
  "intents": [{
    "name": "open_application",
    "description": "Open an app",
    "patterns": ["open {app_name}"],
    "parameters": { "app_name": { "type": "string", "description": "App name" } }
  }]
})";

}  // namespace

TEST_CASE("parses a script manifest") {
    std::string error;
    auto m = parse_manifest(json::parse(kManifest), "C:/plugins/open_app", error);
    REQUIRE_MESSAGE(m, error);
    CHECK(m->id == "open_app");
    CHECK(m->kind == PluginKind::Script);
    CHECK(m->runtime == "powershell");
    CHECK(m->timeout_ms == 10000);
    REQUIRE(m->intents.size() == 1);
    REQUIRE(m->intents[0].parameters.size() == 1);
    CHECK(m->intents[0].parameters[0].name == "app_name");
    CHECK(m->intents[0].parameters[0].required);
}

TEST_CASE("rejects bad manifests") {
    std::string error;
    CHECK_FALSE(parse_manifest(json{{"id", "x"}}, "", error));
    CHECK_FALSE(parse_manifest(json{{"id", "bad id!"}, {"entry", "a"}, {"intents", json::array()}}, "", error));
    CHECK_FALSE(parse_manifest(json{{"id", "x"}, {"type", "wasm"}, {"entry", "a"}, {"intents", json::array()}}, "", error));
}

TEST_CASE("builds Ollama tool definitions with collision prefixes") {
    std::string error;
    auto a = parse_manifest(json::parse(kManifest), "", error);
    auto b_json = json::parse(kManifest);
    b_json["id"] = "other";
    auto b = parse_manifest(b_json, "", error);
    REQUIRE(a);
    REQUIRE(b);

    ToolSet single = build_tools({*a});
    REQUIRE(single.tools.size() == 1);
    const json& fn = single.tools[0]["function"];
    CHECK(fn["name"] == "open_application");
    CHECK(fn["parameters"]["required"] == json::array({"app_name"}));
    CHECK(single.by_name.at("open_application").plugin_id == "open_app");

    ToolSet both = build_tools({*a, *b});
    CHECK(both.by_name.count("open_app__open_application") == 1);
    CHECK(both.by_name.count("other__open_application") == 1);
}

TEST_CASE("intents hidden from the LLM are not tools") {
    auto j = json::parse(kManifest);
    j["intents"][0]["expose_to_llm"] = false;
    std::string error;
    auto m = parse_manifest(j, "", error);
    REQUIRE(m);
    CHECK(build_tools({*m}).tools.empty());
}

TEST_CASE("capabilities come from the plugin and the intent") {
    auto j = json::parse(kManifest);
    j["capabilities"] = json::array({"launch"});
    j["intents"][0]["capabilities"] = json::array({"shell", "launch"});
    j["intents"][0]["always_confirm"] = true;
    std::string error;
    auto m = parse_manifest(j, "", error);
    REQUIRE_MESSAGE(m, error);
    const auto& caps = m->intents[0].capabilities;
    // Plugin-wide first, intent additions appended without duplicates.
    REQUIRE(caps.size() == 2);
    CHECK(caps[0] == "launch");
    CHECK(caps[1] == "shell");
    CHECK(m->intents[0].always_confirm);
}

TEST_CASE("intents default to no capabilities and no forced confirmation") {
    std::string error;
    auto m = parse_manifest(json::parse(kManifest), "", error);
    REQUIRE(m);
    CHECK(m->intents[0].capabilities.empty());
    CHECK_FALSE(m->intents[0].always_confirm);
}
