#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ce::ue5_cvar {

enum class ValueType : uint8_t {
    Int32,
    Float,
};

enum class Activation : uint8_t {
    RayReconstruction,
    RayReconstructionLight,
    RayReconstructionMedium,
    RayReconstructionFull,
    DisablePostProcessing,
    TonemapperSharpen,
    InternalFpsLimit,
    InternalAnisotropicFiltering,
    InternalTextureMipBias,
    // Piecewise-sRGB half of the display gamma override: only written when the
    // game is asking for the sRGB transform, never to pick a device.
    DisplayGammaOutputDevice,
    DisplayGammaExponent,
    DepthOfField,
    // r.NGX.DLSS.Enable itself: the NVIDIA UE plugin's own documented on/off
    // switch, written in both directions.
    DlssSuperResolutionEnable,
    // The levers that only matter when DLSS SR is being forced ON, each with its
    // own constant: without them a game that never exposed DLSS keeps routing
    // through its own upscaler and the enable flag reaches nothing.
    DlssSuperResolutionForceOn,
    DlssSuperResolutionScreenPercentage,
    HdrOutput,
    HdrPeakLuminance,
    HdrPaperWhite,
    HdrUiLuminance,
    HdrMinLuminance,
    HdrColorGamut,
};

inline constexpr uint8_t kRayReconstructionPresetOff = 0;
inline constexpr uint8_t kRayReconstructionPresetLight = 1;
inline constexpr uint8_t kRayReconstructionPresetMedium = 2;
inline constexpr uint8_t kRayReconstructionPresetFull = 3;
inline constexpr std::size_t kCustomCVarOverrideCapacity = 64;

// Whether an override may be applied at all, judged from the value the game
// currently holds. Most CVars are unconditional; a few are only safe to touch in
// a particular engine state, and guessing there is how a capture gets wrecked.
enum class ApplyGuard : uint8_t {
    Always,
    // r.HDR.Display.OutputDevice doubles as the HDR output selector: 0 sRGB,
    // 1 Rec709, 2 explicit gamma, 3-6 ST-2084/ScRGB HDR, 7-9 linear (the engine's
    // own help text). Writing an SDR device over an HDR one would silently drop
    // a game out of HDR and ruin an HDR capture, so this override applies only
    // when the game is already on an SDR device.
    SdrOutputDeviceOnly,
};

// UE's own texture mip bias is a float CVar whose engine help documents the
// range -15.0 to 15.0 (verified in the Talos 5.4.4 binary, where the
// registration passes its default in xmm2 - a float, not an int). Negative
// sharpens, positive blurs, and 0 is a real setting rather than "off", so the
// untouched state has to be a sentinel outside the accepted range.
inline constexpr float kTextureMipBiasLimit = 15.0f;
inline constexpr float kTextureMipBiasDisabled = 1000.0f;

constexpr bool IsTextureMipBiasRequested(float bias) noexcept {
    return bias >= -kTextureMipBiasLimit && bias <= kTextureMipBiasLimit;
}

// The display gamma request is carried as the r.TonemapperGamma value itself:
// negative means untouched, 0 is UE's own "default behavior" (the piecewise sRGB
// / Rec709 transform), and a positive exponent is a pure power curve.
inline constexpr float kDisplayGammaDefault = -1.0f;
inline constexpr float kDisplayGammaSrgb = 0.0f;
inline constexpr float kDisplayGammaMinExponent = 1.0f;
inline constexpr float kDisplayGammaMaxExponent = 3.0f;

constexpr bool IsDisplayGammaRequested(float gamma) noexcept {
    return gamma == kDisplayGammaSrgb ||
           (gamma >= kDisplayGammaMinExponent && gamma <= kDisplayGammaMaxExponent);
}

constexpr bool IsDisplayGammaPiecewiseSrgb(float gamma) noexcept {
    return gamma == kDisplayGammaSrgb;
}

// Tri-state switches (depth of field, DLSS Super Resolution, HDR output) are
// carried as int32 so the shared-memory field and the policy agree on one
// representation. Anything that is not exactly off or on - including a value a
// host built before the field existed leaves behind - means "leave the engine
// alone" rather than a guessed direction.
inline constexpr int32_t kToggleDefault = -1;
inline constexpr int32_t kToggleOff = 0;
inline constexpr int32_t kToggleOn = 1;

