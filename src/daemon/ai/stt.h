#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "daemon/ipc/worker.h"

namespace wa {

struct Transcript {
    std::string text;
    std::string language;
    long long ms = 0;
};

// Client for the out-of-process whisper.cpp worker (wa-stt.exe).
class SttClient {
public:
    explicit SttClient(SttConfig config);
    ~SttClient();

    void set_config(SttConfig config);
    std::optional<Transcript> transcribe(const std::vector<float>& pcm16k, std::string& error, HANDLE cancel);
    // Pre-loads the model so the first transcription is fast.
    void warm_up();
    bool idle_tick();
    void shutdown();

private:
    bool ensure_started(std::string& error, HANDLE cancel);
    SttConfig config() const;

    mutable std::mutex config_mutex_;
    SttConfig config_;
    WorkerProcess worker_;
};

}  // namespace wa
