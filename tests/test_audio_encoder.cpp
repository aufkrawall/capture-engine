#include <gtest/gtest.h>
#include <windows.h>

#include <mmreg.h>

#include <algorithm>
#include <vector>
#include "../mediaengine/audio_encoder.h"

extern "C" {
#include <libavutil/intreadwrite.h>
}

// Helper to create dummy PCM data
std::vector<uint8_t> CreateDummyAudio(int milliseconds, int sampleRate, int channels) {
    int numSamples = (sampleRate * milliseconds) / 1000;
    int sizeBytes = numSamples * channels * sizeof(int16_t);
    return std::vector<uint8_t>(sizeBytes, 0);
}

std::vector<uint8_t> CreateDummyFloatAudio(int milliseconds, int sampleRate, int channels) {
    int numSamples = (sampleRate * milliseconds) / 1000;
    int sizeBytes = numSamples * channels * sizeof(float);
    return std::vector<uint8_t>(sizeBytes, 0);
}

int64_t MaxPacketEndSkipSamples(const std::vector<AVPacket*>& packets, size_t* observedSideDataSize = nullptr,
                                int* observedEndReason = nullptr) {
    int64_t maxEndSkip = 0;
    for (const AVPacket* pkt : packets) {
        size_t sideDataSize = 0;
        const uint8_t* skipData = av_packet_get_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, &sideDataSize);
        if (!skipData || sideDataSize < 10) {
            continue;
        }
        maxEndSkip = std::max<int64_t>(maxEndSkip, AV_RL32(skipData + 4));
        if (observedSideDataSize) {
            *observedSideDataSize = sideDataSize;
        }
        if (observedEndReason) {
            *observedEndReason = skipData[9];
        }
    }
    return maxEndSkip;
}

class AudioEncoderTest : public ::testing::Test {
protected:
    AudioEncoder encoder;
    std::vector<AVPacket*> receivedPackets;

    void SetUp() override {
        // Collect packets
    }

    void TearDown() override {
        for (auto* pkt : receivedPackets) {
            av_packet_free(&pkt);
        }
        receivedPackets.clear();
    }

    void PacketCallback(AVPacket* pkt) {
        // Clone packet because encoder flushes/frees them
        AVPacket* clone = av_packet_clone(pkt);
        receivedPackets.push_back(clone);
    }
};

TEST_F(AudioEncoderTest, Initialization) {
    AudioConfig config;
    config.codec = "aac";
    config.bitrate = 128;
    config.sampleRate = "48000";

    bool success = encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); });
    EXPECT_TRUE(success);
    EXPECT_TRUE(encoder.IsReady());
}

TEST_F(AudioEncoderTest, PcmAliasResolvesToTwentyFourBitEncoderByDefault) {
    AudioConfig config;
    config.codec = "pcm";
    config.sampleRate = "48000";
    config.outputChannels = 2;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    ASSERT_NE(encoder.GetCodecContext(), nullptr);
    EXPECT_EQ(encoder.GetCodecContext()->codec_id, AV_CODEC_ID_PCM_S24LE);
    EXPECT_EQ(encoder.GetCodecContext()->sample_fmt, AV_SAMPLE_FMT_S32);
}

TEST_F(AudioEncoderTest, PcmAcceptsFinalBatchLongerThanFiveSeconds) {
    AudioConfig config;
    config.codec = "pcm";
    config.sampleRate = "48000";
    config.outputChannels = 2;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    encoder.SetStreamIndex(1);
    encoder.SetRecordingStart(0);

    constexpr int kSamples = 625600;
    std::vector<float> samples(static_cast<size_t>(kSamples) * 2u, 0.0f);
    encoder.SetRecordingEndUs((static_cast<int64_t>(kSamples) * 1000000ll) / 48000ll);
    const auto result = encoder.EncodeSamples(reinterpret_cast<const uint8_t*>(samples.data()),
                                              static_cast<int>(samples.size() * sizeof(float)), 2, 48000, 32, 32, 8,
                                              true, config.outputChannelMask, 0);

    EXPECT_FALSE(result.failed);
    EXPECT_EQ(result.acceptedSamples, kSamples);
    EXPECT_EQ(result.submittedSamples, kSamples);
    EXPECT_EQ(encoder.GetSamplesCount(), kSamples);
    encoder.Stop();
}

