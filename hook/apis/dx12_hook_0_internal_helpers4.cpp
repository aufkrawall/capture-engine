#include "dx12_hook_internal.h"


bool ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(const char** reason) {
if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
    if (reason) {
        *reason = "protected official FFX startup";
    }
    return true;
}

const auto runtimeMode = g_FGCompat.GetRuntimeMode();
const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
const bool nativeFSRInternalNoCallbackComposition =
    dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
const bool ffxStalled = IsFFXPresentCallbackStalled();
const bool explicitNativeFSROffPending =
    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
const bool ffxStallAllowsNormalOverlay =
    ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(ffxStalled);
const uint32_t streamlineNoFGPresentCount =
    dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount.load(std::memory_order_acquire);
if (ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
        dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, streamlineNoFGPresentCount,
        dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents)) {
    if (reason) {
        *reason = "fresh runtime-owned Streamline no-FG swapchain";
    }
    static std::atomic<int> s_freshStreamlineNoFGSkipLogCount{0};
    const int logCount = s_freshStreamlineNoFGSkipLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 16 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Deferring separate overlay GPU work on fresh runtime-owned Streamline no-FG swapchain "
            "(presentCount=%u settlePresents=%u scQueue=%p origGame=%p cmdQ=%p log=%d)",
            streamlineNoFGPresentCount, dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents, dx12_hook_g_SwapchainQueue,
            dx12_hook_g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
    }
    return true;
}

const bool skip = ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
    dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, authoritativeFSRActive,
    runtimeOwnedNativeFGPresentPath, ffxStallAllowsNormalOverlay, nativeFSRInternalNoCallbackComposition,
    DX12_IsFFXUiResourceCachedForBundle(), DX12_IsLiveSwapchainQueueOriginalGameQueue(),
    explicitNativeFSROffPending);