constexpr bool IsToggleRequested(int32_t toggle) noexcept {
    return toggle == kToggleOff || toggle == kToggleOn;
}

constexpr bool IsToggleOn(int32_t toggle) noexcept {
    return toggle == kToggleOn;
}

// UE's own r.DepthOfFieldQuality: the engine help documents 0 as "Off" and 2 as
// the default quality (measured in the Gothic 1 Remake UE5 registration, which
// passes 2 in r8d). Forcing it on restores that default; it cannot invent depth
// of field a game never configured, because the blur still comes from the
// post-process settings.
inline constexpr int32_t kDepthOfFieldOffQuality = 0;
inline constexpr int32_t kDepthOfFieldDefaultQuality = 2;

// DLSS Super Resolution quality is expressed as UE's own screen percentage,
// which is what the NVIDIA plugin snaps to its quality modes. 100 is DLAA.
inline constexpr float kDlssScreenPercentageMin = 25.0f;
inline constexpr float kDlssScreenPercentageMax = 100.0f;

constexpr bool IsDlssScreenPercentageRequested(float percentage) noexcept {
    return percentage >= kDlssScreenPercentageMin && percentage <= kDlssScreenPercentageMax;
}

// HDR luminance bounds. The engine's own names for these: r.HDR.Display.MaxLuminance
// is "the configured display output nit level", r.HDR.Display.MidLuminance "the
// configured display output nit level for 18% gray" (paper white),
// r.HDR.UI.Luminance the "base Luminance in nits for UI elements", and
// r.HDR.Display.MinLuminanceLog10 "the configured minimum display output nit
// level (log10 value)" - so the black floor is configured in nits here and
// converted, rather than making the user write a logarithm.
inline constexpr int32_t kHdrPeakLuminanceMin = 80;
inline constexpr int32_t kHdrPeakLuminanceMax = 10000;
inline constexpr float kHdrPaperWhiteMin = 20.0f;
inline constexpr float kHdrPaperWhiteMax = 1000.0f;
inline constexpr float kHdrUiLuminanceMin = 20.0f;
inline constexpr float kHdrUiLuminanceMax = 1000.0f;
inline constexpr float kHdrMinLuminanceMin = 0.0001f;
inline constexpr float kHdrMinLuminanceMax = 10.0f;
inline constexpr int32_t kHdrColorGamutMax = 4;  // 0 Rec709, 1 DCI-P3, 2 Rec2020, 3 ACES, 4 ACEScg

constexpr bool IsHdrPeakLuminanceRequested(int32_t nits) noexcept {
    return nits >= kHdrPeakLuminanceMin && nits <= kHdrPeakLuminanceMax;
}

constexpr bool IsHdrPaperWhiteRequested(float nits) noexcept {
    return nits >= kHdrPaperWhiteMin && nits <= kHdrPaperWhiteMax;
}

constexpr bool IsHdrUiLuminanceRequested(float nits) noexcept {
    return nits >= kHdrUiLuminanceMin && nits <= kHdrUiLuminanceMax;
}

constexpr bool IsHdrMinLuminanceRequested(float nits) noexcept {
    return nits >= kHdrMinLuminanceMin && nits <= kHdrMinLuminanceMax;
}

constexpr bool IsHdrColorGamutRequested(int32_t gamut) noexcept {
    return gamut >= 0 && gamut <= kHdrColorGamutMax;
}

