#include "dx12_hook_internal.h"
#include "dx12_hook_main_shared.h"


bool DX12_TryRenderExactPostSLBeforeStartupHandoffPresent(IDXGISwapChain* pSwapChain, const char* source) {
    auto postSLCallback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
    if (!pSwapChain || !postSLCallback || dx12_hook_g_RequireExactPostSLStartupTransportDraw) {
        return false;
    }

    const uint64_t drawsBefore = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire);
    dx12_hook_g_RequireExactPostSLStartupTransportDraw = true;
    postSLCallback(pSwapChain);
    dx12_hook_g_RequireExactPostSLStartupTransportDraw = false;
    const bool drawn = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire) != drawsBefore;

    static std::atomic<int> s_exactStartupTransportDrawLogCount{0};
    const int logCount = s_exactStartupTransportDrawLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logCount <= 20 || (logCount % 120) == 0 || !drawn) {
        HookLogImportant(
            "[OVERLAY VISIBILITY] exact PostSL startup-transport draw %s before Present "
            "(source=%s swapchain=%p activeOfficialUiCoverage=%d log=%d)",
            drawn ? "SUBMITTED" : "MISSED", source ? source : "unknown", pSwapChain,
            ce::dx12_streamline_ui_overlay::HasActiveCoverage() ? 1 : 0, logCount);
    }
    return drawn;
}
ID3D12CommandQueue* DX12_AcquireOriginalGameQueueForOverlay() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    ID3D12CommandQueue* queue = dx12_hook_g_OriginalGameQueue;
    if (queue) {
        queue->AddRef();
    }
    return queue;
}
void DX12_DumpDredIfDeviceRemoved(const char* reason) {
    ID3D12Device* dev = g_Device.load(std::memory_order_acquire);
    if (dev && FAILED(dev->GetDeviceRemovedReason())) {
        ce::dx12_dred::DumpOnDeviceRemoved(dev, reason ? reason : "freeze watchdog");
    }
}
void DX12_LogOverlayGpuBreadcrumbs(const char* reason) {
    if (dx12_hook_g_OverlayBcMapped) {
        const uint32_t seq = dx12_hook_g_OverlayBcSeq.load(std::memory_order_relaxed);
        uint32_t vals[kOverlayBcSlotCount] = {};
        for (uint32_t i = 0; i < kOverlayBcSlotCount; ++i) {
            vals[i] = dx12_hook_g_OverlayBcMapped[i];
        }
        uint32_t lastCompletedOp = 0;
        for (uint32_t op = kOverlayBcStart; op < kOverlayBcSlotCount; ++op) {
            if (vals[op] == seq && seq != 0) {
                lastCompletedOp = op;
            }
        }
        const char* opName = "none (GPU never reached the overlay list this frame — CE's list is NOT the stall)";
        switch (lastCompletedOp) {
            case kOverlayBcStart:
                opName = "start(list reset) — GPU stalled BEFORE the RT barrier";
                break;
            case kOverlayBcAfterRTBarrier:
                opName = "after RT barrier — GPU stalled in the overlay DRAW";
                break;
            case kOverlayBcAfterDraw:
                opName = "after overlay draw — GPU stalled in the PRESENT-back barrier";
                break;
            case kOverlayBcBeforeClose:
                opName =
                    "before Close — CE's WHOLE overlay list completed; wedge is a fence/CPU deadlock or AMD's work";
                break;
            default:
                break;
        }
        HookLogImportant(
            "DX12: [overlay-gpu-breadcrumb] %s — latestSeq=%u lastCompletedOp=%u (%s) "
            "slots[start=%u rt=%u draw=%u close=%u]",
            reason ? reason : "freeze", seq, lastCompletedOp, opName, vals[kOverlayBcStart],
            vals[kOverlayBcAfterRTBarrier], vals[kOverlayBcAfterDraw], vals[kOverlayBcBeforeClose]);
    }
    // Also dump the FFX UI-composite fence state + timeline (works even if breadcrumb buffer isn't armed).
    DX12_LogFFXUiCompositeFreezeDiagnostics(reason);
}
void DX12_ClearNativeFSRRuntimeOwnedTeardown(const char* reason) {
    if (dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant("DX12: Cleared explicit native FSR runtime-owned teardown latch (%s)",
                         reason && reason[0] ? reason : "unspecified");
    }
}
void DX12_ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason) {
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption(reason);
}
bool DX12_IsNativeFSRStartupConfigureArmingPending() {
    return dx12_hook_g_NativeFSRStartupConfigureArmingPending.load(std::memory_order_acquire);
}
void DX12_ClearNativeFSRStartupConfigureArming(const char* reason) {
    SetNativeFSRStartupConfigureArmingPending(false, reason);
    ClearDeferredOfficialFFXTakeoverSideEffects(reason);
    ClearProtectedOfficialFFXStartupSwapchainPending(reason);
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption(reason);
}
void DX12_RetireProtectedOfficialFFXStartupForSuccessfulStreamlineEnable() {
    const bool protectedFFXStartupPending =
        dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire);
    const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
    if (!ce::dx12_overlay_policy::ShouldRetireProtectedOfficialFFXStartupForSuccessfulStreamlineEnable(
            protectedFFXStartupPending, true, authoritativeFSRActive)) {
        return;
    }

    HookLogImportant(
        "DX12: Successful explicit Streamline enable superseded provisional official FFX startup — "
        "retiring staged FFX queue and restoring CE overlay/queue side effects (apiFSR=%d)",
        authoritativeFSRActive ? 1 : 0);
    DX12_ClearNativeFSRStartupConfigureArming("successful explicit Streamline enable superseded FFX startup");
}
void DX12_RetainStreamlineStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source) {
    if (!swapchain || !IsUsableStartupActivationSwapchainPointer(swapchain)) {
        return;
    }

    swapchain->AddRef();

    IDXGISwapChain* oldSwapchain = nullptr;
    {
        std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
        oldSwapchain = dx12_hook_g_StreamlineStartupActivationSwapchain;
        dx12_hook_g_StreamlineStartupActivationSwapchain = swapchain;
        g_StreamlineStartupActivationSwapchainGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    HookLogImportant(
        "DX12: Retained Streamline startup activation swapchain %p (source=%s generation=%llu) — "
        "PostSL startup can recover even if ProcessFrame is stale",
        swapchain, source ? source : "unknown",
        static_cast<unsigned long long>(
            g_StreamlineStartupActivationSwapchainGeneration.load(std::memory_order_acquire)));

    if (oldSwapchain) {
        SafeReleaseStartupActivationSwapchain(oldSwapchain, source);
    }
}
static IDXGISwapChain* AcquireRetainedStreamlineStartupActivationSwapchain() {
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();

    IDXGISwapChain* swapchain = nullptr;
    {
        std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
        if (ce::dx12_overlay_policy::ShouldPreferRetainedStreamlineStartupActivationSwapchain(
                dx12_hook_g_StreamlineStartupActivationSwapchain != nullptr, startupActivationPending,
                postSLActiveButUnconfirmed) &&
            IsUsableStartupActivationSwapchainPointer(dx12_hook_g_StreamlineStartupActivationSwapchain)) {
            swapchain = dx12_hook_g_StreamlineStartupActivationSwapchain;
            swapchain->AddRef();
        }
    }

    return swapchain;
}
static IDXGISwapChain* AcquireSwapchainForStartupActivation(const char* source) {
    IDXGISwapChain* retained = AcquireRetainedStreamlineStartupActivationSwapchain();
    if (retained) {
        return retained;
    }

    // Late-handoff fallback: the Streamline runtime can create the real
    // swapchain before CE captures the original game queue, so the create-time
    // retention (freshAuthoritativeStreamlineHandoff) never ran. The live
    // swapchain is still the correct PostSL activation target - retain it now
    // so the pending startup activation completes instead of staying
    // half-armed forever (RoboCop: Rogue City session 20260809_143910).
    IDXGISwapChain* liveSwapchain = dx12_hook_g_LastSwapChain;
    if (ce::dx12_overlay_policy::ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation(
            false, liveSwapchain != nullptr && IsUsableStartupActivationSwapchainPointer(liveSwapchain) &&
                       IsDX12Swapchain(liveSwapchain),
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire),
            HookHasRuntimeOwnedNativeFGPresentPath(), g_FGCompat.IsFSRFGApiActive(),
            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire))) {
        static std::atomic<int> s_lateHandoffRetainLogCount{0};
        const int logCount = s_lateHandoffRetainLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: Late-handoff PostSL startup activation - retaining live swapchain %p as activation "
                "swapchain (source=%s log=%d)",
                liveSwapchain, source ? source : "unknown", logCount + 1);
        }
        DX12_RetainStreamlineStartupActivationSwapchain(liveSwapchain, "late-handoff live swapchain fallback");
        // The one-shot Streamline top-level startup bootstrap was consumed by
        // this late-handoff activation service (the create-time top-level
        // Present promotion never ran because the original game queue was
        // unknown when the swapchain was created). Mark it consumed so the
        // confirmed-startup settling window can be covered by the normal
        // keep-startup route; without it the overlay starves after the first
        // confirmed frame until settling ends (RoboCop 20260809_144640).
        if (!DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.exchange(
                true, std::memory_order_acq_rel)) {
            static std::atomic<int> s_consumedBootstrapLogCount{0};
            const int consumedLog = s_consumedBootstrapLogCount.fetch_add(1, std::memory_order_relaxed);
            if (consumedLog < 20 || (consumedLog % 200) == 0) {
                HookLogImportant(
                    "DX12: Late-handoff PostSL startup activation consumed the one-shot top-level bootstrap "
                    "(source=%s swapchain=%p log=%d)",
                    source ? source : "unknown", liveSwapchain, consumedLog + 1);
            }
        }
        retained = AcquireRetainedStreamlineStartupActivationSwapchain();
        if (retained) {
            return retained;
        }
    }

    static std::atomic<int> s_missingStartupActivationSwapchainLogCount{0};
    const int logCount = s_missingStartupActivationSwapchainLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "DX12: No startup activation swapchain available for PostSL recovery "
            "(source=%s startupPending=%d activeButUnconfirmed=%d retained=%p weakLast=%p; weak pointer is "
            "diagnostic-only)",
            source ? source : "unknown",
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_relaxed) ? 1 : 0,
            HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0, dx12_hook_g_StreamlineStartupActivationSwapchain, dx12_hook_g_LastSwapChain);
    }
    return nullptr;
}
bool DX12_TryInvokePostSLStartupActivationCallback(const char* source, bool clearStartupWindow,
                                                   bool allowConfirmedWarmupService) {
    if (HookHasRuntimeOwnedNativeFGPresentPath() || g_FGCompat.IsFSRFGApiActive()) {
        static std::atomic<int> s_nativeFSRStartupActivationSkipLogCount{0};
        const int logCount = s_nativeFSRStartupActivationSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: Skipping retained-swapchain PostSL startup activation callback "
                "(reason=native-fsr-present-path source=%s nativeFGPath=%d apiFSR=%d retained=%p last=%p log=%d)",
                source ? source : "unknown", HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
                g_FGCompat.IsFSRFGApiActive() ? 1 : 0, dx12_hook_g_StreamlineStartupActivationSwapchain, dx12_hook_g_LastSwapChain,
                logCount + 1);
        }
        return false;
    }

    auto postSLCallback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
    if (!postSLCallback) {
        return false;
    }

    IDXGISwapChain* activationSwapchain = AcquireSwapchainForStartupActivation(source);
    if (!activationSwapchain) {
        return false;
    }

    auto logSkippedActivationService = [&](const char* reason, bool inProgress) {
        static std::atomic<int> s_activationServiceSkipLogCount{0};
        const int logCount = s_activationServiceSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: Skipping retained-swapchain PostSL startup activation callback "
                "(reason=%s source=%s swapchain=%p clearWindow=%d startupPending=%d "
                "activeButUnconfirmed=%d startupActivationEntered=%d confirmed=%d inProgress=%d tid=0x%04X)",
                reason ? reason : "policy", source ? source : "unknown", activationSwapchain,
                clearStartupWindow ? 1 : 0,
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire) ? 1
                                                                                                                  : 0,
                HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0,
                HookHasPostSLSyntheticStartupActivationEntered() ? 1 : 0,
                dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) ? 1 : 0, inProgress ? 1 : 0,
                GetCurrentThreadId());
        }
    };

    const bool activationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool serviceAlreadyInProgress = g_PostSLStartupActivationServiceInProgress.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(
            true, true, activationPending, postSLStartupActivationEntered, postSLConfirmedRendering,
            serviceAlreadyInProgress, allowConfirmedWarmupService)) {
        const char* reason = serviceAlreadyInProgress         ? "in-progress"
                             : postSLConfirmedRendering       ? "already-confirmed"
                             : postSLStartupActivationEntered ? "startup-activation-entered"
                             : !activationPending             ? "activation-not-pending"
                                                              : "policy";
        logSkippedActivationService(reason, serviceAlreadyInProgress);
        SafeReleaseStartupActivationSwapchain(activationSwapchain, "DX12_TryInvokePostSLStartupActivationCallback");
        return false;
    }

    bool expectedInProgress = false;
    if (!g_PostSLStartupActivationServiceInProgress.compare_exchange_strong(
            expectedInProgress, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        logSkippedActivationService("in-progress-race", true);
        SafeReleaseStartupActivationSwapchain(activationSwapchain, "DX12_TryInvokePostSLStartupActivationCallback");
        return false;
    }

    const bool activationPendingAfterClaim =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLStartupActivationEnteredAfterClaim = HookHasPostSLSyntheticStartupActivationEntered();
    const bool postSLConfirmedRenderingAfterClaim = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(
            true, true, activationPendingAfterClaim, postSLStartupActivationEnteredAfterClaim,
            postSLConfirmedRenderingAfterClaim, false, allowConfirmedWarmupService)) {
        const char* reason = postSLConfirmedRenderingAfterClaim         ? "already-confirmed"
                             : postSLStartupActivationEnteredAfterClaim ? "startup-activation-entered"
                             : !activationPendingAfterClaim             ? "activation-not-pending"
                                                                        : "policy";
        logSkippedActivationService(reason, false);
        g_PostSLStartupActivationServiceInProgress.store(false, std::memory_order_release);
        SafeReleaseStartupActivationSwapchain(activationSwapchain, "DX12_TryInvokePostSLStartupActivationCallback");
        return false;
    }

    if (clearStartupWindow) {
        DXGIShared::ClearStreamlineStartupTransitionWindow();
    }

    HookLogImportant(
        "DX12: Invoking retained-swapchain PostSL startup activation callback "
        "(source=%s swapchain=%p clearWindow=%d startupPending=%d activeButUnconfirmed=%d "
        "startupActivationEntered=%d confirmed=%d warmupService=%d tid=0x%04X)",
        source ? source : "unknown", activationSwapchain, clearStartupWindow ? 1 : 0,
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire) ? 1 : 0,
        HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0, HookHasPostSLSyntheticStartupActivationEntered() ? 1 : 0,
        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) ? 1 : 0, allowConfirmedWarmupService ? 1 : 0,
        GetCurrentThreadId());
    postSLCallback(activationSwapchain);
    HookLogImportant(
        "DX12: Retained-swapchain PostSL startup activation callback returned "
        "(source=%s swapchain=%p startupPending=%d activeButUnconfirmed=%d startupActivationEntered=%d "
        "confirmed=%d tid=0x%04X)",
        source ? source : "unknown", activationSwapchain,
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire) ? 1 : 0,
        HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0, HookHasPostSLSyntheticStartupActivationEntered() ? 1 : 0,
        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) ? 1 : 0, GetCurrentThreadId());
    g_PostSLStartupActivationServiceInProgress.store(false, std::memory_order_release);
    SafeReleaseStartupActivationSwapchain(activationSwapchain, "DX12_TryInvokePostSLStartupActivationCallback");
    return true;
}
bool DX12_TryInvokePostSLStartupActivationCallbackFromSharedService(const char* source,
                                                                    bool clearStartupWindow) {
    return DX12_TryInvokePostSLStartupActivationCallback(source, clearStartupWindow, false);
}

void SafeReleaseStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source) {
if (!swapchain) {
    return;
}

if (!IsUsableStartupActivationSwapchainPointer(swapchain)) {
    static std::atomic<int> s_skipUnsafeSwapchainReleaseLogCount{0};
    const int logCount = s_skipUnsafeSwapchainReleaseLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "DX12: Skipping unsafe startup activation swapchain Release for stale pointer %p "
            "(source=%s log=%d)",
            swapchain, source ? source : "unknown", logCount + 1);
    }
    return;
}

swapchain->Release();
}


void ReleaseStreamlineStartupActivationSwapchain(const char* source) {
IDXGISwapChain* oldSwapchain = nullptr;
{
    std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
    oldSwapchain = dx12_hook_g_StreamlineStartupActivationSwapchain;
    dx12_hook_g_StreamlineStartupActivationSwapchain = nullptr;
}

if (oldSwapchain) {
    HookLogImportant("DX12: Released retained Streamline startup activation swapchain %p (source=%s)", oldSwapchain,
                     source ? source : "unknown");
    SafeReleaseStartupActivationSwapchain(oldSwapchain, source);
}
}


bool HasRetainedStreamlineStartupActivationSwapchain() {
std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
return dx12_hook_g_StreamlineStartupActivationSwapchain != nullptr;
}

