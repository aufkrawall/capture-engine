#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace ce {

inline bool TryParseInt32(std::string_view value, int32_t& result, int base = 10) {
    if (value.empty() || base < 2 || base > 36)
        return false;
    int32_t parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size())
        return false;
    result = parsed;
    return true;
}

inline bool TryParseUInt32(std::string_view value, uint32_t& result, int base = 10) {
    if (value.empty() || base < 2 || base > 36)
        return false;
    uint32_t parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size())
        return false;
    result = parsed;
    return true;
}

}  // namespace ce
