#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::Phase5() {
if (!observerOnlyMode && !dx12_hook_s_insideECL && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit) {
    bool outerSLFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    bool previousOuterSLFGRunning = dx12_hook_g_OuterTrackedSLFGRunning.load(std::memory_order_acquire);

    if (outerSLFGRunning != previousOuterSLFGRunning) {
        bool slTurnedOff = previousOuterSLFGRunning && !outerSLFGRunning;
        bool slTurnedOn = !previousOuterSLFGRunning && outerSLFGRunning;
        dx12_hook_g_OuterTrackedSLFGRunning.store(outerSLFGRunning, std::memory_order_release);
        const bool preserveActivePostSLOnLateOuterOn =
            slTurnedOn && ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(
                              outerSLFGRunning, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                              HookIsPostSLOverlayActiveButUnconfirmed());
        auto* transitionDevice = g_Device.load(std::memory_order_acquire);
        const HRESULT transitionDeviceHr = transitionDevice ? transitionDevice->GetDeviceRemovedReason() : S_OK;
        const bool bypassPureStreamlineOffCooldown =
            ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
                slTurnedOff, dx12_hook_g_HadFSRFGPhase, g_FGCompat.IsFSRFGApiActive(),
                HookHasRuntimeOwnedNativeFGPresentPath(), dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                dx12_hook_g_SwapchainQueue != nullptr, dx12_hook_g_OriginalGameQueue != nullptr, FAILED(transitionDeviceHr));
        // Make-before-break: DX12_OnStreamlineFGStateChanged latched the
        // keep-alive at the explicit OFF edge; the outer teardown must not
        // disable the confirmed PostSL path that is covering the proxy's
        // remaining presents.
        const bool keepConfirmedPostSLAliveAcrossOuterOff =
            slTurnedOff && dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
        const auto* lastSuccessfulPostSLSwapchain = dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
        const bool preserveConfirmedPostSLProxyResourcesAcrossOuterOff =
            ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(
                slTurnedOff, keepConfirmedPostSLAliveAcrossOuterOff,
                pSwapChain != nullptr && pSwapChain == lastSuccessfulPostSLSwapchain, dx12_hook_g_State.overlayInit,
                dx12_hook_g_State.syncInit, FAILED(transitionDeviceHr));
        // A confirmed-PostSL DLSS-FG SUSPEND (proxy stays live) is safe to rebuild
        // immediately even WITH FSR history: PostSL confirmed rendering means the
        // overlay ECL on the runtime-owned SL queue already succeeded this epoch, so
        // the generic 60-frame reinit cooldown only blanks a provably-live overlay
        // (session 20260613_150750: 60-present / 672 ms blank). The stricter cooldown
        // is kept for any current FSR/native-FG ownership or device-removal.
        const bool bypassConfirmedPostSLSuspensionCooldown =
            ce::dx12_overlay_policy::ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
                slTurnedOff, dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire),
                dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire), g_FGCompat.IsFSRFGApiActive(),
                HookHasRuntimeOwnedNativeFGPresentPath(), dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                dx12_hook_g_SwapchainQueue != nullptr, dx12_hook_g_OriginalGameQueue != nullptr, FAILED(transitionDeviceHr));
        // DLSS-FG -> FSR-FG (no-callback) takeover: the native-FSR takeover path already warm-reinited
        // the overlay on the runtime-owned FSR queue this frame; the [outer] teardown below would
        // force-clear it + 60-frame cooldown (session 20260615_020100: missed=60). Keep it live —
        // strictly the no-callback route (the app-callback bridge keeps the teardown for crash safety).
        const bool keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover =
            ce::dx12_overlay_policy::ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(
                slTurnedOff, g_FGCompat.IsFSRFGApiActive(),
                dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                HookHasRuntimeOwnedNativeFGPresentPath(), dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                FAILED(transitionDeviceHr));
        const bool keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn =
            ce::dx12_overlay_policy::ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(
                slTurnedOff, authoritativeDLSSOffNormalReturnReinitializedThisPresent,
                postFSRNormalRouteOwnershipProven, dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                FAILED(transitionDeviceHr));
        HookLogImportant("DX12: [outer] SL FG %s (allowOverlayRender=%d keepAlive=%d)", slTurnedOn ? "ON" : "OFF",
                         allowOverlayRender ? 1 : 0, keepConfirmedPostSLAliveAcrossOuterOff ? 1 : 0);

        // Set cooldown — prevents rendering during transition window
        if (preserveActivePostSLOnLateOuterOn) {
            HookLogImportant(
                "DX12: [outer] SL FG ON after active PostSL — preserving active PostSL path "
                "instead of re-entering transition cooldown");
        } else if (bypassPureStreamlineOffCooldown || bypassConfirmedPostSLSuspensionCooldown ||
                   keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover ||
                   keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
            dx12_hook_g_FGTransitionCooldown.store(0, std::memory_order_release);
            dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
            HookLogImportant(
                "DX12: [outer] %s — bypassing generic reinit cooldown "
                "(scQueue=%p origGame=%p devHr=0x%08X)",
                keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover
                    ? "DLSS->FSR no-callback takeover (overlay already reinited on FSR queue)"
                : keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn
                    ? "authoritative DLSS-off native return (overlay already reinited on exact game swapchain)"
                    : (bypassPureStreamlineOffCooldown ? "pure Streamline FG OFF"
                                                       : "confirmed-PostSL DLSS-FG suspension (proxy stays live)"),
                dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue, (unsigned)transitionDeviceHr);
        } else {
            dx12_hook_g_FGTransitionCooldown = std::max(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), 60);
            dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                            std::memory_order_release);
        }

        // Reset PostSL state for fresh start after transition.
        // Keep the callback installed on Streamline FG activation so
        // startup synthetic presents can immediately find it.
        if (!preserveActivePostSLOnLateOuterOn && !keepConfirmedPostSLAliveAcrossOuterOff) {
            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
        }
        if (!DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(outerSLFGRunning) &&
            !keepConfirmedPostSLAliveAcrossOuterOff) {
            SetPostSLCallbackInstalled(false, "DX12: [outer] SL transition");
        }
        dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
        if (!preserveActivePostSLOnLateOuterOn && !keepConfirmedPostSLAliveAcrossOuterOff) {
            dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
            dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
        }

        // Clear false heuristic FSR FG (SL's queues trigger queue-change heuristic)
        if (g_FGCompat.IsHeuristicFSRFGActive()) {
            g_FGCompat.SetHeuristicFSRFGActive(false);
            HookLogImportant("DX12: [outer] Cleared heuristic FSR FG during SL FG %s", slTurnedOn ? "ON" : "OFF");
        }

        // Clear NVIDIA_SM detection state — the cached 2× multiplier from
        // departing DLSS FG would otherwise trigger false NVIDIA_SM detection
        // in DetectPattern() within a few frames.
        g_FGCompat.ClearNvidiaSMState();

        // Reset queue-change heuristic so it re-captures initial queue.
        // If PostSL already confirmed rendering on a late outer ON edge,
        // keep the existing proven queue/routing state instead of forcing a
        // fresh startup-style re-capture that can starve confirmed PostSL.
        if (!preserveActivePostSLOnLateOuterOn) {
            RequestFGDetectionHeuristicReset();
        }

        // Bump epoch so the inner transition handler resyncs its local
        // tracking and skips redundant transition work even when the outer
        // path preserved a proven PostSL state instead of resetting it.
        { dx12_hook_g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release); }

        if (slTurnedOff) {
            // Suppress queue-change heuristic for frames after SL OFF.
            // The heuristic runs BEFORE this outer block in ProcessFrame, so
            // on the frame SL turns off, it sees queue switch (SL→origGame)
            // before the reset flag is set → false FSR_FG.
            // Use 600 frames (~4s@150fps) to cover high-fps menus where
            // SL's swapchain queue persists after FG teardown.
            dx12_hook_g_SLOffHeuristicGrace.store(600, std::memory_order_release);
            dx12_hook_g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);

            // DO NOT restore g_SwapchainQueue to g_OriginalGameQueue here.
            // When SL activates FG, it calls CreateSwapChainForHwnd with its
            // own queue (e.g. F0A0).  After FG teardown, SL's swapchain
            // PERSISTS — the game continues presenting on F0A0, not the
            // original game queue (F620).  Restoring to F620 causes a
            // queue/swapchain mismatch: we'd render to F0A0's backbuffers
            // on F620 → DXGI_ERROR_ACCESS_DENIED → DEVICE_REMOVED.
            //
            // g_SwapchainQueue already holds the correct value from the
            // CreateSwapChainForHwnd hook.  If the game creates a new
            // swapchain later, the hook updates g_SwapchainQueue.
            {
                std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                HookLogImportant(
                    "DX12: [outer] FG→off — keeping g_SwapchainQueue=%p "
                    "(origGame=%p) — SL swapchain persists after teardown",
                    dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
            }

            // Disable PostSL immediately — SL is tearing down — unless the
            // make-before-break keep-alive is covering the proxy's
            // remaining presents until the normal route confirms.
            if (!keepConfirmedPostSLAliveAcrossOuterOff) {
                dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                SetPostSLCallbackInstalled(false, "DX12: [outer] FG->off");
            } else {
                HookLogImportant(
                    "DX12: [outer] FG->off — PostSL keep-alive covers proxy presents until normal-route "
                    "recovery");
            }
            InvalidateAllOverlayCachedFrames();

            // Drain in-flight GPU work
            if (dx12_hook_g_State.fence && !preserveConfirmedPostSLProxyResourcesAcrossOuterOff &&
                !keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                UINT64 lastVal = dx12_hook_g_State.currentFenceValue;
                HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (drainEvent) {
                    HRESULT drainHr = dx12_hook_g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
                    if (SUCCEEDED(drainHr)) {
                        DWORD waitResult = WaitForSingleObject(drainEvent, 200);
                        HookLogImportant("DX12: [outer] FG→off GPU drain: fenceVal=%llu wait=%s", lastVal,
                                         waitResult == WAIT_OBJECT_0 ? "OK" : "TIMEOUT");
                    } else {
                        HookLogImportant("DX12: [outer] FG→off GPU drain FAILED: hr=0x%08X", (unsigned)drainHr);
                    }
                    CloseHandle(drainEvent);
                }
            } else if (preserveConfirmedPostSLProxyResourcesAcrossOuterOff) {
                HookLogImportant(
                    "DX12: [outer] FG->off — preserving exact confirmed PostSL proxy resources "
                    "(proxy=%p queue=%p; no drain/reinit/copy/wait)",
                    lastSuccessfulPostSLSwapchain, dx12_hook_g_PostSLLastWorkingQueue);
            } else if (keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                HookLogImportant(
                    "[OVERLAY VISIBILITY] First authoritative DLSS-off native Present keeps its newly rebuilt "
                    "overlay state (swapchain=%p queue=%p; no second drain/reinit)",
                    pSwapChain, dx12_hook_g_SwapchainQueue);
            }

            // Force overlay reinit — PostSL's RTVs reference SL's swapchain
            // backbuffers, which become invalid after SL tears down FG.  Without
            // reinit, pre-SL rendering uses stale RTVs → DEVICE_HUNG.
            // EXCEPTION (DLSS->FSR no-callback takeover): the native-FSR takeover path
            // already warm-reinited the overlay on the runtime-owned FSR swapchain queue
            // this same frame (its RTVs are valid for FSR's swapchain, not stale SL ones).
            // Tearing it down here is what produced the 60-present blank — keep it live.
            if (dx12_hook_g_State.overlayInit && !keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover &&
                !preserveConfirmedPostSLProxyResourcesAcrossOuterOff &&
                !keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                HookLogImportant("DX12: [outer] FG→off — forcing overlay reinit (stale SL backbuffers)");
                dx12_hook_g_State.overlayInit = false;
                CleanupRTVs();
            } else if (dx12_hook_g_State.overlayInit && preserveConfirmedPostSLProxyResourcesAcrossOuterOff) {
                HookLogImportant(
                    "DX12: [outer] FG->off — exact confirmed PostSL proxy remains current; warm backend stays "
                    "drawable for this transition present");
            } else if (dx12_hook_g_State.overlayInit && keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover) {
                HookLogImportant(
                    "DX12: [outer] FG→off — DLSS->FSR no-callback takeover already reinited the overlay on the "
                    "runtime-owned FSR queue; keeping it live (no teardown, no cooldown blank)");
            } else if (dx12_hook_g_State.overlayInit && keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                HookLogImportant(
                    "DX12: [outer] FG->off — exact native return already rebuilt the overlay this Present; "
                    "keeping it drawable");
            }
            dx12_hook_g_ResetReinitSubmitCounter.store(true, std::memory_order_release);

            // Clear realECL so the next SL generation revalidates the method
            // against its currently tracked queue originals. Resolution is
            // passive and never creates a live diagnostic queue.
            {
                auto* oldRealECL = (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
                dx12_hook_g_RealD3D12ECL.store(nullptr, std::memory_order_release);
                HookLogImportant("DX12: [outer] FG→off — cleared realECL %p (will use origECL after reinit)",
                                 oldRealECL);
            }
        }

        if (slTurnedOn) {
            // Probe real D3D12 ECL when SL FG first activates — PostSL needs it
            // to bypass SL's COM wrapper.  The inner transition handler also does
            // this, but the epoch sync skips it for transitions already handled here.
            // Retain the conservative startup-window deferral even though the
            // resolver is passive; no Streamline initialization callback needs
            // to absorb the module/vtable inspection work.
            auto* dev = g_Device.load(std::memory_order_acquire);
            const bool startupWindowActiveForProbe = DXGIShared::IsStreamlineStartupTransitionWindowActive();
            if (dev && IsStreamlineLoaded()) {
                if (!startupWindowActiveForProbe) {
                    ProbeRealD3D12ECL(dev);
                    auto* probed = (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);

                    HookLogImportant("DX12: [outer] SL FG ON — probed realECL=%p (dev=%p)", probed, dev);
                } else {
                    dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                    HookLogImportant("DX12: [outer] SL FG ON — deferred ECL probe (startup window active, dev=%p)",
                                     dev);
                }
            } else {
                HookLogImportant("DX12: [outer] SL FG ON — skipped ECL probe (dev=%p, SL=%d)", dev,
                                 IsStreamlineLoaded() ? 1 : 0);
            }
        }
    }

    // Cooldown countdown — must always tick even when overlay blocked
    if (dx12_hook_g_FGTransitionCooldown > 0 && !allowOverlayRender) {
        // Only decrement here when the inner block won't run.
        // The inner block (inside allowOverlayRender gate) has its own
        // countdown logic.  Avoid double-decrementing.
        dx12_hook_g_FGTransitionCooldown.fetch_sub(1, std::memory_order_acq_rel);
        const bool preserveActivePostSLDuringBlockedCooldown =
            ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(
                outerSLFGRunning, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                HookIsPostSLOverlayActiveButUnconfirmed());
        if (!preserveActivePostSLDuringBlockedCooldown) {
            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
        }
        dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                        std::memory_order_release);
        if (dx12_hook_g_FGTransitionCooldown == 0) {
            HookLogImportant("DX12: [outer] FG transition cooldown complete (slFG=%d)", outerSLFGRunning ? 1 : 0);
            if (outerSLFGRunning) {
                const bool preserveSyntheticStartupState =
                    ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                            std::memory_order_acquire),
                        HookIsPostSLOverlayActiveButUnconfirmed(),
                        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                        HookIsPostSLOverlayConfirmedButStartupSettling());
                const bool keepStartupHandoffPending = ce::dx12_overlay_policy::
                    ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                            std::memory_order_acquire),
                        HookIsPostSLOverlayActiveButUnconfirmed(),
                        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                        HookIsPostSLOverlayConfirmedButStartupSettling());
                dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
                dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                std::memory_order_release);
                if (!preserveSyntheticStartupState) {
                    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                    DXGIShared::ResetStreamlineStartupTransitionState();
                }
            }
        }
    }

    // PostSL callback management — register when SL FG active, even if overlay blocked
    if (outerSLFGRunning && dx12_hook_g_FGTransitionCooldown == 0) {
        if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
            &PostSLOverlayRenderGated) {
            const bool preserveSyntheticStartupState =
                ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                        std::memory_order_acquire),
                    HookIsPostSLOverlayActiveButUnconfirmed(),
                    dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                    HookIsPostSLOverlayConfirmedButStartupSettling());
            const bool keepStartupHandoffPending =
                ce::dx12_overlay_policy::ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                        std::memory_order_acquire),
                    HookIsPostSLOverlayActiveButUnconfirmed(),
                    dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                    HookIsPostSLOverlayConfirmedButStartupSettling());
            SetPostSLCallbackInstalled(true, "DX12: [outer] Registered PostSL callback");
            dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
            if (!preserveSyntheticStartupState) {
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false,
                                                                                        std::memory_order_release);
                dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
            }
            dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                            std::memory_order_release);
            if (!preserveSyntheticStartupState) {
                DXGIShared::ResetStreamlineStartupTransitionState();
            }
            HookLogImportant("DX12: [outer] Registered PostSL callback (overlay blocked, SL FG active)");
        }
    } else if (!outerSLFGRunning && dx12_hook_g_FGTransitionCooldown == 0 &&
               !dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {
        // Make-before-break: while the keep-alive latch is set, the
        // installed callback IS the coverage for the proxy's remaining
        // presents; PostSLOverlayRenderGated retires it on normal-route
        // recovery or Streamline unload.
        if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
            SetPostSLCallbackInstalled(false, "DX12: [outer] cooldown complete");
            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
            dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
        }
    }
}

