#include "test_config_shared.h"

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

TEST_F(ConfigTest, DesktopOverlayProcessListMultiLine) {
    std::string iniContent =
        "[DesktopOverlay]\n"
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
    WriteConfig(
        "[General]\n"
        "wgc_smoothness_buffer_max_ms=300\n"
        "wgc_smoothness_floor_ms=3oops\n"
        "[Performance]\n"
        "gpu_priority=8\n"
        "[Graphics]\n"
        "backbuffer_count=7\n"
        "mip_bias=nan\n"
        "[FpsLimiter]\n"
        "capture_sync_multiplier=0\n"
        "general_fps=1001\n"
        "[Overlay]\n"
        "hdr_paper_white=nan\n"
        "[Video]\n"
        "fps=0\n"
        "b_frames=5\n"
        "hdr_nominal_peak_nits=99\n"
        "[Screenshot]\n"
        "color_space=display-p3\n"
        "[MediaFoundation]\n"
        "quality=101\n"
        "[Scaling]\n"
        "sharpness=-1\n"
        "[AppAudio.1]\n"
        "enabled=true\n"
        "process_id=1oops\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.video.gpuPriority, 7);
    EXPECT_EQ(config.graphics.backbufferCount, -1);
    EXPECT_EQ(config.graphics.mipBias, "default");
    EXPECT_EQ(config.fpsLimiter.captureSyncMultiplier, 1);
    EXPECT_EQ(config.fpsLimiter.generalFps, 120);
    EXPECT_FLOAT_EQ(config.overlay.hdrPaperWhite, 0.0f);
    EXPECT_EQ(config.video.fps, 120);
    EXPECT_EQ(config.video.bFrames, 0);
    EXPECT_EQ(config.video.hdrNominalPeakNits, 1000);
    EXPECT_EQ(config.screenshotColorSpace, "auto");
    EXPECT_EQ(config.video.mfQuality, 80);
    EXPECT_EQ(config.video.scaling.sharpness, 100);
    EXPECT_TRUE(config.wgcSmoothnessFloorAuto);
    EXPECT_EQ(config.wgcSmoothnessFloorMs, 0u);
    ASSERT_EQ(config.audioSources.size(), 1u);
    EXPECT_EQ(config.audioSources.front().sourceType, AudioConfig::SystemAudio);
}

TEST_F(ConfigTest, HdrNominalPeakAcceptsDocumentedRange) {
    WriteConfig(
        "[Video]\n"
        "hdr_nominal_peak_nits=1600\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.video.hdrNominalPeakNits, 1600);
}

TEST_F(ConfigTest, BooleanTyposUseDocumentedFallbacks) {
    WriteConfig(
        "[Overlay]\n"
        "enabled=perhaps\n"
        "capture_include_overlay=perhaps\n"
        "[Video]\n"
        "capture_cursor=perhaps\n"
        "[Audio]\n"
        "enabled=perhaps\n"
        "[Microphone]\n"
        "enabled=perhaps\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.overlay.showOverlay);
    EXPECT_TRUE(config.overlay.captureIncludeOverlay);
    EXPECT_TRUE(config.video.captureCursor);
    ASSERT_EQ(config.audioSources.size(), 1u);
    EXPECT_EQ(config.audioSources.front().sourceType, AudioConfig::SystemAudio);
}

TEST_F(ConfigTest, TrackIdsAreBoundedAndDeduplicated) {
    WriteConfig(
        "[Audio]\n"
        "enabled=true\n"
        "track=1, 0, 256, invalid, 1, 3\n"
        "[Microphone]\n"
        "enabled=true\n"
        "track=-1, 2, 2, 999\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    ASSERT_EQ(config.audioSources.size(), 2u);
    EXPECT_EQ(config.audioSources[0].tracks, (std::vector<int>{1, 3}));
    EXPECT_EQ(config.audioSources[1].tracks, (std::vector<int>{2}));
}

TEST_F(ConfigTest, EntirelyInvalidTrackListUsesSectionDefault) {
    WriteConfig(
        "[Audio]\n"
        "enabled=true\n"
        "track=0, 256, invalid\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    ASSERT_EQ(config.audioSources.size(), 1u);
    EXPECT_EQ(config.audioSources.front().tracks, (std::vector<int>{1}));
}

TEST_F(ConfigTest, OverlayAndPseudoOverlayValuesRespectDocumentedBounds) {
    WriteConfig(
        "[Overlay]\n"
        "padding=-1\n"
        "font_size=-2\n"
        "rounded_corners=-3\n"
        "bg_alpha=1.5\n"
        "bg_color=1234567\n"
        "fps_color=#112233\n"
        "text_outline_thickness=-1\n"
        "text_update_interval=-1\n"
        "[pseudo-overlay]\n"
        "size=0\n"
        "pad=101\n"
        "pos=4\n"
        "mode=3\n"
        "foreground_acquire_grace_ms=10001\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.overlay.padding, 10);
    EXPECT_FLOAT_EQ(config.overlay.fontSize, 0.0f);
    EXPECT_FLOAT_EQ(config.overlay.roundedCorners, 8.0f);
    EXPECT_FLOAT_EQ(config.overlay.bgAlpha, 0.5f);
    EXPECT_EQ(config.overlay.bgColor, 0xFF000000u);
    EXPECT_EQ(config.overlay.fpsColor, 0xFF332211u);
    EXPECT_FLOAT_EQ(config.overlay.textOutlineThickness, 1.5f);
    EXPECT_EQ(config.overlay.textUpdateInterval, 500u);
    EXPECT_EQ(config.pseudoOverlay.size, 30);
    EXPECT_EQ(config.pseudoOverlay.pad, 20);
    EXPECT_EQ(config.pseudoOverlay.pos, 0);
    EXPECT_EQ(config.pseudoOverlay.mode, 0);
    EXPECT_EQ(config.pseudoOverlay.foregroundAcquireGraceMs, 2000);
}

TEST_F(ConfigTest, ParseGraphicsOverrideOptions) {
    std::string iniContent =
        "[Graphics]\n"
        "mip_mapping=trilinear\n"
        "force_mip_bias_clamp=true\n"
        "nv_lod_spread_fix=on\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.graphics.mipMapping, "trilinear");
    EXPECT_TRUE(config.graphics.forceMipBiasClamp);
    EXPECT_TRUE(config.graphics.nvLodSpreadFix);
}

TEST_F(ConfigTest, ParsesRayReconstructionForcePolicyFromDlssAndLegacyGraphicsSections) {
    WriteConfig("[DLSS]\nforce_ray_reconstruction=on\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_TRUE(config.graphics.forceRayReconstruction);

    WriteConfig("[Graphics]\nforce_ray_reconstruction=true\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_TRUE(config.graphics.forceRayReconstruction);

    WriteConfig("[DLSS]\nforce_ray_reconstruction=off\n"
                "[Graphics]\nforce_ray_reconstruction=true\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FALSE(config.graphics.forceRayReconstruction);
}

TEST_F(ConfigTest, NormalizesAndValidatesMipMappingMode) {
    WriteConfig("[Graphics]\nmip_mapping=BiLiNeAr\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.mipMapping, "bilinear");

    WriteConfig("[Graphics]\nmip_mapping=unsupported\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.mipMapping, "default");
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
    EXPECT_EQ(ParseDlssPreset(" A "), 1u);
    EXPECT_EQ(ParseDlssPreset("A-suffix"), 0u);
    EXPECT_EQ(ParseDlssRRPreset("AB"), 0u);
}

TEST(ConfigHelpersTest, DlssFGPresetParsingMatchesTheOtherPresetFamilies) {
    // NVIDIA currently defines only A and B for frame generation, but the driver
    // selection is a 1-based index, so later letters must survive parsing.
    EXPECT_EQ(ParseDlssFGPreset("default"), 0u);
    EXPECT_EQ(ParseDlssFGPreset(""), 0u);
    EXPECT_EQ(ParseDlssFGPreset("A"), 1u);
    EXPECT_EQ(ParseDlssFGPreset("b"), 2u);
    EXPECT_EQ(ParseDlssFGPreset(" C "), 3u);
    EXPECT_EQ(ParseDlssFGPreset("Z"), 26u);
    EXPECT_EQ(ParseDlssFGPreset("AB"), 0u);
    EXPECT_EQ(ParseDlssFGPreset("1"), 0u);
    EXPECT_EQ(ParseDlssFGPreset("A-suffix"), 0u);
}

TEST(ConfigHelpersTest, DlssFGPresetSharedMemoryNormalizationRejectsOutOfRange) {
    EXPECT_EQ(NormalizeDLSSFGPreset(0u), 0u);
    EXPECT_EQ(NormalizeDLSSFGPreset(1u), 1u);
    EXPECT_EQ(NormalizeDLSSFGPreset(26u), 26u);
    EXPECT_EQ(NormalizeDLSSFGPreset(27u), 0u);
    EXPECT_EQ(NormalizeDLSSFGPreset(0xFFFFFFFFu), 0u);
}

TEST(ConfigHelpersTest, NvngxParameterVtableSlotsMatchSdkAbi) {
    EXPECT_EQ(ce::nvngx_parameter_abi::kSetI, 3u);
    EXPECT_EQ(ce::nvngx_parameter_abi::kSetUI, 4u);
    EXPECT_EQ(ce::nvngx_parameter_abi::kSetF, 6u);
    EXPECT_EQ(ce::nvngx_parameter_abi::kGetI, 11u);
    EXPECT_EQ(ce::nvngx_parameter_abi::kGetUI, 12u);
}

TEST(ConfigHelpersTest, DlssSharpeningParsingRejectsMalformedAndOutOfRangeValues) {
    EXPECT_FLOAT_EQ(ParseDlssSharpening("default"), -2.0f);
    EXPECT_FLOAT_EQ(ParseDlssSharpening("off"), -1.0f);
    EXPECT_FLOAT_EQ(ParseDlssSharpening("0.625"), 0.625f);
    EXPECT_FLOAT_EQ(ParseDlssSharpening("1.0suffix"), -2.0f);
    EXPECT_FLOAT_EQ(ParseDlssSharpening("nan"), -2.0f);
    EXPECT_FLOAT_EQ(ParseDlssSharpening("-0.1"), -2.0f);
    EXPECT_FLOAT_EQ(ParseDlssSharpening("1.1"), -2.0f);
}

TEST_F(ConfigTest, GraphicsQueueAndSamplerModesAreValidated) {
    WriteConfig(
        "[Graphics]\n"
        "sampler_override_mode=AGGRESSIVE\n"
        "cpu_prerender_limit=2.5\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.graphics.samplerOverrideMode, "aggressive");
    EXPECT_FLOAT_EQ(config.graphics.cpuPrerenderLimit, -1.0f);
}

TEST(ConfigHelpersTest, MatchModeParsing) {
    EXPECT_EQ(ParseMatchMode("exact"), MatchMode::kExact);
    EXPECT_EQ(ParseMatchMode("contains"), MatchMode::kTitleExecutable);
    EXPECT_EQ(ParseMatchMode("contains_or_class"), MatchMode::kTitleType);
    EXPECT_EQ(ParseMatchMode("title_executable"), MatchMode::kTitleExecutable);
    EXPECT_EQ(ParseMatchMode("title_exec"), MatchMode::kTitleExecutable);
    EXPECT_EQ(ParseMatchMode("title_type"), MatchMode::kTitleType);
    EXPECT_EQ(ParseMatchMode("title_class"), MatchMode::kTitleType);
    EXPECT_EQ(ParseMatchMode("invalid"), MatchMode::kExact);
    EXPECT_EQ(ParseMatchMode(""), MatchMode::kExact);
}

TEST(ConfigHelpersTest, CanonicalProfileProcessNamesStayExactAcrossWindowMatchModes) {
    WhitelistEntry entry;
    entry.pattern = "game.exe";
    entry.mode = MatchMode::kTitleExecutable;

    EXPECT_TRUE(MatchesProcessName(entry, "GAME.EXE", true));
    EXPECT_FALSE(MatchesProcessName(entry, "mygame.exe", true));
    EXPECT_TRUE(MatchesProcessName(entry, "mygame.exe", false));
}

TEST(ConfigHelpersTest, MatchModeToString) {
    EXPECT_STREQ(MatchModeToString(MatchMode::kExact), "exact");
    EXPECT_STREQ(MatchModeToString(MatchMode::kTitleExecutable), "title_executable");
    EXPECT_STREQ(MatchModeToString(MatchMode::kTitleType), "title_type");
}

TEST(ConfigHelpersTest, ParseHotkeyModifierAndKeyCombinations) {
    // Basic function key
    auto hk = ParseHotkey("F9");
    EXPECT_EQ(hk.vkey, VK_F9);
    EXPECT_FALSE(hk.ctrl);
    EXPECT_FALSE(hk.shift);
    EXPECT_FALSE(hk.alt);
    EXPECT_FALSE(hk.win);

    // Single modifier
    hk = ParseHotkey("Ctrl+F9");
    EXPECT_EQ(hk.vkey, VK_F9);
    EXPECT_TRUE(hk.ctrl);
    EXPECT_FALSE(hk.shift);

    hk = ParseHotkey("Shift+F10");
    EXPECT_EQ(hk.vkey, VK_F10);
    EXPECT_TRUE(hk.shift);

    hk = ParseHotkey("Alt+R");
    EXPECT_EQ(hk.vkey, 'R');
    EXPECT_TRUE(hk.alt);

    hk = ParseHotkey("Win+Home");
    EXPECT_EQ(hk.vkey, VK_HOME);
    EXPECT_TRUE(hk.win);

    // Multiple modifiers
    hk = ParseHotkey("Ctrl+Shift+F9");
    EXPECT_EQ(hk.vkey, VK_F9);
    EXPECT_TRUE(hk.ctrl);
    EXPECT_TRUE(hk.shift);
    EXPECT_FALSE(hk.alt);

    hk = ParseHotkey("Alt+Ctrl+Shift+F1");
    EXPECT_EQ(hk.vkey, VK_F1);
    EXPECT_TRUE(hk.alt);
    EXPECT_TRUE(hk.ctrl);
    EXPECT_TRUE(hk.shift);

    // MINUS / DASH / HYPHEN aliases (physical - key, VK_OEM_MINUS)
    hk = ParseHotkey("Ctrl+Minus");
    EXPECT_EQ(hk.vkey, VK_OEM_MINUS);
    EXPECT_TRUE(hk.ctrl);

    hk = ParseHotkey("Alt+Shift+Dash");
    EXPECT_EQ(hk.vkey, VK_OEM_MINUS);
    EXPECT_TRUE(hk.alt);
    EXPECT_TRUE(hk.shift);

    hk = ParseHotkey("Ctrl+Win+Hyphen");
    EXPECT_EQ(hk.vkey, VK_OEM_MINUS);
    EXPECT_TRUE(hk.ctrl);
    EXPECT_TRUE(hk.win);

    // PLUS / EQUALS aliases (physical = key, VK_OEM_PLUS)
    hk = ParseHotkey("Ctrl+Plus");
    EXPECT_EQ(hk.vkey, VK_OEM_PLUS);
    EXPECT_TRUE(hk.ctrl);

    hk = ParseHotkey("Shift+Equals");
    EXPECT_EQ(hk.vkey, VK_OEM_PLUS);
    EXPECT_TRUE(hk.shift);

    // Plain minus/plus without modifiers
    hk = ParseHotkey("Minus");
    EXPECT_EQ(hk.vkey, VK_OEM_MINUS);

    hk = ParseHotkey("Plus");
    EXPECT_EQ(hk.vkey, VK_OEM_PLUS);

    // Arrow keys
    hk = ParseHotkey("Alt+Right");
    EXPECT_EQ(hk.vkey, VK_RIGHT);
    EXPECT_TRUE(hk.alt);

    hk = ParseHotkey("Ctrl+Shift+Left");
    EXPECT_EQ(hk.vkey, VK_LEFT);
    EXPECT_TRUE(hk.ctrl);
    EXPECT_TRUE(hk.shift);

    // Alphanumeric keys
    hk = ParseHotkey("Ctrl+5");
    EXPECT_EQ(hk.vkey, '5');
    EXPECT_TRUE(hk.ctrl);

    hk = ParseHotkey("Alt+Shift+A");
    EXPECT_EQ(hk.vkey, 'A');
    EXPECT_TRUE(hk.alt);
    EXPECT_TRUE(hk.shift);

    // Named keys
    hk = ParseHotkey("Ctrl+Space");
    EXPECT_EQ(hk.vkey, VK_SPACE);
    EXPECT_TRUE(hk.ctrl);

    hk = ParseHotkey("Alt+Enter");
    EXPECT_EQ(hk.vkey, VK_RETURN);
    EXPECT_TRUE(hk.alt);

    // Windows/Control aliases
    hk = ParseHotkey("Control+F1");
    EXPECT_EQ(hk.vkey, VK_F1);
    EXPECT_TRUE(hk.ctrl);

    hk = ParseHotkey("Windows+F2");
    EXPECT_EQ(hk.vkey, VK_F2);
    EXPECT_TRUE(hk.win);

    // Empty/unrecognized returns vkey=0
    hk = ParseHotkey("");
    EXPECT_EQ(hk.vkey, 0);

    hk = ParseHotkey("UnknownKey");
    EXPECT_EQ(hk.vkey, 0);
}

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
        "codec=flac\n"
        "bitrate=999\n"
        "sample_rate=48000\n"
        "bit_depth=24\n"
        "downmix=true\n"
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
                EXPECT_EQ(src.bitrate, 999);
                EXPECT_EQ(src.sampleRate, "48000");
                EXPECT_EQ(src.bitDepth, "24");
                EXPECT_TRUE(src.downmix);
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
