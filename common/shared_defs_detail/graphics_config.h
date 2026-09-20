#pragma once

// The graphics half of the shared configuration: the process-local graphics,
// DLSS/frame-generation, UE5 CVar and sharpen overrides the host publishes for
// the hook and the Vulkan layer, plus the layout contract that pins them.
//
// Split out of abi_constants_and_config.h, which keeps the ABI version, the IPC
// names, the discovery block and the overlay configuration.
//
// NOT standalone, and deliberately so: it is included from inside that file's
// `#pragma pack(push, 8)` region so the layout is byte-identical to what it was
// before the split, and it takes <cstddef>, <cstdint>, MAX_PATH,
// UE5_CVAR_OVERRIDE_CAPACITY and the DLSS FG policy constants from the includes
// that file already made *outside* the pack region. Pulling a standard header
// in here would compile it under the packing instead. The `sizeof` assertion at
// the end is what proves the split changed no layout.

struct SharedGraphicsConfig {
    char vsyncMode[32];                  // "default", "off", "fifo", "mailbox", "adaptive"
    char anisotropicFiltering[32];       // "default", "off", "2x", "4x", "8x", "16x"
    char samplerOverrideMode[16];        // "safe" (default) or "aggressive"
    char mipMapping[32];                 // "default", "nearest", "bilinear", "trilinear"
    char mipBias[32];                    // "default", "0.0", "-0.5", etc.
    char mipBiasMode[32];                // "strict", "offset", "base"
    bool forceMipBiasClamp;              // Force all texture mip bias values to 0
    char msaaSamples[32];                // "default", "off", "2x", "4x", "8x"
    // These flags occupy two bytes of the pre-existing three-byte alignment gap
    // before prerenderLimit, so neither addition changes the shared layout or ABI.
    bool nvLodSpreadFix;                 // Force NVIDIA's process-local LOD-spread branch ON
    bool forceRayReconstruction;         // Persistently select UE NVIDIA DLSS Ray Reconstruction
    bool legacyD3DNativeOverlay;         // Draw the DX6/DX7 overlay with the application's own device
    float prerenderLimit;                // integer semantics: -1=default, 0=serial, 1-6 buffered
    int32_t backbufferCount;             // -1=app controlled, 2-6 actual count
    int32_t frameLatency;                // 0=default, 1-6 (SetMaximumFrameLatency)
    bool sgssaa;                         // Enable Sparse Grid Supersampling
    bool disableAutoMipBias;             // If true, don't adjust mip bias for SGSSAA
    char dlssAutoExposure[32];           // "default", "on", "off"
    char dlssExposureNormalization[32];  // "default", "on", "off"

    // DLSS Presets (Super Resolution) - 0=Default, 1-26 = A-Z
    uint32_t dlssPresetDLAA;
    uint32_t dlssPresetQuality;
    uint32_t dlssPresetBalanced;
    uint32_t dlssPresetPerformance;
    uint32_t dlssPresetUltraPerformance;
    uint32_t dlssPresetUltraQuality;

    // Ray Reconstruction Presets - 0=Default, 1-26 = A-Z
    uint32_t dlssRRPresetDLAA;
    uint32_t dlssRRPresetQuality;
    uint32_t dlssRRPresetBalanced;
    uint32_t dlssRRPresetPerformance;
    uint32_t dlssRRPresetUltraPerformance;
    uint32_t dlssRRPresetUltraQuality;

    uint32_t dlssSRPreset;  // Global SR preset
    uint32_t dlssRRPreset;  // Global RR preset

    float dlssSharpening;  // -2.0 = default, -1.0 = off, else value
    int32_t dlssFGFactor;  // 0 = default, 2/3/4 = Frame Generation multiplier override

    // Frame Generation render preset - 0=Default, 1-26 = A-Z. Occupies the slot
    // previously retained as padding after Smooth Motion became automatic, so the
    // layout and ABI signature are unchanged: a host that predates this field
    // leaves it zero, which reads back as "no override".
    uint32_t dlssFGPreset;

    // Process-local runtime controls must follow the resolved profile into a
    // split child renderer. Paths may name a directory or one exact DLL, using
    // the same contract as GraphicsConfig. An empty path and "default"
    // indicator mode preserve the runtime's own behavior.
    char dlssSrDllPath[MAX_PATH];
    char dlssRrDllPath[MAX_PATH];
    char dlssFgDllPath[MAX_PATH];
    char streamlineDllPath[MAX_PATH];
    char dlssDebugOverlay[16];

