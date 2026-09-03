#include "dx12_hook_internal.h"
#include "../common/hook_cpu_cost.h"
#include "dx12_hook_ecl_shared.h"


#include "dx12_hook_internal.h"

namespace {

using EclBreakTargetClass = ce::dx12_overlay_policy::EclBreakTargetClass;

EclBreakTargetClass ClassifyEclBreakTargetCandidate(ExecuteCommandListsPtr candidate) {
    char modulePath[MAX_PATH] = {};
    const bool resolved =
        TryGetModulePathFromCodeAddress(reinterpret_cast<const void*>(candidate), modulePath, sizeof(modulePath));
    return ce::dx12_overlay_policy::ClassifyEclBreakTargetCandidate(resolved, modulePath);
}

// Chooses the deepest provably safe ExecuteCommandLists for the recursion-break
// path. Never returns a known third-party overlay proxy hook (ReShade throws
// std::system_error(resource_deadlock_would_occur) when re-entered with the
// wrapped real queue — Talos + ReShade-only, session 20260813_041416) or CE's
// own detour.
ExecuteCommandListsPtr ResolveECLRecursionBreakTarget(ID3D12CommandQueue* pThis) {
    void** queueVtable = pThis ? *reinterpret_cast<void***>(pThis) : nullptr;
    char queueVtablePath[MAX_PATH] = {};
    const bool queueVtableResolved =
        TryGetModulePathFromCodeAddress(reinterpret_cast<const void*>(queueVtable), queueVtablePath,
                                        sizeof(queueVtablePath));

    const ExecuteCommandListsPtr perQueueOriginal = GetOriginalExecuteCommandLists(pThis);
    const ExecuteCommandListsPtr realD3D12Ecl = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);

