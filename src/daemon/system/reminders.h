#pragma once

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

namespace wa {

// UI-thread reminder queue. schedule() is safe from worker threads; due items
// are fired from tick() on the message pump (tray balloon + optional TTS hook).
class ReminderService {
public:
    using NotifyFn = void (*)(void* ctx, const std::wstring& message);

    void set_notify(void* ctx, NotifyFn fn) {
        ctx_ = ctx;
        notify_ = fn;
    }

    // Returns false if the delay is out of range.
    bool schedule(int delay_ms, std::string message_utf8);
    void tick();  // call ~1/s from the UI thread
    size_t pending() const;

private:
    struct Item {
        ULONGLONG due_tick = 0;
        std::wstring message;
    };

    mutable std::mutex mutex_;
    std::vector<Item> items_;
    void* ctx_ = nullptr;
    NotifyFn notify_ = nullptr;
};

}  // namespace wa
