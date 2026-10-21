#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace wa {

// Maps an Ollama model reference ("qwen3.5:4b", "huihui_ai/qwen3-abliterated:8b")
// to the GGUF weights blob in Ollama's local store, honoring OLLAMA_MODELS.
std::optional<std::filesystem::path> resolve_ollama_blob(std::string_view model, std::string* error);

// Accepts either "ollama:<model>" or a GGUF path (relative to the exe dir).
std::optional<std::filesystem::path> resolve_gguf(const std::string& spec, std::string* error);

}  // namespace wa
