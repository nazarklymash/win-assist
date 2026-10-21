#include "daemon/ai/stt.h"

#include <nlohmann/json.hpp>

#include "common/log.h"
#include "common/strings.h"

namespace wa {

using nlohmann::json;

SttClient::SttClient(SttConfig config) : config_(std::move(config)), worker_(L"wa-stt.exe", "stt") {}

SttClient::~SttClient() { shutdown(); }

void SttClient::set_config(SttConfig config) {
    std::lock_guard lock(config_mutex_);
    config_ = std::move(config);
}

SttConfig SttClient::config() const {
    std::lock_guard lock(config_mutex_);
    return config_;
}

bool SttClient::ensure_started(std::string& error, HANDLE cancel) {
    const SttConfig cfg = config();
    const auto model = resolve_app_path(cfg.model);
    std::error_code ec;
    if (!std::filesystem::exists(model, ec)) {
        error = "Whisper model not found: " + to_utf8(model.wstring()) + " (run scripts\\download-models.ps1)";
        return false;
    }
    std::vector<std::wstring> args = {L"--model", model.wstring(), L"--gpu", cfg.use_gpu ? L"1" : L"0"};
    if (cfg.threads > 0) {
        args.push_back(L"--threads");
        args.push_back(std::to_wstring(cfg.threads));
    }
    std::string ready;
    if (!worker_.ensure_started(args, ready, error, cancel)) return false;
    if (ready.empty()) return true;  // already running
    json j = json::parse(ready, nullptr, false);
    if (j.is_discarded() || !j.value("ok", false)) {
        error = j.is_discarded() ? "bad ready frame from wa-stt" : j.value("error", "whisper failed to load");
        worker_.stop();
        return false;
    }
    return true;
}

void SttClient::warm_up() {
    auto guard = worker_.lock();
    std::string error;
    if (!ensure_started(error, nullptr)) log::warn("stt warm-up failed: {}", error);
}

std::optional<Transcript> SttClient::transcribe(const std::vector<float>& pcm16k, std::string& error, HANDLE cancel) {
    auto guard = worker_.lock();
    if (!ensure_started(error, cancel)) return std::nullopt;

    const std::string language = config().language;
    json request = {{"op", "transcribe"}, {"language", language}, {"n_samples", pcm16k.size()}};
    std::string reply;
    if (!worker_.send(request.dump(), nullptr) ||
        !worker_.send(pcm16k.data(), static_cast<uint32_t>(pcm16k.size() * sizeof(float)), nullptr) ||
        !worker_.receive(reply, nullptr)) {
        error = "speech worker disconnected";
        worker_.stop();
        return std::nullopt;
    }
    json j = json::parse(reply, nullptr, false);
    if (j.is_discarded() || !j.value("ok", false)) {
        error = j.is_discarded() ? "bad reply from wa-stt" : j.value("error", "transcription failed");
        return std::nullopt;
    }
    Transcript t;
    t.text = trim(j.value("text", ""));
    t.language = j.value("language", "");
    t.ms = j.value("ms", 0LL);
    log::info("transcribed {} samples in {} ms", pcm16k.size(), t.ms);
    return t;
}

bool SttClient::idle_tick() { return worker_.stop_if_idle(std::chrono::seconds(config().idle_unload_s)); }

void SttClient::shutdown() {
    auto guard = worker_.lock();
    worker_.stop();
}

}  // namespace wa
