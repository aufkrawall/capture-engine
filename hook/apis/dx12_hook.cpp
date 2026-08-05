#include "dx12_hook_internal.h"

// Global Function Pointers for detours (Visible to other modules)
ExecuteCommandListsPtr oExecuteCommandLists = nullptr;

CreateCommittedResourcePtr oCreateCommittedResource = nullptr;

CreateCommandQueuePtr oTraceCreateCommandQueue = nullptr;

CreateDescriptorHeapPtr oTraceCreateDescriptorHeap = nullptr;

CommandQueueSignalPtr oTraceCommandQueueSignal = nullptr;

// Bypass trampoline for ECL that skips Streamline's hook.
// When SL FG is active, overlay ECLs are submitted through this trampoline
// so SL's internal frame tracking doesn't see our extra command lists.
static std::atomic<ExecuteCommandListsPtr> g_SLBypassECL{nullptr};

static std::atomic<bool> g_PreferredOverlayFGPublicationStateValid{false};

static std::atomic<bool> g_PreferredOverlayFGPublicationStateActive{false};

static std::atomic<int> g_PreferredOverlayFGPublicationStateRuntimeMode{
    static_cast<int>(ce::fg_runtime::RuntimeMode::kOff)};

static std::atomic<uint64_t> g_OverlayFGPublicationSequence{0};

static std::atomic<uint64_t> g_PreferredOverlayFGPublicationStateSequence{0};

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

// PostSL ECL diagnostic counter — reset on each PostSL reactivation epoch.
std::atomic<int> g_PostSLECLDiagCount{0};

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

// Check if deferred overlay is still pending (not consumed by ECL hook).
extern "C" __declspec(dllexport) bool DX12_IsDeferOverlaySubmitPending() {
    return dx12_hook_g_steamDeferredOverlay.pending;
}

// CRITICAL FIX: Use atomic pointers for thread-safe access
// These are read/written from multiple threads (hook thread, present thread, etc.)
std::atomic<ID3D12Device*> g_Device{nullptr};

std::atomic<ID3D12CommandQueue*> g_CommandQueue{nullptr};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_CommandQueueMutex;

ID3D12CommandQueue* DX12_AcquireOriginalGameQueueForOverlay() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    ID3D12CommandQueue* queue = dx12_hook_g_OriginalGameQueue;
    if (queue) {
        queue->AddRef();
    }
    return queue;
}

// Called by the freeze watchdog when it captures a freeze dump. If the D3D12
// device is in a removed/hung state, emit DRED auto-breadcrumbs + page-fault info
// so a device-hung freeze (e.g. the x86 DX12 focus-loss transition) is recorded
// with the exact faulting GPU op. No-op when the device is healthy or DRED is off.
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

// Thread-safe accessor - ALWAYS use this instead of direct
// g_Device/g_CommandQueue access
static DX12Context GetDX12Context() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return DX12Context(g_Device.load(), g_CommandQueue.load());
}

static std::atomic<uint64_t> g_FrameIndex{0};

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

static std::atomic<uint64_t> g_StreamlineStartupActivationSwapchainGeneration{0};

static std::atomic<bool> g_PostSLStartupActivationServiceInProgress{false};

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

static void FillFGSessionLegacyStateView(ce::fg_session::DX12LegacyStateView* out);

// IPC ready flag
static bool g_IPCReady = false;

ID3D12Resource* g_DummyBackBuffer = nullptr;

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

// Use pointer to prevent static destructor execution in non-game processes
// (Explorer fix)
DX12Hook* g_dx12HookInstance = nullptr;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_DeviceQueuesMutex;

std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

static std::atomic<bool> g_PiggybackDrawnThisFrame{false};

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

extern "C" {
// NOINLINE: Prevents LTO from inlining into the ECL detour, which would
// allow the compiler to merge vtable reads and optimize away our safety checks.
__attribute__((noinline)) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue) {
    DX12_SetCommandQueueInternal(pQueue, false, nullptr);
}

}  // extern "C" (DX12_SetCommandQueue)

// Helper to ensure global hook instance exists
void EnsureDX12Hook() {
    if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
    }
}

static void FindAndWrapPreExistingSwapchains();

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

