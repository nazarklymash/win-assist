#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <winassist/plugin_api.h>

#include "core/config.h"
#include "core/manifest.h"
#include "core/router.h"
#include "core/tools.h"
#include "daemon/plugins/action.h"
#include "daemon/system/target.h"

namespace wa {

inline constexpr const char* kBuiltinPluginId = "builtin";

struct InvokeContext {
    std::string utterance;
    TargetInfo target;
};

// Services the host exposes to native plugins.
struct PluginServices {
    std::function<std::string(const std::string& path, const std::string& body, bool& ok)> ollama_post;
    std::function<std::string()> config_json;
    // 0 = ok; non-zero = rejected.
    std::function<int(int delay_ms, const std::string& message)> schedule_reminder;
    // Asks the user to approve a gated action. Blocks the calling thread until
    // the user answers or the prompt times out. Not exposed to plugins.
    std::function<bool(const std::string& title, const std::string& detail, int timeout_ms)> confirm;
};

// Discovers plugin manifests, loads native DLLs, and routes invocations to
// the right transport. load()/unload() run on the UI thread while idle.
class PluginHost {
public:
    explicit PluginHost(PluginServices services);
    ~PluginHost();

    void load(const std::vector<std::filesystem::path>& dirs, const std::vector<PluginManifest>& builtin,
              std::vector<std::string>& errors);
    void unload();

    const Router& router() const { return router_; }
    const ToolSet& tools() const { return tools_; }
    const std::vector<PluginManifest>& manifests() const { return manifests_; }
    const PluginManifest* find(const std::string& id) const;
    const IntentDef* find_intent(const std::string& plugin_id, const std::string& intent) const;

    // Copied under lock so the worker thread sees a stable policy mid-session.
    void set_permissions(const PermissionsConfig& permissions);

    ActionResult invoke(const std::string& plugin_id, const std::string& intent, const nlohmann::json& params,
                        const InvokeContext& ctx, const std::atomic<bool>& cancel);

private:
    struct NativePlugin {
        HMODULE module = nullptr;
        wa_plugin_invoke_fn invoke = nullptr;
        wa_plugin_free_fn free = nullptr;
        wa_plugin_shutdown_fn shutdown = nullptr;
    };

    bool load_native(const PluginManifest& m, std::string& error);
    // Empty on success, otherwise the user-facing refusal reason.
    std::string check_permission(const PluginManifest& m, const IntentDef& intent, const nlohmann::json& params);

    static void host_log(void* host, int level, const char* message);
    static char* host_ollama_post(void* host, const char* path, const char* body);
    static void host_free_string(void* host, char* text);
    static char* host_config_json(void* host);
    static int host_schedule_reminder(void* host, int delay_ms, const char* message);

    PluginServices services_;
    wa_host_api api_{};
    std::vector<PluginManifest> manifests_;
    std::map<std::string, NativePlugin> native_;
    std::mutex native_call_mutex_;
    Router router_;
    ToolSet tools_;
    std::mutex permissions_mutex_;
    PermissionsConfig permissions_;
};

}  // namespace wa
