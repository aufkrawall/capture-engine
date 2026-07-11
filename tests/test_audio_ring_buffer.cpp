#include <gtest/gtest.h>
#include <atomic>
#include <limits>
#include <thread>
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

TEST(AudioRingBufferTest, PeekDoesNotConsumeAcrossWraparound) {
    AudioRingBuffer ring(4);
    const float seed[] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float wrap[] = {5.0f, 6.0f};

    EXPECT_EQ(ring.Write(seed, 4), 4u);
    std::vector<float> scratch(2, 0.0f);
    EXPECT_EQ(ring.Read(scratch.data(), scratch.size()), 2u);
    EXPECT_EQ(ring.Write(wrap, 2), 2u);

    std::vector<float> peeked(4, 0.0f);
    EXPECT_EQ(ring.Peek(peeked.data(), peeked.size()), 4u);
    EXPECT_FLOAT_EQ(peeked[0], 3.0f);
    EXPECT_FLOAT_EQ(peeked[1], 4.0f);
    EXPECT_FLOAT_EQ(peeked[2], 5.0f);
    EXPECT_FLOAT_EQ(peeked[3], 6.0f);
    EXPECT_EQ(ring.GetAvailable(), 4u);

    std::vector<float> output(4, 0.0f);
    EXPECT_EQ(ring.Read(output.data(), output.size()), 4u);
    EXPECT_EQ(output, peeked);
}