struct Settings {
    bool forceRayReconstruction = false;
    uint8_t rayReconstructionOptimalSettings = kRayReconstructionPresetOff;
    bool disablePostProcessingEffects = false;
    float tonemapperSharpen = -1.0f;
    // -1 leaves UE's own engine limiter alone, 0 disables it (t.MaxFPS=0), a
    // positive value caps it (t.MaxFPS=<value>).
    float internalFpsLimit = -1.0f;
    // 0 leaves UE's internal AF CVars alone, 1..16 applies the same level to
    // r.MaxAnisotropy and r.VT.MaxAnisotropy (1 disables anisotropic filtering).
    int32_t internalAnisotropicFiltering = 0;
    // Anything outside -15..15 leaves r.MipMapLODBias alone; a value inside it
    // (including 0) is applied.
    float internalTextureMipBias = kTextureMipBiasDisabled;
    // Negative leaves the engine's display gamma alone, 0 selects UE's piecewise
    // sRGB/Rec709 transform, 1.0..3.0 selects a pure power curve.
    float displayGamma = kDisplayGammaDefault;
    // -1 leaves r.DepthOfFieldQuality alone, 0 disables depth of field, 1 forces
    // UE's default quality back on.
    int32_t depthOfField = kToggleDefault;
    // -1 leaves the NVIDIA UE plugin alone, 0 disables DLSS SR/RR through its own
    // r.NGX.DLSS.Enable switch, 1 forces the whole DLSS SR path on.
    int32_t dlssSuperResolution = kToggleDefault;
    // Screen percentage the forced DLSS SR path asks for (the plugin resolves it
    // to a quality mode). Outside 25..100 leaves r.ScreenPercentage alone, and it
    // is only written while DLSS SR is being forced on.
    float dlssScreenPercentage = 0.0f;
    // -1 leaves r.HDR.EnableHDROutput alone, 0/1 force the engine's HDR output.
    int32_t hdrOutput = kToggleDefault;
    // HDR display parameters in nits (0 or out of range leaves each one alone).
    int32_t hdrPeakLuminance = 0;
    float hdrPaperWhite = 0.0f;
    float hdrUiLuminance = 0.0f;
    float hdrMinLuminance = 0.0f;
    // -1 leaves r.HDR.Display.ColorGamut alone, 0..4 selects the output gamut.
    int32_t hdrColorGamut = kToggleDefault;
    uint64_t customCVarOverrideMask = 0;
    std::array<uint32_t, kCustomCVarOverrideCapacity> customCVarOverrideValues{};
};

struct Spec {
    const char* name = nullptr;
    ValueType type = ValueType::Int32;
    Activation activation = Activation::RayReconstructionFull;
    double value = 0.0;
    ApplyGuard guard = ApplyGuard::Always;
};

struct ResolvedValue {
    bool enabled = false;
    uint32_t bits = 0;
};