if (!skip) {
    // Normal (non-override) path says don't skip — reset the suppression
    // timer so the next suppression episode gets a fresh 2-second window.
    dx12_hook_g_OverlaySuppressedSinceMs.store(0, std::memory_order_release);

    if (ffxStallAllowsNormalOverlay) {
        static std::atomic<int> s_stallFallbackLogCount{0};
        if (s_stallFallbackLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
            const ULONGLONG ownedSince = dx12_hook_g_FGRuntimeOwnsSwapchainSince;
            const ULONGLONG assumedSince =
                dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
            const ProgressResolvedOfficialFFXOverlayFallbackProof progressProof =
                EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
            const bool directFFXApiConfirmation = g_FGCompat.HasDirectFFXApiConfirmation();
            ULONGLONG currentFFXProofSince = dx12_hook_g_SwapchainQueueCaptureTime;
            if (assumedSince > currentFFXProofSince) {
                currentFFXProofSince = assumedSince;
            }
            const bool currentFFXPresentCallbackProof = ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(
                lastCallback, dx12_hook_g_SwapchainQueueCaptureTime, assumedSince);
            HookLogImportant(
                "DX12: Native FSR fallback proof allows normal overlay rendering "
                "(lastCallback=%llu ownedFor=%llums ffxStalled=%d "
                "progressAssumedFor=%llums proofSince=%llu directFFX=%d explicitNativeOff=%d "
                "currentCallbackProof=%d stableProof=%d stableFor=%llums sameQueue=%d deviceHr=0x%08X "
                "internalNoCallback=%d) "
                "— using the game Present path while native FSR presentation is suspended or its callback is quiet",
                lastCallback, ownedSince ? (GetTickCount64() - ownedSince) : 0, ffxStalled ? 1 : 0,
                assumedSince ? (GetTickCount64() - assumedSince) : 0,
                static_cast<unsigned long long>(currentFFXProofSince), directFFXApiConfirmation ? 1 : 0,
                explicitNativeFSROffPending ? 1 : 0, currentFFXPresentCallbackProof ? 1 : 0,
                progressProof.proof ? 1 : 0, progressProof.stableMs,
                progressProof.swapchainQueueMatchesOriginalGameQueue ? 1 : 0,
                static_cast<unsigned>(progressProof.deviceHr), nativeFSRInternalNoCallbackComposition ? 1 : 0);
        }
    } else if (nativeFSRInternalNoCallbackComposition) {
        static std::atomic<int> s_internalNoCallbackRouteLogCount{0};
        const int logCount = s_internalNoCallbackRouteLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            // This branch (skip=false under no-callback composition) is reached ONLY when the bundle is
            // truly gone: a STALE latch after the game recreated its own native swapchain (live queue back
            // on origGame → backbuffer on the game's own queue is safe), or no UI resource was ever cached.
            // A no-callback SUSPENSION does NOT reach here — it keeps skip=true (the bundle composite draws
            // on CE's own fenced queue; the backbuffer submit would stall the app to ~1 fps, session
            // 20260703_210021).
            const bool uiCached = DX12_IsFFXUiResourceCachedForBundle();
            const bool liveIsOrigGame = DX12_IsLiveSwapchainQueueOriginalGameQueue();
            const char* fallbackReason = liveIsOrigGame ? "stale no-callback latch (live queue back on origGame)"
                                         : !uiCached    ? "no UI resource registered"
                                                        : "no-callback composition";
            HookLogImportant(
                "DX12: Native FSR no-callback composition — allowing normal/backbuffer overlay rendering "
                "(%s) (runtime=%s apiFSR=%d nativeFGPath=%d runtimeOwns=%d explicitNativeOff=%d uiCached=%d "
                "liveQueueIsOrigGame=%d scQueue=%p origGame=%p cmdQ=%p log=%d)",
                fallbackReason, ce::fg_runtime::GetRuntimeModeName(runtimeMode), authoritativeFSRActive ? 1 : 0,
                runtimeOwnedNativeFGPresentPath ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0,
                explicitNativeFSROffPending ? 1 : 0, uiCached ? 1 : 0, liveIsOrigGame ? 1 : 0, dx12_hook_g_SwapchainQueue,
                dx12_hook_g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
        }
    }
    if (reason) {
        *reason = nullptr;
    }
    return false;
}

if (ffxStalled && !ffxStallAllowsNormalOverlay) {
    LogSuppressedFFXPresentCallbackStallNormalOverlayFallback();
}

if (skip && explicitNativeFSROffPending && runtimeOwnedNativeFGPresentPath) {
    static std::atomic<int> s_retainedNativeFSRSuspendSkipLogCount{0};
    const int logCount = s_retainedNativeFSRSuspendSkipLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
        const ULONGLONG now = GetTickCount64();
        HookLogImportant(
            "DX12: Keeping separate overlay GPU work suppressed during native-FSR suspension; retained FFX "
            "present-callback bridge remains authoritative (runtime=%s callbackEver=%d lastCallbackAge=%llums "
            "ffxStalled=%d fallbackAllowed=%d log=%d)",
            ce::fg_runtime::GetRuntimeModeName(runtimeMode), lastCallback != 0 ? 1 : 0,
            lastCallback && now >= lastCallback ? (now - lastCallback) : 0, ffxStalled ? 1 : 0,
            ffxStallAllowsNormalOverlay ? 1 : 0, logCount + 1);
    }
}

