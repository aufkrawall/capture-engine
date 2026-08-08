#include <gtest/gtest.h>
#include <windows.h>

#include <mmreg.h>

#include <algorithm>
#include <limits>
#include <vector>
#include "../mediaengine/audio_encoder.h"

extern "C" {
#include <libavutil/intreadwrite.h>
}

// Helper to create dummy PCM data
std::vector<uint8_t> CreateDummyAudio(int milliseconds, int sampleRate, int channels) {
    const size_t numSamples = static_cast<size_t>(sampleRate) * static_cast<size_t>(milliseconds) / 1000u;
    const size_t sizeBytes = numSamples * static_cast<size_t>(channels) * sizeof(int16_t);
    return std::vector<uint8_t>(sizeBytes, 0);
}

std::vector<uint8_t> CreateDummyFloatAudio(int milliseconds, int sampleRate, int channels) {
    const size_t numSamples = static_cast<size_t>(sampleRate) * static_cast<size_t>(milliseconds) / 1000u;
    const size_t sizeBytes = numSamples * static_cast<size_t>(channels) * sizeof(float);
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
    ASSERT_TRUE(encoder.ResetForRecordingStart(0, 1));

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

    encoder.Stop();
    config.bitDepth = "16trailing";
    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    EXPECT_EQ(encoder.GetCodecContext()->codec_id, AV_CODEC_ID_PCM_S24LE);
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
    ASSERT_TRUE(encoder.ResetForRecordingStart(0, 1));

    auto data = CreateDummyFloatAudio(200, 48000, 8);  // 200ms of 8ch float
    encoder.EncodeSamples(data.data(), static_cast<int>(data.size()), 8, 48000, 32, 32,
                          8 * static_cast<int>(sizeof(float)), true, config.outputChannelMask, 0);
    encoder.Stop();

    EXPECT_FALSE(receivedPackets.empty());
}

TEST_F(AudioEncoderTest, AacFlushPreservesPacketDurationsAndSignalsExactDecodedTarget) {
    AudioConfig config;
    config.codec = "aac";
    config.bitrate = 192;
    config.sampleRate = "48000";
    config.outputChannels = 2;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    encoder.SetStreamIndex(1);
    ASSERT_TRUE(encoder.ResetForRecordingStart(0, 1));

    constexpr int64_t kTargetUs = 10916667;
    constexpr int64_t kTargetSamples = 524000;
    auto data = CreateDummyFloatAudio(10900, 48000, 2);
    encoder.EncodeSamples(data.data(), static_cast<int>(data.size()), 2, 48000, 32, 32, 8, true,
                          config.outputChannelMask, 0);
    encoder.SetRecordingEndUs(kTargetUs);
    encoder.Stop();

    ASSERT_FALSE(receivedPackets.empty());
    int64_t maxPacketEnd = std::numeric_limits<int64_t>::min();
    for (auto* pkt : receivedPackets) {
        ASSERT_NE(pkt, nullptr);
        EXPECT_GT(pkt->duration, 0);
        if (pkt->pts == AV_NOPTS_VALUE || pkt->duration <= 0) {
            continue;
        }
        maxPacketEnd = std::max<int64_t>(maxPacketEnd, pkt->pts + pkt->duration);
    }
    EXPECT_GT(maxPacketEnd, kTargetSamples);
    size_t skipSideDataSize = 0;
    int endReason = -1;
    EXPECT_EQ(MaxPacketEndSkipSamples(receivedPackets, &skipSideDataSize, &endReason), 288);
    EXPECT_EQ(skipSideDataSize, 10u);
    EXPECT_EQ(endReason, 0);
    const auto& report = encoder.GetFinalizationReport();
    EXPECT_EQ(report.timelineTargetSamples, kTargetSamples);
    EXPECT_EQ(report.terminalPaddingSamples, 288);
    EXPECT_EQ(report.durationlessPacketCount, 0u);
    EXPECT_TRUE(report.drainReachedEof);
    EXPECT_FALSE(report.protocolError);
}

