#include <gtest/gtest.h>
#include <windows.h>
#include "../common/config.h"

namespace {
std::string MakeTestPath(const char* filename) {
    char buffer[MAX_PATH] = {};
    DWORD length = GetFullPathNameA(filename, MAX_PATH, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH) {
        return filename;
    }
    return buffer;
}

void WriteTextFile(const std::string& path, const std::string& content) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    ASSERT_TRUE(WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
    CloseHandle(file);
    ASSERT_EQ(written, content.size());
}

std::string ReadTextFile(const std::string& path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(file, INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size = {};
    EXPECT_TRUE(GetFileSizeEx(file, &size));
    std::string content(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!content.empty()) {
        EXPECT_TRUE(ReadFile(file, content.data(), static_cast<DWORD>(content.size()), &read, nullptr));
        content.resize(read);
    }
    CloseHandle(file);
    return content;
}
}  // namespace

class ConfigTest : public ::testing::Test {
protected:
    // Use absolute path because GetPrivateProfileString requires it or checks
    // C:\Windows
    std::string tempConfigFile;

    void SetUp() override {
        tempConfigFile = MakeTestPath("test_config.ini");

        // Clean up before test
        remove(tempConfigFile.c_str());
    }

    void TearDown() override {
        // Clean up after test
        remove(tempConfigFile.c_str());
    }

