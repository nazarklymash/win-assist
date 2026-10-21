#include "common/log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace wa::log {
namespace {

constexpr std::uintmax_t kMaxLogBytes = 1u << 20;

std::mutex g_mutex;
FILE* g_file = nullptr;
std::filesystem::path g_path;

}  // namespace

void init(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_path = file;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > kMaxLogBytes) {
        auto old = file;
        old += L".old";
        std::filesystem::rename(file, old, ec);
    }
    g_file = _wfsopen(file.c_str(), L"ab", _SH_DENYWR);
}

std::filesystem::path file_path() {
    std::lock_guard lock(g_mutex);
    return g_path;
}

void write(std::string_view level, std::string_view message) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string line = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] {}\n", st.wYear, st.wMonth, st.wDay,
                                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level, message);
    OutputDebugStringA(line.c_str());
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fwrite(line.data(), 1, line.size(), g_file);
        fflush(g_file);
    }
}

}  // namespace wa::log
