#include "daemon/system/triggers.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>

#include "common/log.h"
#include "common/strings.h"

namespace wa {
namespace {

// Window titles are often Cyrillic here, so fold with the Win32 table.
std::string fold_case(std::string_view text) {
    std::wstring wide = to_wide(text);
    if (!wide.empty()) CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    return to_utf8(wide);
}

bool has_process(const std::vector<std::string>& processes, const std::string& name) {
    return std::find(processes.begin(), processes.end(), name) != processes.end();
}

bool title_matches(const std::string& title, const std::string& match) {
    return !match.empty() && title.find(match) != std::string::npos;
}

// True when the rule's event happened between `prev` and `now`.
bool fired(const TriggerRule& rule, const std::string& match, const SystemSnapshot& prev, const SystemSnapshot& now) {
    if (rule.when == "app_launch") return has_process(now.processes, match) && !has_process(prev.processes, match);
    if (rule.when == "app_exit") return has_process(prev.processes, match) && !has_process(now.processes, match);
    if (rule.when == "window_title")
        return title_matches(now.foreground_title, match) && !title_matches(prev.foreground_title, match);
    if (rule.when == "battery_below")
        return now.battery_percent >= 0 && now.battery_percent < rule.threshold &&
               !(prev.battery_percent >= 0 && prev.battery_percent < rule.threshold);
    if (rule.when == "battery_above")
        return now.battery_percent >= 0 && now.battery_percent > rule.threshold &&
               !(prev.battery_percent >= 0 && prev.battery_percent > rule.threshold);
    if (rule.when == "on_battery") return !now.on_ac && prev.on_ac;
    if (rule.when == "on_ac") return now.on_ac && !prev.on_ac;
    return false;
}

}  // namespace

void TriggerEngine::set_rules(std::vector<TriggerRule> rules) {
    rules_ = std::move(rules);
    last_fired_.assign(rules_.size(), 0);
}

std::vector<TriggerFire> TriggerEngine::evaluate(const SystemSnapshot& now, int64_t now_sec) {
    std::vector<TriggerFire> fires;
    if (!has_prev_) {
        // Otherwise every already-running app would look like it just launched.
        prev_ = now;
        has_prev_ = true;
        return fires;
    }
    for (size_t i = 0; i < rules_.size(); ++i) {
        const TriggerRule& rule = rules_[i];
        if (!rule.enabled) continue;
        if (rule.notify.empty() && rule.ask.empty()) continue;
        const std::string match = fold_case(rule.match);
        if (!fired(rule, match, prev_, now)) continue;
        if (last_fired_[i] != 0 && now_sec - last_fired_[i] < std::max(0, rule.cooldown_sec)) continue;
        last_fired_[i] = now_sec;
        fires.push_back({rule.name.empty() ? rule.when : rule.name, rule.notify, rule.ask});
    }
    prev_ = now;
    return fires;
}

SystemSnapshot take_snapshot() {
    SystemSnapshot snap;

    DWORD pids[2048];
    DWORD bytes = 0;
    if (EnumProcesses(pids, sizeof(pids), &bytes)) {
        const size_t count = bytes / sizeof(DWORD);
        snap.processes.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
            if (!process) continue;
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                const wchar_t* name = wcsrchr(path, L'\\');
                snap.processes.push_back(fold_case(to_utf8(name ? name + 1 : path)));
            }
            CloseHandle(process);
        }
        std::sort(snap.processes.begin(), snap.processes.end());
        snap.processes.erase(std::unique(snap.processes.begin(), snap.processes.end()), snap.processes.end());
    }

    if (HWND fg = GetForegroundWindow()) {
        wchar_t title[512];
        const int len = GetWindowTextW(fg, title, 512);
        if (len > 0) snap.foreground_title = fold_case(to_utf8(std::wstring_view(title, len)));
    }

    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        snap.on_ac = power.ACLineStatus != 0;
        if (power.BatteryLifePercent <= 100) snap.battery_percent = power.BatteryLifePercent;
    }
    return snap;
}

}  // namespace wa
