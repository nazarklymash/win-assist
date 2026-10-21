#include "daemon/ai/llamacpp.h"

#include "common/log.h"
#include "common/strings.h"
#include "daemon/ai/ollama_models.h"

namespace wa {

using nlohmann::json;

namespace {

// Asks the model for a JSON envelope so plugin tools work without native
// function-calling support in llama.cpp.
std::string tool_instructions(const json& tools) {
    std::string text =
        "\n\nYou can call these tools:\n";
    for (const auto& t : tools) {
        const json& fn = t.value("function", json::object());
        text += "- " + fn.value("name", "") + ": " + fn.value("description", "") +
                " Parameters: " + fn.value("parameters", json::object()).value("properties", json::object()).dump() + "\n";
    }
    text +=
        "Respond with exactly one JSON object and nothing else. To call a tool: "
        "{\"tool\":\"<name>\",\"arguments\":{...}}. To answer directly: {\"reply\":\"<your answer>\"}.";
    return text;
}

}  // namespace

LlamaCppProvider::LlamaCppProvider(LlamaCppConfig config)
    : config_(std::move(config)), worker_(L"wa-llm.exe", "llm"), cancel_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

LlamaCppProvider::~LlamaCppProvider() {
    shutdown();
    CloseHandle(cancel_event_);
}

void LlamaCppProvider::set_config(LlamaCppConfig config) {
    std::lock_guard lock(config_mutex_);
    config_ = std::move(config);
}

LlamaCppConfig LlamaCppProvider::config() const {
    std::lock_guard lock(config_mutex_);
    return config_;
}

bool LlamaCppProvider::available() {
    std::error_code ec;
    if (!std::filesystem::exists(worker_.exe_path(), ec)) return false;
    return resolve_gguf(config().model, nullptr).has_value();
}

void LlamaCppProvider::cancel() { SetEvent(cancel_event_); }

ChatResponse LlamaCppProvider::chat(const ChatRequest& request, const DeltaCallback& on_delta,
                                    const std::atomic<bool>& cancel) {
    ChatResponse result;
    const LlamaCppConfig cfg = config();
    std::string error;
    auto model = resolve_gguf(cfg.model, &error);
    if (!model) {
        result.error = error;
        return result;
    }

    auto guard = worker_.lock();
    ResetEvent(cancel_event_);
    std::vector<std::wstring> args = {L"--model", model->wstring(), L"--ctx", std::to_wstring(cfg.n_ctx),
                                      L"--gpu-layers", std::to_wstring(cfg.n_gpu_layers)};
    if (cfg.threads > 0) {
        args.push_back(L"--threads");
        args.push_back(std::to_wstring(cfg.threads));
    }
    std::string ready;
    if (!worker_.ensure_started(args, ready, error, cancel_event_)) {
        result.error = "llama.cpp: " + error;
        return result;
    }
    json ready_json = ready.empty() ? json{{"ok", true}} : json::parse(ready, nullptr, false);
    if (ready_json.is_discarded() || !ready_json.value("ok", false)) {
        result.error = "llama.cpp: " + (ready_json.is_discarded() ? std::string("bad ready frame")
                                                                   : ready_json.value("error", "load failed"));
        worker_.stop();
        return result;
    }

    const bool use_tools = !request.tools.empty();
    json messages = json::array();
    for (const auto& m : request.messages) {
        std::string content = m.content;
        if (use_tools && m.role == "system") content += tool_instructions(request.tools);
        messages.push_back({{"role", m.role == "tool" ? "user" : m.role}, {"content", content}});
    }
    json req = {{"op", "chat"},
                {"messages", messages},
                {"max_tokens", request.max_tokens},
                {"json", use_tools || !request.format.is_null()},
                {"stream", !use_tools}};
    if (!worker_.send(req.dump(), cancel_event_)) {
        result.error = "llama.cpp worker disconnected";
        worker_.stop();
        return result;
    }

    // Cancellation is only checked between frames so the stream never
    // desynchronises; the worker polls for {"op":"cancel"} after every token.
    bool cancel_sent = false;
    std::string frame;
    for (;;) {
        if (!worker_.receive(frame, nullptr)) {
            result.error = "llama.cpp worker disconnected";
            worker_.stop();
            return result;
        }
        if (cancel.load() && !cancel_sent) {
            worker_.send(std::string(R"({"op":"cancel"})"), nullptr);
            cancel_sent = true;
        }
        json j = json::parse(frame, nullptr, false);
        if (j.is_discarded()) continue;
        if (j.contains("delta")) {
            if (on_delta && !cancel_sent) on_delta(j["delta"].get<std::string>());
            continue;
        }
        if (j.value("done", false)) {
            if (!j.value("ok", false)) {
                result.error = j.value("error", "generation failed");
                return result;
            }
            result.content = j.value("text", "");
            break;
        }
    }
    if (cancel_sent) {
        result.error = "cancelled";
        return result;
    }

    if (use_tools) {
        json envelope = json::parse(result.content, nullptr, false);
        result.content.clear();
        if (!envelope.is_discarded() && envelope.is_object()) {
            if (envelope.contains("tool") && envelope["tool"].is_string()) {
                ToolCall call;
                call.name = envelope["tool"].get<std::string>();
                call.arguments = envelope.value("arguments", json::object());
                result.tool_calls.push_back(std::move(call));
            } else {
                result.content = envelope.value("reply", "");
            }
        }
        if (!result.content.empty() && on_delta) on_delta(result.content);
    }
    result.ok = true;
    return result;
}

bool LlamaCppProvider::idle_tick() { return worker_.stop_if_idle(std::chrono::seconds(config().idle_unload_s)); }

void LlamaCppProvider::shutdown() {
    SetEvent(cancel_event_);
    auto guard = worker_.lock();
    worker_.stop();
}

}  // namespace wa
