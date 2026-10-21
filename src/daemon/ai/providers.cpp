#include "daemon/ai/providers.h"

namespace wa {

ProviderHub::ProviderHub(const Config& config)
    : mode_(config.llm.provider), ollama_(config.ollama), llamacpp_(config.llamacpp) {}

void ProviderHub::apply_config(const Config& config) {
    {
        std::lock_guard lock(mutex_);
        mode_ = config.llm.provider;
    }
    ollama_.set_config(config.ollama);
    llamacpp_.set_config(config.llamacpp);
}

LlmProvider* ProviderHub::select(bool need_vision, std::string& error) {
    std::string mode;
    {
        std::lock_guard lock(mutex_);
        mode = mode_;
    }
    if (mode == "ollama" || need_vision) {
        if (ollama_.available()) return &ollama_;
        error = need_vision ? "Screen understanding needs Ollama with a vision model, and Ollama is not reachable."
                            : "Ollama is not reachable at " + ollama_.config().host + ".";
        return nullptr;
    }
    if (mode == "llamacpp") {
        if (llamacpp_.available()) return &llamacpp_;
        error = "llama.cpp backend unavailable: check llamacpp.model and that wa-llm.exe was built.";
        return nullptr;
    }
    if (ollama_.available()) return &ollama_;
    if (llamacpp_.available()) return &llamacpp_;
    error = "No LLM available: start Ollama, or set llamacpp.model to a GGUF file.";
    return nullptr;
}

void ProviderHub::cancel_all() {
    ollama_.abort();
    llamacpp_.cancel();
}

bool ProviderHub::idle_tick() { return llamacpp_.idle_tick(); }

void ProviderHub::shutdown() { llamacpp_.shutdown(); }

}  // namespace wa
