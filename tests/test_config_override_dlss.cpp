#include "../common/config.h"
#include "test_config_override_fixture.h"

TEST_F(ConfigOverrideTest, ProfileCanEnableAndDisableRayReconstructionForcePolicy) {
    WriteConfig(
        "[DLSS]\n"
        "force_ray_reconstruction=true\n"
        "[App.1]\n"
        "Process=off.exe\n"
        "DLSS.force_ray_reconstruction=false\n"
        "[App.2]\n"
        "Process=on.exe\n"
        "DLSS.force_ray_reconstruction=true\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "off.exe");
    EXPECT_FALSE(config.graphics.forceRayReconstruction);

    LoadConfig(tempConfigFile, config, "on.exe");
    EXPECT_TRUE(config.graphics.forceRayReconstruction);

    LoadConfig(tempConfigFile, config, "other.exe");
    EXPECT_TRUE(config.graphics.forceRayReconstruction);
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
