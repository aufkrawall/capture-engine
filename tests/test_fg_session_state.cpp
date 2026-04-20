#include <gtest/gtest.h>

#include "../hook/common/fg_detection.h"
#include "../hook/common/fg_session_state.h"

namespace {

ce::fg_session::DX12LegacyStateView g_TestLegacyState;

void FillTestLegacyState(ce::fg_session::DX12LegacyStateView* out) {
    if (!out) {
        return;
    }
    *out = g_TestLegacyState;
}

}  // namespace

TEST(FGSessionStateTest, NativeFSRCallbackPathBuildsRuntimeOwnedPlan) {
    ce::fg_session::ResetFGSessionStateForTests();
    g_TestLegacyState = {};
    g_TestLegacyState.runtimeOwnsSwapchain = true;
    g_TestLegacyState.hadFSRPhase = true;
    g_TestLegacyState.swapchainQueue = reinterpret_cast<ID3D12CommandQueue*>(0x1010);

    ce::fg_session::RegisterDX12LegacyStateProvider(&FillTestLegacyState);
    g_FGCompat.SetFSRFGSupportPresent(true);
    g_FGCompat.SetFSRFGActive(true);
    g_FGCompat.SetFSRFGMultiplier(2);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kNativeFSRConfigureOn, "test-native-fsr-on");

    const auto snapshot = ce::fg_session::GetLatestFGSessionSnapshot();
    const auto plan = ce::fg_session::GetLatestFGActionPlan();

    EXPECT_EQ(snapshot.overlayMode, ce::fg_session::FGOverlayBackendMode::kRuntimeOwnedFSRCallback);
    EXPECT_EQ(plan.backendMode, ce::fg_session::FGOverlayBackendMode::kRuntimeOwnedFSRCallback);
    EXPECT_EQ(plan.selectedQueueRole, ce::fg_session::FGQueueRole::kFFXCallbackQueue);
    EXPECT_EQ(plan.selectedQueue, g_TestLegacyState.swapchainQueue);
}

TEST(FGSessionStateTest, PostFSRDLSSPrefersRealQueueBehindWrapperOverWrapperBootstrap) {
    ce::fg_session::ResetFGSessionStateForTests();
    g_TestLegacyState = {};
    g_TestLegacyState.hadFSRPhase = true;
    g_TestLegacyState.safePostFSRBootstrapPath = true;
    g_TestLegacyState.explicitSetOptionsActivationForCurrentComeback = true;
    g_TestLegacyState.postSLCallbackInstalled = true;
    g_TestLegacyState.realQueueBehindWrapper = reinterpret_cast<ID3D12CommandQueue*>(0x2222);
    g_TestLegacyState.slWrapperQueue = reinterpret_cast<ID3D12CommandQueue*>(0x3333);
    g_TestLegacyState.swapchainQueue = reinterpret_cast<ID3D12CommandQueue*>(0x4444);

    ce::fg_session::RegisterDX12LegacyStateProvider(&FillTestLegacyState);
    g_FGCompat.SetStreamlineSupportPresent(true);
    g_FGCompat.SetStreamlineFGSignal(true);
    g_FGCompat.SetDLSSFGActive(true);
    g_FGCompat.SetDLSSFGMultiplier(2);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate,
                                "test-dlss-setoptions");

    const auto plan = ce::fg_session::GetLatestFGActionPlan();
    EXPECT_EQ(plan.selectedQueueRole, ce::fg_session::FGQueueRole::kRealBehindWrapper);
    EXPECT_EQ(plan.selectedQueue, g_TestLegacyState.realQueueBehindWrapper);
    EXPECT_STREQ(plan.reason, "real-queue-behind-wrapper");
}

TEST(FGSessionStateTest, PostFSRDLSSPrefersSwapchainQueueOverWrapperBootstrapOnceBootstrapIsSafe) {
    ce::fg_session::ResetFGSessionStateForTests();
    g_TestLegacyState = {};
    g_TestLegacyState.hadFSRPhase = true;
    g_TestLegacyState.safePostFSRBootstrapPath = true;
    g_TestLegacyState.explicitSetOptionsActivationForCurrentComeback = false;
    g_TestLegacyState.postSLCallbackInstalled = true;
    g_TestLegacyState.slWrapperQueue = reinterpret_cast<ID3D12CommandQueue*>(0x3333);
    g_TestLegacyState.swapchainQueue = reinterpret_cast<ID3D12CommandQueue*>(0x4444);

    ce::fg_session::RegisterDX12LegacyStateProvider(&FillTestLegacyState);
    g_FGCompat.SetStreamlineSupportPresent(true);
    g_FGCompat.SetStreamlineFGSignal(true);
    g_FGCompat.SetDLSSFGActive(true);
    g_FGCompat.SetDLSSFGMultiplier(2);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kStreamlineGetStateRuntimeUpdate, "test-dlss-getstate");

    const auto plan = ce::fg_session::GetLatestFGActionPlan();
    EXPECT_EQ(plan.selectedQueueRole, ce::fg_session::FGQueueRole::kSwapchain);
    EXPECT_EQ(plan.selectedQueue, g_TestLegacyState.swapchainQueue);
    EXPECT_STREQ(plan.reason, "swapchain-queue");
}

TEST(FGSessionStateTest, ValidationRejectsConfirmedPostSLWithoutActivePostSL) {
    ce::fg_session::FGSessionSnapshot snapshot;
    snapshot.postSLConfirmedRendering = true;
    snapshot.postSLActive = false;
    EXPECT_FALSE(ce::fg_session::ValidateFGSessionSnapshot(snapshot, nullptr));
}
