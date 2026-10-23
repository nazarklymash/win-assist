// Native plugin: volume, media, brightness, power, processes, shell,
// filesystem, clipboard, and system info.

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <powrprof.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
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

int parse_int(const json& params, const char* key, int fallback = -1) {
    auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_number()) return static_cast<int>(it->get<double>());
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string param_str(const json& params, const char* key) {
    auto it = params.find(key);
    if (it == params.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number()) return std::to_string(it->is_number_integer() ? it->get<int>() : static_cast<int>(it->get<double>()));
    return it->dump();
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

bool enable_shutdown_privilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(token);
    return ok && GetLastError() == ERROR_SUCCESS;
}

struct ProcRow {
    DWORD pid = 0;
    std::wstring name;
    SIZE_T working_set = 0;
};

std::vector<ProcRow> snapshot_processes() {
    std::vector<ProcRow> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcRow row;
            row.pid = pe.th32ProcessID;
            row.name = pe.szExeFile;
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, row.pid)) {
                PROCESS_MEMORY_COUNTERS pmc{};
                if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) row.working_set = pmc.WorkingSetSize;
                CloseHandle(h);
            }
            out.push_back(std::move(row));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

bool is_protected_process(const std::wstring& name_l, DWORD pid) {
    if (pid == 0 || pid == 4) return true;
    if (pid == GetCurrentProcessId()) return true;
    static const wchar_t* kCritical[] = {
        L"system",       L"smss.exe",     L"csrss.exe",   L"wininit.exe", L"services.exe",
        L"lsass.exe",    L"winlogon.exe", L"svchost.exe", L"fontdrvhost.exe", L"dwm.exe",
        L"wa-daemon.exe", L"wa-stt.exe",  L"wa-llm.exe",
    };
    for (const wchar_t* c : kCritical) {
        if (name_l == c) return true;
    }
    return false;
}

std::string list_processes(const std::string& query) {
    auto rows = snapshot_processes();
    const std::wstring q = to_lower(utf8_to_wide(query));
    std::vector<ProcRow> matched;
    for (auto& r : rows) {
        if (!q.empty() && to_lower(r.name).find(q) == std::wstring::npos) continue;
        matched.push_back(std::move(r));
    }
    std::sort(matched.begin(), matched.end(), [](const ProcRow& a, const ProcRow& b) {
        return a.working_set > b.working_set;
    });
    constexpr size_t kMax = 40;
    std::string text;
    const size_t show = std::min(matched.size(), kMax);
    for (size_t i = 0; i < show; ++i) {
        const auto& r = matched[i];
        const double mb = r.working_set / (1024.0 * 1024.0);
        char line[256];
        std::snprintf(line, sizeof(line), "%s  pid=%lu  %.1f MB\n", wide_to_utf8(r.name).c_str(),
                      static_cast<unsigned long>(r.pid), mb);
        text += line;
    }
    if (matched.size() > kMax) {
        text += "... and " + std::to_string(matched.size() - kMax) + " more\n";
    }
    if (text.empty()) {
        return reply(true, q.empty() ? "No processes found." : "No processes matching '" + query + "'.");
    }
    text = std::to_string(matched.size()) + " process(es):\n" + text;
    return reply(true, text);
}