    void WriteConfig(const std::string& content) {
        WriteTextFile(tempConfigFile, content);
    }
};

TEST_F(ConfigTest, LoadDefaultsWhenFileMissing) {
    AppConfig config;
    // Use a non-existent absolute path to force default creation logic if
    // applicable Or just ensure it doesn't find a file. LoadConfig creates a
    // default file if missing, so we should check THAT file or the loaded values.

    std::string missingFile = MakeTestPath("nonexistent.ini");
    remove(missingFile.c_str());

    LoadConfig(missingFile, config);

    // log_level=trace in default
    EXPECT_TRUE(config.debugLogging);
    EXPECT_EQ(config.logLevel, LogLevel::Trace);
    EXPECT_EQ(config.captureMethod, "auto");
    EXPECT_FALSE(config.wgcSkipSplitDeviceFlush);
    EXPECT_TRUE(config.wgcSameDeviceCapture);
    EXPECT_TRUE(config.wgcSmoothnessBufferEnabled);
    EXPECT_EQ(config.wgcSmoothnessBufferMaxMs, 250u);
    EXPECT_EQ(config.wgcSmoothnessBufferVramBudgetMb, 2048u);
    EXPECT_EQ(config.video.profile, "auto");
    EXPECT_EQ(config.video.scaling.sharpness, 100);
    EXPECT_FALSE(config.graphics.forceMipBiasClamp);
    EXPECT_EQ(config.graphics.backbufferCount, -1);
    EXPECT_TRUE(config.overlay.captureIncludeOverlay);
    EXPECT_TRUE(config.overlay.screenshotIncludeOverlay);

    std::string generatedText = ReadTextFile(missingFile);
    ASSERT_FALSE(generatedText.empty());
    EXPECT_NE(generatedText.find("; capture_method - Values: inject, wgc, auto"), std::string::npos);
    EXPECT_NE(generatedText.find("capture_method=auto"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_skip_split_device_flush=false"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_same_device_capture=true"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_smoothness_buffer_enabled=true"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_smoothness_buffer_max_ms=250"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_smoothness_buffer_vram_budget_mb=2048"), std::string::npos);
    EXPECT_NE(generatedText.find("profile=auto"), std::string::npos);
    EXPECT_NE(generatedText.find("sharpness=100"), std::string::npos);
    EXPECT_NE(generatedText.find("; backbuffer_count, affecting vsync"), std::string::npos);
    EXPECT_NE(generatedText.find("backbuffer_count=-1"), std::string::npos);
    EXPECT_NE(generatedText.find("capture_include_overlay=true"), std::string::npos);
    EXPECT_NE(generatedText.find("screenshot_include_overlay=true"), std::string::npos);
    EXPECT_EQ(generatedText.find("perf_metrics_logging="), std::string::npos);
    EXPECT_NE(generatedText.find("log_level=trace"), std::string::npos);
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

TEST_F(ConfigTest, ParseLogLevelValues) {
    WriteConfig(
        "[General]\n"
        "log_level=trace\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.logLevel, LogLevel::Trace);
    EXPECT_TRUE(config.debugLogging);
}

TEST_F(ConfigTest, DebugLoggingFalseMapsToOff) {
    WriteConfig(
        "[General]\n"
        "debug_logging=false\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.logLevel, LogLevel::Off);
    EXPECT_FALSE(config.debugLogging);
}

TEST_F(ConfigTest, ParseExplicitWgcCaptureMethod) {
    WriteConfig(
        "[General]\n"
        "capture_method=wgc\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.captureMethod, "wgc");
}

TEST_F(ConfigTest, ParseWgcExperimentalFlags) {
    WriteConfig(
        "[General]\n"
        "wgc_skip_split_device_flush=true\n"
        "wgc_same_device_capture=true\n"
        "wgc_smoothness_buffer_enabled=false\n"
        "wgc_smoothness_buffer_max_ms=125\n"
        "wgc_smoothness_buffer_vram_budget_mb=512\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.wgcSkipSplitDeviceFlush);
    EXPECT_TRUE(config.wgcSameDeviceCapture);
    EXPECT_FALSE(config.wgcSmoothnessBufferEnabled);
    EXPECT_EQ(config.wgcSmoothnessBufferMaxMs, 125u);
    EXPECT_EQ(config.wgcSmoothnessBufferVramBudgetMb, 512u);
}

TEST_F(ConfigTest, LegacyWgcAliasesNormalizeToWgc) {
    const char* aliases[] = {"screengrab", "framegrab", "desktop_dup"};

    for (const char* alias : aliases) {
        WriteConfig(std::string("[General]\n") + "capture_method=" + alias + "\n");

        AppConfig config;
        LoadConfig(tempConfigFile, config);

        EXPECT_EQ(config.captureMethod, "wgc") << alias;
    }
}

TEST_F(ConfigTest, ParseOverlayInclusionOptions) {
    std::string iniContent =
        "[Overlay]\n"
        "enabled=true\n"
        "observer_only=true\n"
        "observer_policy_only=true\n"
        "observer_startup_present_only=true\n"
        "capture_include_overlay=false\n"
        "screenshot_include_overlay=false\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.overlay.showOverlay);
    EXPECT_TRUE(config.overlay.observerOnly);
    EXPECT_TRUE(config.overlay.observerPolicyOnly);
    EXPECT_TRUE(config.overlay.observerStartupPresentOnly);
    EXPECT_FALSE(config.overlay.captureIncludeOverlay);
    EXPECT_FALSE(config.overlay.screenshotIncludeOverlay);
    // dx12_focus_analysis is absent here -> defaults off.
    EXPECT_FALSE(config.overlay.dx12FocusAnalysis);
    EXPECT_FALSE(IsOverlayDx12FocusAnalysis(config.overlay));
}

TEST_F(ConfigTest, ParseDx12FocusAnalysisOption) {
    WriteConfig(
        "[Overlay]\n"
        "enabled=true\n"
        "dx12_focus_analysis=true\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.overlay.dx12FocusAnalysis);
    EXPECT_TRUE(IsOverlayDx12FocusAnalysis(config.overlay));
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

TEST_F(ConfigTest, PseudoOverlayProcessListMultiLine) {
    std::string iniContent =
        "[pseudo-overlay]\n"
        "enabled=true\n"
        "mode=2\n"
        "process_list=(\n"
        "FortniteClient-Win64-Shipping.exe\n"
        "StrangeBrigade_DX12.exe\n"
        ")\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.pseudoOverlay.enabled);
    EXPECT_EQ(config.pseudoOverlay.mode, 2);
    EXPECT_EQ(config.pseudoOverlay.processList, "FortniteClient-Win64-Shipping.exe|StrangeBrigade_DX12.exe");
}

TEST_F(ConfigTest, PseudoOverlayProcessListMultiLineWithComments) {
    std::string iniContent =
        "[pseudo-overlay]\n"
        "enabled=true\n"
        "process_list=(\n"
        "FortniteClient-Win64-Shipping.exe\n"
        ";StrangeBrigade_DX12.exe\n"
        ")\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.pseudoOverlay.enabled);
    EXPECT_EQ(config.pseudoOverlay.processList, "FortniteClient-Win64-Shipping.exe");
}

TEST_F(ConfigTest, PseudoOverlayProcessListMultiLineEmpty) {
    std::string iniContent =
        "[pseudo-overlay]\n"
        "enabled=true\n"
        "process_list=(\n"
        ";FortniteClient-Win64-Shipping.exe\n"
        ";StrangeBrigade_DX12.exe\n"
        ")\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.pseudoOverlay.enabled);
    EXPECT_TRUE(config.pseudoOverlay.processList.empty());
}

TEST_F(ConfigTest, LogLevelNoneMapsToOff) {
    WriteConfig(
        "[General]\n"
        "log_level=none\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.logLevel, LogLevel::Off);
    EXPECT_FALSE(config.debugLogging);
    EXPECT_FALSE(IsAnyLoggingEnabled(config.logLevel));
    EXPECT_FALSE(IsDebugLoggingEnabled(config.logLevel));
}

TEST_F(ConfigTest, LogLevelOffMapsToOff) {
    WriteConfig(
        "[General]\n"
        "log_level=off\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.logLevel, LogLevel::Off);
    EXPECT_FALSE(config.debugLogging);
    EXPECT_FALSE(IsAnyLoggingEnabled(config.logLevel));
    EXPECT_FALSE(IsDebugLoggingEnabled(config.logLevel));
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
    EXPECT_EQ(config.logLevel, LogLevel::Trace);
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
        tempConfigFile = MakeTestPath("test_whitelist_entry.ini");
        remove(tempConfigFile.c_str());
    }

    void TearDown() override {
        remove(tempConfigFile.c_str());
    }

    void WriteConfig(const std::string& content) {
        WriteTextFile(tempConfigFile, content);
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

TEST_F(ConfigTest, ParseNumberedAudioSections) {
    std::string iniContent =
        "[Audio]\n"
        "enabled=true\n"
        "device=\n"
        "codec=flac\n"
        "bitrate=320\n"
        "\n"
        "[Audio.1]\n"
        "enabled=true\n"
        "device=Speakers\n"
        "track=11\n"
        "\n"
        "[Audio.2]\n"
        "enabled=true\n"
        "device=Headphones\n"
        "track=12\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    // Should have: [Audio], [Audio.1], [Audio.2], [Microphone] (disabled)
    size_t sysCount = 0;
    size_t micCount = 0;
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::SystemAudio)
            sysCount++;
        if (src.sourceType == AudioConfig::Microphone)
            micCount++;
    }
    EXPECT_EQ(sysCount, 3);
    EXPECT_EQ(micCount, 0);

    // Check that [Audio.1] and [Audio.2] have correct devices and tracks
    bool foundSpeakers = false, foundHeadphones = false;
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::SystemAudio && src.tracks.size() == 1) {
            if (src.tracks[0] == 11) {
                foundSpeakers = true;
                EXPECT_EQ(src.device, "Speakers");
                EXPECT_EQ(src.codec, "flac");
            }
            if (src.tracks[0] == 12) {
                foundHeadphones = true;
                EXPECT_EQ(src.device, "Headphones");
                EXPECT_EQ(src.codec, "flac");
            }
        }
    }
    EXPECT_TRUE(foundSpeakers);
    EXPECT_TRUE(foundHeadphones);
}

TEST_F(ConfigTest, ParseNumberedMicrophoneSections) {
    std::string iniContent =
        "[Audio]\n"
        "enabled=true\n"
        "codec=opus\n"
        "\n"
        "[Microphone.1]\n"
        "enabled=true\n"
        "device=Blue Yeti\n"
        "track=21\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    bool foundMic = false;
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::Microphone) {
            foundMic = true;
            EXPECT_EQ(src.device, "Blue Yeti");
            EXPECT_TRUE(!src.tracks.empty() && src.tracks[0] == 21);
            EXPECT_EQ(src.codec, "opus");
        }
    }
    EXPECT_TRUE(foundMic);
}

TEST_F(ConfigTest, ParseSuppressLegacyWhenNumbered) {
    std::string iniContent =
        "[Audio.1]\n"
        "enabled=true\n"
        "device=Speakers\n"
        "track=11\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    // Should have: [Audio.1] only (no legacy [Audio], no mic)
    size_t sysCount = 0;
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::SystemAudio)
            sysCount++;
    }
    EXPECT_EQ(sysCount, 1);

    // Verify the single source is the numbered one, not legacy
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::SystemAudio) {
            EXPECT_EQ(src.device, "Speakers");
            EXPECT_TRUE(!src.tracks.empty() && src.tracks[0] == 11);
        }
    }
}

TEST_F(ConfigTest, ParseLegacyWhenNumberedWithExplicitEnabled) {
    std::string iniContent =
        "[Audio]\n"
        "enabled=true\n"
        "\n"
        "[Audio.1]\n"
        "enabled=true\n"
        "device=Speakers\n"
        "track=11\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    // Should have: [Audio] (default) + [Audio.1], no mic
    size_t sysCount = 0;
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::SystemAudio)
            sysCount++;
    }
    EXPECT_EQ(sysCount, 2);
}

TEST_F(ConfigTest, ParseNumberedDisabled) {
    std::string iniContent =
        "[Audio]\n"
        "enabled=true\n"
        "\n"
        "[Audio.1]\n"
        "enabled=false\n"
        "device=Speakers\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    // Should have only [Audio] and disabled mic — [Audio.1] is not added
    bool foundAudio1 = false;
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::SystemAudio && !src.device.empty()) {
            foundAudio1 = true;
        }
    }
    EXPECT_FALSE(foundAudio1);
}

