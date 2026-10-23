#include "daemon/tts/piper.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>

#include "common/log.h"
#include "common/paths.h"
#include "common/strings.h"
#include "core/config.h"

namespace wa {
namespace {

std::filesystem::path piper_exe(const TtsConfig& config) {
    if (!config.piper_path.empty()) return resolve_app_path(config.piper_path);
    return paths::exe_dir() / L"tts" / L"piper.exe";
}

std::filesystem::path voice_model(const TtsConfig& config) {
    if (!config.piper_voice.empty()) return resolve_app_path(config.piper_voice);
    return paths::exe_dir() / L"tts" / L"en_US-lessac-medium.onnx";
}

}  // namespace

bool piper_available(const TtsConfig& config) {
    std::error_code ec;
    return std::filesystem::exists(piper_exe(config), ec) && std::filesystem::exists(voice_model(config), ec);
}

bool piper_speak(std::wstring_view text, const TtsConfig& config) {
    if (!piper_available(config) || text.empty()) return false;
    const auto exe = piper_exe(config);
    const auto model = voice_model(config);
    const auto wav = paths::local_data() / L"tts-out.wav";
    std::error_code ec;
    std::filesystem::create_directories(paths::local_data(), ec);

    // piper reads UTF-8 text from stdin: piper -m model -f out.wav
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return false;
    SetHandleInformation(write_pipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + exe.wstring() + L"\" -m \"" + model.wstring() + L"\" -f \"" + wav.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = read_pipe;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        exe.parent_path().c_str(), &si, &pi)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        log::warn("piper: CreateProcess failed ({})", GetLastError());
        return false;
    }
    CloseHandle(read_pipe);

    std::string utf8 = to_utf8(text);
    utf8.push_back('\n');
    DWORD written = 0;
    WriteFile(write_pipe, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(write_pipe);

    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (!std::filesystem::exists(wav, ec)) {
        log::warn("piper: no output wav");
        return false;
    }
    // Play with the shell (async).
    ShellExecuteW(nullptr, L"open", wav.c_str(), nullptr, nullptr, SW_HIDE);
    return true;
}

}  // namespace wa
