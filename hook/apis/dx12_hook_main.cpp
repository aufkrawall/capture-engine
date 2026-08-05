#include "dx12_hook_internal.h"
#include "dx12_hook_main_shared.h"

#include "dx12_hook_internal.h"

// Global Function Pointers for detours (Visible to other modules)
ExecuteCommandListsPtr oExecuteCommandLists = nullptr;

static void FillFGSessionLegacyStateView(ce::fg_session::DX12LegacyStateView* out);

extern "C" {
// NOINLINE: Prevents LTO from inlining into the ECL detour, which would
// allow the compiler to merge vtable reads and optimize away our safety checks.
__attribute__((noinline)) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue) {
    DX12_SetCommandQueueInternal(pQueue, false, nullptr);
}

}  // extern "C" (DX12_SetCommandQueue)

static void FindAndWrapPreExistingSwapchains();

CreateCommittedResourcePtr oCreateCommittedResource = nullptr;

CreateCommandQueuePtr oTraceCreateCommandQueue = nullptr;

CreateDescriptorHeapPtr oTraceCreateDescriptorHeap = nullptr;

CommandQueueSignalPtr oTraceCommandQueueSignal = nullptr;

std::atomic<int> g_PostSLECLDiagCount{0};

std::atomic<ID3D12Device*> g_Device{nullptr};

std::atomic<ID3D12CommandQueue*> g_CommandQueue{nullptr};

std::recursive_mutex g_CommandQueueMutex;

ID3D12Resource* g_DummyBackBuffer = nullptr;

DX12Hook* g_dx12HookInstance = nullptr;

std::recursive_mutex g_DeviceQueuesMutex;

std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

static void FillFGSessionLegacyStateView(ce::fg_session::DX12LegacyStateView* out);
static void FindAndWrapPreExistingSwapchains();

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

static DX12Context GetDX12Context() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return DX12Context(g_Device.load(), g_CommandQueue.load());
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

static bool DX12_TryInvokePostSLStartupActivationCallbackFromSharedService(const char* source,
                                                                           bool clearStartupWindow) {
    return DX12_TryInvokePostSLStartupActivationCallback(source, clearStartupWindow, false);
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

static void FillFGSessionLegacyStateView(ce::fg_session::DX12LegacyStateView* out) {
    if (!out) {
        return;
    }

    ce::fg_session::DX12LegacyStateView view;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        view.originalGameQueue = dx12_hook_g_OriginalGameQueue;
        view.primaryGameQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
        view.swapchainQueue = dx12_hook_g_SwapchainQueue;
        view.currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        view.slWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);
        view.realQueueBehindWrapper = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
        view.postSLLockedQueue = dx12_hook_g_PostSLLockedQueue;
        view.postSLLastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
        view.postSLDedicatedQueue = dx12_hook_g_PostSLDedicatedQueue;
        view.realECL = reinterpret_cast<void*>(dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire));
        view.runtimeOwnsSwapchain = dx12_hook_g_FGRuntimeOwnsSwapchain;
    }

    view.hadFSRPhase = dx12_hook_g_HadFSRFGPhase;
    view.safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    view.explicitSetOptionsActivationForCurrentComeback = HookHasExplicitStreamlineSetOptionsActivation();
    view.streamlineStartupHandoffPending =
        DXGIShared::g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
    view.startupTopLevelPresentConsumed =
        DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    view.postSLCallbackInstalled = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    view.postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
    view.postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    view.postSLSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    view.postSLStartupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    view.postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    view.postSLStableFrameCount = dx12_hook_g_PostSLStableFrameCount.load(std::memory_order_acquire);
    view.fgTransitionCooldown = dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire);
    view.observerOnly = HookOverlayObserverOnlyEnabled();
    view.observerPolicyOnly = HookOverlayObserverPolicyOnlyEnabled();
    view.observerStartupPresentOnly = HookOverlayObserverStartupPresentOnlyEnabled();
    view.usingFFXPresentCallbackPath = dx12_hook_g_FFXPresentOverlayDevice != nullptr;

    *out = view;
}

