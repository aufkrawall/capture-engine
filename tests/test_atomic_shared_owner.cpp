#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

#include "../common/atomic_shared_owner.h"

namespace {

struct TrackedOwnerValue {
    explicit TrackedOwnerValue(int value, std::atomic<int>* destroyed) : value(value), destroyed(destroyed) {}
    ~TrackedOwnerValue() {
        destroyed->fetch_add(1, std::memory_order_relaxed);
    }

    int Read() const {
        return value;
    }

    int ReadAfterGate(std::atomic<bool>* entered, std::atomic<bool>* proceed) const {
        entered->store(true, std::memory_order_release);
        while (!proceed->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return value;
    }

    int value;
    std::atomic<int>* destroyed;
};

}  // namespace

TEST(AtomicSharedOwnerTest, ExchangeReturnsRetiredOwnerForControlledDestruction) {
    std::atomic<int> destroyed{0};
    ce::AtomicSharedOwner<TrackedOwnerValue> owner(std::make_shared<TrackedOwnerValue>(1, &destroyed));

    auto retired = owner.Exchange(std::make_shared<TrackedOwnerValue>(2, &destroyed));
    EXPECT_EQ(owner->Read(), 2);
    EXPECT_EQ(retired->Read(), 1);
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0);

    retired.reset();
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1);
}

TEST(AtomicSharedOwnerTest, ArrowAccessPinsPointeeAcrossConcurrentExchange) {
    std::atomic<int> destroyed{0};
    ce::AtomicSharedOwner<TrackedOwnerValue> owner(std::make_shared<TrackedOwnerValue>(7, &destroyed));
    std::atomic<bool> readerLoaded{false};
    std::atomic<bool> allowReaderToFinish{false};
    std::atomic<bool> exchangeStarted{false};
    std::atomic<bool> exchangeFinished{false};
    int observed = 0;

    std::thread reader([&]() { observed = owner->ReadAfterGate(&readerLoaded, &allowReaderToFinish); });

    while (!readerLoaded.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::thread exchanger([&]() {
        exchangeStarted.store(true, std::memory_order_release);
        auto retired = owner.Exchange(std::make_shared<TrackedOwnerValue>(9, &destroyed));
        retired.reset();
        exchangeFinished.store(true, std::memory_order_release);
    });
    while (!exchangeStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    EXPECT_FALSE(exchangeFinished.load(std::memory_order_acquire));
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0);

    allowReaderToFinish.store(true, std::memory_order_release);
    reader.join();
    exchanger.join();
    EXPECT_EQ(observed, 7);
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(owner->Read(), 9);
}