// 2-second max overlay suspension enforcement. Ordinary transition stalls
// can use this as a visibility backstop, but native FSR without direct
// ffxConfigure/callback proof must stay suppressed. GTA Enhanced removes
// the device on the first normal overlay ECL in that unproven state.
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG suppressedSince = dx12_hook_g_OverlaySuppressedSinceMs.load(std::memory_order_acquire);
    if (suppressedSince == 0) {
        dx12_hook_g_OverlaySuppressedSinceMs.store(now, std::memory_order_release);
        suppressedSince = now;
    }
    constexpr ULONGLONG kMaxOverlaySuppressionMs = 2000;
    if (now >= suppressedSince && (now - suppressedSince) >= kMaxOverlaySuppressionMs) {
        const bool nativeFSRActive = authoritativeFSRActive || ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode);
        const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
        const bool ffxPresentCallbackEverFired = lastCallback != 0;
        const bool timeoutOverrideAllowed =
            ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
                runtimeOwnedNativeFGPresentPath, nativeFSRActive, ffxStalled, ffxStallAllowsNormalOverlay);
        if (!timeoutOverrideAllowed) {
            static std::atomic<int> s_timeoutBlockedByNativeFSRLogCount{0};
            const int logCount = s_timeoutBlockedByNativeFSRLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 600) == 0) {
                HookLogImportant(
                    "DX12: Overlay suppression exceeded 2s but native FSR owns presentation; keeping normal "
                    "overlay GPU work suppressed because the FFX present-callback path is %s "
                    "(elapsed=%llums runtime=%s apiFSR=%d nativeFGPath=%d nativeFSR=%d runtimeOwns=%d "
                    "explicitNativeOff=%d callbackEver=%d lastCallbackAge=%llums ffxStalled=%d "
                    "ffxStallAllows=%d log=%d)",
                    ffxPresentCallbackEverFired && !ffxStalled ? "active" : "not safe for fallback",
                    now - suppressedSince, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
                    g_FGCompat.IsFSRFGApiActive() ? 1 : 0, runtimeOwnedNativeFGPresentPath ? 1 : 0,
                    nativeFSRActive ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0,
                    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) ? 1 : 0,
                    ffxPresentCallbackEverFired ? 1 : 0,
                    ffxPresentCallbackEverFired && now >= lastCallback ? (now - lastCallback) : 0,
                    ffxStalled ? 1 : 0, ffxStallAllowsNormalOverlay ? 1 : 0, logCount + 1);
            }
            if (reason) {
                *reason = "native FSR present-callback path owns overlay after 2s suppression timeout";
            }
            return true;
        }

        // Do NOT reset g_OverlaySuppressedSinceMs here — keep it so all
        // call sites in the same frame see the same expired timer and
        // independently force-render.  It will be reset when the normal
        // (non-override) path returns false on a future frame.
        if (reason) {
            *reason = "overlay suppression exceeded 2s max duration";
        }
        if (suppressedSince == now) {
            // First frame of suppression — only just started the timer,
            // do not force-render yet.
            return true;
        }
        const auto elapsed = now - suppressedSince;
        HookLogImportant(
            "DX12: Overlay suppression exceeded 2s (%llums) — forcing normal overlay rendering "
            "(runtime=%s apiFSR=%d nativeFGPath=%d runtimeOwns=%d ffxStalled=%d ffxStallAllows=%d)",
            elapsed, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
            g_FGCompat.IsFSRFGApiActive() ? 1 : 0, runtimeOwnedNativeFGPresentPath ? 1 : 0,
            dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, ffxStalled ? 1 : 0, ffxStallAllowsNormalOverlay ? 1 : 0);
        return false;
    }
}

if (reason) {
    if (authoritativeFSRActive || runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG) {
        *reason = "runtime-owned native FSR FG swapchain";
    } else if (runtimeOwnedNativeFGPresentPath) {
        *reason = "runtime-owned native FSR Present teardown window";
    } else {
        *reason = "runtime-owned swapchain";
    }
}
return true;
}


void SyncSecondaryDx12OverlayColorState(DXGI_FORMAT format) {
const bool isHdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(format);
dx12_hook_g_D3D11On12Adapter.SetHDR(isHdr, static_cast<int>(format));

static std::atomic<int> s_lastFormat{-1};
static std::atomic<int> s_lastHdr{-1};
const int previousFormat = s_lastFormat.exchange(static_cast<int>(format), std::memory_order_acq_rel);
const int previousHdr = s_lastHdr.exchange(isHdr ? 1 : 0, std::memory_order_acq_rel);
if (previousFormat != static_cast<int>(format) || previousHdr != (isHdr ? 1 : 0)) {
    HookLogImportant("DX12: Secondary overlay color contract synchronized format=%d hdr=%d",
                     static_cast<int>(format), isHdr ? 1 : 0);
}
}


