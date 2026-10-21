#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/llm.h"
#include "daemon/ai/http.h"

namespace wa {

struct OllamaModelInfo {
    std::string name;
    bool vision = false;
    bool tools = false;
    bool thinking = false;
    bool embedding = false;
};

// LLM provider backed by a local (or remote) Ollama server.
class OllamaProvider final : public LlmProvider {
public:
    explicit OllamaProvider(OllamaConfig config);

    void set_config(OllamaConfig config);
    OllamaConfig config() const;

    std::string name() const override { return "ollama"; }
    bool available() override;
    bool supports_vision() const override { return true; }
    ChatResponse chat(const ChatRequest& request, const DeltaCallback& on_delta,
                      const std::atomic<bool>& cancel) override;
    std::optional<std::vector<std::vector<float>>> embed(const std::vector<std::string>& inputs) override;

    std::vector<std::string> list_models(std::string* error = nullptr);
    OllamaModelInfo model_info(const std::string& model);
    // Raw POST used by native plugins through the host API.
    HttpResponse post(const std::string& path, const std::string& body, int timeout_ms);
    void abort();

private:
    std::string url(const std::string& path) const;

    mutable std::mutex mutex_;
    OllamaConfig config_;
    HttpClient http_;
    std::chrono::steady_clock::time_point last_check_{};
    bool last_available_ = false;
    std::map<std::string, OllamaModelInfo> model_cache_;
};

}  // namespace wa
