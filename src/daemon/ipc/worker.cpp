#include "daemon/ipc/worker.h"

#include <atomic>
#include <format>

#include "common/log.h"
#include "common/paths.h"
#include "common/pipe.h"
#include "common/strings.h"

namespace wa {
namespace {

std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

}  // namespace

HANDLE daemon_job() {
    static HANDLE job = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
        return h;
    }();
    return job;
}

WorkerProcess::WorkerProcess(std::wstring exe_name, std::string tag)
    : exe_name_(std::move(exe_name)), tag_(std::move(tag)) {}

WorkerProcess::~WorkerProcess() {
    std::lock_guard guard(mutex_);
    stop();
}

std::filesystem::path WorkerProcess::exe_path() const { return paths::exe_dir() / exe_name_; }

bool WorkerProcess::running() const {
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

void WorkerProcess::stop() {
    if (pipe_) {
        ipc::write_frame(pipe_, std::string_view(R"({"op":"shutdown"})"));
        CloseHandle(pipe_);
        pipe_ = nullptr;
    }
    if (process_) {
        if (WaitForSingleObject(process_, 2000) == WAIT_TIMEOUT) TerminateProcess(process_, 1);
        CloseHandle(process_);
        process_ = nullptr;
        log::info("{} worker stopped", tag_);
    }
    args_.clear();
}

bool WorkerProcess::stop_if_idle(std::chrono::seconds idle) {
    std::unique_lock guard(mutex_, std::try_to_lock);
    if (!guard.owns_lock()) return true;
    if (!process_) return false;
    if (!running()) {
        stop();
        return false;
    }
    if (std::chrono::steady_clock::now() - last_used_ < idle) return true;
    stop();
    return false;
}

bool WorkerProcess::ensure_started(const std::vector<std::wstring>& args, std::string& ready_json, std::string& error,
                                   HANDLE cancel) {
    last_used_ = std::chrono::steady_clock::now();
    if (running() && pipe_ && args == args_) return true;
    stop();

    const auto exe = exe_path();
    if (!std::filesystem::exists(exe)) {
        error = to_utf8(exe_name_) + " not found next to wa-daemon.exe (built with WA_BUILD_WORKERS?)";
        return false;
    }

    static std::atomic<unsigned> counter{0};
    const std::wstring pipe_name = std::format(L"\\\\.\\pipe\\win-assist-{}-{}-{}", GetCurrentProcessId(),
                                               to_wide(tag_), counter.fetch_add(1));
    pipe_ = CreateNamedPipeW(pipe_name.c_str(),
                             PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16,
                             1 << 16, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        pipe_ = nullptr;
        error = "CreateNamedPipe failed";
        return false;
    }

    std::wstring cmdline = quote_arg(exe.wstring()) + L" --pipe " + quote_arg(pipe_name);
    for (const auto& a : args) cmdline += L" " + quote_arg(a);

    auto log_file = paths::local_data() / (L"logs\\" + to_wide(tag_) + L".log");
    std::error_code ec;
    std::filesystem::create_directories(log_file.parent_path(), ec);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log_handle = CreateFileW(log_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    const bool have_log = log_handle != INVALID_HANDLE_VALUE;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buffer(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    if (have_log) {
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log_handle, sizeof(log_handle), nullptr,
                                  nullptr);
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = have_log ? log_handle : nullptr;
    si.StartupInfo.hStdError = have_log ? log_handle : nullptr;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, have_log,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                  exe.parent_path().c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (have_log) CloseHandle(log_handle);
    if (!created) {
        error = "failed to start " + to_utf8(exe_name_) + " (" + std::to_string(GetLastError()) + ")";
        CloseHandle(pipe_);
        pipe_ = nullptr;
        return false;
    }
    AssignProcessToJobObject(daemon_job(), pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = ConnectNamedPipe(pipe_, &ov) != FALSE;
    if (!connected) {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (err == ERROR_IO_PENDING) {
            HANDLE waits[3] = {ov.hEvent, process_, cancel};
            DWORD n = cancel ? 3 : 2;
            DWORD wait = WaitForMultipleObjects(n, waits, FALSE, 15000);
            if (wait == WAIT_OBJECT_0) {
                DWORD dummy = 0;
                connected = GetOverlappedResult(pipe_, &ov, &dummy, FALSE) != FALSE;
            } else {
                CancelIoEx(pipe_, &ov);
                DWORD dummy = 0;
                GetOverlappedResult(pipe_, &ov, &dummy, TRUE);
            }
        }
    }
    CloseHandle(ov.hEvent);
    if (!connected) {
        error = to_utf8(exe_name_) + " did not connect";
        stop();
        return false;
    }

    if (!ipc::read_frame(pipe_, ready_json, cancel)) {
        error = to_utf8(exe_name_) + " exited during startup (see " + to_utf8(log_file.wstring()) + ")";
        stop();
        return false;
    }
    args_ = args;
    last_used_ = std::chrono::steady_clock::now();
    log::info("{} worker started (pid {})", tag_, GetProcessId(process_));
    return true;
}

bool WorkerProcess::send(const void* data, uint32_t size, HANDLE cancel) {
    last_used_ = std::chrono::steady_clock::now();
    return pipe_ && ipc::write_frame(pipe_, data, size, cancel);
}

bool WorkerProcess::receive(std::string& frame, HANDLE cancel) {
    last_used_ = std::chrono::steady_clock::now();
    return pipe_ && ipc::read_frame(pipe_, frame, cancel);
}

}  // namespace wa
