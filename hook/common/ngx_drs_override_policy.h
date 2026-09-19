/**
 * Process-local answers to the DLSS driver-settings (DRS) reads.
 *
 * NVIDIA's DLSS overrides are split across two channels. Super-resolution and
 * ray-reconstruction presets are application-facing NGX parameters
 * (`DLSS.Hint.Render.Preset.*`), so CE overrides them inside its NGX parameter
 * vtable hooks. Frame generation's render preset and its whole multi-frame
 * configuration are NOT application-facing: the DLSS-G runtime reads them out
 * of the NVIDIA driver settings, which is the same channel the NVIDIA app and
 * NVIDIA Profile Inspector write into a driver profile.
 *
 * CE answers those reads inside the game process. Nothing is written to the
 * machine's driver profiles, no other application is affected, and
 * nvapi64.dll's code bytes are never patched - DLSS FG integrations validate
 * NvAPI prologues during Reflex setup (see reflex_limiter.h). With every key
 * left at its default nothing is armed and no call path changes.
 *
 * Who reads what, measured rather than assumed:
 *
 *   * `nvngx_dlssg.dll` 310.6/310.7 reads the render preset (0x10E41DF1) in
 *     `DLSSGDRSKeys::ReadValuesFromDRSImpl`, resolving `NvAPI_DRS_GetSetting`
 *     (function id 0x73BF8338) through `nvapi_QueryInterface` and logging
 *     `INFO: Preset ID: %d`. 310.4 and older have no preset parsing at all.
 *   * `sl.dlss_g.dll` (Streamline 2.14) reads the four multi-frame keys in
 *     `readDRSKeys`/`readSingleDRSKey`. That helper does not call NvAPI itself;
 *     it goes through the DRS context `sl.common.dll` owns, and *that* module
 *     resolves 0x73BF8338 through `nvapi_QueryInterface` and calls
 *     `NvAPI_DRS_GetSetting(session, profile, settingId, setting)` with
 *     `NVDRS_SETTING_VER1` and reads `currentValue.u32Value`. It tries the
 *     application profile first and falls back to the base profile, so the
 *     same key can be asked for twice and both calls reach CE's answer.
 *
 * The value encodings below come from NVIDIA's own `NvApiDriverSettings.h`
 * (`NGX_DLSSG_*_ID` and the matching `EValues_*`) cross-checked against
 * sl.dlss_g's acceptance checks: an out-of-range mode is logged as
 * "Ignoring invalid DLSSG mode %d from DRS", a zero cadence is ignored, and a
 * target rate above 0x00FFFFFF that is not the auto sentinel is logged as
 * "Ignoring invalid dynamic target frame rate %u from DRS". CE therefore never
 * emits a value the runtime would reject.
 *
 * This header is pure policy: no Windows headers, no global state, no logging.
 * ngx_drs_override.h owns the detour, the arming and the process-wide state.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../common/shared_defs.h"

namespace ce::ngx_drs {

// ---------------------------------------------------------------------------
// Driver-side identifiers
// ---------------------------------------------------------------------------

// NvAPI_DRS_GetSetting's function id, the only NvAPI entry point CE wraps here.
inline constexpr uint32_t kNvApiIdDrsGetSetting = 0x73BF8338u;

// NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID. 1-based preset selection
// (1 = A, 2 = B, ...).
inline constexpr uint32_t kRenderPresetDrsSettingId = 0x10E41DF1u;
// NGX_DLSSG_MODE_ID. 0 = no override, then off/on/auto/dynamic.
inline constexpr uint32_t kFrameGenerationModeDrsSettingId = 0x10308298u;
// NGX_DLSSG_MULTI_FRAME_COUNT_ID. 0 = no override, else generated frames
// (1 = 2x ... 5 = 6x).
inline constexpr uint32_t kMultiFrameCountDrsSettingId = 0x104D6667u;
// NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_ID. Same encoding, read as the
// ceiling the dynamic scheduler may not exceed.
inline constexpr uint32_t kDynamicMultiFrameCountMaxDrsSettingId = 0x10562D0Fu;
// NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_ID. 0 = no override, the auto sentinel
// below = the display's maximum refresh rate, else a plain frame rate.
inline constexpr uint32_t kDynamicTargetFrameRateDrsSettingId = 0x10CF4125u;

// EValues_NGX_DLSSG_MODE. CE's own kDlssFGMode* values are deliberately a
// different numbering (they are a configuration enum, not a driver ABI), so
// the translation is explicit.
inline constexpr uint32_t kDrsFrameGenerationModeOff = 1u;
inline constexpr uint32_t kDrsFrameGenerationModeOn = 2u;
inline constexpr uint32_t kDrsFrameGenerationModeAuto = 3u;
inline constexpr uint32_t kDrsFrameGenerationModeDynamic = 4u;

// NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_AUTO, and the largest plain rate the
// runtime still accepts (NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_MAX).
inline constexpr uint32_t kDrsDynamicTargetFrameRateAuto = 0x01000000u;
inline constexpr uint32_t kDrsDynamicTargetFrameRateMax = 0x00FFFFFFu;

// NVAPI_OK / NVAPI_ERROR. NvAPI status is a signed enum; only these two matter here.
inline constexpr int32_t kNvApiOk = 0;
inline constexpr int32_t kNvApiError = -1;

// NVDRS_SETTING_TYPE::NVDRS_DWORD_TYPE and
// NVDRS_SETTING_LOCATION::NVDRS_CURRENT_PROFILE_LOCATION. The DLSS-G snippet
// rejects a value that is not reported as coming from the current profile.
inline constexpr uint32_t kNvDrsDwordType = 0;
inline constexpr uint32_t kNvDrsCurrentProfileLocation = 0;

// ---------------------------------------------------------------------------
// NVDRS_SETTING ABI mirror
// ---------------------------------------------------------------------------
//
// Mirrored rather than vendored: CE does not ship the NvAPI headers. The static
// asserts below pin every field this code touches to the offsets the driver ABI
// actually uses, so a layout mistake fails the build instead of corrupting a
// caller's stack buffer.
inline constexpr size_t kNvApiUnicodeStringChars = 2048;  // NvAPI_UnicodeString
inline constexpr size_t kNvDrsSettingUnionBytes = 4100;   // NVDRS_BINARY_SETTING

union NvDrsSettingValue {
    uint32_t u32Value;
    uint8_t binaryOrStringValue[kNvDrsSettingUnionBytes];
};

struct NvDrsSetting {
    uint32_t version;
    uint16_t settingName[kNvApiUnicodeStringChars];
    uint32_t settingId;
    uint32_t settingType;
    uint32_t settingLocation;
    uint32_t isCurrentPredefined;
    uint32_t isPredefinedValid;
    NvDrsSettingValue predefinedValue;
    NvDrsSettingValue currentValue;
};

// MAKE_NVAPI_VERSION(NVDRS_SETTING, 1): struct size in the low word, version in
// the high word. This is the exact value both readers stamp before the call.
inline constexpr uint32_t kNvDrsSettingVer1 = static_cast<uint32_t>(sizeof(NvDrsSetting)) | (1u << 16);

static_assert(sizeof(NvDrsSetting) == 0x3020, "NVDRS_SETTING v1 is 12320 bytes");
static_assert(offsetof(NvDrsSetting, settingId) == 0x1004, "NVDRS_SETTING::settingId offset");
static_assert(offsetof(NvDrsSetting, settingType) == 0x1008, "NVDRS_SETTING::settingType offset");
static_assert(offsetof(NvDrsSetting, settingLocation) == 0x100C, "NVDRS_SETTING::settingLocation offset");
static_assert(offsetof(NvDrsSetting, currentValue) == 0x201C, "NVDRS_SETTING::currentValue offset");
static_assert(kNvDrsSettingVer1 == 0x13020, "NVDRS_SETTING_VER1 must match the runtime's stamp");

using PfnNvApiDrsGetSetting = int32_t(__cdecl*)(void* session, void* profile, uint32_t settingId,
                                                NvDrsSetting* setting);

// ---------------------------------------------------------------------------
// The configured answers
// ---------------------------------------------------------------------------

// CE's resolved configuration, in CE's own units. Translation into driver
// values happens in ResolveSubstitutedValue and nowhere else.
struct DlssDrsOverrides {
    uint32_t renderPreset = 0;   // 1..26 (A..Z), 0 = untouched
    uint8_t frameGenerationMode = kDlssFGModeDefault;
    uint8_t fixedCountMultiplier = 0;    // 2..6, 0 = untouched
    uint8_t dynamicMaxMultiplier = 0;    // 2..6, 0 = untouched
    uint16_t dynamicTargetFps = kDlssFGTargetFpsDefault;

    bool operator==(const DlssDrsOverrides& other) const {
        return renderPreset == other.renderPreset && frameGenerationMode == other.frameGenerationMode &&
               fixedCountMultiplier == other.fixedCountMultiplier &&
               dynamicMaxMultiplier == other.dynamicMaxMultiplier && dynamicTargetFps == other.dynamicTargetFps;
    }
    bool operator!=(const DlssDrsOverrides& other) const { return !(*this == other); }
};

// A-Z map to the driver's 1-based selection values; anything else is "leave the
// driver alone".
inline constexpr uint32_t NormalizePreset(uint32_t preset) {
    return (preset >= 1 && preset <= 26) ? preset : 0u;
}

// 'A'..'Z' for 1..26, '?' otherwise. Diagnostics only.
inline constexpr char PresetIdToLetter(uint32_t preset) {
    return NormalizePreset(preset) ? static_cast<char>('A' + static_cast<int>(preset) - 1) : '?';
}

// Drops anything outside the accepted ranges, so a value that survives here is
// one both CE and the runtime accept.
inline constexpr DlssDrsOverrides Normalize(DlssDrsOverrides overrides) {
    overrides.renderPreset = NormalizePreset(overrides.renderPreset);
    if (!IsDlssFGMode(overrides.frameGenerationMode))
        overrides.frameGenerationMode = kDlssFGModeDefault;
    overrides.fixedCountMultiplier = NormalizeDlssFGCount(overrides.fixedCountMultiplier);
    overrides.dynamicMaxMultiplier = NormalizeDlssFGCount(overrides.dynamicMaxMultiplier);
    overrides.dynamicTargetFps = NormalizeDlssFGTargetFps(overrides.dynamicTargetFps);
    return overrides;
}

inline constexpr bool HasAnyOverride(const DlssDrsOverrides& overrides) {
    const DlssDrsOverrides normalized = Normalize(overrides);
    return normalized.renderPreset != 0 ||
           HasDlssFGDriverOverride(normalized.frameGenerationMode, normalized.fixedCountMultiplier,
                                   normalized.dynamicMaxMultiplier, normalized.dynamicTargetFps);
}

// CE's configuration enum to EValues_NGX_DLSSG_MODE. 0 means "not configured",
// which is also the driver's own "no override" value.
inline constexpr uint32_t FrameGenerationModeToDrsValue(uint8_t mode) {
    return mode == kDlssFGModeOff       ? kDrsFrameGenerationModeOff
           : mode == kDlssFGModeFixed   ? kDrsFrameGenerationModeOn
           : mode == kDlssFGModeAuto    ? kDrsFrameGenerationModeAuto
           : mode == kDlssFGModeDynamic ? kDrsFrameGenerationModeDynamic
                                        : 0u;
}

// A 2x..6x multiplier is N-1 generated frames between real frames, which is the
// unit both multi-frame keys use. Profile Inspector shows the same mapping
// (value 1 = "2x", value 5 = "6x").
inline constexpr uint32_t MultiplierToDrsGeneratedFrames(uint8_t multiplier) {
    const uint8_t normalized = NormalizeDlssFGCount(multiplier);
    return normalized > 0 ? static_cast<uint32_t>(normalized - 1) : 0u;
}

inline constexpr uint32_t TargetFpsToDrsValue(uint16_t targetFps) {
    return targetFps == kDlssFGTargetFpsMaxRefresh ? kDrsDynamicTargetFrameRateAuto
           : IsDlssFGTargetFpsExplicit(targetFps)  ? static_cast<uint32_t>(targetFps)
                                                   : 0u;
}

// The driver value CE answers a given setting id with. False means "nothing is
// configured for this key", and the caller must forward the driver's own
// answer untouched.
inline constexpr bool ResolveSubstitutedValue(const DlssDrsOverrides& rawOverrides, uint32_t settingId,
                                              uint32_t& outValue) {
    const DlssDrsOverrides overrides = Normalize(rawOverrides);
    uint32_t value = 0;
    switch (settingId) {
        case kRenderPresetDrsSettingId:
            value = overrides.renderPreset;
            break;
        case kFrameGenerationModeDrsSettingId:
            value = FrameGenerationModeToDrsValue(overrides.frameGenerationMode);
            break;
        case kMultiFrameCountDrsSettingId:
            value = MultiplierToDrsGeneratedFrames(overrides.fixedCountMultiplier);
            break;
        case kDynamicMultiFrameCountMaxDrsSettingId:
            value = MultiplierToDrsGeneratedFrames(overrides.dynamicMaxMultiplier);
            break;
        case kDynamicTargetFrameRateDrsSettingId:
            value = TargetFpsToDrsValue(overrides.dynamicTargetFps);
            break;
        default:
            return false;
    }
    if (value == 0)
        return false;
    outValue = value;
    return true;
}

// Diagnostics only.
inline constexpr const char* DrsSettingIdName(uint32_t settingId) {
    return settingId == kRenderPresetDrsSettingId                 ? "DLSS-FG render preset"
           : settingId == kFrameGenerationModeDrsSettingId        ? "DLSS-FG forced mode"
           : settingId == kMultiFrameCountDrsSettingId            ? "DLSS-MFG fixed count"
           : settingId == kDynamicMultiFrameCountMaxDrsSettingId  ? "DLSS-MFG dynamic maximum"
           : settingId == kDynamicTargetFrameRateDrsSettingId     ? "DLSS-MFG target frame rate"
                                                                  : "other";
}

// ---------------------------------------------------------------------------
// Caller attribution
// ---------------------------------------------------------------------------

// Everything that reads these keys is either the NGX frame generation snippet
// or a Streamline core module. Matching them keeps every other NvAPI consumer -
// the game, Reflex, third-party overlays - on the untouched driver pointer.
//
// `exportsStreamlinePluginEntry` exists because a Streamline plugin the driver
// downloaded over the air is mapped from %ProgramData%\NVIDIA\NGX\models under
// a content-addressed name such as `160_E658703.dll`. Its file name says
// nothing, but every Streamline plugin - sl.common included - exports
// `slGetPluginFunction`, and that is what identifies it.
bool IsDlssDrsConsumerModuleName(const char* modulePath);

inline bool IsDlssDrsConsumerModule(const char* modulePath, bool exportsStreamlinePluginEntry) {
    return IsDlssDrsConsumerModuleName(modulePath) || exportsStreamlinePluginEntry;
}

// Retained under its original name: the frame generation snippet is still the
// module whose GetProcAddress import has to be patched before its first
// resolution, and call sites outside this unit ask exactly that question.
bool IsFrameGenerationSnippetModulePath(const char* modulePath);

// True when `functionId` is the DRS getter and the caller is a DLSS driver-
// settings consumer with something actually configured.
inline bool ShouldWrapQueryInterface(const DlssDrsOverrides& overrides, uint32_t functionId,
                                     const char* callerModulePath, bool exportsStreamlinePluginEntry) {
    return HasAnyOverride(overrides) && functionId == kNvApiIdDrsGetSetting &&
           IsDlssDrsConsumerModule(callerModulePath, exportsStreamlinePluginEntry);
}

// True when a wrapped NvAPI_DRS_GetSetting call must answer with a configured
// value. The caller's struct version has to be the one this ABI mirror
// describes; an unknown version is forwarded untouched.
inline bool ShouldSubstituteSetting(const DlssDrsOverrides& overrides, uint32_t settingId,
                                    uint32_t callerStructVersion) {
    uint32_t unused = 0;
    return callerStructVersion == kNvDrsSettingVer1 && ResolveSubstitutedValue(overrides, settingId, unused);
}

// Writes a resolved driver value into a caller-owned NVDRS_SETTING. Only the
// fields the readers touch are written; `settingName` and `version` are left as
// the real call (or the caller) left them.
inline void FillSubstitutedSetting(NvDrsSetting& setting, uint32_t settingId, uint32_t driverValue) {
    setting.settingId = settingId;
    setting.settingType = kNvDrsDwordType;
    // The readers only accept a value they believe the current profile set
    // explicitly, so report current-profile location and a non-predefined value.
    setting.settingLocation = kNvDrsCurrentProfileLocation;
    setting.isCurrentPredefined = 0;
    setting.isPredefinedValid = 0;
    setting.currentValue.u32Value = driverValue;
}

}  // namespace ce::ngx_drs
