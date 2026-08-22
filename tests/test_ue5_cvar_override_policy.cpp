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
        {"r.SSR.Temporal", ce::ue5_cvar::ValueType::Int32, 0.0},
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

TEST(UE5CVarOverridePolicyTest, RayReconstructionSettingsLevelsAreNestedWithoutSelectingDenoiserMode) {
    ce::ue5_cvar::Settings settings;
    const auto countEnabled = [&settings]() {
        std::size_t count = 0;
        for (const auto& spec : ce::ue5_cvar::kSpecs)
            count += ce::ue5_cvar::Resolve(spec, settings).enabled ? 1u : 0u;
        return count;
    };
    settings.rayReconstructionOptimalSettings = ce::ue5_cvar::kRayReconstructionPresetFull;
    const auto denoiser = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[ce::ue5_cvar::kDenoiserModeIndex], settings);
    EXPECT_FALSE(denoiser.enabled);
    EXPECT_EQ(countEnabled(), 29u);

    const auto* bilateral = FindSpec("r.Lumen.Reflections.BilateralFilter");
    const auto* ssrTemporal = FindSpec("r.SSR.Temporal");
    const auto* downsample = FindSpec("r.Lumen.Reflections.DownsampleFactor");
    const auto* maxIntensity = FindSpec("r.Lumen.Reflections.MaxRayIntensity");
    ASSERT_NE(bilateral, nullptr);
    ASSERT_NE(ssrTemporal, nullptr);
    ASSERT_NE(downsample, nullptr);
    ASSERT_NE(maxIntensity, nullptr);

    settings.rayReconstructionOptimalSettings = ce::ue5_cvar::kRayReconstructionPresetLight;
    EXPECT_EQ(countEnabled(), 4u);
    EXPECT_TRUE(ce::ue5_cvar::Resolve(*bilateral, settings).enabled);
    EXPECT_TRUE(ce::ue5_cvar::Resolve(*ssrTemporal, settings).enabled);
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*downsample, settings).enabled);
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*maxIntensity, settings).enabled);

    settings.rayReconstructionOptimalSettings = ce::ue5_cvar::kRayReconstructionPresetMedium;
    EXPECT_EQ(countEnabled(), 5u);
    EXPECT_TRUE(ce::ue5_cvar::Resolve(*downsample, settings).enabled);
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*maxIntensity, settings).enabled);

    settings.rayReconstructionOptimalSettings = ce::ue5_cvar::kRayReconstructionPresetFull;
    EXPECT_TRUE(ce::ue5_cvar::Resolve(*maxIntensity, settings).enabled);

    settings.rayReconstructionOptimalSettings = ce::ue5_cvar::kRayReconstructionPresetOff;
    settings.forceRayReconstruction = true;
    const auto forcedDenoiser =
        ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[ce::ue5_cvar::kDenoiserModeIndex], settings);
    ASSERT_TRUE(forcedDenoiser.enabled);
    EXPECT_EQ(static_cast<int32_t>(forcedDenoiser.bits), 1);
}

TEST(UE5CVarOverridePolicyTest, CustomCVarValueHasFinalPrecedence) {
    const std::size_t sharpenIndex = ce::ue5_cvar::FindSpecIndex("tonemapper_sharpen");
    const std::size_t temporalIndex = ce::ue5_cvar::FindSpecIndex("R.SSR.TEMPORAL");
    ASSERT_EQ(sharpenIndex, ce::ue5_cvar::kTonemapperSharpenIndex);
    ASSERT_LT(temporalIndex, ce::ue5_cvar::kSpecs.size());

    ce::ue5_cvar::Settings settings;
    settings.disablePostProcessingEffects = true;
    settings.rayReconstructionOptimalSettings = ce::ue5_cvar::kRayReconstructionPresetFull;
    settings.customCVarOverrideMask = (uint64_t{1} << sharpenIndex) | (uint64_t{1} << temporalIndex);
    settings.customCVarOverrideValues[sharpenIndex] = std::bit_cast<uint32_t>(0.5f);
    settings.customCVarOverrideValues[temporalIndex] = 1;

    const auto sharpen = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[sharpenIndex], settings);
    const auto temporal = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[temporalIndex], settings);
    ASSERT_TRUE(sharpen.enabled);
    ASSERT_TRUE(temporal.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(sharpen.bits), 0.5f);
    EXPECT_EQ(static_cast<int32_t>(temporal.bits), 1);

    settings.disablePostProcessingEffects = false;
    settings.rayReconstructionOptimalSettings = ce::ue5_cvar::kRayReconstructionPresetOff;
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings));
    EXPECT_TRUE(ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[temporalIndex], settings).enabled)
        << "a custom entry must work without any preset enabling the same CVar";
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