extern "C" __declspec(dllexport) void DX12_SetWrappedPresentFocusLossContext(const char* presentName, int callCount,
                                                                             UINT syncInterval, UINT presentFlags) {
    dx12_hook_s_WrappedPresentFocusLossContext.valid = true;
    dx12_hook_s_WrappedPresentFocusLossContext.presentName = presentName;
    dx12_hook_s_WrappedPresentFocusLossContext.callCount = callCount;
    dx12_hook_s_WrappedPresentFocusLossContext.syncInterval = syncInterval;
    dx12_hook_s_WrappedPresentFocusLossContext.presentFlags = presentFlags;
}

extern "C" __declspec(dllexport) void DX12_ClearWrappedPresentFocusLossContext() {
    dx12_hook_s_WrappedPresentFocusLossContext = {};
}

void EnsureDX12Hook() {
    if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
    }
}

void DX12Hook::Init() {
    EnsureDX12Hook();  // Self-init check
    static std::recursive_mutex s_InitMutex;
    static bool s_InitDone = false;
    std::lock_guard<std::recursive_mutex> lock(s_InitMutex);
    if (s_InitDone)
        return;
    s_InitDone = true;

    ce::fg_session::RegisterDX12LegacyStateProvider(&FillFGSessionLegacyStateView);
    DXGIShared::g_PostSLStartupActivationService.store(&DX12_TryInvokePostSLStartupActivationCallbackFromSharedService,
                                                       std::memory_order_release);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPresentObserved, "DX12Hook::Init");

    // CRITICAL FIX: Check if Vulkan is active before installing ANY DXGI hooks
    // Vulkan games using WSI-to-DXGI mapping can freeze if we hook DXGI
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (hVulkan) {
        HookLog(
            "DX12: Vulkan detected (vulkan-1.dll), SKIPPING ALL DXGI hook "
            "installation");
        return;
    }

    // Arm DRED auto-breadcrumbs + page-fault as early as possible. DRED is a
    // process-global setting that only affects devices created AFTER this call,
    // so it must run before the game creates its D3D12 device. The dedicated
    // Wrapped_D3D12CreateDevice arming point only exists when ENABLE_D3D12_WRAPPER
    // is built (it requires d3d12_wrappers.dll, which is absent in normal builds),
    // so for the common inject path this DX12Hook::Init() call — which runs on a
    // worker thread well before the game's device is created — is the real arming
    // site. Without it a device-hung yields no breadcrumbs (GetAutoBreadcrumbsOutput
    // fails because auto-breadcrumbs were never enabled for the game's device).
    ce::dx12_dred::ArmBeforeDeviceCreation();
    // Optional D3D12 debug layer (env CE_DX12_DEBUG_LAYER) — must be enabled before
    // the game's device is created. Off by default; used to capture the exact
    // resource-state/hazard behind the Alt+Tab overlay-draw hang.
    ce::dx12_dred::ArmDebugLayerBeforeDeviceCreation();

    // Note: Crash handler is installed in DllMain (hook/main.cpp)

    // Start freeze detection watchdog with dynamic timeout based on game engine
    // The watchdog auto-detects UE5, DLSS FG and uses extended timeouts
    double timeout = g_RenderWatchdog.GetRecommendedTimeout();
    g_RenderWatchdog.SetMonitoredThread(GetCurrentThreadId());
    g_RenderWatchdog.SetPreferredThreadProvider(&DX12_GetGamePresentThreadId);
    g_RenderWatchdog.Start(timeout);
    HookLog("DX12: Freeze watchdog started (%.0f second timeout)", timeout);

    // CRITICAL FIX: Install global swapchain vtable hooks by getting the vtable
    // directly from the DXGI module. This avoids creating a temp swapchain which
    // causes deadlocks with Steam overlay + Streamline.
    InstallGlobalVTableHooks();

