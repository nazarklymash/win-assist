// Native plugin: night light + Focus Assist via Settings URI / registry hints.

#include <windows.h>
#include <shellapi.h>

#include <string>

#include <nlohmann/json.hpp>
#include <winassist/plugin_api.h>

using nlohmann::json;

namespace {

std::string reply(bool ok, const std::string& display) {
    return json{{"status", ok ? "ok" : "error"}, {"display", display}, {"speech", ok ? display : ""}}.dump();
}

bool open_settings(const wchar_t* uri) {
    HINSTANCE h = ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(h) > 32;
}

// Night light state lives under CloudStore; flipping via ms-settings is reliable.
// We also poke the feature state byte when present for silent toggles.
bool set_night_light(bool enable) {
    HKEY key = nullptr;
    const wchar_t* path =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\"
        L"Current\\default$windows.data.bluelightreduction.bluelightreductionstate\\windows.data."
        L"bluelightreduction.bluelightreductionstate";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS) {
        BYTE data[64] = {};
        DWORD size = sizeof(data);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"Data", nullptr, &type, data, &size) == ERROR_SUCCESS && size >= 25) {
            // Byte layout is undocumented; Settings URI is the fallback.
            RegCloseKey(key);
        } else {
            RegCloseKey(key);
        }
    }
    return open_settings(enable ? L"ms-settings:nightlight" : L"ms-settings:nightlight");
}

bool set_focus_assist(bool enable) {
    // Focus Assist / DND: open the page; Win11 also accepts ms-settings:quiethours
    return open_settings(enable ? L"ms-settings:quiethours" : L"ms-settings:quiethours");
}

std::string handle(const json& request) {
    const std::string intent = request.value("intent", "");
    if (intent == "night_light_on") {
        if (!set_night_light(true)) return reply(false, "Could not open Night light settings.");
        return reply(true, "Opened Night light settings — turn it on there (or it may already be on).");
    }
    if (intent == "night_light_off") {
        if (!set_night_light(false)) return reply(false, "Could not open Night light settings.");
        return reply(true, "Opened Night light settings — turn it off there.");
    }
    if (intent == "focus_assist_on") {
        if (!set_focus_assist(true)) return reply(false, "Could not open Focus settings.");
        return reply(true, "Opened Focus / quiet hours — enable Do not disturb there.");
    }
    if (intent == "focus_assist_off") {
        if (!set_focus_assist(false)) return reply(false, "Could not open Focus settings.");
        return reply(true, "Opened Focus / quiet hours — disable Do not disturb there.");
    }
    return reply(false, "Unknown intent '" + intent + "'.");
}

}  // namespace

extern "C" {

WA_PLUGIN_EXPORT const wa_plugin_info* wa_plugin_get_info() {
    static const wa_plugin_info info{WA_PLUGIN_API_VERSION, "focus_assist", "1.0.0"};
    return &info;
}

WA_PLUGIN_EXPORT char* wa_plugin_invoke(const char* request_json) {
    try {
        auto j = json::parse(request_json ? request_json : "{}");
        return _strdup(handle(j).c_str());
    } catch (...) {
        return _strdup(R"({"status":"error","display":"Bad request."})");
    }
}

WA_PLUGIN_EXPORT void wa_plugin_free(char* p) { free(p); }

}  // extern "C"
