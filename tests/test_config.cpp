#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "../common/config.h"

class ConfigTest : public ::testing::Test {
protected:
    // Use absolute path because GetPrivateProfileString requires it or checks
    // C:\Windows
    std::string tempConfigFile;

    void SetUp() override {
        // Get absolute path
        tempConfigFile = (std::filesystem::current_path() / "test_config.ini").string();

        // Clean up before test
        remove(tempConfigFile.c_str());
    }

    void TearDown() override {
        // Clean up after test
        remove(tempConfigFile.c_str());
    }

    void WriteConfig(const std::string& content) {
        std::ofstream out(tempConfigFile);
        out << content;
        out.close();
    }
};

TEST_F(ConfigTest, LoadDefaultsWhenFileMissing) {
    AppConfig config;
    // Use a non-existent absolute path to force default creation logic if
    // applicable Or just ensure it doesn't find a file. LoadConfig creates a
    // default file if missing, so we should check THAT file or the loaded values.

    std::string missingFile = (std::filesystem::current_path() / "nonexistent.ini").string();
    remove(missingFile.c_str());

    LoadConfig(missingFile, config);

    // Default is TRUE in config.cpp
    EXPECT_TRUE(config.debugLogging);
    EXPECT_EQ(config.captureMethod, "auto");
    EXPECT_EQ(config.video.profile, "auto");
    EXPECT_EQ(config.video.scaling.sharpness, 100);
    EXPECT_FALSE(config.graphics.forceMipBiasClamp);
    EXPECT_EQ(config.graphics.backbufferCount, -1);

    std::ifstream generated(missingFile);
    ASSERT_TRUE(generated.is_open());
    std::string generatedText((std::istreambuf_iterator<char>(generated)), std::istreambuf_iterator<char>());
    generated.close();
    EXPECT_NE(generatedText.find("capture_method=auto"), std::string::npos);
    EXPECT_NE(generatedText.find("profile=auto"), std::string::npos);
    EXPECT_NE(generatedText.find("sharpness=100"), std::string::npos);
    EXPECT_NE(generatedText.find("; backbuffer_count, affecting vsync"), std::string::npos);
    EXPECT_NE(generatedText.find("backbuffer_count=-1"), std::string::npos);
    EXPECT_EQ(generatedText.find("perf_metrics_logging="), std::string::npos);
    EXPECT_EQ(generatedText.find("nvidia_smooth_motion_compat="), std::string::npos);
    EXPECT_EQ(generatedText.find("\nvfr="), std::string::npos);
    EXPECT_EQ(generatedText.find("\nvfr_audio_sync="), std::string::npos);

    // Clean up the created default file
    remove(missingFile.c_str());
}

TEST_F(ConfigTest, ParseValues) {
    std::string iniContent =
        "[General]\n"
        "debug_logging=true\n"
        "capture_method=inject\n"
        "\n"
        "[Video]\n"
        "encoder=av1_nvenc\n"
        "fps=60\n"
        "bitrate=50Mbps\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.debugLogging);
    EXPECT_EQ(config.captureMethod, "inject");
    EXPECT_EQ(config.video.encoder, "av1_nvenc");
    EXPECT_EQ(config.video.fps, 60);
    EXPECT_EQ(config.video.bitrate, "50Mbps");
}

TEST_F(ConfigTest, PseudoOverlayProcessListIsNormalized) {
    std::string iniContent =
        "[pseudo-overlay]\n"
        "enabled=true\n"
        "process_list=  FortniteClient-Win64-Shipping.exe | | \" StrangeBrigade_DX12.exe \" |  \n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.pseudoOverlay.enabled);
    EXPECT_EQ(config.pseudoOverlay.processList, "FortniteClient-Win64-Shipping.exe|StrangeBrigade_DX12.exe");
}

TEST_F(ConfigTest, LegacyPerfMetricsLoggingEnablesUnifiedDebugLogging) {
    std::string iniContent =
        "[General]\n"
        "debug_logging=false\n"
        "perf_metrics_logging=true\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.debugLogging);
}

TEST_F(ConfigTest, WhitelistParsing) {
    // Test the multiline whitelist format
    // Use comma-separated format which is more reliably parsed
    std::string iniContent =
        "[Injection]\n"
        "whitelist=game1.exe,game2.exe,game3.exe\n"
        "\n"
        "[Overlay]\n"
        "enabled=true\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    // Should have 3 games from the comma-separated list
    EXPECT_EQ(config.gameWhitelist.size(), 3);

    if (config.gameWhitelist.size() >= 3) {
        EXPECT_EQ(config.gameWhitelist[0].pattern, "game1.exe");
        EXPECT_EQ(config.gameWhitelist[0].mode, MatchMode::kExact);
        EXPECT_EQ(config.gameWhitelist[1].pattern, "game2.exe");
        EXPECT_EQ(config.gameWhitelist[2].pattern, "game3.exe");
    }
}

