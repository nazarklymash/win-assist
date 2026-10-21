#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace wa::log {

void init(const std::filesystem::path& file);
void write(std::string_view level, std::string_view message);
std::filesystem::path file_path();

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write("INFO", std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write("WARN", std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write("ERROR", std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace wa::log
