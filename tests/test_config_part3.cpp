#include "test_config_shared.h"

TEST_F(ConfigTest, ParseNumberedSystemAudioSections) {
    WriteConfig(
        "[Audio]\n"
        "codec=opus\n"
        "bitrate=256\n"
        "[SystemAudio.1]\n"
        "enabled=true\n"
        "device=Speakers\n"
        "track=11\n"
        "codec=flac\n"
        "bitrate=999\n"
        "sample_rate=48000\n"
        "bit_depth=24\n"
        "downmix=true\n"
        "[SystemAudio.2]\n"
        "enabled=true\n"
        "device=Headphones\n"
        "track=12,13\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    ASSERT_EQ(config.audioSources.size(), 2u);
    EXPECT_EQ(config.audioSources[0].device, "Speakers");
    EXPECT_EQ(config.audioSources[0].tracks, (std::vector<int>{11}));
    EXPECT_EQ(config.audioSources[1].device, "Headphones");
    EXPECT_EQ(config.audioSources[1].tracks, (std::vector<int>{12, 13}));
    EXPECT_EQ(config.audioSources[0].codec, "flac");
    EXPECT_EQ(config.audioSources[0].bitrate, 999);
    EXPECT_EQ(config.audioSources[0].sampleRate, "48000");
    EXPECT_EQ(config.audioSources[0].bitDepth, "24");
    EXPECT_TRUE(config.audioSources[0].downmix);
    EXPECT_EQ(config.audioSources[1].codec, "opus");
    EXPECT_EQ(config.audioSources[1].bitrate, 256);
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

