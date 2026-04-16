#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "../common/config.h"

class ConfigOverrideTest : public ::testing::Test {
protected:
    std::string tempConfigFile;

    void SetUp() override {
        tempConfigFile = (std::filesystem::current_path() / "test_config_override.ini").string();
        remove(tempConfigFile.c_str());
    }

    void TearDown() override {
        remove(tempConfigFile.c_str());
    }

    void WriteConfig(const std::string& content) {
        std::ofstream out(tempConfigFile);
        out << content;
        out.close();
    }
};

TEST_F(ConfigOverrideTest, SimpleOverride) {
    std::string iniContent =
        "[Overlay]\n"
        "enabled=true\n"
        "observer_only=false\n"
        "observer_policy_only=false\n"
        "observer_startup_present_only=false\n"
        "\n"
        "[App.1]\n"
        "Process=game.exe\n"
        "Overlay.enabled=false\n"
        "Overlay.observer_only=true\n"
        "Overlay.observer_policy_only=true\n"
        "Overlay.observer_startup_present_only=true\n";

    WriteConfig(iniContent);

    AppConfig config;

    // 1. Load without override (simulate other process)
    LoadConfig(tempConfigFile, config, "notepad.exe");
    EXPECT_TRUE(config.overlay.showOverlay);

    // 2. Load with override
    LoadConfig(tempConfigFile, config, "game.exe");
    EXPECT_FALSE(config.overlay.showOverlay);
    EXPECT_TRUE(config.overlay.observerOnly);
    EXPECT_TRUE(config.overlay.observerPolicyOnly);
    EXPECT_TRUE(config.overlay.observerStartupPresentOnly);
}

TEST_F(ConfigOverrideTest, CaseInsensitiveProcessMatch) {
    std::string iniContent =
        "[Overlay]\n"
        "enabled=true\n"
        "\n"
        "[App.1]\n"
        "Process=Game.exe\n"
        "Overlay.enabled=false\n";

    WriteConfig(iniContent);

    AppConfig config;

    // Mixed case override name
    LoadConfig(tempConfigFile, config, "GAME.EXE");
    EXPECT_FALSE(config.overlay.showOverlay);
}

TEST_F(ConfigOverrideTest, FallbackToGlobal) {
    std::string iniContent =
        "[Video]\n"
        "bitrate=50Mbps\n"
        "fps=60\n"
        "\n"
        "[App.1]\n"
        "Process=game.exe\n"
        "Video.bitrate=100Mbps\n";
    // fps NOT overridden

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    // Overridden value
    EXPECT_EQ(config.video.bitrate, "100Mbps");
    // Global fallback value
    EXPECT_EQ(config.video.fps, 60);
}

TEST_F(ConfigOverrideTest, MultipleAppSections) {
    std::string iniContent =
        "[Overlay]\n"
        "enabled=true\n"
        "\n"
        "[App.1]\n"
        "Process=game1.exe\n"
        "Overlay.enabled=false\n"
        "\n"
        "[App.2]\n"
        "Process=game2.exe\n"
        "Overlay.enabled=true\n";

    WriteConfig(iniContent);

    AppConfig config;

    // Game 1 -> False
    LoadConfig(tempConfigFile, config, "game1.exe");
    EXPECT_FALSE(config.overlay.showOverlay);

    // Game 2 -> True (explicitly set, though same as default)
    // Let's change default to false to be sure
    std::string iniContent2 =
        "[Overlay]\n"
        "enabled=false\n"
        "\n"
        "[App.2]\n"
        "Process=game2.exe\n"
        "Overlay.enabled=true\n";
    WriteConfig(iniContent2);

    LoadConfig(tempConfigFile, config, "game2.exe");
    EXPECT_TRUE(config.overlay.showOverlay);
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
