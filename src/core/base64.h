#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wa {

std::string base64_encode(const void* data, size_t size);
std::optional<std::vector<uint8_t>> base64_decode(std::string_view text);

}  // namespace wa
