#pragma once

// Length-prefixed framing shared by the daemon and its worker processes.
// Each frame is a little-endian uint32 byte count followed by the payload.
// Control frames are UTF-8 JSON; bulk payloads (PCM audio) are raw bytes.

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace wa::ipc {

constexpr uint32_t kMaxFrameBytes = 256u << 20;

// `cancel` may be null. When it is signalled, pending I/O is cancelled and the
// call returns false. Works on both overlapped and synchronous handles.
bool write_frame(HANDLE pipe, const void* data, uint32_t size, HANDLE cancel = nullptr);
bool read_frame(HANDLE pipe, std::string& out, HANDLE cancel = nullptr);

inline bool write_frame(HANDLE pipe, std::string_view data, HANDLE cancel = nullptr) {
    return write_frame(pipe, data.data(), static_cast<uint32_t>(data.size()), cancel);
}

}  // namespace wa::ipc
