#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

#include "hook/streamline/streamline_takeover_decision.h"
#include "hook/streamline/streamline_takeover_publication.h"
#include "source_fragment_reader.h"

namespace {
using ce::streamline_interposer_takeover::RuntimePublication;
using ce::streamline_interposer_takeover::TakeoverDecision;
using ce::streamline_interposer_takeover::TakeoverRollbackRequest;

TEST(StreamlineTakeoverDecision, WorkerCannotSelectCustomBeforeAnEntryThunk) {
    TakeoverDecision decision;
    EXPECT_FALSE(decision.TryClaimCustom(false));
    EXPECT_FALSE(decision.IsCustom());
    EXPECT_FALSE(decision.IsResident());
    EXPECT_TRUE(decision.TryClaimCustom(true));
    EXPECT_TRUE(decision.TryClaimCustom(false));
    EXPECT_TRUE(decision.TryClaimCustom(true));
    EXPECT_FALSE(decision.TrySelectResident());
}

TEST(StreamlineTakeoverDecision, ResidentWinsBeforeEntryAndCustomLoaderIsNeverCalled) {
    TakeoverDecision decision;
    RuntimePublication<void*> publication;
    int module = 0;
    unsigned loads = 0;
    auto ensure = [&](bool entry) -> void* {
        if (!decision.TryClaimCustom(entry))
            return nullptr;
        return publication.LoadAndPublish([&]() -> void* {
            ++loads;
            return &module;
        }, [](void*) { return true; });
    };
    EXPECT_EQ(ensure(false), nullptr);
    EXPECT_TRUE(decision.TrySelectResident());
    EXPECT_FALSE(decision.TrySelectResident());
    EXPECT_EQ(ensure(true), nullptr);
    EXPECT_EQ(ensure(false), nullptr);
    EXPECT_EQ(loads, 0u);
    EXPECT_EQ(publication.Query(), nullptr);
    EXPECT_TRUE(decision.IsResident());
}

TEST(StreamlineTakeoverDecision, ResidentSelectionBlocksLateCustomLoadsBeforeWorkerClearsArmedState) {
    TakeoverDecision decision;
    EXPECT_FALSE(decision.AllowsCustomLoad(false, false));
    EXPECT_TRUE(decision.AllowsCustomLoad(true, false));
    EXPECT_TRUE(decision.AllowsCustomLoad(false, true));
    ASSERT_TRUE(decision.TrySelectResident());
    EXPECT_FALSE(decision.AllowsCustomLoad(true, false));
    EXPECT_FALSE(decision.AllowsCustomLoad(false, true));
    EXPECT_FALSE(decision.AllowsCustomLoad(true, true));

    TakeoverDecision custom;
    ASSERT_TRUE(custom.TryClaimCustom(true));
    EXPECT_TRUE(custom.AllowsCustomLoad(true, true));
    custom.SelectResidentAfterLoadFailure();
    EXPECT_FALSE(custom.AllowsCustomLoad(true, true));
}

TEST(StreamlineTakeoverDecision, PostInitCallCannotRollbackAnEntryBlockedInsideTheLoader) {
    TakeoverDecision decision;
    RuntimePublication<void*> publication;
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(entered, nullptr);
    ASSERT_NE(release, nullptr);
    int module = 0;
    std::atomic<unsigned> publications{0};
    std::thread entry([&] {
        EXPECT_TRUE(decision.TryClaimCustom(true));
        EXPECT_EQ(publication.LoadAndPublish([&]() -> void* {
            SetEvent(entered);
            if (WaitForSingleObject(release, 3000) != WAIT_OBJECT_0)
                return nullptr;
            return &module;
        }, [](void*) { return true; }, [&](void*) { ++publications; }), &module);
    });
    EXPECT_EQ(WaitForSingleObject(entered, 3000), WAIT_OBJECT_0);
    // This is the window in which the old dispatch rolled back and hooked the
    // resident, then published the custom runtime after the loader resumed.
    EXPECT_EQ(publication.Query(), nullptr);
    EXPECT_FALSE(decision.TrySelectResident());
    EXPECT_TRUE(decision.TryClaimCustom(false));
    SetEvent(release);
    entry.join();
    EXPECT_EQ(publication.Query(), &module);
    EXPECT_EQ(publications.load(), 1u);
    EXPECT_TRUE(decision.IsCustom());
    EXPECT_FALSE(decision.IsResident());
    EXPECT_TRUE(publication.ClaimBinding());
    EXPECT_FALSE(publication.ClaimBinding());
    CloseHandle(release);
    CloseHandle(entered);
}

TEST(StreamlineTakeoverDecision, FinalLoadFailureReturnsOwnershipToResidentWithoutRetryingCustom) {
    TakeoverDecision decision;
    RuntimePublication<void*> publication;
    ASSERT_TRUE(decision.TryClaimCustom(true));
    EXPECT_EQ(publication.LoadAndPublish([]() -> void* { return nullptr; }, [](void*) { return true; }), nullptr);
    decision.SelectResidentAfterLoadFailure();
    EXPECT_TRUE(decision.IsResident());
    EXPECT_FALSE(decision.TryClaimCustom(true));
    EXPECT_FALSE(decision.TryClaimCustom(false));
    EXPECT_EQ(publication.Query(), nullptr);
}

TEST(StreamlineTakeoverDecision, ResidentNotificationDoesNotWaitForHookInstallationOrRunCleanup) {
    TakeoverDecision decision;
    TakeoverRollbackRequest rollback;
    std::mutex hookMutex;
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(entered, nullptr);
    ASSERT_NE(release, nullptr);
    unsigned restores = 0;
    std::thread worker([&] {
        std::lock_guard<std::mutex> lock(hookMutex);
        SetEvent(entered);
        EXPECT_EQ(WaitForSingleObject(release, 3000), WAIT_OBJECT_0);
    });
    EXPECT_EQ(WaitForSingleObject(entered, 3000), WAIT_OBJECT_0);
    // Models the loader callback while a worker owns the hook-install lock.
    EXPECT_TRUE(decision.TrySelectResident());
    rollback.Request("Streamline core mapped before takeover entry");
    EXPECT_TRUE(decision.IsResident());
    EXPECT_FALSE(decision.TryClaimCustom(true));
    EXPECT_EQ(restores, 0u);
    SetEvent(release);
    worker.join();
    auto restore = [&](const char* reason) {
        std::lock_guard<std::mutex> lock(hookMutex);
        EXPECT_STREQ(reason, "Streamline core mapped before takeover entry");
        ++restores;
    };
    EXPECT_TRUE(rollback.Service(restore));
    EXPECT_FALSE(rollback.Service(restore));
    EXPECT_EQ(restores, 1u);
    CloseHandle(release);
    CloseHandle(entered);
}

TEST(StreamlineTakeoverDecision, CustomCoreNotificationCannotQueueResidentRestoration) {
    TakeoverDecision decision;
    TakeoverRollbackRequest rollback;
    ASSERT_TRUE(decision.TryClaimCustom(true));
    if (decision.TrySelectResident())
        rollback.Request("core mapped");
    EXPECT_FALSE(rollback.Service([](const char*) { ADD_FAILURE() << "Custom runtime must retain its hooks"; }));
    EXPECT_TRUE(decision.IsCustom());
}

TEST(StreamlineTakeoverDecision, ProductionClaimsOwnershipBeforeLoadingAndPeersJoinThatClaim) {
    const auto source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "hook/streamline/streamline_interposer_takeover.cpp");
    const auto ensure = source.find("bool EnsureCustomInterposerReady() {");
    const auto claim = source.find("g_TakeoverDecision.TryClaimCustom(tl_InEntryThunkActivation)", ensure);
    const auto load = source.find("g_RuntimePublication.LoadAndPublish(", ensure);
    ASSERT_NE(ensure, std::string::npos);
    ASSERT_NE(claim, std::string::npos);
    ASSERT_NE(load, std::string::npos);
    EXPECT_LT(claim, load);
    const auto abandon = source.find("void AbandonArmedTakeover(");
    const auto dispatch = source.find("uint64_t DispatchTakeoverSlot(");
    ASSERT_NE(abandon, std::string::npos);
    ASSERT_NE(dispatch, std::string::npos);
    EXPECT_LT(source.find("g_TakeoverDecision.TrySelectResident()", abandon), dispatch);
    EXPECT_NE(source.find("!g_TakeoverDecision.IsCustom()", dispatch), std::string::npos);
    EXPECT_NE(source.find("if (g_TakeoverDecision.IsCustom())", dispatch), std::string::npos);
}

