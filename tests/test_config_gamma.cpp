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

TEST_F(ConfigTest, SharpenDefaultsToOffWithTheDocumentedStrength) {
    WriteConfig("[Graphics]\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.sharpenMode, "off");
    EXPECT_EQ(config.graphics.sharpenColorSpace, "auto");
    EXPECT_FLOAT_EQ(config.graphics.sharpenStrength, 0.5f);
    // Absent means the effect at full weight, so adding this control cannot
    // quietly weaken a configuration written before it existed.
    EXPECT_FLOAT_EQ(config.graphics.sharpenIntensity, 1.0f);
}

TEST_F(ConfigTest, SharpenAcceptsBothEffectsAndTheirParameters) {
    WriteConfig(
        "[Graphics]\n"
        "sharpen=cas\n"
        "sharpen_strength=0.85\n"
        "sharpen_color_space=gamma\n");
    AppConfig casConfig;
    LoadConfig(tempConfigFile, casConfig);
    EXPECT_EQ(casConfig.graphics.sharpenMode, "cas");
    EXPECT_FLOAT_EQ(casConfig.graphics.sharpenStrength, 0.85f);
    EXPECT_EQ(casConfig.graphics.sharpenColorSpace, "gamma");

    WriteConfig(
        "[Graphics]\n"
        "sharpen=RCAS\n"
        "sharpen_color_space=direct\n");
    AppConfig rcasConfig;
    LoadConfig(tempConfigFile, rcasConfig);
    EXPECT_EQ(ce::sharpen::ParseMode(rcasConfig.graphics.sharpenMode.c_str()), ce::sharpen::Mode::Rcas);
    EXPECT_EQ(rcasConfig.graphics.sharpenColorSpace, "direct");
}

TEST_F(ConfigTest, UnknownSharpenValuesFallBackInsteadOfSelectingAnotherEffect) {
    WriteConfig(
        "[Graphics]\n"
        "sharpen=fsr\n"
        "sharpen_color_space=perceptual\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.sharpenMode, "off");
    EXPECT_EQ(config.graphics.sharpenColorSpace, "auto");
}

TEST_F(ConfigTest, SharpenStrengthOutsideItsRangeFallsBackToTheDefault) {
    WriteConfig(
        "[Graphics]\n"
        "sharpen=rcas\n"
        "sharpen_strength=4.0\n");
    AppConfig tooHigh;
    LoadConfig(tempConfigFile, tooHigh);
    EXPECT_FLOAT_EQ(tooHigh.graphics.sharpenStrength, 0.5f);

    WriteConfig(
        "[Graphics]\n"
        "sharpen=rcas\n"
        "sharpen_strength=-1\n");
    AppConfig negative;
    LoadConfig(tempConfigFile, negative);
    EXPECT_FLOAT_EQ(negative.graphics.sharpenStrength, 0.5f);

    // 0 is the mildest setting each effect supports, not a disable, so it has
    // to survive as an explicit value rather than being treated as absent.
    WriteConfig(
        "[Graphics]\n"
        "sharpen=cas\n"
        "sharpen_strength=0\n");
    AppConfig zero;
    LoadConfig(tempConfigFile, zero);
    EXPECT_FLOAT_EQ(zero.graphics.sharpenStrength, 0.0f);
    EXPECT_EQ(zero.graphics.sharpenMode, "cas");
}

TEST_F(ConfigTest, SharpenIntensityIsIndependentOfStrength) {
    WriteConfig(
        "[Graphics]\n"
        "sharpen=cas\n"
        "sharpen_strength=0.2\n"
        "sharpen_intensity=0.6\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.sharpenStrength, 0.2f);
    EXPECT_FLOAT_EQ(config.graphics.sharpenIntensity, 0.6f);
}

TEST_F(ConfigTest, SharpenIntensityOutsideItsRangeFallsBackToFullWeight) {
    WriteConfig(
        "[Graphics]\n"
        "sharpen=cas\n"
        "sharpen_intensity=3.5\n");
    AppConfig tooHigh;
    LoadConfig(tempConfigFile, tooHigh);
    EXPECT_FLOAT_EQ(tooHigh.graphics.sharpenIntensity, 1.0f);

    // Unlike strength, 0 is a meaningful value here - it means "no sharpening"
    // at all - so it has to survive rather than being replaced by the default.
    WriteConfig(
        "[Graphics]\n"
        "sharpen=cas\n"
        "sharpen_intensity=0\n");
    AppConfig zero;
    LoadConfig(tempConfigFile, zero);
    EXPECT_FLOAT_EQ(zero.graphics.sharpenIntensity, 0.0f);
}

TEST_F(ConfigTest, SharpenContrastAndAmountPreferredOverStrengthAndIntensity) {
    WriteConfig(
        "[Graphics]\n"
        "sharpen=rcas\n"
        "sharpen_contrast=0.75\n"
        "sharpen_amount=0.85\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_FLOAT_EQ(config.graphics.sharpenStrength, 0.75f);
    EXPECT_FLOAT_EQ(config.graphics.sharpenIntensity, 0.85f);

    // Legacy aliases are overridden when the new keys are present.
    WriteConfig(
        "[Graphics]\n"
        "sharpen=rcas\n"
        "sharpen_contrast=0.3\n"
        "sharpen_amount=0.4\n"
        "sharpen_strength=0.9\n"
        "sharpen_intensity=0.95\n");
    AppConfig overrideConfig;
    LoadConfig(tempConfigFile, overrideConfig);
    EXPECT_FLOAT_EQ(overrideConfig.graphics.sharpenStrength, 0.3f);
    EXPECT_FLOAT_EQ(overrideConfig.graphics.sharpenIntensity, 0.4f);
}
