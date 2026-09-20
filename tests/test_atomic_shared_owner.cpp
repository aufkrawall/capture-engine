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

TEST(AtomicSharedOwnerTest, AccessSharedAccessorPreservesPointee) {
    std::atomic<int> destroyed{0};
    std::shared_ptr<TrackedOwnerValue> preserved;
    {
        ce::AtomicSharedOwner<TrackedOwnerValue> owner(std::make_shared<TrackedOwnerValue>(42, &destroyed));
        {
            const auto access = owner.Read();
            ASSERT_TRUE(access);
            preserved = access.Shared();
            EXPECT_EQ(preserved->Read(), 42);
        }
        // access is destroyed; owner is replaced
        owner.Store(nullptr);
        EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0);
        EXPECT_EQ(preserved->Read(), 42);
    }
    // owner destroyed; preserved still holds the pointee
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0);
    preserved.reset();
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1);
}

TEST(AtomicSharedOwnerTest, RecursiveReadOnSameThreadDoesNotDeadlockWithConcurrentWriter) {
    std::atomic<int> destroyed{0};
    ce::AtomicSharedOwner<TrackedOwnerValue> owner(std::make_shared<TrackedOwnerValue>(100, &destroyed));
    std::atomic<bool> outerHeld{false};
    std::atomic<bool> writerStarted{false};
    std::atomic<bool> allowWriterToFinish{false};
    std::atomic<bool> innerFinished{false};
    std::atomic<bool> writerFinished{false};
    int innerObserved = 0;

    std::thread reader([&]() {
        const auto outer = owner.Read();
        ASSERT_TRUE(outer);
        EXPECT_EQ(outer->Read(), 100);
        outerHeld.store(true, std::memory_order_release);

        // Wait until writer thread has called LockExclusive() and is actively waiting
        while (!writerStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // On a non-recursive shared mutex with writer priority, calling Read() here
        // would block waiting for the writer to finish, while the writer waits for
        // outer to finish — causing mutual deadlock.
        // With recursive shared read tracking, the inner read succeeds immediately.
        {
            const auto inner = owner.Read();
            ASSERT_TRUE(inner);
            innerObserved = inner->Read();
            EXPECT_EQ(inner.Shared(), outer.Shared());
        }
        innerFinished.store(true, std::memory_order_release);
        allowWriterToFinish.store(true, std::memory_order_release);
        // outer is released when reader thread exits this scope
    });

    while (!outerHeld.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread writer([&]() {
        writerStarted.store(true, std::memory_order_release);
        // Exclusive access waits until all reader shared access is released
        auto exclusive = owner.LockExclusive();
        EXPECT_TRUE(allowWriterToFinish.load(std::memory_order_acquire));
        ASSERT_TRUE(exclusive);
        EXPECT_EQ(exclusive->Read(), 100);
        writerFinished.store(true, std::memory_order_release);
    });

    reader.join();
    writer.join();

    EXPECT_TRUE(innerFinished.load(std::memory_order_acquire));
    EXPECT_TRUE(writerFinished.load(std::memory_order_acquire));
    EXPECT_EQ(innerObserved, 100);
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0);
}

TEST(AtomicSharedOwnerTest, RecursiveReadDestructionOrderDoesNotPrematurelyReleaseExclusiveLock) {
    std::atomic<int> destroyed{0};
    ce::AtomicSharedOwner<TrackedOwnerValue> owner(std::make_shared<TrackedOwnerValue>(200, &destroyed));
    std::atomic<bool> innerHeld{false};
    std::atomic<bool> outerReset{false};
    std::atomic<bool> writerStarted{false};
    std::atomic<bool> writerAcquired{false};
    std::atomic<bool> innerReleased{false};

    std::thread reader([&]() {
        auto outer = std::make_unique<ce::AtomicSharedOwner<TrackedOwnerValue>::Access>(owner.Read());
        ASSERT_TRUE(*outer);
        {
            auto inner = owner.Read();
            ASSERT_TRUE(inner);
            innerHeld.store(true, std::memory_order_release);

            while (!writerStarted.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // Destroy the outer access while inner is still alive.
            // On recursive depth tracking, the underlying shared lock must remain
            // held until the last access (inner) is released.
            outer.reset();
            outerReset.store(true, std::memory_order_release);

            // Give the writer thread opportunities to run; it must NOT acquire the exclusive lock yet.
            for (int i = 0; i < 20; ++i) {
                EXPECT_FALSE(writerAcquired.load(std::memory_order_acquire));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            innerReleased.store(true, std::memory_order_release);
        }
    });

    while (!innerHeld.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread writer([&]() {
        writerStarted.store(true, std::memory_order_release);
        while (!outerReset.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Exclusive access must wait until inner is also released
        auto exclusive = owner.LockExclusive();
        writerAcquired.store(true, std::memory_order_release);
        EXPECT_TRUE(innerReleased.load(std::memory_order_acquire));
        ASSERT_TRUE(exclusive);
        EXPECT_EQ(exclusive->Read(), 200);
    });

    reader.join();
    writer.join();

    EXPECT_TRUE(writerAcquired.load(std::memory_order_acquire));
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0);
}