    // UE5 process-local persistent CVar overrides. A negative sharpen value
    // leaves r.Tonemapper.Sharpen alone unless disablePostProcessingEffects is set.
    // 0=off, 1=light, 2=medium, 3=high, 4=full. The uint8 representation
    // preserves the original Boolean field's layout while allowing the graduated
    // preset. See ce::ue5_cvar::RayReconstructionPresetName for the ordering
    // contract and SHARED_MEMORY_VERSION 58 for why `full` is 4 rather than 3.
    uint8_t rayReconstructionOptimalSettings;
    bool disablePostProcessingEffects;
    float tonemapperSharpen;
    // -1 leaves UE's own engine limiter alone, 0 disables it (t.MaxFPS=0),
    // a positive value caps it (t.MaxFPS=<value>).
    float internalFpsLimit;
    // 0 leaves UE's internal AF CVars alone, 1..16 applies the same level to
    // r.MaxAnisotropy and r.VT.MaxAnisotropy (1 disables anisotropic filtering).
    int32_t internalAnisotropicFiltering;
    // UE's own texture mip bias (r.MipMapLODBias, a float CVar whose engine help
    // documents the range -15.0 to 15.0). Negative sharpens, positive blurs.
    // 0 is a meaningful value, so "leave the engine alone" cannot be 0: any value
    // outside the accepted range means untouched, and the host publishes
    // kUE5TextureMipBiasDisabled for it.
    float internalTextureMipBias;
    // UE's display gamma transform. Negative leaves the engine alone, 0 selects
    // the piecewise sRGB/Rec709 transform (r.TonemapperGamma=0, UE's "default
    // behavior"), and 1.0..3.0 selects a pure power curve of that exponent.
    // The matching r.HDR.Display.OutputDevice write is guarded so it can never
    // pull a game out of an HDR output device.
    float displayGamma;
    // UE depth of field (r.DepthOfFieldQuality): -1 untouched, 0 off, 1 on.
    int32_t depthOfField;
    // UE DLSS Super Resolution (r.NGX.DLSS.Enable plus the engine levers that
    // route rendering through a third-party temporal upscaler): -1 untouched,
    // 0 off, 1 on. The screen percentage selects the forced path's quality mode
    // and is only written while SR is forced on; outside 25..100 it is untouched.
    int32_t dlssSuperResolution;
    float dlssScreenPercentage;
    // UE HDR output (r.HDR.EnableHDROutput): -1 untouched, 0 off, 1 on. The
    // luminance fields are nits (0 or out of range = untouched) for
    // r.HDR.Display.MaxLuminance, r.HDR.Display.MidLuminance, r.HDR.UI.Luminance
    // and r.HDR.Display.MinLuminanceLog10 (converted to log10 hook-side).
    int32_t hdrOutput;
    int32_t hdrPeakLuminance;
    float hdrPaperWhite;
    float hdrUiLuminance;
    float hdrMinLuminance;
    // r.HDR.Display.ColorGamut: -1 untouched, 0..4.
    int32_t hdrColorGamut;
    // A bit selects the matching ue5_cvar::kSpecs entry and the parallel array
    // carries its already type-validated raw Int32/Float bits. Custom values
    // are resolved after all named presets, so they have final precedence.
    uint64_t ue5CustomCVarOverrideMask;
    uint32_t ue5CustomCVarOverrideValues[UE5_CVAR_OVERRIDE_CAPACITY];

    // NVIDIA NGX over-the-air update policy for this process. `_nvngx.dll`
    // launches `nvngx_update.exe` itself, through the very kernel32
    // CreateProcess imports CE already patches, and it also arbitrates its
    // Streamline plugin set against the driver's OTA model repository under
    // %ProgramData%\NVIDIA\NGX\models. Both behaviours are what this byte
    // governs; the values live in ngx_policy_and_override_status.h and the
    // policy that acts on them in hook/common/ngx_ota_policy.h.
    uint8_t ngxOtaMode;
    // NGX's own diagnostic log level, routed into the CE session directory.
    // See kNgxLog* in ngx_policy_and_override_status.h.
    uint8_t ngxLogLevel;

    // DLSS Frame Generation driver-settings overrides - the four keys NVIDIA
    // Profile Inspector writes into a driver profile. CE never writes a
    // profile: the hook answers the DLSS-G runtime's own read of them inside
    // the game process. Encodings live in dlss_frame_generation_policy.h
    // (kDlssFGMode*, NormalizeDlssFGCount, kDlssFGTargetFps*); zero always
    // means "no override". These consume the struct's existing tail padding,
    // which is why SHARED_MEMORY_VERSION had to move even though sizeof did not.
    uint8_t dlssFGMode;         // kDlssFGMode* (default/off/fixed/auto/dynamic)
    uint8_t dlssFGFixedCount;   // 0 = untouched, 2..6 = fixed cadence multiplier
    uint8_t dlssFGDynamicMax;   // 0 = untouched, 2..6 = "up to Nx" in dynamic mode
    uint16_t dlssFGTargetFps;   // 0, kDlssFGTargetFpsMaxRefresh, or 1..1000