void DX12_OnStreamlineFGStateChanged(bool active) {
    const auto visibleRuntimeMode = active ? ce::fg_runtime::RuntimeMode::kDLSSFG : g_FGCompat.GetRuntimeMode();
    const bool visibleFGActive = active ? true : g_FGCompat.IsFGActive();
    HookUpdatePreferredOverlayFGPublicationState(visibleFGActive, visibleRuntimeMode,
                                                 "DX12_OnStreamlineFGStateChanged");

    ce::fg_session::EmitFGEvent(active ? ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate
                                       : ce::fg_session::FGEventKind::kTransitionCooldownComplete,
                                "DX12_OnStreamlineFGStateChanged", nullptr, nullptr,
                                active ? ce::fg_runtime::RuntimeMode::kDLSSFG : g_FGCompat.GetRuntimeMode(), active,
                                HookHasExplicitStreamlineSetOptionsActivation());

    const bool observerOnly = HookOverlayObserverOnlyEnabled();
    const bool observerPolicyOnly = HookOverlayObserverPolicyOnlyEnabled();
    const bool observerStartupPresentOnly = HookOverlayObserverStartupPresentOnlyEnabled();
    if (observerOnly) {
        const auto cleanup =
            ce::streamline_runtime_policy::ResolveObserverOnlyHeuristicCleanupForStreamlineSignalTransition(active);
        if (cleanup.clearRecentTeardownGrace) {
            const int previousHeuristicGrace = dx12_hook_g_SLOffHeuristicGrace.exchange(0, std::memory_order_acq_rel);
            const int previousSwapchainGrace = dx12_hook_g_SLOffSwapchainReinitGrace.exchange(0, std::memory_order_acq_rel);
            if (ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(
                    true, previousHeuristicGrace > 0, previousSwapchainGrace > 0)) {
                HookLogImportant(
                    "DX12: Observer-only Streamline FG ON - cleared stale teardown grace before fresh activation "
                    "(slOffGrace=%d swapchainGrace=%d)",
                    previousHeuristicGrace, previousSwapchainGrace);
            }
        }
        if (cleanup.seedRecentTeardownGrace) {
            dx12_hook_g_SLOffHeuristicGrace.store(600, std::memory_order_release);
            dx12_hook_g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);
        }
        if (cleanup.resetQueueChangeHeuristic) {
            RequestFGDetectionHeuristicReset();
        }
        if (cleanup.clearHeuristicFSR && g_FGCompat.IsHeuristicFSRFGActive()) {
            g_FGCompat.SetHeuristicFSRFGActive(false);
            HookLogImportant("DX12: Observer-only cleared heuristic FSR FG during Streamline %s transition",
                             active ? "ON" : "OFF");
        }
        if (cleanup.clearNvidiaSmoothMotion) {
            g_FGCompat.ClearNvidiaSMState();
        }
        if (active) {
            HookLogImportant(
                observerPolicyOnly
                    ? (observerStartupPresentOnly
                           ? "DX12: Streamline FG ON observed in observer-startup-present-only mode - keeping PostSL "
                             "and special Streamline Present routing passive while preserving startup-policy and "
                             "non-Streamline startup-Present probe state"
                           : "DX12: Streamline FG ON observed in observer-policy-only mode - keeping PostSL/startup "
                             "Present passive while preserving Streamline startup-policy state")
                    : "DX12: Streamline FG ON observed in observer-only mode - skipping PostSL startup routing/state "
                      "mutation");
        } else {
            HookLogImportant(
                observerPolicyOnly
                    ? (observerStartupPresentOnly
                           ? "DX12: Streamline FG OFF observed in observer-startup-present-only mode - keeping PostSL "
                             "and special Streamline Present routing passive while preserving startup-policy and "
                             "non-Streamline startup-Present probe state"
                           : "DX12: Streamline FG OFF observed in observer-policy-only mode - keeping PostSL/startup "
                             "Present passive while preserving Streamline startup-policy state")
                    : "DX12: Streamline FG OFF observed in observer-only mode - keeping PostSL disabled and clearing "
                      "startup state");
        }
        EnsurePostSLDisabledForObserverOnly(
            "DX12: observer-only mode",
            ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(observerOnly,
                                                                                                   observerPolicyOnly));
        return;
    }

    if (active) {
        dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
        const int previousHeuristicGrace = dx12_hook_g_SLOffHeuristicGrace.exchange(0, std::memory_order_acq_rel);
        const int previousSwapchainGrace = dx12_hook_g_SLOffSwapchainReinitGrace.exchange(0, std::memory_order_acq_rel);
        if (ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(
                true, previousHeuristicGrace > 0, previousSwapchainGrace > 0)) {
            HookLogImportant(
                "DX12: Streamline FG ON — cleared stale teardown grace before fresh activation "
                "(slOffGrace=%d swapchainGrace=%d)",
                previousHeuristicGrace, previousSwapchainGrace);
        }

        const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        const ULONGLONG startupWindowRemainingMs =
            startupWindowActive
                ? (DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire) -
                   GetTickCount64())
                : 0;
        const bool startupTopLevelPresentConsumed =
            DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
        const bool wrapperProgressObserved =
            dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.load(std::memory_order_acquire) > 0;
        HookLogImportant(
            "DX12: Streamline FG ON — GetState transition STARTING "
            "(startupWindowActive=%d startupRemaining=%lldms consumed=%d wrapperProgress=%d)",
            startupWindowActive ? 1 : 0, (long long)startupWindowRemainingMs, startupTopLevelPresentConsumed ? 1 : 0,
            wrapperProgressObserved ? 1 : 0);

        const bool callbackAlreadyInstalled =
            DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr;
        const bool resumeConfirmedPostSLFromKeepAlive =
            ce::dx12_overlay_policy::ShouldResumeConfirmedPostSLFromKeepAliveOnStreamlineOn(
                dx12_hook_g_PostSLExplicitOffKeepAlive.exchange(false, std::memory_order_acq_rel),
                dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire));
        dx12_hook_g_PostSLWarmResumePreservationPending.store(callbackAlreadyInstalled && resumeConfirmedPostSLFromKeepAlive,
                                                    std::memory_order_release);

        if (callbackAlreadyInstalled && resumeConfirmedPostSLFromKeepAlive) {
            // Suspend -> resume cycle bridged by the make-before-break
            // keep-alive: PostSL stayed confirmed-and-renderable the whole
            // time, so this is a RESUME of a continuously-live path, not a
            // cold start. No synthetic-startup pending dance, no countdown
            // re-arm, no lifecycle reset — the first re-entrant present after
            // the resume renders immediately.
            dx12_hook_g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
            dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
            dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
            // Keep churn protection armed: a quick OFF right after this resume
            // must take the churn path, not a full teardown.
            DXGIShared::ArmStreamlineStartupTransitionWindow();
            HookLogImportant(
                "DX12: Streamline FG ON — warm PostSL resume from make-before-break keep-alive "
                "(confirmed rendering preserved, no countdown/warm-up re-arm)");
        } else if (callbackAlreadyInstalled) {
            dx12_hook_g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
            dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
            dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
            dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
            dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
            dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
            int cooldownLeft = dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_acquire);
            while (cooldownLeft < 60 && !dx12_hook_g_PostSLCooldownRemaining.compare_exchange_weak(
                                            cooldownLeft, 60, std::memory_order_acq_rel, std::memory_order_acquire)) {}
            HookLogImportant(
                "DX12: Streamline FG ON — re-enabling dormant PostSL callback for startup routing "
                "(churn re-activation, cooldown=%d)",
                dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_relaxed));
            // Re-arm the startup transition window: churn re-activation means
            // DLSS FG is still in its initialization dance (game bouncing
            // ON/OFF/ON).  The original window from the first ON may have expired,
            // leaving no protection for OFF signals during this new cycle.
            DXGIShared::ArmStreamlineStartupTransitionWindow();
            HookLogImportant("DX12: Streamline FG ON churn — re-armed startup transition window");
            ResetPostSLLifecycleForTransition("DX12: Streamline FG ON churn re-activation", true);
        } else {
            SetPostSLCallbackInstalled(true, "DX12: Streamline FG ON");
            HookLogImportant("DX12: Streamline FG ON — pre-armed PostSL callback for startup routing");
            int cooldownLeft = dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_acquire);
            while (cooldownLeft < 60 && !dx12_hook_g_PostSLCooldownRemaining.compare_exchange_weak(
                                            cooldownLeft, 60, std::memory_order_acq_rel, std::memory_order_acquire)) {}
            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
            dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
            dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
            dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
            dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
            dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
            ResetPostSLLifecycleForTransition("DX12: Streamline FG ON transition", true);
        }
        if (dx12_hook_g_HadFSRFGPhase) {
            ID3D12CommandQueue* staleScQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                const bool streamlineStartupHandoffPending =
                    DXGIShared::g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
                const bool explicitSetOptionsActivation = HookHasExplicitStreamlineSetOptionsActivation();
                ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
                    explicitSetOptionsActivation, false, "fresh Streamline active edge");
                if (ce::dx12_overlay_policy::ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(
                        dx12_hook_g_HadFSRFGPhase, dx12_hook_g_SwapchainQueue != nullptr,
                        dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue,
                        streamlineStartupHandoffPending, resumeConfirmedPostSLFromKeepAlive)) {
                    staleScQueue = dx12_hook_g_SwapchainQueue;
                    dx12_hook_g_SwapchainQueue = nullptr;
                    dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                    dx12_hook_g_SwapchainQueueCaptureTime = 0;
                    dx12_hook_g_FGRuntimeOwnsSwapchain = false;
                    DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                    if (g_FGCompat.IsFSRFGApiActive()) {
                        SetNativeFSRStartupConfigureArmingPending(false,
                                                                  "Streamline FG comeback cleared FSR ownership");
                        ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
                            "Streamline FG comeback cleared FSR ownership");
                        g_FGCompat.SetFSRFGActive(false);
                        g_FGCompat.SetFSRFGMultiplier(0);
                        ResetAuthoritativeFSRRealFrameOnlyStreak();
                    }
                    HookLogImportant(
                        "DX12: Streamline FG ON after FSR — cleared stale FSR swapchain queue %p (origGame=%p) "
                        "to prevent DEVICE_REMOVED on FSR→DLSS transition",
                        staleScQueue, dx12_hook_g_OriginalGameQueue);
                } else if (dx12_hook_g_SwapchainQueue && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue &&
                           streamlineStartupHandoffPending) {
                    if (ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
                            dx12_hook_g_HadFSRFGPhase, dx12_hook_g_SwapchainQueue != nullptr,
                            dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue,
                            streamlineStartupHandoffPending, dx12_hook_g_PostSLLastWorkingQueue != nullptr,
                            dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_SwapchainQueue == dx12_hook_g_PostSLLastWorkingQueue)) {
                        HookLogImportant(
                            "DX12: Streamline FG ON after FSR — cleared stale PostSL lastWorking queue %p because "
                            "fresh Streamline handoff moved to new scQueue %p (origGame=%p)",
                            dx12_hook_g_PostSLLastWorkingQueue, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
                        SetPostSLLastWorkingQueue(nullptr);
                    }
                    HookLogImportant(
                        "DX12: Streamline FG ON after FSR — preserving freshly handed-off Streamline swapchain queue "
                        "%p "
                        "during active startup handoff (origGame=%p)",
                        dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
                }
            }
            if (staleScQueue) {
                staleScQueue->Release();
            }
        }

        ID3D12CommandQueue* resumeSwapchainQueue = nullptr;
        ID3D12CommandQueue* resumeLastWorkingQueue = nullptr;
        ID3D12CommandQueue* resumeOriginalGameQueue = nullptr;
        ID3D12CommandQueue* resumeCommandQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            resumeSwapchainQueue = dx12_hook_g_SwapchainQueue;
            resumeLastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
            resumeOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
            resumeCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        }
        if (ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
                dx12_hook_g_HadFSRFGPhase, resumeLastWorkingQueue != nullptr, resumeSwapchainQueue != nullptr,
                resumeSwapchainQueue != nullptr && resumeSwapchainQueue == resumeLastWorkingQueue,
                dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.load(std::memory_order_acquire),
                resumeOriginalGameQueue != nullptr,
                resumeOriginalGameQueue != nullptr && resumeCommandQueue == resumeOriginalGameQueue)) {
            DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.store(true, std::memory_order_release);
            HookLogImportant(
                "DX12: Streamline FG ON — seeded startup bootstrap as already consumed for confirmed PostSL resume "
                "(scQueue=%p lastWorking=%p clearedStaleNoFG=%d origGame=%p cmdQ=%p)",
                resumeSwapchainQueue, resumeLastWorkingQueue,
                dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.load(std::memory_order_relaxed) ? 1 : 0,
                resumeOriginalGameQueue, resumeCommandQueue);
        }
        dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(false, std::memory_order_release);
        return;
    }
    if (!active) {
        dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
    }

    // Make-before-break: a CONFIRMED PostSL path stays armed-and-rendering
    // across the explicit OFF edge — the proxy swapchain keeps presenting
    // after slDLSSGSetOptions(off) (menus/suspension) and tearing PostSL down
    // here is what blanks those presents until an authoritative normal
    // swapchain/queue return. Never while an FSR/native-FG takeover is in play
    // (the quiesce invariant wins).
    dx12_hook_g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
    const bool keepConfirmedPostSLAliveAcrossOff =
        ce::dx12_overlay_policy::ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(
            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire), g_FGCompat.IsFSRFGApiActive(),
            HookHasRuntimeOwnedNativeFGPresentPath(), ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup());
    if (keepConfirmedPostSLAliveAcrossOff) {
        dx12_hook_g_PostSLExplicitOffKeepAlive.store(true, std::memory_order_release);
        HookLogImportant(
            "DX12: Streamline FG OFF — keeping confirmed PostSL armed-and-rendering until an authoritative "
            "normal swapchain/queue return (make-before-break keep-alive)");
    } else {
        dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
    }

    const bool inStartupChurnWindow = DXGIShared::IsStreamlineStartupTransitionWindowActive();

    if (inStartupChurnWindow) {
        if (!keepConfirmedPostSLAliveAcrossOff) {
            dx12_hook_g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);
        }
        HookLogImportant(
            "DX12: Streamline FG OFF during startup transition — keeping PostSL callback %s "
            "(churn suppression, epoch=%u keepAlive=%d)",
            keepConfirmedPostSLAliveAcrossOff ? "armed for keep-alive rendering" : "dormant",
            dx12_hook_g_PostSLLifecycleEpoch.load(std::memory_order_acquire), keepConfirmedPostSLAliveAcrossOff ? 1 : 0);
        // Drop the AddRef'd startup-activation swapchain even on the churn
        // path: pinning it costs nothing on a quick re-ON (every startup-route
        // present re-retains it), but if the game proceeds to a full native
        // teardown instead, CE's reference makes the app's
        // CreateSwapChainForHwnd on the same HWND fail E_ACCESSDENIED through
        // all retries (session 20260613_032326: DLSS->OFF stopped the app's
        // main loop with "no swapchain after OFF request").
        ReleaseStreamlineStartupActivationSwapchain("DX12: Streamline FG OFF (startup churn)");
        dx12_hook_g_SLOffHeuristicGrace.store(600, std::memory_order_release);
        RequestFGDetectionHeuristicReset();
        g_FGCompat.SetHeuristicFSRFGActive(false);
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
            ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, 0.0f, 0.0f, 1, "DX12_OnStreamlineFGStateChanged");
        }
        return;
    }


    DXGIShared::ResetStreamlineStartupTransitionState();
    if (!keepConfirmedPostSLAliveAcrossOff) {
        SetPostSLCallbackInstalled(false, "DX12: Streamline FG OFF");
        dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    } else {
        HookLogImportant(
            "DX12: Streamline FG OFF — PostSL callback stays installed for make-before-break keep-alive "
            "(confirmed rendering preserved)");
    }
    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
    dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
    dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: Streamline FG OFF");
    dx12_hook_g_SLOffHeuristicGrace.store(600, std::memory_order_release);
    dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(false, std::memory_order_release);
    if (dx12_hook_g_PostSLLastWorkingQueue) {
        MarkPostSLRecentTeardownActivity("DX12: Streamline FG OFF seeded recent PostSL teardown activity",
                                         dx12_hook_g_PostSLLastWorkingQueue);
    }
    RequestFGDetectionHeuristicReset();
    g_FGCompat.SetHeuristicFSRFGActive(false);
    g_FGCompat.ClearNvidiaSMState();
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
        const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
        ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, 0.0f, 0.0f, 1, "DX12_OnStreamlineFGStateChanged");
    }
    InvalidateAllOverlayCachedFrames();
    dx12_hook_g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);
    if (!keepConfirmedPostSLAliveAcrossOff) {
        // A lifecycle reset would force a fresh reactivation epoch/warm-up;
        // keep-alive must keep the continuously-live path untouched.
        ResetPostSLLifecycleForTransition("DX12: Streamline FG OFF transition", true, true);
    }

    if (dx12_hook_g_HadFSRFGPhase) {
        ID3D12CommandQueue* staleScQueue = nullptr;
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        // Overlay fallback permission must not be reused as a proof that native
        // FSR ownership is stale.  Preserve ownership while FSR/native Present
        // state is still active and let the explicit native-FSR OFF path clear it.
        const bool preserveRuntimeOwnedFSRTakeover =
            ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
                dx12_hook_g_FGRuntimeOwnsSwapchain, false, runtimeMode, g_FGCompat.IsFSRFGApiActive(),
                HookHasRuntimeOwnedNativeFGPresentPath(), false);
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);

            if (preserveRuntimeOwnedFSRTakeover) {
                HookLogImportant(
                    "DX12: Streamline FG OFF overlapped with authoritative/runtime-owned FSR takeover "
                    "(runtime=%s scQueue=%p origGame=%p) — preserving FSR swapchain ownership until native FSR "
                    "emits a stronger off signal",
                    ce::fg_runtime::GetRuntimeModeName(runtimeMode), dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
            } else if (dx12_hook_g_FGRuntimeOwnsSwapchain) {
                dx12_hook_g_FGRuntimeOwnsSwapchain = false;
                DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
                ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
                ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                if (g_FGCompat.IsFSRFGApiActive()) {
                    SetNativeFSRStartupConfigureArmingPending(false, "Streamline FG off cleared FSR ownership");
                    ClearOfficialFFXRuntimeOwnedPresentPathAssumption("Streamline FG off cleared FSR ownership");
                    g_FGCompat.SetFSRFGActive(false);
                    g_FGCompat.SetFSRFGMultiplier(0);
                    ResetAuthoritativeFSRRealFrameOnlyStreak();
                }
                HookLogImportant("DX12: Streamline FG OFF after FSR history — clearing lingering FG runtime ownership");
            }

            if (!preserveRuntimeOwnedFSRTakeover && dx12_hook_g_SwapchainQueue && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue) {
                staleScQueue = dx12_hook_g_SwapchainQueue;
                dx12_hook_g_SwapchainQueue = nullptr;
                dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                dx12_hook_g_SwapchainQueueCaptureTime = 0;
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — releasing stale swapchain queue %p so top-level "
                    "recovery can recapture the live non-FG queue (origGame=%p)",
                    staleScQueue, dx12_hook_g_OriginalGameQueue);
            }
        }

        if (staleScQueue) {
            staleScQueue->Release();
        }

        if (!preserveRuntimeOwnedFSRTakeover) {
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — leaving swapchain queue uncaptured until a live non-FG "
                "queue is observed again (origGame=%p primary=%p cmdQ=%p)",
                dx12_hook_g_OriginalGameQueue, dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire),
                g_CommandQueue.load(std::memory_order_acquire));
        }

        // A confirmed explicit-OFF keep-alive continues rendering on the exact
        // proxy swapchain/queue that succeeded one Present earlier. Preserve its
        // warm RTV/sync objects until a separately proven normal swapchain takes
        // ownership; tearing them down here defeats make-before-break and adds a
        // transition-time GPU drain/rebuild on the same route.
        const bool preserveConfirmedPostSLProxyResources =
            keepConfirmedPostSLAliveAcrossOff && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit &&
            dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire) != nullptr;

        // The post-FSR DLSS path rendered through Streamline/PostSL against a
        // different swapchain topology than the resumed non-FG path. Force a
        // swapchain-level reinit only when no exact confirmed-proxy keep-alive
        // remains; the normal-return proof retires the preserved state later.
        if (!preserveRuntimeOwnedFSRTakeover && dx12_hook_g_State.overlayInit && !preserveConfirmedPostSLProxyResources) {
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — forcing overlay swapchain reinit for non-FG recovery");
            dx12_hook_g_State.overlayInit = false;
            CleanupRTVs();
        }

        if (!preserveRuntimeOwnedFSRTakeover && preserveConfirmedPostSLProxyResources) {
            dx12_hook_g_FGTransitionCooldown.store(0, std::memory_order_release);
            dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
            dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.store(true, std::memory_order_release);
            // The direct state-change callback already owns this OFF edge. Keep
            // the later ProcessFrame outer tracker from replaying a destructive
            // teardown once the native normal route eventually returns.
            dx12_hook_g_OuterTrackedSLFGRunning.store(false, std::memory_order_release);
            dx12_hook_g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release);
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — preserved warm confirmed-PostSL proxy resources "
                "for exact-swapchain keep-alive (proxy=%p queue=%p; no reinit/copy/wait)",
                dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_relaxed), dx12_hook_g_PostSLLastWorkingQueue);
        } else if (!preserveRuntimeOwnedFSRTakeover &&
                   ce::dx12_overlay_policy::ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(
                       dx12_hook_g_HadFSRFGPhase, dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit)) {
            dx12_hook_g_State.syncInit = false;
            dx12_hook_g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
            dx12_hook_g_OuterTrackedSLFGRunning.store(false, std::memory_order_release);
            dx12_hook_g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release);
            auto* oldRealECL = (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
            const ID3D12CommandQueue* currentPrimaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
            const ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
            const bool commandQueueSettledToPrimary =
                currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
            // DLSS-FG SUSPEND with FSR history (slDLSSGSetOptions(off), proxy stays live):
            // when the make-before-break keep-alive is armed this is a CONFIRMED PostSL
            // suspension, not a teardown. PostSL confirmed rendering means the overlay ECL on
            // the runtime-owned SL queue already succeeded many times this epoch (device
            // demonstrably healthy on that exact path) and the proxy keeps presenting, so
            // there is no Streamline teardown for the 60-frame deferral to wait for — it only
            // blanks a provably-live overlay (session 20260613_202646: 60-present /
            // confirmedDuringStreak=1 blank, gate=overlay-backend-uninitialized). The bypass
            // mirrors ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown but is gated on
            // keepConfirmedPostSLAliveAcrossOff (captured at function entry, BEFORE the teardown
            // above nulled the swapchain queue / cleared overlayInit) plus a device-health guard;
            // a real FSR/native-FG takeover or removed device keeps the strict cooldown.
            auto* suspendDevice = g_Device.load(std::memory_order_acquire);
            const bool suspendDeviceRemoved =
                suspendDevice != nullptr && FAILED(suspendDevice->GetDeviceRemovedReason());
            const bool confirmedPostSLSuspensionImmediateReinit =
                keepConfirmedPostSLAliveAcrossOff && !suspendDeviceRemoved;
            const bool useShortPostFSRCooldown = ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase, true);
            const int cooldownFrames =
                confirmedPostSLSuspensionImmediateReinit ? 0 : (useShortPostFSRCooldown ? 15 : 60);
            dx12_hook_g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), cooldownFrames,
                confirmedPostSLSuspensionImmediateReinit || useShortPostFSRCooldown);
            dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                            std::memory_order_release);
            dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
            if (confirmedPostSLSuspensionImmediateReinit) {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — confirmed-PostSL suspension (proxy stays live, "
                    "keep-alive armed), immediate warm overlay reinit instead of 60-frame blank (cooldown=%d)",
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire));
            } else {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — deferring non-FG overlay reinit for %d frames so "
                    "Talos/Streamline teardown can settle before pre-SL resources are rebuilt",
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire));
            }
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — invalidated sync resources for delayed reinit");
            if (ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(
                    commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase)) {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — preserving realECL %p for delayed non-FG "
                    "recovery because cmdQ=%p already settled to primary",
                    oldRealECL, currentCommandQueue);
            } else {
                dx12_hook_g_RealD3D12ECL.store(nullptr, std::memory_order_release);
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — cleared realECL %p for delayed non-FG recovery",
                    oldRealECL);
            }
            dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG = true;
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — enabled offscreen overlay compositing for non-FG "
                "recovery (backbuffer state indeterminate after FG teardown)");
        }
    }

    HookLogImportant("DX12: Streamline FG OFF — seeded heuristic reset/grace (slOffGrace=600)");
    HookLogImportant("DX12: Streamline FG OFF — applied PostSL callback/keep-alive state");
}

