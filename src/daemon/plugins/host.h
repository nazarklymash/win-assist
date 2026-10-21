#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <winassist/plugin_api.h>

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

    static void host_log(void* host, int level, const char* message);
    static char* host_ollama_post(void* host, const char* path, const char* body);
    static void host_free_string(void* host, char* text);
    static char* host_config_json(void* host);

    PluginServices services_;
    wa_host_api api_{};
    std::vector<PluginManifest> manifests_;
    std::map<std::string, NativePlugin> native_;
    std::mutex native_call_mutex_;
    Router router_;
    ToolSet tools_;
};

}  // namespace wa
