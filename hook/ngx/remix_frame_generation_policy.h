#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ce::remix_fg {

inline constexpr char kScheduleOption[] = "rtx.dlfg.maxInterpolatedFrames";
// NGX parameter spellings observed across the supported Remix interfaces.
inline constexpr char kNgxGeneratedFrameParameter[] = "DLSSG.MultiFrameCount";
inline constexpr char kNgxGeneratedFrameParameterCompat[] = "MultiFrameCount";
inline constexpr size_t kPublicInterfacePrefixFunctionCount = 13;
inline constexpr size_t kSetConfigVariableFunctionIndex = 10;
inline constexpr size_t kPublicInterfaceStorageFunctionCount = 64;
inline constexpr uint32_t kIncompatiblePublicApiVersion = 8;

inline constexpr uint64_t MakePublicApiVersion(uint16_t major, uint32_t minor,
                                               uint16_t patch) noexcept {
    return (static_cast<uint64_t>(major) << 48) |
           (static_cast<uint64_t>(minor) << 16) |
           static_cast<uint64_t>(patch);
}

// Major zero Remix API builds require an exact minor match. Try the current
// official interface first, then the 0.5.1 interface shipped by RTX Remix 1.0.
inline constexpr std::array<uint64_t, 2> kKnownPublicApiVersions = {
    MakePublicApiVersion(0, 6, 4),
    MakePublicApiVersion(0, 5, 1),
};

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
