// occlusion signal; historically that only existed on the wrapped path, so vtable-hooked
// apps (e.g. dx12_test) never engaged the hold and drew to the backbuffer through the
// Alt+Tab iflip<->composited mode switch — the GPU-hang root cause. DetourPresent now also
// feeds the present result, so this gates the hold for both paths.
static std::atomic<bool> g_HaveD3D12PresentResultSignal{false};

// Focus-transition backbuffer-work hold (v10). DRED proved that ANY CE backbuffer
// touch — direct overlay draw (v8) OR the offscreen path's bb<->offscreen copies
// (v9) — pure-hangs the GPU (pageFaultVA=0) while the swapchain is mid
// iflip<->composited mode switch around a focus change (the backbuffer is
// transiently owned by DWM/the display). The hangs were observed at the refocus
// edge (composited->iflip). So CE holds ALL backbuffer overlay/capture work for a
// short window after EACH foreground-change edge (both directions), then resumes.
// Steady states — focused AND unfocused-but-visible — render directly, exactly
// like a lightweight inject overlay, so the overlay is only briefly absent during the actual mode switch
// (when the screen is transitioning anyway), never during steady visible use.
// Counter is set on the edge by DX12_NoteWrappedD3D12PresentResult and decremented
// per wrapped Present.
static constexpr int kFocusTransitionHoldFrames = 60;
static std::atomic<int> g_FocusTransitionHoldFrames{0};

struct DX12WrappedPresentFocusLossContext {
    bool valid = false;
    const char* presentName = nullptr;
    int callCount = 0;
    UINT syncInterval = 0;
    UINT presentFlags = 0;
};

static thread_local DX12WrappedPresentFocusLossContext s_WrappedPresentFocusLossContext = {};

static const char* DX12WaitResultName(DWORD waitResult);

extern "C" __declspec(dllexport) void DX12_SetWrappedPresentFocusLossContext(const char* presentName, int callCount,
                                                                             UINT syncInterval, UINT presentFlags) {
    s_WrappedPresentFocusLossContext.valid = true;
    s_WrappedPresentFocusLossContext.presentName = presentName;
    s_WrappedPresentFocusLossContext.callCount = callCount;
    s_WrappedPresentFocusLossContext.syncInterval = syncInterval;
    s_WrappedPresentFocusLossContext.presentFlags = presentFlags;
}

extern "C" __declspec(dllexport) void DX12_ClearWrappedPresentFocusLossContext() {
    s_WrappedPresentFocusLossContext = {};
}

// Re-entrancy guard: set when the current thread is inside DetourECL.
// During Alt+Tab, D3D12's internal WaitImpl inside ECL can pump window messages
// (DefWindowProc), which may trigger Present → ProcessFrame.  If ProcessFrame
// submits an overlay ECL while the outer ECL is still inside WaitImpl, a second
// WaitImpl cascades and the render thread hangs.  ProcessFrame checks this flag
// and skips overlay rendering when it's set.
static thread_local bool s_insideECL = false;

// Flag to indicate the current thread is inside a PostSL overlay ECL virtual call.
// When we submit our overlay ECL through SL's COM wrapper (virtual call on origGame),
// SL dispatches to the real D3D12 queue, which re-enters our ECL detour.  The detour
// must NOT update queue tracking (g_CommandQueue, g_SLWrapperQueue, etc.) for these
// overlay submissions — they'd pollute the game's queue state.
static thread_local bool s_insidePostSLOverlayECL = false;

// Broader CE-owned overlay submission guard. Normal, Steam-deferred, FSR callback,
// and PostSL submissions can re-enter the ECL hook through Streamline/FFX/driver
// wrapper queues even when we call a saved "original" entrypoint. Those re-entrant
// calls must be forwarded, but they must not mutate game/runtime queue tracking.
static thread_local int s_insideCEOverlayECLDepth = 0;
static thread_local const char* s_insideCEOverlayECLReason = nullptr;