inline constexpr std::array kSpecs{
    Spec{"r.NGX.DLSS.DenoiserMode", ValueType::Int32, Activation::RayReconstruction, 1.0},
    Spec{"r.Lumen.Reflections.BilateralFilter", ValueType::Int32,
         Activation::RayReconstructionLight, 0.0},
    Spec{"r.Lumen.Reflections.ScreenSpaceReconstruction", ValueType::Int32,
         Activation::RayReconstructionLight, 0.0},
    Spec{"r.Lumen.Reflections.Temporal", ValueType::Int32, Activation::RayReconstructionLight, 0.0},
    Spec{"r.Lumen.Reflections.DownsampleFactor", ValueType::Int32,
         Activation::RayReconstructionMedium, 1.0},
    Spec{"r.Lumen.Reflections.DownsampleCheckerboard", ValueType::Int32,
         Activation::RayReconstructionFull, 0.0},
    Spec{"r.Lumen.Reflections.MaxRayIntensity", ValueType::Float,
         Activation::RayReconstructionFull, 100.0},
    Spec{"r.Lumen.ScreenProbeGather.StochasticInterpolation", ValueType::Int32,
         Activation::RayReconstructionFull, 0.0},
    Spec{"r.Lumen.ScreenProbeGather.SpatialFilterProbes", ValueType::Int32,
         Activation::RayReconstructionFull, 1.0},
    Spec{"r.Lumen.ScreenProbeGather.SpatialFilterNumPasses", ValueType::Int32,
         Activation::RayReconstructionFull, 3.0},
    // Float, not int: Talos's console object carries a shadow of 25.0f
    // (0x41C80000) behind its reference pointer, which the Int32 plausibility
    // check refused for the whole life of this entry. The refusal was right -
    // installing it as an int would have written 10 (1.4e-44 as a float) into a
    // float global, i.e. no temporal accumulation at all. The type checks stay
    // fail-closed in the other direction too: an int-typed build would present
    // 10 or 25, which reinterpret as denormal floats and are refused rather
    // than written.
    Spec{"r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated", ValueType::Float,
         Activation::RayReconstructionFull, 10.0},
    Spec{"r.Lumen.ScreenProbeGather.Temporal.MaxRayDirections", ValueType::Int32,
         Activation::RayReconstructionFull, 8.0},
    Spec{"r.Lumen.ScreenProbeGather.Temporal.RejectBasedOnNormal", ValueType::Int32,
         Activation::RayReconstructionFull, 0.0},
    Spec{"r.Lumen.ScreenProbeGather.Temporal.FastUpdateModeUseNeighborhoodClamp", ValueType::Int32,
         Activation::RayReconstructionFull, 0.0},
    Spec{"r.Lumen.ScreenProbeGather.TracingOctahedronResolution", ValueType::Int32,
         Activation::RayReconstructionFull, 16.0},
    Spec{"r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget", ValueType::Int32,
         Activation::RayReconstructionFull, 600.0},
    Spec{"r.Lumen.ScreenProbeGather.RadianceCache.ProbeResolution", ValueType::Int32,
         Activation::RayReconstructionFull, 32.0},
    Spec{"r.Lumen.ScreenProbeGather.ShortRangeAO.ApplyDuringIntegration", ValueType::Int32,
         Activation::RayReconstructionFull, 0.0},
    Spec{"r.LumenScene.Radiosity.Temporal.MaxFramesAccumulated", ValueType::Int32,
         Activation::RayReconstructionFull, 4.0},
    Spec{"r.LumenScene.Radiosity.UpdateFactor", ValueType::Int32,
         Activation::RayReconstructionFull, 16.0},
    Spec{"r.LumenScene.DirectLighting.UpdateFactor", ValueType::Int32,
         Activation::RayReconstructionFull, 16.0},
    Spec{"r.Shadow.Virtual.SMRT.RayCountDirectional", ValueType::Int32,
         Activation::RayReconstructionFull, 12.0},
    Spec{"r.Shadow.Virtual.SMRT.SamplesPerRayDirectional", ValueType::Int32,
         Activation::RayReconstructionFull, 4.0},
    Spec{"r.Shadow.Virtual.SMRT.RayCountLocal", ValueType::Int32,
         Activation::RayReconstructionFull, 12.0},
    Spec{"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", ValueType::Int32,
         Activation::RayReconstructionFull, 4.0},
    Spec{"r.Shadow.Virtual.ResolutionLodBiasLocal", ValueType::Float,
         Activation::RayReconstructionFull, -0.5},
    Spec{"r.Shadow.Virtual.ResolutionLodBiasLocalMoving", ValueType::Float,
         Activation::RayReconstructionFull, 0.5},
    Spec{"r.MegaLights.DownsampleMode", ValueType::Int32, Activation::RayReconstructionFull, 0.0},
    Spec{"r.MegaLights.NumSamplesPerPixel", ValueType::Int32,
         Activation::RayReconstructionFull, 8.0},
    Spec{"r.Tonemapper.Sharpen", ValueType::Float, Activation::TonemapperSharpen, 0.0},
    Spec{"r.FilmGrain", ValueType::Int32, Activation::DisablePostProcessing, 0.0},
    Spec{"r.Tonemapper.GrainQuantization", ValueType::Int32,
         Activation::DisablePostProcessing, 0.0},
    Spec{"r.MotionBlurQuality", ValueType::Int32, Activation::DisablePostProcessing, 0.0},
    Spec{"r.SceneColorFringeQuality", ValueType::Int32, Activation::DisablePostProcessing, 0.0},
    Spec{"ShowFlag.Vignette", ValueType::Int32, Activation::DisablePostProcessing, 0.0},
    Spec{"ShowFlag.Grain", ValueType::Int32, Activation::DisablePostProcessing, 0.0},
    Spec{"ShowFlag.MotionBlur", ValueType::Int32, Activation::DisablePostProcessing, 0.0},
    Spec{"ShowFlag.SceneColorFringe", ValueType::Int32, Activation::DisablePostProcessing, 0.0},
    Spec{"t.MaxFPS", ValueType::Float, Activation::InternalFpsLimit, 0.0},
    Spec{"r.MaxAnisotropy", ValueType::Int32, Activation::InternalAnisotropicFiltering, 16.0},
    Spec{"r.VT.MaxAnisotropy", ValueType::Int32, Activation::InternalAnisotropicFiltering, 16.0},
    // Appended last on purpose: kDenoiserModeIndex and kTonemapperSharpenIndex
    // are positional, so a new entry anywhere earlier would silently retarget
    // them. Float type confirmed from the engine's own registration.
    Spec{"r.MipMapLODBias", ValueType::Float, Activation::InternalTextureMipBias, 0.0},
    // Display gamma. Only the sRGB direction needs to touch the output device:
    // UE raises the device to explicit-gamma mapping by itself once
    // r.TonemapperGamma is positive, so the pure-power direction writes the
    // exponent alone and never selects a device.
    Spec{"r.HDR.Display.OutputDevice", ValueType::Int32, Activation::DisplayGammaOutputDevice, 0.0,
         ApplyGuard::SdrOutputDeviceOnly},
    Spec{"r.TonemapperGamma", ValueType::Float, Activation::DisplayGammaExponent, 0.0},
    // Depth of field. Int32 with default 2, read out of the UE5 registration
    // itself (`mov r8d, 2` into the int32 registration slot).
    Spec{"r.DepthOfFieldQuality", ValueType::Int32, Activation::DepthOfField, 0.0},
    // DLSS Super Resolution. r.NGX.DLSS.Enable is the plugin's own switch
    // ("0: Disable DLSS-SR and DLSS-RR (default), 1: Enable DLSS-SR or DLSS-RR"
    // in its registered help text), so it carries both directions. The rest only
    // move when SR is forced on: UE only routes through a third-party temporal
    // upscaler when the AA method is TAA and r.TemporalAA.Upscaler is 1, which is
    // exactly what a game that ships TSR (r.AntiAliasingMethod default 4 in the
    // measured build) never does.
    Spec{"r.NGX.DLSS.Enable", ValueType::Int32, Activation::DlssSuperResolutionEnable, 0.0},
    Spec{"r.NGX.Enable", ValueType::Int32, Activation::DlssSuperResolutionForceOn, 1.0},
    Spec{"r.TemporalAA.Upscaler", ValueType::Int32, Activation::DlssSuperResolutionForceOn, 1.0},
    Spec{"r.AntiAliasingMethod", ValueType::Int32, Activation::DlssSuperResolutionForceOn, 2.0},
    Spec{"r.ScreenPercentage", ValueType::Float, Activation::DlssSuperResolutionScreenPercentage, 100.0},
    // HDR. Types measured from the UE5 registrations: the int32 registration
    // passes its default in r8d, the float one in xmm2, and MaxLuminance/
    // ColorGamut/EnableHDROutput take the int slot while Mid/MinLuminance and
    // UI.Luminance take the float slot.
    Spec{"r.HDR.EnableHDROutput", ValueType::Int32, Activation::HdrOutput, 0.0},
    Spec{"r.HDR.Display.MaxLuminance", ValueType::Int32, Activation::HdrPeakLuminance, 0.0},
    Spec{"r.HDR.Display.MidLuminance", ValueType::Float, Activation::HdrPaperWhite, 0.0},
    Spec{"r.HDR.UI.Luminance", ValueType::Float, Activation::HdrUiLuminance, 0.0},
    Spec{"r.HDR.Display.MinLuminanceLog10", ValueType::Float, Activation::HdrMinLuminance, 0.0},
    Spec{"r.HDR.Display.ColorGamut", ValueType::Int32, Activation::HdrColorGamut, 0.0},
    // Kept at the end so every established positional index remains stable.
    Spec{"r.SSR.Temporal", ValueType::Int32, Activation::RayReconstructionLight, 0.0},
};

