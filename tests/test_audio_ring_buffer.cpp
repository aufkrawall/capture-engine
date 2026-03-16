#include <gtest/gtest.h>
#include <vector>
#include "../mediaengine/audio_ring_buffer.h"

TEST(AudioRingBufferTest, InitialState) {
    AudioRingBuffer ring(8);
    EXPECT_EQ(ring.GetCapacity(), 8u);
    EXPECT_EQ(ring.GetAvailable(), 0u);
    EXPECT_EQ(ring.GetFree(), 8u);
    EXPECT_FALSE(ring.HasOverflowed());
    EXPECT_EQ(ring.GetAndClearDroppedSamples(), 0u);
    EXPECT_EQ(ring.GetAndClearRetainedSamples(), 0u);
}

TEST(AudioRingBufferTest, WriteReadRoundTrip) {
    AudioRingBuffer ring(8);
    const float input[] = {0.5f, 1.25f, -2.0f, 3.5f};
    EXPECT_EQ(ring.Write(input, 4), 4u);
    EXPECT_EQ(ring.GetAvailable(), 4u);

    std::vector<float> output(4, 0.0f);
    EXPECT_EQ(ring.Read(output.data(), output.size()), 4u);
    EXPECT_EQ(ring.GetAvailable(), 0u);

    for (size_t i = 0; i < output.size(); ++i) {
        EXPECT_FLOAT_EQ(output[i], input[i]);
    }
}

TEST(AudioRingBufferTest, OverflowDropsNewestAndTracksDroppedSamples) {
    AudioRingBuffer ring(4);
    const float initial[] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float overflow[] = {5.0f, 6.0f, 7.0f};

    EXPECT_EQ(ring.Write(initial, 4), 4u);
    EXPECT_EQ(ring.Write(overflow, 3), 0u);
    EXPECT_TRUE(ring.HasOverflowed());
    EXPECT_EQ(ring.GetAndClearDroppedSamples(), 3u);
    EXPECT_EQ(ring.GetAndClearDroppedSamples(), 0u);

    std::vector<float> output(4, 0.0f);
    EXPECT_EQ(ring.Read(output.data(), output.size()), 4u);
    EXPECT_FLOAT_EQ(output[0], 1.0f);
    EXPECT_FLOAT_EQ(output[1], 2.0f);
    EXPECT_FLOAT_EQ(output[2], 3.0f);
    EXPECT_FLOAT_EQ(output[3], 4.0f);
}

TEST(AudioRingBufferTest, WriteRetainNewKeepsLatestSamples) {
    AudioRingBuffer ring(4);
    const float initial[] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float newest[] = {5.0f, 6.0f, 7.0f};

    EXPECT_EQ(ring.Write(initial, 4), 4u);
    EXPECT_EQ(ring.WriteRetainNew(newest, 3), 3u);
    EXPECT_EQ(ring.GetAndClearDroppedSamples(), 0u);
    EXPECT_EQ(ring.GetAndClearRetainedSamples(), 3u);

    std::vector<float> output(4, 0.0f);
    EXPECT_EQ(ring.Read(output.data(), output.size()), 4u);
    EXPECT_FLOAT_EQ(output[0], 4.0f);
    EXPECT_FLOAT_EQ(output[1], 5.0f);
    EXPECT_FLOAT_EQ(output[2], 6.0f);
    EXPECT_FLOAT_EQ(output[3], 7.0f);
}

TEST(AudioRingBufferTest, ClearResetsStateAndCounters) {
    AudioRingBuffer ring(4);
    const float input[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};

    EXPECT_EQ(ring.Write(input, 6), 4u);
    EXPECT_TRUE(ring.HasOverflowed());
    EXPECT_EQ(ring.GetAndClearDroppedSamples(), 2u);
    EXPECT_EQ(ring.GetAndClearRetainedSamples(), 0u);

    ring.Clear();
    EXPECT_EQ(ring.GetAndClearDroppedSamples(), 0u);
    EXPECT_EQ(ring.GetAndClearRetainedSamples(), 0u);
    EXPECT_EQ(ring.Write(input, 6), 4u);
    EXPECT_TRUE(ring.HasOverflowed());

    ring.Clear();
    EXPECT_EQ(ring.GetAvailable(), 0u);
    EXPECT_EQ(ring.GetFree(), 4u);
    EXPECT_FALSE(ring.HasOverflowed());
    EXPECT_EQ(ring.GetAndClearDroppedSamples(), 0u);
    EXPECT_EQ(ring.GetAndClearRetainedSamples(), 0u);
}