bool ResolveSwapchainOutputHDRState(IDXGISwapChain* swapchain, DXGI_FORMAT format, const char* logPrefix, int* outColorSpace, bool* outSupported) {
if (outColorSpace) {
    *outColorSpace = -1;
}
if (outSupported)
    *outSupported = false;

DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
bool hasTrackedColorSpace = false;
const auto encoding = DXGIShared::ResolveSwapChainPresentationEncoding(
    swapchain, format, &colorSpace, &hasTrackedColorSpace);
const bool supported = encoding != ce::presentation_color::Encoding::Unsupported;
const bool isActualHDR = ce::presentation_color::IsHDR(encoding);
if (outColorSpace && hasTrackedColorSpace)
    *outColorSpace = static_cast<int>(colorSpace);
if (outSupported)
    *outSupported = supported;
if (logPrefix) {
    HookLogImportant("%s - format=%d tracked=%d colorSpace=%d encoding=%s isHDR=%d", logPrefix,
                     static_cast<int>(format), hasTrackedColorSpace ? 1 : 0, static_cast<int>(colorSpace),
                     ce::presentation_color::Describe(encoding), isActualHDR ? 1 : 0);
}
return isActualHDR;
}


void ResetFFXPresentCallbackOverlayBackend(const char* reason) {
std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);
if (dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized()) {
    HookLogImportant("%s — resetting native FSR present-callback overlay adapter", reason);
    dx12_hook_g_FFXPresentOverlayAdapter.Shutdown();
}
dx12_hook_g_FFXPresentOverlayDevice = nullptr;
dx12_hook_g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;
if (dx12_hook_g_FFXPresentRtvHeap) {
    dx12_hook_g_FFXPresentRtvHeap->Release();
    dx12_hook_g_FFXPresentRtvHeap = nullptr;
}
}


void ForceClearNativeFSRInternalNoCallbackComposition(const char* reason) {
if (dx12_hook_g_NativeFSRInternalNoCallbackComposition.exchange(false, std::memory_order_acq_rel)) {
    HookLogImportant("DX12: Cleared retained native FSR internal no-callback composition route (%s)", reason);
    // Re-arm the VEH breakpoint for the next FG-on transition (one-shot detection cycle reset).
    FFXHook_ResetVehDisarmAndRearm();
}
}


bool UpdateHeuristicFSRFGState(bool active, const char* source) {
if (active && ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(
                  dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
                  dx12_hook_g_FGRuntimeOwnsSwapchain)) {
    g_FGCompat.SetHeuristicFSRFGActive(false);

    static std::atomic<int> s_explicitOffSuppressedLogCount{0};
    if (s_explicitOffSuppressedLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
        HookLogImportant(
            "DX12: Suppressing %s FSR FG heuristic because native FSR explicitly turned FG off while the "
            "runtime-owned "
            "swapchain teardown is still active",
            source ? source : "unknown");
    }
    return false;
}

const char* blockedReason = nullptr;
if (!CanUseFSRFGHeuristics(&blockedReason)) {
    g_FGCompat.SetHeuristicFSRFGActive(false);

    if (active) {
        static std::atomic<int> s_suppressedLogCount{0};
        if (s_suppressedLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLog("DX12: Suppressing %s FSR FG heuristic because %s", source,
                    blockedReason ? blockedReason : "it is unsafe");
        }
    }
    return false;
}

g_FGCompat.SetHeuristicFSRFGActive(active);
return true;
}


void CleanupOverlay() {
CleanupOverlay(false);
}


