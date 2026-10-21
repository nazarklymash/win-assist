#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "core/config.h"
#include "daemon/ai/llamacpp.h"
#include "daemon/ai/ollama.h"

namespace wa {

// Selects the LLM backend per request according to llm.provider.
class ProviderHub {
public:
    explicit ProviderHub(const Config& config);

    void apply_config(const Config& config);
    // Returns null (with `error`) when no backend can serve the request.
    LlmProvider* select(bool need_vision, std::string& error);

    OllamaProvider& ollama() { return ollama_; }
    LlamaCppProvider& llamacpp() { return llamacpp_; }

    void cancel_all();
    bool idle_tick();
    void shutdown();

private:
    std::mutex mutex_;
    std::string mode_;
    OllamaProvider ollama_;
    LlamaCppProvider llamacpp_;
};

}  // namespace wa
