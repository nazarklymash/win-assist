#pragma once

#include <string>
#include <string_view>

namespace wa {

std::string to_utf8(std::wstring_view text);
std::wstring to_wide(std::string_view text);

std::string trim(std::string_view text);
std::string to_lower_ascii(std::string_view text);
bool iequals_ascii(std::string_view a, std::string_view b);

}  // namespace wa
