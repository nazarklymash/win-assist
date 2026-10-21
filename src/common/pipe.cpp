#include "common/pipe.h"

namespace wa::ipc {
namespace {

bool transfer(HANDLE pipe, bool writing, void* buffer, DWORD size, HANDLE cancel) {
    auto* cursor = static_cast<char*>(buffer);
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    bool ok = true;
    while (size > 0) {
        OVERLAPPED ov{};
        ov.hEvent = event;
        ResetEvent(event);
        DWORD done = 0;
        BOOL started = writing ? WriteFile(pipe, cursor, size, &done, &ov) : ReadFile(pipe, cursor, size, &done, &ov);
        if (!started) {
            if (GetLastError() != ERROR_IO_PENDING) {
                ok = false;
                break;
            }
            HANDLE waits[2] = {event, cancel};
            DWORD wait = WaitForMultipleObjects(cancel ? 2 : 1, waits, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &done, TRUE);
                ok = false;
                break;
            }
            if (!GetOverlappedResult(pipe, &ov, &done, FALSE)) {
                ok = false;
                break;
            }
        }
        if (done == 0 && !writing) {
            ok = false;
            break;
        }
        cursor += done;
        size -= done;
    }
    CloseHandle(event);
    return ok;
}

}  // namespace

bool write_frame(HANDLE pipe, const void* data, uint32_t size, HANDLE cancel) {
    if (size > kMaxFrameBytes) return false;
    uint32_t header = size;
    if (!transfer(pipe, true, &header, sizeof(header), cancel)) return false;
    if (size == 0) return true;
    return transfer(pipe, true, const_cast<void*>(data), size, cancel);
}

bool read_frame(HANDLE pipe, std::string& out, HANDLE cancel) {
    uint32_t size = 0;
    if (!transfer(pipe, false, &size, sizeof(size), cancel)) return false;
    if (size > kMaxFrameBytes) return false;
    out.resize(size);
    if (size == 0) return true;
    return transfer(pipe, false, out.data(), size, cancel);
}

}  // namespace wa::ipc