TEST(AudioRingBufferTest, SkipConsumesOldestSamplesAcrossWraparound) {
    AudioRingBuffer ring(5);
    const float input[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    const float wrap[] = {6.0f, 7.0f};

    EXPECT_EQ(ring.Write(input, 5), 5u);
    EXPECT_EQ(ring.Skip(3), 3u);
    EXPECT_EQ(ring.Write(wrap, 2), 2u);

    std::vector<float> output(4, 0.0f);
    EXPECT_EQ(ring.Read(output.data(), output.size()), 4u);
    EXPECT_FLOAT_EQ(output[0], 4.0f);
    EXPECT_FLOAT_EQ(output[1], 5.0f);
    EXPECT_FLOAT_EQ(output[2], 6.0f);
    EXPECT_FLOAT_EQ(output[3], 7.0f);
}

TEST(AudioRingBufferTest, WriteRetainNewWithOversizedInputKeepsNewestCapacityWindow) {
    AudioRingBuffer ring(4);
    const float input[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};

    EXPECT_EQ(ring.WriteRetainNew(input, 6), 4u);
    EXPECT_TRUE(ring.HasOverflowed());
    EXPECT_EQ(ring.GetAndClearDroppedSamples(), 0u);
    EXPECT_EQ(ring.GetAndClearRetainedSamples(), 2u);

    std::vector<float> output(4, 0.0f);
    EXPECT_EQ(ring.Read(output.data(), output.size()), 4u);
    EXPECT_FLOAT_EQ(output[0], 3.0f);
    EXPECT_FLOAT_EQ(output[1], 4.0f);
    EXPECT_FLOAT_EQ(output[2], 5.0f);
    EXPECT_FLOAT_EQ(output[3], 6.0f);
}

// EnsureCapacity backs the deferred app-audio ring buffer growth: idle sources
// keep a small buffer; on first capture the buffer grows in-place to full size.
TEST(AudioRingBufferTest, EnsureCapacityGrowsEmptyBufferAndAcceptsFullWindow) {
    AudioRingBuffer ring(2);  // small initial (deferred app-audio source)
    EXPECT_EQ(ring.GetCapacity(), 2u);

    EXPECT_TRUE(ring.EnsureCapacity(8));
    EXPECT_EQ(ring.GetCapacity(), 8u);
    EXPECT_EQ(ring.GetAvailable(), 0u);
    EXPECT_EQ(ring.GetFree(), 8u);

    // The grown buffer behaves like one constructed at the full size.
    const float input[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    EXPECT_EQ(ring.Write(input, 8), 8u);
    EXPECT_FALSE(ring.HasOverflowed());
    std::vector<float> output(8, 0.0f);
    EXPECT_EQ(ring.Read(output.data(), output.size()), 8u);
    for (size_t i = 0; i < output.size(); ++i) {
        EXPECT_FLOAT_EQ(output[i], input[i]);
    }
}

TEST(AudioRingBufferTest, EnsureCapacityNeverShrinksAndIsIdempotent) {
    AudioRingBuffer ring(8);
    EXPECT_TRUE(ring.EnsureCapacity(4));  // already large enough
    EXPECT_EQ(ring.GetCapacity(), 8u);
    EXPECT_TRUE(ring.EnsureCapacity(8));  // equal
    EXPECT_EQ(ring.GetCapacity(), 8u);
}

TEST(AudioRingBufferTest, EnsureCapacityRefusesToGrowWhileHoldingBufferedAudio) {
    AudioRingBuffer ring(4);
    const float input[] = {1.0f, 2.0f};
    EXPECT_EQ(ring.Write(input, 2), 2u);  // buffer now non-empty

    EXPECT_FALSE(ring.EnsureCapacity(16));  // must not realloc with data present
    EXPECT_EQ(ring.GetCapacity(), 4u);
    EXPECT_EQ(ring.GetAvailable(), 2u);

    // After draining, growth is allowed again.
    std::vector<float> output(2, 0.0f);
    EXPECT_EQ(ring.Read(output.data(), output.size()), 2u);
    EXPECT_TRUE(ring.EnsureCapacity(16));
    EXPECT_EQ(ring.GetCapacity(), 16u);
}

TEST(AudioRingBufferTest, ZeroCapacityDropsSafelyAndCanGrowLater) {
    AudioRingBuffer ring(0);
    const float input[] = {1.0f, 2.0f};
    float output[2] = {};

    EXPECT_EQ(ring.Write(input, 2), 0u);
    EXPECT_EQ(ring.GetAndClearDroppedSamples(), 2u);
    EXPECT_EQ(ring.WriteRetainNew(input, 2), 0u);
    EXPECT_EQ(ring.GetAndClearRetainedSamples(), 2u);
    EXPECT_EQ(ring.Read(output, 2), 0u);
    EXPECT_EQ(ring.Skip(2), 0u);

    EXPECT_TRUE(ring.EnsureCapacity(4));
    EXPECT_EQ(ring.Write(input, 2), 2u);
    EXPECT_EQ(ring.Read(output, 2), 2u);
    EXPECT_FLOAT_EQ(output[0], 1.0f);
    EXPECT_FLOAT_EQ(output[1], 2.0f);
}

TEST(AudioRingBufferTest, CapacityPublicationIsRaceFreeDuringDeferredGrowth) {
    AudioRingBuffer ring(2);
    std::atomic<bool> start{false};
    std::atomic<bool> readerEntered{false};
    std::atomic<bool> done{false};
    std::atomic<bool> sawInvalidCapacity{false};

    std::thread reader([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const size_t initialCapacity = ring.GetCapacity();
        const size_t initialFree = ring.GetFree();
        if (initialCapacity != 2 || initialFree > initialCapacity) {
            sawInvalidCapacity.store(true, std::memory_order_relaxed);
        }
        readerEntered.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
            const size_t capacity = ring.GetCapacity();
            const size_t free = ring.GetFree();
            // These are separate lock-free API calls, not one transactional
            // snapshot: growth may publish between them. Each value must be a
            // complete old or new state; a mixed old-capacity/new-free pair is
            // valid and must not be compared as if sampled atomically.
            if ((capacity != 2 && capacity != 8192) || (free != 2 && free != 8192)) {
                sawInvalidCapacity.store(true, std::memory_order_relaxed);
            }
        }
    });

    start.store(true, std::memory_order_release);
    while (!readerEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(ring.EnsureCapacity(8192));
    done.store(true, std::memory_order_release);
    reader.join();

    EXPECT_FALSE(sawInvalidCapacity.load(std::memory_order_relaxed));
    EXPECT_EQ(ring.GetCapacity(), 8192u);
    EXPECT_EQ(ring.GetFree(), 8192u);
}

TEST(AudioRingBufferTest, FailedGrowthPreservesExistingEmptyBuffer) {
    AudioRingBuffer ring(4);

    EXPECT_FALSE(ring.EnsureCapacity(std::numeric_limits<size_t>::max()));
    EXPECT_EQ(ring.GetCapacity(), 4u);
    EXPECT_EQ(ring.GetAvailable(), 0u);

    const float input[] = {1.0f, 2.0f, 3.0f, 4.0f};
    float output[4] = {};
    EXPECT_EQ(ring.Write(input, 4), 4u);
    EXPECT_EQ(ring.Read(output, 4), 4u);
    EXPECT_FLOAT_EQ(output[0], 1.0f);
    EXPECT_FLOAT_EQ(output[3], 4.0f);
}