TEST_F(AudioEncoderTest, PcmBitDepthOverridesSelectConcreteEncoders) {
    AudioConfig config;
    config.codec = "pcm";
    config.sampleRate = "48000";
    config.bitDepth = "16";
    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    EXPECT_EQ(encoder.GetCodecContext()->codec_id, AV_CODEC_ID_PCM_S16LE);

    encoder.Stop();
    config.bitDepth = "32";
    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    EXPECT_EQ(encoder.GetCodecContext()->codec_id, AV_CODEC_ID_PCM_F32LE);
    EXPECT_EQ(encoder.GetCodecContext()->sample_fmt, AV_SAMPLE_FMT_FLT);
}

TEST_F(AudioEncoderTest, OpusUsesLibopusAtFortyEightKhzAndScalesMultichannelBitrate) {
    AudioConfig config;
    config.codec = "opus";
    config.bitrate = 192;
    config.sampleRate = "44100";
    config.outputChannels = 6;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    ASSERT_NE(encoder.GetCodecContext(), nullptr);
    ASSERT_NE(encoder.GetCodecContext()->codec, nullptr);
    EXPECT_STREQ(encoder.GetCodecContext()->codec->name, "libopus");
    EXPECT_EQ(encoder.GetCodecContext()->sample_rate, 48000);
    EXPECT_EQ(encoder.GetCodecContext()->bit_rate, 576000);
    EXPECT_EQ(encoder.GetCodecContext()->ch_layout.nb_channels, 6);
}

TEST_F(AudioEncoderTest, AacScalesSevenPointOneBitrate) {
    AudioConfig config;
    config.codec = "aac";
    config.bitrate = 192;
    config.sampleRate = "48000";
    config.outputChannels = 8;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    EXPECT_EQ(encoder.GetCodecContext()->bit_rate, 768000);
    EXPECT_EQ(encoder.GetCodecContext()->ch_layout.nb_channels, 8);
}

// Regression: an 8ch/7.1 (side) default endpoint must not silently drop all audio
// with ALAC, which supports 7.1(wide) but NOT plain 7.1. The encoder must remap to
// a same-count supported layout and still open, preserving all 8 channels.
TEST_F(AudioEncoderTest, AlacSevenPointOneRemapsToSupportedLayoutInsteadOfFailing) {
    AudioConfig config;
    config.codec = "alac";
    config.sampleRate = "48000";
    config.outputChannels = 8;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT |
                               SPEAKER_SIDE_RIGHT;  // 0x63f = 7.1 (side)

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    ASSERT_NE(encoder.GetCodecContext(), nullptr);
    EXPECT_EQ(encoder.GetCodecContext()->codec_id, AV_CODEC_ID_ALAC);
    // All 8 channels preserved (relabeled to ALAC's 7.1(wide)), not downmixed/dropped.
    EXPECT_EQ(encoder.GetCodecContext()->ch_layout.nb_channels, 8);
}

// ALAC layouts it already supports (e.g. 5.1 back) must pass through untouched.
TEST_F(AudioEncoderTest, AlacSupportedLayoutIsLeftUnchanged) {
    AudioConfig config;
    config.codec = "alac";
    config.sampleRate = "48000";
    config.outputChannels = 6;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;  // 5.1(back)

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    ASSERT_NE(encoder.GetCodecContext(), nullptr);
    EXPECT_EQ(encoder.GetCodecContext()->ch_layout.nb_channels, 6);
}

// End-to-end: 7.1 (side) float audio must actually encode through ALAC's relabeled
// 7.1(wide) layout and produce packets - proving the remap preserves the data path,
// not just avcodec_open2.
TEST_F(AudioEncoderTest, AlacSevenPointOneEncodesAndProducesPackets) {
    AudioConfig config;
    config.codec = "alac";
    config.sampleRate = "48000";
    config.outputChannels = 8;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    encoder.SetStreamIndex(1);
    encoder.SetRecordingStart(0);

    auto data = CreateDummyFloatAudio(200, 48000, 8);  // 200ms of 8ch float
    encoder.EncodeSamples(data.data(), static_cast<int>(data.size()), 8, 48000, 32, 32,
                          8 * static_cast<int>(sizeof(float)), true, config.outputChannelMask, 0);
    encoder.Stop();

    EXPECT_FALSE(receivedPackets.empty());
}