    // Post-processing sharpen. CE filters the frame the game is about to
    // present, immediately before the overlay draws into the same target, so
    // the overlay itself is never sharpened. The encodings are
    // ce::sharpen::Mode and ce::sharpen::ConfiguredSpace; zero is "off" and
    // "auto" respectively, so a host that predates these fields reads back as
    // no sharpening at all. Unlike the fields above these do not fit existing
    // padding - they grow the mapping, which is what the version bump covers.
    uint8_t sharpenMode;
    uint8_t sharpenColorSpace;
    uint16_t sharpenReserved;
    // 0..1 within each effect's own native range. Neither effect is off at 0;
    // sharpenMode is the only switch. See common/sharpen_policy.h.
    float sharpenStrength;
    // 0..1 weight of the filtered result against the original pixels. This one
    // IS off at 0, and it is what a viewer reads as "how much sharpening".
    float sharpenIntensity;
};

// Deliberately outside UE's accepted -15..15 range, so 0 stays usable as a real
// setting. A host that predates this field publishes 0, which would otherwise
// read back as an explicit "no bias" override rather than "untouched" - the
// version bump above is what keeps such a host from being talked to at all.
inline constexpr float kUE5TextureMipBiasDisabled = 1000.0f;
inline constexpr float kUE5TextureMipBiasLimit = 15.0f;

constexpr bool IsUE5TextureMipBiasRequested(float bias) noexcept {
    return bias >= -kUE5TextureMipBiasLimit && bias <= kUE5TextureMipBiasLimit;
}

// Screen percentage the forced UE5 DLSS Super Resolution path may request. 100 is
// DLAA; the NVIDIA plugin resolves everything below it to one of its quality
// modes. The hook-side policy carries the same bounds and the unit tests pin the
// two together, so a value that survives configuration is one the hook accepts.
inline constexpr float kUE5DlssScreenPercentageMin = 25.0f;
inline constexpr float kUE5DlssScreenPercentageMax = 100.0f;

constexpr bool IsUE5DlssScreenPercentageRequested(float percentage) noexcept {
    return percentage >= kUE5DlssScreenPercentageMin && percentage <= kUE5DlssScreenPercentageMax;
}

// UE5 HDR parameter bounds, in the units the engine's own CVar help documents:
// peak and paper white in nits, the black floor in nits (the hook converts it to
// the log10 level r.HDR.Display.MinLuminanceLog10 stores). Same contract as the
// screen percentage above - configuration and hook-side policy carry one set of
// bounds, pinned together by the unit tests.
inline constexpr int32_t kUE5HdrPeakLuminanceMin = 80;
inline constexpr int32_t kUE5HdrPeakLuminanceMax = 10000;
inline constexpr float kUE5HdrPaperWhiteMin = 20.0f;
inline constexpr float kUE5HdrPaperWhiteMax = 1000.0f;
inline constexpr float kUE5HdrUiLuminanceMin = 20.0f;
inline constexpr float kUE5HdrUiLuminanceMax = 1000.0f;
inline constexpr float kUE5HdrMinLuminanceMin = 0.0001f;
inline constexpr float kUE5HdrMinLuminanceMax = 10.0f;

static_assert(offsetof(SharedGraphicsConfig, nvLodSpreadFix) ==
                  offsetof(SharedGraphicsConfig, msaaSamples) + 32,
              "nvLodSpreadFix must remain in the existing SharedGraphicsConfig padding");
static_assert(offsetof(SharedGraphicsConfig, forceRayReconstruction) ==
                  offsetof(SharedGraphicsConfig, nvLodSpreadFix) + 1,
              "forceRayReconstruction must remain in the existing SharedGraphicsConfig padding");
static_assert(offsetof(SharedGraphicsConfig, legacyD3DNativeOverlay) ==
                  offsetof(SharedGraphicsConfig, forceRayReconstruction) + 1,
              "legacyD3DNativeOverlay must consume the last byte of the existing padding");
static_assert(offsetof(SharedGraphicsConfig, prerenderLimit) ==
                  offsetof(SharedGraphicsConfig, legacyD3DNativeOverlay) + 1,
              "policy flags must not move later SharedGraphicsConfig fields");
static_assert(offsetof(SharedGraphicsConfig, rayReconstructionOptimalSettings) ==
                  offsetof(SharedGraphicsConfig, dlssDebugOverlay) + 16,
              "UE5 policy fields must remain appended to SharedGraphicsConfig");
static_assert(offsetof(SharedGraphicsConfig, tonemapperSharpen) ==
                  offsetof(SharedGraphicsConfig, rayReconstructionOptimalSettings) + 4,
              "UE5 sharpen must retain natural float alignment");
