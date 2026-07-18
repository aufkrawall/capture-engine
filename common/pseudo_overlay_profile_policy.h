#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"

namespace ce::pseudo_overlay {

inline std::string NormalizeProfileProcessName(std::string_view value) {
    constexpr std::string_view kTrimChars = " \t\r\n\"";
    const size_t first = value.find_first_not_of(kTrimChars);
    if (first == std::string_view::npos)
        return {};
    const size_t last = value.find_last_not_of(kTrimChars);

    std::string normalized(value.substr(first, last - first + 1));
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized;
}

inline const PseudoOverlayApplicationConfig* FindApplicationConfig(
    const std::vector<PseudoOverlayApplicationConfig>& profiles, std::string_view processName) {
    const std::string normalizedProcess = NormalizeProfileProcessName(processName);
    if (normalizedProcess.empty())
        return nullptr;

    for (const PseudoOverlayApplicationConfig& profile : profiles) {
        if (NormalizeProfileProcessName(profile.processName) == normalizedProcess)
            return &profile;
    }
    return nullptr;
}

inline bool ProcessListContains(std::string_view processList, std::string_view processName) {
    const std::string normalizedProcess = NormalizeProfileProcessName(processName);
    if (normalizedProcess.empty())
        return false;

    size_t begin = 0;
    while (begin <= processList.size()) {
        const size_t end = processList.find('|', begin);
        const std::string_view item =
            end == std::string_view::npos ? processList.substr(begin) : processList.substr(begin, end - begin);
        if (NormalizeProfileProcessName(item) == normalizedProcess)
            return true;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return false;
}

inline bool IsForegroundWarningTarget(const PseudoOverlayApplicationConfig* profile,
                                      std::string_view globalProcessList, std::string_view processName) {
    return (profile && profile->warningTarget) || ProcessListContains(globalProcessList, processName);
}

}  // namespace ce::pseudo_overlay