namespace DXGIShared {
bool DX12_ShouldEagerDrawOverlayBeforeStreamlineStartupBypass(IDXGISwapChain* pSwapChain, bool isD3D12,
                                                              bool streamlineFGRunning,
                                                              bool postSLConfirmedRendering, bool hadFSRFGPhase,
                                                              bool explicitSetOptionsActivation) {
    (void)pSwapChain;
    const bool configEagerEnabled = DXGIShared::IsDlssToggleEagerOverlayEnabled();
    const bool postSLCallbackInstalled =
        DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    const bool explicitEnablePureDLSSColdStartProof =
        ce::dx12_overlay_policy::HasExplicitEnablePureDLSSColdStartProof(
            hadFSRFGPhase, explicitSetOptionsActivation, HasRetainedStreamlineStartupActivationSwapchain(),
            postSLCallbackInstalled);
    bool backendQueueMatchesSwapchainQueue = false;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        backendQueueMatchesSwapchainQueue = dx12_hook_g_SwapchainQueue != nullptr &&
                                            dx12_hook_g_OverlayAdapterBackendQueue.load(std::memory_order_acquire) ==
                                                dx12_hook_g_SwapchainQueue;
    }
    return ce::dx12_overlay_policy::ShouldEagerDrawOverlayBeforeStreamlineStartupBypass(
        isD3D12, streamlineFGRunning, postSLConfirmedRendering, hadFSRFGPhase, configEagerEnabled,
        explicitEnablePureDLSSColdStartProof, dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
        backendQueueMatchesSwapchainQueue);
}
}


bool HasUsableRetainedStreamlineStartupActivationSwapchainCandidate() {
std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
return IsUsableStartupActivationSwapchainPointer(dx12_hook_g_StreamlineStartupActivationSwapchain);
}


bool HasStartupActivationSwapchainCandidateForECLProbe() {
return HasUsableRetainedStreamlineStartupActivationSwapchainCandidate();
}
