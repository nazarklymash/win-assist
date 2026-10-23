#include "core/manifest.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include "common/strings.h"

namespace wa {
namespace {

bool valid_identifier(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

std::optional<PluginManifest> parse_manifest(const nlohmann::json& j, const std::filesystem::path& dir,
                                             std::string& error) {
    try {
        PluginManifest m;
        m.dir = dir;
        m.id = j.at("id").get<std::string>();
        if (!valid_identifier(m.id)) {
            error = "invalid plugin id '" + m.id + "'";
            return std::nullopt;
        }
        m.name = j.value("name", m.id);
        m.version = j.value("version", "0.0.0");
        m.description = j.value("description", "");
        std::string type = j.value("type", "script");
        if (type == "native") {
            m.kind = PluginKind::Native;
        } else if (type == "script") {
            m.kind = PluginKind::Script;
        } else {
            error = "unknown plugin type '" + type + "'";
            return std::nullopt;
        }
        m.entry = j.at("entry").get<std::string>();
        m.runtime = j.value("runtime", m.kind == PluginKind::Native ? "" : "exe");
        m.timeout_ms = j.value("timeout_ms", 10000);
        // Plugin-wide capabilities apply to every intent it declares.
        auto plugin_caps = j.value("capabilities", std::vector<std::string>{});

        for (const auto& ji : j.at("intents")) {
            IntentDef intent;
            intent.name = ji.at("name").get<std::string>();
            if (!valid_identifier(intent.name)) {
                error = "invalid intent name '" + intent.name + "'";
                return std::nullopt;
            }
            intent.description = ji.value("description", "");
            intent.patterns = ji.value("patterns", std::vector<std::string>{});
            intent.expose_to_llm = ji.value("expose_to_llm", true);
            intent.capabilities = plugin_caps;
            for (auto& c : ji.value("capabilities", std::vector<std::string>{})) {
                if (std::find(intent.capabilities.begin(), intent.capabilities.end(), c) == intent.capabilities.end()) {
                    intent.capabilities.push_back(std::move(c));
                }
            }
            intent.always_confirm = ji.value("always_confirm", false);
            if (auto params = ji.find("parameters"); params != ji.end()) {
                for (const auto& [name, jp] : params->items()) {
                    IntentParam p;
                    p.name = name;
                    p.type = jp.value("type", "string");
                    p.description = jp.value("description", "");
                    p.required = jp.value("required", true);
                    p.enum_values = jp.value("enum", std::vector<std::string>{});
                    intent.parameters.push_back(std::move(p));
                }
            }
            m.intents.push_back(std::move(intent));
        }
        return m;
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

std::vector<PluginManifest> discover_manifests(const std::vector<std::filesystem::path>& dirs,
                                               std::vector<std::string>& errors) {
    std::map<std::string, PluginManifest> by_id;
    for (const auto& root : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (!entry.is_directory()) continue;
            auto file = entry.path() / L"manifest.json";
            if (!std::filesystem::exists(file, ec)) continue;
            std::ifstream in(file, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            std::string error;
            try {
                auto j = nlohmann::json::parse(buffer.str(), nullptr, true, true);
                if (auto m = parse_manifest(j, entry.path(), error)) {
                    by_id[m->id] = std::move(*m);
                    continue;
                }
            } catch (const std::exception& e) {
                error = e.what();
            }
            errors.push_back(to_utf8(file.wstring()) + ": " + error);
        }
    }
    std::vector<PluginManifest> out;
    out.reserve(by_id.size());
    for (auto& [id, m] : by_id) out.push_back(std::move(m));
    return out;
}

}  // namespace wa
