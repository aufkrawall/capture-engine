#pragma once

#include <cstdint>
#include <string_view>

namespace ce::remix_fg {

inline constexpr char kScheduleOption[] = "rtx.dlfg.maxInterpolatedFrames";
inline constexpr char kNgxGeneratedFrameParameter[] = "DLSSG.MultiFrameCount";
inline constexpr char kNgxGeneratedFrameParameterCompat[] = "MultiFrameCount";

inline bool IsScheduleOption(std::string_view key) noexcept {
    return key == kScheduleOption;
}

inline bool IsNgxGeneratedFrameParameter(std::string_view name) noexcept {
    return name == kNgxGeneratedFrameParameter || name == kNgxGeneratedFrameParameterCompat;
}

inline bool ShouldOverrideConfigVariable(std::string_view key, uint32_t configuredGeneratedFrames) noexcept {
    return configuredGeneratedFrames >= 1 && configuredGeneratedFrames <= 3 && IsScheduleOption(key);
}

inline bool ShouldReassertFromNgx(std::string_view parameterName, uint32_t observedGeneratedFrames,
                                 uint32_t configuredGeneratedFrames, uint32_t lastAppliedGeneratedFrames,
                                 uint32_t previousObservedGeneratedFrames) noexcept {
    return configuredGeneratedFrames >= 1 && configuredGeneratedFrames <= 3 &&
           IsNgxGeneratedFrameParameter(parameterName) &&
           (lastAppliedGeneratedFrames != configuredGeneratedFrames ||
            (observedGeneratedFrames != configuredGeneratedFrames &&
             previousObservedGeneratedFrames != observedGeneratedFrames));
}

inline const char* GeneratedFrameCountString(uint32_t generatedFrames) noexcept {
    switch (generatedFrames) {
    case 1:
        return "1";
    case 2:
        return "2";
    case 3:
        return "3";
    default:
        return nullptr;
    }
}

}  // namespace ce::remix_fg
