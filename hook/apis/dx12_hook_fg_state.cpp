#include "dx12_hook_internal.h"
#include "dx12_hook_main_shared.h"


uint64_t HookAllocateOverlayFGPublicationSequence() {
    return g_OverlayFGPublicationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
}
void HookUpdatePreferredOverlayFGPublicationState(bool active, ce::fg_runtime::RuntimeMode runtimeMode,
                                                  const char* source) {
    const bool previousValid = g_PreferredOverlayFGPublicationStateValid.load(std::memory_order_acquire);
    const bool previousActive = g_PreferredOverlayFGPublicationStateActive.load(std::memory_order_acquire);
    const auto previousRuntimeMode = static_cast<ce::fg_runtime::RuntimeMode>(
        g_PreferredOverlayFGPublicationStateRuntimeMode.load(std::memory_order_acquire));
    const uint64_t nextSequence = HookAllocateOverlayFGPublicationSequence();

    g_PreferredOverlayFGPublicationStateActive.store(active, std::memory_order_release);
    g_PreferredOverlayFGPublicationStateRuntimeMode.store(static_cast<int>(runtimeMode), std::memory_order_release);
    g_PreferredOverlayFGPublicationStateSequence.store(nextSequence, std::memory_order_release);
    g_PreferredOverlayFGPublicationStateValid.store(true, std::memory_order_release);

    if (!previousValid || previousActive != active || previousRuntimeMode != runtimeMode) {
        HookLogImportant("FG publication preferred state: source=%s runtime=%s active=%d sequence=%llu",
                         source ? source : "unknown", ce::fg_runtime::GetRuntimeModeName(runtimeMode), active ? 1 : 0,
                         static_cast<unsigned long long>(nextSequence));
    }
}
bool HookTryGetPreferredOverlayFGPublicationState(PreferredOverlayFGPublicationState* state) {
    if (!state) {
        return false;
    }

    if (g_PreferredOverlayFGPublicationStateValid.load(std::memory_order_acquire)) {
        state->valid = true;
        state->active = g_PreferredOverlayFGPublicationStateActive.load(std::memory_order_acquire);
        state->runtimeMode = static_cast<ce::fg_runtime::RuntimeMode>(
            g_PreferredOverlayFGPublicationStateRuntimeMode.load(std::memory_order_acquire));
        state->sequence = g_PreferredOverlayFGPublicationStateSequence.load(std::memory_order_acquire);
        return true;
    }

    state->valid = true;
    state->active = g_FGCompat.IsFGActive();
    state->runtimeMode = g_FGCompat.GetRuntimeMode();
    state->sequence = 0;
    return true;
}
void DX12_AccountOverlayTransportPresent(bool inheritCoverageIfNoDraw, const char* gate, const char* source) {
    NoteDX12OverlayCoverageGate(gate ? gate : "transport-present-uncovered");
    AccountPresentForOverlayCoverage(inheritCoverageIfNoDraw, source ? source : "transport-present");
}
static void LogDX12OverlayVisibilityGap(const char* context, const char* reason, ULONGLONG warnAfterMs = 250) {
    const ULONGLONG lastRenderMs = dx12_hook_g_LastDX12OverlayRenderTickMs.load(std::memory_order_acquire);
    if (!lastRenderMs) {
        return;
    }

    const ULONGLONG nowMs = GetTickCount64();
    if (nowMs < lastRenderMs || nowMs - lastRenderMs < warnAfterMs) {
        return;
    }

    static std::atomic<int> s_visibilityGapLogCount{0};
    const int logCount = s_visibilityGapLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
        HookLogImportant("DX12: Overlay visibility gap while %s (%s) — lastRenderAge=%llums lastRoute=%s log=%d",
                         context && context[0] ? context : "transitioning",
                         reason && reason[0] ? reason : "waiting for safe render route",
                         static_cast<unsigned long long>(nowMs - lastRenderMs), DX12OverlayRenderRouteName(route),
                         logCount + 1);
    }
}
bool HookIsPostSLOverlayActiveButUnconfirmed() {
    return dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.load(std::memory_order_acquire) ||
           (dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) &&
            !dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire));
}
bool HookHasPostSLSyntheticStartupActivationEntered() {
    return dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.load(std::memory_order_acquire);
}
bool HookIsPostSLOverlayConfirmedRendering() {
    return dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
}
bool HookIsPostSLOverlayConfirmedButStartupSettling() {
    return ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(
        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
        dx12_hook_g_PostSLStableFrameCount.load(std::memory_order_acquire));

}
bool HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() {
    const bool extendRuntimeStateStabilization =
        dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.load(std::memory_order_acquire);
    return ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(
        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
        dx12_hook_g_PostSLStableFrameCount.load(std::memory_order_acquire), extendRuntimeStateStabilization);
}
int HookGetPostSLRuntimeStateStabilizationLastFrame() {
    return ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationLastFrame(
        dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.load(std::memory_order_acquire));
}
bool HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected() {
    return ce::dx12_overlay_policy::ShouldDeferStaleOffDuringConfirmedPostSLWarmup(
        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
        dx12_hook_g_PostSLStableFrameCount.load(std::memory_order_acquire));
}
int HookGetPostSLStaleOffWarmupProtectionLastFrame() {
    return ce::dx12_overlay_policy::GetConfirmedPostSLStaleOffWarmupProtectionLastFrame();
}
bool HookIsPostSLOverlayConfirmedButGetStateOffWarmupProtected() {
    return HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
}
int HookGetPostSLGetStateOffWarmupProtectionLastFrame() {
    return HookGetPostSLStaleOffWarmupProtectionLastFrame();
}
bool HookHasFSRFGHistory() {
    return dx12_hook_g_HadFSRFGPhase;
}
bool HookHasExplicitStreamlineSetOptionsActivation() {
    return StreamlineHook::HasExplicitSetOptionsActivationForCurrentComeback();
}
extern "C" __declspec(dllexport) void DX12_SetDeferOverlaySubmitToSteamECL(bool defer) {
    dx12_hook_g_deferOverlaySubmitToSteamECL = defer;
    if (!defer) {
        // Clear any stale deferred state
        dx12_hook_g_steamDeferredOverlay.pending = false;
        dx12_hook_g_steamDeferredOverlay.cmdList = nullptr;
        dx12_hook_g_steamDeferredOverlay.allocIdx = -1;
        dx12_hook_g_steamDeferredOverlay.eclQueue = nullptr;
    }
}
extern "C" __declspec(dllexport) bool DX12_IsDeferOverlaySubmitPending() {
    return dx12_hook_g_steamDeferredOverlay.pending;
}
bool HookHasSafePostFSRBootstrapPath() {
    return HookHasSafePostFSRBootstrapPathImpl();
}
bool HookHasRuntimeOwnedNativeFGPresentPath() {
    if (dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire)) {
        return true;
    }
    if (dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire)) {
        return true;
    }
    if (dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire)) {
        return true;
    }
    return ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(
        DXGIShared::DoesFGRuntimeOwnSwapchain(), g_FGCompat.HasDirectFFXApiConfirmation(),
        dx12_hook_g_NativeFSRStartupConfigureArmingPending.load(std::memory_order_acquire));
}
void DX12_StartTransitionCooldown() {
    StartTransitionCooldown();
}
void DX12_BeginStreamlineEnableCall() {
    dx12_hook_g_StreamlineEnableCallsInFlight.fetch_add(1, std::memory_order_acq_rel);
}
void DX12_EndStreamlineEnableCall() {
    uint32_t current = dx12_hook_g_StreamlineEnableCallsInFlight.load(std::memory_order_acquire);
    while (current != 0 && !dx12_hook_g_StreamlineEnableCallsInFlight.compare_exchange_weak(
                               current, current - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {}
    if (current == 0) {
        HookLogImportant("DX12: Streamline enable-call tracking underflow — reset in-flight count");
    }
}
void DX12_PrepareForStreamlineEnableTransition() {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    if (runtimeMode != ce::fg_runtime::RuntimeMode::kFSRFG || !dx12_hook_g_FGRuntimeOwnsSwapchain) {
        return;
    }

    DXGIShared::ArmStreamlineStartupTransitionWindow();
    StartTransitionCooldown();
    WaitForOverlayGpuIdle("DX12: Streamline enable prep");
    InvalidateAllOverlayCachedFrames();
    HookLogImportant(
        "DX12: Preparing for Streamline FG enable while live FSR runtime owns the swapchain "
        "(runtime=%s apiFSR=%d origGame=%p scQueue=%p cmdQ=%p)",
        ce::fg_runtime::GetRuntimeModeName(runtimeMode), g_FGCompat.IsFSRFGApiActive() ? 1 : 0, dx12_hook_g_OriginalGameQueue,
        dx12_hook_g_SwapchainQueue, g_CommandQueue.load(std::memory_order_acquire));
}
bool DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration() {
    const bool progressResolvedOfficialFFXPresentPath =
        dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);
    return ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(
        dx12_hook_g_FGRuntimeOwnsSwapchain || progressResolvedOfficialFFXPresentPath,
        dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire));
}
DWORD DX12_GetGamePresentThreadId() {
    return dx12_hook_g_GamePresentThreadId.load(std::memory_order_acquire);
}
void DX12_OnStreamlineExplicitSetOptionsActivationConfirmed() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
        true, false, "already-live comeback upgraded by explicit SetOptions");
}
