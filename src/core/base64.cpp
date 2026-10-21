#include "core/base64.h"

#include <array>

namespace wa {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<uint8_t, 256> kDecode = [] {
    std::array<uint8_t, 256> table{};
    table.fill(64);
    for (uint8_t i = 0; i < 64; ++i) table[static_cast<uint8_t>(kAlphabet[i])] = i;
    return table;
}();

}  // namespace

std::string base64_encode(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i < size) {
        uint32_t v = bytes[i] << 16;
        if (i + 1 < size) v |= bytes[i + 1] << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < size ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::optional<std::vector<uint8_t>> base64_decode(std::string_view text) {
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    uint32_t accum = 0;
    int bits = 0;
    size_t significant = 0;
    bool padding = false;
    for (char ch : text) {
        if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
        if (ch == '=') {
            padding = true;
            continue;
        }
        if (padding) return std::nullopt;
        uint8_t v = kDecode[static_cast<uint8_t>(ch)];
        if (v == 64) return std::nullopt;
        ++significant;
        accum = (accum << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }
    if (significant % 4 == 1) return std::nullopt;
    return out;
}

}  // namespace wa