inline constexpr std::size_t kDenoiserModeIndex = 0;
inline constexpr std::size_t kTonemapperSharpenIndex = 29;
static_assert(kSpecs.size() <= kCustomCVarOverrideCapacity,
              "the custom CVar selection mask needs one bit per spec");

inline char AsciiLower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

inline bool EqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (AsciiLower(left[index]) != AsciiLower(right[index]))
            return false;
    }
    return true;
}

inline bool MatchesConfigAlias(std::string_view canonical, std::string_view candidate) noexcept {
    std::size_t source = canonical.size() > 2 && canonical[1] == '.' ? 2 : 0;
    if (canonical.size() - source != candidate.size())
        return false;
    for (std::size_t index = 0; index < candidate.size(); ++index) {
        char expected = canonical[source + index];
        if (expected == '.')
            expected = '_';
        if (AsciiLower(expected) != AsciiLower(candidate[index]))
            return false;
    }
    return true;
}

inline std::size_t FindSpecIndex(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kSpecs.size(); ++index) {
        if (EqualsIgnoreCase(kSpecs[index].name, name) || MatchesConfigAlias(kSpecs[index].name, name))
            return index;
    }
    return kSpecs.size();
}

inline uint32_t ValueBits(ValueType type, double value) noexcept {
    if (type == ValueType::Float)
        return std::bit_cast<uint32_t>(static_cast<float>(value));
    return static_cast<uint32_t>(static_cast<int32_t>(value));
}