TEST_F(AudioEncoderTest, OpusFlushPreservesPreSkipAndSignalsTerminalDiscard) {
    AudioConfig config;
    config.codec = "opus";
    config.bitrate = 192;
    config.sampleRate = "48000";
    config.outputChannels = 2;
    config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
    encoder.SetStreamIndex(1);
    ASSERT_TRUE(encoder.ResetForRecordingStart(0, 1));
    encoder.SetRecordingEndUs(25000);
    encoder.Stop();

    ASSERT_FALSE(receivedPackets.empty());
    constexpr int64_t kTargetSamples = 1200;
    int64_t maxPacketEnd = std::numeric_limits<int64_t>::min();
    bool sawNegativePrimingPts = false;
    for (auto* pkt : receivedPackets) {
        ASSERT_NE(pkt, nullptr);
        EXPECT_GT(pkt->duration, 0);
        if (pkt->pts == AV_NOPTS_VALUE || pkt->duration <= 0) {
            continue;
        }
        sawNegativePrimingPts = sawNegativePrimingPts || pkt->pts < 0;
        maxPacketEnd = std::max<int64_t>(maxPacketEnd, pkt->pts + pkt->duration);
    }
    EXPECT_TRUE(sawNegativePrimingPts);
    EXPECT_GT(maxPacketEnd, kTargetSamples);
    size_t skipSideDataSize = 0;
    int endReason = -1;
    EXPECT_GT(MaxPacketEndSkipSamples(receivedPackets, &skipSideDataSize, &endReason), 0);
    EXPECT_EQ(skipSideDataSize, 10u);
    EXPECT_EQ(endReason, 0);
    const auto& report = encoder.GetFinalizationReport();
    EXPECT_EQ(report.timelineTargetSamples, kTargetSamples);
    EXPECT_EQ(report.terminalPaddingSamples, 720);
    EXPECT_TRUE(report.drainReachedEof);
    EXPECT_FALSE(report.protocolError);
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
        ASSERT_TRUE(localEncoder.ResetForRecordingStart(0, 1));
        localEncoder.SetRecordingEndUs(kTargetUs);
        localEncoder.Stop();

        ASSERT_FALSE(packets.empty()) << codecCase.codec;
        bool sawTimedPacket = false;
        for (auto* pkt : packets) {
            ASSERT_NE(pkt, nullptr) << codecCase.codec;
            if (pkt->pts != AV_NOPTS_VALUE && pkt->duration > 0) {
                sawTimedPacket = true;
            }
        }
        EXPECT_TRUE(sawTimedPacket) << codecCase.codec;
        const auto& report = localEncoder.GetFinalizationReport();
        EXPECT_EQ(report.timelineTargetSamples, kTargetSamples) << codecCase.codec;
        EXPECT_EQ(report.expectedDecodedSamples, kTargetSamples) << codecCase.codec;
        EXPECT_EQ(report.durationlessPacketCount, 0u) << codecCase.codec;
        EXPECT_TRUE(report.drainReachedEof) << codecCase.codec;
        EXPECT_FALSE(report.protocolError) << codecCase.codec;
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
    ASSERT_TRUE(encoder.ResetForRecordingStart(0, 1));

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
    ASSERT_TRUE(encoder.ResetForRecordingStart(1000, 1));

    // Feed audio at timestamp 500us (before start)
    auto data = CreateDummyAudio(10, 48000, 2);  // 10ms
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
    ASSERT_TRUE(encoder.ResetForRecordingStart(0, 1));

    // Send enough audio to produce at least one AAC frame (1024 samples @ 48kHz ≈ 21.3ms)
    // Send 100ms to ensure multiple complete frames
    auto data = CreateDummyAudio(100, 48000, 2);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 0);

    // AAC encoder should produce packets from 100ms of audio (≈4 frames)
    EXPECT_GE(receivedPackets.size(), 1);

    size_t initialPackets = receivedPackets.size();

    // Send another 100ms of audio at a later timestamp
    // Note: gap detection was removed from the encoder — it just encodes what it receives
    // The timestamp parameter is only used for internal tracking, not gap filling
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
    ASSERT_TRUE(encoder.ResetForRecordingStart(0, 1));

    // Send 100ms of audio (enough for ~4 AAC frames)
    auto data = CreateDummyAudio(100, 48000, 2);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 0);

    // Should have received some packets now that stream index is set
    EXPECT_GE(receivedPackets.size(), 1);
    EXPECT_LE(receivedPackets.size(), 20);

    // Verify stream index is correct on all packets
    for (auto* pkt : receivedPackets) {
        EXPECT_EQ(pkt->stream_index, 1);
    }
}

