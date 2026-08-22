#include "test_config_shared.h"

#include "../hook/common/ue5_cvar_override_policy.h"

#include <string>
#include <string_view>

TEST_F(ConfigTest, ParsesUE5OverridesAndLegacyRayReconstructionLocations) {
    WriteConfig("[UE5]\n"
                "force_ray_reconstruction=on\n"
                "disable_post_processing_effects=true\n"
                "tonemapper_sharpen=0.75\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_TRUE(config.graphics.forceRayReconstruction);
    EXPECT_FALSE(config.graphics.rayReconstructionOptimalSettings);
    EXPECT_TRUE(config.graphics.disablePostProcessingEffects);
    EXPECT_FLOAT_EQ(config.graphics.tonemapperSharpen, 0.75f);

    WriteConfig("[UE5]\nray_reconstruction_optimal_settings=full\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.rayReconstructionOptimalSettings,
              ce::ue5_cvar::kRayReconstructionPresetFull);
    EXPECT_FALSE(config.graphics.forceRayReconstruction)
        << "quality settings must not silently select the DLSS RR denoiser";

    WriteConfig("[DLSS]\nforce_ray_reconstruction=on\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_TRUE(config.graphics.forceRayReconstruction);

    WriteConfig("[Graphics]\nforce_ray_reconstruction=true\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_TRUE(config.graphics.forceRayReconstruction);

    WriteConfig("[DLSS]\nforce_ray_reconstruction=off\n"
                "[Graphics]\nforce_ray_reconstruction=true\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FALSE(config.graphics.forceRayReconstruction);

    WriteConfig("[UE5]\nforce_ray_reconstruction=on\n"
                "[DLSS]\nforce_ray_reconstruction=off\n"
                "[Graphics]\nforce_ray_reconstruction=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_TRUE(config.graphics.forceRayReconstruction);
}

TEST_F(ConfigTest, ParsesGraduatedRayReconstructionSettingsAndLegacyOnAsFull) {
    struct PresetCase {
        const char* value;
        uint8_t expected;
    };
    const PresetCase cases[] = {
        {"off", ce::ue5_cvar::kRayReconstructionPresetOff},
        {"light", ce::ue5_cvar::kRayReconstructionPresetLight},
        {"medium", ce::ue5_cvar::kRayReconstructionPresetMedium},
        {"full", ce::ue5_cvar::kRayReconstructionPresetFull},
        {"on", ce::ue5_cvar::kRayReconstructionPresetFull},
        {"true", ce::ue5_cvar::kRayReconstructionPresetFull},
        {"invalid", ce::ue5_cvar::kRayReconstructionPresetOff},
    };
    AppConfig config;
    for (const auto& preset : cases) {
        WriteConfig(std::string("[UE5]\nray_reconstruction_optimal_settings=") + preset.value + "\n");
        LoadConfig(tempConfigFile, config);
        EXPECT_EQ(config.graphics.rayReconstructionOptimalSettings, preset.expected) << preset.value;
        EXPECT_FALSE(config.graphics.forceRayReconstruction) << preset.value;
    }
}

TEST_F(ConfigTest, ParsesTypedCustomCVarOverridesAndIgnoresUnsupportedEntries) {
    WriteConfig("[UE5]\n"
                "custom_cvar_overrides=r.NGX.DLSS.denoisermode=1,r.SSR.Temporal=0,"
                "tonemapper_sharpen=0.5,not.supported=2,r.MaxAnisotropy=bad,r.SSR.Temporal=1\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);

    const std::size_t denoiser = ce::ue5_cvar::FindSpecIndex("r.NGX.DLSS.DenoiserMode");
    const std::size_t temporal = ce::ue5_cvar::FindSpecIndex("r.SSR.Temporal");
    const std::size_t sharpen = ce::ue5_cvar::FindSpecIndex("tonemapper_sharpen");
    const std::size_t anisotropy = ce::ue5_cvar::FindSpecIndex("r.MaxAnisotropy");
    ASSERT_LT(denoiser, ce::ue5_cvar::kSpecs.size());
    ASSERT_LT(temporal, ce::ue5_cvar::kSpecs.size());
    ASSERT_LT(sharpen, ce::ue5_cvar::kSpecs.size());
    ASSERT_LT(anisotropy, ce::ue5_cvar::kSpecs.size());
    EXPECT_NE(config.graphics.ue5CustomCVarOverrideMask & (uint64_t{1} << denoiser), 0u);
    EXPECT_NE(config.graphics.ue5CustomCVarOverrideMask & (uint64_t{1} << temporal), 0u);
    EXPECT_NE(config.graphics.ue5CustomCVarOverrideMask & (uint64_t{1} << sharpen), 0u);
    EXPECT_EQ(config.graphics.ue5CustomCVarOverrideMask & (uint64_t{1} << anisotropy), 0u);
    EXPECT_EQ(static_cast<int32_t>(config.graphics.ue5CustomCVarOverrideValues[denoiser]), 1);
    EXPECT_EQ(static_cast<int32_t>(config.graphics.ue5CustomCVarOverrideValues[temporal]), 1);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(config.graphics.ue5CustomCVarOverrideValues[sharpen]), 0.5f);
}

TEST_F(ConfigTest, RejectsInvalidUE5TonemapperSharpenStrength) {
    AppConfig config;
    WriteConfig("[UE5]\ntonemapper_sharpen=10\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.tonemapperSharpen, 10.0f);

    WriteConfig("[UE5]\ntonemapper_sharpen=-0.01\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.tonemapperSharpen, -1.0f);

    WriteConfig("[UE5]\ntonemapper_sharpen=10.01\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.tonemapperSharpen, -1.0f);

    WriteConfig("[UE5]\ntonemapper_sharpen=not-a-number\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.tonemapperSharpen, -1.0f);
}

TEST_F(ConfigTest, ParsesUE5InternalFpsLimitOverride) {
    AppConfig config;

    WriteConfig("[UE5]\ninternal_fps_limit=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, 0.0f);

    WriteConfig("[UE5]\ninternal_fps_limit=0\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, 0.0f);

    WriteConfig("[UE5]\ninternal_fps_limit=60\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, 60.0f);

    WriteConfig("[UE5]\ninternal_fps_limit=59.94\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, 59.94f);

    WriteConfig("[UE5]\ninternal_fps_limit=default\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, -1.0f);

    WriteConfig("[UE5]\ninternal_fps_limit=-1\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, -1.0f);

    WriteConfig("[UE5]\ninternal_fps_limit=1001\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, -1.0f);

    WriteConfig("[UE5]\ninternal_fps_limit=not-a-number\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, -1.0f);
}

TEST_F(ConfigTest, ParsesUE5InternalAnisotropicFilteringOverride) {
    AppConfig config;

    WriteConfig("[UE5]\ninternal_anisotropic_filtering=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 1);

    WriteConfig("[UE5]\ninternal_anisotropic_filtering=1x\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 1);

    WriteConfig("[UE5]\ninternal_anisotropic_filtering=2x\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 2);

    WriteConfig("[UE5]\ninternal_anisotropic_filtering=4x\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 4);

    WriteConfig("[UE5]\ninternal_anisotropic_filtering=8x\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 8);

    WriteConfig("[UE5]\ninternal_anisotropic_filtering=16x\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 16);

    WriteConfig("[UE5]\ninternal_anisotropic_filtering=default\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 0);

    WriteConfig("[UE5]\ninternal_anisotropic_filtering=3x\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 0);

    WriteConfig("[UE5]\ninternal_anisotropic_filtering=not-a-level\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 0);
}

TEST_F(ConfigTest, ParsesUE5DepthOfFieldOverride) {
    AppConfig config;

    WriteConfig("[UE5]\ndepth_of_field=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.depthOfField, 0);

    WriteConfig("[UE5]\ndepth_of_field=on\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.depthOfField, 1);

    WriteConfig("[UE5]\ndepth_of_field=true\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.depthOfField, 1);

    WriteConfig("[UE5]\ndepth_of_field=default\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.depthOfField, -1);

    WriteConfig("[UE5]\ndepth_of_field=maybe\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.depthOfField, -1);

    WriteConfig("[Graphics]\nvsync_mode=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.depthOfField, -1) << "an absent key must leave the engine alone";
}

TEST_F(ConfigTest, ParsesUE5DlssSuperResolutionOverrideAndQualityMode) {
    AppConfig config;

    WriteConfig("[UE5]\ndlss_super_resolution=on\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.dlssSuperResolution, 1);
    EXPECT_FLOAT_EQ(config.graphics.dlssScreenPercentage, 0.0f)
        << "without a quality mode the game's own screen percentage stays untouched";

    WriteConfig("[UE5]\ndlss_super_resolution=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.dlssSuperResolution, 0);

    WriteConfig("[UE5]\ndlss_super_resolution=default\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.dlssSuperResolution, -1);

    WriteConfig("[UE5]\ndlss_super_resolution=sometimes\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.dlssSuperResolution, -1);

    struct QualityCase {
        const char* value;
        float screenPercentage;
    };
    const QualityCase cases[] = {
        {"dlaa", 100.0f},       {"quality", 66.67f},          {"balanced", 58.0f},
        {"performance", 50.0f}, {"ultra_performance", 33.33f}, {"75", 75.0f},
        {"66,67", 66.67f},
    };
    for (const auto& item : cases) {
        WriteConfig(std::string("[UE5]\ndlss_super_resolution=on\ndlss_super_resolution_quality=") +
                    item.value + "\n");
        LoadConfig(tempConfigFile, config);
        EXPECT_EQ(config.graphics.dlssSuperResolution, 1) << item.value;
        EXPECT_FLOAT_EQ(config.graphics.dlssScreenPercentage, item.screenPercentage) << item.value;
    }

    for (const char* rejected : {"24", "101", "not-a-mode"}) {
        WriteConfig(std::string("[UE5]\ndlss_super_resolution=on\ndlss_super_resolution_quality=") +
                    rejected + "\n");
        LoadConfig(tempConfigFile, config);
        EXPECT_FLOAT_EQ(config.graphics.dlssScreenPercentage, 0.0f) << rejected;
    }
}

TEST_F(ConfigTest, ParsesUE5HdrOverrides) {
    AppConfig config;

    WriteConfig("[UE5]\n"
                "hdr_output=on\n"
                "hdr_peak_luminance=1000\n"
                "hdr_paper_white=200\n"
                "hdr_ui_luminance=250.5\n"
                "hdr_min_luminance=0.005\n"
                "hdr_color_gamut=rec2020\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.hdrOutput, 1);
    EXPECT_EQ(config.graphics.hdrPeakLuminance, 1000);
    EXPECT_FLOAT_EQ(config.graphics.hdrPaperWhite, 200.0f);
    EXPECT_FLOAT_EQ(config.graphics.hdrUiLuminance, 250.5f);
    EXPECT_FLOAT_EQ(config.graphics.hdrMinLuminance, 0.005f);
    EXPECT_EQ(config.graphics.hdrColorGamut, 2);

    WriteConfig("[UE5]\nhdr_output=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.hdrOutput, 0);

    WriteConfig("[Graphics]\nvsync_mode=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.hdrOutput, -1);
    EXPECT_EQ(config.graphics.hdrPeakLuminance, 0);
    EXPECT_FLOAT_EQ(config.graphics.hdrPaperWhite, 0.0f);
    EXPECT_FLOAT_EQ(config.graphics.hdrUiLuminance, 0.0f);
    EXPECT_FLOAT_EQ(config.graphics.hdrMinLuminance, 0.0f);
    EXPECT_EQ(config.graphics.hdrColorGamut, -1);

    const char* gamutNames[] = {"rec709", "dcip3", "rec2020", "aces", "acescg"};
    for (int index = 0; index < 5; ++index) {
        WriteConfig(std::string("[UE5]\nhdr_color_gamut=") + gamutNames[index] + "\n");
        LoadConfig(tempConfigFile, config);
        EXPECT_EQ(config.graphics.hdrColorGamut, index) << gamutNames[index];
    }
}

TEST_F(ConfigTest, RejectsUE5HdrValuesOutsideTheAcceptedRanges) {
    AppConfig config;

    for (const char* rejected : {"79", "10001", "-100", "not-a-number"}) {
        WriteConfig(std::string("[UE5]\nhdr_peak_luminance=") + rejected + "\n");
        LoadConfig(tempConfigFile, config);
        EXPECT_EQ(config.graphics.hdrPeakLuminance, 0) << rejected;
    }
    for (const char* rejected : {"19", "1001", "-1", "bright"}) {
        WriteConfig(std::string("[UE5]\nhdr_paper_white=") + rejected + "\nhdr_ui_luminance=" +
                    rejected + "\n");
        LoadConfig(tempConfigFile, config);
        EXPECT_FLOAT_EQ(config.graphics.hdrPaperWhite, 0.0f) << rejected;
        EXPECT_FLOAT_EQ(config.graphics.hdrUiLuminance, 0.0f) << rejected;
    }
    for (const char* rejected : {"0", "0.00001", "11", "dark"}) {
        WriteConfig(std::string("[UE5]\nhdr_min_luminance=") + rejected + "\n");
        LoadConfig(tempConfigFile, config);
        EXPECT_FLOAT_EQ(config.graphics.hdrMinLuminance, 0.0f) << rejected;
    }
    WriteConfig("[UE5]\nhdr_color_gamut=bt601\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.hdrColorGamut, -1);
}

// The values a configuration accepts and the values the hook is willing to write
// have to be the same set, or a setting could survive parsing and then be
// silently dropped in the game process.
TEST_F(ConfigTest, UE5ConfigBoundsAgreeWithTheHookSidePolicy) {
    EXPECT_FLOAT_EQ(kUE5DlssScreenPercentageMin, ce::ue5_cvar::kDlssScreenPercentageMin);
    EXPECT_FLOAT_EQ(kUE5DlssScreenPercentageMax, ce::ue5_cvar::kDlssScreenPercentageMax);
    EXPECT_EQ(kUE5HdrPeakLuminanceMin, ce::ue5_cvar::kHdrPeakLuminanceMin);
    EXPECT_EQ(kUE5HdrPeakLuminanceMax, ce::ue5_cvar::kHdrPeakLuminanceMax);
    EXPECT_FLOAT_EQ(kUE5HdrPaperWhiteMin, ce::ue5_cvar::kHdrPaperWhiteMin);
    EXPECT_FLOAT_EQ(kUE5HdrPaperWhiteMax, ce::ue5_cvar::kHdrPaperWhiteMax);
    EXPECT_FLOAT_EQ(kUE5HdrUiLuminanceMin, ce::ue5_cvar::kHdrUiLuminanceMin);
    EXPECT_FLOAT_EQ(kUE5HdrUiLuminanceMax, ce::ue5_cvar::kHdrUiLuminanceMax);
    EXPECT_FLOAT_EQ(kUE5HdrMinLuminanceMin, ce::ue5_cvar::kHdrMinLuminanceMin);
    EXPECT_FLOAT_EQ(kUE5HdrMinLuminanceMax, ce::ue5_cvar::kHdrMinLuminanceMax);
    // The tri-state sentinels the config publishes are the ones the policy reads.
    EXPECT_EQ(ce::ue5_cvar::kToggleDefault, -1);
    EXPECT_EQ(ce::ue5_cvar::kToggleOff, 0);
    EXPECT_EQ(ce::ue5_cvar::kToggleOn, 1);
    // acescg is the last gamut the config vocabulary maps, and the policy accepts
    // exactly that far.
    EXPECT_EQ(ce::ue5_cvar::kHdrColorGamutMax, 4);

    AppConfig config;
    WriteConfig("[UE5]\n"
                "depth_of_field=off\n"
                "dlss_super_resolution=on\n"
                "dlss_super_resolution_quality=performance\n"
                "hdr_output=on\n"
                "hdr_peak_luminance=800\n"
                "hdr_paper_white=200\n"
                "hdr_ui_luminance=250\n"
                "hdr_min_luminance=0.005\n"
                "hdr_color_gamut=dcip3\n");
    LoadConfig(tempConfigFile, config);

    ce::ue5_cvar::Settings settings;
    settings.depthOfField = config.graphics.depthOfField;
    settings.dlssSuperResolution = config.graphics.dlssSuperResolution;
    settings.dlssScreenPercentage = config.graphics.dlssScreenPercentage;
    settings.hdrOutput = config.graphics.hdrOutput;
    settings.hdrPeakLuminance = config.graphics.hdrPeakLuminance;
    settings.hdrPaperWhite = config.graphics.hdrPaperWhite;
    settings.hdrUiLuminance = config.graphics.hdrUiLuminance;
    settings.hdrMinLuminance = config.graphics.hdrMinLuminance;
    settings.hdrColorGamut = config.graphics.hdrColorGamut;
    EXPECT_TRUE(ce::ue5_cvar::AnyEnabled(settings));

    for (std::string_view name :
         {"r.DepthOfFieldQuality", "r.NGX.DLSS.Enable", "r.NGX.Enable", "r.TemporalAA.Upscaler",
          "r.AntiAliasingMethod", "r.ScreenPercentage", "r.HDR.EnableHDROutput",
          "r.HDR.Display.MaxLuminance", "r.HDR.Display.MidLuminance", "r.HDR.UI.Luminance",
          "r.HDR.Display.MinLuminanceLog10", "r.HDR.Display.ColorGamut"}) {
        const ce::ue5_cvar::Spec* found = nullptr;
        for (const auto& spec : ce::ue5_cvar::kSpecs) {
            if (name == spec.name)
                found = &spec;
        }
        ASSERT_NE(found, nullptr) << name;
        EXPECT_TRUE(ce::ue5_cvar::Resolve(*found, settings).enabled) << name;
    }
}
