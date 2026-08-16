#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <string_view>

#include "../hook/common/ue5_cvar_override_policy.h"

namespace {

const ce::ue5_cvar::Spec* FindSpec(std::string_view name) {
    for (const auto& spec : ce::ue5_cvar::kSpecs) {
        if (name == spec.name)
            return &spec;
    }
    return nullptr;
}

}  // namespace

TEST(UE5CVarOverridePolicyTest, ContainsCompleteRayReconstructionOptimalBundle) {
    struct Expected {
        std::string_view name;
        ce::ue5_cvar::ValueType type;
        double value;
    };
    constexpr Expected expected[] = {
        {"r.NGX.DLSS.DenoiserMode", ce::ue5_cvar::ValueType::Int32, 1.0},
        {"r.Lumen.Reflections.BilateralFilter", ce::ue5_cvar::ValueType::Int32, 0.0},
        {"r.Lumen.Reflections.ScreenSpaceReconstruction", ce::ue5_cvar::ValueType::Int32, 0.0},
        {"r.Lumen.Reflections.Temporal", ce::ue5_cvar::ValueType::Int32, 0.0},
        {"r.Lumen.Reflections.DownsampleFactor", ce::ue5_cvar::ValueType::Int32, 1.0},
        {"r.Lumen.Reflections.DownsampleCheckerboard", ce::ue5_cvar::ValueType::Int32, 0.0},
        {"r.Lumen.Reflections.MaxRayIntensity", ce::ue5_cvar::ValueType::Float, 100.0},
        {"r.Lumen.ScreenProbeGather.StochasticInterpolation", ce::ue5_cvar::ValueType::Int32, 0.0},
        {"r.Lumen.ScreenProbeGather.SpatialFilterProbes", ce::ue5_cvar::ValueType::Int32, 1.0},
        {"r.Lumen.ScreenProbeGather.SpatialFilterNumPasses", ce::ue5_cvar::ValueType::Int32, 3.0},
        // Float in the engine, not int: Talos's console object holds 25.0f.
        {"r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated", ce::ue5_cvar::ValueType::Float, 10.0},
        {"r.Lumen.ScreenProbeGather.Temporal.MaxRayDirections", ce::ue5_cvar::ValueType::Int32, 8.0},
        {"r.Lumen.ScreenProbeGather.Temporal.RejectBasedOnNormal", ce::ue5_cvar::ValueType::Int32, 0.0},
        {"r.Lumen.ScreenProbeGather.Temporal.FastUpdateModeUseNeighborhoodClamp", ce::ue5_cvar::ValueType::Int32, 0.0},
        {"r.Lumen.ScreenProbeGather.TracingOctahedronResolution", ce::ue5_cvar::ValueType::Int32, 16.0},
        {"r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget", ce::ue5_cvar::ValueType::Int32, 600.0},
        {"r.Lumen.ScreenProbeGather.RadianceCache.ProbeResolution", ce::ue5_cvar::ValueType::Int32, 32.0},
        {"r.Lumen.ScreenProbeGather.ShortRangeAO.ApplyDuringIntegration", ce::ue5_cvar::ValueType::Int32, 0.0},
        {"r.LumenScene.Radiosity.Temporal.MaxFramesAccumulated", ce::ue5_cvar::ValueType::Int32, 4.0},
        {"r.LumenScene.Radiosity.UpdateFactor", ce::ue5_cvar::ValueType::Int32, 16.0},
        {"r.LumenScene.DirectLighting.UpdateFactor", ce::ue5_cvar::ValueType::Int32, 16.0},
        {"r.Shadow.Virtual.SMRT.RayCountDirectional", ce::ue5_cvar::ValueType::Int32, 12.0},
        {"r.Shadow.Virtual.SMRT.SamplesPerRayDirectional", ce::ue5_cvar::ValueType::Int32, 4.0},
        {"r.Shadow.Virtual.SMRT.RayCountLocal", ce::ue5_cvar::ValueType::Int32, 12.0},
        {"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", ce::ue5_cvar::ValueType::Int32, 4.0},
        {"r.Shadow.Virtual.ResolutionLodBiasLocal", ce::ue5_cvar::ValueType::Float, -0.5},
        {"r.Shadow.Virtual.ResolutionLodBiasLocalMoving", ce::ue5_cvar::ValueType::Float, 0.5},
        {"r.MegaLights.DownsampleMode", ce::ue5_cvar::ValueType::Int32, 0.0},
        {"r.MegaLights.NumSamplesPerPixel", ce::ue5_cvar::ValueType::Int32, 8.0},
    };
    for (const auto& item : expected) {
        const auto* spec = FindSpec(item.name);
        ASSERT_NE(spec, nullptr) << item.name;
        EXPECT_EQ(spec->type, item.type) << item.name;
        EXPECT_DOUBLE_EQ(spec->value, item.value) << item.name;
    }
}

TEST(UE5CVarOverridePolicyTest, PostProcessingBundleUsesDedicatedControlsAndShowFlags) {
    for (std::string_view name : {"r.Tonemapper.Sharpen", "r.FilmGrain",
                                  "r.Tonemapper.GrainQuantization", "r.MotionBlurQuality",
                                  "r.SceneColorFringeQuality", "ShowFlag.Vignette", "ShowFlag.Grain",
                                  "ShowFlag.MotionBlur", "ShowFlag.SceneColorFringe"}) {
        const auto* spec = FindSpec(name);
        ASSERT_NE(spec, nullptr) << name;
        EXPECT_DOUBLE_EQ(spec->value, 0.0) << name;
    }
    EXPECT_EQ(FindSpec("r.Tonemapper.Quality"), nullptr)
        << "vignette removal must not lower the game's tonemapper quality";
    EXPECT_EQ(FindSpec("ShowFlag.PostProcessMaterial"), nullptr)
        << "unrelated game-authored post-process materials must remain enabled";
}

