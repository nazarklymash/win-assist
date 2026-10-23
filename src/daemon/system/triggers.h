#pragma once

#include <string>
#include <vector>

#include "core/config.h"

namespace wa {

// Polled system state the rules are matched against.
struct SystemSnapshot {
    // Lowercase executable names of the running processes, e.g. "code.exe".
    std::vector<std::string> processes;
    // Lowercase title of the foreground window.
    std::string foreground_title;
    // -1 when the machine has no battery.
    int battery_percent = -1;
    bool on_ac = true;
};

struct TriggerFire {
    std::string rule;
    std::string notify;
    std::string ask;
};

// Edge detection over consecutive snapshots. It never reads system state
// itself, so the rule semantics are testable; App feeds it real snapshots
// once per poll_sec.
class TriggerEngine {
public:
    void set_rules(std::vector<TriggerRule> rules);
    bool empty() const { return rules_.empty(); }

    // Fires rules whose event occurred between the previous snapshot and this
    // one. The first snapshot only establishes a baseline and fires nothing.
    std::vector<TriggerFire> evaluate(const SystemSnapshot& now, int64_t now_sec);

private:
    std::vector<TriggerRule> rules_;
    std::vector<int64_t> last_fired_;
    SystemSnapshot prev_;
    bool has_prev_ = false;
};

// Collects the current state of the machine.
SystemSnapshot take_snapshot();

}  // namespace wa
