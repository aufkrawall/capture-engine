#include "test_config_shared.h"

TEST_F(ConfigTest, LoadDefaultsWhenFileMissing) {
    AppConfig config;
    // Use a non-existent absolute path to force default creation logic if
    // applicable Or just ensure it doesn't find a file. LoadConfig creates a
    // default file if missing, so we should check THAT file or the loaded values.

    std::string missingFile = MakeTestPath("nonexistent.ini");
    remove(missingFile.c_str());

    LoadConfig(missingFile, config);

    // The shipped config now defaults to trace-level logging.
    EXPECT_TRUE(config.debugLogging);
    EXPECT_EQ(config.logLevel, LogLevel::Trace);
    EXPECT_EQ(config.captureMethod, "auto");
    EXPECT_EQ(config.captureMonitor, "auto");
    EXPECT_FALSE(config.wgcSkipSplitDeviceFlush);
    EXPECT_TRUE(config.wgcSameDeviceCapture);
    EXPECT_TRUE(config.wgcSmoothnessBufferEnabled);
    EXPECT_EQ(config.wgcSmoothnessBufferMaxMs, 300u);
    EXPECT_EQ(config.wgcSmoothnessBufferVramBudgetMb, 3000u);
    EXPECT_EQ(config.wgcVideoMemoryReservation, "off");
    EXPECT_TRUE(config.wgcSmoothnessFloorAuto);
    EXPECT_FALSE(config.wgcAllowLossyBgra8Pool);
    EXPECT_EQ(config.processPriority, "high");
    EXPECT_EQ(config.video.gpuPriority, 7);
    EXPECT_EQ(config.gpuSchedulingPriority, "auto");
    EXPECT_EQ(config.copyQueuePriority, "normal");
    EXPECT_EQ(config.video.profile, "auto");
    EXPECT_EQ(config.video.multipass, "disabled");
    EXPECT_EQ(config.video.splitEncode, "0");
    EXPECT_EQ(config.video.lookahead, "off");
    EXPECT_FALSE(config.video.spatialAq);
    EXPECT_FALSE(config.video.temporalAq);
    EXPECT_EQ(config.video.aqStrength, 8);
    EXPECT_EQ(config.video.bRefMode, "disabled");
    EXPECT_EQ(config.video.bitDepth, "auto");
    EXPECT_EQ(config.video.hdrNominalPeakNits, 1000);
    EXPECT_EQ(config.video.scaling.sharpness, 100);
    EXPECT_FALSE(config.graphics.forceMipBiasClamp);
    EXPECT_EQ(config.graphics.backbufferCount, -1);
    EXPECT_TRUE(config.overlay.captureIncludeOverlay);
    EXPECT_TRUE(config.overlay.screenshotIncludeOverlay);
    EXPECT_FLOAT_EQ(config.overlay.bgAlpha, 0.5f);
    EXPECT_EQ(config.overlay.fpsColor, 0xFF05FAB8u);
    EXPECT_TRUE(config.pseudoOverlay.enabled);
    EXPECT_EQ(config.pseudoOverlay.mode, 2);
    EXPECT_FALSE(config.pseudoOverlay.showEncoderOverloadWarn);
    EXPECT_EQ(config.pseudoOverlay.foregroundAcquireGraceMs, 2000);
    ASSERT_EQ(config.audioSources.size(), 1u);
    EXPECT_EQ(config.audioSources.front().sourceType, AudioConfig::SystemAudio);
    EXPECT_EQ(config.audioSources.front().tracks, (std::vector<int>{1}));

    std::string generatedText = ReadTextFile(missingFile);
    ASSERT_FALSE(generatedText.empty());
    const std::string sourceTemplate = ReadTextFile(DefaultTemplatePath());
    ASSERT_FALSE(sourceTemplate.empty());
    EXPECT_EQ(generatedText, sourceTemplate);
    EXPECT_NE(generatedText.find("capture_method=auto"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_skip_split_device_flush=false"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_same_device_capture=true"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_smoothness_buffer_enabled=true"), std::string::npos);
    EXPECT_NE(generatedText.find("monitor=auto"), std::string::npos);
    EXPECT_NE(generatedText.find("CaptureEngine.exe --list-monitors"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_smoothness_buffer_max_ms=300"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_smoothness_buffer_vram_budget_mb=3000"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_video_memory_reservation=off"), std::string::npos);
    EXPECT_NE(generatedText.find("wgc_allow_lossy_bgra8_pool=false"), std::string::npos);
    EXPECT_NE(generatedText.find("gpu_scheduling_priority=auto"), std::string::npos);
    EXPECT_EQ(generatedText.find("copy_queue_priority"), std::string::npos);
    EXPECT_NE(generatedText.find("profile=auto"), std::string::npos);
    EXPECT_NE(generatedText.find("multipass=disabled"), std::string::npos);
    EXPECT_NE(generatedText.find("auto - selects qres for CBR or when b_frames>0"), std::string::npos);
    EXPECT_NE(generatedText.find("disabled - guarantees single-pass"), std::string::npos);
    EXPECT_NE(generatedText.find("split_encode=0"), std::string::npos);
    EXPECT_NE(generatedText.find("1 requests splitting when multiple engines exist"), std::string::npos);
    EXPECT_NE(generatedText.find("lookahead=off"), std::string::npos);
    EXPECT_NE(generatedText.find("auto enables a 20-frame depth"), std::string::npos);
    EXPECT_NE(generatedText.find("spatial_aq=false"), std::string::npos);
    EXPECT_NE(generatedText.find("temporal_aq=false"), std::string::npos);
    EXPECT_NE(generatedText.find("aq_strength=8"), std::string::npos);
    EXPECT_NE(generatedText.find("For an OBS-like enabled setting"), std::string::npos);
    EXPECT_NE(generatedText.find("default uses 48000 Hz"), std::string::npos);
    EXPECT_NE(generatedText.find("false keeps the source's channel layout"), std::string::npos);
    EXPECT_NE(generatedText.find("b_ref_mode=disabled"), std::string::npos);
    EXPECT_NE(generatedText.find("auto - chooses middle when b_frames>0"), std::string::npos);
    EXPECT_NE(generatedText.find("bit_depth=auto"), std::string::npos);
    EXPECT_NE(generatedText.find("hdr_nominal_peak_nits=1000"), std::string::npos);
    EXPECT_EQ(generatedText.find("\naq="), std::string::npos);
    EXPECT_EQ(generatedText.find("\nmsaa_samples="), std::string::npos);
    EXPECT_EQ(generatedText.find("\nsgssaa="), std::string::npos);
    EXPECT_EQ(generatedText.find("\ndisable_auto_mip_bias="), std::string::npos);
    EXPECT_EQ(generatedText.find(";[AppAudio.1]"), std::string::npos);
    const size_t diagnosticsSection = generatedText.find("\n[Diagnostics]\n");
    const size_t profileExample = generatedText.find(";[Profile.My Game]");
    ASSERT_NE(diagnosticsSection, std::string::npos);
    ASSERT_NE(profileExample, std::string::npos);
    EXPECT_GT(profileExample, diagnosticsSection);
    EXPECT_NE(generatedText.find(";video_capture=inherit", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";monitor=window", profileExample), std::string::npos);
    EXPECT_EQ(generatedText.find(";video_capture=global", profileExample), std::string::npos);
    const size_t practicalProfileExample = generatedText.find(";[Profile.hots]", profileExample);
    ASSERT_NE(practicalProfileExample, std::string::npos);
    EXPECT_NE(generatedText.find(";process=HeroesOfTheStorm_x64.exe", practicalProfileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";dll_injection=never", practicalProfileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";video_capture=dxgi_dup", practicalProfileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";DesktopOverlay.enabled=true", practicalProfileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";audio_enabled=true", practicalProfileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";audio_track=1,2", practicalProfileExample), std::string::npos);
    const size_t injectedProfileExample = generatedText.find(";[Profile.Injected Example]", practicalProfileExample);
    ASSERT_NE(injectedProfileExample, std::string::npos);
    EXPECT_NE(generatedText.find(";process=InjectedGame.exe", injectedProfileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";video_capture=inject", injectedProfileExample), std::string::npos);
    const size_t dxgiOverlayProfileExample =
        generatedText.find(";[Profile.DXGI Overlay Example]", injectedProfileExample);
    ASSERT_NE(dxgiOverlayProfileExample, std::string::npos);
    EXPECT_EQ(generatedText.substr(injectedProfileExample, dxgiOverlayProfileExample - injectedProfileExample)
                  .find(";dll_injection="),
              std::string::npos);
    EXPECT_NE(generatedText.find(";process=DxgiOverlayGame.exe", dxgiOverlayProfileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";video_capture=dxgi_dup", dxgiOverlayProfileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";dll_injection=always", dxgiOverlayProfileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";dll_injection=never", profileExample), std::string::npos);
    EXPECT_EQ(generatedText.find(";injection_mode=", profileExample), std::string::npos);
    EXPECT_EQ(generatedText.find(";injection=normal", profileExample), std::string::npos);
    EXPECT_EQ(generatedText.find(";dll_injection=when_needed", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find("no second setting is needed", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find("DLL injection normally follows the selected video source", profileExample),
              std::string::npos);
    EXPECT_NE(generatedText.find("dll_injection is only needed to override", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find("always = also load the DLL", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find("never = block DLL loading", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find("when_needed = the normal behavior", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find("No dll_injection setting is needed"), std::string::npos);
    EXPECT_NE(generatedText.find(";window_title=My Game", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";window_match=contains", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";audio_enabled=true", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find(";DesktopOverlay.enabled=true", profileExample), std::string::npos);
    EXPECT_NE(generatedText.find("automatically a NOT RECORDING warning target"), std::string::npos);
    EXPECT_NE(generatedText.find("Choosing inject or always can trigger anti-cheat protection"), std::string::npos);
    EXPECT_NE(generatedText.find("output_dir and screenshot_dir are independent"), std::string::npos);
    EXPECT_NE(generatedText.find("auto saves an HDR source as a native 10-bit BT.2020/PQ AVIF"), std::string::npos);
    EXPECT_NE(generatedText.find("start_stop, which falls back to F9"), std::string::npos);
    EXPECT_NE(generatedText.find("Seconds between keyframes"), std::string::npos);
    EXPECT_NE(generatedText.find("Quality target from 0 to 100"), std::string::npos);
    EXPECT_NE(generatedText.find("always_render_only_when_game limits that keepalive"), std::string::npos);
    EXPECT_NE(generatedText.find("Capture sync limits an injected application"), std::string::npos);
    EXPECT_NE(generatedText.find("Shows the frame-time graph"), std::string::npos);
    EXPECT_NE(generatedText.find("vsync_mode: default, off, fifo"), std::string::npos);
    EXPECT_NE(generatedText.find("Sharpening accepts default, off"), std::string::npos);
    EXPECT_NE(generatedText.find("audio_capture_latency_ms=0 measures"), std::string::npos);
    EXPECT_NE(generatedText.find("sharpness=100"), std::string::npos);
    EXPECT_NE(generatedText.find("Other valid values are 2-6"), std::string::npos);
    EXPECT_NE(generatedText.find("backbuffer_count=-1"), std::string::npos);
    EXPECT_NE(generatedText.find("capture_include_overlay=true"), std::string::npos);
    EXPECT_NE(generatedText.find("screenshot_include_overlay=true"), std::string::npos);
    EXPECT_NE(generatedText.find("audio_only="), std::string::npos);
    EXPECT_NE(generatedText.find("foreground_acquire_grace_ms=2000"), std::string::npos);
    EXPECT_NE(generatedText.find("process_list fallback is only needed"), std::string::npos);
    EXPECT_NE(generatedText.find("enabled=false\n"), std::string::npos);
    EXPECT_EQ(generatedText.find("perf_metrics_logging="), std::string::npos);
    EXPECT_NE(generatedText.find("log_level=trace"), std::string::npos);
    EXPECT_EQ(generatedText.find("nvidia_smooth_motion_compat="), std::string::npos);
    EXPECT_NE(generatedText.find("\n[Capture]\n"), std::string::npos);
    EXPECT_NE(generatedText.find("\n[WGC]\n"), std::string::npos);
    EXPECT_NE(generatedText.find("\n[Output]\n"), std::string::npos);
    EXPECT_NE(generatedText.find("\n[SystemAudio]\n"), std::string::npos);
    EXPECT_NE(generatedText.find("\n[DesktopOverlay]\n"), std::string::npos);
    EXPECT_NE(generatedText.find("\n[Diagnostics]\n"), std::string::npos);
    EXPECT_EQ(generatedText.find("\n[General]\n"), std::string::npos);
    EXPECT_EQ(generatedText.find("\n[Scaling]\n"), std::string::npos);
    EXPECT_EQ(generatedText.find("\n[pseudo-overlay]\n"), std::string::npos);
    EXPECT_NE(generatedText.find("\n[Screenshot]\n"), std::string::npos);
    EXPECT_EQ(generatedText.find("\n[Injection]\n"), std::string::npos);
    EXPECT_EQ(generatedText.find("whitelist="), std::string::npos);
    EXPECT_EQ(generatedText.find("wgc_window_detection="), std::string::npos);
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

TEST_F(ConfigTest, ParseIndependentNvencQualityPolicies) {
    WriteConfig(
        "[NVENC]\n"
        "multipass=fullres\n"
        "split_encode=3\n"
        "lookahead=13\n"
        "spatial_aq=true\n"
        "temporal_aq=false\n"
        "aq_strength=9\n"
        "b_ref_mode=middle\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.video.multipass, "fullres");
    EXPECT_EQ(config.video.splitEncode, "3");
    EXPECT_EQ(config.video.lookahead, "13");
    EXPECT_TRUE(config.video.spatialAq);
    EXPECT_FALSE(config.video.temporalAq);
    EXPECT_EQ(config.video.aqStrength, 9);
    EXPECT_EQ(config.video.bRefMode, "middle");
}

TEST_F(ConfigTest, MissingNvencSplitEncodeDefaultsOff) {
    WriteConfig("[NVENC]\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.video.splitEncode, "0");
}

TEST_F(ConfigTest, LegacyNvencAqOnlySuppliesMissingSplitAqValues) {
    WriteConfig("[NVENC]\naq=true\n");
    AppConfig legacyOnly;
    LoadConfig(tempConfigFile, legacyOnly);
    EXPECT_TRUE(legacyOnly.video.spatialAq);
    EXPECT_TRUE(legacyOnly.video.temporalAq);

    WriteConfig(
        "[NVENC]\n"
        "aq=true\n"
        "spatial_aq=false\n"
        "temporal_aq=false\n");
    AppConfig canonicalWins;
    LoadConfig(tempConfigFile, canonicalWins);
    EXPECT_FALSE(canonicalWins.video.spatialAq);
    EXPECT_FALSE(canonicalWins.video.temporalAq);
}

TEST_F(ConfigTest, ParseAmfQuickSyncAndMediaFoundationPolicies) {
    WriteConfig(
        "[Video]\n"
        "buffer_size=240Mbps\n"
        "[AMF]\n"
        "usage=high_quality\n"
        "preset=quality\n"
        "qp=31\n"
        "async_depth=12\n"
        "preencode=true\n"
        "preanalysis=true\n"
        "lookahead=17\n"
        "spatial_aq=true\n"
        "temporal_aq=true\n"
        "aq_strength=2\n"
        "high_motion_quality_boost=true\n"
        "b_ref_mode=enabled\n"
        "enforce_hrd=true\n"
        "filler_data=true\n"
        "[QuickSync]\n"
        "preset=slow\n"
        "qp=27\n"
        "async_depth=8\n"
        "low_power=enabled\n"
        "lookahead=24\n"
        "mbbrc=enabled\n"
        "extbrc=enabled\n"
        "adaptive_i=disabled\n"
        "adaptive_b=enabled\n"
        "low_delay_brc=disabled\n"
        "scenario=archive\n"
        "[MediaFoundation]\n"
        "scenario=camera_record\n"
        "quality_vs_speed=73\n"
        "low_latency=true\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.video.bufferSize, "240Mbps");
    EXPECT_EQ(config.video.amfUsage, "high_quality");
    EXPECT_EQ(config.video.amfPreset, "quality");
    EXPECT_EQ(config.video.amfQp, 31);
    EXPECT_EQ(config.video.amfAsyncDepth, 12);
    EXPECT_TRUE(config.video.amfPreencode);
    EXPECT_TRUE(config.video.amfPreanalysis);
    EXPECT_EQ(config.video.amfLookahead, "17");
    EXPECT_TRUE(config.video.amfSpatialAq);
    EXPECT_TRUE(config.video.amfTemporalAq);
    EXPECT_EQ(config.video.amfAqStrength, 2);
    EXPECT_TRUE(config.video.amfHighMotionQualityBoost);
    EXPECT_EQ(config.video.amfBRefMode, "enabled");
    EXPECT_TRUE(config.video.amfEnforceHrd);
    EXPECT_TRUE(config.video.amfFillerData);
    EXPECT_EQ(config.video.qsvPreset, "slow");
    EXPECT_EQ(config.video.qsvQp, 27);
    EXPECT_EQ(config.video.qsvAsyncDepth, 8);
    EXPECT_EQ(config.video.qsvLowPower, "enabled");
    EXPECT_EQ(config.video.qsvLookahead, "24");
    EXPECT_EQ(config.video.qsvMbbRc, "enabled");
    EXPECT_EQ(config.video.qsvExtBrc, "enabled");
    EXPECT_EQ(config.video.qsvAdaptiveI, "disabled");
    EXPECT_EQ(config.video.qsvAdaptiveB, "enabled");
    EXPECT_EQ(config.video.qsvLowDelayBrc, "disabled");
    EXPECT_EQ(config.video.qsvScenario, "archive");
    EXPECT_EQ(config.video.mfScenario, "camera_record");
    EXPECT_EQ(config.video.mfQualityVsSpeed, 73);
    EXPECT_TRUE(config.video.mfLowLatency);
}

TEST_F(ConfigTest, NewHardwareEncoderPoliciesHaveSafeDefaults) {
    WriteConfig("[Video]\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.video.bufferSize.empty());
    EXPECT_EQ(config.video.amfUsage, "transcoding");
    EXPECT_EQ(config.video.amfPreset, "balanced");
    EXPECT_EQ(config.video.amfLookahead, "off");
    EXPECT_EQ(config.video.qsvPreset, "veryfast");
    EXPECT_EQ(config.video.qsvLookahead, "off");
    EXPECT_EQ(config.video.qsvScenario, "unknown");
    EXPECT_EQ(config.video.mfScenario, "live_streaming");
    EXPECT_EQ(config.video.mfQualityVsSpeed, -1);
    EXPECT_FALSE(config.video.mfLowLatency);
}

TEST_F(ConfigTest, ParsePerformancePriorityValues) {
    WriteConfig(
        "[Performance]\n"
        "process_priority=realtime\n"
        "gpu_priority=5\n"
        "gpu_scheduling_priority=high\n"
        "copy_queue_priority=high\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.processPriority, "realtime");
    EXPECT_EQ(config.video.gpuPriority, 5);
    EXPECT_EQ(config.gpuSchedulingPriority, "high");
    EXPECT_EQ(config.copyQueuePriority, "high");
}

TEST_F(ConfigTest, InvalidPerformancePriorityValuesFallBackConservatively) {
    WriteConfig(
        "[Performance]\n"
        "process_priority=definitely_not_valid\n"
        "gpu_scheduling_priority=also_invalid\n"
        "copy_queue_priority=urgent\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.processPriority, "above_normal");
    EXPECT_EQ(config.gpuSchedulingPriority, "off");
    EXPECT_EQ(config.copyQueuePriority, "normal");
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

TEST_F(ConfigTest, CanonicalSectionLayoutParsesAndTakesPrecedence) {
    WriteConfig(
        "[General]\n"
        "capture_method=inject\n"
        "log_level=trace\n"
        "wgc_same_device_capture=true\n"
        "audio_capture_latency_ms=99\n"
        "[Capture]\n"
        "capture_method=wgc\n"
        "auto_fullscreen_capture=wgc_window\n"
        "[WGC]\n"
        "wgc_same_device_capture=false\n"
        "wgc_smoothness_buffer_max_ms=111\n"
        "wgc_smoothness_buffer_vram_budget_mb=222\n"
        "wgc_smoothness_floor_ms=33\n"
        "wgc_allow_lossy_bgra8_pool=true\n"
        "[Diagnostics]\n"
        "wgc_skip_split_device_flush=true\n"
        "wgc_video_memory_reservation=full\n"
        "overlay_observer_only=true\n"
        "overlay_observer_policy_only=true\n"
        "overlay_observer_startup_present_only=true\n"
        "dx12_focus_analysis=true\n"
        "[AudioSync]\n"
        "audio_capture_latency_ms=4.25\n"
        "mic_capture_latency_ms=7.5\n"
        "audio_latency_autodetect=false\n"
        "[Logging]\n"
        "log_level=info\n"
        "crash_dump_dir=extra-dumps\n"
        "[Output]\n"
        "output_dir=recordings\n"
        "screenshot_dir=stills\n"
        "container=mov\n"
        "[Screenshot]\n"
        "color_space=bt709\n"
        "[VideoScaling]\n"
        "enabled=true\n"
        "output_resolution=720p\n"
        "quality=normal\n"
        "sharpness=42\n"
        "[DLSS]\n"
        "dlss_fg_factor=3x\n"
        "[Overlay]\n"
        "copy_queue_priority=high\n"
        "[DesktopOverlay]\n"
        "enabled=true\n"
        "mode=1\n"
        "process_list=game.exe|other.exe\n"
        "[Audio]\n"
        "codec=aac\n"
        "bitrate=320\n"
        "[SystemAudio]\n"
        "enabled=true\n"
        "device=Speakers\n"
        "track=4,5\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.captureMethod, "wgc");
    EXPECT_FALSE(config.autoFullscreenPrefersDxgiDup);
    EXPECT_FALSE(config.wgcSameDeviceCapture);
    EXPECT_TRUE(config.wgcSkipSplitDeviceFlush);
    EXPECT_EQ(config.wgcSmoothnessBufferMaxMs, 111u);
    EXPECT_EQ(config.wgcSmoothnessBufferVramBudgetMb, 222u);
    EXPECT_FALSE(config.wgcSmoothnessFloorAuto);
    EXPECT_EQ(config.wgcSmoothnessFloorMs, 33u);
    EXPECT_TRUE(config.wgcAllowLossyBgra8Pool);
    EXPECT_EQ(config.wgcVideoMemoryReservation, "full");
    EXPECT_FLOAT_EQ(config.audioCaptureLatencyMs, 4.25f);
    EXPECT_FLOAT_EQ(config.micCaptureLatencyMs, 7.5f);
    EXPECT_FALSE(config.audioLatencyAutodetect);
    EXPECT_EQ(config.logLevel, LogLevel::Info);
    EXPECT_EQ(config.crashDumpDir, "extra-dumps");
    EXPECT_EQ(config.video.outputDir, "recordings");
    EXPECT_EQ(config.screenshotDir, "stills");
    EXPECT_EQ(config.screenshotColorSpace, "bt709");
    EXPECT_EQ(config.video.container, "mov");
    EXPECT_TRUE(config.video.scaling.enabled);
    EXPECT_EQ(config.video.scaling.outputResolution, "720p");
    EXPECT_EQ(config.video.scaling.sharpness, 42);
    EXPECT_EQ(config.graphics.dlssFgFactor, "3x");
    EXPECT_EQ(config.copyQueuePriority, "high");
    EXPECT_TRUE(config.overlay.observerOnly);
    EXPECT_TRUE(config.overlay.observerPolicyOnly);
    EXPECT_TRUE(config.overlay.observerStartupPresentOnly);
    EXPECT_TRUE(config.overlay.dx12FocusAnalysis);
    EXPECT_TRUE(config.pseudoOverlay.enabled);
    EXPECT_EQ(config.pseudoOverlay.mode, 1);
    EXPECT_EQ(config.pseudoOverlay.processList, "game.exe|other.exe");
    ASSERT_EQ(config.audioSources.size(), 1u);
    EXPECT_EQ(config.audioSources.front().device, "Speakers");
    EXPECT_EQ(config.audioSources.front().tracks, (std::vector<int>{4, 5}));
    EXPECT_EQ(config.audioSources.front().codec, "aac");
    EXPECT_EQ(config.audioSources.front().bitrate, 320);
    EXPECT_FLOAT_EQ(config.audioSources.front().captureLatencyMs, 4.25f);
}

TEST_F(ConfigTest, ParseWgcExperimentalFlags) {
    WriteConfig(
        "[General]\n"
        "wgc_skip_split_device_flush=true\n"
        "wgc_same_device_capture=true\n"
        "wgc_smoothness_buffer_enabled=false\n"
        "wgc_smoothness_buffer_max_ms=125\n"
        "wgc_smoothness_buffer_vram_budget_mb=512\n"
        "wgc_video_memory_reservation=full\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.wgcSkipSplitDeviceFlush);
    EXPECT_TRUE(config.wgcSameDeviceCapture);
    EXPECT_FALSE(config.wgcSmoothnessBufferEnabled);
    EXPECT_EQ(config.wgcSmoothnessBufferMaxMs, 125u);
    EXPECT_EQ(config.wgcSmoothnessBufferVramBudgetMb, 512u);
    EXPECT_EQ(config.wgcVideoMemoryReservation, "full");
}

TEST_F(ConfigTest, InvalidWgcVideoMemoryReservationStaysDiagnosticOff) {
    WriteConfig("[General]\nwgc_video_memory_reservation=aggressive\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.wgcVideoMemoryReservation, "off");
}

TEST_F(ConfigTest, MissingPerformancePriorityValuesUseShippedDefaults) {
    WriteConfig("[Performance]\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.processPriority, "high");
    EXPECT_EQ(config.video.gpuPriority, 7);
    EXPECT_EQ(config.gpuSchedulingPriority, "auto");
}

TEST_F(ConfigTest, ParseCanonicalWgcLossyPoolOption) {
    WriteConfig("[General]\nwgc_allow_lossy_bgra8_pool=true\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.wgcAllowLossyBgra8Pool);
}

TEST_F(ConfigTest, DeprecatedCompactPoolAliasIsUsedOnlyWhenCanonicalOptionIsAbsent) {
    WriteConfig("[General]\nwgc_prefer_compact_10bit_pool=true\n");
    AppConfig legacyOnly;
    LoadConfig(tempConfigFile, legacyOnly);
    EXPECT_TRUE(legacyOnly.wgcAllowLossyBgra8Pool);

    WriteConfig(
        "[General]\n"
        "wgc_prefer_compact_10bit_pool=true\n"
        "wgc_allow_lossy_bgra8_pool=false\n");
    AppConfig canonicalWins;
    LoadConfig(tempConfigFile, canonicalWins);
    EXPECT_FALSE(canonicalWins.wgcAllowLossyBgra8Pool);
}

TEST_F(ConfigTest, LegacyWgcAliasesNormalizeToWgc) {
    const char* aliases[] = {"screengrab", "framegrab"};

    for (const char* alias : aliases) {
        WriteConfig(std::string("[General]\n") + "capture_method=" + alias + "\n");

        AppConfig config;
        LoadConfig(tempConfigFile, config);

        EXPECT_EQ(config.captureMethod, "wgc") << alias;
    }
}

TEST_F(ConfigTest, DxgiDupAliasesNormalizeToDxgiDup) {
    const char* aliases[] = {"dxgi_dup", "desktop_dup", "duplication", "dxgi_duplication", "DXGI_DUP"};

    for (const char* alias : aliases) {
        WriteConfig(std::string("[General]\n") + "capture_method=" + alias + "\n");

        AppConfig config;
        LoadConfig(tempConfigFile, config);

        EXPECT_EQ(config.captureMethod, "dxgi_dup") << alias;
    }
}

TEST_F(ConfigTest, AutoFullscreenCaptureBackendOption) {
    // Default: duplication preferred for unhooked fullscreen games (hardware
    // cursor preservation).
    {
        WriteConfig("[General]\ncapture_method=auto\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config);
        EXPECT_TRUE(config.autoFullscreenPrefersDxgiDup);
    }
    {
        WriteConfig("[General]\nauto_fullscreen_capture=dxgi_dup\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config);
        EXPECT_TRUE(config.autoFullscreenPrefersDxgiDup);
    }
    {
        WriteConfig("[General]\nauto_fullscreen_capture=wgc_window\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config);
        EXPECT_FALSE(config.autoFullscreenPrefersDxgiDup);
    }
    {
        WriteConfig("[General]\nauto_fullscreen_capture=WGC\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config);
        EXPECT_FALSE(config.autoFullscreenPrefersDxgiDup);
    }
    {
        // Unknown values keep the safe default (duplication).
        WriteConfig("[General]\nauto_fullscreen_capture=bogus\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config);
        EXPECT_TRUE(config.autoFullscreenPrefersDxgiDup);
    }
}

TEST_F(ConfigTest, MonitorSelectorParsesAndInvalidValuesFailToAuto) {
    WriteConfig("[Capture]\nmonitor=cursor\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.captureMonitor, "cursor");

    WriteConfig("[Capture]\nmonitor=id:\\\\?\\DISPLAY#ACME123#{monitor-guid}\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.captureMonitor, "id:\\\\?\\DISPLAY#ACME123#{monitor-guid}");

    WriteConfig("[Capture]\nmonitor=id:\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.captureMonitor, "auto");

    WriteConfig("[Capture]\nmonitor=display-2\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.captureMonitor, "auto");
}

TEST_F(ConfigTest, CaptureMethodPredicateFamilies) {
    EXPECT_TRUE(IsDxgiDupCaptureMethod("dxgi_dup"));
    EXPECT_TRUE(IsDxgiDupCaptureMethod("desktop_dup"));
    EXPECT_FALSE(IsDxgiDupCaptureMethod("wgc"));
    EXPECT_FALSE(IsDxgiDupCaptureMethod("inject"));
    EXPECT_FALSE(IsDxgiDupCaptureMethod("auto"));

    // Screen-grab family = any non-inject desktop/window grab method.
    EXPECT_TRUE(IsScreenGrabCaptureMethod("wgc"));
    EXPECT_TRUE(IsScreenGrabCaptureMethod("screengrab"));
    EXPECT_TRUE(IsScreenGrabCaptureMethod("dxgi_dup"));
    EXPECT_TRUE(IsScreenGrabCaptureMethod("desktop_dup"));
    EXPECT_FALSE(IsScreenGrabCaptureMethod("inject"));
    EXPECT_FALSE(IsScreenGrabCaptureMethod("auto"));
    EXPECT_FALSE(IsScreenGrabCaptureMethod("none"));
    EXPECT_TRUE(IsVideoCaptureDisabledMethod("none"));
    EXPECT_FALSE(IsVideoCaptureDisabledMethod("auto"));

    // dxgi_dup must not be mistaken for wgc or auto.
    EXPECT_FALSE(IsWgcCaptureMethod("dxgi_dup"));
    EXPECT_FALSE(IsAutoCaptureMethod("dxgi_dup"));
    EXPECT_FALSE(IsInjectCaptureMethod("dxgi_dup"));
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