TEST_F(ConfigTest, ParseNumberedEmptySection) {
    std::string iniContent =
        "[Audio]\n"
        "enabled=true\n"
        "\n"
        "[Audio.1]\n"
        // No keys at all — GetStr returns empty, section is skipped
        "\n"
        "[Audio.2]\n"
        "enabled=true\n"
        "device=Headphones\n"
        "track=12\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    // Should have: [Audio] + [Audio.2] (Audio.1 has no keys, skipped)
    size_t sysCount = 0;
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::SystemAudio)
            sysCount++;
    }

    // [Audio] (default device="") + [Audio.2] (device=Headphones)
    EXPECT_EQ(sysCount, 2);

    bool foundHeadphones = false;
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::SystemAudio && src.device == "Headphones") {
            foundHeadphones = true;
        }
    }
    EXPECT_TRUE(foundHeadphones);
}

TEST_F(ConfigTest, ParseAudioNumberedInheritsCodec) {
    std::string iniContent =
        "[Audio]\n"
        "enabled=true\n"
        "codec=alac\n"
        "bitrate=192\n"
        "\n"
        "[Audio.1]\n"
        "enabled=true\n"
        "device=Speakers\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    // [Audio.1] should inherit codec/bitrate from [Audio]
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::SystemAudio && src.device == "Speakers") {
            EXPECT_EQ(src.codec, "alac");
            EXPECT_EQ(src.bitrate, 192);
        }
    }
}