bool IsStartupOverlayCompatibilityActive() {
return ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(
    ce::overlay_compat::GetStartupBlockingOverlayModuleName() != nullptr, IsActualFrameGenerationActive(),
    dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire),
    dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire), dx12_hook_g_FGRuntimeOwnsSwapchain);
}


bool ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff() {
return ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
    dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire), dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit,
    dx12_hook_g_FGRuntimeOwnsSwapchain, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
    g_FGCompat.GetRuntimeMode(), HookHasExplicitStreamlineSetOptionsActivation(),
    dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire), dx12_hook_g_HadFSRFGPhase, dx12_hook_g_OriginalGameQueue != nullptr);
}


void UpdateStartupOverlayCompatibilityState() {
const bool actualFGActive = IsActualFrameGenerationActive();

if (actualFGActive) {
    dx12_hook_s_startupOverlayObservedAnyFG.store(true, std::memory_order_release);
    dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
    ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
    dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(false, std::memory_order_release);
    return;
}

const bool startupBlockingOverlayLoaded = ce::overlay_compat::GetStartupBlockingOverlayModuleName() != nullptr;
if (!startupBlockingOverlayLoaded || !dx12_hook_g_FGRuntimeOwnsSwapchain) {
    dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
}

const bool observedAnyFrameGenerationActivity = dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire);
const bool startupCompatSettled = dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire);
const bool lateRuntimeOwnedHandoffJustObserved =
    dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.exchange(false, std::memory_order_acq_rel);
const bool preserveLiveOverlayDuringHandoff =
    ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
if (!ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
        startupBlockingOverlayLoaded, actualFGActive, startupCompatSettled, dx12_hook_g_FGRuntimeOwnsSwapchain,
        observedAnyFrameGenerationActivity, lateRuntimeOwnedHandoffJustObserved,
        preserveLiveOverlayDuringHandoff)) {
    if (lateRuntimeOwnedHandoffJustObserved && preserveLiveOverlayDuringHandoff) {
        HookLogImportant(
            "DX12: Keeping settled startup overlay live through runtime-inactive Streamline handoff "
            "(overlayInit=%d syncInit=%d runtime=%s origGame=%p)",
            dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0,
            ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), dx12_hook_g_OriginalGameQueue);
    }
    return;
}

if (dx12_hook_s_startupOverlayCompatSettled.exchange(false, std::memory_order_acq_rel)) {
    HookLogImportant(
        "DX12: Re-arming startup overlay compatibility after late runtime-owned swapchain handoff before any real "
        "FG activity");
    ResetStartupOverlayBackendActivationStage();
}
}


const char* GetStartupOverlayFirstDrawProbeStageName(StartupOverlayFirstDrawProbeStage stage) {
switch (stage) {
    case StartupOverlayFirstDrawProbeStage::kBackbufferTouchOnly:
        return "backbuffer touch";
    case StartupOverlayFirstDrawProbeStage::kPipelineStateOnly:
        return "pipeline state setup";
    case StartupOverlayFirstDrawProbeStage::kActualRender:
        return "real overlay draw";
    case StartupOverlayFirstDrawProbeStage::kComplete:
        return "complete";
    case StartupOverlayFirstDrawProbeStage::kNone:
    default:
        return "overlay probe";
}
}


void ResetStartupOverlayBackendActivationStage() {
dx12_hook_s_startupOverlayActivationStage = StartupOverlayActivationStage::kNone;
dx12_hook_s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kNone;
dx12_hook_s_startupOverlayActivationStageMs = 0;
dx12_hook_s_startupOverlaySyncInitMs = 0;
dx12_hook_s_startupOverlayResourcePrimeMs = 0;
dx12_hook_s_startupOverlayFirstDrawProbeMs = 0;
dx12_hook_s_lastStartupBlockingRenderModuleActivityMs.store(0, std::memory_order_release);
}


