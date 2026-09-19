#pragma once

// DLSS Frame Generation override values carried in `SharedGraphicsConfig`.
//
// Two unrelated delivery channels end up here, and keeping their encodings in
// one place is what stops them drifting apart:
//
//   * The *application parameter* channel - `dlss_fg_factor`. The game (or
//     Streamline, or RTX Remix) sets an NGX parameter per evaluation and CE
//     rewrites it. Multiplier semantics, 2x/3x/4x.
//   * The *driver settings* (DRS) channel - `dlss_fg_mode`,
//     `dlss_fg_fixed_count`, `dlss_fg_dynamic_max` and `dlss_fg_target_fps`.
//     These are the four keys NVIDIA Profile Inspector and the NVIDIA app write
//     into a driver profile; the DLSS-G runtime reads them itself. CE answers
//     that read inside the game process instead of writing any profile - see
//     hook/common/ngx_drs_override_policy.h for the driver-side ids and value
//     encodings, which are deliberately NOT part of this shared ABI.
//
// Everything here is CE's own normalized representation. A zero always means
// "no override configured", so a host that predates a field publishes zero and
// reads back as untouched.

#include <cstdint>

// ---------------------------------------------------------------------------
// Application-parameter channel (`dlss_fg_factor`)
// ---------------------------------------------------------------------------

inline int NormalizeDLSSFGFactor(int32_t dlssFGFactor) {
    return (dlssFGFactor >= 2 && dlssFGFactor <= 4) ? dlssFGFactor : 0;
}

// Frame Generation render preset letters map to 1-based driver selection values
// (A=1, B=2, ...). Anything outside A-Z means "leave the driver alone".
inline uint32_t NormalizeDLSSFGPreset(uint32_t dlssFGPreset) {
    return (dlssFGPreset >= 1 && dlssFGPreset <= 26) ? dlssFGPreset : 0u;
}

inline uint32_t DLSSFGMultiplierToGeneratedFrames(int32_t dlssFGFactor) {
    const int normalized = NormalizeDLSSFGFactor(dlssFGFactor);
    return normalized > 0 ? static_cast<uint32_t>(normalized - 1) : 0u;
}

inline int StreamlineGeneratedFramesToDLSSFGMultiplier(uint32_t generatedFrames) {
    return (generatedFrames >= 1 && generatedFrames <= 3) ? static_cast<int>(generatedFrames + 1) : 0;
}

// ---------------------------------------------------------------------------
// Driver-settings channel (`dlss_fg_mode` and the multi-frame keys)
// ---------------------------------------------------------------------------

// `dlss_fg_mode`. The names follow the runtime's own mode enum rather than
// Profile Inspector's shorter list, because the driver key accepts all of them
// and refusing a value the driver documents would be an arbitrary restriction.
// `Fixed` is what Profile Inspector labels "Fixed"; it is the runtime's "on".
inline constexpr uint8_t kDlssFGModeDefault = 0;
inline constexpr uint8_t kDlssFGModeOff = 1;
inline constexpr uint8_t kDlssFGModeFixed = 2;
inline constexpr uint8_t kDlssFGModeAuto = 3;
inline constexpr uint8_t kDlssFGModeDynamic = 4;

inline constexpr bool IsDlssFGMode(uint8_t mode) {
    return mode <= kDlssFGModeDynamic;
}

// `auto` and `dynamic` hand the cadence to the runtime. Anything that judges
// frame generation by "is a generated frame appearing" has to stand down for
// these, because not generating one is a legitimate choice there.
inline constexpr bool IsDlssFGModeCadenceChosenByRuntime(uint8_t mode) {
    return mode == kDlssFGModeAuto || mode == kDlssFGModeDynamic;
}

inline constexpr const char* DlssFGModeName(uint8_t mode) {
    return mode == kDlssFGModeOff       ? "off"
           : mode == kDlssFGModeFixed   ? "fixed"
           : mode == kDlssFGModeAuto    ? "auto"
           : mode == kDlssFGModeDynamic ? "dynamic"
                                        : "default";
}

// Multi-frame cadence, stored as the user-facing multiplier (2x..6x) rather
// than the driver's generated-frame count, so a configuration value and a log
// line read the same way. 0 means untouched.
inline constexpr uint8_t kDlssFGCountMinMultiplier = 2;
inline constexpr uint8_t kDlssFGCountMaxMultiplier = 6;

inline constexpr uint8_t NormalizeDlssFGCount(uint8_t multiplier) {
    return (multiplier >= kDlssFGCountMinMultiplier && multiplier <= kDlssFGCountMaxMultiplier) ? multiplier : 0u;
}

// `dlss_fg_target_fps`. 0 is untouched and kDlssFGTargetFpsMaxRefresh asks the
// runtime to target the display's maximum refresh rate. Everything else is a
// plain frame rate; the bounds match CE's own fps limiter range so one config
// file cannot ask for a rate the rest of CE would refuse.
inline constexpr uint16_t kDlssFGTargetFpsDefault = 0;
inline constexpr uint16_t kDlssFGTargetFpsMaxRefresh = 0xFFFFu;
inline constexpr uint16_t kDlssFGTargetFpsMin = 1;
inline constexpr uint16_t kDlssFGTargetFpsMax = 1000;

inline constexpr bool IsDlssFGTargetFpsExplicit(uint16_t targetFps) {
    return targetFps >= kDlssFGTargetFpsMin && targetFps <= kDlssFGTargetFpsMax;
}

inline constexpr uint16_t NormalizeDlssFGTargetFps(uint16_t targetFps) {
    return (targetFps == kDlssFGTargetFpsMaxRefresh || IsDlssFGTargetFpsExplicit(targetFps))
               ? targetFps
               : kDlssFGTargetFpsDefault;
}

// Dynamic mode asks the runtime to pick a cadence per frame. CE's own
// `dlss_fg_factor` forcing pins that cadence through the NGX/Streamline
// parameter channel, so the two cannot both be live: the explicit dynamic
// request wins and the fixed factor stands down. Without this the runtime would
// be told "vary the count" by the driver and "it is exactly N" by every
// evaluation, and the observable result would depend on call ordering.
inline int ResolveEffectiveDLSSFGFactor(int32_t dlssFGFactor, uint8_t dlssFGMode) {
    return dlssFGMode == kDlssFGModeDynamic ? 0 : NormalizeDLSSFGFactor(dlssFGFactor);
}

// True when any of the four driver-settings keys asks for something.
inline constexpr bool HasDlssFGDriverOverride(uint8_t mode, uint8_t fixedCount, uint8_t dynamicMax,
                                              uint16_t targetFps) {
    return mode != kDlssFGModeDefault || fixedCount != 0 || dynamicMax != 0 ||
           targetFps != kDlssFGTargetFpsDefault;
}
