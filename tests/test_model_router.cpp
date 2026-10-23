#include <doctest/doctest.h>

#include "daemon/ai/gpu.h"
#include "daemon/ai/model_router.h"

using namespace wa;

namespace {

Config routed_config() {
    Config c;
    c.ollama.model = "chat-model";
    c.ollama.vision_model = "vision-model";
    c.ollama.routing.enabled = true;
    c.ollama.routing.quick = "quick-model";
    c.ollama.routing.chat = "";  // falls back to ollama.model
    c.ollama.routing.reasoning = "big-model";
    return c;
}

}  // namespace

TEST_CASE("routing disabled keeps the configured defaults") {
    Config c = routed_config();
    c.ollama.routing.enabled = false;
    CHECK(select_model(TaskClass::Quick, c) == "chat-model");
    CHECK(select_model(TaskClass::Reasoning, c) == "chat-model");
    CHECK(select_model(TaskClass::Vision, c) == "vision-model");
}

TEST_CASE("vision never downgrades") {
    Config c = routed_config();
    c.ollama.routing.vision = "custom-vision";
    // Even with an absurd memory floor, vision keeps its own model.
    c.ollama.routing.chat_min_free_mb = 1 << 30;
    CHECK(select_model(TaskClass::Vision, c) == "custom-vision");
}

TEST_CASE("empty routing entries fall back to the chat model") {
    Config c = routed_config();
    c.ollama.routing.quick = "";
    c.ollama.routing.chat_min_free_mb = 0;
    c.ollama.routing.reasoning_min_free_mb = 0;
    CHECK(select_model(TaskClass::Chat, c) == "chat-model");
    CHECK(select_model(TaskClass::Quick, c) == "chat-model");
}

TEST_CASE("plenty of headroom keeps the requested tier") {
    Config c = routed_config();
    c.ollama.routing.reasoning_min_free_mb = 0;
    c.ollama.routing.chat_min_free_mb = 0;
    CHECK(select_model(TaskClass::Reasoning, c) == "big-model");
    CHECK(select_model(TaskClass::Quick, c) == "quick-model");
}

TEST_CASE("no headroom downgrades reasoning and chat to quick") {
    Config c = routed_config();
    // Require more VRAM than any adapter can offer so both floors trip.
    c.ollama.routing.reasoning_min_free_mb = 1 << 28;
    c.ollama.routing.chat_min_free_mb = 1 << 28;
    const GpuMemory gpu = query_gpu_memory();
    if (!gpu.ok) {
        // Without GPU info the router must not pretend to know better.
        CHECK(select_model(TaskClass::Reasoning, c) == "big-model");
        return;
    }
    CHECK(select_model(TaskClass::Reasoning, c) == "quick-model");
    CHECK(select_model(TaskClass::Chat, c) == "quick-model");
}

TEST_CASE("utterance classification") {
    CHECK(classify_utterance("mute", false) == TaskClass::Quick);
    // Tool-capable turns stay on the chat model even when short.
    CHECK(classify_utterance("mute", true) == TaskClass::Chat);
    CHECK(classify_utterance("why is my laptop so slow", true) == TaskClass::Reasoning);
    CHECK(classify_utterance("explain this code", true) == TaskClass::Reasoning);
    CHECK(classify_utterance("\u043f\u043e\u044f\u0441\u043d\u0438 \u0446\u0435", true) == TaskClass::Reasoning);
    CHECK(classify_utterance(std::string(300, 'a'), true) == TaskClass::Reasoning);
    CHECK(classify_utterance("open notepad and then tell me the time please", true) == TaskClass::Chat);
}