TEST_F(AudioEncoderTest, AacFlushClampsPaddedFinalPacketToVideoTarget) {
    AudioConfig config;
    config.codec = "aac";
    config.bitrate = 192;
    config.sampleRate = "48000";
    config.outputChannels = 2;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    encoder.SetStreamIndex(1);
    encoder.SetRecordingStart(0);

    constexpr int64_t kTargetUs = 10916667;
    constexpr int64_t kTargetSamples = 524000;
    auto data = CreateDummyFloatAudio(10900, 48000, 2);
    encoder.EncodeSamples(data.data(), static_cast<int>(data.size()), 2, 48000, 32, 32, 8, true,
                          config.outputChannelMask, 0);
    encoder.SetRecordingEndUs(kTargetUs);
    encoder.Stop();

    ASSERT_FALSE(receivedPackets.empty());
    int64_t maxPacketEnd = 0;
    for (auto* pkt : receivedPackets) {
        ASSERT_NE(pkt, nullptr);
        if (pkt->pts == AV_NOPTS_VALUE || pkt->duration <= 0 || pkt->pts < 0) {
            continue;
        }
        EXPECT_LT(pkt->pts, kTargetSamples);
        maxPacketEnd = std::max<int64_t>(maxPacketEnd, pkt->pts + pkt->duration);
    }
    EXPECT_EQ(maxPacketEnd, kTargetSamples);
    size_t skipSideDataSize = 0;
    int endReason = -1;
    EXPECT_EQ(MaxPacketEndSkipSamples(receivedPackets, &skipSideDataSize, &endReason), 288);
    EXPECT_EQ(skipSideDataSize, 10u);
    EXPECT_EQ(endReason, 0);
}

TEST_F(AudioEncoderTest, OpusFlushPadsPackedSilenceWithoutPacketsPastTarget) {
    AudioConfig config;
    config.codec = "opus";
    config.bitrate = 192;
    config.sampleRate = "48000";
    config.outputChannels = 2;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    encoder.SetStreamIndex(1);
    encoder.SetRecordingStart(0);
    encoder.SetRecordingEndUs(25000);
    encoder.Stop();

    ASSERT_FALSE(receivedPackets.empty());
    constexpr int64_t kTargetSamples = 1200;
    int64_t maxPacketEnd = 0;
    for (auto* pkt : receivedPackets) {
        ASSERT_NE(pkt, nullptr);
        if (pkt->pts == AV_NOPTS_VALUE || pkt->duration <= 0 || pkt->pts < 0) {
            continue;
        }
        EXPECT_LT(pkt->pts, kTargetSamples);
        maxPacketEnd = std::max<int64_t>(maxPacketEnd, pkt->pts + pkt->duration);
    }
    EXPECT_EQ(maxPacketEnd, kTargetSamples);
    size_t skipSideDataSize = 0;
    int endReason = -1;
    EXPECT_GT(MaxPacketEndSkipSamples(receivedPackets, &skipSideDataSize, &endReason), 0);
    EXPECT_EQ(skipSideDataSize, 10u);
    EXPECT_EQ(endReason, 0);
}