// Flush the deferred fence Signal AFTER Present.  The NVIDIA driver stalls the
// GPU when a Signal call sits between the overlay ECL and Present.  By deferring
// the Signal to after Present, the presentation pipeline is uninterrupted.
extern "C" __declspec(dllexport) bool DX12_FlushDeferredSignalWithInfo(
    ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* outInfo) {
    if (outInfo) {
        *outInfo = {};
    }

    UINT64 deferredVal = dx12_hook_g_deferredSignalValue.load(std::memory_order_acquire);
    if (outInfo) {
        outInfo->hadDeferredSignal = (deferredVal != 0);
        outInfo->hasFence = (dx12_hook_g_State.fence != nullptr);
        outInfo->hasFenceEvent = (dx12_hook_g_State.fenceEvent != nullptr);
        outInfo->fence = dx12_hook_g_State.fence;
        outInfo->fenceEvent = dx12_hook_g_State.fenceEvent;
        outInfo->fenceValue = deferredVal;
        outInfo->completedValue = dx12_hook_g_State.fence ? dx12_hook_g_State.fence->GetCompletedValue() : 0;
    }
    if (deferredVal == 0 || !dx12_hook_g_State.fence) {
        return false;
    }

    // Use the queue that actually submitted the overlay ECL.  When FG runtimes
    // create swapchains with their own queue, this may differ from g_CommandQueue.
    ID3D12CommandQueue* q = dx12_hook_g_deferredSignalQueue.load(std::memory_order_acquire);
    if (!q)
        q = g_CommandQueue.load(std::memory_order_acquire);
    if (outInfo) {
        outInfo->queue = q;
    }
    if (!q) {
        return false;
    }

    HRESULT hr = q->Signal(dx12_hook_g_State.fence, deferredVal);
    if (outInfo) {
        outInfo->signalHr = hr;
        outInfo->signalSucceeded = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr)) {
        int allocIdx = dx12_hook_g_deferredSignalAllocIdx.load(std::memory_order_acquire);
        dx12_hook_g_State.currentFenceValue = deferredVal;
        if (allocIdx >= 0 && allocIdx < (int)dx12_hook_g_State.fenceValues.size())
            dx12_hook_g_State.fenceValues[allocIdx] = deferredVal;
        if (outInfo) {
            outInfo->completedValue = dx12_hook_g_State.fence->GetCompletedValue();
        }
    }
    dx12_hook_g_deferredSignalValue.store(0, std::memory_order_release);
    dx12_hook_g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    dx12_hook_g_deferredSignalQueue.store(nullptr, std::memory_order_release);
    return SUCCEEDED(hr);
}

