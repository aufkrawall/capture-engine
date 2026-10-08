#include "test_config_shared.h"

TEST_F(ConfigTest, GenericGammaDefaultsAreIndependentOfUeGamma) {
    WriteConfig("[UE5]\ndisplay_gamma=2.4\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.postProcessDisplayGamma, "default");
    EXPECT_EQ(config.graphics.postProcessGammaSource, "2.2");
    EXPECT_FLOAT_EQ(config.graphics.displayGamma, 2.4f);
}

TEST_F(ConfigTest, GenericGammaAcceptsCurvesAndRejectsInvalidSource) {
    for (const char* curve : {"2.2", "2.4", "srgb"}) {
        WriteConfig(std::string("[Graphics]\ndisplay_gamma=") + curve + "\ngamma_source=2.4\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config);
        EXPECT_EQ(config.graphics.postProcessDisplayGamma, curve);
        EXPECT_EQ(config.graphics.postProcessGammaSource, "2.4");
    }
    WriteConfig("[Graphics]\ndisplay_gamma=bogus\ngamma_source=default\n");
    AppConfig invalid;
    LoadConfig(tempConfigFile, invalid);
    EXPECT_EQ(invalid.graphics.postProcessDisplayGamma, "default");
    EXPECT_EQ(invalid.graphics.postProcessGammaSource, "2.2");
}

TEST_F(ConfigTest, GenericGammaProfileCanDisableGlobalCorrection) {
    WriteConfig("[Graphics]\ndisplay_gamma=srgb\n[Profile.Example]\nprocess=example.exe\nGraphics.display_gamma=off\nGraphics.gamma_source=srgb\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config, "example.exe");
    EXPECT_EQ(config.graphics.postProcessDisplayGamma, "default");
    EXPECT_EQ(config.graphics.postProcessGammaSource, "srgb");
}
