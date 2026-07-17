#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace logger_service_policy {

inline std::string SelectSessionLogsDirectory(const char* discoveryPath, size_t capacity,
                                              const std::string& fallbackDirectory) {
    if (!discoveryPath || capacity == 0)
        return fallbackDirectory;
    size_t length = 0;
    while (length < capacity && discoveryPath[length] != '\0')
        ++length;
    if (length == 0 || length == capacity)
        return fallbackDirectory;
    return std::string(discoveryPath, length);
}

inline bool IsSafeLogFilename(std::string_view filename) {
    if (filename.empty() || filename.size() >= 64 || filename == "." || filename == "..")
        return false;
    for (const char ch : filename) {
        const bool alphaNumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                  (ch >= '0' && ch <= '9');
        if (!alphaNumeric && ch != '.' && ch != '-' && ch != '_')
            return false;
    }
    return true;
}

}  // namespace logger_service_policy
