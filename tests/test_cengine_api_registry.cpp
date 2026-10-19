// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/api/handle_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <latch>
#include <thread>
#include <vector>

// The live-handle registry behind every ce_runtime_* entry point (llm-wiki/library/03 "Strings and
// memory": stale handles return CE_E_INVALID_ARGUMENT; 02 "Ownership limits": one runtime per process,
// a second create is CE_E_BUSY, create after destroy works). Tested on a private instance over a stand-in
// type, so the process-wide registry stays free for the runtime's own tests.
namespace {

struct FakeRuntime {
    int id = 0;
};

using Registry = ce::capi::HandleRegistry<FakeRuntime>;

TEST(CengineApiRegistryTest, NothingIsLiveInTheBeginning) {
    Registry registry;
    FakeRuntime runtime;
    EXPECT_EQ(registry.GetState(), Registry::State::Empty);
    EXPECT_EQ(registry.Find(nullptr), nullptr);
    EXPECT_EQ(registry.Find(&runtime), nullptr) << "a handle nobody issued";
    EXPECT_EQ(registry.Retire(&runtime), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(registry.Generation(), 0u);
}

TEST(CengineApiRegistryTest, PublishedHandleResolvesAndStaleOnesDoNot) {
    Registry registry;
    FakeRuntime first;
    FakeRuntime other;
    auto claim = registry.TryClaim();
    ASSERT_TRUE(claim);
    EXPECT_EQ(registry.Find(&first), nullptr) << "claimed but not yet published is not a live handle";
    ASSERT_TRUE(claim.Publish(&first));
    EXPECT_EQ(registry.GetState(), Registry::State::Live);
    EXPECT_EQ(registry.Find(&first), &first);
    EXPECT_EQ(registry.Find(&other), nullptr) << "a pointer that is not the handle";
    EXPECT_EQ(registry.Find(nullptr), nullptr);
    EXPECT_EQ(registry.Generation(), 1u);

    ASSERT_EQ(registry.Retire(&first), CE_OK);
    EXPECT_EQ(registry.GetState(), Registry::State::Empty);
    EXPECT_EQ(registry.Find(&first), nullptr) << "after destroy the handle is stale";
    EXPECT_EQ(registry.Retire(&first), CE_E_INVALID_ARGUMENT) << "a second destroy is a stale handle too";
}

TEST(CengineApiRegistryTest, SecondRuntimeIsBusyUntilTheFirstIsRetired) {
    Registry registry;
    FakeRuntime first;
    FakeRuntime second;
    auto claim = registry.TryClaim();
    ASSERT_TRUE(claim);
    ASSERT_TRUE(claim.Publish(&first));

    auto refused = registry.TryClaim();
    EXPECT_FALSE(refused);
    EXPECT_EQ(refused.status(), CE_E_BUSY);
    EXPECT_FALSE(refused.Publish(&second)) << "a refused claim cannot publish";
    EXPECT_EQ(registry.Find(&first), &first) << "the failed attempt must not disturb the live runtime";
    EXPECT_EQ(registry.Find(&second), nullptr);

    ASSERT_EQ(registry.Retire(&first), CE_OK);
    auto again = registry.TryClaim();  // recreate after destroy
    ASSERT_TRUE(again);
    ASSERT_TRUE(again.Publish(&second));
    EXPECT_EQ(registry.Find(&second), &second);
    EXPECT_EQ(registry.Find(&first), nullptr);
    EXPECT_EQ(registry.Generation(), 2u);
}

TEST(CengineApiRegistryTest, AClaimInProgressAlreadyMakesCreateBusy) {
    Registry registry;
    auto claim = registry.TryClaim();
    ASSERT_TRUE(claim);
    EXPECT_EQ(registry.GetState(), Registry::State::Claiming);
    auto second = registry.TryClaim();
    EXPECT_EQ(second.status(), CE_E_BUSY) << "no second runtime may start constructing";
}

TEST(CengineApiRegistryTest, AbandonedClaimFreesTheSlot) {
    Registry registry;
    {
        auto claim = registry.TryClaim();  // create failed before it could publish
        ASSERT_TRUE(claim);
    }
    EXPECT_EQ(registry.GetState(), Registry::State::Empty);
    EXPECT_EQ(registry.Generation(), 0u);
    auto retry = registry.TryClaim();
    EXPECT_TRUE(retry);

    FakeRuntime runtime;
    auto moved = std::move(retry);  // a moved claim keeps the slot exactly once
    EXPECT_EQ(registry.GetState(), Registry::State::Claiming);
    ASSERT_TRUE(moved.Publish(&runtime));
    EXPECT_FALSE(moved.Publish(&runtime)) << "a claim publishes once";
    EXPECT_EQ(registry.Find(&runtime), &runtime);
}

TEST(CengineApiRegistryTest, PublishingNullAbandonsTheClaim) {
    Registry registry;
    auto claim = registry.TryClaim();
    ASSERT_TRUE(claim);
    EXPECT_FALSE(claim.Publish(nullptr));
    EXPECT_EQ(registry.GetState(), Registry::State::Empty);
}

TEST(CengineApiRegistryTest, EveryFailedLookupIsCounted) {
    Registry registry;
    FakeRuntime runtime;
    EXPECT_EQ(registry.Misses(), 0u);
    (void)registry.Find(&runtime);
    (void)registry.Find(nullptr);
    EXPECT_EQ(registry.Misses(), 2u);
    auto claim = registry.TryClaim();
    ASSERT_TRUE(claim.Publish(&runtime));
    (void)registry.Find(&runtime);
    EXPECT_EQ(registry.Misses(), 2u) << "a hit is not a miss";
    EXPECT_EQ(registry.Retire(&runtime), CE_OK);
    EXPECT_EQ(registry.Retire(&runtime), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(registry.Misses(), 3u);
}

TEST(CengineApiRegistryTest, ConcurrentCreatesLetExactlyOneWin) {
    constexpr int kThreads = 8;
    for (int round = 0; round < 50; ++round) {
        Registry registry;
        std::vector<FakeRuntime> candidates(kThreads);
        std::atomic<int> winners{0};
        std::atomic<int> busy{0};
        std::latch start(kThreads);
        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i] {
                start.arrive_and_wait();
                auto claim = registry.TryClaim();
                if (claim) {
                    winners.fetch_add(1);
                    EXPECT_TRUE(claim.Publish(&candidates[i]));
                } else if (claim.status() == CE_E_BUSY) {
                    busy.fetch_add(1);
                }
            });
        }
        for (std::thread& thread : threads)
            thread.join();
        EXPECT_EQ(winners.load(), 1);
        EXPECT_EQ(busy.load(), kThreads - 1);
        EXPECT_EQ(registry.GetState(), Registry::State::Live);
    }
}

TEST(CengineApiRegistryTest, ProcessWideRegistryIsOneInstanceForTheRuntimeHandleType) {
    auto& registry = ce::capi::LiveRuntimes();
    EXPECT_EQ(&registry, &ce::capi::LiveRuntimes());
    // The shim's resolve helper: NULL and never-issued handles come back empty (and are logged, rate-limited).
    EXPECT_EQ(ce::capi::ResolveRuntime(nullptr, "test"), nullptr);
    EXPECT_EQ(registry.GetState(), ce::capi::HandleRegistry<ce_runtime>::State::Empty)
        << "no test may leave the global slot taken";
}

}  // namespace
