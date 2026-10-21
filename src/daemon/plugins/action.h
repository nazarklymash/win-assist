#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace wa {

// Outcome of a plugin or built-in skill, shared by every plugin transport.
struct ActionResult {
    bool ok = true;
    std::string display;
    std::string speech;
    std::string inject_text;

    static ActionResult error(std::string message) {
        ActionResult r;
        r.ok = false;
        r.display = std::move(message);
        return r;
    }

    // Parses {"status","display","speech","inject_text"}; non-JSON output is
    // shown verbatim so trivial scripts can just print text.
    static ActionResult from_output(const std::string& output);
};

}  // namespace wa
