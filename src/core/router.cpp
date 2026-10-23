#include "core/router.h"

#include <windows.h>

#include <algorithm>

#include "common/strings.h"

namespace wa {
namespace {

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

std::wstring strip_edges(std::wstring s) {
    while (!s.empty() && (is_space(s.back()) || is_trailing_punct(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (is_space(s[i]) || is_leading_punct(s[i]))) ++i;
    return s.substr(i);
}

void collapse_spaces(std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool pending = false;
    for (wchar_t c : s) {
        if (is_space(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending) out += L' ';
        pending = false;
        out += c;
    }
    s.swap(out);
}

bool starts_with(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool has_word(const std::wstring& hay, std::wstring_view needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (hay.compare(i, needle.size(), needle) != 0) continue;
        const bool left_ok = i == 0 || !iswalnum(hay[i - 1]);
        const bool right_ok = i + needle.size() == hay.size() || !iswalnum(hay[i + needle.size()]);
        if (left_ok && right_ok) return true;
    }
    return false;
}

bool has_any(const std::wstring& hay, std::initializer_list<const wchar_t*> words) {
    for (const wchar_t* w : words) {
        const std::wstring_view v = w;
        if (v.find(L' ') != std::wstring_view::npos) {
            if (hay.find(v) != std::wstring::npos) return true;
        } else if (has_word(hay, v)) {
            return true;
        }
    }
    return false;
}

bool looks_like_file_request(const std::wstring& folded) {
    if (has_any(folded, {L"file", L"files", L"document", L"documents", L"report", L"pdf", L"docx", L"xlsx", L"файл",
                         L"файли", L"документ", L"документи", L"звіт"})) {
        return true;
    }
    return folded.find(L".pdf") != std::wstring::npos || folded.find(L".doc") != std::wstring::npos ||
           folded.find(L".txt") != std::wstring::npos || folded.find(L".xls") != std::wstring::npos ||
           folded.find(L".png") != std::wstring::npos || folded.find(L".jpg") != std::wstring::npos;
}

// CharLowerBuffW is length-preserving, so erasing the same span from `original`
// keeps capture casing while matching stays case-insensitive.
void strip_fillers(std::wstring& original) {
    static const wchar_t* kLead[] = {
        L"hey ",       L"hi ",           L"hello ",         L"ok ",              L"okay ",
        L"please ",    L"just ",         L"so ",            L"umm ",             L"uh ",
        L"can you ",   L"could you ",    L"would you ",     L"will you ",        L"can u ",
        L"could u ",   L"can ya ",       L"i want you to ", L"i need you to ",   L"i'd like you to ",
        L"i would like you to ", L"do me a favor and ", L"go ahead and ",
        L"будь ласка ", L"можеш ",       L"можна ",         L"давай ",
    };
    static const wchar_t* kTrail[] = {
        L" please", L" for me", L" for us", L" thanks", L" thank you", L" real quick", L" quickly",
        L" будь ласка", L" дякую", L" швидко", L" зараз",
    };
    static const wchar_t* kMid[] = {L" please ", L" just ", L" for me ", L" будь ласка "};

    bool changed = true;
    while (changed) {
        changed = false;
        std::wstring folded = fold_case(original);
        for (const wchar_t* p : kLead) {
            const std::wstring prefix = p;
            if (starts_with(folded, prefix)) {
                original.erase(0, prefix.size());
                changed = true;
                break;
            }
        }
        if (changed) {
            collapse_spaces(original);
            original = strip_edges(original);
            continue;
        }
        folded = fold_case(original);
        for (const wchar_t* p : kTrail) {
            const std::wstring suffix = p;
            if (ends_with(folded, suffix)) {
                original.erase(original.size() - suffix.size());
                changed = true;
                break;
            }
        }
        if (changed) {
            collapse_spaces(original);
            original = strip_edges(original);
            continue;
        }
        folded = fold_case(original);
        for (const wchar_t* mid : kMid) {
            const auto pos = folded.find(mid);
            if (pos == std::wstring::npos) continue;
            original.replace(pos, std::wcslen(mid), L" ");
            changed = true;
            break;
        }
        collapse_spaces(original);
        original = strip_edges(original);
    }
}

std::wstring erase_words(std::wstring folded, std::initializer_list<const wchar_t*> words) {
    for (const wchar_t* raw : words) {
        const std::wstring w = raw;
        for (;;) {
            if (w.find(L' ') != std::wstring::npos) {
                const auto pos = folded.find(w);
                if (pos == std::wstring::npos) break;
                folded.erase(pos, w.size());
                continue;
            }
            size_t i = 0;
            bool found = false;
            while (i + w.size() <= folded.size()) {
                if (folded.compare(i, w.size(), w) == 0) {
                    const bool left_ok = i == 0 || !iswalnum(folded[i - 1]);
                    const bool right_ok = i + w.size() == folded.size() || !iswalnum(folded[i + w.size()]);
                    if (left_ok && right_ok) {
                        folded.erase(i, w.size());
                        found = true;
                        break;
                    }
                }
                ++i;
            }
            if (!found) break;
        }
    }
    collapse_spaces(folded);
    return strip_edges(folded);
}

struct SoftFamily {
    const char* plugin;
    const char* intent;
    const char* param;
    std::initializer_list<const wchar_t*> verbs;
    std::initializer_list<const wchar_t*> require_any;
    std::initializer_list<const wchar_t*> reject_any;
};

const SoftFamily kSoftFamilies[] = {
    {"open_app", "open_application", "app_name",
     {L"open", L"launch", L"start", L"run", L"fire up", L"boot up", L"відкрий", L"відкрити", L"запусти", L"запустити"},
     {},
     {L"file", L"files", L"document", L"documents", L"folder", L"website", L"url", L"link", L"файл", L"документ",
      L"папку", L"сайт"}},
    {"file_search", "find_file", "query",
     {L"find", L"locate", L"search", L"знайди", L"шукай", L"знайти"},
     {},
     {}},
    {"system_controls", "kill_process", "name",
     {L"kill", L"terminate", L"закрий", L"вбий", L"зупини", L"заверши"},
     {},
     {L"window", L"tab", L"file", L"folder", L"вікно", L"вкладку", L"файл"}},
    {"system_controls", "volume_up", nullptr, {L"louder", L"гучніше", L"голосніше"}, {}, {}},
    {"system_controls", "volume_down", nullptr, {L"quieter", L"тихіше", L"тише"}, {}, {}},
    {"system_controls", "lock_computer", nullptr,
     {L"lock", L"заблокуй"},
     {L"pc", L"computer", L"screen", L"workstation", L"комп", L"комп'ютер", L"екран"},
     {}},
    {"system_controls", "show_desktop", nullptr,
     {L"desktop", L"стіл"},
     {L"show", L"minimize", L"покажи", L"згорни"},
     {}},
};

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

std::optional<IntentMatch> Router::match_hard(const std::wstring& text, const std::wstring& folded) const {
    struct Cand {
        IntentMatch match;
        int score = 0;
    };
    std::vector<Cand> cands;
    for (const auto& rule : rules_) {
        std::wsmatch m;
        if (!std::regex_match(folded, m, rule.re)) continue;
        IntentMatch result;
        result.plugin_id = rule.plugin_id;
        result.intent = rule.intent;
        for (size_t i = 0; i < rule.captures.size() && i + 1 < m.size(); ++i) {
            const auto pos = static_cast<size_t>(m.position(static_cast<int>(i + 1)));
            const auto len = static_cast<size_t>(m.length(static_cast<int>(i + 1)));
            result.parameters[rule.captures[i]] = to_utf8(strip_edges(text.substr(pos, len)));
        }
        int score = static_cast<int>(rule.literal_chars) * 10;
        const bool fileish = looks_like_file_request(folded);
        if (rule.intent == "find_file" || rule.plugin_id == "file_search") score += fileish ? 80 : -120;
        if (rule.intent == "open_application" || rule.plugin_id == "open_app") score += fileish ? -80 : 40;
        if (!rule.captures.empty() && result.parameters.is_object()) {
            for (auto it = result.parameters.begin(); it != result.parameters.end(); ++it) {
                if (it->is_string()) score -= static_cast<int>(it->get<std::string>().size());
            }
        }
        result.score = score;
        cands.push_back({std::move(result), score});
    }
    if (cands.empty()) return std::nullopt;
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    if (cands.front().score < 0) return std::nullopt;
    return cands.front().match;
}

std::optional<IntentMatch> Router::match_soft(const std::wstring& /*text*/, const std::wstring& folded) const {
    struct Cand {
        IntentMatch match;
        int score = 0;
    };
    std::vector<Cand> cands;

    for (const SoftFamily& fam : kSoftFamilies) {
        if (!has_any(folded, fam.verbs)) continue;
        if (fam.require_any.size() > 0 && !has_any(folded, fam.require_any)) continue;
        if (fam.reject_any.size() > 0 && has_any(folded, fam.reject_any)) continue;

        bool known = false;
        for (const auto& r : rules_) {
            if (r.plugin_id == fam.plugin && r.intent == fam.intent) {
                known = true;
                break;
            }
        }
        if (!known) continue;

        IntentMatch result;
        result.plugin_id = fam.plugin;
        result.intent = fam.intent;
        int score = 55;
        if (fam.param) {
            std::wstring remainder = erase_words(folded, fam.verbs);
            remainder = erase_words(remainder, fam.require_any);
            remainder = erase_words(remainder, {L"the", L"a", L"an", L"my", L"app", L"application", L"program",
                                                L"process", L"програму", L"додаток", L"процес"});
            if (remainder.empty()) continue;
            result.parameters[fam.param] = to_utf8(remainder);
            score += 10;
            score -= static_cast<int>(remainder.size() / 4);
        }
        if (result.intent == "open_application" && looks_like_file_request(folded)) continue;
        if (result.intent == "find_file" && !looks_like_file_request(folded)) score -= 15;
        result.score = score;
        cands.push_back({std::move(result), score});
    }

    if (cands.empty()) return std::nullopt;
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    if (cands.front().score < 40) return std::nullopt;
    return cands.front().match;
}

std::optional<IntentMatch> Router::match(std::string_view utterance) const {
    const std::wstring text = to_wide(normalize(utterance));
    if (text.empty()) return std::nullopt;
    const std::wstring folded = fold_case(text);
    if (auto hard = match_hard(text, folded)) return hard;
    return match_soft(text, folded);
}

std::string Router::normalize(std::string_view utterance) {
    std::wstring w = to_wide(utterance);
    collapse_spaces(w);
    w = strip_edges(w);
    strip_fillers(w);
    return to_utf8(w);
}

}  // namespace wa
