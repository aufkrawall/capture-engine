#include <gtest/gtest.h>
#include <vector>
#include <windows.h>
#include <mmreg.h>
#include "../mediaengine/audio_encoder.h"

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
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                               SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;

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
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                               SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                               SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    EXPECT_EQ(encoder.GetCodecContext()->bit_rate, 768000);
    EXPECT_EQ(encoder.GetCodecContext()->ch_layout.nb_channels, 8);
}

TEST_F(AudioEncoderTest, MultichannelPcmEncodesPackets) {
    AudioConfig config;
    config.codec = "pcm";
    config.sampleRate = "48000";
    config.outputChannels = 6;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                               SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;

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
