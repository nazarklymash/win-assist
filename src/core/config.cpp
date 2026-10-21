#include "core/config.h"

#include <fstream>
#include <sstream>

#include "common/paths.h"
#include "common/strings.h"

namespace wa {

Config parse_config(std::string_view json_text, std::string* error) {
    try {
        auto j = nlohmann::json::parse(json_text, nullptr, true, true);
        return j.get<Config>();
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return Config{};
    }
}

Config load_or_create_config(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary);
        out << nlohmann::json(Config{}).dump(2) << '\n';
        return Config{};
    }
    std::ifstream in(file, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return parse_config(buffer.str(), error);
}

std::filesystem::path resolve_app_path(const std::string& path) {
    std::filesystem::path p(to_wide(path));
    if (p.is_absolute()) return p;
    return paths::exe_dir() / p;
}

}  // namespace wa