bool IsActualFrameGenerationActive() {
const auto runtimeMode = g_FGCompat.GetRuntimeMode();
return runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG || runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
}


bool IsFSRFrameGenerationActive() {
return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kFSRFG;
}


bool IsNvidiaSmoothMotionActiveRuntime() {
return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion;
}


bool ShouldUseDedicatedOverlayQueue(const char** disabledByOverlayModule) {
const char* overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
const bool processNeedsDelay = IsStartupOverlayCompatibilityActive();
const bool actualFGActive = IsActualFrameGenerationActive();
const bool fsrFGActive = IsFSRFrameGenerationActive();
const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
const bool runtimeOwnsSwapchain = dx12_hook_g_FGRuntimeOwnsSwapchain;
const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();

// When Streamline FG is active, do NOT use a dedicated overlay queue.
// D3D12 rejects cross-queue access to swapchain backbuffers with
// DXGI_ERROR_ACCESS_DENIED during SL FG (SL takes exclusive control
// of the swapchain queue association).  Render on the game queue
// instead, skipping fence operations to avoid interfering with SL's
// internal frame synchronization.
if (streamlineFGRunning) {
    if (disabledByOverlayModule)
        *disabledByOverlayModule = nullptr;
    return false;
}

if (ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        actualFGActive, fsrFGActive, streamlineFGRunning, runtimeOwnsSwapchain, runtimeOwnedNativeFGPresentPath)) {
    static std::atomic<int> s_runtimeOwnedDedicatedQueueDisableLogCount{0};
    int logCount = s_runtimeOwnedDedicatedQueueDisableLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Dedicated overlay queue disabled for native/runtime-owned FG "
            "(fsrFG=%d runtimeOwns=%d nativePresentPath=%d scQueue=%p origGame=%p)",
            fsrFGActive ? 1 : 0, runtimeOwnsSwapchain ? 1 : 0, runtimeOwnedNativeFGPresentPath ? 1 : 0,
            dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
    }
    if (disabledByOverlayModule)
        *disabledByOverlayModule = nullptr;
    return false;
}

const bool shouldUseDedicatedQueue =
    ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(actualFGActive, processNeedsDelay, overlayModule);
if (disabledByOverlayModule) {
    *disabledByOverlayModule = shouldUseDedicatedQueue ? nullptr : overlayModule;
}

return shouldUseDedicatedQueue;
}


bool WaitForGameQueueBeforeDedicatedOverlaySubmission(ID3D12CommandQueue* gameQueue, const char* phase) {
if (!dx12_hook_g_State.overlayQueue || !dx12_hook_g_State.crossQueueFence || !dx12_hook_g_State.crossQueueFenceEvent) {
    return true;
}
if (!gameQueue) {
    HookLogImportant("DX12: Cannot synchronize dedicated overlay queue before %s because the game queue is null",
                     phase ? phase : "overlay submission");
    return false;
}

const UINT64 waitValue = dx12_hook_g_State.crossQueueFenceValue + 1;
HRESULT signalHr = gameQueue->Signal(dx12_hook_g_State.crossQueueFence, waitValue);
if (FAILED(signalHr)) {
    HookLogImportant("DX12: Failed to signal game queue before %s on dedicated overlay queue hr=0x%08X",
                     phase ? phase : "overlay submission", signalHr);
    return false;
}

dx12_hook_g_State.crossQueueFenceValue = waitValue;
if (dx12_hook_g_State.crossQueueFence->GetCompletedValue() >= waitValue) {
    return true;
}

HRESULT setHr = dx12_hook_g_State.crossQueueFence->SetEventOnCompletion(waitValue, dx12_hook_g_State.crossQueueFenceEvent);
if (FAILED(setHr)) {
    HookLogImportant("DX12: Failed to arm cross-queue wait before %s hr=0x%08X",
                     phase ? phase : "overlay submission", setHr);
    return false;
}

DWORD waitHr = WaitForSingleObject(dx12_hook_g_State.crossQueueFenceEvent, dx12_hook_kOverlayCrossQueueWaitMs);
if (waitHr == WAIT_OBJECT_0) {
    static std::atomic<int> s_crossQueueWaitSuccessLogCount{0};
    if (s_crossQueueWaitSuccessLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
        HookLogImportant("DX12: Dedicated overlay queue synchronized with game queue for %s (value=%llu)",
                         phase ? phase : "overlay submission", static_cast<unsigned long long>(waitValue));
    }
    return true;
}

if (waitHr == WAIT_TIMEOUT) {
    HookLogImportant("DX12: Timed out waiting for game queue before %s on dedicated overlay queue (value=%llu)",
                     phase ? phase : "overlay submission", static_cast<unsigned long long>(waitValue));
} else {
    HookLogImportant("DX12: WaitForSingleObject failed before %s on dedicated overlay queue result=%lu",
                     phase ? phase : "overlay submission", waitHr);
}
return false;
}