extern "C" __declspec(dllexport) void DX12_FlushDeferredSignal() {
    DX12_FlushDeferredSignalWithInfo(nullptr);
}

static const char* DescribeFocusLossPostPresentFenceSkip(
    const ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext& ctx,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo& info) {
    if (!ctx.isD3D12Swapchain)
        return "non-DX12";
    if (ctx.isFullscreen)
        return "fullscreen";
    if (ctx.processHasForeground)
        return "foreground";
    if (ctx.isIconic)
        return "iconic";
    if (ctx.hasZeroSize)
        return "zero-sized";
    if (!ctx.presentSucceeded)
        return "present-failed";
    if (ctx.presentDeviceLost)
        return "present-device-lost";
    if (ctx.frameGenerationActive)
        return "frame-generation-active";
    if (ctx.runtimeOwnedPresentation)
        return "runtime-owned-presentation";
    if (ctx.usingDedicatedQueue)
        return "dedicated-queue";
    if (!info.hadDeferredSignal)
        return "no-deferred-overlay-signal";
    if (!info.signalSucceeded)
        return "signal-failed";
    if (!info.hasFence)
        return "no-fence";
    if (!info.hasFenceEvent)
        return "no-fence-event";
    if (info.fenceValue == 0)
        return "zero-fence-value";
    return "policy";
}