TEST_F(ConfigTest, InvalidValuesFallBack) {
    // Test robustness if needed
}

TEST_F(ConfigTest, ParseGraphicsOverrideOptions) {
    std::string iniContent =
        "[Graphics]\n"
        "mip_mapping=trilinear\n"
        "force_mip_bias_clamp=true\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.graphics.mipMapping, "trilinear");
    EXPECT_TRUE(config.graphics.forceMipBiasClamp);
}

TEST_F(ConfigTest, LegacyBackbufferZeroRemainsAppControlled) {
    std::string iniContent =
        "[Graphics]\n"
        "backbuffer_count=0\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.graphics.backbufferCount, -1);
}

TEST_F(ConfigTest, LegacyScalingFilterStillAppliesWhenSharpnessMissing) {
    std::string iniContent =
        "[Scaling]\n"
        "filter=lanczos\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.video.scaling.quality, "best");
    EXPECT_EQ(config.video.scaling.sharpness, 50);
}

TEST(ConfigHelpersTest, BackbufferOverrideRangeStartsAtTwo) {
    EXPECT_FALSE(HasBackbufferCountOverride(-1));
    EXPECT_FALSE(HasBackbufferCountOverride(0));
    EXPECT_FALSE(HasBackbufferCountOverride(1));
    EXPECT_TRUE(HasBackbufferCountOverride(2));
    EXPECT_TRUE(HasBackbufferCountOverride(6));
    EXPECT_FALSE(HasBackbufferCountOverride(7));
}

TEST(ConfigHelpersTest, DlssPresetParsingAcceptsFutureLetters) {
    EXPECT_EQ(ParseDlssPreset("A"), 1u);
    EXPECT_EQ(ParseDlssPreset("Z"), 26u);
    EXPECT_EQ(ParseDlssRRPreset("A"), 1u);
    EXPECT_EQ(ParseDlssRRPreset("Z"), 26u);
}

TEST(ConfigHelpersTest, MatchModeParsing) {
    EXPECT_EQ(ParseMatchMode("exact"), MatchMode::kExact);
    EXPECT_EQ(ParseMatchMode("title_executable"), MatchMode::kTitleExecutable);
    EXPECT_EQ(ParseMatchMode("title_exec"), MatchMode::kTitleExecutable);
    EXPECT_EQ(ParseMatchMode("title_type"), MatchMode::kTitleType);
    EXPECT_EQ(ParseMatchMode("title_class"), MatchMode::kTitleType);
    EXPECT_EQ(ParseMatchMode("invalid"), MatchMode::kExact);
    EXPECT_EQ(ParseMatchMode(""), MatchMode::kExact);
}

TEST(ConfigHelpersTest, MatchModeToString) {
    EXPECT_STREQ(MatchModeToString(MatchMode::kExact), "exact");
    EXPECT_STREQ(MatchModeToString(MatchMode::kTitleExecutable), "title_executable");
    EXPECT_STREQ(MatchModeToString(MatchMode::kTitleType), "title_type");
}

class WhitelistEntryTest : public ::testing::Test {
protected:
    std::string tempConfigFile;