#ifdef ENABLE_D3D12_WRAPPER
    // When D3D12 wrapper is enabled, Present inline hooks are deferred to
    // EnsurePresentHooks() (called from Wrapped_D3D12CreateDevice) to avoid
    // creating a temp D3D12 device in DX11-only apps that load d3d12.dll via
    // D3D11On12.
    HookLog("DX12Hook: Initialized (factory hooks installed; Present hooks deferred to D3D12CreateDevice)");
#else
    const bool d3d12DeviceCreated = WasD3D12DeviceCreated();
    const char* startupOverlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    // Without D3D12 wrapper, D3D12CreateDevice isn't hooked and the deferred
    // trigger never fires.  Install Present inline hooks now via a temp
    // swapchain so pre-existing swapchains (created before injection) are
    // covered.  The temp device/swapchain is destroyed immediately after
    // hooking, so DX11 state corruption is not a concern.
    if (ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(d3d12DeviceCreated,
                                                                                     startupOverlayModule != nullptr)) {
        HookLogImportant(
            "DX12Hook: Deferring eager temp-swapchain Present hook install because third-party overlay %s is already "
            "loaded before the first real D3D12 device",
            startupOverlayModule);
    } else {
        HookLog("DX12Hook: Installing Present hooks eagerly (no D3D12 wrapper)");
        HookSwapchainVTableViaTempSwapchain();
    }
    if (DXGIShared::HasPresentInlineHooks() || DXGIShared::HasPresentDetourHooks()) {
        HookLog("DX12Hook: Initialized (factory + Present hooks installed)");
    } else {
        HookLogImportant(
            "DX12Hook: Initialized (factory hooks installed; Present hooks deferred to "
            "FindAndWrapPreExistingSwapchains)");
    }
#endif

    FindAndWrapPreExistingSwapchains();
}

void DX12Hook::EnsurePresentHooks() {
    static std::atomic<bool> s_done{false};
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // Already installed
    }
    HookLog("DX12: Installing Present inline hooks (D3D12 device created by game)");
    HookSwapchainVTableViaTempSwapchain();
    HookLog("DX12: Present inline hooks installed");
}

static void FindAndWrapPreExistingSwapchains() {
    if (DXGIShared::HasPresentInlineHooks() || DXGIShared::HasPresentDetourHooks()) {
        HookLog("DX12: Pre-existing swapchain support via inline Present hooks already active");
        return;
    }

    // Present hooks were deferred during DX12Hook::Init() because a third-party
    // overlay (e.g. nvspcap64.dll) was loaded before the game's first real D3D12
    // device existed.  The eager temp-swapchain approach was skipped to avoid
    // recursing through the overlay's startup hook chain.
    //
    // If the game has already created its swapchain during the injection window,
    // the CreateSwapChainForHwnd detours will never fire, and Present hooks
    // would remain uninstalled forever — the overlay would never render.
    //
    // Try installing Present hooks now via a second temp swapchain.  The
    // g_CreatingTempSwapchain guard prevents our own CreateSwapChainForHwnd
    // hooks from processing the temp swapchain's queue, and calling
    // oCreateSwapChainForHwndGlobal bypasses our hooks entirely.  At this point
    // the overlay's startup hook chain should be settled, so the recursion risk
    // is minimal.
    HookLogImportant(
        "DX12: Present hooks not installed during init — installing via "
        "postponed temp swapchain for pre-existing swapchain coverage");
    HookSwapchainVTableViaTempSwapchain();

    if (DXGIShared::HasPresentInlineHooks() || DXGIShared::HasPresentDetourHooks()) {
        HookLogImportant("DX12: Present hooks installed via postponed temp swapchain");
    } else {
        HookLogImportant(
            "DX12: Postponed temp swapchain also failed — pre-existing "
            "swapchains will not have overlay until a real CreateSwapChainForHwnd "
            "call is intercepted");
    }
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