TEST_F(ConfigTest, AudioDerivedSourcesInheritQualityAndDownmix) {
    std::string iniContent =
        "[Audio]\n"
        "enabled=false\n"
        "codec=flac\n"
        "bitrate=384\n"
        "sample_rate=96000\n"
        "bit_depth=24\n"
        "downmix=true\n"
        "\n"
        "[Audio.1]\n"
        "enabled=true\n"
        "device=Speakers\n"
        "track=11\n"
        "\n"
        "[Microphone]\n"
        "enabled=true\n"
        "device=Mic\n"
        "track=12\n"
        "\n"
        "[Microphone.1]\n"
        "enabled=true\n"
        "device=Mic 2\n"
        "track=13\n"
        "\n"
        "[AppAudio.1]\n"
        "enabled=true\n"
        "process=Game.exe\n"
        "track=14\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    bool foundAudio = false;
    bool foundMic = false;
    bool foundMicNumbered = false;
    bool foundApp = false;
    for (const auto& src : config.audioSources) {
        if (src.tracks.empty()) {
            continue;
        }
        if (src.tracks[0] == 11)
            foundAudio = true;
        if (src.tracks[0] == 12)
            foundMic = true;
        if (src.tracks[0] == 13)
            foundMicNumbered = true;
        if (src.tracks[0] == 14)
            foundApp = true;

        EXPECT_EQ(src.codec, "flac");
        EXPECT_EQ(src.bitrate, 384);
        EXPECT_EQ(src.sampleRate, "96000");
        EXPECT_EQ(src.bitDepth, "24");
        EXPECT_TRUE(src.downmix);
    }

    EXPECT_TRUE(foundAudio);
    EXPECT_TRUE(foundMic);
    EXPECT_TRUE(foundMicNumbered);
    EXPECT_TRUE(foundApp);
}

TEST_F(ConfigTest, AppAudioCanOverrideInheritedDownmixAndQuality) {
    std::string iniContent =
        "[Audio]\n"
        "enabled=false\n"
        "codec=alac\n"
        "bitrate=192\n"
        "sample_rate=48000\n"
        "bit_depth=24\n"
        "downmix=true\n"
        "\n"
        "[AppAudio.1]\n"
        "enabled=true\n"
        "process=Game.exe\n"
        "track=3\n"
        "codec=opus\n"
        "bitrate=256\n"
        "sample_rate=44100\n"
        "bit_depth=16\n"
        "downmix=false\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    bool foundApp = false;
    for (const auto& src : config.audioSources) {
        if (src.sourceType == AudioConfig::AppAudio) {
            foundApp = true;
            EXPECT_EQ(src.codec, "opus");
            EXPECT_EQ(src.bitrate, 256);
            EXPECT_EQ(src.sampleRate, "44100");
            EXPECT_EQ(src.bitDepth, "16");
            EXPECT_FALSE(src.downmix);
        }
    }
    EXPECT_TRUE(foundApp);
}

