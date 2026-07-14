#include <gtest/gtest.h>

#include "../hook/common/dx12_overlay_policy.h"

namespace {

using ce::dx12_overlay_policy::ChooseFFXUiCompositeDriver;
using ce::dx12_overlay_policy::FFXUiCompositeDriver;
using ce::dx12_overlay_policy::MayReassertSubstituteUiResource;
using ce::dx12_overlay_policy::NativeFSROwnerQueueRoute;
using ce::dx12_overlay_policy::ShouldInstallFFXProxyPresentHook;
using ce::dx12_overlay_policy::ShouldRecoverNativeFSRProxyBindingFromProtectedCreate;

// Regression for session 20260701_213656: GTA froze PERMANENTLY on the first FSR-FG frame because CE
// re-asserted the substitute UI resource (ffxConfigure/RegisterUiResource, which takes AMD's
// FrameInterpolationSwapchain criticalSection) from DetourPresent on AMD's PRESENTER thread, while AMD's
// Present held that criticalSection on the GAME thread spin-waiting (no timeout) on compositionFenceCPU —
// a fence only the presenter thread can advance. The re-assert must be forbidden for the real-present
// (presenter-thread) fallback driver in every case.
TEST(FFXUiCompositeDriverPolicyTest, ReassertForbiddenFromRealPresentFallbackDriver) {
    EXPECT_FALSE(MayReassertSubstituteUiResource(FFXUiCompositeDriver::kRealPresentFallback));
}

// The proxy-present prework runs on the game thread BEFORE AMD's Present enters its criticalSection — the
// same thread and lock order as the game's own per-frame RegisterUiResource — so the re-assert is legal
// there (and only there).
TEST(FFXUiCompositeDriverPolicyTest, ReassertAllowedOnlyFromProxyPresentPrework) {
    EXPECT_TRUE(MayReassertSubstituteUiResource(FFXUiCompositeDriver::kProxyPresentPrework));
}

TEST(FFXUiCompositeDriverPolicyTest, DriverFollowsProxyHookLiveness) {
    EXPECT_EQ(ChooseFFXUiCompositeDriver(true), FFXUiCompositeDriver::kProxyPresentPrework);
    EXPECT_EQ(ChooseFFXUiCompositeDriver(false), FFXUiCompositeDriver::kRealPresentFallback);
}

// The proxy hook may only patch a Present entry that resolves into the FFX runtime module. A real DXGI
// swapchain (dxgi.dll) or an sl_interposer/CE-wrapped chain is NOT the game-facing FFX proxy — hooking one
// of those would run the prework on the wrong thread (AMD's presenter), recreating the deadlock the proxy
// driver exists to prevent.
TEST(FFXProxyPresentHookInstallPolicyTest, InstallsOnlyForFFXRuntimeModulePresentEntry) {
    EXPECT_TRUE(ShouldInstallFFXProxyPresentHook(/*presentEntryInFFXRuntimeModule=*/true,
                                                 /*presentEntryIsCEDetour=*/false,
                                                 /*alreadyInstalledOnThisVtableEntry=*/false));
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(false, false, false));
}

TEST(FFXProxyPresentHookInstallPolicyTest, NeverStacksCEDetours) {
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(true, true, false));
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(false, true, false));
}

TEST(FFXProxyPresentHookInstallPolicyTest, IdempotentWhenAlreadyInstalledOnThisVtableEntry) {
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(true, false, true));
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(false, false, true));
}

// The route selection itself is unchanged by the driver split: under active runtime-owned no-callback FSR
// FG (not suspended, live queue is AMD's separate FG queue) the composite route must still be chosen — the
// driver split only decides WHERE the composite runs, never WHETHER it runs. Guards the shared route
// decision both drivers now consult (DX12_RunFFXProxyPrePresentWork and DetourPresent must never disagree).
TEST(FFXUiCompositeDriverPolicyTest, ActiveInterpolationStillChoosesCompositeRouteForBothDrivers) {
    const auto route = ce::dx12_overlay_policy::ChooseNoCallbackFSRFGOverlayRoute(
        /*runtimeOwnsSwapchain=*/true, /*liveSwapchainQueueIsOriginalGameQueue=*/false,
        /*fsrFGDisabledSuspendPending=*/false, /*uiResourceCachedForBundle=*/true,
        /*bundleOverlayActivelyFiring=*/false);
    EXPECT_EQ(route, ce::dx12_overlay_policy::NoCallbackFSRFGOverlayRoute::kSkipBundleCovers);
}

