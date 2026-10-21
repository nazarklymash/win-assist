#pragma once

#include <windows.h>

#include <string>

#include <nlohmann/json.hpp>

namespace wa {

// The window that had keyboard focus when the assistant was triggered;
// results are injected back into it.
struct TargetInfo {
    HWND window = nullptr;  // top-level foreground window
    HWND focus = nullptr;   // focused child control, if any
    RECT caret{};
    bool has_caret = false;
    DWORD pid = 0;
    std::wstring title;
    std::wstring process;  // executable file name, e.g. WINWORD.EXE
    bool elevated = false;

    nlohmann::json to_json() const;
};

TargetInfo capture_target();

// Brings `hwnd` to the foreground even when the foreground lock would refuse.
bool force_foreground(HWND hwnd);

bool current_process_elevated();

}  // namespace wa
