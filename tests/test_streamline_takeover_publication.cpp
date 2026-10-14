#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <thread>

#include "hook/streamline/streamline_takeover_publication.h"
#include "source_fragment_reader.h"

namespace {
using ce::streamline_interposer_takeover::RuntimePublication;

TEST(StreamlineTakeoverPublication, QueryDoesNotWaitForBlockedLoaderOrExposeUnpreparedModule) {
    RuntimePublication<void*> publication;
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(entered, nullptr);
    ASSERT_NE(release, nullptr);
    int module = 0;
    std::atomic<bool> prepared{false};
    std::thread worker([&] {
        EXPECT_EQ(publication.LoadAndPublish([&]() -> void* {
            SetEvent(entered);
            // Models a worker blocked behind a DLL callback's loader lock.
            if (WaitForSingleObject(release, 3000) != WAIT_OBJECT_0)
                return nullptr;
            return &module;
        }, [&](void*) {
            EXPECT_EQ(publication.Query(), nullptr);
            prepared.store(true, std::memory_order_release);
            return true;
        }), &module);
    });
    EXPECT_EQ(WaitForSingleObject(entered, 3000), WAIT_OBJECT_0);
    // The DLL callback must be able to query before releasing the worker.
    EXPECT_EQ(publication.Query(), nullptr);
    EXPECT_FALSE(prepared.load(std::memory_order_acquire));
    SetEvent(release);
    worker.join();
    EXPECT_EQ(publication.Query(), &module);
    EXPECT_TRUE(prepared.load(std::memory_order_acquire));
    CloseHandle(release);
    CloseHandle(entered);
}

TEST(StreamlineTakeoverPublication, ConcurrentCallersLoadWithoutApplicationLockAndBindOnce) {
    RuntimePublication<void*> publication;
    HANDLE entered[] = {CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    ASSERT_NE(entered[0], nullptr);
    ASSERT_NE(entered[1], nullptr);
    int module = 0;
    std::atomic<int> preparations{0};
    std::atomic<int> publications{0};
    std::atomic<int> bindings{0};
    auto run = [&](int index) {
        EXPECT_EQ(publication.LoadAndPublish([&]() -> void* {
            SetEvent(entered[index]);
            // Both callers must reach the OS loader, which owns serialization.
            // An application lock across load/prepare makes this rendezvous fail.
            if (WaitForMultipleObjects(2, entered, TRUE, 3000) != WAIT_OBJECT_0)
                return nullptr;
            return &module;
        }, [&](void*) {
            preparations.fetch_add(1, std::memory_order_relaxed);
            return true;
        }, [&](void*) {
            publications.fetch_add(1, std::memory_order_relaxed);
        }), &module);
        if (publication.ClaimBinding())
            bindings.fetch_add(1, std::memory_order_relaxed);
    };
    std::thread first(run, 0);
    std::thread second(run, 1);
    first.join();
    second.join();
    EXPECT_EQ(preparations.load(), 2);
    EXPECT_EQ(publications.load(), 1);
    EXPECT_EQ(bindings.load(), 1);
    EXPECT_EQ(publication.Query(), &module);
    EXPECT_EQ(publication.LoadAndPublish([]() -> void* {
        ADD_FAILURE() << "An already published module must not be loaded again";
        return nullptr;
    }, [](void*) { return false; }), &module);
    CloseHandle(entered[1]);
    CloseHandle(entered[0]);
}

TEST(StreamlineTakeoverPublication, FailedLoadOrPreparationIsNeverPublished) {
    RuntimePublication<void*> publication;
    int module = 0;
    EXPECT_EQ(publication.LoadAndPublish([]() -> void* { return nullptr; }, [](void*) {
        ADD_FAILURE() << "A failed load must not be prepared";
        return true;
    }), nullptr);
    EXPECT_EQ(publication.LoadAndPublish([&]() -> void* { return &module; }, [](void*) { return false; }), nullptr);
    EXPECT_EQ(publication.Query(), nullptr);
}

TEST(StreamlineTakeoverPublication, CompetingFailureCannotOverwritePublishedSuccess) {
    RuntimePublication<void*> publication;
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(entered, nullptr);
    ASSERT_NE(release, nullptr);
    int module = 0;
    std::thread failing([&] {
        EXPECT_EQ(publication.LoadAndPublish([&]() -> void* {
            SetEvent(entered);
            EXPECT_EQ(WaitForSingleObject(release, 3000), WAIT_OBJECT_0);
            return nullptr;
        }, [](void*) { return true; }), &module);
    });
    EXPECT_EQ(WaitForSingleObject(entered, 3000), WAIT_OBJECT_0);
    EXPECT_EQ(publication.LoadAndPublish([&]() -> void* { return &module; }, [](void*) { return true; }), &module);
    SetEvent(release);
    failing.join();
    EXPECT_EQ(publication.Query(), &module);
    CloseHandle(release);
    CloseHandle(entered);
}

TEST(StreamlineTakeoverPublication, FinalFailureCannotBeOverwrittenByCompetingPreparation) {
    RuntimePublication<void*> publication;
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(entered, nullptr);
    ASSERT_NE(release, nullptr);
    int module = 0;
    std::atomic<bool> published{false};
    std::thread preparing([&] {
        EXPECT_EQ(publication.LoadAndPublish([&]() -> void* { return &module; }, [&](void*) {
            SetEvent(entered);
            EXPECT_EQ(WaitForSingleObject(release, 3000), WAIT_OBJECT_0);
            return true;
        }, [&](void*) { published.store(true); }), nullptr);
    });
    EXPECT_EQ(WaitForSingleObject(entered, 3000), WAIT_OBJECT_0);
    EXPECT_EQ(publication.LoadAndPublish([]() -> void* { return nullptr; }, [](void*) { return true; }), nullptr);
    SetEvent(release);
    preparing.join();
    EXPECT_EQ(publication.Query(), nullptr);
    EXPECT_FALSE(published.load());
    CloseHandle(release);
    CloseHandle(entered);
}

TEST(StreamlineTakeoverPublication, BindingMustFinishBeforeEntryCallersSeeTheOverride) {
    RuntimePublication<void*> publication;
    HANDLE binding = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE peerPrepared = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(binding, nullptr);
    ASSERT_NE(peerPrepared, nullptr);
    ASSERT_NE(release, nullptr);
    int module = 0;
    std::atomic<int> bindings{0};
    std::atomic<int> publications{0};
    auto bind = [&](void*) {
        ++bindings;
        SetEvent(binding);
        EXPECT_EQ(publication.Query(), nullptr);
        return WaitForSingleObject(release, 3000) == WAIT_OBJECT_0;
    };
    auto publish = [&](void*) { ++publications; };
    std::thread first([&] {
        EXPECT_EQ(publication.LoadAndPublish([&]() -> void* { return &module; },
                                             [](void*) { return true; }, bind, publish), &module);
    });
    EXPECT_EQ(WaitForSingleObject(binding, 3000), WAIT_OBJECT_0);
    std::thread peer([&] {
        EXPECT_EQ(publication.LoadAndPublish([&]() -> void* { return &module; }, [&](void*) {
            SetEvent(peerPrepared);
            return true;
        }, bind, publish), &module);
    });
    EXPECT_EQ(WaitForSingleObject(peerPrepared, 3000), WAIT_OBJECT_0);
    EXPECT_EQ(publication.Query(), nullptr);
    EXPECT_EQ(publications.load(), 0);
    SetEvent(release);
    first.join();
    peer.join();
    EXPECT_EQ(bindings.load(), 1);
    EXPECT_EQ(publications.load(), 1);
    EXPECT_EQ(publication.Query(), &module);
    CloseHandle(release);
    CloseHandle(peerPrepared);
    CloseHandle(binding);
}

TEST(StreamlineTakeoverPublication, BindingFailureCannotPublishAnyOverrideTargets) {
    RuntimePublication<void*> publication;
    int module = 0;
    bool published = false;
    EXPECT_EQ(publication.LoadAndPublish([&]() -> void* { return &module; }, [](void*) { return true; },
                                         [](void*) { return false; }, [&](void*) { published = true; }), nullptr);
    EXPECT_EQ(publication.Query(), nullptr);
    EXPECT_FALSE(published);
}

TEST(StreamlineTakeoverPublication, ProductionQueriesNeverLoadAndDynamicExportsKeepTheirGate) {
    const auto root = std::filesystem::current_path();
    const auto source = ce::test_source::ReadLogicalSource(root / "hook/streamline/streamline_interposer_takeover.cpp");
    const auto hooks = ce::test_source::ReadLogicalSource(root / "hook/hooking/iat_hook_init.cpp");
    const auto queryBegin = source.find("HMODULE ResolveCustomInterposerForModuleQuery() {");
    const auto ensureBegin = source.find("bool EnsureCustomInterposerReady() {");
    ASSERT_NE(queryBegin, std::string::npos);
    ASSERT_NE(ensureBegin, std::string::npos);
    const auto query = source.substr(queryBegin, source.find("\n}  // namespace", queryBegin) - queryBegin);
    const auto ensure = source.substr(ensureBegin, source.find("\nbool IsTakeoverArmed()", ensureBegin) - ensureBegin);
    ASSERT_FALSE(query.empty());
    ASSERT_FALSE(ensure.empty());
    EXPECT_EQ(query.find("EnsureCustomInterposerReady"), std::string::npos);
    EXPECT_NE(query.find("g_RuntimePublication.Query()"), std::string::npos);
    EXPECT_EQ(ensure.find("g_TakeoverMutex"), std::string::npos);
    EXPECT_NE(ensure.find("g_RuntimePublication.LoadAndPublish("), std::string::npos);
    const auto nativeLookup = hooks.find("FARPROC proc = ::GetProcAddress(hModule, lpProcName)");
    const auto dormantReturn = hooks.find("if (HookIsShuttingDown())", nativeLookup);
    const auto takeoverLookup = hooks.find("ResolvePendingTakeoverExport(hModule, lpProcName)");
    ASSERT_NE(nativeLookup, std::string::npos);
    ASSERT_NE(dormantReturn, std::string::npos);
    ASSERT_NE(takeoverLookup, std::string::npos);
    EXPECT_LT(nativeLookup, dormantReturn);
    EXPECT_LT(dormantReturn, takeoverLookup);
    EXPECT_LT(takeoverLookup, hooks.find("g_DynamicHookLock", dormantReturn));
    EXPECT_NE(source.find("reinterpret_cast<uintptr_t>(name) < 0x10000"), std::string::npos);
}
}  // namespace
