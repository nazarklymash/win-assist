#include "daemon/plugins/action.h"

#include "common/strings.h"

namespace wa {

ActionResult ActionResult::from_output(const std::string& output) {
    const std::string text = trim(output);
    if (text.empty()) return ActionResult{};

    // Scripts may log before the reply; the reply is the last JSON line.
    size_t line_start = text.rfind('\n');
    std::string last = trim(line_start == std::string::npos ? text : text.substr(line_start + 1));
    nlohmann::json j = nlohmann::json::parse(last, nullptr, false);
    if (j.is_discarded() || !j.is_object()) j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        ActionResult r;
        r.display = text;
        return r;
    }
    ActionResult r;
    r.ok = j.value("status", "ok") != "error";
    r.display = j.value("display", "");
    r.speech = j.value("speech", "");
    r.inject_text = j.value("inject_text", "");
    return r;
}

}  // namespace wa
