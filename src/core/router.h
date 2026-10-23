#pragma once

#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/manifest.h"

namespace wa {

struct IntentMatch {
    std::string plugin_id;
    std::string intent;
    nlohmann::json parameters = nlohmann::json::object();
    // 0 = hard pattern; higher soft scores are more confident keyword hits.
    int score = 0;
};

struct CompiledPattern {
    std::wstring regex;
    std::vector<std::string> captures;
    size_t literal_chars = 0;
};

// Pattern syntax:
//   {name}      captures a parameter (lazy, at least one character)
//   [words]     optional words
//   (a|b|c)     alternatives
// Whitespace matches one or more spaces; matching is case-insensitive and
// anchored to the whole (normalized) utterance.
std::optional<CompiledPattern> compile_pattern(std::string_view pattern, std::string* error);

class Router {
public:
    void clear();
    bool add(const std::string& plugin_id, const IntentDef& intent, std::string* error);
    std::optional<IntentMatch> match(std::string_view utterance) const;
    size_t size() const { return rules_.size(); }

    // Trims whitespace/quotes/STT punctuation, then peels polite filler
    // ("can you … please", "будь ласка …") so patterns don't need every phrasing.
    static std::string normalize(std::string_view utterance);

private:
    struct Rule {
        std::string plugin_id;
        std::string intent;
        std::wregex re;
        std::vector<std::string> captures;
        size_t literal_chars = 0;
    };
    std::vector<Rule> rules_;

    std::optional<IntentMatch> match_hard(const std::wstring& text, const std::wstring& folded) const;
    std::optional<IntentMatch> match_soft(const std::wstring& text, const std::wstring& folded) const;
};

}  // namespace wa