// UE's texture mip bias is the one numeric UE5 knob where 0 is a real setting
// ("no bias") rather than "leave the engine alone", so the untouched state is a
// sentinel outside UE's accepted -15..15 range. It is also a float: the Talos
// 5.4.4 registration passes its default in xmm2, and typing it as an int would
// write denormal garbage into the engine's global.
TEST(UE5CVarOverridePolicyTest, InternalTextureMipBiasIsAFloatWithZeroAsARealValue) {
    const auto* spec = FindSpec("r.MipMapLODBias");
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->type, ce::ue5_cvar::ValueType::Float);
    EXPECT_EQ(spec->activation, ce::ue5_cvar::Activation::InternalTextureMipBias);

    const ce::ue5_cvar::Settings defaults;
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, defaults).enabled);
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(defaults));

    ce::ue5_cvar::Settings settings;
    settings.internalTextureMipBias = 0.0f;
    auto resolved = ce::ue5_cvar::Resolve(*spec, settings);
    ASSERT_TRUE(resolved.enabled) << "0 must be applied, not treated as unset";
    EXPECT_FLOAT_EQ(std::bit_cast<float>(resolved.bits), 0.0f);
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings));

    for (float bias : {-15.0f, -1.5f, -1.0f, 1.0f, 15.0f}) {
        settings.internalTextureMipBias = bias;
        resolved = ce::ue5_cvar::Resolve(*spec, settings);
        ASSERT_TRUE(resolved.enabled) << bias;
        EXPECT_FLOAT_EQ(std::bit_cast<float>(resolved.bits), bias);
    }

    for (float outside : {-15.001f, 15.001f, ce::ue5_cvar::kTextureMipBiasDisabled}) {
        settings.internalTextureMipBias = outside;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, settings).enabled) << outside;
    }
}

// The two positional indices are the reason new specs get appended rather than
// inserted; this pins them so a future entry cannot silently retarget them.
TEST(UE5CVarOverridePolicyTest, PositionalSpecIndicesStillNameTheirCVars) {
    EXPECT_STREQ(ce::ue5_cvar::kSpecs[ce::ue5_cvar::kDenoiserModeIndex].name, "r.NGX.DLSS.DenoiserMode");
    EXPECT_STREQ(ce::ue5_cvar::kSpecs[ce::ue5_cvar::kTonemapperSharpenIndex].name, "r.Tonemapper.Sharpen");
}

// The display gamma request is carried as the r.TonemapperGamma value itself:
// negative untouched, 0 UE's piecewise sRGB/Rec709 transform, 1.0..3.0 a pure
// power curve. Only the sRGB direction touches the output device, because UE
// raises the device to explicit-gamma mapping by itself once the exponent is
// positive.
TEST(UE5CVarOverridePolicyTest, DisplayGammaDrivesExponentAlwaysAndDeviceOnlyForSrgb) {
    const auto* device = FindSpec("r.HDR.Display.OutputDevice");
    const auto* exponent = FindSpec("r.TonemapperGamma");
    ASSERT_NE(device, nullptr);
    ASSERT_NE(exponent, nullptr);
    EXPECT_EQ(device->type, ce::ue5_cvar::ValueType::Int32);
    EXPECT_EQ(exponent->type, ce::ue5_cvar::ValueType::Float);

    const ce::ue5_cvar::Settings defaults;
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*device, defaults).enabled);
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*exponent, defaults).enabled);
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(defaults));

    ce::ue5_cvar::Settings srgb;
    srgb.displayGamma = ce::ue5_cvar::kDisplayGammaSrgb;
    auto resolvedDevice = ce::ue5_cvar::Resolve(*device, srgb);
    ASSERT_TRUE(resolvedDevice.enabled);
    EXPECT_EQ(static_cast<int32_t>(resolvedDevice.bits), 0);  // SDR_sRGB
    auto resolvedExponent = ce::ue5_cvar::Resolve(*exponent, srgb);
    ASSERT_TRUE(resolvedExponent.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(resolvedExponent.bits), 0.0f);

    ce::ue5_cvar::Settings power;
    power.displayGamma = 2.2f;
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*device, power).enabled)
        << "the pure-power direction must not select an output device";
    resolvedExponent = ce::ue5_cvar::Resolve(*exponent, power);
    ASSERT_TRUE(resolvedExponent.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(resolvedExponent.bits), 2.2f);

    for (float outside : {-0.5f, 0.99f, 3.01f, ce::ue5_cvar::kDisplayGammaDefault}) {
        ce::ue5_cvar::Settings rejected;
        rejected.displayGamma = outside;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*exponent, rejected).enabled) << outside;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*device, rejected).enabled) << outside;
    }
}

