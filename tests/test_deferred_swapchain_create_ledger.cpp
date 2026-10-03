#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/present/deferred_swapchain_create_ledger.h"

#include "source_fragment_reader.h"

namespace {

namespace deferred = ce::deferred_swapchain_create;

struct TestEvidence {
    int caller = 0;
};

using TestLedger = deferred::Ledger<TestEvidence, 2>;

// Distinct fake identities; the ledger only compares and hands back pointers.
int g_swapchainA, g_swapchainB, g_swapchainC;
int g_queueA, g_queueB, g_queueC, g_queueA2;
int g_windowGame, g_windowHelper;

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / relativePath);
}

}  // namespace

// Talos Reawakened logs/20260927_034946: the game swapchain was created on a hidden window and
// CE dropped its queue capture; parking it keeps the queue for the first visible Present.
TEST(DeferredSwapchainCreateLedgerTest, AParkedCreateIsTakenOnceWithItsQueueAndEvidence) {
    TestLedger ledger;
    EXPECT_EQ(ledger.Park(&g_swapchainA, &g_queueA, &g_windowGame, TestEvidence{7}), nullptr);
    EXPECT_EQ(ledger.Count(), 1u);

    TestLedger::Entry entry;
    ASSERT_TRUE(ledger.Take(&g_swapchainA, &entry));
    EXPECT_EQ(entry.swapchain, &g_swapchainA);
    EXPECT_EQ(entry.queue, &g_queueA);
    EXPECT_EQ(entry.window, &g_windowGame);
    EXPECT_EQ(entry.evidence.caller, 7);
    EXPECT_EQ(ledger.Count(), 0u);
    EXPECT_FALSE(ledger.Take(&g_swapchainA, &entry)) << "a create is replayed at most once";
}

TEST(DeferredSwapchainCreateLedgerTest, UnknownAndNullSwapchainsAreNeverTaken) {
    TestLedger ledger;
    ledger.Park(&g_swapchainA, &g_queueA, &g_windowGame, {});
    TestLedger::Entry entry;
    EXPECT_FALSE(ledger.Take(&g_swapchainB, &entry));
    EXPECT_FALSE(ledger.Take(nullptr, &entry));
    EXPECT_EQ(ledger.Count(), 1u);
}

TEST(DeferredSwapchainCreateLedgerTest, ReparkingTheSameSwapchainHandsBackTheOldQueue) {
    TestLedger ledger;
    ledger.Park(&g_swapchainA, &g_queueA, &g_windowGame, {});
    EXPECT_EQ(ledger.Park(&g_swapchainA, &g_queueA2, &g_windowGame, {}), &g_queueA)
        << "the displaced queue reference must reach the caller to be released";
    EXPECT_EQ(ledger.Count(), 1u);
    TestLedger::Entry entry;
    ASSERT_TRUE(ledger.Take(&g_swapchainA, &entry));
    EXPECT_EQ(entry.queue, &g_queueA2);
}

TEST(DeferredSwapchainCreateLedgerTest, AFullLedgerEvictsTheOldestRecord) {
    TestLedger ledger;
    ledger.Park(&g_swapchainA, &g_queueA, &g_windowHelper, {});
    ledger.Park(&g_swapchainB, &g_queueB, &g_windowHelper, {});
    EXPECT_EQ(ledger.Park(&g_swapchainC, &g_queueC, &g_windowGame, {}), &g_queueA);
    EXPECT_EQ(ledger.Count(), 2u);
    TestLedger::Entry entry;
    EXPECT_FALSE(ledger.Take(&g_swapchainA, &entry));
    EXPECT_TRUE(ledger.Take(&g_swapchainB, &entry));
    EXPECT_TRUE(ledger.Take(&g_swapchainC, &entry));
}

TEST(DeferredSwapchainCreateLedgerTest, AVisibleCreateSupersedesOnlyItsOwnWindow) {
    TestLedger ledger;
    ledger.Park(&g_swapchainA, &g_queueA, &g_windowGame, {});
    ledger.Park(&g_swapchainB, &g_queueB, &g_windowHelper, {});
    void* released[2] = {};
    ASSERT_EQ(ledger.Forget(&g_windowGame, released), 1u);
    EXPECT_EQ(released[0], &g_queueA);
    EXPECT_EQ(ledger.Count(), 1u);
    TestLedger::Entry entry;
    EXPECT_FALSE(ledger.Take(&g_swapchainA, &entry));
    EXPECT_TRUE(ledger.Take(&g_swapchainB, &entry));
}

