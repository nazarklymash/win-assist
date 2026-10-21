#include "core/tools.h"

namespace wa {

ToolSet build_tools(const std::vector<PluginManifest>& manifests) {
    std::map<std::string, int> name_counts;
    for (const auto& m : manifests) {
        for (const auto& intent : m.intents) {
            if (intent.expose_to_llm) ++name_counts[intent.name];
        }
    }

    ToolSet set;
    for (const auto& m : manifests) {
        for (const auto& intent : m.intents) {
            if (!intent.expose_to_llm) continue;
            std::string name = name_counts[intent.name] > 1 ? m.id + "__" + intent.name : intent.name;

            nlohmann::json properties = nlohmann::json::object();
            nlohmann::json required = nlohmann::json::array();
            for (const auto& p : intent.parameters) {
                nlohmann::json prop = {{"type", p.type}};
                if (!p.description.empty()) prop["description"] = p.description;
                if (!p.enum_values.empty()) prop["enum"] = p.enum_values;
                properties[p.name] = std::move(prop);
                if (p.required) required.push_back(p.name);
            }
            set.tools.push_back({
                {"type", "function"},
                {"function",
                 {{"name", name},
                  {"description", intent.description.empty() ? m.description : intent.description},
                  {"parameters", {{"type", "object"}, {"properties", properties}, {"required", required}}}}},
            });
            set.by_name[name] = ToolTarget{m.id, intent.name};
        }
    }
    return set;
}

}  // namespace wa