    void SetUp() override {
        tempConfigFile = (std::filesystem::current_path() / "test_whitelist_entry.ini").string();
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

TEST_F(WhitelistEntryTest, SimpleProcessNameOnly) {
    std::string iniContent =
        "[Injection]\n"
        "whitelist=(\n"
        "  game1.exe\n"
        "  game2.exe:exact\n"
        "  game3.exe:title_executable\n"
        ")\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.gameWhitelist.size(), 3);

    if (config.gameWhitelist.size() >= 3) {
        EXPECT_EQ(config.gameWhitelist[0].pattern, "game1.exe");
        EXPECT_EQ(config.gameWhitelist[0].windowName, "");
        EXPECT_EQ(config.gameWhitelist[0].mode, MatchMode::kExact);

        EXPECT_EQ(config.gameWhitelist[1].pattern, "game2.exe");
        EXPECT_EQ(config.gameWhitelist[1].windowName, "");
        EXPECT_EQ(config.gameWhitelist[1].mode, MatchMode::kExact);

        EXPECT_EQ(config.gameWhitelist[2].pattern, "game3.exe");
        EXPECT_EQ(config.gameWhitelist[2].windowName, "");
        EXPECT_EQ(config.gameWhitelist[2].mode, MatchMode::kTitleExecutable);
    }
}

TEST_F(WhitelistEntryTest, ProcessAndWindowWithMode) {
    std::string iniContent =
        "[Injection]\n"
        "whitelist=(\n"
        "  game.exe:My Game Window:title_executable\n"
        ")\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.gameWhitelist.size(), 1);

    if (config.gameWhitelist.size() >= 1) {
        EXPECT_EQ(config.gameWhitelist[0].pattern, "game.exe");
        EXPECT_EQ(config.gameWhitelist[0].windowName, "My Game Window");
        EXPECT_EQ(config.gameWhitelist[0].mode, MatchMode::kTitleExecutable);
    }
}

TEST_F(WhitelistEntryTest, QuotedNamesWithColons) {
    std::string iniContent =
        "[Injection]\n"
        "whitelist=(\n"
        "  \"Game: DX12.exe\":\"Window: Title\":title_executable\n"
        ")\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.gameWhitelist.size(), 1);

    if (config.gameWhitelist.size() >= 1) {
        EXPECT_EQ(config.gameWhitelist[0].pattern, "Game: DX12.exe");
        EXPECT_EQ(config.gameWhitelist[0].windowName, "Window: Title");
        EXPECT_EQ(config.gameWhitelist[0].mode, MatchMode::kTitleExecutable);
    }
}

TEST_F(WhitelistEntryTest, WindowOnlyEntry) {
    std::string iniContent =
        "[Injection]\n"
        "wgc_window_detection=(\n"
        "  :\"My Window Title\":title_type\n"
        ")\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.wgcWindowTitles.size(), 1);

    if (config.wgcWindowTitles.size() >= 1) {
        EXPECT_EQ(config.wgcWindowTitles[0].pattern, "");
        EXPECT_EQ(config.wgcWindowTitles[0].windowName, "My Window Title");
        EXPECT_EQ(config.wgcWindowTitles[0].mode, MatchMode::kTitleType);
    }
}

TEST_F(WhitelistEntryTest, WgcWindowDetectionEntries) {
    std::string iniContent =
        "[Injection]\n"
        "wgc_window_detection=(\n"
        "  \"Game Window\":title_executable\n"
        "  MyGame.exe:title_type\n"
        "  MyGame.exe:My Window:title_type\n"
        ")\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.wgcWindowTitles.size(), 3);

    if (config.wgcWindowTitles.size() >= 3) {
        // Single window name with mode - treated as window (no .exe extension)
        EXPECT_EQ(config.wgcWindowTitles[0].windowName, "Game Window");
        EXPECT_EQ(config.wgcWindowTitles[0].pattern, "");
        EXPECT_EQ(config.wgcWindowTitles[0].mode, MatchMode::kTitleExecutable);

        // Process name with mode
        EXPECT_EQ(config.wgcWindowTitles[1].pattern, "MyGame.exe");
        EXPECT_EQ(config.wgcWindowTitles[1].windowName, "");
        EXPECT_EQ(config.wgcWindowTitles[1].mode, MatchMode::kTitleType);

        // Process + window + mode
        EXPECT_EQ(config.wgcWindowTitles[2].pattern, "MyGame.exe");
        EXPECT_EQ(config.wgcWindowTitles[2].windowName, "My Window");
        EXPECT_EQ(config.wgcWindowTitles[2].mode, MatchMode::kTitleType);
    }
}

TEST_F(WhitelistEntryTest, OverlayWhitelistEntries) {
    std::string iniContent =
        "[Injection]\n"
        "overlay_whitelist=(\n"
        "  overlay.exe:exact\n"
        "  app.exe:App Window:title_executable\n"
        ")\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.overlayWhitelist.size(), 2);

    if (config.overlayWhitelist.size() >= 2) {
        EXPECT_EQ(config.overlayWhitelist[0].pattern, "overlay.exe");
        EXPECT_EQ(config.overlayWhitelist[0].windowName, "");
        EXPECT_EQ(config.overlayWhitelist[0].mode, MatchMode::kExact);

        EXPECT_EQ(config.overlayWhitelist[1].pattern, "app.exe");
        EXPECT_EQ(config.overlayWhitelist[1].windowName, "App Window");
        EXPECT_EQ(config.overlayWhitelist[1].mode, MatchMode::kTitleExecutable);
    }
}