    switch (ce::dx12_overlay_policy::SelectEclRecursionBreakTarget(
        ce::dx12_overlay_policy::ClassifyEclBreakTargetCandidate(queueVtableResolved, queueVtablePath),
        ClassifyEclBreakTargetCandidate(perQueueOriginal), ClassifyEclBreakTargetCandidate(realD3D12Ecl),
        ClassifyEclBreakTargetCandidate(oExecuteCommandLists))) {
        case ce::dx12_overlay_policy::EclBreakSelection::kPerQueueOriginal:
            return perQueueOriginal;
        case ce::dx12_overlay_policy::EclBreakSelection::kRealD3D12Ecl:
            return realD3D12Ecl;
        case ce::dx12_overlay_policy::EclBreakSelection::kGlobalOriginal:
            return oExecuteCommandLists;
        case ce::dx12_overlay_policy::EclBreakSelection::kNone:
            return nullptr;
    }
    return nullptr;
}

}  // namespace

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists) {
    // Safety: during FG transitions, SL may call ECL on a queue that's being freed.
    // Freed COM objects have null vtable.  Forward directly to real ECL to avoid crash.
    if (!pThis || !*reinterpret_cast<void**>(pThis)) {
        ExecuteCommandListsPtr real = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
        if (!real)
            real = oExecuteCommandLists;
        if (real)
            {
                ScopedHookForwardedCall forwardedCycles;
                real(pThis, NumCommandLists, ppCommandLists);
            }
        return;
    }
    // Frame generation multiplies submissions per frame, so this hook is on a
    // hotter path than the present hook is.
    ScopedHookCpuCost eclCpuCost(HookExecuteCommandListsCpuCost());
    if (HookIsShuttingDown()) {
        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
        if (!original)
            original = oExecuteCommandLists;
        if (original)
            {
                ScopedHookForwardedCall forwardedCycles;
                original(pThis, NumCommandLists, ppCommandLists);
            }
        return;
    }

    // Safety: once the D3D12 device is removed/hung, the NV UMD context behind this
    // queue is torn down and forwarding the app's command lists into it dereferences
    // freed driver state — the deterministic nvwgf2um access violation seen ~1s after
    // a 32-bit DEVICE_HUNG TDR, while the app's render loop kept calling
    // ExecuteCommandLists (logs/20260608_211517). A D3D12 device is permanently lost
    // once removed; drop the submission. The app still learns of the loss when its next
    // Present returns DXGI_ERROR_DEVICE_*. g_DeviceRemoved is cleared by
    // DX12_SetCommandQueue when a fresh device is adopted, so a recovering app resumes.
    if (!ce::dx12_overlay_policy::ShouldForwardAppCommandListsToDriver(
            dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed))) {
        // A naive app that ignores Present's DXGI_ERROR_DEVICE_* (e.g. dx12_test) keeps
        // looping after removal, so this can fire hundreds of thousands of times — keep
        // the log bounded so it never floods (a real game recreates the device instead).
        static std::atomic<int> s_eclRemovedSkipLog{0};
        const int n = s_eclRemovedSkipLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 5 || (n % 100000) == 0) {
            HookLogImportant(
                "DX12: Skipping app ExecuteCommandLists forward — device removed (queue=%p numLists=%u skip#%d); "
                "avoids nvwgf2um AV from submitting into a torn-down driver",
                (void*)pThis, NumCommandLists, n + 1);
        }
        return;
    }

    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_traceEclN{0};
        const int sn = s_traceEclN.fetch_add(1, std::memory_order_relaxed);
        // Finer steady-state sampling (every 64th ECL instead of every 300th) so
        // per-module submission composition (game vs RTSS vs Steam overlay) can be
        // tracked over time; needed to attribute ECL-activity loss (e.g. RTSS's
        // OSD stops submitting in Strange Brigade while the game keeps ~1200 fps).
        if (sn < 80 || (sn % 64) == 0) {
            char d[200];
            _snprintf_s(d, sizeof(d), _TRUNCATE, "queue=%p numLists=%u list0=%p seq=%d", (void*)pThis, NumCommandLists,
                        (NumCommandLists && ppCommandLists) ? (void*)ppCommandLists[0] : nullptr, sn);
            Dx12TraceLog("ExecuteCommandLists", d);
        }
    }

    // ===================== DIAGNOSTIC: ExecuteCommandLists timing =====================
    // Times the WHOLE ExecuteCommandLists call (including the real forward, where the NV
    // driver's command-buffer allocation D3D12Core CDevice::AllocateCB ->
    // NtGdiDdDDICreateAllocation happens). A slow ECL here during the Alt+Tab mode switch is
    // the 32-bit/WoW64 freeze (kernel GPU allocation slow under WoW64); on native 64-bit the
    // same ECL stays fast. Always-on, no env/flag — compare 32-bit vs 64-bit hook_debug.log.
    const bool diagEclIsOverlayQueue = (pThis == dx12_hook_g_State.overlayQueue);
    LARGE_INTEGER diagEclStart;
    QueryPerformanceCounter(&diagEclStart);
    auto diagEclTimer = ce::make_scope_guard([&]() {
        LARGE_INTEGER diagEclEnd, diagEclFreq;
        QueryPerformanceCounter(&diagEclEnd);
        QueryPerformanceFrequency(&diagEclFreq);
        const double diagEclMs =
            (double)(diagEclEnd.QuadPart - diagEclStart.QuadPart) * 1000.0 / (double)diagEclFreq.QuadPart;
        // Windowed stats (per ~1s) so steady-state baselines are visible too, not just spikes.
        static std::atomic<double> s_eclWindowMaxMs{0.0};
        static std::atomic<double> s_eclWindowSumMs{0.0};
        static std::atomic<uint32_t> s_eclWindowCount{0};
        static std::atomic<ULONGLONG> s_eclWindowStartMs{0};
        double prevMax = s_eclWindowMaxMs.load(std::memory_order_relaxed);
        while (diagEclMs > prevMax &&
               !s_eclWindowMaxMs.compare_exchange_weak(prevMax, diagEclMs, std::memory_order_relaxed)) {}
        // Approximate sum via double CAS-free add (relaxed; diagnostic only).
        s_eclWindowSumMs.store(s_eclWindowSumMs.load(std::memory_order_relaxed) + diagEclMs, std::memory_order_relaxed);
        const uint32_t windowCount = s_eclWindowCount.fetch_add(1, std::memory_order_relaxed) + 1;
        const ULONGLONG nowMs = GetTickCount64();
        ULONGLONG windowStart = s_eclWindowStartMs.load(std::memory_order_relaxed);
        if (windowStart == 0) {
            s_eclWindowStartMs.compare_exchange_strong(windowStart, nowMs, std::memory_order_relaxed);
            windowStart = nowMs;
        }
        // Immediately log any slow ECL (the freeze signature).
        if (diagEclMs >= 2.0) {
            static std::atomic<int> s_slowEclLogCount{0};
            const int n = s_slowEclLogCount.fetch_add(1, std::memory_order_relaxed);
            if (n < 400 || (n % 50) == 0) {
                auto* diagDev = g_Device.load(std::memory_order_acquire);
                HRESULT diagDr = diagDev ? diagDev->GetDeviceRemovedReason() : E_FAIL;
                HookLogImportant(
                    "DX12 DIAG: ExecuteCommandLists SLOW %.1fms (queue=%p overlayQueue=%d lists=%u "
                    "devRemoved=0x%08X tid=0x%04X)",
                    diagEclMs, pThis, diagEclIsOverlayQueue ? 1 : 0, NumCommandLists, (unsigned)diagDr,
                    GetCurrentThreadId());
            }
        }
        // Periodic (~1s) ECL-timing summary for steady-state comparison.
        if (nowMs - windowStart >= 1000 && windowCount > 0) {
            if (s_eclWindowStartMs.compare_exchange_strong(windowStart, nowMs, std::memory_order_relaxed)) {
                const double winMax = s_eclWindowMaxMs.exchange(0.0, std::memory_order_relaxed);
                const double winSum = s_eclWindowSumMs.exchange(0.0, std::memory_order_relaxed);
                const uint32_t winCnt = s_eclWindowCount.exchange(0, std::memory_order_relaxed);
                HookLogImportant("DX12 DIAG: ECL timing/1s: count=%u maxMs=%.2f avgMs=%.3f tid=0x%04X", winCnt, winMax,
                                 winCnt ? (winSum / (double)winCnt) : 0.0, GetCurrentThreadId());
            }
        }
    });
    // ================================================================================

    // ECL heartbeat counter — read by SL hook to verify ECL is still firing.
    static std::atomic<uint64_t> s_eclCallCounter{0};
    uint64_t eclCount = s_eclCallCounter.fetch_add(1, std::memory_order_relaxed);
    if ((eclCount & 0xFFF) == 0) {
        // Every 4096 ECL calls (~every few frames), log a heartbeat
        static std::atomic<uint32_t> s_eclHeartbeatLogCount{0};
        if (s_eclHeartbeatLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLogImportant("DX12: ECL heartbeat #%llu (queue=%p, tid=0x%04X)", (unsigned long long)eclCount, pThis,
                             GetCurrentThreadId());
        }
    }

    // Heartbeat for freeze watchdog — skip when device is removed
    if (!dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
        g_RenderWatchdog.HeartbeatFromHelperThread();
    }

    // CRITICAL: Recursion depth guard.  If an FG engine (FSR FG, DLSS FG) or a
    // third-party overlay proxy (ReShade) hooks ECL and its "original" pointer
    // loops back to us, we'd recurse infinitely. Detect and break the cycle by
    // forwarding to the deepest known native D3D12 ECL. Never call the global
    // oExecuteCommandLists blindly here: when a third-party overlay proxy queue
    // was hooked first, that global is the proxy's own hook and re-entering it
    // with the wrapped real queue throws std::system_error
    // (resource_deadlock_would_occur) from ReShade's queue mutex (Talos +
    // ReShade-only, session 20260813_041416).
    static thread_local int s_eclRecursionDepth = 0;
    if (s_eclRecursionDepth > 0) {
        if (s_eclRecursionDepth >= 2) {
            // A previously selected break target looped back into CE. Stop
            // instead of recursing forever; the foreign hook above us is
            // mid-flight, so dropping this submission is the only safe exit.
            static std::atomic<int> s_eclBreakLoopBackLogCount{0};
            const int logCount = s_eclBreakLoopBackLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 5 || (logCount % 1000) == 0) {
                HookLogImportant(
                    "DX12: ECL recursion break target looped back - dropping submission "
                    "(queue=%p lists=%u depth=%d log=%d)",
                    pThis, NumCommandLists, s_eclRecursionDepth, logCount + 1);
            }
            return;
        }
        ExecuteCommandListsPtr breakTarget = ResolveECLRecursionBreakTarget(pThis);
        if (breakTarget) {
            ++s_eclRecursionDepth;
            auto breakGuard = ce::make_scope_guard([&]() { --s_eclRecursionDepth; });
            breakTarget(pThis, NumCommandLists, ppCommandLists);
        } else {
            static std::atomic<int> s_eclBreakUnresolvedLogCount{0};
            const int logCount = s_eclBreakUnresolvedLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 5 || (logCount % 1000) == 0) {
                HookLogImportant(
                    "DX12: ECL recursion with no usable native break target - skipping forward "
                    "(queue=%p lists=%u depth=%d log=%d)",
                    pThis, NumCommandLists, s_eclRecursionDepth, logCount + 1);
            }
        }
        return;
    }
    ++s_eclRecursionDepth;
    auto depthGuard = ce::make_scope_guard([&]() { --s_eclRecursionDepth; });

    // Track that this thread is inside an ECL call.  During Alt+Tab, D3D12's
    // internal WaitImpl can pump window messages which may trigger Present →
    // ProcessFrame.  ProcessFrame checks s_insideECL and skips overlay rendering
    // to prevent a cascading second WaitImpl that hangs the render thread.
    bool wasInsideECL = dx12_hook_s_insideECL;
    dx12_hook_s_insideECL = true;
    auto eclGuard = ce::make_scope_guard([&]() { dx12_hook_s_insideECL = wasInsideECL; });

    // Minimal-overhead path: during no-callback FSR FG, observe every non-CE batch on the presenter thread so
    // the final submitter is learned without depending on a module/product name. Once its exact return-address
    // and ordinal signature is stable, CE joins THAT SAME batch as the final list. All other runtime-queue calls
    // remain pure forwards and avoid CE's policy/lock/module-resolution path, which would desync AMD's QPC-timed
    // pacing (ffxQuery+0x225fe). Game-queue calls continue into normal processing for frame counting.
    const bool noCallbackFSR =
        dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
    ExecuteCommandListsPtr noCallbackOriginal = nullptr;
    if (noCallbackFSR && dx12_hook_s_insideCEOverlayECLDepth == 0) {
        noCallbackOriginal = GetOriginalExecuteCommandLists(pThis);
        if (DX12_TryAppendNoCallbackFSRTopmostOverlayToECL(
                pThis, NumCommandLists, ppCommandLists, noCallbackOriginal, CE_RETURN_ADDRESS())) {
            return;
        }
    }
    if (noCallbackFSR && pThis != dx12_hook_g_OriginalGameQueue) {
        ExecuteCommandListsPtr original = noCallbackOriginal ? noCallbackOriginal : GetOriginalExecuteCommandLists(pThis);
        if (original) {
            {
                ScopedHookForwardedCall forwardedCycles;
                original(pThis, NumCommandLists, ppCommandLists);
            }
        } else {
            pThis->ExecuteCommandLists(NumCommandLists, ppCommandLists);
        }
        return;
    }

    // Skip our own overlay queue - don't count overlay submissions as game
    // command lists and don't re-register the overlay queue as the game queue.
    if (pThis == dx12_hook_g_State.overlayQueue) {
        // During SL FG, use the real D3D12 ECL to bypass SL's vtable hook.
        ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
        if (realECL && IsStreamlineLoaded() && IsActualFrameGenerationActive()) {
            realECL(pThis, NumCommandLists, ppCommandLists);
        } else {
            ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
            if (original)
                {
                    ScopedHookForwardedCall forwardedCycles;
                    original(pThis, NumCommandLists, ppCommandLists);
                }
        }
        return;
    }

    // Skip queue tracking for PostSL overlay virtual calls.  When PostSL submits
    // via queue->ExecuteCommandLists() (virtual call through SL's COM wrapper),
    // SL dispatches to the real D3D12 queue which re-enters this detour.
    // The real queue address differs from g_OriginalGameQueue (SL's wrapper),
    // so without this guard we'd corrupt queue tracking state.
    if (dx12_hook_s_insidePostSLOverlayECL) {
        // REAL QUEUE CAPTURE from SL's COM wrapper dispatch:
        //
        // When PostSL submits via slQueue->ExecuteCommandLists() (bootstrap frame),
        // SL's COM wrapper dispatches to its internal real D3D12 queue, which
        // re-enters this ECL detour.  pThis here is the REAL D3D12 queue, not
        // SL's wrapper.
        //
        // We capture this queue into g_RealQueueBehindSLWrapper for subsequent
        // frames to use direct submission (bypassing SL's cumulative damage).
        //
        // The bootstrap happens ONCE per PostSL reactivation epoch.  All subsequent
        // PostSL frames use: g_RealD3D12ECL(g_RealQueueBehindSLWrapper, ...).
        //
        // NOTE: If SL recreates its internal queues during a session (e.g., after
        // DLSS mode switch), this captured pointer could become stale.  Currently
        // no known trigger for this.  If stale, PostSL would crash and we'd see
        // DEVICE_REMOVED in logs — at which point re-bootstrap can be triggered.
        static std::atomic<ID3D12CommandQueue*> s_realQueueBehindSL{nullptr};
        ID3D12CommandQueue* realQueue = (ID3D12CommandQueue*)pThis;
        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
        ExecuteCommandListsPtr real = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
        const bool queueLooksDirect = original && real && original == real;
        ID3D12CommandQueue* capturedSLWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        const bool usableDirectQueueCandidate = ce::dx12_overlay_policy::IsUsableValidatedPostSLDirectQueueCandidate(
            queueLooksDirect, realQueue == capturedSLWrapperQueue, realQueue == currentCommandQueue,
            realQueue == dx12_hook_g_OriginalGameQueue, realQueue == dx12_hook_g_SwapchainQueue);
        if (usableDirectQueueCandidate) {
            ID3D12CommandQueue* previousRealQueue = s_realQueueBehindSL.exchange(realQueue, std::memory_order_acq_rel);
            dx12_hook_g_RealQueueBehindSLWrapper.store(realQueue, std::memory_order_release);
            if (previousRealQueue != realQueue) {
                HookLogImportant(
                    "DX12: ECL captured validated real queue behind SL wrapper %p during PostSL submit/probe",
                    realQueue);
            }
        } else {
            HookLogImportant(
                "DX12: ECL ignored PostSL direct-queue capture candidate %p (origECL=%p realECL=%p matchesWrapper=%d "
                "matchesCmdQ=%d matchesOrig=%d matchesScQ=%d)",
                realQueue, (void*)original, (void*)real, realQueue == capturedSLWrapperQueue ? 1 : 0,
                realQueue == currentCommandQueue ? 1 : 0, realQueue == dx12_hook_g_OriginalGameQueue ? 1 : 0,
                realQueue == dx12_hook_g_SwapchainQueue ? 1 : 0);
        }

        if (original)
            {
                ScopedHookForwardedCall forwardedCycles;
                original(pThis, NumCommandLists, ppCommandLists);
            }
        else {
            if (real)
                {
                    ScopedHookForwardedCall forwardedCycles;
                    real(pThis, NumCommandLists, ppCommandLists);
                }
        }
        return;
    }

    if (ce::dx12_overlay_policy::ShouldSuppressQueueTrackingForCEOverlaySubmission(dx12_hook_s_insideCEOverlayECLDepth > 0)) {
        static std::atomic<int> s_ceOverlayECLPassthroughLogCount{0};
        const int logCount = s_ceOverlayECLPassthroughLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 50 || (logCount % 512) == 0) {
            HookLogImportant(
                "DX12: Passing CE overlay ExecuteCommandLists through without queue tracking "
                "(queue=%p lists=%u reason=%s depth=%d count=%d)",
                pThis, NumCommandLists, dx12_hook_s_insideCEOverlayECLReason ? dx12_hook_s_insideCEOverlayECLReason : "unknown",
                dx12_hook_s_insideCEOverlayECLDepth, logCount + 1);
        }

        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
        if (original) {
            {
                ScopedHookForwardedCall forwardedCycles;
                original(pThis, NumCommandLists, ppCommandLists);
            }
        } else {
            ExecuteCommandListsPtr real = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
            if (real) {
                {
                    ScopedHookForwardedCall forwardedCycles;
                    real(pThis, NumCommandLists, ppCommandLists);
                }
            } else if (oExecuteCommandLists) {
                oExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
            }
        }
        return;
    }

    if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
        static std::atomic<int> s_protectedOfficialFFXECLPassThroughLogCount{0};
        const uint32_t progressCount =
            dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (MaybeFinalizeProtectedOfficialFFXStartupAfterSustainedProgress("ExecuteCommandLists")) {
            HookLogImportant(
                "DX12: Protected official FFX startup progress fallback completed on ECL; resuming CE side effects "
                "(queue=%p lists=%u eclProgress=%u)",
                pThis, NumCommandLists, progressCount);
        } else if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
            const int logCount = s_protectedOfficialFFXECLPassThroughLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 1024) == 0) {
                HookLogImportant(
                    "DX12: Protected official FFX startup pending - passing ExecuteCommandLists through without CE "
                    "side effects until enabled ffxConfigure or present-callback proof "
                    "(queue=%p lists=%u eclCount=%llu tid=0x%04X count=%d progress=%u processFrameProgress=%u)",
                    pThis, NumCommandLists, (unsigned long long)eclCount, GetCurrentThreadId(), logCount + 1,
                    progressCount, dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.load(std::memory_order_acquire));
            }

            ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
            if (original) {
                {
                    ScopedHookForwardedCall forwardedCycles;
                    original(pThis, NumCommandLists, ppCommandLists);
                }
            }
            return;
        }
    }

    NoteStartupBlockingRenderModuleActivityFromECL(pThis, CE_RETURN_ADDRESS());

    // Critical fix for GTA V Enhanced DLSS FG startup freeze:
    // When the startup-handoff Present bypasses the synthetic Present path, PostSL
    // activation is deferred until the startup transition window expires.  If
    // ProcessFrame stops running (freeze), the deferred callback never fires.
    // Detect window expiry from the ECL hook (present thread) and trigger the
    // PostSL callback directly to complete activation before Streamline times out.
    {
        static bool s_startupWindowWasActive = false;
        static bool s_callbackTriggeredWithCachedSwapchain = false;
        static std::atomic<ULONGLONG> s_lastVisibleOverlayStartupProgressTriggerMs{0};
        static std::atomic<int> s_visibleOverlayStartupProgressLogCount{0};
        static std::atomic<int> s_visibleOverlayStartupProgressCompleteLogCount{0};
        const bool activationPending =
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
        const bool callbackInstalled =
            DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
        const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
        const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
        const bool postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
        const bool overlayVisible = GetHookOverlayConfig().showOverlay;
        const bool windowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        const bool startupTransitionWindowJustExpired = s_startupWindowWasActive && !windowActive;
        const bool nativeFSRPresentPathActive = HookHasRuntimeOwnedNativeFGPresentPath();
        const bool nativeFSRActive = g_FGCompat.IsFSRFGApiActive();
        const bool shouldProbeStartupActivationSwapchain =

            ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(
                activationPending, callbackInstalled, postSLConfirmedRendering, nativeFSRPresentPathActive,
                nativeFSRActive);
        const bool activationSwapchainAvailable =
            shouldProbeStartupActivationSwapchain && HasStartupActivationSwapchainCandidateForECLProbe();
        if (!shouldProbeStartupActivationSwapchain && activationPending && callbackInstalled) {
            static std::atomic<int> s_eclStartupProbeSuppressedLogCount{0};
            const int logCount = s_eclStartupProbeSuppressedLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 200) == 0) {
                HookLogImportant(
                    "DX12: ECL startup activation swapchain probe suppressed "
                    "(activationPending=%d callbackInstalled=%d confirmed=%d nativeFGPath=%d apiFSR=%d "
                    "retained=%p last=%p log=%d)",
                    activationPending ? 1 : 0, callbackInstalled ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                    nativeFSRPresentPathActive ? 1 : 0, nativeFSRActive ? 1 : 0, dx12_hook_g_StreamlineStartupActivationSwapchain,
                    dx12_hook_g_LastSwapChain, logCount + 1);
            }
        }
        const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
        const bool allowExpiryTriggeredStartupActivation =
            ce::dx12_overlay_policy::ShouldTriggerExpiryDrivenECLPostSLStartupActivation(
                startupTransitionWindowJustExpired, activationPending, callbackInstalled, dx12_hook_g_HadFSRFGPhase,
                safePostFSRBootstrapPath);
        const bool continueVisibleOverlayStartupProgress =
            ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(
                overlayVisible, activationPending, postSLStartupActivationEntered, postSLConfirmedRendering,
                callbackInstalled, activationSwapchainAvailable, dx12_hook_g_HadFSRFGPhase, safePostFSRBootstrapPath);
        const ULONGLONG nowMs = GetTickCount64();
        const ULONGLONG lastVisibleOverlayStartupProgressTriggerMs =
            s_lastVisibleOverlayStartupProgressTriggerMs.load(std::memory_order_acquire);
        const bool visibleOverlayStartupProgressTick = continueVisibleOverlayStartupProgress && !windowActive &&
                                                       (lastVisibleOverlayStartupProgressTriggerMs == 0 ||
                                                        nowMs - lastVisibleOverlayStartupProgressTriggerMs >= 16);

        if (allowExpiryTriggeredStartupActivation || visibleOverlayStartupProgressTick) {
            // If the deferred ECL probe is pending and the startup window has expired,
            // try to probe now.  ProcessFrame may not be running (synthetic re-entrant
            // Present path), so the deferred probe check in ProcessFrame would never fire.
            DX12_ServiceDeferredECLProbe();

            if (activationSwapchainAvailable) {
                if (startupTransitionWindowJustExpired) {
                    HookLogImportant(
                        "DX12: ECL hook detected startup transition window expiry with pending PostSL activation — "
                        "triggering retained-swapchain PostSL activation service "
                        "(startupWindowExpired=1 activationPending=1 activeButUnconfirmed=%d "
                        "startupActivationEntered=%d callbackInstalled=1)",
                        postSLActiveButUnconfirmed ? 1 : 0, postSLStartupActivationEntered ? 1 : 0);
                } else {
                    const int logCount =
                        s_visibleOverlayStartupProgressLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 10 || (logCount % 120) == 0) {
                        HookLogImportant(
                            "DX12: ECL hook continuing visible-overlay PostSL startup progress while render remains "
                            "unconfirmed (startupPending=%d activeButUnconfirmed=%d "
                            "startupActivationEntered=%d retainedSwapchain=1)",
                            activationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                            postSLStartupActivationEntered ? 1 : 0);
                    }
                }

                s_callbackTriggeredWithCachedSwapchain = true;
                if (!startupTransitionWindowJustExpired) {
                    s_lastVisibleOverlayStartupProgressTriggerMs.store(nowMs, std::memory_order_release);
                }
                const bool invoked = DX12_TryInvokePostSLStartupActivationCallback(
                    startupTransitionWindowJustExpired ? "DX12::ECL startup-window expiry"
                                                       : "DX12::ECL visible startup progress",
                    startupTransitionWindowJustExpired);
                s_callbackTriggeredWithCachedSwapchain = false;
                if (invoked) {
                    if (startupTransitionWindowJustExpired) {
                        HookLogImportant("DX12: ECL hook retained-swapchain PostSL callback completed");
                    } else {
                        const int logCount =
                            s_visibleOverlayStartupProgressCompleteLogCount.fetch_add(1, std::memory_order_relaxed);
                        if (logCount < 10 || (logCount % 120) == 0) {
                            HookLogImportant("DX12: ECL hook visible-overlay PostSL progress callback completed");
                        }
                    }
                }
            } else {
                static int s_nullSwapchainSkipLog = 0;
                if (s_nullSwapchainSkipLog < 5) {
                    HookLogImportant(
                        "DX12: ECL hook skipping PostSL callback — no retained/fresh activation swapchain "
                        "(allowExpiry=%d visibleTick=%d)",
                        allowExpiryTriggeredStartupActivation ? 1 : 0, visibleOverlayStartupProgressTick ? 1 : 0);
                    ++s_nullSwapchainSkipLog;
                }
            }
        } else if (startupTransitionWindowJustExpired && activationPending && callbackInstalled &&
                   !allowExpiryTriggeredStartupActivation) {
            HookLogImportant(
                "DX12: ECL hook leaving pending PostSL activation dormant after startup window expiry "
                "because post-FSR bootstrap path is still unsafe "
                "(activationPending=1 hadFSR=%d safeBootstrap=%d activationSwapchainAvailable=%d)",
                dx12_hook_g_HadFSRFGPhase ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0, activationSwapchainAvailable ? 1 : 0);
        } else if (s_callbackTriggeredWithCachedSwapchain) {
            // Log if we're still processing after callback was triggered (callbacks from ProcessFrame)
            static int s_postEclCallbackLogCount = 0;
            if (s_postEclCallbackLogCount < 5) {
                HookLogImportant(
                    "DX12: PostSL callback path after ECL hook trigger "
                    "(windowActive=%d startupPending=%d callbackInstalled=%d)",
                    windowActive ? 1 : 0, activationPending ? 1 : 0, callbackInstalled ? 1 : 0);
                s_postEclCallbackLogCount++;
            }
        }
        s_startupWindowWasActive = windowActive;
    }

    // Debug: Log first few calls to verify hook is working
    int count = dx12_hook_g_ECLCallCount.load(std::memory_order_relaxed);
    if (count < 5) {
        count = dx12_hook_g_ECLCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 5) {
            HookLog("DX12: ExecuteCommandLists called #%d (queue=%p)", count, pThis);
        }
    }

    // Count command lists only from the trusted frame-classification queue.
    // Once ProcessFrame has identified the original game queue, prefer it over
    // the "first direct queue seen" heuristic to avoid auxiliary queue bursts
    // being misclassified as real presents.
    ID3D12CommandQueue* primaryQ = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* classificationQueue = GetFrameClassificationQueue();
    char eclCallerModulePath[MAX_PATH] = {};
    const bool anyFGActiveEarly = IsActualFrameGenerationActive() ||
                                  DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ||
                                  IsNvidiaSmoothMotionActiveRuntime();
    ID3D12CommandQueue* currentQEarly = g_CommandQueue.load(std::memory_order_acquire);
    const bool isKnownQueueEarly = primaryQ && (pThis == primaryQ || pThis == currentQEarly ||
                                                pThis == dx12_hook_g_OriginalGameQueue || pThis == dx12_hook_g_SwapchainQueue);
    bool callerFromThirdPartyOverlay = false;
    if (!anyFGActiveEarly || !isKnownQueueEarly) {
        callerFromThirdPartyOverlay =
            IsCurrentECLCallerFromThirdPartyOverlay(eclCallerModulePath, sizeof(eclCallerModulePath));
    }
    if ((!classificationQueue || pThis == classificationQueue) && !callerFromThirdPartyOverlay) {
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        dx12_hook_g_CommandListsExecutedThisFrame.fetch_add(NumCommandLists, std::memory_order_relaxed);
    } else if (callerFromThirdPartyOverlay) {
        static std::atomic<int> s_overlayECLCountIgnoreLogCount{0};
        int logCount = s_overlayECLCountIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant("DX12: Ignoring command-list count from third-party overlay caller %s on queue %p",
                             eclCallerModulePath[0] ? eclCallerModulePath : "unknown", pThis);
        }
    }

    // Register game's queue for overlay execution.
    // During FG, SL's worker threads may call ECL on transient internal queues
    // that can be freed at any time.  Skip registration for unknown queues to
    // avoid calling virtual methods (GetDesc/GetDevice) on freed objects.
    {
        const bool actualFGActive = IsActualFrameGenerationActive();
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool anyFGActive = actualFGActive || IsNvidiaSmoothMotionActiveRuntime() || streamlineFGRunning;
        const bool postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
        const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
            dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, actualFGActive, streamlineFGRunning,
            dx12_hook_g_SwapchainQueue != nullptr);
        const bool lastWorkingQueueStillActiveDuringRecentTeardown =
            dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
            GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);

        // Capture SL's wrapper queue: during SL FG, any DIRECT queue in ECL
        // that's NOT origGame/scQueue/primaryQ is likely SL's COM wrapper.
        // This wrapper routes ECL through SL's internal handler to the correct
        // queue.  We need it for PostSL overlay rendering after FSR→DLSS
        // transitions where origGame and scQueue both fail BB barriers.
        if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) && pThis != dx12_hook_g_OriginalGameQueue &&
            pThis != dx12_hook_g_SwapchainQueue && pThis != primaryQ) {
            ID3D12CommandQueue* prevWrapper = dx12_hook_g_SLWrapperQueue.load(std::memory_order_relaxed);
            if (prevWrapper != pThis) {
                // Must use mutex: PostSL reads g_SLWrapperQueue under this mutex
                // and calls AddRef. Without mutex, we could Release the old wrapper
                // while PostSL is between load() and AddRef() → use-after-free.
                std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                prevWrapper = dx12_hook_g_SLWrapperQueue.load(std::memory_order_relaxed);
                if (prevWrapper != pThis) {
                    pThis->AddRef();
                    dx12_hook_g_SLWrapperQueue.store(pThis, std::memory_order_release);
                    if (prevWrapper)
                        prevWrapper->Release();
                    static int s_wrapperLog = 0;
                    if (s_wrapperLog++ < 10)
                        HookLogImportant("DX12: ECL captured SL wrapper queue %p (origGame=%p scQ=%p primaryQ=%p)",
                                         pThis, dx12_hook_g_OriginalGameQueue, dx12_hook_g_SwapchainQueue, primaryQ);
                }
            }

            const bool pureDLSSStartupWrapperProgressCandidate =
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire) &&
                !dx12_hook_g_HadFSRFGPhase &&
                DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire) &&
                DXGIShared::IsStreamlineStartupTransitionWindowActive();
            if (pureDLSSStartupWrapperProgressCandidate) {
                int progressCount =
                    dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (progressCount <= 10 || progressCount == 50 || (progressCount % 500) == 0) {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup observed wrapper ECL progress #%d after top-level handoff "
                        "(queue=%p)",
                        progressCount, pThis);
                }

                const ULONGLONG lastProcessFrameTickMs = dx12_hook_g_LastProcessFrameTickMs.load(std::memory_order_acquire);
                const ULONGLONG nowMs = GetTickCount64();
                const ULONGLONG processFrameDormantMs = lastProcessFrameTickMs != 0 && nowMs >= lastProcessFrameTickMs
                                                            ? (nowMs - lastProcessFrameTickMs)
                                                            : 0;
                const bool dumpAlreadyRequested =
                    dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.load(std::memory_order_acquire);
                if (ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
                        dx12_hook_g_HadFSRFGPhase,
                        DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(
                            std::memory_order_acquire),
                        progressCount,
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                            std::memory_order_acquire),
                        dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire),
                        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire), processFrameDormantMs,
                        dumpAlreadyRequested)) {
                    bool expectedDumpRequested = false;
                    if (dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.compare_exchange_strong(
                            expectedDumpRequested, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
                        HookLogImportant(
                            "DX12: Pure-DLSS startup stall detected — wrapper ECL progress continues without top-level "
                            "Present recovery (progress=%d dormant=%llums postSLActive=%d confirmed=%d "
                            "startupPending=%d)",
                            progressCount, (unsigned long long)processFrameDormantMs,
                            dx12_hook_g_PostSLOverlayActive.load(std::memory_order_relaxed) ? 1 : 0,
                            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
                            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                std::memory_order_relaxed)
                                ? 1
                                : 0);
                        g_RenderWatchdog.RequestImmediateDump(
                            "Pure DLSS FG startup stalled after top-level handoff while wrapper queue still advanced",
                            DX12_GetGamePresentThreadId());
                    }
                }
            }
        }

        // PERF FIX: During FG (especially FSR FG), the ECL detour fires on every
        // command list submission.  FSR FG alternates queues each call (origGame
        // vs FSR queue), so DX12_SetCommandQueue's early-out (g_CommandQueue==pThis)
        // NEVER fires.  This means GetDesc(), g_CommandQueueMutex, GetDevice(),
        // and DX12_HookQueueVTable all execute on every single ECL call —
        // tens of thousands per second.  The cumulative overhead breaks FSR FG's
        // tight internal timing, causing its fence wait to never complete → freeze.
        //
        // Fix: During active FG, skip DX12_SetCommandQueue for queues we already
        // know (origGame, scQueue, primaryQ).  These are stable DIRECT queues that
        // don't need repeated registration.  Only register truly new/unknown queues.
        ID3D12CommandQueue* currentQ = g_CommandQueue.load(std::memory_order_acquire);
        bool isKnownQueue =
            (pThis == primaryQ || pThis == currentQ || pThis == dx12_hook_g_OriginalGameQueue || pThis == dx12_hook_g_SwapchainQueue);
        const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
        if (ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
                recentStreamlineTeardown, postFSRNonFGRecovery, lastWorkingQueueStillActiveDuringRecentTeardown,
                pThis == primaryQ, pThis == dx12_hook_g_OriginalGameQueue, pThis == dx12_hook_g_SwapchainQueue,
                pThis == dx12_hook_g_PostSLLastWorkingQueue)) {
            if (ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(
                    recentStreamlineTeardown, dx12_hook_g_PostSLLastWorkingQueue && pThis == dx12_hook_g_PostSLLastWorkingQueue,
                    streamlineFGRunning, postSLActive)) {
                MarkPostSLRecentTeardownActivity("DX12: ECL recent PostSL teardown activity", pThis);
            }
            static std::atomic<int> s_recentSLTeardownQueueIgnoreLogCount{0};
            int logCount = s_recentSLTeardownQueueIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 256) == 0) {
                HookLogImportant(
                    "DX12: Ignoring departed queue %p during Streamline teardown / post-FSR recovery "
                    "(primary=%p orig=%p scQ=%p current=%p slOffGrace=%d postSLRecent=%d postFSR=%d)",
                    pThis, primaryQ, dx12_hook_g_OriginalGameQueue, dx12_hook_g_SwapchainQueue, currentQ,
                    dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire),
                    lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0, postFSRNonFGRecovery ? 1 : 0);
            }
            goto skip_command_queue_registration;
        }
        if (!anyFGActive || !primaryQ || !isKnownQueue) {
            // No FG, or FG active with a primary queue still missing/unknown: register this queue.
            DX12_SetCommandQueueInternal(pThis, callerFromThirdPartyOverlay, eclCallerModulePath);
        }
        // else: FG active, known queue — skip registration (fast path)
    }
