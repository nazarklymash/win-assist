#include "daemon/plugins/host.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "common/strings.h"
#include "daemon/plugins/script_runner.h"

namespace wa {
namespace {

char* dup_string(const std::string& s) {
    auto* out = static_cast<char*>(malloc(s.size() + 1));
    if (out) memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

}  // namespace

PluginHost::PluginHost(PluginServices services) : services_(std::move(services)) {
    api_.api_version = WA_PLUGIN_API_VERSION;
    api_.host = this;
    api_.log = &PluginHost::host_log;
    api_.ollama_post = &PluginHost::host_ollama_post;
    api_.free_string = &PluginHost::host_free_string;
    api_.config_json = &PluginHost::host_config_json;
    api_.schedule_reminder = &PluginHost::host_schedule_reminder;
}

PluginHost::~PluginHost() { unload(); }

void PluginHost::host_log(void*, int level, const char* message) {
    if (!message) return;
    if (level >= WA_LOG_ERROR) log::error("plugin: {}", message);
    else if (level == WA_LOG_WARN) log::warn("plugin: {}", message);
    else log::info("plugin: {}", message);
}

char* PluginHost::host_ollama_post(void* host, const char* path, const char* body) {
    auto* self = static_cast<PluginHost*>(host);
    if (!self->services_.ollama_post || !path) return nullptr;
    bool ok = false;
    std::string response = self->services_.ollama_post(path, body ? body : "", ok);
    return ok ? dup_string(response) : nullptr;
}

void PluginHost::host_free_string(void*, char* text) { free(text); }

char* PluginHost::host_config_json(void* host) {
    auto* self = static_cast<PluginHost*>(host);
    return self->services_.config_json ? dup_string(self->services_.config_json()) : nullptr;
}

int PluginHost::host_schedule_reminder(void* host, int delay_ms, const char* message) {
    auto* self = static_cast<PluginHost*>(host);
    if (!self->services_.schedule_reminder) return 1;
    return self->services_.schedule_reminder(delay_ms, message ? message : "");
}

const PluginManifest* PluginHost::find(const std::string& id) const {
    for (const auto& m : manifests_) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const IntentDef* PluginHost::find_intent(const std::string& plugin_id, const std::string& intent) const {
    const PluginManifest* m = find(plugin_id);
    if (!m) return nullptr;
    for (const auto& i : m->intents) {
        if (i.name == intent) return &i;
    }
    return nullptr;
}

void PluginHost::set_permissions(const PermissionsConfig& permissions) {
    std::lock_guard lock(permissions_mutex_);
    permissions_ = permissions;
}

std::string PluginHost::check_permission(const PluginManifest& m, const IntentDef& intent,
                                         const nlohmann::json& params) {
    PermissionsConfig policy;
    {
        std::lock_guard lock(permissions_mutex_);
        policy = permissions_;
    }
    if (!policy.enabled) return {};
    if (intent.capabilities.empty() && !intent.always_confirm) return {};

    const std::string key = m.id + "/" + intent.name;
    auto listed = [](const std::vector<std::string>& list, const std::string& value) {
        return std::find(list.begin(), list.end(), value) != list.end();
    };

    for (const auto& cap : intent.capabilities) {
        if (listed(policy.deny, cap)) {
            if (policy.audit) log::warn("permission: denied {} (capability '{}' is blocked)", key, cap);
            return "That action is blocked by your permission settings (" + cap + ").";
        }
    }

    if (listed(policy.allow_intents, key)) {
        if (policy.audit) log::info("permission: {} pre-approved", key);
        return {};
    }

    bool needs_confirm = intent.always_confirm;
    std::string reason;
    for (const auto& cap : intent.capabilities) {
        if (listed(policy.confirm, cap)) {
            needs_confirm = true;
            reason = cap;
            break;
        }
    }
    if (!needs_confirm) return {};

    if (!services_.confirm) {
        log::warn("permission: no confirmation UI available, refusing {}", key);
        return "This action needs confirmation, but no prompt is available.";
    }

    std::string detail = intent.description.empty() ? key : intent.description;
    if (params.is_object() && !params.empty()) detail += "\n" + params.dump();
    if (policy.audit) log::info("permission: asking to allow {} ({})", key, reason.empty() ? "always_confirm" : reason);
    const bool allowed = services_.confirm("Allow " + intent.name + "?", detail, policy.confirm_timeout_ms);
    if (policy.audit) log::info("permission: {} {}", key, allowed ? "ALLOWED by user" : "DENIED");
    if (!allowed) return "Cancelled \u2014 you did not approve " + intent.name + ".";
    return {};
}

bool PluginHost::load_native(const PluginManifest& m, std::string& error) {
    const auto dll = m.dir / to_wide(m.entry);
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        error = "LoadLibrary failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    auto get_info = reinterpret_cast<wa_plugin_get_info_fn>(GetProcAddress(module, "wa_plugin_get_info"));
    NativePlugin p;
    p.module = module;
    p.invoke = reinterpret_cast<wa_plugin_invoke_fn>(GetProcAddress(module, "wa_plugin_invoke"));
    p.free = reinterpret_cast<wa_plugin_free_fn>(GetProcAddress(module, "wa_plugin_free"));
    p.shutdown = reinterpret_cast<wa_plugin_shutdown_fn>(GetProcAddress(module, "wa_plugin_shutdown"));
    auto init = reinterpret_cast<wa_plugin_init_fn>(GetProcAddress(module, "wa_plugin_init"));
    const wa_plugin_info* info = get_info ? get_info() : nullptr;
    if (!info || !p.invoke || !p.free) {
        error = "missing required exports";
        FreeLibrary(module);
        return false;
    }
    if (info->api_version != WA_PLUGIN_API_VERSION) {
        error = "plugin API version " + std::to_string(info->api_version) + " != " + std::to_string(WA_PLUGIN_API_VERSION);
        FreeLibrary(module);
        return false;
    }
    if (init && init(&api_) != 0) {
        error = "wa_plugin_init failed";
        FreeLibrary(module);
        return false;
    }
    native_[m.id] = p;
    return true;
}

void PluginHost::load(const std::vector<std::filesystem::path>& dirs, const std::vector<PluginManifest>& builtin,
                      std::vector<std::string>& errors) {
    unload();
    std::vector<PluginManifest> found = discover_manifests(dirs, errors);
    manifests_ = builtin;
    for (auto& m : found) {
        if (m.id == kBuiltinPluginId) {
            errors.push_back("plugin id 'builtin' is reserved");
            continue;
        }
        if (m.kind == PluginKind::Native) {
            std::string error;
            if (!load_native(m, error)) {
                errors.push_back(m.id + ": " + error);
                continue;
            }
        }
        manifests_.push_back(std::move(m));
    }

    for (const auto& m : manifests_) {
        for (const auto& intent : m.intents) {
            std::string error;
            if (!router_.add(m.id, intent, &error)) errors.push_back(m.id + "/" + intent.name + ": " + error);
        }
    }
    tools_ = build_tools(manifests_);
    for (const auto& e : errors) log::warn("plugins: {}", e);
    log::info("plugins: {} loaded, {} patterns, {} tools", manifests_.size(), router_.size(), tools_.tools.size());
}

void PluginHost::unload() {
    for (auto& [id, p] : native_) {
        if (p.shutdown) p.shutdown();
        FreeLibrary(p.module);
    }
    native_.clear();
    manifests_.clear();
    router_.clear();
    tools_ = {};
}

ActionResult PluginHost::invoke(const std::string& plugin_id, const std::string& intent, const nlohmann::json& params,
                                const InvokeContext& ctx, const std::atomic<bool>& cancel) {
    const PluginManifest* m = find(plugin_id);
    if (!m) return ActionResult::error("Unknown plugin '" + plugin_id + "'.");

    if (const IntentDef* def = find_intent(plugin_id, intent)) {
        if (std::string refusal = check_permission(*m, *def, params); !refusal.empty()) {
            return ActionResult::error(refusal);
        }
    }

    nlohmann::json request = {
        {"intent", intent},
        {"parameters", params.is_object() ? params : nlohmann::json::object()},
        {"utterance", ctx.utterance},
        {"active_window", ctx.target.to_json()},
    };
    log::info("invoke {}/{} {}", plugin_id, intent, request["parameters"].dump());

    if (m->kind == PluginKind::Script) return run_script_plugin(*m, request.dump(), cancel);

    auto it = native_.find(plugin_id);
    if (it == native_.end()) return ActionResult::error("Plugin '" + plugin_id + "' is not loaded.");
    std::lock_guard lock(native_call_mutex_);
    char* raw = it->second.invoke(request.dump().c_str());
    std::string output = raw ? raw : "";
    if (raw) it->second.free(raw);
    return ActionResult::from_output(output);
}

}  // namespace wa
