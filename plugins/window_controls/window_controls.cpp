// Native plugin: focus, snap, minimize, move across monitors.

#include <windows.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <winassist/plugin_api.h>

using nlohmann::json;

namespace {

std::string reply(bool ok, const std::string& display) {
    return json{{"status", ok ? "ok" : "error"}, {"display", display}, {"speech", ok ? display : ""}}.dump();
}

std::wstring utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring to_lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

struct WinInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring process;
};

std::wstring process_name(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &size)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        name = base ? base + 1 : path;
    }
    CloseHandle(h);
    return name;
}

std::vector<WinInfo> list_windows() {
    std::vector<WinInfo> out;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
            wchar_t title[512] = {};
            GetWindowTextW(hwnd, title, 512);
            if (!title[0]) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            auto* list = reinterpret_cast<std::vector<WinInfo>*>(lp);
            list->push_back({hwnd, title, process_name(pid)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

HWND resolve_window(const json& params, const json& active) {
    std::string name = params.value("name", "");
    if (name.empty()) {
        uint64_t hwnd = active.value("hwnd", 0ULL);
        if (hwnd) return reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd));
        return GetForegroundWindow();
    }
    const std::wstring needle = to_lower(utf8(name));
    int best = -1;
    HWND best_hwnd = nullptr;
    for (const auto& w : list_windows()) {
        const std::wstring t = to_lower(w.title);
        const std::wstring p = to_lower(w.process);
        int score = 0;
        if (t == needle || p == needle) score = 100;
        else if (t.find(needle) != std::wstring::npos) score = 80;
        else if (p.find(needle) != std::wstring::npos) score = 70;
        if (score > best) {
            best = score;
            best_hwnd = w.hwnd;
        }
    }
    return best_hwnd;
}

bool force_fg(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD self = GetCurrentThreadId();
    bool attached = fg_thread && fg_thread != self && AttachThreadInput(fg_thread, self, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(fg_thread, self, FALSE);
    return GetForegroundWindow() == hwnd;
}

void snap(HWND hwnd, bool left) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& r = mi.rcWork;
    const int w = (r.right - r.left) / 2;
    const int h = r.bottom - r.top;
    const int x = left ? r.left : r.left + w;
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, nullptr, x, r.top, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
}

void move_next_monitor(HWND hwnd) {
    HMONITOR cur = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    struct Ctx {
        HMONITOR cur;
        HMONITOR next = nullptr;
        HMONITOR first = nullptr;
        bool seen = false;
    } ctx{cur};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (!c->first) c->first = mon;
            if (c->seen && !c->next) c->next = mon;
            if (mon == c->cur) c->seen = true;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    HMONITOR dest = ctx.next ? ctx.next : ctx.first;
    if (!dest || dest == cur) return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(dest, &mi);
    WINDOWPLACEMENT wp{sizeof(wp)};
    GetWindowPlacement(hwnd, &wp);
    const int width = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    const int height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, nullptr, mi.rcWork.left + 40, mi.rcWork.top + 40, width, height, SWP_NOZORDER | SWP_SHOWWINDOW);
}

std::string title_of(HWND hwnd) {
    wchar_t t[256] = {};
    GetWindowTextW(hwnd, t, 256);
    return narrow(t);
}

std::string handle(const json& request) {
    const std::string intent = request.value("intent", "");
    const json params = request.value("parameters", json::object());
    const json active = request.value("active_window", json::object());
    HWND hwnd = resolve_window(params, active);
    if (!hwnd && intent != "minimize_others") return reply(false, "No matching window found.");

    if (intent == "focus_window") {
        if (!force_fg(hwnd)) return reply(false, "Could not focus that window.");
        return reply(true, "Focused " + title_of(hwnd));
    }
    if (intent == "snap_left") {
        force_fg(hwnd);
        snap(hwnd, true);
        return reply(true, "Snapped left: " + title_of(hwnd));
    }
    if (intent == "snap_right") {
        force_fg(hwnd);
        snap(hwnd, false);
        return reply(true, "Snapped right: " + title_of(hwnd));
    }
    if (intent == "maximize_window") {
        force_fg(hwnd);
        ShowWindow(hwnd, SW_MAXIMIZE);
        return reply(true, "Maximized " + title_of(hwnd));
    }
    if (intent == "minimize_window") {
        ShowWindow(hwnd, SW_MINIMIZE);
        return reply(true, "Minimized " + title_of(hwnd));
    }
    if (intent == "minimize_others") {
        HWND keep = hwnd ? hwnd : GetForegroundWindow();
        for (const auto& w : list_windows()) {
            if (w.hwnd != keep) ShowWindow(w.hwnd, SW_MINIMIZE);
        }
        return reply(true, "Minimized other windows.");
    }
    if (intent == "move_to_next_monitor") {
        force_fg(hwnd);
        move_next_monitor(hwnd);
        return reply(true, "Moved to the next monitor: " + title_of(hwnd));
    }
    return reply(false, "Unknown intent '" + intent + "'.");
}

}  // namespace

extern "C" {

WA_PLUGIN_EXPORT const wa_plugin_info* wa_plugin_get_info() {
    static const wa_plugin_info info{WA_PLUGIN_API_VERSION, "window_controls", "1.0.0"};
    return &info;
}

WA_PLUGIN_EXPORT char* wa_plugin_invoke(const char* request_json) {
    try {
        auto j = json::parse(request_json ? request_json : "{}");
        std::string out = handle(j);
        return _strdup(out.c_str());
    } catch (...) {
        return _strdup(R"({"status":"error","display":"Bad request."})");
    }
}

WA_PLUGIN_EXPORT void wa_plugin_free(char* response_json) { free(response_json); }

}  // extern "C"
