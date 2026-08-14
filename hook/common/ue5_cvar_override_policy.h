#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ce::ue5_cvar {

enum class ValueType : uint8_t {
    Int32,
    Float,
};

enum class Activation : uint8_t {
    RayReconstruction,
    RayReconstructionOptimal,
    DisablePostProcessing,
    TonemapperSharpen,
};

struct Settings {
    bool forceRayReconstruction = false;
    bool rayReconstructionOptimalSettings = false;
    bool disablePostProcessingEffects = false;
    float tonemapperSharpen = -1.0f;
};

struct Spec {
    const char* name = nullptr;
    ValueType type = ValueType::Int32;
    Activation activation = Activation::RayReconstructionOptimal;
    double value = 0.0;
};

struct ResolvedValue {
    bool enabled = false;
    uint32_t bits = 0;
};

inline constexpr std::array kSpecs{
    Spec{"r.NGX.DLSS.DenoiserMode", ValueType::Int32, Activation::RayReconstruction, 1.0},
    Spec{"r.Lumen.Reflections.BilateralFilter", ValueType::Int32,
         Activation::RayReconstructionOptimal, 0.0},
    Spec{"r.Lumen.Reflections.ScreenSpaceReconstruction", ValueType::Int32,
         Activation::RayReconstructionOptimal, 0.0},
    Spec{"r.Lumen.Reflections.Temporal", ValueType::Int32, Activation::RayReconstructionOptimal, 0.0},
    Spec{"r.Lumen.Reflections.DownsampleFactor", ValueType::Int32,
         Activation::RayReconstructionOptimal, 1.0},
    Spec{"r.Lumen.Reflections.DownsampleCheckerboard", ValueType::Int32,
         Activation::RayReconstructionOptimal, 0.0},
    Spec{"r.Lumen.Reflections.MaxRayIntensity", ValueType::Float,
         Activation::RayReconstructionOptimal, 100.0},
    Spec{"r.Lumen.ScreenProbeGather.StochasticInterpolation", ValueType::Int32,
         Activation::RayReconstructionOptimal, 0.0},
    Spec{"r.Lumen.ScreenProbeGather.SpatialFilterProbes", ValueType::Int32,
         Activation::RayReconstructionOptimal, 1.0},
    Spec{"r.Lumen.ScreenProbeGather.SpatialFilterNumPasses", ValueType::Int32,
         Activation::RayReconstructionOptimal, 3.0},
    Spec{"r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated", ValueType::Int32,
         Activation::RayReconstructionOptimal, 10.0},
    Spec{"r.Lumen.ScreenProbeGather.Temporal.MaxRayDirections", ValueType::Int32,
         Activation::RayReconstructionOptimal, 8.0},
    Spec{"r.Lumen.ScreenProbeGather.Temporal.RejectBasedOnNormal", ValueType::Int32,
         Activation::RayReconstructionOptimal, 0.0},
    Spec{"r.Lumen.ScreenProbeGather.Temporal.FastUpdateModeUseNeighborhoodClamp", ValueType::Int32,
         Activation::RayReconstructionOptimal, 0.0},
    Spec{"r.Lumen.ScreenProbeGather.TracingOctahedronResolution", ValueType::Int32,
         Activation::RayReconstructionOptimal, 16.0},
    Spec{"r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget", ValueType::Int32,
         Activation::RayReconstructionOptimal, 600.0},
    Spec{"r.Lumen.ScreenProbeGather.RadianceCache.ProbeResolution", ValueType::Int32,
         Activation::RayReconstructionOptimal, 32.0},
    Spec{"r.Lumen.ScreenProbeGather.ShortRangeAO.ApplyDuringIntegration", ValueType::Int32,
         Activation::RayReconstructionOptimal, 0.0},
    Spec{"r.LumenScene.Radiosity.Temporal.MaxFramesAccumulated", ValueType::Int32,
         Activation::RayReconstructionOptimal, 4.0},
    Spec{"r.LumenScene.Radiosity.UpdateFactor", ValueType::Int32,
         Activation::RayReconstructionOptimal, 16.0},
    Spec{"r.LumenScene.DirectLighting.UpdateFactor", ValueType::Int32,
         Activation::RayReconstructionOptimal, 16.0},
    Spec{"r.Shadow.Virtual.SMRT.RayCountDirectional", ValueType::Int32,
         Activation::RayReconstructionOptimal, 12.0},
    Spec{"r.Shadow.Virtual.SMRT.SamplesPerRayDirectional", ValueType::Int32,
         Activation::RayReconstructionOptimal, 4.0},
    Spec{"r.Shadow.Virtual.SMRT.RayCountLocal", ValueType::Int32,
         Activation::RayReconstructionOptimal, 12.0},
    Spec{"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", ValueType::Int32,
         Activation::RayReconstructionOptimal, 4.0},
    Spec{"r.Shadow.Virtual.ResolutionLodBiasLocal", ValueType::Float,
         Activation::RayReconstructionOptimal, -0.5},
    Spec{"r.Shadow.Virtual.ResolutionLodBiasLocalMoving", ValueType::Float,
         Activation::RayReconstructionOptimal, 0.5},
    Spec{"r.MegaLights.DownsampleMode", ValueType::Int32, Activation::RayReconstructionOptimal, 0.0},
    Spec{"r.MegaLights.NumSamplesPerPixel", ValueType::Int32,
         Activation::RayReconstructionOptimal, 8.0},
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
};

inline constexpr std::size_t kDenoiserModeIndex = 0;
inline constexpr std::size_t kTonemapperSharpenIndex = 29;

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
            enabled = settings.forceRayReconstruction || settings.rayReconstructionOptimalSettings;
            break;
        case Activation::RayReconstructionOptimal:
            enabled = settings.rayReconstructionOptimalSettings;
            break;
        case Activation::DisablePostProcessing:
            enabled = settings.disablePostProcessingEffects;
            break;
        case Activation::TonemapperSharpen:
            enabled = settings.disablePostProcessingEffects || settings.tonemapperSharpen >= 0.0f;
            value = settings.tonemapperSharpen >= 0.0f ? settings.tonemapperSharpen : 0.0;
            break;
    }
    return {enabled, ValueBits(spec.type, value)};
}

inline bool AnyEnabled(const Settings& settings) noexcept {
    return settings.forceRayReconstruction || settings.rayReconstructionOptimalSettings ||
           settings.disablePostProcessingEffects || settings.tonemapperSharpen >= 0.0f;
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