inline ResolvedValue Resolve(const Spec& spec, const Settings& settings) noexcept {
    bool enabled = false;
    double value = spec.value;
    switch (spec.activation) {
        case Activation::RayReconstruction:
            enabled = settings.forceRayReconstruction;
            break;
        case Activation::RayReconstructionLight:
            enabled = settings.rayReconstructionOptimalSettings >= kRayReconstructionPresetLight;
            break;
        case Activation::RayReconstructionMedium:
            enabled = settings.rayReconstructionOptimalSettings >= kRayReconstructionPresetMedium;
            break;
        case Activation::RayReconstructionFull:
            enabled = settings.rayReconstructionOptimalSettings >= kRayReconstructionPresetFull;
            break;
        case Activation::DisablePostProcessing:
            enabled = settings.disablePostProcessingEffects;
            break;
        case Activation::TonemapperSharpen:
            enabled = settings.disablePostProcessingEffects || settings.tonemapperSharpen >= 0.0f;
            value = settings.tonemapperSharpen >= 0.0f ? settings.tonemapperSharpen : 0.0;
            break;
        case Activation::InternalFpsLimit:
            enabled = settings.internalFpsLimit >= 0.0f;
            value = enabled ? settings.internalFpsLimit : 0.0;
            break;
        case Activation::InternalAnisotropicFiltering:
            enabled = settings.internalAnisotropicFiltering != 0;
            value = settings.internalAnisotropicFiltering;
            break;
        case Activation::InternalTextureMipBias:
            enabled = IsTextureMipBiasRequested(settings.internalTextureMipBias);
            value = enabled ? settings.internalTextureMipBias : 0.0;
            break;
        case Activation::DisplayGammaOutputDevice:
            // Pinning an SDR output device while the user is also asking for HDR
            // output would be CE arguing with itself, so the HDR request wins.
            enabled = IsDisplayGammaRequested(settings.displayGamma) &&
                      IsDisplayGammaPiecewiseSrgb(settings.displayGamma) &&
                      !IsToggleOn(settings.hdrOutput);
            value = 0.0;  // EDisplayOutputFormat::SDR_sRGB
            break;
        case Activation::DisplayGammaExponent:
            enabled = IsDisplayGammaRequested(settings.displayGamma);
            value = enabled ? settings.displayGamma : 0.0;
            break;
        case Activation::DepthOfField:
            enabled = IsToggleRequested(settings.depthOfField);
            value = IsToggleOn(settings.depthOfField) ? kDepthOfFieldDefaultQuality
                                                      : kDepthOfFieldOffQuality;
            break;
        case Activation::DlssSuperResolutionEnable:
            enabled = IsToggleRequested(settings.dlssSuperResolution);
            value = IsToggleOn(settings.dlssSuperResolution) ? 1.0 : 0.0;
            break;
        case Activation::DlssSuperResolutionForceOn:
            enabled = IsToggleOn(settings.dlssSuperResolution);
            break;
        case Activation::DlssSuperResolutionScreenPercentage:
            enabled = IsToggleOn(settings.dlssSuperResolution) &&
                      IsDlssScreenPercentageRequested(settings.dlssScreenPercentage);
            value = enabled ? settings.dlssScreenPercentage : kDlssScreenPercentageMax;
            break;
        case Activation::HdrOutput:
            enabled = IsToggleRequested(settings.hdrOutput);
            value = IsToggleOn(settings.hdrOutput) ? 1.0 : 0.0;
            break;
        case Activation::HdrPeakLuminance:
            enabled = IsHdrPeakLuminanceRequested(settings.hdrPeakLuminance);
            value = enabled ? settings.hdrPeakLuminance : 0.0;
            break;
        case Activation::HdrPaperWhite:
            enabled = IsHdrPaperWhiteRequested(settings.hdrPaperWhite);
            value = enabled ? settings.hdrPaperWhite : 0.0;
            break;
        case Activation::HdrUiLuminance:
            enabled = IsHdrUiLuminanceRequested(settings.hdrUiLuminance);
            value = enabled ? settings.hdrUiLuminance : 0.0;
            break;
        case Activation::HdrMinLuminance:
            // The engine stores this one as a log10 nit level, so the configured
            // nits are converted here rather than asking the user for a logarithm.
            enabled = IsHdrMinLuminanceRequested(settings.hdrMinLuminance);
            value = enabled ? std::log10(static_cast<double>(settings.hdrMinLuminance)) : 0.0;
            break;
        case Activation::HdrColorGamut:
            enabled = IsHdrColorGamutRequested(settings.hdrColorGamut);
            value = enabled ? settings.hdrColorGamut : 0.0;
            break;
    }
    const std::size_t specIndex = FindSpecIndex(spec.name ? spec.name : "");
    if (specIndex < kSpecs.size() && (settings.customCVarOverrideMask & (uint64_t{1} << specIndex)))
        return {true, settings.customCVarOverrideValues[specIndex]};
    return {enabled, ValueBits(spec.type, value)};
}