// Probe the real D3D12 ECL by creating a temporary COMPUTE queue.
// SL only vtable-hooks DIRECT queues for FG; COMPUTE queues keep the
// pristine d3d12.dll function pointer.  When DIRECT and COMPUTE queues
// share the same vtable (all hooks applied to the shared vtable), we
// fall back to scanning SL's hook for an indirect JMP/CALL target.
void ProbeRealD3D12ECL(ID3D12Device* device) {
if (dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire))
    return;
if (!device)
    return;

// Create a temporary COMPUTE queue
D3D12_COMMAND_QUEUE_DESC desc = {};
desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
desc.NodeMask = 0;

ID3D12CommandQueue* probeQueue = nullptr;
HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probeQueue));
if (FAILED(hr) || !probeQueue) {
    HookLogImportant("DX12: ECL probe - COMPUTE queue creation failed (hr=0x%08X)", (unsigned)hr);
    return;
}

void** probeVtable = *(void***)probeQueue;
void* probeECL = probeVtable[10];
void* probeSignal = probeVtable[14];  // Signal is at vtable[14] on ID3D12CommandQueue

// Check which module owns the COMPUTE queue's ECL
HMODULE probeModule = nullptr;
GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                   (LPCSTR)probeECL, &probeModule);
char probeMod[MAX_PATH] = {};
if (probeModule)
    GetModuleFileNameA(probeModule, probeMod, MAX_PATH);

// Check which module owns the COMPUTE queue's Signal
HMODULE probeSignalModule = nullptr;
GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                   (LPCSTR)probeSignal, &probeSignalModule);
char probeSignalMod[MAX_PATH] = {};
if (probeSignalModule)
    GetModuleFileNameA(probeSignalModule, probeSignalMod, MAX_PATH);

// Compare with the current DIRECT queue's vtable[10] (our hooked version)
ID3D12CommandQueue* directQueue = dx12_hook_g_SwapchainQueue;
void* directECL = nullptr;
char directMod[MAX_PATH] = {};
if (directQueue) {
    void** directVtable = *(void***)directQueue;
    directECL = directVtable[10];
    HMODULE dMod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)directECL, &dMod);
    if (dMod)
        GetModuleFileNameA(dMod, directMod, MAX_PATH);
}

bool sameVtable = (probeVtable == (directQueue ? *(void***)directQueue : nullptr));
bool sameECL = (probeECL == directECL);
bool probeIsD3D12 = (strstr(probeMod, "d3d12") != nullptr || strstr(probeMod, "D3D12") != nullptr);
bool probeSignalIsD3D12 =
    (strstr(probeSignalMod, "d3d12") != nullptr || strstr(probeSignalMod, "D3D12") != nullptr);

HookLogImportant("DX12: ECL probe - COMPUTE ECL=%p (%s), DIRECT ECL=%p (%s), sameVtable=%d sameECL=%d isD3D12=%d",
                 probeECL, probeMod, directECL, directMod, sameVtable ? 1 : 0, sameECL ? 1 : 0,
                 probeIsD3D12 ? 1 : 0);