// The output-device write must never pull a game out of an HDR device. The guard
// is judged from the value the game currently holds, before anything is written.
TEST(UE5CVarOverridePolicyTest, OutputDeviceOverrideIsRefusedOnHdrDevices) {
    const auto* device = FindSpec("r.HDR.Display.OutputDevice");
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->guard, ce::ue5_cvar::ApplyGuard::SdrOutputDeviceOnly);

    for (int32_t sdr : {0, 1, 2}) {
        EXPECT_TRUE(ce::ue5_cvar::MayApplyOverObservedValue(*device, static_cast<uint32_t>(sdr))) << sdr;
    }
    // 3-6 ST-2084/ScRGB HDR, 7-9 linear: all left alone.
    for (int32_t hdr : {3, 4, 5, 6, 7, 8, 9}) {
        EXPECT_FALSE(ce::ue5_cvar::MayApplyOverObservedValue(*device, static_cast<uint32_t>(hdr))) << hdr;
    }
    EXPECT_FALSE(ce::ue5_cvar::MayApplyOverObservedValue(*device, static_cast<uint32_t>(-1)));

    // Everything else stays unconditional.
    const auto* mipBias = FindSpec("r.MipMapLODBias");
    ASSERT_NE(mipBias, nullptr);
    EXPECT_EQ(mipBias->guard, ce::ue5_cvar::ApplyGuard::Always);
    EXPECT_TRUE(ce::ue5_cvar::MayApplyOverObservedValue(*mipBias, 0x41C80000));
}

// Depth of field is a plain quality CVar: 0 is the engine's own "Off", and the
// on direction restores UE's registered default (2) rather than inventing a
// quality level the game never used.
TEST(UE5CVarOverridePolicyTest, DepthOfFieldTogglesTheEngineQualityCVar) {
    const auto* spec = FindSpec("r.DepthOfFieldQuality");
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->type, ce::ue5_cvar::ValueType::Int32);
    EXPECT_EQ(spec->activation, ce::ue5_cvar::Activation::DepthOfField);

    const ce::ue5_cvar::Settings defaults;
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, defaults).enabled);
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(defaults));

    ce::ue5_cvar::Settings off;
    off.depthOfField = ce::ue5_cvar::kToggleOff;
    auto resolved = ce::ue5_cvar::Resolve(*spec, off);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_EQ(static_cast<int32_t>(resolved.bits), ce::ue5_cvar::kDepthOfFieldOffQuality);
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(off));

    ce::ue5_cvar::Settings on;
    on.depthOfField = ce::ue5_cvar::kToggleOn;
    resolved = ce::ue5_cvar::Resolve(*spec, on);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_EQ(static_cast<int32_t>(resolved.bits), ce::ue5_cvar::kDepthOfFieldDefaultQuality);

    // A value that is neither off nor on cannot be turned into a direction.
    for (int32_t bogus : {-2, 2, 7}) {
        ce::ue5_cvar::Settings rejected;
        rejected.depthOfField = bogus;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, rejected).enabled) << bogus;
        EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(rejected)) << bogus;
    }
}

