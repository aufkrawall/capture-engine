#include <gtest/gtest.h>

#include "../hook/common/callback_snapshot_cache.h"

namespace {
using Cache = ce::CallbackSnapshotCache<int>;

TEST(CallbackSnapshotCacheTest, StableCallbackDoesNotReenterRegistry) {
    Cache cache;
    std::atomic<uint64_t> generation{1};
    int calls = 0;
    auto loader = [&](void*) { ++calls; return Cache::Snapshot{42, generation.load()}; };
    EXPECT_EQ(cache.Resolve(nullptr, generation, loader), 42);
    EXPECT_EQ(cache.Resolve(nullptr, generation, loader), 42);
    EXPECT_EQ(calls, 1);
}

TEST(CallbackSnapshotCacheTest, ReplacementAndRemovalInvalidateTheSameKey) {
    Cache cache;
    std::atomic<uint64_t> generation{1};
    int value = 42;
    auto loader = [&](void*) { return Cache::Snapshot{value, generation.load()}; };
    EXPECT_EQ(cache.Resolve(nullptr, generation, loader), 42);
    value = 91;
    generation.fetch_add(1, std::memory_order_release);
    EXPECT_EQ(cache.Resolve(nullptr, generation, loader), 91);
    value = 0;
    generation.fetch_add(1, std::memory_order_release);
    EXPECT_EQ(cache.Resolve(nullptr, generation, loader), 0);
}

TEST(CallbackSnapshotCacheTest, DifferentContextsAndThreadCachesStayIndependent) {
    Cache first, second;
    std::atomic<uint64_t> generation{1};
    int a = 12, b = 34, calls = 0;
    auto loader = [&](void* key) {
        ++calls;
        return Cache::Snapshot{*static_cast<int*>(key), generation.load()};
    };
    EXPECT_EQ(first.Resolve(&a, generation, loader), 12);
    EXPECT_EQ(second.Resolve(&b, generation, loader), 34);
    EXPECT_EQ(first.Resolve(&b, generation, loader), 34);
    EXPECT_EQ(second.Resolve(&b, generation, loader), 34);
    EXPECT_EQ(calls, 3);
}

TEST(CallbackSnapshotCacheTest, LoaderPublishesGenerationOfItsLockedSnapshot) {
    Cache cache;
    std::atomic<uint64_t> generation{1};
    int calls = 0;
    auto loader = [&](void*) {
        ++calls;
        // A writer commits after the fast-path load but before the registry lock is acquired.
        generation.store(2, std::memory_order_release);
        return Cache::Snapshot{99, 2};
    };
    EXPECT_EQ(cache.Resolve(nullptr, generation, loader), 99);
    EXPECT_EQ(cache.Resolve(nullptr, generation, loader), 99);
    EXPECT_EQ(calls, 1);
}
}  // namespace