TEST(DeferredSwapchainCreateLedgerTest, ForgettingEverythingHandsBackEveryQueue) {
    TestLedger ledger;
    ledger.Park(&g_swapchainA, &g_queueA, &g_windowGame, {});
    ledger.Park(&g_swapchainB, &g_queueB, &g_windowHelper, {});
    void* released[2] = {};
    EXPECT_EQ(ledger.Forget(nullptr, released), 2u);
    EXPECT_EQ(ledger.Count(), 0u);
    EXPECT_TRUE((released[0] == &g_queueA && released[1] == &g_queueB) ||
                (released[0] == &g_queueB && released[1] == &g_queueA));
}

// A helper swapchain whose window is destroyed never presents visibly; its parked queue
// reference must not keep the queue (and the device behind it) alive.
TEST(DeferredSwapchainCreateLedgerTest, RecordsOfDestroyedWindowsAreReleasedAndOthersKept) {
    TestLedger ledger;
    ledger.Park(&g_swapchainA, &g_queueA, &g_windowGame, {});
    ledger.Park(&g_swapchainB, &g_queueB, &g_windowHelper, {});
    void* released[2] = {};
    const size_t count = ledger.ForgetWhereWindowGone(
        [](const void* window) { return window == &g_windowHelper; }, released);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(released[0], &g_queueB);
    EXPECT_EQ(ledger.Count(), 1u);
    TestLedger::Entry entry;
    EXPECT_FALSE(ledger.Take(&g_swapchainB, &entry));
    EXPECT_TRUE(ledger.Take(&g_swapchainA, &entry));
    EXPECT_EQ(entry.queue, &g_queueA);
}

TEST(DeferredSwapchainCreateLedgerTest, NothingIsReleasedWhileEveryWindowLives) {
    TestLedger ledger;
    ledger.Park(&g_swapchainA, &g_queueA, &g_windowGame, {});
    void* released[2] = {};
    EXPECT_EQ(ledger.ForgetWhereWindowGone([](const void*) { return false; }, released), 0u);
    EXPECT_EQ(ledger.Count(), 1u);
}

TEST(DeferredSwapchainCreateLedgerTest, TheGlueSweepsDeadWindowsWhenParkingAndWhileNothingMatches) {
    const std::string glue = ReadSource("hook/d3d12/dx12_hook_deferred_swapchain_create.cpp");
    const size_t park = glue.find("void ParkInvisibleWindowCreateSwapchain(");
    const size_t promote = glue.find("void PromoteParkedCreateSwapchainOnVisiblePresent(");
    ASSERT_NE(park, std::string::npos);
    ASSERT_NE(promote, std::string::npos);
    const size_t parkSweep = glue.find("ForgetWhereWindowGone(", park);
    const size_t parkCall = glue.find("g_ParkedCreates.Park(", park);
    ASSERT_NE(parkSweep, std::string::npos);
    ASSERT_NE(parkCall, std::string::npos);
    EXPECT_LT(parkSweep, parkCall) << "dead records leave before a new one takes a slot";
    EXPECT_NE(glue.find("ForgetWhereWindowGone(", promote), std::string::npos)
        << "records must also be released when no new swapchain is ever parked";
}

TEST(DeferredSwapchainCreateLedgerTest, PromotionRequiresTheSwapchainStillOnItsCreateQueue) {
    using deferred::PromotionDecision;
    EXPECT_EQ(deferred::DecidePromotion(false, true, true), PromotionDecision::kNotParked);
    EXPECT_EQ(deferred::DecidePromotion(true, true, true), PromotionDecision::kPromote);
    EXPECT_EQ(deferred::DecidePromotion(true, false, false), PromotionDecision::kPromote)
        << "an unreadable queue is no evidence against the parked identity";
    EXPECT_EQ(deferred::DecidePromotion(true, true, false), PromotionDecision::kDiscardStaleIdentity);
    EXPECT_STREQ(deferred::PromotionDecisionName(PromotionDecision::kPromote), "promote");
}

