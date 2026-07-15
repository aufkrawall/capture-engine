#pragma once

#include <cstddef>
#include <limits>
#include <vector>
#include <windows.h>
#include <psapi.h>

namespace ce {

// PSAPI reports the number of bytes required for the complete module list,
// even when the caller-provided storage is smaller. Bound iteration to the
// entries that can actually have been written into that storage.
constexpr std::size_t GetEnumeratedModuleCount(std::size_t capacity, DWORD bytesNeeded) noexcept {
    const std::size_t reportedCount = static_cast<std::size_t>(bytesNeeded) / sizeof(HMODULE);
    return reportedCount < capacity ? reportedCount : capacity;
}

namespace detail {

template <typename Enumerate>
inline bool EnumerateProcessModulesGrowing(std::vector<HMODULE>& modules, Enumerate enumerate) {
    constexpr std::size_t kInitialCapacity = 256;
    constexpr int kMaxAttempts = 4;

    modules.resize(kInitialCapacity);
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const std::size_t byteCapacity = modules.size() * sizeof(HMODULE);
        DWORD bytesNeeded = 0;
        if (!enumerate(modules.data(), static_cast<DWORD>(byteCapacity), &bytesNeeded)) {
            modules.clear();
            return false;
        }

        if (bytesNeeded <= byteCapacity) {
            modules.resize(GetEnumeratedModuleCount(modules.size(), bytesNeeded));
            return true;
        }

        const std::size_t requiredCapacity =
            (static_cast<std::size_t>(bytesNeeded) + sizeof(HMODULE) - 1) / sizeof(HMODULE);
        const std::size_t maxDwordCapacity = (std::numeric_limits<DWORD>::max)() / sizeof(HMODULE);
        if (requiredCapacity <= modules.size() || requiredCapacity > maxDwordCapacity ||
            requiredCapacity > modules.max_size()) {
            modules.clear();
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return false;
        }
        modules.resize(requiredCapacity);
    }

    modules.clear();
    SetLastError(ERROR_RETRY);
    return false;
}

}  // namespace detail

inline bool EnumerateProcessModules(HANDLE process, std::vector<HMODULE>& modules) {
    return detail::EnumerateProcessModulesGrowing(
        modules, [process](HMODULE* buffer, DWORD bufferBytes, DWORD* bytesNeeded) {
            return ::EnumProcessModules(process, buffer, bufferBytes, bytesNeeded) != FALSE;
        });
}

inline bool EnumerateProcessModulesEx(HANDLE process, DWORD filterFlag, std::vector<HMODULE>& modules) {
    return detail::EnumerateProcessModulesGrowing(
        modules, [process, filterFlag](HMODULE* buffer, DWORD bufferBytes, DWORD* bytesNeeded) {
            return ::EnumProcessModulesEx(process, buffer, bufferBytes, bytesNeeded, filterFlag) != FALSE;
        });
}

}  // namespace ce