// Regression: capture latency is per device-domain. The render endpoint ([General]
// audio_capture_latency_ms) covers the system loopback AND every app process-loopback source,
// but the microphone is a separate input device and must NOT inherit the render-endpoint value
// (it falls back to mic_capture_latency_ms, default 0). Before the device-domain fix the mic
// inherited the loopback value and got wrongly equalized, delaying it ~46 ms.
TEST_F(ConfigTest, CaptureLatencyIsPerDeviceDomain) {
    std::string iniContent =
        "[General]\n"
        "audio_capture_latency_ms=46\n"
        "\n"
        "[Audio]\n"
        "enabled=true\n"
        "device=Speakers\n"
        "track=1\n"
        "\n"
        "[Audio.1]\n"
        "enabled=true\n"
        "device=Speakers2\n"
        "track=11\n"
        "\n"
        "[Microphone]\n"
        "enabled=true\n"
        "device=Mic\n"
        "track=2\n"
        "\n"
        "[Microphone.1]\n"
        "enabled=true\n"
        "device=Mic2\n"
        "track=13\n"
        "\n"
        "[AppAudio.1]\n"
        "enabled=true\n"
        "process=Game.exe\n"
        "track=3\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_FLOAT_EQ(config.audioCaptureLatencyMs, 46.0f);
    EXPECT_FLOAT_EQ(config.micCaptureLatencyMs, 0.0f);
    EXPECT_TRUE(config.audioLatencyAutodetect);

    bool sawSystem = false, sawSystemNumbered = false, sawApp = false, sawMic = false, sawMicNumbered = false;
    for (const auto& src : config.audioSources) {
        if (src.tracks.empty())
            continue;
        switch (src.sourceType) {
            case AudioConfig::SystemAudio:
                // Render-endpoint domain: inherits the [General] value.
                EXPECT_FLOAT_EQ(src.captureLatencyMs, 46.0f)
                    << "system loopback should inherit render-endpoint latency";
                if (src.tracks[0] == 1)
                    sawSystem = true;
                if (src.tracks[0] == 11)
                    sawSystemNumbered = true;
                break;
            case AudioConfig::AppAudio:
                // Same render endpoint: inherits the [General] value.
                EXPECT_FLOAT_EQ(src.captureLatencyMs, 46.0f) << "app process-loopback shares the render endpoint";
                sawApp = true;
                break;
            case AudioConfig::Microphone:
                // Separate input domain: must NOT inherit the render-endpoint value.
                EXPECT_FLOAT_EQ(src.captureLatencyMs, 0.0f) << "microphone must not inherit render-endpoint latency";
                if (src.tracks[0] == 2)
                    sawMic = true;
                if (src.tracks[0] == 13)
                    sawMicNumbered = true;
                break;
        }
    }
    EXPECT_TRUE(sawSystem);
    EXPECT_TRUE(sawSystemNumbered);
    EXPECT_TRUE(sawApp);
    EXPECT_TRUE(sawMic);
    EXPECT_TRUE(sawMicNumbered);
}

// A per-mic capture_latency_ms override and the [General] mic_capture_latency_ms fallback both
// apply to the mic domain independently of the render-endpoint value.
TEST_F(ConfigTest, MicLatencyOverrideAndFallback) {
    std::string iniContent =
        "[General]\n"
        "audio_capture_latency_ms=46\n"
        "mic_capture_latency_ms=5\n"
        "\n"
        "[Microphone]\n"
        "enabled=true\n"
        "device=Mic\n"
        "track=2\n"
        "\n"
        "[Microphone.1]\n"
        "enabled=true\n"
        "device=Mic2\n"
        "track=13\n"
        "capture_latency_ms=8\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_FLOAT_EQ(config.micCaptureLatencyMs, 5.0f);
    for (const auto& src : config.audioSources) {
        if (src.sourceType != AudioConfig::Microphone || src.tracks.empty())
            continue;
        if (src.tracks[0] == 2)
            EXPECT_FLOAT_EQ(src.captureLatencyMs, 5.0f);  // [General] mic fallback
        if (src.tracks[0] == 13)
            EXPECT_FLOAT_EQ(src.captureLatencyMs, 8.0f);  // per-mic override
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
