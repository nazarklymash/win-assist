#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace wa {

// Kill-on-close job shared by every child the daemon spawns, so workers and
// script plugins never outlive it.
HANDLE daemon_job();

// An on-demand child process (wa-stt.exe / wa-llm.exe) connected over a
// private named pipe. Callers hold lock() for the duration of a request.
class WorkerProcess {
public:
    WorkerProcess(std::wstring exe_name, std::string tag);
    ~WorkerProcess();
    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    std::unique_lock<std::mutex> lock() { return std::unique_lock(mutex_); }

    // Starts the worker unless it is already running with the same arguments.
    // On success `ready_json` holds the worker's ready frame. Requires lock().
    bool ensure_started(const std::vector<std::wstring>& args, std::string& ready_json, std::string& error,
                        HANDLE cancel);
    bool send(const void* data, uint32_t size, HANDLE cancel);
    bool send(const std::string& frame, HANDLE cancel) {
        return send(frame.data(), static_cast<uint32_t>(frame.size()), cancel);
    }
    bool receive(std::string& frame, HANDLE cancel);
    // Requires lock().
    void stop();
    bool running() const;

    // Stops the worker if it has been unused for `idle`. Skips if busy.
    // Returns true while the worker is still running.
    bool stop_if_idle(std::chrono::seconds idle);

    std::filesystem::path exe_path() const;

private:
    std::wstring exe_name_;
    std::string tag_;
    mutable std::mutex mutex_;
    HANDLE process_ = nullptr;
    HANDLE pipe_ = nullptr;
    std::vector<std::wstring> args_;
    std::chrono::steady_clock::time_point last_used_{};
};

}  // namespace wa