class ScopedCEOverlayECLSubmission {
public:
    explicit ScopedCEOverlayECLSubmission(const char* reason) : previousReason_(s_insideCEOverlayECLReason) {
        ++s_insideCEOverlayECLDepth;
        s_insideCEOverlayECLReason = reason;
    }

    ~ScopedCEOverlayECLSubmission() {
        s_insideCEOverlayECLReason = previousReason_;
        --s_insideCEOverlayECLDepth;
    }

    ScopedCEOverlayECLSubmission(const ScopedCEOverlayECLSubmission&) = delete;
    ScopedCEOverlayECLSubmission& operator=(const ScopedCEOverlayECLSubmission&) = delete;

private:
    const char* previousReason_ = nullptr;
};

static bool KnownDLSSFGModuleLoaded() {
    if (g_KnownDLSSFGModuleSeen.load(std::memory_order_acquire)) {
        return true;
    }

    constexpr const wchar_t* kKnownDLSSFGModules[] = {
        L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", L"sl.dlss_g.dll", L"nvngx_dlssg.dll", L"nvngx_dlss.dll",
    };

    for (const wchar_t* moduleName : kKnownDLSSFGModules) {
        if (GetModuleHandleW(moduleName)) {
            g_KnownDLSSFGModuleSeen.store(true, std::memory_order_release);
            return true;
        }
    }

    return false;
}

static bool CanUseFSRFGHeuristics(const char** blockedReason = nullptr) {
    if (g_QueueChangeHeuristicAuthoritativeBaseline.load(std::memory_order_acquire) != nullptr) {
        if (blockedReason) {
            *blockedReason = "normal swapchain return is awaiting its authoritative queue baseline";
        }
        return false;
    }

    if (g_FGCompat.IsFSRFGApiActive()) {
        if (blockedReason) {
            *blockedReason = "authoritative FSR FG state is already active";
        }
        return false;
    }

    // Block when Streamline FG is running — SL creates internal queues that
    // trigger queue-change heuristics.  Without this check, enabling DLSS FG
    // causes false FSR FG detection (SL's queue ≠ origGame → "queue change"
    // heuristic fires → pre-SL renders on wrong queue → DEVICE_HUNG).
    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        if (blockedReason) {
            *blockedReason = "Streamline FG is running (queue changes are from SL, not FSR)";
        }
        return false;
    }

    // Block during grace period after SL FG turns OFF.  The queue naturally
    // changes from SL's internal queue back to origGame — this must not be
    // misinterpreted as FSR FG.  The heuristic runs BEFORE the outer block in
    // ProcessFrame, so g_StreamlineFGRunning alone can't prevent the false
    // positive on the same frame SL OFF fires.
    // NOTE: Do NOT decrement here — this function is called per-ECL (thousands/sec).
    // The counter is decremented once per ProcessFrame in the queue-change heuristic.
    if (g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0) {
        if (blockedReason) {
            *blockedReason = "SL FG just turned OFF (grace period)";
        }
        return false;
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
    if (ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
            g_FGRuntimeOwnsSwapchain, streamlineStartupHandoffPending, runtimeMode)) {
        if (blockedReason) {
            *blockedReason = "fresh authoritative Streamline startup handoff is still runtime-inactive";
        }
        return false;
    }

    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        currentSwapchainQueue = g_SwapchainQueue;
    }
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        g_HadFSRFGPhase, g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
        currentSwapchainQueue != nullptr);
    const bool postSLLastWorkingQueueStillActiveDuringRecentTeardown =
        g_PostSLLastWorkingQueue != nullptr &&
        GetTickCount64() < g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
            postFSRNonFGRecovery, false, postSLLastWorkingQueueStillActiveDuringRecentTeardown)) {
        if (blockedReason) {
            *blockedReason = "post-FSR non-FG recovery is still seeing preserved PostSL teardown traffic";
        }
        return false;
    }

    // Only block when DLSS FG is confirmed active WITH a known multiplier.
    // When DLSS modules are merely loaded but FG is off (or API state is transiently
    // toggling — common when switching to FSR FG), heuristics are safe.  The
    // g_PrimaryGameQueue filter ensures only game-queue ECL calls are counted,
    // preventing false positives from FG runtime queues.
    if (g_FGCompat.IsDLSSFGApiActive()) {
        int mult = g_FGCompat.GetFGMultiplier();
        if (mult >= 2) {
            if (blockedReason) {
                *blockedReason = "DLSS FG is actively generating frames";
            }
            return false;
        }
    }

    if (blockedReason) {
        *blockedReason = nullptr;
    }
    return true;
}