TEST(AudioEncoderContractTest, CodecRuntimeContractsReflectOpenedEncoderState) {
    struct CodecCase {
        const char* codec;
        const char* bitDepth;
        int expectedRate;
        AudioEncoder::FinalFramePolicy finalPolicy;
    };
    const CodecCase cases[] = {
        {"aac", "default", 48000, AudioEncoder::FinalFramePolicy::PadAndSignalDiscard},
        {"alac", "24", 48000, AudioEncoder::FinalFramePolicy::ExactShortFrame},
        {"flac", "24", 48000, AudioEncoder::FinalFramePolicy::ExactShortFrame},
        {"opus", "default", 48000, AudioEncoder::FinalFramePolicy::PadAndSignalDiscard},
        {"pcm", "24", 48000, AudioEncoder::FinalFramePolicy::BlockAlignedPcm},
    };
    for (const auto& codecCase : cases) {
        AudioEncoder localEncoder;
        AudioConfig config;
        config.codec = codecCase.codec;
        config.bitDepth = codecCase.bitDepth;
        config.sampleRate = std::string(codecCase.codec) == "opus" ? "44100" : "48000";
        config.outputChannels = 2;
        config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        ASSERT_TRUE(localEncoder.Init(config, [](AVPacket*) {})) << codecCase.codec;
        const auto& contract = localEncoder.GetRuntimeContract();
        EXPECT_TRUE(contract.valid) << codecCase.codec;
        EXPECT_EQ(contract.sampleRate, codecCase.expectedRate) << codecCase.codec;
        EXPECT_EQ(contract.channels, 2) << codecCase.codec;
        EXPECT_EQ(contract.finalFramePolicy, codecCase.finalPolicy) << codecCase.codec;
        EXPECT_FALSE(contract.encoderName.empty()) << codecCase.codec;
        if (std::string(codecCase.codec) == "aac") {
            EXPECT_EQ(contract.encoderName, "aac");
            EXPECT_TRUE(contract.nativeAacNmr);
            EXPECT_EQ(contract.nativeAacNmrSpeed, 0);
        }
        if (std::string(codecCase.codec) == "opus") {
            EXPECT_EQ(contract.encoderName, "libopus");
            EXPECT_EQ(contract.frameSize, 960);
            EXPECT_TRUE(contract.requiresMatroskaCodecDelay);
            EXPECT_TRUE(contract.requiresMatroskaDiscardPadding);
        }
        localEncoder.Stop();
        EXPECT_FALSE(localEncoder.IsReady()) << codecCase.codec;
    }
}

TEST(AudioEncoderContractTest, BoundaryLengthsFinalizeExactlyForEveryCodec) {
    const char* codecs[] = {"aac", "alac", "flac", "opus", "pcm"};
    for (const char* codec : codecs) {
        AudioEncoder probe;
        AudioConfig config;
        config.codec = codec;
        config.bitrate = 192;
        config.sampleRate = "48000";
        config.bitDepth = "24";
        config.outputChannels = 2;
        config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        ASSERT_TRUE(probe.Init(config, [](AVPacket*) {})) << codec;
        const int nominalFrame = probe.GetRuntimeContract().frameSize > 0 ? probe.GetRuntimeContract().frameSize : 4096;
        probe.Stop();
        const int lengths[] = {1, nominalFrame - 1, nominalFrame, nominalFrame + 1, nominalFrame * 3};
        uint64_t generation = 1;
        for (int targetSamples : lengths) {
            AudioEncoder localEncoder;
            std::vector<AVPacket*> packets;
            ASSERT_TRUE(localEncoder.Init(config,
                                          [&packets](AVPacket* packet) {
                                              if (AVPacket* clone = av_packet_clone(packet)) {
                                                  packets.push_back(clone);
                                              }
                                          }))
                << codec << " target=" << targetSamples;
            localEncoder.SetStreamIndex(1);
            ASSERT_TRUE(localEncoder.ResetForRecordingStart(0, generation++));
            const int64_t targetUs = (static_cast<int64_t>(targetSamples) * 1000000ll + 24000ll) / 48000ll;
            localEncoder.SetRecordingEndUs(targetUs);
            localEncoder.Stop();
            const auto& report = localEncoder.GetFinalizationReport();
            EXPECT_EQ(report.timelineTargetSamples, targetSamples) << codec << " target=" << targetSamples;
            EXPECT_EQ(report.expectedDecodedSamples, targetSamples) << codec << " target=" << targetSamples;
            EXPECT_GE(report.codecSubmittedSamples, targetSamples) << codec << " target=" << targetSamples;
            EXPECT_EQ(report.durationlessPacketCount, 0u) << codec << " target=" << targetSamples;
            EXPECT_EQ(report.controlPacketCount, codec == "flac" ? 1u : 0u) << codec << " target=" << targetSamples;
            EXPECT_TRUE(report.drainReachedEof) << codec << " target=" << targetSamples;
            EXPECT_FALSE(report.protocolError) << codec << " target=" << targetSamples;
            EXPECT_FALSE(packets.empty()) << codec << " target=" << targetSamples;
            for (AVPacket* packet : packets) {
                av_packet_free(&packet);
            }
        }
    }
}

