#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ce::remix_fg {

inline constexpr char kScheduleOption[] = "rtx.dlfg.maxInterpolatedFrames";
// The runtime's own choice between hardware present metering and the CPU
// pacer it uses instead: "Use hardware present metering for DLSS 4.0 frame
// generation instead of CPU pacing".
inline constexpr char kPresentMeteringOption[] = "rtx.dlfg.enablePresentMetering";
inline constexpr char kPresentMeteringDisabledValue[] = "False";
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

inline bool IsPresentMeteringOption(std::string_view key) noexcept {
    return key == kPresentMeteringOption;
}

// Two authorities cannot pace the same presents. When the profile asks for
// vertical-blank-paced presentation the Vulkan layer withholds
// VK_NV_present_metering from the renderer entirely (see
// hook/vulkan_layer/vulkan_present_metering_policy.h), and this is the other
// half of that same decision: a runtime whose option still says "use hardware
// metering" keeps asking for pacing it can no longer get and never engages the
// CPU pacer it would otherwise use, which leaves a generated group arriving in
// one burst with nothing spreading it. Only fifo and adaptive ask for the rate
// contract; every other vsync_mode leaves the runtime's own choice alone.
inline bool RequestsVblankPacedPresentation(std::string_view vsyncMode) noexcept {
    return vsyncMode == "fifo" || vsyncMode == "adaptive";
}

inline bool ShouldForcePresentMeteringOff(std::string_view key, std::string_view vsyncMode) noexcept {
    return RequestsVblankPacedPresentation(vsyncMode) && IsPresentMeteringOption(key);
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
