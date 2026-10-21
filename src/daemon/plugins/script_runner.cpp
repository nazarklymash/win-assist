#include "daemon/plugins/script_runner.h"

#include <windows.h>

#include <thread>
#include <vector>

#include "common/log.h"
#include "common/strings.h"
#include "daemon/ipc/worker.h"

namespace wa {
namespace {

std::wstring command_for(const PluginManifest& m) {
    const std::wstring entry = L"\"" + (m.dir / to_wide(m.entry)).wstring() + L"\"";
    if (m.runtime == "powershell")
        return L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + entry;
    if (m.runtime == "pwsh") return L"pwsh.exe -NoLogo -NoProfile -NonInteractive -File " + entry;
    if (m.runtime == "python") return L"python.exe " + entry;
    if (m.runtime == "node") return L"node.exe " + entry;
    if (m.runtime == "cmd") return L"cmd.exe /d /c " + entry;
    return entry;
}

}  // namespace

ActionResult run_script_plugin(const PluginManifest& m, const std::string& request_json,
                               const std::atomic<bool>& cancel) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE stdin_read = nullptr, stdin_write = nullptr, stdout_read = nullptr, stdout_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0) || !CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        return ActionResult::error("Could not create plugin pipes.");
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    // Pass exactly the two pipe ends, so concurrently spawned children do not
    // inherit each other's handles.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buffer(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    HANDLE inherit[2] = {stdin_read, stdout_write};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = stdin_read;
    si.StartupInfo.hStdOutput = stdout_write;
    si.StartupInfo.hStdError = stdout_write;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = command_for(m);
    const std::wstring dir = m.dir.wstring();
    BOOL created = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                  dir.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    if (!created) {
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        log::error("plugin {}: failed to start '{}' ({})", m.id, to_utf8(cmdline), GetLastError());
        return ActionResult::error("Plugin '" + m.name + "' failed to start (is its runtime installed?).");
    }
    AssignProcessToJobObject(daemon_job(), pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::string line = request_json + "\n";
    DWORD written = 0;
    WriteFile(stdin_write, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(stdin_write);

    std::string output;
    std::thread reader([&] {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(stdout_read, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            output.append(buffer, read);
            if (output.size() > (1u << 20)) break;
        }
    });

    bool timed_out = false;
    bool cancelled = false;
    const DWORD timeout = static_cast<DWORD>(m.timeout_ms > 0 ? m.timeout_ms : 10000);
    for (DWORD waited = 0;; waited += 50) {
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
        if (cancel.load()) {
            cancelled = true;
        } else if (waited >= timeout) {
            timed_out = true;
        } else {
            continue;
        }
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        break;
    }
    reader.join();
    CloseHandle(stdout_read);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);

    if (cancelled) return ActionResult::error("Cancelled.");
    if (timed_out) return ActionResult::error("Plugin '" + m.name + "' timed out.");
    ActionResult result = ActionResult::from_output(output);
    if (exit_code != 0 && result.ok && result.display.empty()) {
        return ActionResult::error("Plugin '" + m.name + "' exited with code " + std::to_string(exit_code) + ".");
    }
    return result;
}

}  // namespace wa