if (probeIsD3D12) {
    dx12_hook_g_RealD3D12ECL.store((ExecuteCommandListsPtr)probeECL, std::memory_order_release);
    HookLogImportant("DX12: Real D3D12 ECL found via COMPUTE probe: %p", probeECL);
}

// Probe the real D3D12 Signal from the COMPUTE queue's vtable
if (probeSignalIsD3D12 && !dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire)) {
    dx12_hook_g_RealD3D12Signal.store(reinterpret_cast<SignalPtr>(probeSignal), std::memory_order_release);
    HookLogImportant("DX12: Real D3D12 Signal found via COMPUTE probe: %p (%s)", probeSignal, probeSignalMod);
}

// Always check saved original — in GTA V both COMPUTE and DIRECT share
// the same vtable (sameECL=1) so our hook is on both, but
// oExecuteCommandLists still holds the real D3D12 function.
if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
    ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
    if (savedOrig) {
        HMODULE origMod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)savedOrig, &origMod);
        char origModName[MAX_PATH] = {};
        if (origMod)
            GetModuleFileNameA(origMod, origModName, MAX_PATH);
        bool origIsD3D12 = (strstr(origModName, "d3d12") != nullptr || strstr(origModName, "D3D12") != nullptr);
        HookLogImportant("DX12: ECL probe - saved oECL=%p (%s) isD3D12=%d", (void*)savedOrig, origModName,
                         origIsD3D12 ? 1 : 0);
        if (origIsD3D12) {
            dx12_hook_g_RealD3D12ECL.store(savedOrig, std::memory_order_release);
            HookLogImportant("DX12: Real D3D12 ECL found via saved original: %p", (void*)savedOrig);
        }
    }
}

// If still not found, try to follow the saved original's JMP chain
if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
    ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
    if (savedOrig) {
        const uint8_t* fn = (const uint8_t*)savedOrig;
        void* target = nullptr;
        // Check for E9 rel32 (JMP rel32) — SL's hook might be a simple JMP
        if (fn[0] == 0xE9) {
            int32_t rel = *(const int32_t*)(fn + 1);
            target = (void*)(fn + 5 + rel);
        }
        // Check for FF 25 (JMP [rip+disp32]) — indirect JMP
        else if (fn[0] == 0xFF && fn[1] == 0x25) {
            int32_t disp = *(const int32_t*)(fn + 2);
            void** addr = (void**)(fn + 6 + disp);
            target = *addr;
        }
        // Check for 48 FF 25 (REX.W JMP [rip+disp32])
        else if (fn[0] == 0x48 && fn[1] == 0xFF && fn[2] == 0x25) {
            int32_t disp = *(const int32_t*)(fn + 3);
            void** addr = (void**)(fn + 7 + disp);
            target = *addr;
        }

        if (target) {
            HMODULE targetMod = nullptr;
            GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)target, &targetMod);
            char targetModName[MAX_PATH] = {};
            if (targetMod)
                GetModuleFileNameA(targetMod, targetModName, MAX_PATH);
            bool isD3D12 = (strstr(targetModName, "d3d12") != nullptr || strstr(targetModName, "D3D12") != nullptr);
            HookLogImportant("DX12: ECL probe - followed JMP chain: target=%p (%s) isD3D12=%d", target,
                             targetModName, isD3D12 ? 1 : 0);
            if (isD3D12) {
                dx12_hook_g_RealD3D12ECL.store((ExecuteCommandListsPtr)target, std::memory_order_release);
                HookLogImportant("DX12: Real D3D12 ECL found via JMP chain: %p", target);
            }
        }
    }
}

if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
    HookLogImportant(
        "DX12: ECL probe - FAILED to find real D3D12 ECL! "
        "Overlay will be disabled during SL FG to prevent crash");
}

probeQueue->Release();
}

