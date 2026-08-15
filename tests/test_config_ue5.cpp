#include "test_config_shared.h"

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

    WriteConfig("[UE5]\nray_reconstruction_optimal_settings=on\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_TRUE(config.graphics.rayReconstructionOptimalSettings);
    EXPECT_TRUE(config.graphics.forceRayReconstruction)
        << "the documented optimal bundle includes r.NGX.DLSS.DenoiserMode=1";

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
