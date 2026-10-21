#include "daemon/system/target.h"

#include "common/strings.h"

namespace wa {
namespace {

bool process_elevated(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

}  // namespace

nlohmann::json TargetInfo::to_json() const {
    return {{"title", to_utf8(title)},
            {"process", to_utf8(process)},
            {"hwnd", reinterpret_cast<uintptr_t>(window)},
            {"elevated", elevated}};
}

bool current_process_elevated() { return process_elevated(GetCurrentProcess()); }

TargetInfo capture_target() {
    TargetInfo info;
    info.window = GetForegroundWindow();
    if (!info.window) return info;

    DWORD thread = GetWindowThreadProcessId(info.window, &info.pid);
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(thread, &gti)) {
        info.focus = gti.hwndFocus;
        if (gti.hwndCaret) {
            info.has_caret = true;
            info.caret = gti.rcCaret;
            MapWindowPoints(gti.hwndCaret, nullptr, reinterpret_cast<POINT*>(&info.caret), 2);
        }
    }

    int len = GetWindowTextLengthW(info.window);
    if (len > 0) {
        info.title.resize(static_cast<size_t>(len) + 1);
        info.title.resize(static_cast<size_t>(GetWindowTextW(info.window, info.title.data(), len + 1)));
    }

    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info.pid)) {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            std::wstring full(path, size);
            size_t slash = full.find_last_of(L"\\/");
            info.process = slash == std::wstring::npos ? full : full.substr(slash + 1);
        }
        info.elevated = process_elevated(process);
        CloseHandle(process);
    }
    return info;
}

bool force_foreground(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (GetForegroundWindow() == hwnd) return true;
    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return true;

    HWND foreground = GetForegroundWindow();
    DWORD fg_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    DWORD self = GetCurrentThreadId();
    bool attached = fg_thread && fg_thread != self && AttachThreadInput(fg_thread, self, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (attached) AttachThreadInput(fg_thread, self, FALSE);
    return GetForegroundWindow() == hwnd;
}

}  // namespace wa
