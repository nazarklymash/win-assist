#pragma once

#include <string>
#include <string_view>

#include "daemon/system/target.h"

namespace wa {

enum class InjectMethod { Auto, Keys, Clipboard };

// Types `text` into the target window. Short text is sent as Unicode key
// events; long or multi-line text is pasted via the clipboard, whose previous
// text content is restored afterwards.
bool inject_text(const TargetInfo& target, std::wstring_view text, std::string& error,
                 InjectMethod method = InjectMethod::Auto);

// Sends an unassigned virtual key so a lone Alt/Win release (from the hotkey)
// does not open the target's menu bar or the Start menu.
void send_menu_mask_key();

// Waits until the user has physically released Ctrl/Alt/Shift/Win.
void wait_for_modifier_release(int timeout_ms);

bool set_clipboard_text(std::wstring_view text, bool exclude_from_history);
std::wstring get_clipboard_text();

// Copies the current selection in `target` via Ctrl+C, restoring the previous
// clipboard contents afterwards. Returns false when nothing was selected.
bool capture_selection(const TargetInfo& target, std::wstring& out, std::string& error);

}  // namespace wa