// Turning DLSS SR off is the plugin's own switch and nothing else. Turning it on
// also has to move the engine levers that decide whether a third-party temporal
// upscaler runs at all, which is exactly what a game that hides DLSS never does.
TEST(UE5CVarOverridePolicyTest, DlssSuperResolutionForcesTheThirdPartyUpscalerPathOnlyWhenOn) {
    const auto* enable = FindSpec("r.NGX.DLSS.Enable");
    ASSERT_NE(enable, nullptr);
    EXPECT_EQ(enable->type, ce::ue5_cvar::ValueType::Int32);
    EXPECT_EQ(enable->activation, ce::ue5_cvar::Activation::DlssSuperResolutionEnable);

    struct ForceOnSpec {
        std::string_view name;
        int32_t value;
    };
    constexpr ForceOnSpec forceOn[] = {
        {"r.NGX.Enable", 1},
        {"r.TemporalAA.Upscaler", 1},
        // AAM_TemporalAA: UE only offers the third-party upscaler on the TAA path.
        {"r.AntiAliasingMethod", 2},
    };

    ce::ue5_cvar::Settings off;
    off.dlssSuperResolution = ce::ue5_cvar::kToggleOff;
    auto resolved = ce::ue5_cvar::Resolve(*enable, off);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_EQ(static_cast<int32_t>(resolved.bits), 0);
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(off));

    ce::ue5_cvar::Settings on;
    on.dlssSuperResolution = ce::ue5_cvar::kToggleOn;
    resolved = ce::ue5_cvar::Resolve(*enable, on);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_EQ(static_cast<int32_t>(resolved.bits), 1);

    for (const auto& item : forceOn) {
        const auto* spec = FindSpec(item.name);
        ASSERT_NE(spec, nullptr) << item.name;
        EXPECT_EQ(spec->type, ce::ue5_cvar::ValueType::Int32) << item.name;
        EXPECT_EQ(spec->activation, ce::ue5_cvar::Activation::DlssSuperResolutionForceOn) << item.name;

        EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, ce::ue5_cvar::Settings{}).enabled) << item.name;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, off).enabled)
            << item.name << " must not be touched while only disabling DLSS";
        const auto forced = ce::ue5_cvar::Resolve(*spec, on);
        ASSERT_TRUE(forced.enabled) << item.name;
        EXPECT_EQ(static_cast<int32_t>(forced.bits), item.value) << item.name;
    }
}

TEST(UE5CVarOverridePolicyTest, DlssScreenPercentageOnlyAppliesWhileSuperResolutionIsForcedOn) {
    const auto* spec = FindSpec("r.ScreenPercentage");
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->type, ce::ue5_cvar::ValueType::Float);
    EXPECT_EQ(spec->activation, ce::ue5_cvar::Activation::DlssSuperResolutionScreenPercentage);

    ce::ue5_cvar::Settings percentageWithoutSr;
    percentageWithoutSr.dlssScreenPercentage = 66.67f;
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, percentageWithoutSr).enabled)
        << "a resolution scale must never be forced on a game that did not ask for DLSS";
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(percentageWithoutSr));

    ce::ue5_cvar::Settings on;
    on.dlssSuperResolution = ce::ue5_cvar::kToggleOn;
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, on).enabled)
        << "forcing DLSS on without a quality mode leaves the game's own screen percentage";

    for (float percentage : {33.33f, 50.0f, 58.0f, 66.67f, 100.0f}) {
        on.dlssScreenPercentage = percentage;
        const auto resolved = ce::ue5_cvar::Resolve(*spec, on);
        ASSERT_TRUE(resolved.enabled) << percentage;
        EXPECT_FLOAT_EQ(std::bit_cast<float>(resolved.bits), percentage);
    }

    for (float outside : {0.0f, 24.99f, 100.01f, -50.0f}) {
        on.dlssScreenPercentage = outside;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, on).enabled) << outside;
    }
}

