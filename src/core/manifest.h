#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace wa {

struct IntentParam {
    std::string name;
    std::string type = "string";
    std::string description;
    bool required = true;
    std::vector<std::string> enum_values;
};

struct IntentDef {
    std::string name;
    std::string description;
    // Utterance templates, e.g. "open [the] {app_name}" or "(volume|sound) up".
    std::vector<std::string> patterns;
    std::vector<IntentParam> parameters;
    // When false the intent is only reachable through explicit patterns, never
    // offered to the LLM as a tool.
    bool expose_to_llm = true;
    // Capability tags ("process_kill", "shell", "power", …) checked against
    // config.permissions before the intent runs.
    std::vector<std::string> capabilities;
    // Forces a yes/no prompt regardless of capability configuration.
    bool always_confirm = false;
};

enum class PluginKind { Native, Script, Builtin };

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    PluginKind kind = PluginKind::Script;
    std::string runtime;  // script plugins: powershell | python | node | exe | cmd
    std::string entry;
    std::filesystem::path dir;
    int timeout_ms = 10000;
    std::vector<IntentDef> intents;
};

std::optional<PluginManifest> parse_manifest(const nlohmann::json& j, const std::filesystem::path& dir,
                                             std::string& error);

// Scans each directory for `<plugin>/manifest.json`. Later directories override
// plugins with the same id from earlier ones.
std::vector<PluginManifest> discover_manifests(const std::vector<std::filesystem::path>& dirs,
                                               std::vector<std::string>& errors);

}  // namespace wa
