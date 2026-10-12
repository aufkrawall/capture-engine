#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>

#include "common/logging/log_ring.h"
#include "source_fragment_reader.h"

namespace {
struct Ring {
    static constexpr uint32_t SLOT_COUNT = 4;
    static constexpr uint32_t SLOT_SIZE = 64;
    char buffer[SLOT_COUNT][SLOT_SIZE]{};
    std::atomic<uint8_t> committed[SLOT_COUNT]{};
    std::atomic<uint32_t> writeIndex{0}, readIndex{0}, overflowCount{0};
};

struct ScriptedIndex {
    uint32_t value = 0;
    std::function<void()> onLoad, onCompare;
    uint32_t load(std::memory_order) {
        if (onLoad) {
            std::function<void()> callback;
            callback.swap(onLoad);
            callback();
        }
        return value;
    }
    bool compare_exchange_weak(uint32_t& expected, uint32_t desired, std::memory_order, std::memory_order) {
        if (onCompare) {
            std::function<void()> callback;
            callback.swap(onCompare);
            callback();
        }
        if (expected != value) {
            expected = value;
            return false;
        }
        value = desired;
        return true;
    }
};
struct ScriptedRing {
    static constexpr uint32_t SLOT_COUNT = 4;
    ScriptedIndex writeIndex, readIndex;
};
}  // namespace

TEST(LogRingTest, ConsumerPassingAnOldWriteSnapshotDoesNotLookFull) {
    ScriptedRing ring;
    ring.readIndex.onLoad = [&] {
        ring.writeIndex.value = 100;
        ring.readIndex.value = 100;
    };
    uint32_t index = 0;
    ASSERT_TRUE(ce::logging::TryReserveLogSlot(ring, index));
    EXPECT_EQ(index, 100u);
}

TEST(LogRingTest, CasRetryRefreshesBothSnapshotsAfterCompetingProgress) {
    ScriptedRing ring;
    ring.writeIndex.onCompare = [&] {
        ring.writeIndex.value = 10;
        ring.readIndex.value = 10;
    };
    uint32_t index = 0;
    ASSERT_TRUE(ce::logging::TryReserveLogSlot(ring, index));
    EXPECT_EQ(index, 10u);
}

TEST(LogRingTest, FullRingDoesNotReserveAHoleAndReleasedSlotsAreReusable) {
    Ring ring;
    for (int index = 0; index < 4; ++index) {
        ASSERT_EQ(ce::logging::TryPushLogRecord(ring, "a", std::to_string(index)), ce::logging::LogRingPush::Published);
    }
    EXPECT_EQ(ce::logging::TryPushLogRecord(ring, "a", "full"), ce::logging::LogRingPush::Full);
    EXPECT_EQ(ring.writeIndex.load(), 4u);
    EXPECT_EQ(ring.overflowCount.load(), 1u);
    uint32_t read = ring.readIndex.load();
    char record[Ring::SLOT_SIZE]{};
    ASSERT_TRUE(ce::logging::TryPopLogRecord(ring, read, record));
    EXPECT_STREQ(record, "[a] 0");
    ASSERT_EQ(ce::logging::TryPushLogRecord(ring, "a", "4"), ce::logging::LogRingPush::Published);
    for (int index = 1; index <= 4; ++index) {
        ASSERT_TRUE(ce::logging::TryPopLogRecord(ring, read, record));
        EXPECT_EQ(std::string(record), "[a] " + std::to_string(index));
    }
    EXPECT_FALSE(ce::logging::TryPopLogRecord(ring, read, record));
}

TEST(LogRingTest, UncommittedHeadBlocksLaterRecordsWithoutConsumingOrDuplicatingThem) {
    Ring ring;
    uint32_t reserved = 0;
    ASSERT_TRUE(ce::logging::TryReserveLogSlot(ring, reserved));
    ASSERT_EQ(ce::logging::TryPushLogRecord(ring, "a", "second"), ce::logging::LogRingPush::Published);
    uint32_t read = ring.readIndex.load();
    char record[Ring::SLOT_SIZE]{};
    EXPECT_FALSE(ce::logging::TryPopLogRecord(ring, read, record));
    EXPECT_EQ(read, 0u);
    std::strcpy(ring.buffer[reserved], "[a] first");
    ring.committed[reserved].store(1, std::memory_order_release);
    ASSERT_TRUE(ce::logging::TryPopLogRecord(ring, read, record));
    EXPECT_STREQ(record, "[a] first");
    // A restarted consumer begins at the published read cursor, preserving the backlog.
    read = ring.readIndex.load();
    ASSERT_TRUE(ce::logging::TryPopLogRecord(ring, read, record));
    EXPECT_STREQ(record, "[a] second");
    EXPECT_FALSE(ce::logging::TryPopLogRecord(ring, read, record));
}

TEST(LogRingTest, CounterWrapPreservesReservationAndConsumptionOrder) {
    Ring ring;
    ring.readIndex.store(std::numeric_limits<uint32_t>::max() - 1);
    ring.writeIndex.store(ring.readIndex.load());
    for (int index = 0; index < 4; ++index) {
        ASSERT_EQ(ce::logging::TryPushLogRecord(ring, "a", std::to_string(index)), ce::logging::LogRingPush::Published);
    }
    uint32_t read = ring.readIndex.load();
    char record[Ring::SLOT_SIZE]{};
    for (int index = 0; index < 4; ++index) {
        ASSERT_TRUE(ce::logging::TryPopLogRecord(ring, read, record));
        EXPECT_EQ(std::string(record), "[a] " + std::to_string(index));
    }
    EXPECT_EQ(read, 2u);
    EXPECT_FALSE(ce::logging::TryPopLogRecord(ring, read, record));
}

TEST(LogRingTest, OversizedRecordDoesNotTruncateOrReserveASlot) {
    Ring ring;
    EXPECT_EQ(ce::logging::TryPushLogRecord(ring, "a", std::string(60, 'x')), ce::logging::LogRingPush::TooLarge);
    EXPECT_EQ(ring.writeIndex.load(), 0u);
    EXPECT_EQ(ring.overflowCount.load(), 0u);
    EXPECT_EQ(ce::logging::TryPushLogRecord(ring, "a", std::string(59, 'x')), ce::logging::LogRingPush::Published);
    uint32_t read = 0;
    char record[Ring::SLOT_SIZE]{};
    ASSERT_TRUE(ce::logging::TryPopLogRecord(ring, read, record));
    EXPECT_EQ(std::string(record), "[a] " + std::string(59, 'x'));
}

TEST(LogRingSourceTest, ProducersShareTheProtocolAndServiceResumesTheReleasedCursor) {
    for (const char* path : {"hook/runtime/hook_common.cpp", "hook/vulkan_layer/layer_ipc.cpp"}) {
        const auto source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / path);
        EXPECT_NE(source.find("ce::logging::TryPushLogRecord("), std::string::npos) << path;
    }
    const auto source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() /
                                                           "captureengine/diagnostics/logger_service.cpp");
    EXPECT_NE(source.find("ce::logging::TryPopLogRecord("), std::string::npos);
    EXPECT_NE(source.find("shm->logs.readIndex.load(std::memory_order_acquire)"), std::string::npos);
    EXPECT_EQ(source.find("sessions[pid] = {hSM, shm, currentWriteIdx"), std::string::npos);
    EXPECT_NE(source.find("readIdx != writeIdx && ce::logging::TryPopLogRecord("), std::string::npos);
    EXPECT_NE(source.find("finalDrain = true;"), std::string::npos);
}
