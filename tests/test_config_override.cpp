#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
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

TEST_F(ConfigOverrideTest, StreamlineDllPathParsing) {
    std::string iniContent =
        "[Graphics]\n"
        "streamline_dll_path=C:\\custom\\sl\\dlls\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "test.exe");

    EXPECT_EQ(config.graphics.streamlineDllPath, "C:\\custom\\sl\\dlls");
}

// Regression: an [App.N] override section's reserved "Process" selector key must
// NOT leak into the [AppAudio.N] "process" field. Before the fix this collapsed
// every app-audio source onto the running game, summing identical captures into a
// track and producing comb-filter ("metallic") audio.
TEST_F(ConfigOverrideTest, AppAudioProcessNotRewrittenByAppSelectorKey) {
    std::string iniContent =
        "[App.1]\n"
        "Process=game.exe\n"
        "\n"
        "[AppAudio.1]\n"
        "enabled=true\n"
        "process=other.exe\n"
        "track=1\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    int appCount = 0;
    for (const auto& s : config.audioSources) {
        if (s.sourceType == AudioConfig::AppAudio) {
            ++appCount;
            EXPECT_EQ(s.processName, "other.exe");  // not rewritten to "game.exe"
        }
    }
    EXPECT_EQ(appCount, 1);
}

// The explicit per-app override form ([App.N] AppAudio.1.process=...) must still
// work; only the bare reserved selector key is excluded from the fallback.
TEST_F(ConfigOverrideTest, AppAudioExplicitProcessOverrideStillApplies) {
    std::string iniContent =
        "[App.1]\n"
        "Process=game.exe\n"
        "AppAudio.1.process=explicit.exe\n"
        "\n"
        "[AppAudio.1]\n"
        "enabled=true\n"
        "process=other.exe\n"
        "track=1\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    for (const auto& s : config.audioSources) {
        if (s.sourceType == AudioConfig::AppAudio) {
            EXPECT_EQ(s.processName, "explicit.exe");
        }
    }
}

// Reproduces the Strange Brigade profile: three candidate game processes routed to
// tracks 1 & 2, with an [App.N] override matching the running game. All three
// app-audio sources must keep their distinct process names (only the running one
// will actually capture at runtime), never collapsing onto the running game.
TEST_F(ConfigOverrideTest, MultipleAppAudioSourcesKeepDistinctProcessNames) {
    std::string iniContent =
        "[AppAudio.1]\n"
        "enabled=true\n"
        "Process=FortniteClient-Win64-Shipping.exe\n"
        "track=1,2\n"
        "\n"
        "[AppAudio.2]\n"
        "enabled=true\n"
        "Process=StrangeBrigade_DX12.exe\n"
        "track=1,2\n"
        "\n"
        "[AppAudio.3]\n"
        "enabled=true\n"
        "Process=StrangeBrigade_Vulkan.exe\n"
        "track=1,2\n"
        "\n"
        "[App.2]\n"
        "Process=StrangeBrigade_DX12.exe\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "StrangeBrigade_DX12.exe");

    std::set<std::string> names;
    for (const auto& s : config.audioSources) {
        if (s.sourceType == AudioConfig::AppAudio) {
            names.insert(s.processName);
        }
    }
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(names.count("FortniteClient-Win64-Shipping.exe"), 1u);
    EXPECT_EQ(names.count("StrangeBrigade_DX12.exe"), 1u);
    EXPECT_EQ(names.count("StrangeBrigade_Vulkan.exe"), 1u);
}