TEST(UE5CVarOverridePolicyTest, OptimalBundleAlsoSelectsRayReconstruction) {
    ce::ue5_cvar::Settings settings;
    settings.rayReconstructionOptimalSettings = true;
    const auto denoiser = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[ce::ue5_cvar::kDenoiserModeIndex], settings);
    EXPECT_TRUE(denoiser.enabled);
    EXPECT_EQ(static_cast<int32_t>(denoiser.bits), 1);
}

TEST(UE5CVarOverridePolicyTest, CustomSharpenTakesPrecedenceOverDisableBundle) {
    ce::ue5_cvar::Settings settings;
    settings.disablePostProcessingEffects = true;
    auto sharpen = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[ce::ue5_cvar::kTonemapperSharpenIndex], settings);
    ASSERT_TRUE(sharpen.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(sharpen.bits), 0.0f);

    settings.tonemapperSharpen = 0.65f;
    sharpen = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[ce::ue5_cvar::kTonemapperSharpenIndex], settings);
    ASSERT_TRUE(sharpen.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(sharpen.bits), 0.65f);

    settings.disablePostProcessingEffects = false;
    sharpen = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[ce::ue5_cvar::kTonemapperSharpenIndex], settings);
    EXPECT_TRUE(sharpen.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(sharpen.bits), 0.65f);
}

TEST(UE5CVarOverridePolicyTest, DefaultSettingsRequestNoMemoryOverrides) {
    const ce::ue5_cvar::Settings settings;
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings));
    for (const auto& spec : ce::ue5_cvar::kSpecs)
        EXPECT_FALSE(ce::ue5_cvar::Resolve(spec, settings).enabled) << spec.name;
}

TEST(UE5CVarOverridePolicyTest, ContainsInternalFpsLimitAndAnisotropicFilteringSpecs) {
    const auto* maxFps = FindSpec("t.MaxFPS");
    ASSERT_NE(maxFps, nullptr);
    EXPECT_EQ(maxFps->type, ce::ue5_cvar::ValueType::Float);
    EXPECT_EQ(maxFps->activation, ce::ue5_cvar::Activation::InternalFpsLimit);

    const auto* maxAnisotropy = FindSpec("r.MaxAnisotropy");
    ASSERT_NE(maxAnisotropy, nullptr);
    EXPECT_EQ(maxAnisotropy->type, ce::ue5_cvar::ValueType::Int32);
    EXPECT_EQ(maxAnisotropy->activation, ce::ue5_cvar::Activation::InternalAnisotropicFiltering);

    const auto* vtMaxAnisotropy = FindSpec("r.VT.MaxAnisotropy");
    ASSERT_NE(vtMaxAnisotropy, nullptr);
    EXPECT_EQ(vtMaxAnisotropy->type, ce::ue5_cvar::ValueType::Int32);
    EXPECT_EQ(vtMaxAnisotropy->activation, ce::ue5_cvar::Activation::InternalAnisotropicFiltering);
}

TEST(UE5CVarOverridePolicyTest, InternalFpsLimitResolution) {
    const auto* maxFps = FindSpec("t.MaxFPS");
    ASSERT_NE(maxFps, nullptr);

    const ce::ue5_cvar::Settings defaultSettings;
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*maxFps, defaultSettings).enabled);

    ce::ue5_cvar::Settings settings;
    settings.internalFpsLimit = 0.0f;
    auto resolved = ce::ue5_cvar::Resolve(*maxFps, settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(resolved.bits), 0.0f);

    settings.internalFpsLimit = 60.0f;
    resolved = ce::ue5_cvar::Resolve(*maxFps, settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(resolved.bits), 60.0f);

    settings.internalFpsLimit = 59.94f;
    resolved = ce::ue5_cvar::Resolve(*maxFps, settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(resolved.bits), 59.94f);
}

TEST(UE5CVarOverridePolicyTest, InternalAnisotropicFilteringAppliesToBothSpecs) {
    const auto* maxAnisotropy = FindSpec("r.MaxAnisotropy");
    const auto* vtMaxAnisotropy = FindSpec("r.VT.MaxAnisotropy");
    ASSERT_NE(maxAnisotropy, nullptr);
    ASSERT_NE(vtMaxAnisotropy, nullptr);

    const ce::ue5_cvar::Settings defaultSettings;
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*maxAnisotropy, defaultSettings).enabled);
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*vtMaxAnisotropy, defaultSettings).enabled);

    for (int32_t level : {1, 2, 4, 8, 16}) {
        ce::ue5_cvar::Settings settings;
        settings.internalAnisotropicFiltering = level;
        const auto resolvedR = ce::ue5_cvar::Resolve(*maxAnisotropy, settings);
        const auto resolvedVt = ce::ue5_cvar::Resolve(*vtMaxAnisotropy, settings);
        ASSERT_TRUE(resolvedR.enabled) << level;
        ASSERT_TRUE(resolvedVt.enabled) << level;
        EXPECT_EQ(static_cast<int32_t>(resolvedR.bits), level) << level;
        EXPECT_EQ(static_cast<int32_t>(resolvedVt.bits), level) << level;
    }
}

TEST(UE5CVarOverridePolicyTest, InternalOverridesAreIndependentlySelectable) {
    ce::ue5_cvar::Settings settings;
    settings.internalFpsLimit = 144.0f;
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings));
    settings.internalFpsLimit = -1.0f;
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings));
    settings.internalAnisotropicFiltering = 8;
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings));
    settings.internalAnisotropicFiltering = 0;
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings));
}
