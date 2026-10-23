// Schedules reminders through the host's schedule_reminder hook.

#include <windows.h>

#include <cctype>
#include <string>

#include <nlohmann/json.hpp>
#include <winassist/plugin_api.h>

using nlohmann::json;

namespace {

const wa_host_api* g_host = nullptr;

std::string reply(bool ok, const std::string& display) {
    return json{{"status", ok ? "ok" : "error"}, {"display", display}, {"speech", ok ? display : ""}}.dump();
}

int parse_delay_ms(const std::string& when) {
    int number = -1;
    std::string unit;
    for (char c : when) {
        if (std::isdigit(static_cast<unsigned char>(c))) number = (number < 0 ? 0 : number * 10) + (c - '0');
        else if (std::isalpha(static_cast<unsigned char>(c))) unit.push_back(static_cast<char>(std::tolower(c)));
    }
    if (number <= 0) {
        // word forms
        if (when.find("half") != std::string::npos) number = 30;
        else if (when.find("quarter") != std::string::npos) number = 15;
        else return -1;
    }
    if (unit.empty() || unit.find("min") != std::string::npos || unit.find("хв") != std::string::npos)
        return number * 60 * 1000;
    if (unit.find("hour") != std::string::npos || unit.find("год") != std::string::npos)
        return number * 60 * 60 * 1000;
    if (unit.find("sec") != std::string::npos || unit.find("сек") != std::string::npos)
        return number * 1000;
    return number * 60 * 1000;  // bare number = minutes
}

std::string handle(const json& request) {
    if (request.value("intent", "") != "set_reminder") return reply(false, "Unknown intent.");
    if (!g_host || !g_host->schedule_reminder)
        return reply(false, "Reminders are not available in this build.");
    const json params = request.value("parameters", json::object());
    const std::string when = params.value("when", "");
    std::string message = params.value("message", "");
    if (message.empty()) message = "Time is up.";
    const int ms = parse_delay_ms(when);
    if (ms < 0) return reply(false, "I couldn't understand the delay. Try 'in 10 minutes'.");
    if (g_host->schedule_reminder(g_host->host, ms, message.c_str()) != 0)
        return reply(false, "Could not schedule that reminder (max 24 hours).");
    const int minutes = (ms + 59999) / 60000;
    return reply(true, "Okay — I'll remind you in about " + std::to_string(minutes) + " min: " + message);
}

}  // namespace

extern "C" {

WA_PLUGIN_EXPORT const wa_plugin_info* wa_plugin_get_info() {
    static const wa_plugin_info info{WA_PLUGIN_API_VERSION, "reminders", "1.0.0"};
    return &info;
}

WA_PLUGIN_EXPORT int wa_plugin_init(const wa_host_api* host) {
    g_host = host;
    return 0;
}

WA_PLUGIN_EXPORT void wa_plugin_shutdown() { g_host = nullptr; }

WA_PLUGIN_EXPORT char* wa_plugin_invoke(const char* request_json) {
    try {
        return _strdup(handle(json::parse(request_json ? request_json : "{}")).c_str());
    } catch (...) {
        return _strdup(R"({"status":"error","display":"Bad request."})");
    }
}

WA_PLUGIN_EXPORT void wa_plugin_free(char* p) { free(p); }

}  // extern "C"
