#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/manifest.h"

namespace wa {

struct ToolTarget {
    std::string plugin_id;
    std::string intent;
};

struct ToolSet {
    nlohmann::json tools = nlohmann::json::array();  // Ollama /api/chat "tools" format
    std::map<std::string, ToolTarget> by_name;
};

// Turns plugin intents into function-calling tool definitions. Tool names are
// the intent names, prefixed with "<plugin>__" only when two plugins collide.
ToolSet build_tools(const std::vector<PluginManifest>& manifests);

}  // namespace wa
