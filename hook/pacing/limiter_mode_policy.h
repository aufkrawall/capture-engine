#pragma once

#include <cstdint>

// LimiterMode values matching the enum in config.h (duplicated here to avoid
// config.h dependency in the hook DLL which has no STL string support at load).
namespace LimiterModeValues {
constexpr uint32_t kBasic = 0;
// 1 was fg_fallback: it ran exactly the basic timer path (the frame-generation
// adjustment is common to every mode) and is parsed as basic now. The value
// stays unused so the shared-memory encoding of the other modes is unchanged.
constexpr uint32_t kNative = 2;  // NVIDIA Reflex
constexpr uint32_t kAuto = 3;
}  // namespace LimiterModeValues

// Which limiter mode governs one Apply(). Pure functions of the configuration
// and the runtime evidence, so the whole auto matrix is pinned exactly by
// tests/test_limiter_mode_policy.cpp instead of through the live limiter.
namespace ce::limiter_mode_policy {

// The configured mode of the constraint that won target selection. The
// display's vertical-blank ceiling is a bound, not a configured limiter, so it
// carries no mode of its own and resolves like auto.
inline uint32_t ResolveConfiguredLimiterMode(bool usingCaptureSync, bool usingDisplayVblankCeiling,
                                             uint32_t captureSyncMode, uint32_t generalMode) {
    if (usingCaptureSync) {
        return captureSyncMode;
    }
    return usingDisplayVblankCeiling ? LimiterModeValues::kAuto : generalMode;
}

// Auto: the game's own low-latency pacing (Reflex Sleep through NvAPI, or the
// Vulkan backend's equivalent) when the game has activated it, basic
// otherwise - with or without frame generation, because basic already paces
// in the final-output domain for every FG runtime. API availability alone is
// not enough: only a game-activated path proves the driver owns the cadence,
// and auto never starts a CE-owned Reflex Sleep in a game that has none.
inline uint32_t ResolveAutoLimiterMode(bool nativeGamePacingActive) {
    return nativeGamePacingActive ? LimiterModeValues::kNative : LimiterModeValues::kBasic;
}

inline uint32_t ResolveEffectiveLimiterMode(uint32_t configuredMode, bool nativeGamePacingActive) {
    if (configuredMode != LimiterModeValues::kAuto) {
        return configuredMode;
    }
    return ResolveAutoLimiterMode(nativeGamePacingActive);
}

// A native selection (explicit or auto) needs an API to drive. Without one
// the timer path takes over; explicit native mode accepts mere availability.
inline uint32_t ResolveUnavailableNativeLimiterMode(uint32_t effectiveMode, bool nativeApiAvailable) {
    if (effectiveMode != LimiterModeValues::kNative || nativeApiAvailable) {
        return effectiveMode;
    }
    return LimiterModeValues::kBasic;
}

inline const char* LimiterModeName(uint32_t mode) {
    switch (mode) {
        case LimiterModeValues::kNative:
            return "reflex";
        case LimiterModeValues::kAuto:
            return "auto";
        default:
            return "basic";
    }
}

inline const char* AutoLimiterModeReason(uint32_t mode) {
    switch (mode) {
        case LimiterModeValues::kNative:
            return "reflex available + game activated";
        default:
            return "no native API active";
    }
}

}  // namespace ce::limiter_mode_policy