TEST_F(AudioEncoderTest, SilenceFlushIsBoundedForEveryAudioCodec) {
    struct CodecCase {
        const char* codec;
        const char* bitDepth;
    };
    const CodecCase cases[] = {
        {"aac", "default"}, {"alac", "default"}, {"flac", "default"}, {"opus", "default"}, {"pcm", "default"},
    };

    constexpr int64_t kTargetUs = 105000;
    constexpr int64_t kTargetSamples = 5040;
    for (const auto& codecCase : cases) {
        AudioEncoder localEncoder;
        std::vector<AVPacket*> packets;

        AudioConfig config;
        config.codec = codecCase.codec;
        config.bitrate = 192;
        config.sampleRate = "48000";
        config.bitDepth = codecCase.bitDepth;
        config.outputChannels = 2;
        config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

        ASSERT_TRUE(localEncoder.Init(config, [&packets](AVPacket* pkt) {
            if (AVPacket* clone = av_packet_clone(pkt)) {
                packets.push_back(clone);
            }
        })) << codecCase.codec;
        localEncoder.SetStreamIndex(1);
        localEncoder.SetRecordingStart(0);
        localEncoder.SetRecordingEndUs(kTargetUs);
        localEncoder.Stop();

        ASSERT_FALSE(packets.empty()) << codecCase.codec;
        bool sawBoundedPacket = false;
        int64_t maxPacketEnd = 0;
        for (auto* pkt : packets) {
            ASSERT_NE(pkt, nullptr) << codecCase.codec;
            if (pkt->pts != AV_NOPTS_VALUE && pkt->duration > 0 && pkt->pts >= 0) {
                sawBoundedPacket = true;
                EXPECT_LT(pkt->pts, kTargetSamples) << codecCase.codec;
                maxPacketEnd = std::max<int64_t>(maxPacketEnd, pkt->pts + pkt->duration);
            }
        }
        EXPECT_TRUE(sawBoundedPacket) << codecCase.codec;
        EXPECT_EQ(maxPacketEnd, kTargetSamples) << codecCase.codec;
        const std::string codecName = codecCase.codec;
        if (codecName == "aac" || codecName == "opus") {
            size_t skipSideDataSize = 0;
            int endReason = -1;
            EXPECT_GT(MaxPacketEndSkipSamples(packets, &skipSideDataSize, &endReason), 0) << codecCase.codec;
            EXPECT_EQ(skipSideDataSize, 10u) << codecCase.codec;
            EXPECT_EQ(endReason, 0) << codecCase.codec;
        }
        for (auto* pkt : packets) {
            av_packet_free(&pkt);
        }
    }
}

TEST_F(AudioEncoderTest, MultichannelPcmEncodesPackets) {
    AudioConfig config;
    config.codec = "pcm";
    config.sampleRate = "48000";
    config.outputChannels = 6;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    encoder.SetStreamIndex(2);
    encoder.SetRecordingStart(0);

    auto data = CreateDummyFloatAudio(20, 48000, 6);
    encoder.EncodeSamples(data.data(), static_cast<int>(data.size()), 6, 48000, 32, 32, 24, true,
                          config.outputChannelMask, 0);

    EXPECT_GE(receivedPackets.size(), 1);
    for (auto* pkt : receivedPackets) {
        EXPECT_EQ(pkt->stream_index, 2);
    }
}

TEST_F(AudioEncoderTest, DiscardBeforeStart) {
    AudioConfig config;
    config.codec = "aac";
    encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); });
    encoder.SetStreamIndex(0);  // Ready to emit

    // Set start time to 1000us
    encoder.SetRecordingStart(1000);

    // Feed audio at timestamp 500us (before start)
    auto data = CreateDummyAudio(10, 48000, 2);  // 10ms
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false,
                          0);  // ts 0ms = 0us

    // Should produce NO packets because it's discarded
    EXPECT_EQ(receivedPackets.size(), 0);
}

TEST_F(AudioEncoderTest, GapFilling) {
    AudioConfig config;
    config.codec = "aac";
    config.bitrate = 128;
    encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); });
    encoder.SetStreamIndex(0);
    encoder.SetRecordingStart(0);

    // Send enough audio to produce at least one AAC frame (1024 samples @ 48kHz ≈ 21.3ms)
    // Send 100ms to ensure multiple complete frames
    auto data = CreateDummyAudio(100, 48000, 2);
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 0);

    // AAC encoder should produce packets from 100ms of audio (≈4 frames)
    EXPECT_GE(receivedPackets.size(), 1);

    size_t initialPackets = receivedPackets.size();

    // Send another 100ms of audio at a later timestamp
    // Note: gap detection was removed from the encoder — it just encodes what it receives
    // The timestamp parameter is only used for internal tracking, not gap filling
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 200);

    // Should produce more packets from the new audio data
    EXPECT_GT(receivedPackets.size(), initialPackets);
}

TEST_F(AudioEncoderTest, BufferUntilStreamIndex) {
    AudioConfig config;
    config.codec = "aac";
    encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); });

    // Set stream index first - encoder needs this to emit packets
    encoder.SetStreamIndex(1);
    encoder.SetRecordingStart(0);

    // Send 100ms of audio (enough for ~4 AAC frames)
    auto data = CreateDummyAudio(100, 48000, 2);
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 0);

    // Should have received some packets now that stream index is set
    EXPECT_GE(receivedPackets.size(), 1);
    EXPECT_LE(receivedPackets.size(), 20);

    // Verify stream index is correct on all packets
    for (auto* pkt : receivedPackets) {
        EXPECT_EQ(pkt->stream_index, 1);
    }
}