// [OVERLAY COVERAGE] attribute the responsible gate when this present cannot
// reach the overlay draw section below (condition mirrors the if that follows).
if (!(allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit &&
      !delayOverlayRenderAfterResume && !delayOverlayRenderAfterSyncInit &&
      !suppressOverlayRenderForLoadedStartupOverlay && !delayOverlayRenderAfterResourcePrime &&
      !delayOverlayRenderAfterFirstDrawProbe)) {
    NoteDX12OverlayCoverageGate(!allowOverlayRender    ? "overlay-render-suppressed"
                                : suspendOverlayRender ? "swapchain-not-drawable"
                                : dx12_hook_s_insideECL          ? "inside-ecl-reentry"
                                : !dx12_hook_g_State.overlayInit ? "overlay-backend-uninitialized"
                                : !dx12_hook_g_State.syncInit    ? "overlay-sync-uninitialized"
                                                       : "startup-render-delay");
}
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawOverlayFrame() {
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
    if (allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit &&
        !delayOverlayRenderAfterResume && !delayOverlayRenderAfterSyncInit &&
        !suppressOverlayRenderForLoadedStartupOverlay && !delayOverlayRenderAfterResourcePrime &&
        !delayOverlayRenderAfterFirstDrawProbe) {
    flow = DrawFrameTransition();
    if (flow == ProcessFrameFlow::kReturn) {
        return flow;
    }
    if (flow != ProcessFrameFlow::kContinue) {
        goto overlay_done;
    }
    flow = DrawCooldownAndRoute();
    if (flow == ProcessFrameFlow::kReturn) {
        return flow;
    }
    if (flow != ProcessFrameFlow::kContinue) {
        goto overlay_done;
    }
    flow = DrawMain();
    if (flow == ProcessFrameFlow::kReturn) {
        return flow;
    }
    if (flow != ProcessFrameFlow::kContinue) {
        goto overlay_done;
    }
    skip_overlay_draw:;
    overlay_done:;
    }
    return ProcessFrameFlow::kContinue;
}
