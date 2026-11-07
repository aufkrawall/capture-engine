// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "test_config_shared.h"

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
