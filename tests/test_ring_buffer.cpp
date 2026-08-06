/**
 * Unit tests for LockFreeRingBuffer and DynamicRingBuffer (common/ring_buffer.h)
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "../common/ring_buffer.h"

using namespace ce;

// ============================================================================
// LockFreeRingBuffer tests
// ============================================================================

class LockFreeRingBufferTest : public ::testing::Test {
protected:
    LockFreeRingBuffer<int, 8> buf;  // capacity 8 (power-of-2 required)
};

TEST_F(LockFreeRingBufferTest, InitialState) {
    EXPECT_TRUE(buf.Empty());
    EXPECT_FALSE(buf.Full());
    EXPECT_EQ(buf.Size(), 0u);
    EXPECT_EQ(buf.Capacity(), 8u);
    EXPECT_EQ(buf.Available(), 8u);
    EXPECT_EQ(buf.DroppedCount(), 0u);
}

TEST_F(LockFreeRingBufferTest, PushPop) {
    EXPECT_TRUE(buf.Push(42));
    EXPECT_FALSE(buf.Empty());
    EXPECT_EQ(buf.Size(), 1u);

    int val = 0;
    EXPECT_TRUE(buf.Pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_TRUE(buf.Empty());
    EXPECT_EQ(buf.Size(), 0u);
}

TEST_F(LockFreeRingBufferTest, PopEmpty) {
    int val = 0;
    EXPECT_FALSE(buf.Pop(val));  // Empty buffer returns false
}

TEST_F(LockFreeRingBufferTest, FillToCapacity) {
    for (int i = 0; i < 8; i++) {
        EXPECT_TRUE(buf.Push(i));
    }
    EXPECT_TRUE(buf.Full());
    EXPECT_EQ(buf.Size(), 8u);
    EXPECT_EQ(buf.Available(), 0u);
}

TEST_F(LockFreeRingBufferTest, DropNewWhenFull) {
    // Default policy is DropNew
    for (int i = 0; i < 8; i++) {
        buf.Push(i);
    }
    EXPECT_FALSE(buf.Push(99));  // Should be dropped
    EXPECT_EQ(buf.DroppedCount(), 1u);
    EXPECT_EQ(buf.Size(), 8u);

    // Original items still intact
    int val = 0;
    EXPECT_TRUE(buf.Pop(val));
    EXPECT_EQ(val, 0);
}

TEST_F(LockFreeRingBufferTest, DropOldFailsClosedWhenFull) {
    // DropOld/Overwrite would race a concurrent consumer (torn slot read), so
    // the lock-free SPSC variant fails closed by dropping the new element.
    LockFreeRingBuffer<int, 4> dropOld(RingBufferPolicy::DropOld);
    for (int i = 0; i < 4; i++) {
        dropOld.Push(i);
    }
    // Buffer full: 0,1,2,3
    EXPECT_FALSE(dropOld.Push(10));  // Fail closed: new element dropped
    EXPECT_EQ(dropOld.DroppedCount(), 1u);
    EXPECT_EQ(dropOld.Size(), 4u);   // Oldest element is retained

    int val = 0;
    EXPECT_TRUE(dropOld.Pop(val));
    EXPECT_EQ(val, 0);
}

TEST_F(LockFreeRingBufferTest, Peek) {
    buf.Push(7);
    buf.Push(8);

    int val = 0;
    EXPECT_TRUE(buf.Peek(val));
    EXPECT_EQ(val, 7);
    EXPECT_EQ(buf.Size(), 2u);  // Peek doesn't consume

    EXPECT_TRUE(buf.Pop(val));
    EXPECT_EQ(val, 7);  // Same element returned
}

TEST_F(LockFreeRingBufferTest, PeekEmpty) {
    int val = 0;
    EXPECT_FALSE(buf.Peek(val));
}

TEST_F(LockFreeRingBufferTest, Skip) {
    buf.Push(1);
    buf.Push(2);
    buf.Push(3);

    EXPECT_TRUE(buf.Skip());  // Skips 1
    EXPECT_EQ(buf.Size(), 2u);

    int val = 0;
    EXPECT_TRUE(buf.Pop(val));
    EXPECT_EQ(val, 2);  // 1 was skipped
}

TEST_F(LockFreeRingBufferTest, SkipEmpty) {
    EXPECT_FALSE(buf.Skip());
}

TEST_F(LockFreeRingBufferTest, Clear) {
    buf.Push(1);
    buf.Push(2);
    buf.Push(3);
    EXPECT_EQ(buf.Size(), 3u);

    buf.Clear();
    EXPECT_TRUE(buf.Empty());
    EXPECT_EQ(buf.Size(), 0u);
}

TEST_F(LockFreeRingBufferTest, Wraparound) {
    // Push and pop alternately to force index wraparound past capacity
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 8; i++) {
            EXPECT_TRUE(buf.Push(round * 8 + i));
        }
        for (int i = 0; i < 8; i++) {
            int val = 0;
            EXPECT_TRUE(buf.Pop(val));
            EXPECT_EQ(val, round * 8 + i);
        }
    }
    EXPECT_TRUE(buf.Empty());
}

TEST_F(LockFreeRingBufferTest, DroppedCountReset) {
    LockFreeRingBuffer<int, 2> small;
    small.Push(1);
    small.Push(2);
    small.Push(3);  // dropped
    EXPECT_EQ(small.DroppedCount(), 1u);

    small.ResetDroppedCount();
    EXPECT_EQ(small.DroppedCount(), 0u);
}

TEST_F(LockFreeRingBufferTest, OverwritePolicyFailsClosedWhenFull) {
    LockFreeRingBuffer<int, 4> overwrite(RingBufferPolicy::Overwrite);
    for (int i = 0; i < 4; i++) {
        overwrite.Push(i);  // 0,1,2,3
    }
    EXPECT_FALSE(overwrite.Push(99));  // Fail closed: new element dropped
    EXPECT_EQ(overwrite.DroppedCount(), 1u);
    EXPECT_EQ(overwrite.Size(), 4u);
    EXPECT_EQ(overwrite.Available(), 0u);

    int value = 0;
    for (int expected : {0, 1, 2, 3}) {
        ASSERT_TRUE(overwrite.Pop(value));
        EXPECT_EQ(value, expected);
    }
    EXPECT_TRUE(overwrite.Empty());
}

TEST_F(LockFreeRingBufferTest, DirectIndexAccess) {
    buf.Push(10);
    buf.Push(20);
    // Direct indexed access uses ring modulo
    EXPECT_EQ(buf[buf.ReadIndex()], 10);
}

// ============================================================================
// DynamicRingBuffer tests
// ============================================================================

class DynamicRingBufferTest : public ::testing::Test {
protected:
    DynamicRingBuffer<int> buf{8};
};

TEST_F(DynamicRingBufferTest, InitialState) {
    EXPECT_TRUE(buf.Empty());
    EXPECT_FALSE(buf.Full());
    EXPECT_EQ(buf.Size(), 0u);
    EXPECT_EQ(buf.Capacity(), 8u);
}

TEST_F(DynamicRingBufferTest, PushPop) {
    EXPECT_TRUE(buf.Push(100));
    int val = 0;
    EXPECT_TRUE(buf.Pop(val));
    EXPECT_EQ(val, 100);
}

TEST_F(DynamicRingBufferTest, PopEmpty) {
    int val = 0;
    EXPECT_FALSE(buf.Pop(val));
}

TEST_F(DynamicRingBufferTest, FullDetection) {
    for (int i = 0; i < 8; i++) {
        buf.Push(i);
    }
    EXPECT_TRUE(buf.Full());
    EXPECT_FALSE(buf.Push(99));  // DropNew by default
    EXPECT_EQ(buf.DroppedCount(), 1u);
}

TEST_F(DynamicRingBufferTest, Peek) {
    buf.Push(5);
    int val = 0;
    EXPECT_TRUE(buf.Peek(val));
    EXPECT_EQ(val, 5);
    EXPECT_EQ(buf.Size(), 1u);  // Still there
}

TEST_F(DynamicRingBufferTest, Skip) {
    buf.Push(1);
    buf.Push(2);
    EXPECT_TRUE(buf.Skip());
    int val = 0;
    EXPECT_TRUE(buf.Pop(val));
    EXPECT_EQ(val, 2);
}

TEST_F(DynamicRingBufferTest, Clear) {
    buf.Push(1);
    buf.Push(2);
    buf.Clear();
    EXPECT_TRUE(buf.Empty());
}

TEST_F(DynamicRingBufferTest, Resize) {
    buf.Push(1);
    buf.Push(2);
    buf.Resize(16);
    EXPECT_EQ(buf.Capacity(), 16u);
    EXPECT_TRUE(buf.Empty());  // Resize clears
}

TEST_F(DynamicRingBufferTest, MultipleValues) {
    for (int i = 0; i < 8; i++) {
        EXPECT_TRUE(buf.Push(i));
    }
    for (int i = 0; i < 8; i++) {
        int val = 0;
        EXPECT_TRUE(buf.Pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(buf.Empty());
}

TEST_F(DynamicRingBufferTest, DropOldPolicyFailsClosedWhenFull) {
    DynamicRingBuffer<int> dropOld(4, RingBufferPolicy::DropOld);
    for (int i = 0; i < 4; i++) {
        dropOld.Push(i);
    }
    EXPECT_FALSE(dropOld.Push(10));  // Fail closed: new element dropped
    EXPECT_EQ(dropOld.DroppedCount(), 1u);
    EXPECT_EQ(dropOld.Size(), 4u);
    int val = 0;
    dropOld.Pop(val);
    EXPECT_EQ(val, 0);
}

TEST_F(DynamicRingBufferTest, OverwritePolicyFailsClosedWhenFull) {
    DynamicRingBuffer<int> overwrite(4, RingBufferPolicy::Overwrite);
    for (int i = 0; i < 4; i++) {
        overwrite.Push(i);
    }
    EXPECT_FALSE(overwrite.Push(10));  // Fail closed: new element dropped

    EXPECT_EQ(overwrite.DroppedCount(), 1u);
    EXPECT_EQ(overwrite.Size(), 4u);
    EXPECT_EQ(overwrite.Available(), 0u);

    int value = 0;
    for (int expected : {0, 1, 2, 3}) {
        ASSERT_TRUE(overwrite.Pop(value));
        EXPECT_EQ(value, expected);
    }
}

TEST_F(DynamicRingBufferTest, ZeroCapacityRejectsPushForEveryPolicy) {
    for (RingBufferPolicy policy :
         {RingBufferPolicy::DropNew, RingBufferPolicy::DropOld, RingBufferPolicy::Overwrite, RingBufferPolicy::Block}) {
        DynamicRingBuffer<int> zeroCapacity(0, policy);
        EXPECT_TRUE(zeroCapacity.Empty());
        EXPECT_TRUE(zeroCapacity.Full());
        EXPECT_EQ(zeroCapacity.Available(), 0u);
        EXPECT_FALSE(zeroCapacity.Push(1));
        EXPECT_EQ(zeroCapacity.DroppedCount(), 1u);
    }
}

TEST_F(DynamicRingBufferTest, MovedFromBufferIsSafelyEmpty) {
    DynamicRingBuffer<int> source(4);
    source.Push(1);

    DynamicRingBuffer<int> destination(std::move(source));

    // NOLINTNEXTLINE(bugprone-use-after-move) - intentionally verify the moved-from buffer contract
    EXPECT_EQ(source.Capacity(), 0u);
    EXPECT_TRUE(source.Empty());
    EXPECT_FALSE(source.Push(2));

    int value = 0;
    ASSERT_TRUE(destination.Pop(value));
    EXPECT_EQ(value, 1);
}

TEST_F(DynamicRingBufferTest, MoveAssignmentLeavesSourceSafelyEmpty) {
    DynamicRingBuffer<int> source(4);
    source.Push(7);
    DynamicRingBuffer<int> destination(2);

    destination = std::move(source);

    // NOLINTNEXTLINE(bugprone-use-after-move) - intentionally verify the moved-from buffer contract
    EXPECT_EQ(source.Capacity(), 0u);
    EXPECT_TRUE(source.Empty());
    EXPECT_FALSE(source.Push(8));

    int value = 0;
    ASSERT_TRUE(destination.Pop(value));
    EXPECT_EQ(value, 7);
}

// ============================================================================
// Concurrent SPSC correctness test
// ============================================================================

TEST(RingBufferConcurrentTest, SPSCCorrectness) {
    LockFreeRingBuffer<int, 64> buf;
    const int kItems = 10000;
    std::vector<int> received;
    received.reserve(kItems);

    std::thread producer([&]() {
        for (int i = 0; i < kItems; i++) {
            while (!buf.Push(i)) {
                // Spin until space available (buffer may fill briefly)
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        int val = 0;
        int count = 0;
        while (count < kItems) {
            if (buf.Pop(val)) {
                received.push_back(val);
                count++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ((int)received.size(), kItems);
    for (int i = 0; i < kItems; i++) {
        EXPECT_EQ(received[i], i);
    }
}

TEST(RingBufferConcurrentTest, DropOldPolicyNeverPublishesTornElements) {
    struct TaggedValue {
        uint32_t sequence = 0;
        uint32_t checksum = 0;
    };
    LockFreeRingBuffer<TaggedValue, 4> buffer(RingBufferPolicy::DropOld);
    constexpr uint32_t kItems = 20000;
    std::atomic<bool> done{false};
    std::atomic<uint32_t> invalidReads{0};
    std::atomic<uint32_t> received{0};

    std::thread producer([&]() {
        for (uint32_t i = 1; i <= kItems; ++i) {
            // Pushes while full are dropped (fail-closed contract); that is
            // fine for this test, which verifies every *published* element is
            // fully written (no torn reads under a concurrent consumer).
            buffer.Push(TaggedValue{i, i * 2u + 1u});
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        TaggedValue value{};
        uint32_t last = 0;
        while (!done.load(std::memory_order_acquire) || !buffer.Empty()) {
            if (buffer.Pop(value)) {
                if (value.checksum != value.sequence * 2u + 1u ||
                    value.sequence <= last) {
                    invalidReads.fetch_add(1, std::memory_order_relaxed);
                }
                last = value.sequence;
                received.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(invalidReads.load(), 0u);
    EXPECT_GT(received.load(), 0u);
    EXPECT_LE(received.load(), kItems);
}
