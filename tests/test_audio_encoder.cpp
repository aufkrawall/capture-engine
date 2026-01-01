#include "../mediaengine/audio_encoder.h"
#include <gtest/gtest.h>
#include <vector>

// Helper to create dummy PCM data
std::vector<uint8_t> CreateDummyAudio(int milliseconds, int sampleRate, int channels) {
    int numSamples = (sampleRate * milliseconds) / 1000;
    int sizeBytes = numSamples * channels * sizeof(int16_t);
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

TEST_F(AudioEncoderTest, DiscardBeforeStart) {
    AudioConfig config;
    config.codec = "aac";
    encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); });
    encoder.SetStreamIndex(0); // Ready to emit

    // Set start time to 1000us
    encoder.SetRecordingStart(1000);

    // Feed audio at timestamp 500us (before start)
    auto data = CreateDummyAudio(10, 48000, 2); // 10ms
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 0); // ts 0ms = 0us

    // Should produce NO packets because it's discarded
    EXPECT_EQ(receivedPackets.size(), 0);
}

TEST_F(AudioEncoderTest, GapFilling) {
    AudioConfig config;
    config.codec = "aac";
    config.bitrate = 128; // Ensure valid
    encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); });
    encoder.SetStreamIndex(0);
    encoder.SetRecordingStart(0);

    // 1. Send first packet at 0ms
    auto data = CreateDummyAudio(20, 48000, 2); // 20ms audio roughly 1 frame
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 0);
    
    // Check we got something (might be buffered inside encoder if frame not full yet)
    // AAC frame size 1024 samples. 48000Hz -> 1024/48000 = ~21.3ms.
    // We sent 20ms. Might not trigger a packet yet.
    // Let's send 1 second of audio.
    data = CreateDummyAudio(1000, 48000, 2);
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 0); // ts 0
    
    EXPECT_GT(receivedPackets.size(), 0);
    size_t initialPackets = receivedPackets.size();

    // 2. Simulate a gap! Next packet comes at 2000ms (1 second silence gap after the 1st second)
    // Current stream time approx 1000ms.
    // Timestamp 2000ms. Gap ~1000ms.
    // Should fill silence.
    
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 2000);
    
    // We expect SIGNIFICANTLY more packets now due to silence fill + new data
    // Silence fill ~1s = ~46 packets (at ~21ms per packet)
    // New data ~1s = ~46 packets.
    // Total added ~92 packets.
    
    size_t newPackets = receivedPackets.size() - initialPackets;
    EXPECT_GT(newPackets, 80); // Rough check
}

TEST_F(AudioEncoderTest, BufferUntilStreamIndex) {
    AudioConfig config;
    config.codec = "aac";
    encoder.Init(config, [this](AVPacket* p) { PacketCallback(p); });
    
    // DO NOT set stream index yet
    encoder.SetRecordingStart(0);
    
    // Send 1 second of audio
    auto data = CreateDummyAudio(1000, 48000, 2);
    encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false, 0);
    
    // Should be 0 received because we haven't flushed yet
    EXPECT_EQ(receivedPackets.size(), 0);
    
    // Now set stream index
    encoder.SetStreamIndex(1);
    
    // Should receive all buffered packets
    EXPECT_GT(receivedPackets.size(), 40);
    
    // Verify stream index is correct
    EXPECT_EQ(receivedPackets[0]->stream_index, 1);
}