static bool IsFFXPresentCallbackStalled() {
    if (!g_FFXPresentCallbackBridgeExpected.load(std::memory_order_acquire)) {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastCallback = g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
    if (lastCallback != 0) {
        constexpr ULONGLONG kStallThresholdMs = 2000;
        return (now - lastCallback) > kStallThresholdMs;
    }
    // The callback has never fired since hook init.  If the runtime has owned
    // the swapchain for several seconds without a single callback, treat it as
    // stalled so the overlay does not stay invisible indefinitely.
    if (g_FGRuntimeOwnsSwapchain && g_FGRuntimeOwnsSwapchainSince != 0) {
        constexpr ULONGLONG kNeverFiredStallThresholdMs = 3000;
        return (now - g_FGRuntimeOwnsSwapchainSince) > kNeverFiredStallThresholdMs;
    }
    const ULONGLONG assumedSince = g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
    if (g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire) && assumedSince != 0) {
        constexpr ULONGLONG kProgressFallbackNeverFiredStallThresholdMs = 1500;
        return (now - assumedSince) > kProgressFallbackNeverFiredStallThresholdMs;
    }
    return false;
}

static constexpr ULONGLONG kProgressResolvedOfficialFFXOverlayFallbackStableMs = 5000;

struct ProgressResolvedOfficialFFXOverlayFallbackProof {
    bool proof = false;
    bool progressResolved = false;
    bool hasSwapchainQueue = false;
    bool hasOriginalGameQueue = false;
    bool swapchainQueueMatchesOriginalGameQueue = false;
    bool hasDevice = false;
    ULONGLONG stableMs = 0;
    HRESULT deviceHr = E_POINTER;
};

static ProgressResolvedOfficialFFXOverlayFallbackProof EvaluateProgressResolvedOfficialFFXOverlayFallbackProof() {
    ProgressResolvedOfficialFFXOverlayFallbackProof result{};
    result.progressResolved = g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);

    const ULONGLONG assumedSince = g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
    if (result.progressResolved && assumedSince != 0) {
        const ULONGLONG now = GetTickCount64();
        result.stableMs = (now >= assumedSince) ? (now - assumedSince) : 0;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        result.hasSwapchainQueue = g_SwapchainQueue != nullptr;
        result.hasOriginalGameQueue = g_OriginalGameQueue != nullptr;
        result.swapchainQueueMatchesOriginalGameQueue =
            result.hasSwapchainQueue && result.hasOriginalGameQueue && g_SwapchainQueue == g_OriginalGameQueue;
    }

    ID3D12Device* device = g_Device.load(std::memory_order_acquire);
    result.hasDevice = device != nullptr;
    result.deviceHr = device ? device->GetDeviceRemovedReason() : E_POINTER;

    result.proof = result.progressResolved && result.stableMs >= kProgressResolvedOfficialFFXOverlayFallbackStableMs &&
                   result.swapchainQueueMatchesOriginalGameQueue && result.hasDevice && SUCCEEDED(result.deviceHr);
    return result;
}

// Tracks when the FFX present callback was first detected as stalled and has
// never fired.  Used by the long-timeout escape hatch in the policy function.
static std::atomic<ULONGLONG> g_FFXPresentCallbackFirstStallEverDetectedMs{0};

// Tracks the timestamp when the overlay was first suppressed (set when
// ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain first returns true
// after having returned false).  Reset to 0 when the overlay is allowed
// to render via the normal non-FG path.  If suppression exceeds 2 seconds,
// the overlay is force-rendered regardless of FG state to guarantee a
// maximum 2-second overlay blackout across all FG transitions.
static std::atomic<ULONGLONG> g_OverlaySuppressedSinceMs{0};

