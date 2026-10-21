#include "daemon/system/inject.h"

#include <windows.h>

#include <vector>

#include "common/log.h"

namespace wa {
namespace {

constexpr size_t kMaxTypedChars = 120;
constexpr DWORD kTypeDelayMs = 5;
constexpr WORD kMaskKey = 0xE8;  // unassigned VK

INPUT key_input(WORD vk, WORD scan, DWORD flags) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = scan;
    in.ki.dwFlags = flags;
    return in;
}

bool send_inputs(std::vector<INPUT>& inputs) {
    if (inputs.empty()) return true;
    UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    return sent == inputs.size();
}

// One character per SendInput call with a short pause: apps that read input
// through TSF (Windows 11 Notepad, WinUI) garble large VK_PACKET bursts.
bool type_unicode(std::wstring_view text) {
    std::vector<INPUT> inputs;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'\r') continue;
        inputs.clear();
        if (c == L'\n') {
            inputs.push_back(key_input(VK_RETURN, 0, 0));
            inputs.push_back(key_input(VK_RETURN, 0, KEYEVENTF_KEYUP));
        } else {
            inputs.push_back(key_input(0, c, KEYEVENTF_UNICODE));
            inputs.push_back(key_input(0, c, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
            if (IS_HIGH_SURROGATE(c) && i + 1 < text.size()) {
                wchar_t low = text[++i];
                inputs.push_back(key_input(0, low, KEYEVENTF_UNICODE));
                inputs.push_back(key_input(0, low, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
            }
        }
        if (!send_inputs(inputs)) return false;
        Sleep(kTypeDelayMs);
    }
    return true;
}

bool send_paste() {
    std::vector<INPUT> inputs = {
        key_input(VK_CONTROL, 0, 0),
        key_input('V', 0, 0),
        key_input('V', 0, KEYEVENTF_KEYUP),
        key_input(VK_CONTROL, 0, KEYEVENTF_KEYUP),
    };
    return send_inputs(inputs);
}

bool open_clipboard_retry() {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(20);
    }
    return false;
}

}  // namespace

void send_menu_mask_key() {
    std::vector<INPUT> inputs = {key_input(kMaskKey, 0, 0), key_input(kMaskKey, 0, KEYEVENTF_KEYUP)};
    send_inputs(inputs);
}

void wait_for_modifier_release(int timeout_ms) {
    const int keys[] = {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN};
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        bool any = false;
        for (int k : keys) any |= (GetAsyncKeyState(k) & 0x8000) != 0;
        if (!any) return;
        Sleep(10);
    }
}

std::wstring get_clipboard_text() {
    std::wstring text;
    if (!open_clipboard_retry()) return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
            text = p;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

bool set_clipboard_text(std::wstring_view text, bool exclude_from_history) {
    if (!open_clipboard_retry()) return false;
    EmptyClipboard();
    bool ok = false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (mem) {
        auto* p = static_cast<wchar_t*>(GlobalLock(mem));
        memcpy(p, text.data(), text.size() * sizeof(wchar_t));
        p[text.size()] = L'\0';
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
    }
    if (ok && exclude_from_history) {
        // Honored by Windows clipboard history and well-behaved clipboard managers.
        static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
        if (HGLOBAL flag = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
            *static_cast<DWORD*>(GlobalLock(flag)) = 0;
            GlobalUnlock(flag);
            if (!SetClipboardData(exclude, flag)) GlobalFree(flag);
        }
    }
    CloseClipboard();
    return ok;
}

bool inject_text(const TargetInfo& target, std::wstring_view text, std::string& error, InjectMethod method) {
    if (text.empty()) return true;
    if (!target.window || !IsWindow(target.window)) {
        error = "The target window is gone.";
        return false;
    }
    if (target.elevated && !current_process_elevated()) {
        error = "The target app runs as administrator; Windows blocks typing into it. Text copied to clipboard.";
        set_clipboard_text(text, false);
        return false;
    }

    wait_for_modifier_release(1500);
    if (GetForegroundWindow() != target.window) {
        force_foreground(target.window);
        for (int i = 0; i < 25 && GetForegroundWindow() != target.window; ++i) Sleep(20);
        if (target.focus && IsWindow(target.focus)) {
            DWORD thread = GetWindowThreadProcessId(target.window, nullptr);
            DWORD self = GetCurrentThreadId();
            if (AttachThreadInput(self, thread, TRUE)) {
                SetFocus(target.focus);
                AttachThreadInput(self, thread, FALSE);
            }
        }
        Sleep(30);
    }

    bool use_clipboard = method == InjectMethod::Clipboard ||
                         (method == InjectMethod::Auto && (text.size() > kMaxTypedChars ||
                                                           text.find(L'\n') != std::wstring_view::npos));
    if (!use_clipboard) {
        if (type_unicode(text)) return true;
        log::warn("SendInput typing failed, falling back to clipboard");
    }

    const std::wstring previous = get_clipboard_text();
    if (!set_clipboard_text(text, true)) {
        error = "Could not open the clipboard.";
        return false;
    }
    bool ok = send_paste();
    // Give the target time to read the clipboard before restoring it.
    Sleep(250);
    if (!previous.empty()) set_clipboard_text(previous, true);
    if (!ok) error = "Paste was blocked by the target window.";
    return ok;
}

}  // namespace wa
