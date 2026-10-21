#include "core/router.h"

#include <windows.h>

#include <algorithm>

#include "common/strings.h"

namespace wa {
namespace {

// std::regex icase only folds ASCII under the "C" locale; fold with the OS
// instead (length-preserving, so match positions map back to the original).
std::wstring fold_case(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool is_space(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

bool is_regex_special(wchar_t c) {
    return std::wstring_view(L"\\^$.|?*+()[]{}").find(c) != std::wstring_view::npos;
}

bool is_trailing_punct(wchar_t c) {
    return std::wstring_view(L".,!?;:\"'\u201D\u2019\u00BB").find(c) != std::wstring_view::npos;
}

bool is_leading_punct(wchar_t c) { return std::wstring_view(L"\"'\u201C\u2018\u00AB").find(c) != std::wstring_view::npos; }

std::wstring strip_value(std::wstring s) {
    while (!s.empty() && (is_space(s.back()) || is_trailing_punct(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (is_space(s[i]) || is_leading_punct(s[i]))) ++i;
    return s.substr(i);
}

}  // namespace

std::optional<CompiledPattern> compile_pattern(std::string_view pattern_utf8, std::string* error) {
    const std::wstring pattern = fold_case(to_wide(trim(pattern_utf8)));
    CompiledPattern out;
    std::wstring body;
    int group_depth = 0;
    int optional_depth = 0;
    bool leading_optional = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        wchar_t c = pattern[i];
        if (is_space(c)) {
            while (i + 1 < pattern.size() && is_space(pattern[i + 1])) ++i;
            // A space right before "[...]" is folded into the optional group,
            // so "open [the] app" also matches "open app".
            if (i + 1 < pattern.size() && pattern[i + 1] == L'[') continue;
            body += L"\\s+";
            continue;
        }
        switch (c) {
            case L'{': {
                size_t close = pattern.find(L'}', i);
                if (close == std::wstring::npos) {
                    if (error) *error = "unterminated '{' in pattern";
                    return std::nullopt;
                }
                std::string name = to_utf8(to_wide(trim(pattern_utf8)).substr(i + 1, close - i - 1));
                if (name.empty()) {
                    if (error) *error = "empty capture name in pattern";
                    return std::nullopt;
                }
                out.captures.push_back(name);
                body += L"(.+?)";
                i = close;
                break;
            }
            case L'[':
                // Optional words consume the separating space in front of them,
                // or the one after them when they open the pattern.
                if (body.empty()) {
                    body += L"(?:(?:";
                    leading_optional = true;
                } else {
                    body += L"(?:\\s+(?:";
                }
                ++optional_depth;
                break;
            case L']':
                if (optional_depth == 0) {
                    if (error) *error = "unbalanced ']' in pattern";
                    return std::nullopt;
                }
                --optional_depth;
                if (leading_optional && optional_depth == 0) {
                    leading_optional = false;
                    body += L")\\s+)?";
                    while (i + 1 < pattern.size() && is_space(pattern[i + 1])) ++i;
                } else {
                    body += L"))?";
                }
                break;
            case L'(':
                body += L"(?:";
                ++group_depth;
                break;
            case L')':
                if (group_depth == 0) {
                    if (error) *error = "unbalanced ')' in pattern";
                    return std::nullopt;
                }
                body += L')';
                --group_depth;
                break;
            case L'|':
                body += L'|';
                break;
            default:
                if (is_regex_special(c)) body += L'\\';
                body += c;
                ++out.literal_chars;
        }
    }
    if (group_depth != 0 || optional_depth != 0) {
        if (error) *error = "unbalanced brackets in pattern";
        return std::nullopt;
    }
    out.regex = L"^\\s*" + body + L"\\s*$";
    return out;
}

void Router::clear() { rules_.clear(); }

bool Router::add(const std::string& plugin_id, const IntentDef& intent, std::string* error) {
    for (const auto& pattern : intent.patterns) {
        auto compiled = compile_pattern(pattern, error);
        if (!compiled) return false;
        Rule rule;
        rule.plugin_id = plugin_id;
        rule.intent = intent.name;
        rule.captures = compiled->captures;
        rule.literal_chars = compiled->literal_chars;
        try {
            rule.re = std::wregex(compiled->regex, std::regex::ECMAScript | std::regex::optimize);
        } catch (const std::regex_error& e) {
            if (error) *error = std::string("bad pattern '") + pattern + "': " + e.what();
            return false;
        }
        rules_.push_back(std::move(rule));
    }
    std::stable_sort(rules_.begin(), rules_.end(),
                     [](const Rule& a, const Rule& b) { return a.literal_chars > b.literal_chars; });
    return true;
}

std::optional<IntentMatch> Router::match(std::string_view utterance) const {
    const std::wstring text = to_wide(normalize(utterance));
    if (text.empty()) return std::nullopt;
    const std::wstring folded = fold_case(text);
    std::wsmatch m;
    for (const auto& rule : rules_) {
        if (!std::regex_match(folded, m, rule.re)) continue;
        IntentMatch result;
        result.plugin_id = rule.plugin_id;
        result.intent = rule.intent;
        for (size_t i = 0; i < rule.captures.size() && i + 1 < m.size(); ++i) {
            const auto pos = static_cast<size_t>(m.position(static_cast<int>(i + 1)));
            const auto len = static_cast<size_t>(m.length(static_cast<int>(i + 1)));
            result.parameters[rule.captures[i]] = to_utf8(strip_value(text.substr(pos, len)));
        }
        return result;
    }
    return std::nullopt;
}

std::string Router::normalize(std::string_view utterance) {
    std::wstring w = to_wide(utterance);
    std::wstring collapsed;
    collapsed.reserve(w.size());
    bool pending_space = false;
    for (wchar_t c : w) {
        if (is_space(c)) {
            pending_space = !collapsed.empty();
            continue;
        }
        if (pending_space) collapsed += L' ';
        pending_space = false;
        collapsed += c;
    }
    return to_utf8(strip_value(collapsed));
}

}  // namespace wa
