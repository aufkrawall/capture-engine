#include <gtest/gtest.h>
#include <windows.h>

#include <mmreg.h>

#include <vector>
#include "mediaengine/audio/audio_encoder.h"

extern "C" {
#include <libavutil/intreadwrite.h>
}

namespace {

std::vector<uint8_t> MakeSilentFloatAudio(int milliseconds, int sampleRate, int channels) {
    const size_t numSamples = static_cast<size_t>(sampleRate) * static_cast<size_t>(milliseconds) / 1000u;
    return std::vector<uint8_t>(numSamples * static_cast<size_t>(channels) * sizeof(float), 0);
}

class AudioEncoderFinalPacketTest : public ::testing::Test {
protected:
    std::vector<AVPacket*> receivedPackets;

    void TearDown() override {
        ClearPackets();
    }

    void ClearPackets() {
        for (auto* pkt : receivedPackets) {
            av_packet_free(&pkt);
        }
        receivedPackets.clear();
    }

    void PacketCallback(AVPacket* pkt) {
        receivedPackets.push_back(av_packet_clone(pkt));
    }
};

}  // namespace

// Whatever fraction of its last frame the recording end falls in, no packet may start at or after the end,
// and the last packet's frame minus its total end discard (the encoder's own plus ours) must land on the
// end exactly, with the discard inside that one packet. Opus (960-sample frames, 312 samples of lookahead)
// drains a tail packet wholly past the end when the end is in the first 648 samples of a frame.
TEST_F(AudioEncoderFinalPacketTest, LastPacketTimelineLandsOnTheTargetAtEveryPositionInTheLastFrame) {
    for (const char* codec : {"opus", "aac"}) {
        for (const int64_t targetSamples : {2400, 2448, 2976, 3024, 3072, 3120, 3360, 3840}) {
            SCOPED_TRACE(::testing::Message() << codec << " target " << targetSamples);
            ClearPackets();
            AudioEncoder encoder;
            AudioConfig config;
            config.codec = codec;
            config.bitrate = 192;
            config.sampleRate = "48000";
            config.outputChannels = 2;
            config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            ASSERT_TRUE(encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); }));
            encoder.SetStreamIndex(1);
            ASSERT_TRUE(encoder.ResetForRecordingStart(0, 1));
            const int64_t targetUs = targetSamples * 1000000 / 48000;
            auto data = MakeSilentFloatAudio(static_cast<int>(targetUs / 1000), 48000, 2);
            encoder.EncodeSamples(data.data(), static_cast<int>(data.size()), 2, 48000, 32, 32, 8, true,
                                  config.outputChannelMask, 0);
            encoder.SetRecordingEndUs(targetUs);
            const int frameSize = encoder.GetCodecContext()->frame_size;
            encoder.Stop();

            const int64_t target = encoder.GetFinalizationReport().timelineTargetSamples;
            ASSERT_FALSE(receivedPackets.empty());
            const AVPacket* last = receivedPackets.back();
            for (const AVPacket* pkt : receivedPackets) {
                EXPECT_LT(pkt->pts, target) << "packet wholly past the recording end";
            }
            size_t skipSize = 0;
            const uint8_t* skipData = av_packet_get_side_data(last, AV_PKT_DATA_SKIP_SAMPLES, &skipSize);
            const int64_t endSkip = (skipData && skipSize >= 10) ? static_cast<int64_t>(AV_RL32(skipData + 4)) : 0;
            EXPECT_GE(endSkip, 0);
            EXPECT_LT(endSkip, frameSize) << "end discard larger than the packet that carries it";
            EXPECT_EQ(last->pts + frameSize - endSkip, target);
            EXPECT_FALSE(encoder.GetFinalizationReport().protocolError);
        }
    }
}
