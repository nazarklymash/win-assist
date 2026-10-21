#pragma once

#include <windows.h>

#include <string>

namespace wa {

enum class TrayState { Idle, Listening, Busy, Error };

class Tray {
public:
    ~Tray();
    bool create(HWND owner, UINT callback_message);
    void destroy();
    // Re-adds the icon after Explorer restarts.
    void recreate();
    void set_state(TrayState state);
    void set_tooltip(const std::wstring& text);
    void notify(const std::wstring& title, const std::wstring& text);

private:
    HICON icon_for(TrayState state);
    void apply(DWORD message);

    HWND owner_ = nullptr;
    UINT callback_ = 0;
    bool added_ = false;
    TrayState state_ = TrayState::Idle;
    std::wstring tooltip_ = L"win-assist";
    HICON icons_[4] = {};
};

}  // namespace wa
