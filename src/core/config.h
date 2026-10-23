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
        "Reply in the same language the user wrote or spoke (English or Ukrainian). "
        "Answer in at most three short sentences unless asked for more. "
        "Call a tool when the user asks for an action a tool can perform.";
    int max_tokens = 512;
    int history_turns = 4;
    // How many tool calls one request may chain before the loop stops.
    int max_tool_steps = 5;
};

// Picks an Ollama model per task, degrading when the GPU is busy (a game can
// leave too little VRAM for the bigger models).
struct ModelRoutingConfig {
    bool enabled = true;
    // Empty entries fall back to ollama.model / ollama.vision_model.
    std::string quick = "qwen3.5:4b";
    std::string chat;
    std::string reasoning;
    std::string vision;
    // Minimum free VRAM (MB) required to pick `reasoning` / keep `chat`.
    int reasoning_min_free_mb = 18000;
    int chat_min_free_mb = 5000;
};

struct OllamaConfig {
    std::string host = "http://127.0.0.1:11434";
    std::string model = "qwen3.5:4b";
    std::string vision_model = "qwen3.5:4b";
    std::string embed_model = "nomic-embed-text";
    std::string keep_alive = "5m";
    bool think = false;
    int timeout_ms = 120000;
    ModelRoutingConfig routing;
};

struct LlamaCppConfig {
    // A GGUF path, or "ollama:<model>" to reuse a model already pulled by Ollama.
    std::string model = "ollama:qwen3.5:4b";
    int n_ctx = 4096;
    // 0 = CPU. Prefer release-cuda build and set e.g. 99 to offload to GPU.
    int n_gpu_layers = 99;
    int threads = 0;
    int idle_unload_s = 120;
};

struct SttConfig {
    // large-v3-turbo: best EN+UK accuracy that still feels snappy on GPU.
    // Fall back with scripts/download-models.ps1 -Whisper small|base if VRAM is tight.
    std::string model = "models/ggml-large-v3-turbo.bin";
    // Whisper language code, or "auto" to detect (best for EN+UK bilingual use).
    std::string language = "auto";
    int threads = 0;
    bool use_gpu = true;
    int idle_unload_s = 120;
    int max_record_s = 30;
    // Empty = Windows default capture device. WASAPI endpoint ID otherwise.
    std::string device_id;
};

struct TtsConfig {
    bool enabled = true;
    bool speak_typed_answers = false;
    std::string voice;
    int rate = 0;
    // "auto" = piper.exe if present under bin/tts, else SAPI. "sapi" / "piper" force.
    std::string engine = "auto";
    std::string piper_path;   // optional override, relative to exe dir
    std::string piper_voice;  // optional .onnx path
};

struct OverlayConfig {
    std::string position = "bottom";  // "top" or "bottom" of the target window's monitor
    int width = 620;
    int min_display_ms = 4000;
};

// Gate for plugin/built-in intents that can change or destroy system state.
// Capability tags come from each intent's "capabilities" in manifest.json.
struct PermissionsConfig {
    bool enabled = true;
    // Capabilities that require an explicit yes/no in the overlay.
    std::vector<std::string> confirm = {"process_kill", "shell", "power", "filesystem_write"};
    // Capabilities refused outright, without prompting.
    std::vector<std::string> deny;
    // Intents ("<plugin>/<intent>") the user allowed permanently.
    std::vector<std::string> allow_intents;
    // How long the confirmation prompt waits before denying.
    int confirm_timeout_ms = 20000;
    // Log every gated invocation to the daemon log.
    bool audit = true;
};

// One "when X happens, do Y" rule, evaluated against polled system state.
struct TriggerRule {
    std::string name;
    // app_launch | app_exit | window_title | battery_below | battery_above | on_battery | on_ac
    std::string when;
    // Process executable name or foreground-title substring, case-insensitive.
    std::string match;
    // Battery percentage for the battery_* events.
    int threshold = 20;
    // Tray notification text (spoken too when TTS is on).
    std::string notify;
    // Utterance handed to the assistant, as if the user had typed it.
    std::string ask;
    int cooldown_sec = 300;
    bool enabled = true;
};

struct TriggersConfig {
    bool enabled = true;
    int poll_sec = 5;
    std::vector<TriggerRule> rules = {{"battery-low", "battery_below", "", 20,
                                       "Battery is under 20% \u2014 plug in.", "", 1800, true}};
};

struct Config {
    HotkeyConfig hotkeys;
    LlmConfig llm;
    OllamaConfig ollama;
    LlamaCppConfig llamacpp;
    SttConfig stt;
    TtsConfig tts;
    OverlayConfig overlay;
    PermissionsConfig permissions;
    TriggersConfig triggers;
    std::vector<std::string> plugin_dirs;
    // Register wa-daemon under HKCU...\Run so it starts at Windows sign-in.
    bool start_with_windows = true;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HotkeyConfig, assistant, dictation, tap_threshold_ms)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LlmConfig, provider, system_prompt, max_tokens, history_turns,
                                                max_tool_steps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ModelRoutingConfig, enabled, quick, chat, reasoning, vision,
                                                reasoning_min_free_mb, chat_min_free_mb)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OllamaConfig, host, model, vision_model, embed_model, keep_alive, think,
                                                timeout_ms, routing)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LlamaCppConfig, model, n_ctx, n_gpu_layers, threads, idle_unload_s)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SttConfig, model, language, threads, use_gpu, idle_unload_s,
                                                max_record_s, device_id)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TtsConfig, enabled, speak_typed_answers, voice, rate, engine,
                                                piper_path, piper_voice)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OverlayConfig, position, width, min_display_ms)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PermissionsConfig, enabled, confirm, deny, allow_intents,
                                                confirm_timeout_ms, audit)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TriggerRule, name, when, match, threshold, notify, ask, cooldown_sec,
                                                enabled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TriggersConfig, enabled, poll_sec, rules)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, hotkeys, llm, ollama, llamacpp, stt, tts, overlay, permissions,
                                                triggers, plugin_dirs, start_with_windows)

Config parse_config(std::string_view json_text, std::string* error);
// Loads `file`, writing a default config there first if it does not exist.
Config load_or_create_config(const std::filesystem::path& file, std::string* error);

// Resolves a config path: absolute paths are kept, relative ones are taken
// relative to the executable directory.
std::filesystem::path resolve_app_path(const std::string& path);

}  // namespace wa
