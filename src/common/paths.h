#pragma once

#include <filesystem>

namespace wa::paths {

std::filesystem::path exe_path();
std::filesystem::path exe_dir();
// %APPDATA%\win-assist (config, user plugins). Created on first call.
std::filesystem::path app_data();
// %LOCALAPPDATA%\win-assist (logs, caches). Created on first call.
std::filesystem::path local_data();

}  // namespace wa::paths
