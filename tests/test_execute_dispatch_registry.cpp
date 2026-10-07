#include <gtest/gtest.h>
#include <condition_variable>
#include <thread>

#include "hook/d3d12/execute_dispatch_registry.h"

namespace {
using Target = void (*)();
using Registry = ce::dx12::ExecuteDispatchRegistry<Target>;
void First() {}
void Second() {}
void Detour() {}
void Follower() {}

TEST(ExecuteDispatchRegistryTest, UntrackedReceiversUseTheirOwnLiveSlotsWithoutCachingThem) {
    Registry registry;
    void* table[19]{};
    EXPECT_EQ(registry.Resolve(table, First, Detour), First);
    EXPECT_EQ(registry.Resolve(table, Second, Detour), Second);
    EXPECT_EQ(registry.Resolve(table, Detour, Detour), nullptr);
    EXPECT_EQ(registry.Resolve(nullptr, First, Detour), nullptr);
}

TEST(ExecuteDispatchRegistryTest, InterceptionRecoveryPrecedesForeignLiveEntryAfterReset) {
    Registry registry;
    void* table[19]{};
    registry.Install(table, First, Detour, [&](Target* original) {
        *original = First;
        return true;
    });
    registry.Reset();
    int liveReads = 0;
    EXPECT_EQ(registry.ResolveInterception(
                  table, Detour, [&] { return First; },
                  [&] {
                      ++liveReads;
                      return Follower;
                  }),
              First);
    EXPECT_EQ(liveReads, 0) << "the foreign slot can still forward to CE";
}

TEST(ExecuteDispatchRegistryTest, OwnedBindingsDoNotConsultColdRecoveryOrLiveSlots) {
    Registry registry;
    void* table[19]{};
    registry.Install(table, First, Detour, [&](Target* original) {
        *original = First;
        return true;
    });
    int coldReads = 0;
    for (int call = 0; call < 8; ++call) {
        EXPECT_EQ(registry.ResolveInterception(
                      table, Detour,
                      [&] {
                          ++coldReads;
                          return Second;
                      },
                      [&] {
                          ++coldReads;
                          return Follower;
                      }),
                  First);
    }
    EXPECT_EQ(coldReads, 0);
}

TEST(ExecuteDispatchRegistryTest, SavedOriginalIsVisibleDuringPublicationReentry) {
    Registry registry;
    void* table[19]{};
    auto result = registry.Install(table, First, Detour, [&](Target* original) {
        *original = First;  // The primitive supplies this before publishing the detour.
        EXPECT_EQ(registry.Resolve(table, Detour, Detour), First);
        return true;
    });
    EXPECT_EQ(result.result, Registry::InstallResult::kCaptured);
    EXPECT_EQ(registry.Resolve(table, Detour, Detour), First);
}

TEST(ExecuteDispatchRegistryTest, DuplicateAndForeignFollowersPreserveTheEstablishedChain) {
    Registry registry;
    void* table[19]{};
    int patches = 0;
    auto patch = [&](Target* original) {
        ++patches;
        *original = First;
        return true;
    };
    ASSERT_EQ(registry.Install(table, First, Detour, patch).result, Registry::InstallResult::kCaptured);
    EXPECT_EQ(registry.Install(table, Detour, Detour, patch).result, Registry::InstallResult::kKnown);
    EXPECT_EQ(registry.Install(table, Follower, Detour, patch).result, Registry::InstallResult::kFollower);
    EXPECT_EQ(registry.Resolve(table, Follower, Detour), First);
    EXPECT_EQ(patches, 1);
    // A removed follower restored the true predecessor: reclaiming it cannot create CE -> follower -> CE.
    EXPECT_EQ(registry.Install(table, First, Detour, patch).result, Registry::InstallResult::kCaptured);
    EXPECT_EQ(patches, 2);
}

TEST(ExecuteDispatchRegistryTest, ResetInvalidatesCachedPairsBeforeIdentityReuse) {
    Registry registry;
    void* table[19]{};
    auto first = [&](Target* original) {
        *original = First;
        return true;
    };
    auto second = [&](Target* original) {
        *original = Second;
        return true;
    };
    registry.Install(table, First, Detour, first);
    ASSERT_EQ(registry.Resolve(table, Detour, Detour), First);
    registry.Reset();
    EXPECT_EQ(registry.Resolve(table, Detour, Detour), nullptr);
    registry.Install(table, Second, Detour, second);
    EXPECT_EQ(registry.Resolve(table, Detour, Detour), Second);
}

TEST(ExecuteDispatchRegistryTest, ReinstallationCannotUseAnOldCacheDuringPublication) {
    Registry registry;
    void* table[19]{};
    registry.Install(table, First, Detour, [&](Target* original) {
        *original = First;
        return true;
    });
    ASSERT_EQ(registry.Resolve(table, Detour, Detour), First);
    registry.Install(table, First, Detour, [&](Target* original) {
        // The primitive observed a replacement after the initial slot read but before patching.
        *original = Second;
        EXPECT_EQ(registry.Resolve(table, Detour, Detour), Second);
        return true;
    });
    EXPECT_EQ(registry.Resolve(table, Detour, Detour), Second);
}

TEST(ExecuteDispatchRegistryTest, FailedCaptureDoesNotPublishPendingOrCachedEvidence) {
    Registry registry;
    void* table[19]{};
    auto result = registry.Install(table, First, Detour, [&](Target* original) {
        *original = First;
        EXPECT_EQ(registry.Resolve(table, Detour, Detour), First);
        return false;  // A controlled failed patch can already have supplied an output pointer.
    });
    EXPECT_EQ(result.result, Registry::InstallResult::kFailed);
    EXPECT_FALSE(registry.HasBinding(table));
    EXPECT_EQ(registry.Resolve(table, Detour, Detour), nullptr);
    EXPECT_EQ(registry.Resolve(table, Second, Detour), Second);
}

TEST(ExecuteDispatchRegistryTest, ReentrantRetirementCannotResurrectAnErasedInstallation) {
    Registry registry;
    void* table[19]{};
    auto result = registry.Install(table, First, Detour, [&](Target* original) {
        *original = First;
        EXPECT_EQ(registry.Resolve(table, Detour, Detour), First);
        registry.Reset();
        return true;
    });
    EXPECT_EQ(result.result, Registry::InstallResult::kRetired);
    EXPECT_FALSE(registry.HasBinding(table));
    EXPECT_EQ(registry.Resolve(table, Detour, Detour), nullptr);
}

TEST(ExecuteDispatchRegistryTest, NestedIndependentCaptureDoesNotCancelTheOuterBinding) {
    Registry registry;
    void* first[19]{};
    void* second[19]{};
    auto result = registry.Install(first, First, Detour, [&](Target* original) {
        *original = First;
        registry.Install(second, Second, Detour, [&](Target* nested) {
            *nested = Second;
            return true;
        });
        return true;
    });
    EXPECT_EQ(result.result, Registry::InstallResult::kCaptured);
    EXPECT_EQ(registry.Resolve(first, Detour, Detour), First);
    EXPECT_EQ(registry.Resolve(second, Detour, Detour), Second);
}

TEST(ExecuteDispatchRegistryTest, AnotherThreadsCachedBindingIsInvalidatedByResetAndReuse) {
    Registry registry;
    void* table[19]{};
    registry.Install(table, First, Detour, [&](Target* original) {
        *original = First;
        return true;
    });
    std::mutex mutex;
    std::condition_variable changed;
    bool cached = false;
    bool rebound = false;
    Target before = nullptr;
    Target after = nullptr;
    std::thread reader([&] {
        before = registry.Resolve(table, Detour, Detour);
        std::unique_lock<std::mutex> lock(mutex);
        cached = true;
        changed.notify_all();
        changed.wait(lock, [&] { return rebound; });
        lock.unlock();
        after = registry.Resolve(table, Detour, Detour);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return cached; });
    }
    registry.Reset();
    registry.Install(table, Second, Detour, [&](Target* original) {
        *original = Second;
        return true;
    });
    {
        std::lock_guard<std::mutex> lock(mutex);
        rebound = true;
    }
    changed.notify_all();
    reader.join();
    EXPECT_EQ(before, First);
    EXPECT_EQ(after, Second);
}

}  // namespace