TEST(StreamlineTakeoverDecision, ProductionRollbackNotificationOnlyQueuesWorkerCleanup) {
    const auto root = std::filesystem::current_path();
    const auto source = ce::test_source::ReadLogicalSource(root / "hook/streamline/streamline_interposer_takeover.cpp");
    const auto worker = ce::test_source::ReadLogicalSource(root / "hook/runtime/main_hookthread.cpp");
    const auto hooks = ce::test_source::ReadLogicalSource(root / "hook/streamline/streamline_hook.cpp");
    const auto begin = source.find("void AbandonArmedTakeover(");
    const auto end = source.find("uint64_t DispatchTakeoverSlot(", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const auto notification = source.substr(begin, end - begin);
    EXPECT_NE(notification.find("QueueResidentRollback(reason)"), std::string::npos);
    EXPECT_EQ(notification.find("CollectInterposerImportingModules"), std::string::npos);
    EXPECT_EQ(notification.find("RebindIATBaseline"), std::string::npos);
    EXPECT_EQ(notification.find("StreamlineHook::OnModuleLoaded"), std::string::npos);
    EXPECT_NE(source.find("g_ResidentRollback.Service(&FinishResidentRollback)"), std::string::npos);
    EXPECT_NE(source.find("&& !g_TakeoverDecision.IsResident()"), std::string::npos);
    const auto allowLoad = source.find("bool ShouldAllowTakeoverInterposerLoad(");
    const auto mapped = source.find("void NoteTakeoverInterposerMapped(", allowLoad);
    ASSERT_NE(allowLoad, std::string::npos);
    ASSERT_NE(mapped, std::string::npos);
    EXPECT_LT(source.find("g_TakeoverDecision.AllowsCustomLoad(", allowLoad), mapped);
    const auto loop = worker.find("while (true)", worker.find("// Monitor Loop"));
    ASSERT_NE(loop, std::string::npos);
    EXPECT_NE(worker.find("ServicePendingRollback()", loop), std::string::npos);
    const auto init = hooks.find("void Init() {");
    const auto service = hooks.find("ServicePendingRollback()", init);
    const auto lock = hooks.find("lock(streamline_hook_g_InitMutex)", init);
    ASSERT_NE(service, std::string::npos);
    ASSERT_NE(lock, std::string::npos);
    EXPECT_LT(service, lock);
}
}  // namespace