extern "C" __declspec(dllexport) bool DX12_WaitForFocusLossOverlayFenceAfterPresent(
    const ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext* context,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* flushInfo) {
    if (!context || !flushInfo) {
        return false;
    }

    const auto& ctx = *context;
    const auto& info = *flushInfo;
    const bool shouldWait = ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        ctx.isD3D12Swapchain, ctx.isFullscreen, ctx.processHasForeground, ctx.isIconic, ctx.hasZeroSize,
        ctx.presentSucceeded, ctx.presentDeviceLost, ctx.frameGenerationActive, ctx.runtimeOwnedPresentation,
        ctx.usingDedicatedQueue, info.hadDeferredSignal, info.signalSucceeded, info.hasFence, info.hasFenceEvent,
        info.fenceValue);

    if (!shouldWait) {
        if (!ctx.processHasForeground || info.hadDeferredSignal) {
            static std::atomic<int> s_focusFenceSkipLog{0};
            const int logCount = s_focusFenceSkipLog.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 24 || (logCount % 1000) == 0) {
                HookLog(
                    "DX12: Post-Present focus-loss overlay fence wait skipped (%s present=%s#%d "
                    "fg=%p/%lu game=%p/%lu sync=%u flags=0x%08X presentHr=0x%08X "
                    "deferred=%d signal=%d signalHr=0x%08X fence=%p event=%p value=%llu queue=%p)",
                    DescribeFocusLossPostPresentFenceSkip(ctx, info), ctx.presentName ? ctx.presentName : "Present",
                    ctx.callCount, ctx.foregroundWindow, ctx.foregroundPid, ctx.gameWindow, ctx.processId,
                    ctx.syncInterval, ctx.presentFlags, (unsigned)ctx.presentHr, info.hadDeferredSignal ? 1 : 0,
                    info.signalSucceeded ? 1 : 0, (unsigned)info.signalHr, info.fence, info.fenceEvent,
                    (unsigned long long)info.fenceValue, info.queue);
            }
        }
        return false;
    }

    ID3D12Fence* fence = info.fence;
    HANDLE fenceEvent = info.fenceEvent;
    UINT64 completedValue = fence->GetCompletedValue();
    if (completedValue >= info.fenceValue) {
        ClearFocusLossPendingOverlayFence("post-Present wait already complete", info.fenceValue, completedValue);
        static std::atomic<int> s_focusFenceAlreadyLog{0};
        const int logCount = s_focusFenceAlreadyLog.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 24 || (logCount % 300) == 0) {
            HookLog(
                "DX12: Post-Present focus-loss overlay fence already complete "
                "(present=%s#%d fence=%llu completed=%llu queue=%p fg=%p/%lu sync=%u flags=0x%08X)",
                ctx.presentName ? ctx.presentName : "Present", ctx.callCount, (unsigned long long)info.fenceValue,
                (unsigned long long)completedValue, info.queue, ctx.foregroundWindow, ctx.foregroundPid,
                ctx.syncInterval, ctx.presentFlags);
        }
        return true;
    }

    HRESULT setHr = fence->SetEventOnCompletion(info.fenceValue, fenceEvent);
    if (FAILED(setHr)) {
        dx12_hook_g_FocusLossPendingOverlayFenceValue.store(info.fenceValue, std::memory_order_release);
        static std::atomic<int> s_focusFenceSetEventFailLog{0};
        const int logCount = s_focusFenceSetEventFailLog.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 24 || (logCount % 1000) == 0) {
            HookLogImportant(
                "DX12: Post-Present focus-loss overlay fence wait could not arm event "
                "(hr=0x%08X present=%s#%d fence=%llu completed=%llu event=%p queue=%p); "
                "holding future unfocused overlay draws until completion",
                (unsigned)setHr, ctx.presentName ? ctx.presentName : "Present", ctx.callCount,
                (unsigned long long)info.fenceValue, (unsigned long long)completedValue, fenceEvent, info.queue);
        }
        return false;
    }

    constexpr DWORD kFocusLossOverlayFenceWaitMs = 16;
    DWORD waitResult = WaitForSingleObject(fenceEvent, kFocusLossOverlayFenceWaitMs);
    DWORD waitLastError = (waitResult == WAIT_FAILED) ? GetLastError() : 0;
    completedValue = fence->GetCompletedValue();
    const bool completed = completedValue >= info.fenceValue || waitResult == WAIT_OBJECT_0;
    if (completed) {
        ClearFocusLossPendingOverlayFence("post-Present wait completed", info.fenceValue, completedValue);
    } else {
        dx12_hook_g_FocusLossPendingOverlayFenceValue.store(info.fenceValue, std::memory_order_release);
    }

    static std::atomic<int> s_focusFenceWaitLog{0};
    const int logCount = s_focusFenceWaitLog.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 60 || !completed || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Post-Present focus-loss overlay fence wait result=%s(0x%08lX) "
            "(present=%s#%d fence=%llu completed=%llu queue=%p fg=%p/%lu game=%p/%lu "
            "sync=%u flags=0x%08X presentHr=0x%08X timeoutMs=%lu gle=%lu pendingHold=%d)",
            DX12WaitResultName(waitResult), waitResult, ctx.presentName ? ctx.presentName : "Present", ctx.callCount,
            (unsigned long long)info.fenceValue, (unsigned long long)completedValue, info.queue, ctx.foregroundWindow,
            ctx.foregroundPid, ctx.gameWindow, ctx.processId, ctx.syncInterval, ctx.presentFlags,
            (unsigned)ctx.presentHr, kFocusLossOverlayFenceWaitMs, waitLastError, completed ? 0 : 1);
    }

    return completed;
}

