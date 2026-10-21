#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/llm.h"
#include "core/manifest.h"
#include "daemon/ai/providers.h"
#include "daemon/ai/stt.h"
#include "daemon/plugins/host.h"
#include "daemon/system/target.h"

namespace wa {

enum class SessionMode { Assistant, Dictation };

struct SessionRequest {
    uint64_t id = 0;
    SessionMode mode = SessionMode::Assistant;
    bool voice = false;
    std::vector<float> audio;
    std::string text;
    TargetInfo target;
    Config config;
};

// Posted to the UI window (lParam = SessionEvent*, receiver deletes it).
struct SessionEvent {
    enum class Kind { Transcript, Status, Delta, Reset, Done, Error };
    uint64_t session = 0;
    Kind kind = Kind::Status;
    std::string text;
    ActionResult result;
    bool injected = false;
    // Plain-text answer the user may insert later with the insert hotkey.
    std::string answer;
};

std::vector<PluginManifest> builtin_manifests();

// Runs one request at a time on a background thread:
// speech-to-text -> intent router -> plugin, or LLM (with tool calling) -> result.
class Assistant {
public:
    Assistant(HWND notify, UINT message, PluginHost& plugins, ProviderHub& providers, SttClient& stt);
    ~Assistant();

    void start(SessionRequest request);
    void cancel();
    void join();
    bool busy() const { return busy_.load(); }
    void clear_history();

private:
    void run(SessionRequest request);
    void post(SessionEvent::Kind kind, std::string text = {});
    void finish(ActionResult result, bool injected, std::string answer = {});

    ActionResult dispatch(const std::string& plugin_id, const std::string& intent, const nlohmann::json& params,
                          const SessionRequest& request, const std::string& utterance);
    ActionResult run_builtin(const std::string& intent, const nlohmann::json& params, const SessionRequest& request,
                             const std::string& utterance);
    ActionResult ask_llm(const std::string& utterance, const SessionRequest& request, std::string& answer);
    ActionResult compose(const std::string& instruction, const SessionRequest& request);
    ActionResult describe_screen(const std::string& question, const SessionRequest& request);
    std::string system_context(const SessionRequest& request) const;

    HWND notify_;
    UINT message_;
    PluginHost& plugins_;
    ProviderHub& providers_;
    SttClient& stt_;

    std::thread thread_;
    std::atomic<uint64_t> session_{0};
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    HANDLE cancel_event_;

    std::mutex history_mutex_;
    std::vector<ChatMessage> history_;
    std::chrono::steady_clock::time_point last_turn_{};
};

}  // namespace wa
