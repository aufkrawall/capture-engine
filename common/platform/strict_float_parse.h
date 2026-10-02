#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ce {

inline bool HasStrictDecimalFloatSyntax(const std::string& value) {
    size_t index = 0;
    if (index < value.size() && (value[index] == '+' || value[index] == '-')) {
        ++index;
    }

    bool hasDigits = false;
    while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
        hasDigits = true;
        ++index;
    }
    if (index < value.size() && value[index] == '.') {
        ++index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
            hasDigits = true;
            ++index;
        }
    }
    if (!hasDigits) {
        return false;
    }

    if (index < value.size() && (value[index] == 'e' || value[index] == 'E')) {
        ++index;
        if (index < value.size() && (value[index] == '+' || value[index] == '-')) {
            ++index;
        }
        const size_t exponentStart = index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
            ++index;
        }
        if (index == exponentStart) {
            return false;
        }
    }

    return index == value.size();
}

inline bool IsFiniteFloatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

inline bool TryParseFiniteFloat(const std::string& value, float& result) {
    if (!HasStrictDecimalFloatSyntax(value)) {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    result = std::strtof(value.c_str(), &end);
    return end != value.c_str() && *end == '\0' && errno != ERANGE && IsFiniteFloatBits(result);
}

}  // namespace ce
