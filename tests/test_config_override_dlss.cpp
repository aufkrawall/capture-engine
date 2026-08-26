#include "../common/config.h"
#include "../hook/common/ue5_cvar_override_policy.h"
#include "test_config_override_fixture.h"

#include <bit>

TEST_F(ConfigOverrideTest, ProfileCanEnableAndDisableRayReconstructionForcePolicy) {
    WriteConfig(
        "[UE5]\n"
        "force_ray_reconstruction=true\n"
        "[App.1]\n"
        "Process=off.exe\n"
        "UE5.force_ray_reconstruction=false\n"
        "[App.2]\n"
        "Process=on.exe\n"
        "UE5.force_ray_reconstruction=true\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "off.exe");
    EXPECT_FALSE(config.graphics.forceRayReconstruction);

    LoadConfig(tempConfigFile, config, "on.exe");
    EXPECT_TRUE(config.graphics.forceRayReconstruction);

    LoadConfig(tempConfigFile, config, "other.exe");
    EXPECT_TRUE(config.graphics.forceRayReconstruction);
}

TEST_F(ConfigOverrideTest, LegacyDLSSProfileRayReconstructionValueBeatsNewUE5GlobalDefault) {
    WriteConfig(
        "[UE5]\n"
        "force_ray_reconstruction=false\n"
        "[Profile.Legacy]\n"
        "Process=legacy.exe\n"
        "DLSS.force_ray_reconstruction=true\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "legacy.exe");
    EXPECT_TRUE(config.graphics.forceRayReconstruction);

    LoadConfig(tempConfigFile, config, "other.exe");
    EXPECT_FALSE(config.graphics.forceRayReconstruction);
}

TEST_F(ConfigOverrideTest, ProfileControlsUE5BundlesAndSharpenPrecedenceInput) {
    WriteConfig(
        "[UE5]\n"
        "ray_reconstruction_optimal_settings=false\n"
        "disable_post_processing_effects=false\n"
        "tonemapper_sharpen=default\n"
        "[Profile.UE5]\n"
        "process=ue5.exe\n"
        "UE5.ray_reconstruction_optimal_settings=true\n"
        "UE5.disable_post_processing_effects=true\n"
        "UE5.tonemapper_sharpen=0.6\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "ue5.exe");
    EXPECT_EQ(config.graphics.rayReconstructionOptimalSettings, 3);
    EXPECT_FALSE(config.graphics.forceRayReconstruction);
    EXPECT_TRUE(config.graphics.disablePostProcessingEffects);
    EXPECT_FLOAT_EQ(config.graphics.tonemapperSharpen, 0.6f);
}

TEST_F(ConfigOverrideTest, ProfileControlsUE5InternalFpsLimitAndAnisotropy) {
    WriteConfig(
        "[UE5]\n"
        "internal_fps_limit=default\n"
        "internal_anisotropic_filtering=default\n"
        "[Profile.UE5]\n"
        "process=ue5.exe\n"
        "UE5.internal_fps_limit=60\n"
        "UE5.internal_anisotropic_filtering=8x\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "ue5.exe");
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, 60.0f);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 8);

    LoadConfig(tempConfigFile, config, "other.exe");
    EXPECT_FLOAT_EQ(config.graphics.internalFpsLimit, -1.0f);
    EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 0);
}

TEST_F(ConfigOverrideTest, ProfileCanReplaceOrDisableGlobalCustomCVarOverrides) {
    WriteConfig(
        "[UE5]\n"
        "custom_cvar_overrides=r.SSR.Temporal=0\n"
        "[Profile.Custom]\n"
        "process=custom.exe\n"
        "UE5.custom_cvar_overrides=tonemapper_sharpen=0.25\n"
        "[Profile.Off]\n"
        "process=off.exe\n"
        "UE5.custom_cvar_overrides=off\n");

    const std::size_t temporal = ce::ue5_cvar::FindSpecIndex("r.SSR.Temporal");
    const std::size_t sharpen = ce::ue5_cvar::FindSpecIndex("tonemapper_sharpen");
    ASSERT_LT(temporal, ce::ue5_cvar::kSpecs.size());
    ASSERT_LT(sharpen, ce::ue5_cvar::kSpecs.size());

    AppConfig config;
    LoadConfig(tempConfigFile, config, "custom.exe");
    EXPECT_EQ(config.graphics.ue5CustomCVarOverrideMask, uint64_t{1} << sharpen);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(config.graphics.ue5CustomCVarOverrideValues[sharpen]), 0.25f);

    LoadConfig(tempConfigFile, config, "off.exe");
    EXPECT_EQ(config.graphics.ue5CustomCVarOverrideMask, 0u);

    LoadConfig(tempConfigFile, config, "other.exe");
    EXPECT_EQ(config.graphics.ue5CustomCVarOverrideMask, uint64_t{1} << temporal);
    EXPECT_EQ(static_cast<int32_t>(config.graphics.ue5CustomCVarOverrideValues[temporal]), 0);
}

