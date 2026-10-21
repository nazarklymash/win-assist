#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace wa {

struct HotkeyConfig {
    std::string assistant = "Alt+Space";
    std::string dictation = "Ctrl+Alt+Space";
    int tap_threshold_ms = 300;
};

struct LlmConfig {
    // "auto" = Ollama when reachable, otherwise the embedded llama.cpp worker.
    std::string provider = "auto";
    std::string system_prompt =
        "You are win-assist, a concise desktop assistant running on Windows. "
        "Answer in at most three short sentences unless asked for more. "
        "Call a tool when the user asks for an action a tool can perform.";
    int max_tokens = 512;
    int history_turns = 4;
};

struct OllamaConfig {
    std::string host = "http://127.0.0.1:11434";
    std::string model = "qwen3.5:4b";
    std::string vision_model = "qwen3.5:4b";
    std::string embed_model = "nomic-embed-text";
    std::string keep_alive = "5m";
    bool think = false;
    int timeout_ms = 120000;
};

struct LlamaCppConfig {
    // A GGUF path, or "ollama:<model>" to reuse a model already pulled by Ollama.
    std::string model = "ollama:qwen3.5:4b";
    int n_ctx = 4096;
    int n_gpu_layers = 0;
    int threads = 0;
    int idle_unload_s = 120;
};

struct SttConfig {
    std::string model = "models/ggml-base.bin";
    std::string language = "en";
    int threads = 0;
    bool use_gpu = true;
    int idle_unload_s = 120;
    int max_record_s = 30;
};

struct TtsConfig {
    bool enabled = true;
    bool speak_typed_answers = false;
    std::string voice;
    int rate = 0;
};

struct OverlayConfig {
    std::string position = "bottom";  // "top" or "bottom" of the target window's monitor
    int width = 620;
    int min_display_ms = 4000;
};

struct Config {
    HotkeyConfig hotkeys;
    LlmConfig llm;
    OllamaConfig ollama;
    LlamaCppConfig llamacpp;
    SttConfig stt;
    TtsConfig tts;
    OverlayConfig overlay;
    std::vector<std::string> plugin_dirs;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HotkeyConfig, assistant, dictation, tap_threshold_ms)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LlmConfig, provider, system_prompt, max_tokens, history_turns)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OllamaConfig, host, model, vision_model, embed_model, keep_alive, think,
                                                timeout_ms)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LlamaCppConfig, model, n_ctx, n_gpu_layers, threads, idle_unload_s)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SttConfig, model, language, threads, use_gpu, idle_unload_s,
                                                max_record_s)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TtsConfig, enabled, speak_typed_answers, voice, rate)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OverlayConfig, position, width, min_display_ms)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, hotkeys, llm, ollama, llamacpp, stt, tts, overlay, plugin_dirs)

Config parse_config(std::string_view json_text, std::string* error);
// Loads `file`, writing a default config there first if it does not exist.
Config load_or_create_config(const std::filesystem::path& file, std::string* error);

// Resolves a config path: absolute paths are kept, relative ones are taken
// relative to the executable directory.
std::filesystem::path resolve_app_path(const std::string& path);

}  // namespace wa
