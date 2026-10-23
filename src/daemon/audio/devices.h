#pragma once

#include <string>
#include <vector>

namespace wa {

struct AudioDevice {
    std::string id;    // WASAPI endpoint ID (UTF-8)
    std::string name;  // Friendly name
    bool is_default = false;
};

// Capture (microphone) endpoints. Empty id in config means the system default.
std::vector<AudioDevice> list_capture_devices();

}  // namespace wa
