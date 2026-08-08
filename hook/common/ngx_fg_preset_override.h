/**
 * DLSS Frame Generation render-preset override (config key `dlss_fg_preset`).
 *
 * Super-resolution and ray-reconstruction presets are application-facing NGX
 * parameters (`DLSS.Hint.Render.Preset.*`, `RayReconstruction.Hint.Render.Preset.*`),
 * so CE overrides them inside its NGX parameter vtable hooks. The frame
 * generation preset has no such parameter: nvngx_dlssg.dll reads it directly out
 * of the NVIDIA driver settings (DRS) in `DLSSGDRSKeys::ReadValuesFromDRSImpl`,
 * which is the same channel the NVIDIA app and third-party profile editors write.
 *
 * Verified against nvngx_dlssg.dll 310.6 and 310.7: the snippet resolves
 * `NvAPI_DRS_GetSetting` (function id 0x73BF8338) through `nvapi_QueryInterface`
 * and reads setting id 0x10E41DF1, logging `INFO: Preset ID: %d`. Value 1 selects
 * preset A and 2 selects preset B; the selection is a plain 1-based index, so
 * letters beyond NVIDIA's current A/B are accepted here and simply do nothing
 * until a runtime defines them. 310.4 and older have no preset parsing at all.
 *
 * CE answers that single DRS read process-locally by wrapping the pointer the
 * snippet resolves. Nothing is written to the machine's driver profiles, and
 * nvapi64.dll's code bytes are never patched - DLSS FG integrations validate
 * NvAPI prologues during Reflex setup (see reflex_limiter.h). With
 * `dlss_fg_preset=default` nothing is armed and no call path changes.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ce::ngx_fg_preset {

// The DRS setting nvngx_dlssg reads for its render preset, and the NvAPI
// function id it resolves to read it with.
inline constexpr uint32_t kRenderPresetDrsSettingId = 0x10E41DF1u;
inline constexpr uint32_t kNvApiIdDrsGetSetting = 0x73BF8338u;

// NVAPI_OK / NVAPI_ERROR. NvAPI status is a signed enum; only these two matter here.
inline constexpr int32_t kNvApiOk = 0;
inline constexpr int32_t kNvApiError = -1;

// NVDRS_SETTING_TYPE::NVDRS_DWORD_TYPE and
// NVDRS_SETTING_LOCATION::NVDRS_CURRENT_PROFILE_LOCATION. The snippet rejects a
// value that is not reported as coming from the current profile.
inline constexpr uint32_t kNvDrsDwordType = 0;
inline constexpr uint32_t kNvDrsCurrentProfileLocation = 0;

// NvAPI's NVDRS_SETTING, mirrored rather than vendored: CE does not ship the
// NvAPI headers. The static asserts below pin every field this code touches to
// the offsets the driver ABI actually uses, so a layout mistake fails the build
// instead of corrupting a caller's stack buffer.
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
// the high word. This is the exact value nvngx_dlssg stamps before the call.
inline constexpr uint32_t kNvDrsSettingVer1 = static_cast<uint32_t>(sizeof(NvDrsSetting)) | (1u << 16);

static_assert(sizeof(NvDrsSetting) == 0x3020, "NVDRS_SETTING v1 is 12320 bytes");
static_assert(offsetof(NvDrsSetting, settingId) == 0x1004, "NVDRS_SETTING::settingId offset");
static_assert(offsetof(NvDrsSetting, settingType) == 0x1008, "NVDRS_SETTING::settingType offset");
static_assert(offsetof(NvDrsSetting, settingLocation) == 0x100C, "NVDRS_SETTING::settingLocation offset");
static_assert(offsetof(NvDrsSetting, currentValue) == 0x201C, "NVDRS_SETTING::currentValue offset");
static_assert(kNvDrsSettingVer1 == 0x13020, "NVDRS_SETTING_VER1 must match the runtime's stamp");

using PfnNvApiDrsGetSetting = int32_t(__cdecl*)(void* session, void* profile, uint32_t settingId,
                                                NvDrsSetting* setting);

// A-Z map to the driver's 1-based selection values; anything else is "leave the
// driver alone".
uint32_t NormalizePreset(uint32_t preset);

// 'A'..'Z' for 1..26, '?' otherwise. Diagnostics only.
char PresetIdToLetter(uint32_t preset);

bool IsFrameGenerationSnippetModulePath(const char* modulePath);

// True when `functionId` is the DRS getter and the caller is the DLSS-G snippet
// with a preset actually configured. Every other caller and function id keeps
// the untouched driver pointer.
bool ShouldWrapQueryInterface(uint32_t configuredPreset, uint32_t functionId, const char* callerModulePath);

// True when a wrapped NvAPI_DRS_GetSetting call must answer with the configured
// preset. The caller's struct version has to be the one this ABI mirror
// describes; an unknown version is forwarded untouched.
bool ShouldSubstituteSetting(uint32_t configuredPreset, uint32_t settingId, uint32_t callerStructVersion);

// Writes the configured preset into a caller-owned NVDRS_SETTING. Only the
// fields the snippet reads are touched; `settingName` and `version` are left as
// the real call (or the caller) left them.
void FillSubstitutedSetting(NvDrsSetting& setting, uint32_t settingId, uint32_t preset);

// Process-wide configured preset (0 = inactive). Set from the resolved graphics
// config; safe to call repeatedly.
void SetConfiguredPreset(uint32_t preset);
uint32_t GetConfiguredPreset();
bool IsArmed();

// `nvapi_QueryInterface` participation. Returns CE's NvAPI_DRS_GetSetting
// wrapper when this resolution must be intercepted, or nullptr to hand the
// caller the pointer the driver returned.
void* MaybeWrapQueryInterface(uint32_t functionId, void* resolved, const void* callerAddress);

}  // namespace ce::ngx_fg_preset
