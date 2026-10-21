#include "daemon/ai/http.h"

#include <windows.h>
#include <winhttp.h>

#include "common/strings.h"

namespace wa {
namespace {

struct HandleCloser {
    HINTERNET h = nullptr;
    ~HandleCloser() {
        if (h) WinHttpCloseHandle(h);
    }
};

}  // namespace

HttpClient::HttpClient() {
    session_ = WinHttpOpen(L"win-assist/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
}

HttpClient::~HttpClient() {
    if (session_) WinHttpCloseHandle(session_);
}

void HttpClient::abort() {
    std::lock_guard lock(mutex_);
    if (active_request_) {
        WinHttpCloseHandle(active_request_);
        active_request_ = nullptr;
    }
}

HttpResponse HttpClient::request(std::string_view method, std::string_view url, std::string_view body, int timeout_ms,
                                 const std::function<bool(std::string_view)>& on_chunk,
                                 const std::atomic<bool>* cancel) {
    HttpResponse response;
    if (!session_) {
        response.error = "WinHTTP unavailable";
        return response;
    }

    std::wstring wurl = to_wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        response.error = "invalid URL: " + std::string(url);
        return response;
    }

    HandleCloser connection{WinHttpConnect(session_, host, parts.nPort, 0)};
    if (!connection.h) {
        response.error = "connect failed";
        return response;
    }
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    std::wstring wmethod = to_wide(method);
    HINTERNET req = WinHttpOpenRequest(connection.h, wmethod.c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        response.error = "open request failed";
        return response;
    }
    {
        std::lock_guard lock(mutex_);
        active_request_ = req;
    }
    auto release = [&] {
        std::lock_guard lock(mutex_);
        if (active_request_ == req) {
            WinHttpCloseHandle(req);
            active_request_ = nullptr;
        }
    };

    int connect_timeout = timeout_ms < 5000 ? timeout_ms : 5000;
    WinHttpSetTimeouts(req, connect_timeout, connect_timeout, timeout_ms, timeout_ms);

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    BOOL sent = WinHttpSendRequest(req, body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers,
                                   body.empty() ? 0 : static_cast<DWORD>(-1L),
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
        DWORD err = GetLastError();
        response.error = err == ERROR_WINHTTP_CANNOT_CONNECT ? "connection refused"
                         : err == ERROR_WINHTTP_TIMEOUT      ? "timed out"
                                                             : "request failed (" + std::to_string(err) + ")";
        release();
        return response;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);

    const bool streaming = static_cast<bool>(on_chunk) && response.status >= 200 && response.status < 300;
    std::string buffer;
    for (;;) {
        if (cancel && cancel->load()) {
            response.error = "cancelled";
            break;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available)) {
            if (!(cancel && cancel->load())) response.error = "read failed";
            else response.error = "cancelled";
            break;
        }
        if (available == 0) break;
        buffer.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(req, buffer.data(), available, &read)) {
            response.error = "read failed";
            break;
        }
        if (read == 0) break;
        std::string_view chunk(buffer.data(), read);
        if (streaming) {
            if (!on_chunk(chunk)) break;
        } else {
            response.body.append(chunk);
        }
    }
    release();
    return response;
}

}  // namespace wa