// Every create path that bypasses a hidden-window swapchain must hand its queue and evidence
// to the ledger; dropping them is what sent the first overlay frame to the render queue.
TEST(DeferredSwapchainCreateLedgerTest, EveryHiddenWindowCreateBypassParksItsQueue) {
    const std::string bypass = ReadSource("hook/d3d12/dx12_hook_ffx_startup.cpp");
    const size_t fn = bypass.find("bool ShouldBypassInvisibleWindowCreateSwapchainSideEffects(");
    ASSERT_NE(fn, std::string::npos);
    const size_t policy =
        bypass.find("ShouldSkipDX12CreateSwapchainSideEffectsForInvisibleWindowSwapchain(", fn);
    const size_t forget = bypass.find("ForgetParkedCreateSwapchainsForWindow(hWnd", fn);
    const size_t park = bypass.find("ParkInvisibleWindowCreateSwapchain(swapchain, hWnd, createDevice", fn);
    const size_t bypassed = bypass.find("return true;", fn);
    ASSERT_NE(policy, std::string::npos);
    ASSERT_NE(forget, std::string::npos);
    ASSERT_NE(park, std::string::npos);
    EXPECT_LT(policy, forget);
    EXPECT_LT(forget, park) << "the visible branch supersedes, the hidden branch parks";
    EXPECT_LT(park, bypassed) << "a bypassed create must park before it returns";

    const std::string create = ReadSource("hook/d3d12/dx12_hook_swapchain_create.cpp");
    const std::string tracking = ReadSource("hook/d3d12/dx12_hook_swapchain_tracking.cpp");
    size_t callSites = 0;
    for (const std::string* source : {&create, &tracking}) {
        for (size_t at = source->find("ShouldBypassInvisibleWindowCreateSwapchainSideEffects(");
             at != std::string::npos;
             at = source->find("ShouldBypassInvisibleWindowCreateSwapchainSideEffects(", at + 1)) {
            const size_t close = source->find(")) {", at);
            ASSERT_NE(close, std::string::npos);
            const std::string call = source->substr(at, close - at);
            EXPECT_NE(call.find("pDevice"), std::string::npos) << call;
            EXPECT_NE(call.find("captureEvidence"), std::string::npos) << call;
            ++callSites;
        }
    }
    EXPECT_EQ(callSites, 4u) << "INLINE, global CreateSwapChain, global ForHwnd and the deep hook";
}

TEST(DeferredSwapchainCreateLedgerTest, TheFirstVisiblePresentPromotesBeforeAQueueIsChosen) {
    const std::string phase1 = ReadSource("hook/d3d12/dx12_hook_process_session_stage1_prepare_frame.cpp");
    const size_t invisibleSkip = phase1.find("ShouldSkipDX12PresentProcessingForInvisibleWindowSwapchain(");
    const size_t promote = phase1.find("PromoteParkedCreateSwapchainOnVisiblePresent(pSwapChain)");
    ASSERT_NE(invisibleSkip, std::string::npos);
    ASSERT_NE(promote, std::string::npos);
    EXPECT_LT(invisibleSkip, promote) << "only a Present whose window is visible may promote";

    const std::string phase2 = ReadSource("hook/d3d12/dx12_hook_process_session_stage2_swapchain_queue.cpp");
    EXPECT_EQ(phase2.find("PromoteParkedCreateSwapchainOnVisiblePresent"), std::string::npos)
        << "TrackSwapchainAndSelectQueue chooses the overlay queue; promotion belongs in PrepareFrame, ahead of it";

    const std::string glue = ReadSource("hook/d3d12/dx12_hook_deferred_swapchain_create.cpp");
    const size_t fn = glue.find("void PromoteParkedCreateSwapchainOnVisiblePresent(");
    ASSERT_NE(fn, std::string::npos);
    const size_t protectedFFX = glue.find("HandleProtectedOfficialFFXStartupSwapchainCreate(", fn);
    const size_t capture = glue.find("CaptureSwapchainQueueFromCreateDevice(parkedQueue, swapchain", fn);
    ASSERT_NE(protectedFFX, std::string::npos);
    ASSERT_NE(capture, std::string::npos);
    EXPECT_LT(protectedFFX, capture) << "replay in the visible create path's order";

    const std::string shutdown = ReadSource("hook/d3d12/dx12_hook_main.cpp");
    EXPECT_NE(shutdown.find("ReleaseParkedCreateSwapchains("), std::string::npos)
        << "parked queue references must not outlive the DX12 hook";
}
