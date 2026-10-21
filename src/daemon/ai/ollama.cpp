#include "daemon/ai/ollama.h"

#include <algorithm>

#include "common/log.h"

namespace wa {

using nlohmann::json;

OllamaProvider::OllamaProvider(OllamaConfig config) : config_(std::move(config)) {}

void OllamaProvider::set_config(OllamaConfig config) {
    std::lock_guard lock(mutex_);
    config_ = std::move(config);
    last_check_ = {};
}

OllamaConfig OllamaProvider::config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

std::string OllamaProvider::url(const std::string& path) const {
    std::string host = config().host;
    while (!host.empty() && host.back() == '/') host.pop_back();
    return host + path;
}

bool OllamaProvider::available() {
    {
        std::lock_guard lock(mutex_);
        if (std::chrono::steady_clock::now() - last_check_ < std::chrono::seconds(3)) return last_available_;
    }
    HttpClient probe;
    auto response = probe.request("GET", url("/api/version"), {}, 800);
    std::lock_guard lock(mutex_);
    last_available_ = response.ok();
    last_check_ = std::chrono::steady_clock::now();
    return last_available_;
}

OllamaModelInfo OllamaProvider::model_info(const std::string& model) {
    {
        std::lock_guard lock(mutex_);
        if (auto it = model_cache_.find(model); it != model_cache_.end()) return it->second;
    }
    OllamaModelInfo info;
    info.name = model;
    HttpClient client;
    auto response = client.request("POST", url("/api/show"), json{{"model", model}}.dump(), 5000);
    if (response.ok()) {
        json j = json::parse(response.body, nullptr, false);
        if (!j.is_discarded()) {
            for (const auto& cap : j.value("capabilities", json::array())) {
                const std::string c = cap.get<std::string>();
                info.vision |= c == "vision";
                info.tools |= c == "tools";
                info.thinking |= c == "thinking";
                info.embedding |= c == "embedding";
            }
        }
        std::lock_guard lock(mutex_);
        model_cache_[model] = info;
    }
    return info;
}

std::vector<std::string> OllamaProvider::list_models(std::string* error) {
    std::vector<std::string> names;
    HttpClient client;
    auto response = client.request("GET", url("/api/tags"), {}, 3000);
    if (!response.ok()) {
        if (error) *error = response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error;
        return names;
    }
    json j = json::parse(response.body, nullptr, false);
    if (j.is_discarded()) return names;
    for (const auto& m : j.value("models", json::array())) names.push_back(m.value("name", ""));
    std::sort(names.begin(), names.end());
    return names;
}

ChatResponse OllamaProvider::chat(const ChatRequest& request, const DeltaCallback& on_delta,
                                  const std::atomic<bool>& cancel) {
    const OllamaConfig cfg = config();
    const std::string model = !request.model.empty() ? request.model : request.vision ? cfg.vision_model : cfg.model;
    const OllamaModelInfo info = model_info(model);

    json messages = json::array();
    for (const auto& m : request.messages) {
        json jm = {{"role", m.role}, {"content", m.content}};
        if (!m.images_base64.empty()) jm["images"] = m.images_base64;
        messages.push_back(std::move(jm));
    }
    json body = {
        {"model", model},
        {"messages", messages},
        {"stream", true},
        {"keep_alive", cfg.keep_alive},
        {"options", {{"num_predict", request.max_tokens}}},
    };
    if (info.thinking) body["think"] = cfg.think;
    if (!request.tools.empty() && info.tools) body["tools"] = request.tools;
    if (!request.format.is_null()) body["format"] = request.format;

    ChatResponse result;
    std::string line_buffer;
    std::string stream_error;
    auto handle_line = [&](std::string_view line) {
        if (line.empty()) return;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded()) return;
        if (j.contains("error")) {
            stream_error = j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump();
            return;
        }
        const json& msg = j.value("message", json::object());
        if (auto content = msg.find("content"); content != msg.end() && content->is_string()) {
            const std::string piece = content->get<std::string>();
            if (!piece.empty()) {
                result.content += piece;
                if (on_delta) on_delta(piece);
            }
        }
        for (const auto& call : msg.value("tool_calls", json::array())) {
            const json& fn = call.value("function", json::object());
            ToolCall tc;
            tc.name = fn.value("name", "");
            tc.arguments = fn.value("arguments", json::object());
            if (tc.arguments.is_string()) {
                json parsed = json::parse(tc.arguments.get<std::string>(), nullptr, false);
                tc.arguments = parsed.is_discarded() ? json::object() : parsed;
            }
            if (!tc.name.empty()) result.tool_calls.push_back(std::move(tc));
        }
    };

    auto response = http_.request(
        "POST", url("/api/chat"), body.dump(), cfg.timeout_ms,
        [&](std::string_view chunk) {
            line_buffer.append(chunk);
            size_t start = 0;
            for (size_t nl; (nl = line_buffer.find('\n', start)) != std::string::npos; start = nl + 1) {
                handle_line(std::string_view(line_buffer).substr(start, nl - start));
            }
            line_buffer.erase(0, start);
            return !cancel.load();
        },
        &cancel);
    if (!line_buffer.empty()) handle_line(line_buffer);

    if (cancel.load()) {
        result.error = "cancelled";
    } else if (!response.error.empty()) {
        result.error = "Ollama: " + response.error;
    } else if (response.status < 200 || response.status >= 300) {
        json j = json::parse(response.body, nullptr, false);
        result.error = "Ollama HTTP " + std::to_string(response.status) +
                       (!j.is_discarded() && j.contains("error") ? ": " + j["error"].get<std::string>() : "");
    } else if (!stream_error.empty()) {
        result.error = "Ollama: " + stream_error;
    } else {
        result.ok = true;
    }
    return result;
}

std::optional<std::vector<std::vector<float>>> OllamaProvider::embed(const std::vector<std::string>& inputs) {
    const OllamaConfig cfg = config();
    auto response = post("/api/embed", json{{"model", cfg.embed_model}, {"input", inputs}}.dump(), 30000);
    if (!response.ok()) {
        log::warn("ollama embed failed: {} {}", response.status, response.error);
        return std::nullopt;
    }
    json j = json::parse(response.body, nullptr, false);
    if (j.is_discarded() || !j.contains("embeddings")) return std::nullopt;
    return j["embeddings"].get<std::vector<std::vector<float>>>();
}

HttpResponse OllamaProvider::post(const std::string& path, const std::string& body, int timeout_ms) {
    HttpClient client;
    return client.request("POST", url(path), body, timeout_ms);
}

void OllamaProvider::abort() { http_.abort(); }

}  // namespace wa