skip_command_queue_registration:

    // Periodic device-removed check in ECL detour during FG —
    // helps pinpoint when GPU dies relative to our hook activity.
    if ((eclCount & 0x3FF) == 0x200 && g_FGCompat.IsFGActive()) {
        auto* eclDev = g_Device.load(std::memory_order_acquire);
        if (eclDev) {
            HRESULT eclDevHr = eclDev->GetDeviceRemovedReason();
            if (FAILED(eclDevHr)) {
                static std::atomic<int> s_eclDevRemovedLogs{0};
                if (s_eclDevRemovedLogs.fetch_add(1, std::memory_order_relaxed) < 5) {
                    HookLogImportant(
                        "DX12: DEVICE REMOVED in ECL detour (eclCount=%llu queue=%p devRemoved=0x%08X tid=0x%04X)",
                        (unsigned long long)eclCount, pThis, (unsigned)eclDevHr, GetCurrentThreadId());
                }
            }
        }
    }

    ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);

    // Detect Steam overlay ECL: if the caller is from gameoverlayrenderer64.dll
    // AND we have a deferred CE overlay pending (set by DetourPresent for the
    // non-SL Steam invoke path), submit CE overlay commands AFTER Steam's ECL.
    // This ensures CE overlay renders on top of Steam's cleared backbuffer
    // instead of being overwritten by Steam's clear.
    const bool eclCallerIsSteam = eclCallerModulePath[0] != '\0' && IsSteamOverlayModulePath(eclCallerModulePath);
    const bool hasDeferredOverlay = dx12_hook_g_steamDeferredOverlay.pending;

    if (original) {
        // Arm coverage before entering a wrapped queue. Its ExecuteCommandLists implementation may
        // synchronously re-enter Present; the PostSL consumer accounts the official-UI route only
        // when an output actually inherits it (standby submissions remain invisible while FG is off).
        (void)ce::dx12_streamline_ui_overlay::BeforeExecuteCommandLists(NumCommandLists, ppCommandLists);
        {
            ScopedHookForwardedCall forwardedCycles;
            original(pThis, NumCommandLists, ppCommandLists);
        }
        ce::dx12_streamline_ui_overlay::AfterExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
    }

    // After Steam's ECL completes: submit CE deferred overlay to the same queue.
    if (eclCallerIsSteam && hasDeferredOverlay) {
        static std::atomic<int> s_deferredECLSubmitCount{0};
        int eclSubmitNum = s_deferredECLSubmitCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (eclSubmitNum <= 50 || (eclSubmitNum % 500) == 0) {
            HookLogImportant(
                "DX12: ECL hook detected Steam with deferred overlay pending #%d (queue=%p, eclCount=%llu)",
                eclSubmitNum, pThis, (unsigned long long)eclCount);
        }
        SubmitSteamDeferredOverlay(pThis, "ecl_hook");
    } else if (eclCallerIsSteam && !hasDeferredOverlay) {
        static std::atomic<int> s_deferredECLSkipCount{0};
        int eclSkipNum = s_deferredECLSkipCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (eclSkipNum == 1 || eclSkipNum <= 20 || (eclSkipNum % 500) == 0) {
            HookLogImportant(
                "DX12: ECL hook detected Steam but no deferred overlay pending #%d (queue=%p, eclCount=%llu)",
                eclSkipNum, pThis, (unsigned long long)eclCount);
        }
    }
}