std::string kill_process(const json& params) {
    const int pid_param = parse_int(params, "pid", -1);
    std::string name = param_str(params, "name");
    if (name.empty() && pid_param < 0) {
        // Allow a single free-form "target" from LLM improvisation.
        name = param_str(params, "target");
        if (name.empty()) name = param_str(params, "process");
    }

    std::vector<DWORD> targets;
    std::wstring killed_label;

    if (pid_param > 0) {
        targets.push_back(static_cast<DWORD>(pid_param));
        killed_label = L"pid " + std::to_wstring(pid_param);
    } else if (!name.empty()) {
        // Digits-only name → treat as PID.
        bool digits = !name.empty();
        for (char c : name) {
            if (c < '0' || c > '9') {
                digits = false;
                break;
            }
        }
        if (digits) {
            targets.push_back(static_cast<DWORD>(std::stoul(name)));
            killed_label = L"pid " + utf8_to_wide(name);
        } else {
            std::wstring want = to_lower(utf8_to_wide(name));
            if (want.size() < 4 || want.substr(want.size() - 4) != L".exe") want += L".exe";
            for (const auto& r : snapshot_processes()) {
                if (to_lower(r.name) == want) targets.push_back(r.pid);
            }
            killed_label = want;
            if (targets.empty()) {
                // Substring match as fallback.
                for (const auto& r : snapshot_processes()) {
                    if (to_lower(r.name).find(to_lower(utf8_to_wide(name))) != std::wstring::npos) {
                        targets.push_back(r.pid);
                        if (killed_label.empty()) killed_label = r.name;
                    }
                }
            }
        }
    }

    if (targets.empty()) return reply(false, "No matching process found.");

    int killed = 0;
    int skipped = 0;
    std::string errors;
    for (DWORD pid : targets) {
        std::wstring pname;
        for (const auto& r : snapshot_processes()) {
            if (r.pid == pid) {
                pname = r.name;
                break;
            }
        }
        if (is_protected_process(to_lower(pname), pid)) {
            ++skipped;
            errors += "Skipped protected " + wide_to_utf8(pname.empty() ? L"process" : pname) + " (pid " +
                      std::to_string(pid) + "). ";
            continue;
        }
        HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) {
            errors += "Cannot open pid " + std::to_string(pid) + " (access denied?). ";
            continue;
        }
        if (!TerminateProcess(h, 1)) {
            errors += "Terminate failed for pid " + std::to_string(pid) + ". ";
        } else {
            ++killed;
        }
        CloseHandle(h);
    }

    if (killed == 0) {
        return reply(false, errors.empty() ? "Could not terminate process." : errors);
    }
    std::string msg = "Terminated " + std::to_string(killed) + " process(es) matching " + wide_to_utf8(killed_label) + ".";
    if (skipped) msg += " " + std::to_string(skipped) + " protected process(es) skipped.";
    if (!errors.empty() && killed < static_cast<int>(targets.size())) msg += " " + errors;
    return reply(true, msg);
}

std::string run_program(const json& params) {
    const std::string program = param_str(params, "program");
    const std::string args = param_str(params, "args");
    if (program.empty()) return reply(false, "Missing program path or name.");
    const std::wstring wprog = utf8_to_wide(program);
    const std::wstring wargs = utf8_to_wide(args);
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", wprog.c_str(), wargs.empty() ? nullptr : wargs.c_str(), nullptr,
                                 SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        return reply(false, "Could not launch '" + program + "' (code " +
                                 std::to_string(reinterpret_cast<INT_PTR>(rc)) + ").");
    }
    return reply(true, "Launched " + program);
}

std::string open_path(const json& params) {
    const std::string path = param_str(params, "path");
    if (path.empty()) return reply(false, "Missing path or URL.");
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", utf8_to_wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        return reply(false, "Could not open '" + path + "'.");
    }
    return reply(true, "Opened " + path);
}

std::string run_shell(const json& params) {
    const std::string command = param_str(params, "command");
    if (command.empty()) return reply(false, "Missing command.");
    std::string shell = param_str(params, "shell");
    if (shell.empty()) shell = "powershell";
    for (char& c : shell) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    std::wstring cmdline;
    if (shell == "cmd") {
        cmdline = L"cmd.exe /c " + utf8_to_wide(command);
    } else {
        cmdline = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command " +
                  utf8_to_wide(command);
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return reply(false, "Failed to create pipe.");
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi);
    CloseHandle(write_pipe);
    if (!ok) {
        CloseHandle(read_pipe);
        return reply(false, "Failed to start shell.");
    }

    std::string output;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(read_pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
        output.append(buf, buf + n);
        if (output.size() > 12000) {
            output += "\n...[truncated]";
            break;
        }
    }
    CloseHandle(read_pipe);
    WaitForSingleObject(pi.hProcess, 25000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Trim trailing whitespace.
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) output.pop_back();
    std::string display = "Exit " + std::to_string(code);
    if (!output.empty()) display += ":\n" + output;
    else display += " (no output)";
    return reply(code == 0, display);
}

