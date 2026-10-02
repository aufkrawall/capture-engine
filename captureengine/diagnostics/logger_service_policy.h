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

// How long the logger may sleep before draining the hook's log ring again.
//
// The ring holds a fixed number of slots and the producer does not block: when
// it is full the hook falls back to writing the file itself, and that fallback
// gives up whenever its lock is contended, because a game thread must never
// stall on a log write. So a saturated ring is not a throughput detail, it is
// the point at which log lines start disappearing.
//
// Session `20260921_183446` overflowed the ring 1091 times in two UE5 titles
// while a steady-output title never reached it at all. Sleeping the normal
// interval right after emptying a full ring guarantees the following burst
// overflows too, so a saturated drain is followed by an immediate re-drain.
inline unsigned long SelectLogDrainWaitMs(bool sawSaturatedRing, bool hasPendingLogs, bool hasActiveSource) {
    if (sawSaturatedRing)
        return 0;
    if (hasPendingLogs)
        return 100;
    return hasActiveSource ? 250 : 1000;
}

}  // namespace logger_service_policy
