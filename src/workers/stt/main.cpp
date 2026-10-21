// wa-stt: whisper.cpp speech-to-text worker.
//
// Protocol (frames over the daemon's named pipe):
//   worker -> {"event":"ready","ok":true,"model":"..."}
//   daemon -> {"op":"transcribe","language":"en","n_samples":N}  followed by
//             one binary frame of N float32 samples, 16 kHz mono
//   worker -> {"ok":true,"text":"...","language":"en","ms":123}
//   daemon -> {"op":"shutdown"}
// The worker exits when the pipe closes.

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <whisper.h>

#include "common/pipe.h"
#include "workers/worker_common.h"

namespace {

using nlohmann::json;

void quiet_log(enum ggml_log_level level, const char* text, void*) {
    if (level >= GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

bool send_json(HANDLE pipe, const json& j) { return wa::ipc::write_frame(pipe, j.dump()); }

std::string transcribe(whisper_context* ctx, std::vector<float>& pcm, const std::string& language, int threads,
                       std::string& detected) {
    // whisper needs at least one second of audio; pad short clips with silence.
    constexpr size_t kMinSamples = WHISPER_SAMPLE_RATE + WHISPER_SAMPLE_RATE / 10;
    if (pcm.size() < kMinSamples) pcm.resize(kMinSamples, 0.0f);

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = threads;
    params.no_context = true;
    params.no_timestamps = true;
    params.single_segment = false;
    params.print_special = false;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.suppress_nst = true;
    params.language = language.empty() ? "auto" : language.c_str();
    params.detect_language = false;

    if (whisper_full(ctx, params, pcm.data(), static_cast<int>(pcm.size())) != 0) return {};

    std::string text;
    int segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < segments; ++i) text += whisper_full_get_segment_text(ctx, i);
    const char* lang = whisper_lang_str(whisper_full_lang_id(ctx));
    detected = lang ? lang : "";
    return wa::trim(text);
}

}  // namespace

int main() {
    auto args = wa::worker::parse_args();
    HANDLE pipe = wa::worker::connect_pipe(wa::worker::arg_str(args, "pipe"));
    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "wa-stt: could not connect to pipe\n");
        return 2;
    }

    whisper_log_set(quiet_log, nullptr);
    const std::string model = wa::worker::arg_str(args, "model");
    const int threads = wa::worker::arg_int(args, "threads", 0) > 0 ? wa::worker::arg_int(args, "threads", 0)
                                                                     : wa::worker::default_threads();
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = wa::worker::arg_int(args, "gpu", 1) != 0;
    whisper_context* ctx = whisper_init_from_file_with_params(model.c_str(), cparams);
    if (!ctx) {
        send_json(pipe, {{"event", "ready"}, {"ok", false}, {"error", "failed to load whisper model: " + model}});
        CloseHandle(pipe);
        return 3;
    }
    send_json(pipe, {{"event", "ready"}, {"ok", true}, {"model", model}, {"multilingual", whisper_is_multilingual(ctx) != 0}});

    std::string frame;
    while (wa::ipc::read_frame(pipe, frame)) {
        json request = json::parse(frame, nullptr, false);
        if (request.is_discarded()) break;
        const std::string op = request.value("op", "");
        if (op == "shutdown") break;
        if (op != "transcribe") {
            send_json(pipe, {{"ok", false}, {"error", "unknown op"}});
            continue;
        }

        std::string audio;
        if (!wa::ipc::read_frame(pipe, audio)) break;
        std::vector<float> pcm(audio.size() / sizeof(float));
        memcpy(pcm.data(), audio.data(), pcm.size() * sizeof(float));

        auto start = std::chrono::steady_clock::now();
        std::string detected;
        std::string text = transcribe(ctx, pcm, request.value("language", "en"), threads, detected);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        send_json(pipe, {{"ok", true}, {"text", text}, {"language", detected}, {"ms", ms}});
    }

    whisper_free(ctx);
    CloseHandle(pipe);
    return 0;
}
