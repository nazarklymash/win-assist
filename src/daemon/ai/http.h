#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace wa {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// Minimal synchronous WinHTTP client. When `on_chunk` is set the body is
// streamed to it instead of being collected; returning false stops the read.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse request(std::string_view method, std::string_view url, std::string_view body, int timeout_ms,
                         const std::function<bool(std::string_view)>& on_chunk = {},
                         const std::atomic<bool>* cancel = nullptr);

    // Aborts whatever request is in flight on another thread.
    void abort();

private:
    void* session_ = nullptr;
    std::mutex mutex_;
    void* active_request_ = nullptr;
};

}  // namespace wa