static bool ShouldLogOverlayCompletionWaitDiagnostic(std::atomic<int>& counter) {
    const int n = counter.fetch_add(1, std::memory_order_relaxed);
    return n < 24 || (n % 1000) == 0;
}

// External function for swapchain wrapper to wait for overlay completion before
// Present
extern "C" __declspec(dllexport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pGameQueue) {
    (void)pGameQueue;
    if (!dx12_hook_g_State.fence) {
        static std::atomic<int> s_noFenceLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_noFenceLog)) {
            HookLog("DX12: Overlay completion wait skipped (no fence; event=%p currentFence=%llu)", dx12_hook_g_State.fenceEvent,
                    (unsigned long long)dx12_hook_g_State.currentFenceValue);
        }
        return;
    }

    UINT64 fenceValueToWait = dx12_hook_g_State.currentFenceValue;
    if (fenceValueToWait == 0) {
        static std::atomic<int> s_noFenceValueLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_noFenceValueLog)) {
            HookLog("DX12: Overlay completion wait skipped (no signaled fence value; fence=%p event=%p)", dx12_hook_g_State.fence,
                    dx12_hook_g_State.fenceEvent);
        }
        return;
    }

    // Check ShouldUseDedicatedOverlayQueue() (FG active) instead of just queue
    // existence, since the queue is now kept alive across FG mode switches.
    const bool usingDedicatedQueue = ShouldUseDedicatedOverlayQueue() && (dx12_hook_g_State.overlayQueue != nullptr);
    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    bool processHasForeground = true;
    if (!usingDedicatedQueue) {
        foregroundWindow = GetForegroundWindow();
        processHasForeground = false;
        if (foregroundWindow) {
            GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
            processHasForeground = (foregroundPid == GetCurrentProcessId());
        }
    }

    const char* overlayModule = nullptr;
    if (!usingDedicatedQueue && processHasForeground) {
        overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    if (!ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(dx12_hook_g_State.fenceEvent != nullptr, usingDedicatedQueue,
                                                                 overlayModule != nullptr, runtimeMode,
                                                                 processHasForeground)) {
        static std::atomic<int> s_policySkipLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_policySkipLog)) {
            HookLog(
                "DX12: Overlay completion wait skipped by policy "
                "(event=%p dedicated=%d overlayModule=%s runtime=%s foreground=%d fg=%p/%lu fence=%llu)",
                dx12_hook_g_State.fenceEvent, usingDedicatedQueue ? 1 : 0, overlayModule ? overlayModule : "none",
                ce::fg_runtime::GetRuntimeModeName(runtimeMode), processHasForeground ? 1 : 0, foregroundWindow,
                foregroundPid, (unsigned long long)fenceValueToWait);
        }
        return;
    }

    const char* waitMode = usingDedicatedQueue       ? "dedicated-queue"
                           : (!processHasForeground) ? "focus-loss"
                                                     : (overlayModule ? overlayModule : "single-queue");
    const bool focusLossMode = !usingDedicatedQueue && !processHasForeground;

    {
        UINT64 completedVal = dx12_hook_g_State.fence->GetCompletedValue();
        if (completedVal >= fenceValueToWait) {
            if (focusLossMode) {
                ClearFocusLossPendingOverlayFence("pre-Present wait already complete", fenceValueToWait, completedVal);
            }
            static std::atomic<int> s_fenceAlreadyCompleteLog{0};
            if (s_fenceAlreadyCompleteLog.fetch_add(1, std::memory_order_relaxed) < 50) {
                HookLog("DX12: Overlay fence already complete (fence=%llu, completed=%llu, mode=%s)",
                        (unsigned long long)fenceValueToWait, (unsigned long long)completedVal, waitMode);
            }
            return;
        }
    }

    HRESULT setHr = dx12_hook_g_State.fence->SetEventOnCompletion(fenceValueToWait, dx12_hook_g_State.fenceEvent);
    if (FAILED(setHr)) {
        if (focusLossMode) {
            dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValueToWait, std::memory_order_release);
        }
        static std::atomic<int> s_setEventFailureLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_setEventFailureLog)) {
            HookLog(
                "DX12: Overlay completion wait skipped (SetEventOnCompletion failed hr=0x%08X fence=%llu event=%p "
                "mode=%s pendingHold=%d)",
                setHr, (unsigned long long)fenceValueToWait, dx12_hook_g_State.fenceEvent, waitMode, focusLossMode ? 1 : 0);
        }
        return;
    }

    static std::atomic<int> s_waitLogCount{0};
    constexpr DWORD kCompatWaitTimeoutMs = 16;
    DWORD waitHr = WaitForSingleObject(dx12_hook_g_State.fenceEvent, kCompatWaitTimeoutMs);
    if (waitHr == WAIT_TIMEOUT) {
        if (focusLossMode) {
            dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValueToWait, std::memory_order_release);
        }
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
            HookLog("DX12: Overlay completion wait timed out for %s mode (fence=%llu pendingHold=%d)", waitMode,
                    (unsigned long long)fenceValueToWait, focusLossMode ? 1 : 0);
        }
    } else if (waitHr == WAIT_OBJECT_0) {
        if (focusLossMode) {
            UINT64 completedVal = dx12_hook_g_State.fence->GetCompletedValue();
            ClearFocusLossPendingOverlayFence("pre-Present wait completed", fenceValueToWait, completedVal);
        }
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
            HookLog("DX12: Overlay completion wait finished for %s mode (fence=%llu)", waitMode,
                    (unsigned long long)fenceValueToWait);
        }
    } else {
        if (focusLossMode) {
            dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValueToWait, std::memory_order_release);
        }
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
            HookLog("DX12: Overlay completion wait returned result=%lu for %s mode (fence=%llu pendingHold=%d)", waitHr,
                    waitMode, (unsigned long long)fenceValueToWait, focusLossMode ? 1 : 0);
        }
    }
}

