#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace wa {

struct ChatMessage {
    std::string role;  // system | user | assistant | tool
    std::string content;
    std::vector<std::string> images_base64;  // PNG/JPEG, vision models only
};

struct ToolCall {
    std::string name;
    nlohmann::json arguments = nlohmann::json::object();
};

struct ChatRequest {
    std::vector<ChatMessage> messages;
    nlohmann::json tools = nlohmann::json::array();
    nlohmann::json format;  // null, "json" or a JSON schema
    std::string model;      // empty = provider default
    int max_tokens = 512;
    bool vision = false;
};

struct ChatResponse {
    bool ok = false;
    std::string error;
    std::string content;
    std::vector<ToolCall> tool_calls;
};

using DeltaCallback = std::function<void(std::string_view)>;

class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    virtual std::string name() const = 0;
    virtual bool available() = 0;
    virtual bool supports_vision() const = 0;
    virtual ChatResponse chat(const ChatRequest& request, const DeltaCallback& on_delta,
                              const std::atomic<bool>& cancel) = 0;
    virtual std::optional<std::vector<std::vector<float>>> embed(const std::vector<std::string>& inputs) = 0;
};

}  // namespace wa