static_assert(offsetof(SharedGraphicsConfig, internalFpsLimit) ==
                  offsetof(SharedGraphicsConfig, tonemapperSharpen) + sizeof(float),
              "UE5 internal fps limit must follow the sharpen field");
static_assert(offsetof(SharedGraphicsConfig, internalAnisotropicFiltering) ==
                  offsetof(SharedGraphicsConfig, internalFpsLimit) + sizeof(float),
              "UE5 internal AF level must follow the fps limit field");
static_assert(offsetof(SharedGraphicsConfig, internalTextureMipBias) ==
                  offsetof(SharedGraphicsConfig, internalAnisotropicFiltering) + sizeof(int32_t),
              "UE5 texture mip bias must follow the internal AF level");
static_assert(offsetof(SharedGraphicsConfig, displayGamma) ==
                  offsetof(SharedGraphicsConfig, internalTextureMipBias) + sizeof(float),
              "UE5 display gamma must follow the texture mip bias");
static_assert(offsetof(SharedGraphicsConfig, depthOfField) ==
                  offsetof(SharedGraphicsConfig, displayGamma) + sizeof(float),
              "UE5 depth of field must follow the display gamma");
static_assert(offsetof(SharedGraphicsConfig, hdrColorGamut) ==
                  offsetof(SharedGraphicsConfig, depthOfField) + 8 * sizeof(int32_t),
              "the UE5 DLSS SR and HDR fields must stay contiguous after depth of field");
static_assert(offsetof(SharedGraphicsConfig, ue5CustomCVarOverrideValues) ==
                  offsetof(SharedGraphicsConfig, ue5CustomCVarOverrideMask) + sizeof(uint64_t),
              "UE5 custom CVar values must immediately follow their selection mask");
static_assert(offsetof(SharedGraphicsConfig, ngxOtaMode) ==
                  offsetof(SharedGraphicsConfig, ue5CustomCVarOverrideValues) +
                      sizeof(uint32_t) * UE5_CVAR_OVERRIDE_CAPACITY,
              "the NGX policy bytes must remain appended to SharedGraphicsConfig");
static_assert(offsetof(SharedGraphicsConfig, ngxLogLevel) == offsetof(SharedGraphicsConfig, ngxOtaMode) + 1,
              "the NGX log level must share the NGX policy byte pair");
static_assert(offsetof(SharedGraphicsConfig, dlssFGMode) == offsetof(SharedGraphicsConfig, ngxLogLevel) + 1,
              "the DLSS FG driver-settings bytes must follow the NGX policy byte pair");
static_assert(offsetof(SharedGraphicsConfig, dlssFGFixedCount) == offsetof(SharedGraphicsConfig, dlssFGMode) + 1,
              "the DLSS FG fixed cadence must follow the forced mode byte");
static_assert(offsetof(SharedGraphicsConfig, dlssFGDynamicMax) == offsetof(SharedGraphicsConfig, dlssFGFixedCount) + 1,
              "the DLSS FG dynamic maximum must follow the fixed cadence byte");
// One alignment byte separates the three policy bytes from the 16-bit target
// rate; spelling the offset out is what proves the four fields still fit the
// tail padding rather than having grown the mapping.
static_assert(offsetof(SharedGraphicsConfig, dlssFGTargetFps) == offsetof(SharedGraphicsConfig, dlssFGDynamicMax) + 2,
              "the DLSS FG target frame rate must keep its natural 16-bit alignment");
static_assert(offsetof(SharedGraphicsConfig, sharpenMode) == offsetof(SharedGraphicsConfig, dlssFGTargetFps) + 2,
              "the sharpen fields must follow the DLSS FG target frame rate");
static_assert(offsetof(SharedGraphicsConfig, sharpenColorSpace) == offsetof(SharedGraphicsConfig, sharpenMode) + 1,
              "the sharpen working space must share the sharpen byte pair");
static_assert(offsetof(SharedGraphicsConfig, sharpenStrength) == offsetof(SharedGraphicsConfig, sharpenMode) + 4,
              "the sharpen strength must keep its natural 32-bit alignment behind the two policy bytes");
static_assert(offsetof(SharedGraphicsConfig, sharpenIntensity) ==
                  offsetof(SharedGraphicsConfig, sharpenStrength) + sizeof(float),
              "the sharpen intensity must follow the strength it is independent of");
// 1752 + the twelve bytes of the sharpen block. The DLSS FG fields had already
// consumed the tail padding, so unlike those this block grows the mapping -
// which is what SHARED_MEMORY_VERSION 62 exists for.
static_assert(sizeof(SharedGraphicsConfig) == 1768,
              "SharedGraphicsConfig size change requires an IPC ABI version bump");
static_assert(offsetof(SharedGraphicsConfig, sharpenIntensity) + sizeof(float) <= sizeof(SharedGraphicsConfig),
              "the sharpen intensity must fit inside the mapping");