static std::atomic<uint32_t> g_RuntimeOwnedStreamlineNoFGPresentCount{0};
static constexpr uint32_t kRuntimeOwnedStreamlineNoFGSettlePresents = 8;

static void ResetFFXPresentCallbackFirstStallDetection() {
    g_FFXPresentCallbackFirstStallEverDetectedMs.store(0, std::memory_order_release);
}

static ULONGLONG GetFFXPresentCallbackStallDurationMs() {
    const ULONGLONG firstStallMs = g_FFXPresentCallbackFirstStallEverDetectedMs.load(std::memory_order_acquire);
    if (firstStallMs == 0) {
        return 0;
    }
    const ULONGLONG now = GetTickCount64();
    return (now >= firstStallMs) ? (now - firstStallMs) : 0;
}

static void UpdateFFXPresentCallbackFirstStallDetection(bool ffxPresentCallbackStalled) {
    if (!ffxPresentCallbackStalled) {
        return;
    }
    const bool callbackEverFired = g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire) != 0;
    if (callbackEverFired) {
        // The callback fired at least once — the stall is transient, not a
        // never-fired scenario.  Do not arm the long-timeout escape hatch.
        return;
    }
    ULONGLONG expected = 0;
    g_FFXPresentCallbackFirstStallEverDetectedMs.compare_exchange_strong(expected, GetTickCount64(),
                                                                         std::memory_order_acq_rel);
}

static bool ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(bool ffxPresentCallbackStalled) {
    const bool explicitNativeFSROffPending =
        g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
    const bool evaluateFFXCallbackFallback = ce::dx12_overlay_policy::ShouldEvaluateFFXPresentCallbackFallback(
        ffxPresentCallbackStalled, explicitNativeFSROffPending);
    UpdateFFXPresentCallbackFirstStallDetection(ffxPresentCallbackStalled);
    const bool progressResolvedOfficialFFXPresentPath =
        g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);
    const bool directFFXApiConfirmation = g_FGCompat.HasDirectFFXApiConfirmation();
    const ULONGLONG lastCallback = g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
    const ULONGLONG assumedSince = g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
    const bool currentFFXPresentCallbackProof = ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(
        lastCallback, g_SwapchainQueueCaptureTime, assumedSince);
    const ProgressResolvedOfficialFFXOverlayFallbackProof progressProof =
        EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
    const ULONGLONG stallDurationMs = GetFFXPresentCallbackStallDurationMs();
    return ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        evaluateFFXCallbackFallback, progressResolvedOfficialFFXPresentPath, directFFXApiConfirmation,
        currentFFXPresentCallbackProof, progressProof.proof, stallDurationMs, explicitNativeFSROffPending);
}

static void LogSuppressedFFXPresentCallbackStallNormalOverlayFallback() {
    static std::atomic<int> s_suppressedStallFallbackLogCount{0};
    const int logCount = s_suppressedStallFallbackLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount >= 5 && (logCount % 600) != 0) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastCallback = g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
    const ULONGLONG assumedSince = g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
    const ProgressResolvedOfficialFFXOverlayFallbackProof progressProof =
        EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
    HookLogImportant(
        "DX12: FFX present callback appears stalled but normal overlay fallback is unsafe for "
        "this native FSR handoff until direct ffxConfigure/present-callback proof exists "
        "(lastCallback=%llu progressAssumedFor=%llums directFFX=%d explicitNativeOff=%d runtimeOwns=%d "
        "runtime=%s apiFSR=%d nativeFGPath=%d stableProof=%d stableFor=%llums requiredStable=%llums "
        "hasScQ=%d hasOrig=%d sameQueue=%d "
        "hasDevice=%d deviceHr=0x%08X scQueue=%p origGame=%p cmdQ=%p log=%d)",
        lastCallback, assumedSince ? (now - assumedSince) : 0, g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0,
        g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) ? 1 : 0,
        g_FGRuntimeOwnsSwapchain ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
        g_FGCompat.IsFSRFGApiActive() ? 1 : 0, HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
        progressProof.proof ? 1 : 0, progressProof.stableMs, kProgressResolvedOfficialFFXOverlayFallbackStableMs,
        progressProof.hasSwapchainQueue ? 1 : 0, progressProof.hasOriginalGameQueue ? 1 : 0,
        progressProof.swapchainQueueMatchesOriginalGameQueue ? 1 : 0, progressProof.hasDevice ? 1 : 0,
        static_cast<unsigned>(progressProof.deviceHr), g_SwapchainQueue, g_OriginalGameQueue,
        g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
}