inline bool AnyEnabled(const Settings& settings) noexcept {
    return settings.forceRayReconstruction ||
           settings.rayReconstructionOptimalSettings != kRayReconstructionPresetOff ||
           settings.customCVarOverrideMask != 0 ||
           settings.disablePostProcessingEffects || settings.tonemapperSharpen >= 0.0f ||
           settings.internalFpsLimit >= 0.0f || settings.internalAnisotropicFiltering != 0 ||
           IsTextureMipBiasRequested(settings.internalTextureMipBias) ||
           IsDisplayGammaRequested(settings.displayGamma) ||
           IsToggleRequested(settings.depthOfField) ||
           IsToggleRequested(settings.dlssSuperResolution) ||
           IsToggleRequested(settings.hdrOutput) ||
           IsHdrPeakLuminanceRequested(settings.hdrPeakLuminance) ||
           IsHdrPaperWhiteRequested(settings.hdrPaperWhite) ||
           IsHdrUiLuminanceRequested(settings.hdrUiLuminance) ||
           IsHdrMinLuminanceRequested(settings.hdrMinLuminance) ||
           IsHdrColorGamutRequested(settings.hdrColorGamut);
}

// UE registers the show flag console variables as `FConsoleVariableBitRef` -
// one bit in two process-wide force masks - rather than as a variable owning a
// value. They are the only names in this table with that layout, so the bit
// handling stays confined to them instead of being offered to every object.
inline constexpr char kShowFlagPrefix[] = "ShowFlag.";

inline bool IsShowFlagSpec(std::size_t specIndex) noexcept {
    return specIndex < kSpecs.size() && kSpecs[specIndex].name &&
           std::strncmp(kSpecs[specIndex].name, kShowFlagPrefix, sizeof(kShowFlagPrefix) - 1) == 0;
}

// Whether the value the game currently holds permits this override at all.
// Evaluated at install time, where the observed value is known, so a refusal
// leaves game memory untouched rather than being undone afterwards.
inline bool MayApplyOverObservedValue(const Spec& spec, uint32_t observedBits) noexcept {
    if (spec.guard == ApplyGuard::Always)
        return true;
    const int32_t observed = static_cast<int32_t>(observedBits);
    return observed >= 0 && observed <= 2;
}

inline bool IsPlausibleShadowValue(std::size_t specIndex, uint32_t bits) noexcept {
    if (specIndex == kDenoiserModeIndex) {
        const int32_t value = static_cast<int32_t>(bits);
        return value == 0 || value == 1;
    }
    const Spec& spec = kSpecs[specIndex];
    if (spec.type == ValueType::Float) {
        const float value = std::bit_cast<float>(bits);
        return std::isfinite(value) && (value == 0.0f || std::fpclassify(value) == FP_NORMAL) &&
               std::abs(value) <= 1000000000.0f;
    }
    const int32_t value = static_cast<int32_t>(bits);
    return value >= -1000000 && value <= 1000000;
}

}  // namespace ce::ue5_cvar
