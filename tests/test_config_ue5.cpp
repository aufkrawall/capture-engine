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