TEST_F(ConfigOverrideTest, PerAppDLSSFGFactorOverride) {
    std::string iniContent =
        "[Graphics]\n"
        "dlss_fg_factor=2x\n"
        "dlss_sr_preset=K\n"
        "\n"
        "[App.1]\n"
        "Process=RoboCop-Win64-Shipping.exe\n"
        "dlss_fg_factor=3x\n"
        "dlss_sr_preset=L\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "RoboCop-Win64-Shipping.exe");

    EXPECT_EQ(config.graphics.dlssFgFactor, "3x");
    EXPECT_EQ(config.graphics.parsed.dlssFGFactor, 3);
    EXPECT_EQ(config.graphics.parsed.srPreset, 12u);
}

TEST_F(ConfigOverrideTest, PerAppDLSSFGPresetOverride) {
    std::string iniContent =
        "[DLSS]\n"
        "dlss_fg_preset=A\n"
        "\n"
        "[App.1]\n"
        "Process=RoboCop-Win64-Shipping.exe\n"
        "dlss_fg_preset=B\n";

    WriteConfig(iniContent);

    AppConfig matched;
    LoadConfig(tempConfigFile, matched, "RoboCop-Win64-Shipping.exe");
    EXPECT_EQ(matched.graphics.dlssFgPreset, "B");
    EXPECT_EQ(matched.graphics.parsed.fgPreset, 2u);

    AppConfig unmatched;
    LoadConfig(tempConfigFile, unmatched, "other.exe");
    EXPECT_EQ(unmatched.graphics.dlssFgPreset, "A");
    EXPECT_EQ(unmatched.graphics.parsed.fgPreset, 1u);
}

TEST_F(ConfigOverrideTest, DLSSFGPresetDefaultsToNoOverride) {
    WriteConfig("[DLSS]\ndlss_fg_factor=default\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "any.exe");

    EXPECT_EQ(config.graphics.dlssFgPreset, "default");
    EXPECT_EQ(config.graphics.parsed.fgPreset, 0u);
}

TEST_F(ConfigOverrideTest, StreamlineDllPathParsing) {
    std::string iniContent =
        "[Graphics]\n"
        "streamline_dll_path=C:\\custom\\sl\\dlls\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "test.exe");

    EXPECT_EQ(config.graphics.streamlineDllPath, "C:\\custom\\sl\\dlls");
}

TEST_F(ConfigOverrideTest, ProfileResolvesTheCompleteProcessLocalDlssRuntimeSet) {
    WriteConfig(
        "[Profile.SplitRenderer]\n"
        "process=client.exe\n"
        "dlss_sr_dll_path=C:\\runtime\\sl\n"
        "dlss_rr_dll_path=C:\\runtime\\sl\n"
        "dlss_fg_dll_path=C:\\runtime\\sl\n"
        "streamline_dll_path=C:\\runtime\\sl\n"
        "dlss_sr_preset=M\n"
        "dlss_rr_preset=F\n"
        "dlss_fg_preset=B\n"
        "dlss_debug_overlay=on\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "client.exe");
    EXPECT_EQ(config.graphics.dlssSrDllPath, "C:\\runtime\\sl");
    EXPECT_EQ(config.graphics.dlssRrDllPath, "C:\\runtime\\sl");
    EXPECT_EQ(config.graphics.dlssFgDllPath, "C:\\runtime\\sl");
    EXPECT_EQ(config.graphics.streamlineDllPath, "C:\\runtime\\sl");
    EXPECT_EQ(config.graphics.parsed.srPreset, 13u);
    EXPECT_EQ(config.graphics.parsed.rrPreset, 6u);
    EXPECT_EQ(config.graphics.parsed.fgPreset, 2u);
    EXPECT_EQ(config.graphics.dlssDebugOverlay, "on");
}
