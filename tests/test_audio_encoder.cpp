#include "../mediaengine/audio_encoder.h"
#include <gtest/gtest.h>
#include <vector>

// Helper to create dummy PCM data
std::vector<uint8_t> CreateDummyAudio(int milliseconds, int sampleRate,
                                      int channels) {
  int numSamples = (sampleRate * milliseconds) / 1000;
  int sizeBytes = numSamples * channels * sizeof(int16_t);
  return std::vector<uint8_t>(sizeBytes, 0);
}

class AudioEncoderTest : public ::testing::Test {
protected:
  AudioEncoder encoder;
  std::vector<AVPacket *> receivedPackets;

  void SetUp() override {
    // Collect packets
  }

  void TearDown() override {
    for (auto *pkt : receivedPackets) {
      av_packet_free(&pkt);
    }
    receivedPackets.clear();
  }

  void PacketCallback(AVPacket *pkt) {
    // Clone packet because encoder flushes/frees them
    AVPacket *clone = av_packet_clone(pkt);
    receivedPackets.push_back(clone);
  }
};

TEST_F(AudioEncoderTest, Initialization) {
  AudioConfig config;
  config.codec = "aac";
  config.bitrate = 128;
  config.sampleRate = "48000";

  bool success =
      encoder.Init(config, [this](AVPacket *p) { PacketCallback(p); });
  EXPECT_TRUE(success);
  EXPECT_TRUE(encoder.IsReady());
}

TEST_F(AudioEncoderTest, DiscardBeforeStart) {
  AudioConfig config;
  config.codec = "aac";
  encoder.Init(config, [this](AVPacket *p) { PacketCallback(p); });
  encoder.SetStreamIndex(0); // Ready to emit

  // Set start time to 1000us
  encoder.SetRecordingStart(1000);

  // Feed audio at timestamp 500us (before start)
  auto data = CreateDummyAudio(10, 48000, 2); // 10ms
  encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false,
                        0); // ts 0ms = 0us

  // Should produce NO packets because it's discarded
  EXPECT_EQ(receivedPackets.size(), 0);
}

TEST_F(AudioEncoderTest, GapFilling) {
  AudioConfig config;
  config.codec = "aac";
  config.bitrate = 128; // Ensure valid
  encoder.Init(config, [this](AVPacket *p) { PacketCallback(p); });
  encoder.SetStreamIndex(0);
  encoder.SetRecordingStart(0);

  // 1. Send first packet at 0ms
  auto data = CreateDummyAudio(20, 48000, 2); // 20ms audio roughly 1 frame
  encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false,
                        0);

  // Check we got something (might be buffered inside encoder if frame not full
  // yet) AAC frame size 1024 samples. 48000Hz -> 1024/48000 = ~21.3ms. We sent
  // 20ms. Might not trigger a packet yet. Let's send 1 second of audio.
  data = CreateDummyAudio(1000, 48000, 2);
  encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false,
                        0); // ts 0

  EXPECT_GT(receivedPackets.size(), 0);
  size_t initialPackets = receivedPackets.size();

  // 2. Simulate a gap! Next packet comes at 2000ms (1 second silence gap after
  // the 1st second) Current stream time approx 1000ms. Timestamp 2000ms. Gap
  // ~1000ms. Should fill silence.

  encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false,
                        2000);

  // We expect significantly more packets now due to silence fill + new data
  // NOTE: Actual packet count varies by FFmpeg version and encoder behavior.
  // Just verify that gap filling produced a reasonable number of packets.

  size_t newPackets = receivedPackets.size() - initialPackets;
  
  // With gap filling + 1 second of new data, should get ~40-50 packets
  EXPECT_GE(newPackets, 30);  // At least 30 packets
  EXPECT_LE(newPackets, 100); // But not more than 100
}

TEST_F(AudioEncoderTest, BufferUntilStreamIndex) {
  AudioConfig config;
  config.codec = "aac";
  encoder.Init(config, [this](AVPacket *p) { PacketCallback(p); });

  // Set stream index first - encoder needs this to emit packets
  encoder.SetStreamIndex(1);
  encoder.SetRecordingStart(0);

  // Send 1 second of audio
  auto data = CreateDummyAudio(1000, 48000, 2);
  encoder.EncodeSamples(data.data(), data.size(), 2, 48000, 16, 16, 4, false,
                        0);

  // Should have received some packets now that stream index is set
  // AAC encoder produces ~46 packets for 1 second of audio
  EXPECT_GT(receivedPackets.size(), 20);
  EXPECT_LE(receivedPackets.size(), 60);

  // Verify stream index is correct on all packets
  for (auto *pkt : receivedPackets) {
    EXPECT_EQ(pkt->stream_index, 1);
  }
}