// Every HDR knob is written in the unit the engine's own help text names, and
// the black floor is the one that is configured in nits but stored as a log10
// level, so the conversion belongs here rather than in the user's config file.
TEST(UE5CVarOverridePolicyTest, HdrOverridesUseTheEngineUnits) {
    const auto* output = FindSpec("r.HDR.EnableHDROutput");
    const auto* peak = FindSpec("r.HDR.Display.MaxLuminance");
    const auto* paperWhite = FindSpec("r.HDR.Display.MidLuminance");
    const auto* uiLuminance = FindSpec("r.HDR.UI.Luminance");
    const auto* minLuminance = FindSpec("r.HDR.Display.MinLuminanceLog10");
    const auto* gamut = FindSpec("r.HDR.Display.ColorGamut");
    ASSERT_NE(output, nullptr);
    ASSERT_NE(peak, nullptr);
    ASSERT_NE(paperWhite, nullptr);
    ASSERT_NE(uiLuminance, nullptr);
    ASSERT_NE(minLuminance, nullptr);
    ASSERT_NE(gamut, nullptr);
    // Measured from the UE5 registrations: the int32 registration passes its
    // default in r8d, the float one in xmm2.
    EXPECT_EQ(output->type, ce::ue5_cvar::ValueType::Int32);
    EXPECT_EQ(peak->type, ce::ue5_cvar::ValueType::Int32);
    EXPECT_EQ(gamut->type, ce::ue5_cvar::ValueType::Int32);
    EXPECT_EQ(paperWhite->type, ce::ue5_cvar::ValueType::Float);
    EXPECT_EQ(uiLuminance->type, ce::ue5_cvar::ValueType::Float);
    EXPECT_EQ(minLuminance->type, ce::ue5_cvar::ValueType::Float);

    const ce::ue5_cvar::Settings defaults;
    for (const auto* spec : {output, peak, paperWhite, uiLuminance, minLuminance, gamut})
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, defaults).enabled) << spec->name;

    ce::ue5_cvar::Settings settings;
    settings.hdrOutput = ce::ue5_cvar::kToggleOn;
    settings.hdrPeakLuminance = 1000;
    settings.hdrPaperWhite = 200.0f;
    settings.hdrUiLuminance = 300.0f;
    settings.hdrMinLuminance = 0.001f;
    settings.hdrColorGamut = 2;

    auto resolved = ce::ue5_cvar::Resolve(*output, settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_EQ(static_cast<int32_t>(resolved.bits), 1);
    resolved = ce::ue5_cvar::Resolve(*peak, settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_EQ(static_cast<int32_t>(resolved.bits), 1000);
    resolved = ce::ue5_cvar::Resolve(*paperWhite, settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(resolved.bits), 200.0f);
    resolved = ce::ue5_cvar::Resolve(*uiLuminance, settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(resolved.bits), 300.0f);
    resolved = ce::ue5_cvar::Resolve(*minLuminance, settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(resolved.bits), -3.0f) << "0.001 nits is log10 -3";
    resolved = ce::ue5_cvar::Resolve(*gamut, settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_EQ(static_cast<int32_t>(resolved.bits), 2);

    ce::ue5_cvar::Settings hdrOff;
    hdrOff.hdrOutput = ce::ue5_cvar::kToggleOff;
    resolved = ce::ue5_cvar::Resolve(*output, hdrOff);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_EQ(static_cast<int32_t>(resolved.bits), 0);
}

TEST(UE5CVarOverridePolicyTest, HdrValuesOutsideTheAcceptedRangeLeaveTheEngineAlone) {
    const auto* peak = FindSpec("r.HDR.Display.MaxLuminance");
    const auto* paperWhite = FindSpec("r.HDR.Display.MidLuminance");
    const auto* uiLuminance = FindSpec("r.HDR.UI.Luminance");
    const auto* minLuminance = FindSpec("r.HDR.Display.MinLuminanceLog10");
    const auto* gamut = FindSpec("r.HDR.Display.ColorGamut");
    ASSERT_NE(peak, nullptr);
    ASSERT_NE(paperWhite, nullptr);
    ASSERT_NE(uiLuminance, nullptr);
    ASSERT_NE(minLuminance, nullptr);
    ASSERT_NE(gamut, nullptr);

    for (int32_t nits : {0, 79, 10001, -100}) {
        ce::ue5_cvar::Settings settings;
        settings.hdrPeakLuminance = nits;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*peak, settings).enabled) << nits;
        EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings)) << nits;
    }
    for (float nits : {0.0f, 19.99f, 1000.01f, -5.0f}) {
        ce::ue5_cvar::Settings settings;
        settings.hdrPaperWhite = nits;
        settings.hdrUiLuminance = nits;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*paperWhite, settings).enabled) << nits;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*uiLuminance, settings).enabled) << nits;
        EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings)) << nits;
    }
    for (float nits : {0.0f, 0.00009f, 10.01f, -1.0f}) {
        ce::ue5_cvar::Settings settings;
        settings.hdrMinLuminance = nits;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*minLuminance, settings).enabled) << nits;
        EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings)) << nits;
    }
    for (int32_t value : {-1, -2, 5, 9}) {
        ce::ue5_cvar::Settings settings;
        settings.hdrColorGamut = value;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*gamut, settings).enabled) << value;
        EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings)) << value;
    }
}

