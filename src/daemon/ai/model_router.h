#pragma once

#include <string>

#include "core/config.h"

namespace wa {

enum class TaskClass {
    // Short, mechanical rewrites: narration, selection skills, classification.
    Quick,
    // Normal conversation and tool calling.
    Chat,
    // Explicitly deep questions worth a bigger model.
    Reasoning,
    // Needs a vision-capable model.
    Vision,
};

// Chooses the Ollama model for `task`, downgrading when free VRAM is short.
// `why` (optional) receives a short reason for the daemon log.
std::string select_model(TaskClass task, const Config& config, std::string* why = nullptr);

// Heuristic: picks Reasoning for long or deliberately deep asks, Quick for very
// short ones, Chat otherwise.
TaskClass classify_utterance(const std::string& utterance, bool has_tools);

}  // namespace wa
