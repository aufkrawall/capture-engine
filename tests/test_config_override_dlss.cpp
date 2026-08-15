#include "../common/config.h"
#include "test_config_override_fixture.h"

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
    EXPECT_TRUE(config.graphics.rayReconstructionOptimalSettings);
    EXPECT_TRUE(config.graphics.forceRayReconstruction);
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
