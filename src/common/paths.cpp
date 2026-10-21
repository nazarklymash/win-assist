#include "common/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <string>

namespace wa::paths {
namespace {

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) {
        result = std::filesystem::path(raw) / L"win-assist";
    }
    CoTaskMemFree(raw);
    std::error_code ec;
    std::filesystem::create_directories(result, ec);
    return result;
}

}  // namespace

std::filesystem::path exe_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len < buffer.size()) {
            buffer.resize(len);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path exe_dir() { return exe_path().parent_path(); }

std::filesystem::path app_data() {
    static const std::filesystem::path path = known_folder(FOLDERID_RoamingAppData);
    return path;
}

std::filesystem::path local_data() {
    static const std::filesystem::path path = known_folder(FOLDERID_LocalAppData);
    return path;
}

}  // namespace wa::paths