TEST(NativeFSROwnerQueuePolicyTest, UsesExactDescriptorQueueWhenItsDeviceOwnsTheTarget) {
    EXPECT_EQ(ce::dx12_overlay_policy::ChooseNativeFSROwnerQueueRoute(true, false, false),
              NativeFSROwnerQueueRoute::kExactDescriptorQueue);
    EXPECT_EQ(ce::dx12_overlay_policy::ChooseNativeFSROwnerQueueRoute(true, true, true),
              NativeFSROwnerQueueRoute::kExactDescriptorQueue);
}

TEST(NativeFSROwnerQueuePolicyTest, UnwrapsOnlyAProvenStreamlineQueueToTheRealGameQueue) {
    EXPECT_EQ(ce::dx12_overlay_policy::ChooseNativeFSROwnerQueueRoute(false, true, true),
              NativeFSROwnerQueueRoute::kStreamlineUnderlyingGameQueue);
    EXPECT_EQ(ce::dx12_overlay_policy::ChooseNativeFSROwnerQueueRoute(false, false, true),
              NativeFSROwnerQueueRoute::kUnavailable);
    EXPECT_EQ(ce::dx12_overlay_policy::ChooseNativeFSROwnerQueueRoute(false, true, false),
              NativeFSROwnerQueueRoute::kUnavailable);
}

// GTA session gtafsrfgflicker entered ffxCreateContext before CE finished replacing the cached export pointer.
// The nested protected DXGI create still captured the descriptor gameQueue, and ffxConfigure later supplied the
// proxy. That joined evidence must fill the missing binding, but must never replace a binding published by the
// primary ffxCreateContext hook.
TEST(NativeFSROwnerQueuePolicyTest, RecoversMissingBindingFromProtectedCreateAndConfigureEvidence) {
    EXPECT_TRUE(ShouldRecoverNativeFSRProxyBindingFromProtectedCreate(false, true, true, true));
    EXPECT_FALSE(ShouldRecoverNativeFSRProxyBindingFromProtectedCreate(true, true, true, true));
}

TEST(NativeFSROwnerQueuePolicyTest, RecoveryRequiresEveryHalfOfTheJoinedEvidence) {
    EXPECT_FALSE(ShouldRecoverNativeFSRProxyBindingFromProtectedCreate(false, false, true, true));
    EXPECT_FALSE(ShouldRecoverNativeFSRProxyBindingFromProtectedCreate(false, true, false, true));
    EXPECT_FALSE(ShouldRecoverNativeFSRProxyBindingFromProtectedCreate(false, true, true, false));
}

TEST(FFXPresenterFallbackPolicyTest, CompositesOnlyOncePerAcceptedUiRegistration) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldCompositeFFXPresenterFallback(41, 40));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldCompositeFFXPresenterFallback(41, 41));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldCompositeFFXPresenterFallback(42, 41));
}

TEST(FFXPresenterFallbackPolicyTest, UnknownRegistrationSequenceDoesNotSuppressCoverage) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldCompositeFFXPresenterFallback(0, 0));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldCompositeFFXPresenterFallback(0, 9));
}

TEST(PostSLAllocatorPolicyTest, KeepsPreferredCompletedSlotForRoundRobinLocality) {
    const uint64_t fenceValues[] = {10, 20, 30, 40};
    EXPECT_EQ(2, ce::dx12_overlay_policy::ChooseReadyOverlayAllocatorSlot(fenceValues, 4, 2, 30));
}

TEST(PostSLAllocatorPolicyTest, SkipsBusyPreferredSlotWithoutWaiting) {
    const uint64_t fenceValues[] = {10, 50, 30, 60};
    EXPECT_EQ(2, ce::dx12_overlay_policy::ChooseReadyOverlayAllocatorSlot(fenceValues, 4, 1, 30));
}

TEST(PostSLAllocatorPolicyTest, ReportsOnlyAnEntirelyBusyPoolAsUnavailable) {
    const uint64_t fenceValues[] = {31, 50, 32, 60};
    EXPECT_EQ(-1, ce::dx12_overlay_policy::ChooseReadyOverlayAllocatorSlot(fenceValues, 4, 1, 30));
}

}  // namespace