TEST(AudioEncoderContractTest, RepeatedRecordingCyclesOwnFreshCodecResources) {
    const char* codecs[] = {"aac", "alac", "flac", "opus", "pcm"};
    for (const char* codec : codecs) {
        AudioEncoder localEncoder;
        AudioConfig config;
        config.codec = codec;
        config.bitrate = 192;
        config.sampleRate = "48000";
        config.bitDepth = "24";
        config.outputChannels = 2;
        config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        for (uint64_t cycle = 1; cycle <= 6; ++cycle) {
            size_t packetCount = 0;
            ASSERT_TRUE(localEncoder.Init(config, [&packetCount](AVPacket*) { ++packetCount; }))
                << codec << " cycle=" << cycle;
            ASSERT_TRUE(localEncoder.IsReady());
            localEncoder.SetStreamIndex(1);
            ASSERT_TRUE(localEncoder.ResetForRecordingStart(0, cycle));
            localEncoder.SetRecordingEndUs(100000);
            localEncoder.Stop();
            EXPECT_FALSE(localEncoder.IsReady()) << codec << " cycle=" << cycle;
            EXPECT_GT(packetCount, 0u) << codec << " cycle=" << cycle;
            EXPECT_FALSE(localEncoder.GetFinalizationReport().protocolError) << codec << " cycle=" << cycle;
        }
    }
}

TEST(AudioEncoderContractTest, ActiveCodecContextCannotBeReinitializedWithoutDrain) {
    AudioEncoder localEncoder;
    AudioConfig config;
    config.codec = "aac";
    config.sampleRate = "48000";
    ASSERT_TRUE(localEncoder.Init(config, [](AVPacket*) {}));
    EXPECT_FALSE(localEncoder.Init(config, [](AVPacket*) {}));
    EXPECT_TRUE(localEncoder.IsReady());
    localEncoder.SetStreamIndex(1);
    ASSERT_TRUE(localEncoder.ResetForRecordingStart(0, 1));
    localEncoder.SetRecordingEndUs(100000);
    localEncoder.Stop();
    EXPECT_TRUE(localEncoder.GetFinalizationReport().drainReachedEof);
}

TEST(AudioEncoderContractTest, LosslessBitDepthAndSampleRateAreResolvedBeforeOpen) {
    struct Case {
        const char* codec;
        const char* bitDepth;
        const char* sampleRate;
        int expectedBits;
        int expectedRate;
    };
    const Case cases[] = {
        {"alac", "16", "44100", 16, 44100}, {"alac", "24", "48000", 24, 48000}, {"alac", "32", "96000", 24, 96000},
        {"flac", "16", "44100", 16, 44100}, {"flac", "24", "48000", 24, 48000}, {"flac", "32", "96000", 32, 96000},
        {"pcm", "16", "44100", 16, 44100},  {"pcm", "24", "48000", 24, 48000},  {"pcm", "32", "96000", 32, 96000},
    };
    for (const auto& item : cases) {
        AudioEncoder localEncoder;
        AudioConfig config;
        config.codec = item.codec;
        config.bitDepth = item.bitDepth;
        config.sampleRate = item.sampleRate;
        config.outputChannels = 1;
        config.outputChannelMask = SPEAKER_FRONT_CENTER;
        ASSERT_TRUE(localEncoder.Init(config, [](AVPacket*) {}))
            << item.codec << '/' << item.bitDepth << '/' << item.sampleRate;
        EXPECT_EQ(localEncoder.GetRuntimeContract().sampleRate, item.expectedRate);
        EXPECT_EQ(localEncoder.GetRuntimeContract().rawBitDepth, item.expectedBits);
        localEncoder.Stop();
    }
}
