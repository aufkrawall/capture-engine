#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../common/sequence_lock.h"

namespace {

struct StressConfig {
    uint32_t frameLimit;
    uint32_t mode;
    uint64_t counter;
    int32_t padding[4];
};

TEST(SequenceLockStressTest, ConcurrentReadsUnderWrites) {
    ce::SequenceLock<StressConfig> lock;
    StressConfig init{60, 1, 0, {}};
    lock.Write(init);

    constexpr int kWriters = 2;
    constexpr int kReaders = 4;
    constexpr auto kDuration = std::chrono::milliseconds(500);
    std::atomic<bool> running{true};
    std::atomic<uint64_t> totalWrites{0};
    std::atomic<uint64_t> totalReads{0};
    std::atomic<uint64_t> readFailures{0};

    auto writer = [&](int id) {
        uint64_t count = 0;
        while (running.load(std::memory_order_acquire)) {
            StressConfig data{static_cast<uint32_t>(60 + id), static_cast<uint32_t>(1 + (count % 3)), count, {}};
            lock.Write(data);
            ++count;
            std::this_thread::yield();
        }
        totalWrites.fetch_add(count, std::memory_order_relaxed);
    };

    auto reader = [&]() {
        uint64_t count = 0;
        uint64_t failures = 0;
        while (running.load(std::memory_order_acquire)) {
            StressConfig out{};
            if (!lock.Read(out)) {
                ++failures;
            }
            ++count;
        }
        totalReads.fetch_add(count, std::memory_order_relaxed);
        readFailures.fetch_add(failures, std::memory_order_relaxed);
    };

    std::vector<std::thread> writerThreads;
    std::vector<std::thread> readerThreads;
    writerThreads.reserve(kWriters);
    readerThreads.reserve(kReaders);
    for (int i = 0; i < kWriters; ++i)
        writerThreads.emplace_back(writer, i);
    for (int i = 0; i < kReaders; ++i)
        readerThreads.emplace_back(reader);

    std::this_thread::sleep_for(kDuration);
    running.store(false, std::memory_order_release);

    for (auto& t : writerThreads)
        t.join();
    for (auto& t : readerThreads)
        t.join();

    EXPECT_EQ(readFailures.load(std::memory_order_relaxed), 0u);
    EXPECT_GT(totalWrites.load(std::memory_order_relaxed), 0u);
    EXPECT_GT(totalReads.load(std::memory_order_relaxed), 0u);
}

TEST(SequenceLockStressTest, ReadIfChangedConcurrent) {
    ce::VersionedConfig<StressConfig> config;
    StressConfig init{60, 1, 0, {}};
    config.Write(init);

    constexpr auto kDuration = std::chrono::milliseconds(300);
    std::atomic<bool> running{true};
    std::atomic<uint64_t> updateCount{0};

    auto writer = [&]() {
        uint64_t count = 0;
        while (running.load(std::memory_order_acquire)) {
            StressConfig data{60, 1, count, {}};
            config.Write(data);
            ++count;
            std::this_thread::yield();
        }
    };

    auto reader = [&]() {
        StressConfig out{};
        uint32_t version = 0;
        while (running.load(std::memory_order_acquire)) {
            if (config.ReadIfChanged(out, version)) {
                updateCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int i = 0; i < 3; ++i)
        readers.emplace_back(reader);

    std::this_thread::sleep_for(kDuration);
    running.store(false, std::memory_order_release);

    w.join();
    for (auto& t : readers)
        t.join();

    EXPECT_GT(updateCount.load(std::memory_order_relaxed), 0u);
}

TEST(SequenceLockStressTest, CWrapperAtomicSequence) {
    SequenceLockHeader header;
    header.sequence.store(0, std::memory_order_relaxed);

    constexpr auto kDuration = std::chrono::milliseconds(300);
    std::atomic<bool> running{true};

    auto writer = [&]() {
        while (running.load(std::memory_order_acquire)) {
            uint32_t seq = header.sequence.load(std::memory_order_relaxed);
            uint32_t desired = seq + 1;
            if ((desired & 1) == 0)
                ++desired;
            while (!header.sequence.compare_exchange_weak(seq, desired, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
                desired = seq + 1;
                if ((desired & 1) == 0)
                    ++desired;
            }
            header.sequence.store(desired + 1, std::memory_order_release);
            std::this_thread::yield();
        }
    };

    auto reader = [&]() {
        while (running.load(std::memory_order_acquire)) {
            uint32_t start = SequenceLock_ReadBegin(&header);
            EXPECT_EQ(start % 2, 0u);
            (void)SequenceLock_ReadRetry(&header, start);
            std::this_thread::yield();
        }
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int i = 0; i < 4; ++i)
        readers.emplace_back(reader);

    std::this_thread::sleep_for(kDuration);
    running.store(false, std::memory_order_release);

    w.join();
    for (auto& t : readers)
        t.join();
}

}  // namespace