static bool ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(const char** reason = nullptr) {
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
        g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
    const bool ffxStalled = IsFFXPresentCallbackStalled();
    const bool explicitNativeFSROffPending =
        g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
    const bool ffxStallAllowsNormalOverlay =
        ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(ffxStalled);
    const uint32_t streamlineNoFGPresentCount =
        g_RuntimeOwnedStreamlineNoFGPresentCount.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
            g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, streamlineNoFGPresentCount,
            kRuntimeOwnedStreamlineNoFGSettlePresents)) {
        if (reason) {
            *reason = "fresh runtime-owned Streamline no-FG swapchain";
        }
        static std::atomic<int> s_freshStreamlineNoFGSkipLogCount{0};
        const int logCount = s_freshStreamlineNoFGSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 16 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Deferring separate overlay GPU work on fresh runtime-owned Streamline no-FG swapchain "
                "(presentCount=%u settlePresents=%u scQueue=%p origGame=%p cmdQ=%p log=%d)",
                streamlineNoFGPresentCount, kRuntimeOwnedStreamlineNoFGSettlePresents, g_SwapchainQueue,
                g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
        }
        return true;
    }

    const bool skip = ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, authoritativeFSRActive,
        runtimeOwnedNativeFGPresentPath, ffxStallAllowsNormalOverlay, nativeFSRInternalNoCallbackComposition,
        DX12_IsFFXUiResourceCachedForBundle(), DX12_IsLiveSwapchainQueueOriginalGameQueue(),
        explicitNativeFSROffPending);
    if (!skip) {
        // Normal (non-override) path says don't skip — reset the suppression
        // timer so the next suppression episode gets a fresh 2-second window.
        g_OverlaySuppressedSinceMs.store(0, std::memory_order_release);

        if (ffxStallAllowsNormalOverlay) {
            static std::atomic<int> s_stallFallbackLogCount{0};
            if (s_stallFallbackLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                const ULONGLONG lastCallback = g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
                const ULONGLONG ownedSince = g_FGRuntimeOwnsSwapchainSince;
                const ULONGLONG assumedSince =
                    g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
                const ProgressResolvedOfficialFFXOverlayFallbackProof progressProof =
                    EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
                const bool directFFXApiConfirmation = g_FGCompat.HasDirectFFXApiConfirmation();
                ULONGLONG currentFFXProofSince = g_SwapchainQueueCaptureTime;
                if (assumedSince > currentFFXProofSince) {
                    currentFFXProofSince = assumedSince;
                }
                const bool currentFFXPresentCallbackProof = ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(
                    lastCallback, g_SwapchainQueueCaptureTime, assumedSince);
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
                    runtimeOwnedNativeFGPresentPath ? 1 : 0, g_FGRuntimeOwnsSwapchain ? 1 : 0,
                    explicitNativeFSROffPending ? 1 : 0, uiCached ? 1 : 0, liveIsOrigGame ? 1 : 0, g_SwapchainQueue,
                    g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
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
            const ULONGLONG lastCallback = g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
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
        ULONGLONG suppressedSince = g_OverlaySuppressedSinceMs.load(std::memory_order_acquire);
        if (suppressedSince == 0) {
            g_OverlaySuppressedSinceMs.store(now, std::memory_order_release);
            suppressedSince = now;
        }
        constexpr ULONGLONG kMaxOverlaySuppressionMs = 2000;
        if (now >= suppressedSince && (now - suppressedSince) >= kMaxOverlaySuppressionMs) {
            const bool nativeFSRActive = authoritativeFSRActive || ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode);
            const ULONGLONG lastCallback = g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
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
                        nativeFSRActive ? 1 : 0, g_FGRuntimeOwnsSwapchain ? 1 : 0,
                        g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) ? 1 : 0,
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
                g_FGRuntimeOwnsSwapchain ? 1 : 0, ffxStalled ? 1 : 0, ffxStallAllowsNormalOverlay ? 1 : 0);
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

static bool ShouldBridgeOverlayViaFFXPresentCallback(const ce::ffx_api::CallbackDescFrameGenerationPresent* desc) {
    if (!desc) {
        return false;
    }

    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
    const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
    const bool directFFXConfirmation = g_FGCompat.HasDirectFFXApiConfirmation();
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    if (!ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(
            runtimeOwnedNativeFGPresentPath, authoritativeFSRActive, directFFXConfirmation, runtimeMode)) {
        static std::atomic<int> s_ffxPresentBridgeSkippedNoEvidenceLogCount{0};
        const int logCount = s_ffxPresentBridgeSkippedNoEvidenceLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge skipped overlay because no authoritative native FSR evidence is "
                "active (runtimeOwnedNativePath=%d apiFSR=%d directFFX=%d runtime=%s frameId=%llu log=%d)",
                runtimeOwnedNativeFGPresentPath ? 1 : 0, authoritativeFSRActive ? 1 : 0, directFFXConfirmation ? 1 : 0,
                ce::fg_runtime::GetRuntimeModeName(runtimeMode), static_cast<unsigned long long>(desc->frameID),
                logCount + 1);
        }
        return false;
    }

    if (!desc->device || !desc->commandList || !desc->outputSwapChainBuffer.resource) {
        static std::atomic<int> s_ffxPresentBridgeInvalidDescLogCount{0};
        const int logCount = s_ffxPresentBridgeInvalidDescLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge skipped overlay because callback resources are incomplete "
                "(device=%p cmdList=%p output=%p frameId=%llu log=%d)",
                desc->device, desc->commandList, desc->outputSwapChainBuffer.resource,
                static_cast<unsigned long long>(desc->frameID), logCount + 1);
        }
        return false;
    }

    if (!g_IPC) {
        static std::atomic<int> s_ffxPresentBridgeNoIPCLogCount{0};
        const int logCount = s_ffxPresentBridgeNoIPCLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge skipped overlay because IPC is not connected "
                "(frameId=%llu log=%d)",
                static_cast<unsigned long long>(desc->frameID), logCount + 1);
        }
        return false;
    }

    SharedMemoryLayout* shm = g_IPC->GetSharedMem();
    if (!shm) {
        static std::atomic<int> s_ffxPresentBridgeNoSharedMemLogCount{0};
        const int logCount = s_ffxPresentBridgeNoSharedMemLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge skipped overlay because shared memory is unavailable "
                "(frameId=%llu log=%d)",
                static_cast<unsigned long long>(desc->frameID), logCount + 1);
        }
        return false;
    }

    const OverlayConfig cfg = GetActiveDX12OverlayConfig(shm);
    if (!cfg.showOverlay) {
        static std::atomic<int> s_ffxPresentBridgeOverlayHiddenLogCount{0};
        const int logCount = s_ffxPresentBridgeOverlayHiddenLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLog(
                "DX12: FFX present callback bridge skipped overlay because overlay is hidden "
                "(frameId=%llu log=%d)",
                static_cast<unsigned long long>(desc->frameID), logCount + 1);
        }
        return false;
    }

    return true;
}

static D3D12_RESOURCE_STATES GetDX12StateFromFFXResourceState(uint32_t state) {
    D3D12_RESOURCE_STATES dx12State = static_cast<D3D12_RESOURCE_STATES>(0);

    if (state & ce::ffx_api::kResourceStateUnorderedAccess) {
        dx12State |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
