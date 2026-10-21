// Native plugin: volume, media keys, brightness, lock and sleep.

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <powrprof.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>
#include <winassist/plugin_api.h>

using Microsoft::WRL::ComPtr;
using nlohmann::json;

namespace {

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::string reply(bool ok, const std::string& display) {
    return json{{"status", ok ? "ok" : "error"}, {"display", display}, {"speech", ok ? display : ""}}.dump();
}

ComPtr<IAudioEndpointVolume> endpoint_volume() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(volume.GetAddressOf())))) {
        return nullptr;
    }
    return volume;
}

int parse_percent(const json& params) {
    auto it = params.find("level");
    if (it == params.end()) return -1;
    if (it->is_number()) return static_cast<int>(it->get<double>());
    if (!it->is_string()) return -1;
    const std::string s = it->get<std::string>();
    int value = -1;
    for (char c : s) {
        if (c >= '0' && c <= '9') value = (value < 0 ? 0 : value * 10) + (c - '0');
        else if (value >= 0) break;
    }
    if (value >= 0) return value;
    static const std::pair<const char*, int> words[] = {
        {"zero", 0},   {"ten", 10},     {"twenty", 20}, {"thirty", 30}, {"forty", 40},  {"fifty", 50},
        {"sixty", 60}, {"seventy", 70}, {"eighty", 80}, {"ninety", 90}, {"hundred", 100}, {"half", 50},
        {"max", 100},  {"full", 100},
    };
    for (const auto& [word, n] : words) {
        if (s.find(word) != std::string::npos) return n;
    }
    return -1;
}

std::string change_volume(float delta) {
    auto volume = endpoint_volume();
    if (!volume) return reply(false, "No audio output device found.");
    float level = 0.0f;
    volume->GetMasterVolumeLevelScalar(&level);
    level = std::fmin(1.0f, std::fmax(0.0f, level + delta));
    volume->SetMute(FALSE, nullptr);
    volume->SetMasterVolumeLevelScalar(level, nullptr);
    return reply(true, "Volume " + std::to_string(static_cast<int>(std::lround(level * 100))) + "%");
}

std::string set_volume(int percent) {
    if (percent < 0 || percent > 100) return reply(false, "Volume must be between 0 and 100.");
    auto volume = endpoint_volume();
    if (!volume) return reply(false, "No audio output device found.");
    volume->SetMute(FALSE, nullptr);
    volume->SetMasterVolumeLevelScalar(percent / 100.0f, nullptr);
    return reply(true, "Volume " + std::to_string(percent) + "%");
}

std::string set_mute(bool mute) {
    auto volume = endpoint_volume();
    if (!volume) return reply(false, "No audio output device found.");
    volume->SetMute(mute ? TRUE : FALSE, nullptr);
    return reply(true, mute ? "Muted" : "Unmuted");
}

std::string media_key(WORD vk, const char* label) {
    INPUT inputs[2] = {};
    inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
    return reply(true, label);
}

std::string set_brightness(int percent) {
    if (percent < 0 || percent > 100) return reply(false, "Brightness must be between 0 and 100.");
    ComPtr<IWbemLocator> locator;
    ComPtr<IWbemServices> services;
    ComPtr<IEnumWbemClassObject> instances;
    ComPtr<IWbemClassObject> instance;
    ComPtr<IWbemClassObject> class_def;
    ComPtr<IWbemClassObject> in_def;
    ComPtr<IWbemClassObject> args;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator));
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    if (SUCCEEDED(hr)) hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(ns);
    if (SUCCEEDED(hr)) {
        hr = CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                               RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    }
    BSTR cls = SysAllocString(L"WmiMonitorBrightnessMethods");
    if (SUCCEEDED(hr)) hr = services->CreateInstanceEnum(cls, WBEM_FLAG_FORWARD_ONLY, nullptr, &instances);
    ULONG count = 0;
    if (SUCCEEDED(hr)) hr = instances->Next(WBEM_INFINITE, 1, &instance, &count);
    if (SUCCEEDED(hr) && count == 0) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = services->GetObject(cls, 0, nullptr, &class_def, nullptr);
    if (SUCCEEDED(hr)) hr = class_def->GetMethod(L"WmiSetBrightness", 0, &in_def, nullptr);
    if (SUCCEEDED(hr)) hr = in_def->SpawnInstance(0, &args);
    if (SUCCEEDED(hr)) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = 0;
        args->Put(L"Timeout", 0, &v, CIM_UINT32);
        v.vt = VT_UI1;
        v.bVal = static_cast<BYTE>(percent);
        hr = args->Put(L"Brightness", 0, &v, CIM_UINT8);
    }
    VARIANT path;
    VariantInit(&path);
    if (SUCCEEDED(hr)) hr = instance->Get(L"__PATH", 0, &path, nullptr, nullptr);
    BSTR method = SysAllocString(L"WmiSetBrightness");
    if (SUCCEEDED(hr)) hr = services->ExecMethod(path.bstrVal, method, 0, nullptr, args.Get(), nullptr, nullptr);
    SysFreeString(method);
    VariantClear(&path);
    SysFreeString(cls);
    if (FAILED(hr)) return reply(false, "This display does not support brightness control.");
    return reply(true, "Brightness " + std::to_string(percent) + "%");
}

std::string handle(const std::string& request_json) {
    json request = json::parse(request_json, nullptr, false);
    if (request.is_discarded()) return reply(false, "Bad request.");
    const std::string intent = request.value("intent", "");
    const json params = request.value("parameters", json::object());
    ComScope com;

    if (intent == "volume_up") return change_volume(0.1f);
    if (intent == "volume_down") return change_volume(-0.1f);
    if (intent == "set_volume") return set_volume(parse_percent(params));
    if (intent == "mute") return set_mute(true);
    if (intent == "unmute") return set_mute(false);
    if (intent == "media_play_pause") return media_key(VK_MEDIA_PLAY_PAUSE, "Play/pause");
    if (intent == "media_next") return media_key(VK_MEDIA_NEXT_TRACK, "Next track");
    if (intent == "media_previous") return media_key(VK_MEDIA_PREV_TRACK, "Previous track");
    if (intent == "set_brightness") return set_brightness(parse_percent(params));
    if (intent == "lock_computer") {
        return LockWorkStation() ? reply(true, "Locked") : reply(false, "Could not lock the computer.");
    }
    if (intent == "sleep_computer") {
        return SetSuspendState(FALSE, FALSE, FALSE) ? reply(true, "Going to sleep")
                                                    : reply(false, "Sleep is not available on this device.");
    }
    return reply(false, "Unknown intent '" + intent + "'.");
}

const wa_plugin_info kInfo = {WA_PLUGIN_API_VERSION, "system_controls", "1.0.0"};

}  // namespace

extern "C" {

WA_PLUGIN_EXPORT const wa_plugin_info* wa_plugin_get_info(void) { return &kInfo; }

WA_PLUGIN_EXPORT char* wa_plugin_invoke(const char* request_json) {
    std::string out = handle(request_json ? request_json : "");
    char* buffer = static_cast<char*>(malloc(out.size() + 1));
    if (buffer) memcpy(buffer, out.c_str(), out.size() + 1);
    return buffer;
}

WA_PLUGIN_EXPORT void wa_plugin_free(char* response_json) { free(response_json); }

}  // extern "C"
