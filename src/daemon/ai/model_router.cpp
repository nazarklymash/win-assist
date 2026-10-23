#include "daemon/ai/model_router.h"

#include <windows.h>

#include <algorithm>

#include "common/strings.h"
#include "daemon/ai/gpu.h"

namespace wa {
namespace {

// Case-folds Cyrillic as well as ASCII, matching the intent router's behaviour.
std::string fold_case(const std::string& text) {
    std::wstring w = to_wide(text);
    if (!w.empty()) CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    return to_utf8(w);
}

const char* name_of(TaskClass task) {
    switch (task) {
        case TaskClass::Quick: return "quick";
        case TaskClass::Chat: return "chat";
        case TaskClass::Reasoning: return "reasoning";
        case TaskClass::Vision: return "vision";
    }
    return "chat";
}

std::string configured(TaskClass task, const OllamaConfig& o) {
    switch (task) {
        case TaskClass::Quick: return o.routing.quick;
        case TaskClass::Chat: return o.routing.chat;
        case TaskClass::Reasoning: return o.routing.reasoning;
        case TaskClass::Vision: return o.routing.vision;
    }
    return {};
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

std::string select_model(TaskClass task, const Config& config, std::string* why) {
    const OllamaConfig& o = config.ollama;
    const std::string fallback = task == TaskClass::Vision ? o.vision_model : o.model;
    if (!o.routing.enabled) {
        if (why) *why = "routing disabled";
        return fallback;
    }

    std::string picked = configured(task, o);
    if (picked.empty()) picked = fallback;
    std::string reason = name_of(task);

    // Vision models are not interchangeable, so never downgrade those.
    if (task == TaskClass::Vision) {
        if (why) *why = reason;
        return picked;
    }

    const GpuMemory gpu = query_gpu_memory();
    if (!gpu.ok) {
        if (why) *why = reason + " (no GPU info)";
        return picked;
    }

    const int free_mb = static_cast<int>(gpu.free_mb);
    if (task == TaskClass::Reasoning && free_mb < o.routing.reasoning_min_free_mb) {
        // Not enough headroom for the big model; use the normal chat model.
        picked = o.routing.chat.empty() ? o.model : o.routing.chat;
        reason = "reasoning downgraded to chat (" + std::to_string(free_mb) + " MB free)";
    }
    if (free_mb < o.routing.chat_min_free_mb && !o.routing.quick.empty() && picked != o.routing.quick) {
        picked = o.routing.quick;
        reason = "downgraded to quick (" + std::to_string(free_mb) + " MB free)";
    }

    if (why) *why = reason;
    return picked;
}

TaskClass classify_utterance(const std::string& utterance, bool has_tools) {
    const std::string text = fold_case(utterance);
    static const char* kDeep[] = {"why",      "explain",  "compare",   "plan",     "design",
                                  "analyz",   "analys",   "debug",     "refactor", "step by step",
                                  "in detail", "think",   "чому",      "поясни",   "порівняй",
                                  "детально", "проаналіз"};
    for (const char* k : kDeep) {
        if (contains(text, k)) return TaskClass::Reasoning;
    }
    if (text.size() > 240) return TaskClass::Reasoning;
    // Very short asks that still may need a tool stay on the chat model so tool
    // calling keeps working; only tool-free one-liners go to the quick model.
    if (!has_tools && text.size() <= 60) return TaskClass::Quick;
    return TaskClass::Chat;
}

}  // namespace wa