// CE must not argue with itself: display_gamma=srgb selects an SDR output device,
// so it has to stand down when the same configuration is asking the engine for
// HDR output. The exponent is unaffected - it is a different CVar.
TEST(UE5CVarOverridePolicyTest, ForcedHdrOutputSuppressesTheSrgbOutputDeviceWrite) {
    const auto* device = FindSpec("r.HDR.Display.OutputDevice");
    const auto* exponent = FindSpec("r.TonemapperGamma");
    ASSERT_NE(device, nullptr);
    ASSERT_NE(exponent, nullptr);

    ce::ue5_cvar::Settings srgb;
    srgb.displayGamma = ce::ue5_cvar::kDisplayGammaSrgb;
    EXPECT_TRUE(ce::ue5_cvar::Resolve(*device, srgb).enabled);

    ce::ue5_cvar::Settings withHdr = srgb;
    withHdr.hdrOutput = ce::ue5_cvar::kToggleOn;
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*device, withHdr).enabled);
    EXPECT_TRUE(ce::ue5_cvar::Resolve(*exponent, withHdr).enabled);

    // Explicitly disabling HDR output is not a reason to hold the write back.
    ce::ue5_cvar::Settings withHdrOff = srgb;
    withHdrOff.hdrOutput = ce::ue5_cvar::kToggleOff;
    EXPECT_TRUE(ce::ue5_cvar::Resolve(*device, withHdrOff).enabled);
}

TEST(UE5CVarOverridePolicyTest, NewOverridesAreIndependentlySelectable) {
    ce::ue5_cvar::Settings settings;
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings));

    settings.depthOfField = ce::ue5_cvar::kToggleOff;
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings));
    settings.depthOfField = ce::ue5_cvar::kToggleDefault;
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings));

    settings.dlssSuperResolution = ce::ue5_cvar::kToggleOn;
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings));
    settings.dlssSuperResolution = ce::ue5_cvar::kToggleDefault;
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings));

    settings.hdrOutput = ce::ue5_cvar::kToggleOn;
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings));
    settings.hdrOutput = ce::ue5_cvar::kToggleDefault;
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings));

    settings.hdrPaperWhite = 200.0f;
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings));
    settings.hdrPaperWhite = 0.0f;
    EXPECT_FALSE(ce::ue5_cvar::AnyEnabled(settings));

    settings.hdrColorGamut = 0;
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings))
        << "Rec709 is a real gamut selection, not the untouched state";
}

// Every spec name has to be unique: two entries sharing a CVar would race for the
// same console object, and the install path resolves by name.
TEST(UE5CVarOverridePolicyTest, SpecNamesAreUnique) {
    for (std::size_t outer = 0; outer < ce::ue5_cvar::kSpecs.size(); ++outer) {
        ASSERT_NE(ce::ue5_cvar::kSpecs[outer].name, nullptr) << outer;
        for (std::size_t inner = outer + 1; inner < ce::ue5_cvar::kSpecs.size(); ++inner) {
            EXPECT_STRNE(ce::ue5_cvar::kSpecs[outer].name, ce::ue5_cvar::kSpecs[inner].name)
                << outer << " vs " << inner;
        }
    }
}