std::string schedule_power(bool restart, int delay_seconds) {
    if (delay_seconds < 0) delay_seconds = 30;
    delay_seconds = std::clamp(delay_seconds, 0, 600);
    std::wstring args = restart ? L"/r" : L"/s";
    args += L" /t " + std::to_wstring(delay_seconds) + L" /c \"win-assist scheduled power action\"";
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", L"shutdown.exe", args.c_str(), nullptr, SW_HIDE);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        return reply(false, restart ? "Could not schedule restart." : "Could not schedule shutdown.");
    }
    return reply(true, std::string(restart ? "Restarting" : "Shutting down") + " in " +
                           std::to_string(delay_seconds) + " seconds. Say \"cancel shutdown\" to abort.");
}

std::string cancel_shutdown() {
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/a", nullptr, SW_HIDE);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) return reply(false, "No pending shutdown, or cancel failed.");
    return reply(true, "Pending shutdown/restart cancelled.");
}

std::string log_off_user() {
    if (!ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER)) {
        return reply(false, "Could not sign out (access denied?).");
    }
    return reply(true, "Signing out…");
}

std::string hibernate() {
    enable_shutdown_privilege();
    return SetSuspendState(TRUE, FALSE, FALSE) ? reply(true, "Hibernating")
                                               : reply(false, "Hibernate is not available on this device.");
}

std::string empty_bin() {
    HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    if (FAILED(hr) && hr != S_FALSE) return reply(false, "Could not empty the Recycle Bin.");
    return reply(true, "Recycle Bin emptied.");
}

std::string show_desktop() {
    // Win+D via shell keybd event equivalent — minimize-all is more reliable.
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
            if (IsIconic(hwnd)) return TRUE;
            wchar_t title[4] = {};
            GetWindowTextW(hwnd, title, 4);
            if (!title[0]) return TRUE;
            ShowWindow(hwnd, SW_MINIMIZE);
            return TRUE;
        },
        0);
    return reply(true, "Desktop shown.");
}

std::string set_clipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return reply(false, "Could not open clipboard.");
    EmptyClipboard();
    const std::wstring w = utf8_to_wide(text);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (!mem) {
        CloseClipboard();
        return reply(false, "Out of memory.");
    }
    void* ptr = GlobalLock(mem);
    memcpy(ptr, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);
    SetClipboardData(CF_UNICODETEXT, mem);
    CloseClipboard();
    return reply(true, "Clipboard updated (" + std::to_string(text.size()) + " chars).");
}

std::string get_clipboard() {
    if (!OpenClipboard(nullptr)) return reply(false, "Could not open clipboard.");
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    std::string text;
    if (data) {
        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(data));
        if (p) {
            text = wide_to_utf8(p);
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    if (text.empty()) return reply(true, "Clipboard is empty.");
    if (text.size() > 8000) text = text.substr(0, 8000) + "\n...[truncated]";
    return reply(true, text);
}

namespace fs = std::filesystem;

std::wstring expand_path(const std::string& path) {
    std::wstring w = utf8_to_wide(path);
    if (w.empty()) return w;
    // Expand %USERPROFILE% etc. so the model can pass common env paths.
    wchar_t expanded[MAX_PATH * 4];
    const DWORD n = ExpandEnvironmentStringsW(w.c_str(), expanded, MAX_PATH * 4);
    if (n == 0 || n > MAX_PATH * 4) return w;
    return expanded;
}

bool is_dangerous_path(const fs::path& p) {
    // Refuse obvious foot-guns even when the user confirmed.
    std::wstring full = to_lower(p.wstring());
    if (full.size() <= 3) return true;  // drive roots like "C:/"
    static const wchar_t* kBlocked[] = {
        L"\\windows\\",
        L"\\windows",
        L"\\program files\\",
        L"\\program files (x86)\\",
        L"\\programdata\\",
        L"recycle.bin",
        L"\\system volume information\\",
    };
    for (const wchar_t* b : kBlocked) {
        if (full.find(b) != std::wstring::npos) return true;
    }
    return false;
}

std::string list_directory(const json& params) {
    const std::string path = param_str(params, "path");
    if (path.empty()) return reply(false, "Missing path.");
    std::error_code ec;
    fs::path dir = expand_path(path);
    if (!fs::exists(dir, ec)) return reply(false, "Path does not exist: " + path);
    if (!fs::is_directory(dir, ec)) return reply(false, "Not a directory: " + path);

    struct Entry {
        std::string name;
        bool is_dir = false;
        uintmax_t size = 0;
    };
    std::vector<Entry> entries;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        Entry row;
        row.name = wide_to_utf8(e.path().filename().wstring());
        row.is_dir = e.is_directory(ec);
        if (!row.is_dir) row.size = e.file_size(ec);
        entries.push_back(std::move(row));
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.name < b.name;
    });

    constexpr size_t kMax = 80;
    std::string text = wide_to_utf8(dir.wstring()) + " (" + std::to_string(entries.size()) + " items):\n";
    const size_t show = std::min(entries.size(), kMax);
    for (size_t i = 0; i < show; ++i) {
        const auto& e = entries[i];
        if (e.is_dir) {
            text += "  [dir]  " + e.name + "\n";
        } else {
            char line[128];
            std::snprintf(line, sizeof(line), "  [file] %s  (%.1f KB)\n", e.name.c_str(), e.size / 1024.0);
            text += line;
        }
    }
    if (entries.size() > kMax) text += "... and " + std::to_string(entries.size() - kMax) + " more\n";
    return reply(true, text);
}

