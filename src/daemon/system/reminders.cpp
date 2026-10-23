#include "daemon/system/reminders.h"

#include "common/strings.h"

namespace wa {

bool ReminderService::schedule(int delay_ms, std::string message_utf8) {
    if (delay_ms < 1000 || delay_ms > 24 * 60 * 60 * 1000) return false;
    if (message_utf8.empty()) message_utf8 = "Reminder";
    Item item;
    item.due_tick = GetTickCount64() + static_cast<ULONGLONG>(delay_ms);
    item.message = to_wide(message_utf8);
    std::lock_guard lock(mutex_);
    items_.push_back(std::move(item));
    return true;
}

size_t ReminderService::pending() const {
    std::lock_guard lock(mutex_);
    return items_.size();
}

void ReminderService::tick() {
    const ULONGLONG now = GetTickCount64();
    std::vector<std::wstring> due;
    {
        std::lock_guard lock(mutex_);
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->due_tick <= now) {
                due.push_back(std::move(it->message));
                it = items_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& msg : due) {
        if (notify_) notify_(ctx_, msg);
    }
}

}  // namespace wa
