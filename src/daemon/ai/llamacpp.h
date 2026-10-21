#pragma once

#include <mutex>

#include "core/config.h"
#include "core/llm.h"
#include "daemon/ipc/worker.h"

namespace wa {

// LLM provider backed by the out-of-process llama.cpp worker (wa-llm.exe).
class LlamaCppProvider final : public LlmProvider {
public:
    explicit LlamaCppProvider(LlamaCppConfig config);
    ~LlamaCppProvider() override;

    void set_config(LlamaCppConfig config);

    std::string name() const override { return "llama.cpp"; }
    bool available() override;
    bool supports_vision() const override { return false; }
    ChatResponse chat(const ChatRequest& request, const DeltaCallback& on_delta,
                      const std::atomic<bool>& cancel) override;
    std::optional<std::vector<std::vector<float>>> embed(const std::vector<std::string>&) override {
        return std::nullopt;
    }

    // Called periodically from the UI thread; returns true while the worker lives.
    bool idle_tick();
    void shutdown();
    void cancel();

private:
    LlamaCppConfig config() const;

    mutable std::mutex config_mutex_;
    LlamaCppConfig config_;
    WorkerProcess worker_;
    HANDLE cancel_event_;
};

}  // namespace wa