std::string read_file(const json& params) {
    const std::string path = param_str(params, "path");
    if (path.empty()) return reply(false, "Missing path.");
    std::error_code ec;
    fs::path file = expand_path(path);
    if (!fs::exists(file, ec) || !fs::is_regular_file(file, ec)) {
        return reply(false, "File not found: " + path);
    }
    const auto size = fs::file_size(file, ec);
    if (ec) return reply(false, "Cannot read size of " + path);
    if (size > 256 * 1024) {
        return reply(false, "File is too large to read (" + std::to_string(size / 1024) +
                                " KB). Max is 256 KB.");
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return reply(false, "Could not open " + path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Reject obvious binary (NUL bytes).
    if (content.find('\0') != std::string::npos) {
        return reply(false, "File looks binary; only text files are supported.");
    }
    constexpr size_t kCap = 12000;
    std::string display = "File " + wide_to_utf8(file.wstring()) + " (" + std::to_string(content.size()) + " bytes):\n";
    if (content.size() > kCap) {
        display += content.substr(0, kCap) + "\n...[truncated]";
    } else {
        display += content;
    }
    return reply(true, display);
}

std::string write_file(const json& params) {
    const std::string path = param_str(params, "path");
    const std::string content = param_str(params, "content");
    if (path.empty()) return reply(false, "Missing path.");
    fs::path file = expand_path(path);
    if (is_dangerous_path(file)) {
        return reply(false, "Refusing to write under a protected system path.");
    }
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return reply(false, "Could not write " + path);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) return reply(false, "Write failed for " + path);
    return reply(true, "Wrote " + std::to_string(content.size()) + " bytes to " + wide_to_utf8(file.wstring()));
}

std::string delete_path(const json& params) {
    const std::string path = param_str(params, "path");
    if (path.empty()) return reply(false, "Missing path.");
    fs::path target = expand_path(path);
    if (is_dangerous_path(target)) {
        return reply(false, "Refusing to delete a protected system path.");
    }
    std::error_code ec;
    if (!fs::exists(target, ec)) return reply(false, "Path does not exist: " + path);
    const bool recursive = params.value("recursive", false);
    const auto removed = recursive ? fs::remove_all(target, ec) : (fs::remove(target, ec) ? 1 : 0);
    if (ec) return reply(false, "Delete failed: " + ec.message());
    if (removed == 0) return reply(false, "Nothing deleted (is the directory non-empty? set recursive=true).");
    return reply(true, "Deleted " + std::to_string(removed) + " item(s) at " + wide_to_utf8(target.wstring()));
}

std::string copy_path(const json& params) {
    const std::string from = param_str(params, "from");
    const std::string to = param_str(params, "to");
    if (from.empty() || to.empty()) return reply(false, "Need both 'from' and 'to' paths.");
    fs::path src = expand_path(from);
    fs::path dst = expand_path(to);
    if (is_dangerous_path(dst)) return reply(false, "Refusing to write under a protected system path.");
    std::error_code ec;
    if (!fs::exists(src, ec)) return reply(false, "Source does not exist: " + from);
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path(), ec);
    fs::copy(src, dst, fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
    if (ec) return reply(false, "Copy failed: " + ec.message());
    return reply(true, "Copied to " + wide_to_utf8(dst.wstring()));
}

std::string move_path(const json& params) {
    const std::string from = param_str(params, "from");
    const std::string to = param_str(params, "to");
    if (from.empty() || to.empty()) return reply(false, "Need both 'from' and 'to' paths.");
    fs::path src = expand_path(from);
    fs::path dst = expand_path(to);
    if (is_dangerous_path(src) || is_dangerous_path(dst)) {
        return reply(false, "Refusing to move a protected system path.");
    }
    std::error_code ec;
    if (!fs::exists(src, ec)) return reply(false, "Source does not exist: " + from);
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path(), ec);
    fs::rename(src, dst, ec);
    if (ec) {
        // Cross-volume rename falls back to copy+delete.
        fs::copy(src, dst, fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
        if (ec) return reply(false, "Move failed: " + ec.message());
        fs::remove_all(src, ec);
    }
    return reply(true, "Moved to " + wide_to_utf8(dst.wstring()));
}

std::string get_system_info() {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);

    OSVERSIONINFOEXW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    // RtlGetVersion avoids the GetVersionEx lie; fall back if unavailable.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&os);
    }

    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD computer_n = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(computer, &computer_n);

    wchar_t user[256] = {};
    DWORD user_n = 256;
    GetUserNameW(user, &user_n);

    SYSTEM_POWER_STATUS power{};
    GetSystemPowerStatus(&power);

    char line[512];
    std::string text;
    text += "Computer: " + wide_to_utf8(computer) + "\n";
    text += "User: " + wide_to_utf8(user) + "\n";
    std::snprintf(line, sizeof(line), "Windows %lu.%lu (build %lu)\n", os.dwMajorVersion, os.dwMinorVersion,
                  os.dwBuildNumber);
    text += line;
    std::snprintf(line, sizeof(line), "CPU logical processors: %u\n", si.dwNumberOfProcessors);
    text += line;
    std::snprintf(line, sizeof(line), "RAM: %.1f GB total, %.1f GB free (%lu%% in use)\n",
                  mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0), mem.ullAvailPhys / (1024.0 * 1024.0 * 1024.0),
                  mem.dwMemoryLoad);
    text += line;
    if (power.BatteryLifePercent <= 100) {
        std::snprintf(line, sizeof(line), "Battery: %u%% (%s)\n", power.BatteryLifePercent,
                      power.ACLineStatus == 1 ? "on AC" : "on battery");
        text += line;
    }
    return reply(true, text);
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
        enable_shutdown_privilege();
        return SetSuspendState(FALSE, FALSE, FALSE) ? reply(true, "Going to sleep")
                                                    : reply(false, "Sleep is not available on this device.");
    }
    if (intent == "list_processes") return list_processes(param_str(params, "query"));
    if (intent == "kill_process") return kill_process(params);
    if (intent == "run_program") return run_program(params);
    if (intent == "open_path") return open_path(params);
    if (intent == "run_shell") return run_shell(params);
    if (intent == "shutdown_computer") return schedule_power(false, parse_int(params, "delay_seconds", 30));
    if (intent == "restart_computer") return schedule_power(true, parse_int(params, "delay_seconds", 30));
    if (intent == "hibernate_computer") return hibernate();
    if (intent == "log_off") return log_off_user();
    if (intent == "cancel_shutdown") return cancel_shutdown();
    if (intent == "empty_recycle_bin") return empty_bin();
    if (intent == "show_desktop") return show_desktop();
    if (intent == "set_clipboard") return set_clipboard(param_str(params, "text"));
    if (intent == "get_clipboard") return get_clipboard();
    if (intent == "list_directory") return list_directory(params);
    if (intent == "read_file") return read_file(params);
    if (intent == "write_file") return write_file(params);
    if (intent == "delete_path") return delete_path(params);
    if (intent == "copy_path") return copy_path(params);
    if (intent == "move_path") return move_path(params);
    if (intent == "get_system_info") return get_system_info();
    return reply(false, "Unknown intent '" + intent + "'.");
}

const wa_plugin_info kInfo = {WA_PLUGIN_API_VERSION, "system_controls", "1.2.0"};

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
