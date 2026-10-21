#pragma once

// Shared bootstrap for worker processes: argument parsing and connecting to
// the daemon's named pipe.

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <map>
#include <string>

#include "common/strings.h"

namespace wa::worker {

inline std::map<std::string, std::string> parse_args() {
    std::map<std::string, std::string> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string key = to_utf8(argv[i]);
        if (key.rfind("--", 0) == 0) args[key.substr(2)] = to_utf8(argv[i + 1]);
    }
    LocalFree(argv);
    return args;
}

inline HANDLE connect_pipe(const std::string& name) {
    std::wstring wname = to_wide(name);
    for (int attempt = 0; attempt < 50; ++attempt) {
        HANDLE h = CreateFileW(wname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(wname.c_str(), 200);
        else Sleep(100);
    }
    return INVALID_HANDLE_VALUE;
}

inline int arg_int(const std::map<std::string, std::string>& args, const char* key, int fallback) {
    auto it = args.find(key);
    if (it == args.end()) return fallback;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return fallback;
    }
}

inline std::string arg_str(const std::map<std::string, std::string>& args, const char* key) {
    auto it = args.find(key);
    return it == args.end() ? std::string() : it->second;
}

inline int default_threads() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = static_cast<int>(si.dwNumberOfProcessors);
    return n > 8 ? 8 : (n > 1 ? n - 1 : 1);
}

}  // namespace wa::worker