static const GUID SKID_D3D12SwapChainBufferBitmap = {
    0xbc53df3b, 0x956f, 0x47db, {0xa6, 0x53, 0x5, 0xd7, 0xb8, 0x71, 0x53, 0x38}};

void DX12Hook::Shutdown() {
    LogOverlayCoverageSummary("shutdown summary");
    ce::dx12_sampler_hooks::LogSummary("shutdown");
    HookLogImportant(
        "DX12: Shutdown — cleaning up FFX state (runtime=%s overlayInit=%d syncInit=%d "
        "fgOwned=%d nativeFGPath=%d progressResolved=%d callbackBridges=%zu)",
        ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), dx12_hook_g_State.overlayInit ? 1 : 0,
        dx12_hook_g_State.syncInit ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
        dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire) ? 1 : 0,
        dx12_hook_g_FFXPresentCallbackBridges.size());

    // Stop new proxy prework and drain callbacks that entered before quiescing before releasing any queue,
    // renderer, or cached UI-resource state they can touch.
    DX12_RemoveFFXProxyPresentHook("DX12 shutdown");

    // Force-clean FFX present callback state before D3D12 teardown, so CE
    // does not hold references that could stall the game's DX12 shutdown.
    DX12_UnregisterNativeFSRSwapchainPresentationQueue(nullptr, "DX12 shutdown");
    ce::dx12_ffx_suspend_overlay::Shutdown("DX12 shutdown");
    ce::dx12_streamline_ui_overlay::Shutdown("DX12 shutdown");
    ResetFFXPresentCallbackOverlayBackend("DX12: Shutdown");
    {
        std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
        dx12_hook_g_FFXPresentCallbackBridges.clear();
    }
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption("DX12: Shutdown");
    ResetFFXPresentCallbackFirstStallDetection();
    dx12_hook_g_FFXPresentCallbackBridgeExpected.store(false, std::memory_order_release);
    dx12_hook_g_NativeFSRInternalNoCallbackComposition.store(false, std::memory_order_release);
    dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
    dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.store(nullptr, std::memory_order_release);
    dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
    g_RenderWatchdog.SetRuntimePresentationMonitor(false);
    dx12_hook_g_OverlaySuppressedSinceMs.store(0, std::memory_order_release);

    // Release overlay completion fence
    {
        ID3D12Fence* fence = dx12_hook_g_OverlayCompletionFence.exchange(nullptr, std::memory_order_acq_rel);
        if (fence) {
            fence->Release();
            HookLog("DX12: Released overlay completion fence");
        }
    }

    CleanupResources();
    CleanupOverlay();
    CleanupRTVs();
    DXGIShared::g_PostSLStartupActivationService.store(nullptr, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: Shutdown");

    // Clean up drain fence/event (used for FSR→DLSS transition)
    if (dx12_hook_g_DrainFence) {
        dx12_hook_g_DrainFence->Release();
        dx12_hook_g_DrainFence = nullptr;
    }
    if (dx12_hook_g_DrainEvent) {
        CloseHandle(dx12_hook_g_DrainEvent);
        dx12_hook_g_DrainEvent = nullptr;
    }
    dx12_hook_g_DrainFenceValue = 0;
    dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG = false;

    // Clean up prerender fences/events
    for (auto* fence : dx12_hook_g_PrerenderFences) {
        if (fence)
            fence->Release();
    }
    dx12_hook_g_PrerenderFences.clear();
    for (auto event : dx12_hook_g_PrerenderEvents) {
        if (event)
            CloseHandle(event);
    }
    dx12_hook_g_PrerenderEvents.clear();
    dx12_hook_g_PrerenderFrameIndex = 0;
    if (dx12_hook_g_PrerenderDevice) {
        dx12_hook_g_PrerenderDevice->Release();
        dx12_hook_g_PrerenderDevice = nullptr;
    }
    if (dx12_hook_g_PrerenderQueue) {
        dx12_hook_g_PrerenderQueue->Release();
        dx12_hook_g_PrerenderQueue = nullptr;
    }

    // Clean up descriptor-free backend
    ShutdownDescFreeBackend("DX12Hook::Shutdown", true);

    {
        std::lock_guard<std::recursive_mutex> lock(g_DeviceQueuesMutex);
        for (auto& pair : g_DeviceQueues)
            if (pair.second)
                pair.second->Release();
        g_DeviceQueues.clear();
    }
    if (dx12_hook_g_SwapchainQueue) {
        dx12_hook_g_SwapchainQueue->Release();
        dx12_hook_g_SwapchainQueue = nullptr;
        dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
    }
    // Disable post-SL overlay callback before tearing down D3D12 resources.
    SetPostSLCallbackInstalled(false, "DX12: Shutdown");
    WaitForInFlightPostSLCallbacks("DX12: Shutdown");
    dx12_hook_g_PostSLDeferredQueueCleanupPending.store(false, std::memory_order_release);
    ClearPostSLQueues("DX12: Shutdown");
    ClearPostSLPinnedSLWrapperQueue("DX12: Shutdown");
    SetPostSLLastWorkingQueue(nullptr);
    if (auto* deferredLockedQueue = dx12_hook_g_DeferredPostSLLockedQueueRelease.exchange(nullptr, std::memory_order_acq_rel)) {
        deferredLockedQueue->Release();
    }
    if (g_CommandQueue.load()) {
        g_CommandQueue.load()->Release();
        g_CommandQueue.store(nullptr);
    }
    if (auto* deferredCommandQueue = dx12_hook_g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel)) {
        deferredCommandQueue->Release();
    }
    // The authoritative-baseline pointer is non-owning and backed by
    // g_OriginalGameQueue. Clear it before releasing that retained queue.
    dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.store(nullptr, std::memory_order_release);
    dx12_hook_g_ResetQueueChangeHeuristic.store(false, std::memory_order_release);
    dx12_hook_g_ResetECLPatternHeuristic.store(false, std::memory_order_release);
    if (dx12_hook_g_OriginalGameQueue) {
        dx12_hook_g_OriginalGameQueue->Release();
        dx12_hook_g_OriginalGameQueue = nullptr;
    }
    if (dx12_hook_g_PreFGGameQueue) {
        dx12_hook_g_PreFGGameQueue->Release();
        dx12_hook_g_PreFGGameQueue = nullptr;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_ExecuteCommandListsHookStateMutex);
        dx12_hook_g_ExecuteCommandListsOriginalByVTable.clear();
        oExecuteCommandLists = nullptr;
    }
    dx12_hook_g_LastExecuteCommandListsVTable.store(nullptr, std::memory_order_release);
    dx12_hook_g_LastExecuteCommandListsOriginal.store(nullptr, std::memory_order_release);
    if (g_Device.load()) {
        g_Device.load()->Release();
        g_Device.store(nullptr);
    }
    // g_LastSwapChain is a raw (non-AddRef'd) pointer — do NOT Release
    if (dx12_hook_g_LastSwapChain) {
        dx12_hook_g_LastSwapChain = nullptr;
    }
    dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_HadSuccessfulPostSLPhase.store(false, std::memory_order_release);
    dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_LastKnownSwapchainHDRStateValid.store(false, std::memory_order_release);
    dx12_hook_g_LastKnownSwapchainIsHDR.store(false, std::memory_order_release);
    dx12_hook_g_LastKnownSwapchainColorSpace.store(-1, std::memory_order_release);
    if (dx12_hook_g_SharedCaptureD3D12.IsActive()) {
        std::lock_guard<std::recursive_mutex> capLock(dx12_hook_g_DX12CaptureMutex);
        dx12_hook_g_SharedCaptureD3D12.Reset();
    }
    g_IPCReady = false;
}

void DX12Hook::OnHostDisconnect() {
    g_IPCReady = false;
}

void DX12Hook::TrackResource(IUnknown* res) {
    if (!res)
        return;
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    res->AddRef();
    trackedResources.push_back(res);
}

void DX12Hook::CleanupResources() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    for (auto* res : trackedResources)
        if (res)
            res->Release();
    trackedResources.clear();
}
