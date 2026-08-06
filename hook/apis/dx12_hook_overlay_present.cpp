#include "dx12_hook_internal.h"


void DX12_OnSwapchainResizeBegin() {
    bool wasAlreadySet = dx12_hook_g_InSwapchainResizeCleanup.exchange(true);
    HookLog("DX12: DX12_OnSwapchainResizeBegin called, wasAlreadySet=%d", wasAlreadySet);

    // Disable post-SL overlay rendering IMMEDIATELY to prevent rendering
    // to invalidated backbuffers during the resize.
    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
    dx12_hook_g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
    dx12_hook_g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: swapchain resize");
    ResetPostSLLifecycleForTransition("DX12: swapchain resize", true);
    SetPostSLLastWorkingQueue(nullptr);  // Swapchain resize — rendering setup changed
    // Prevent recursion - if already in resize, return immediately
    if (wasAlreadySet) {
        HookLog(
            "DX12: DX12_OnSwapchainResizeBegin - already in resize, returning "
            "early");
        return;

    }

    DXGIShared::g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();

    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);

    // CRITICAL: Flush GPU before releasing resources.  In-flight overlay
    // commands still reference backbuffers; ResizeBuffers returns
    // E_ACCESSDENIED if any GPU references remain.
    CleanupOverlay();  // waits on fence, releases sync resources
    CleanupRTVs();
    dx12_hook_g_State.overlayInit = false;
    if (g_OverlayAdapter.IsInitialized()) {
        dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(true, std::memory_order_release);
        HookLogImportant(
            "DX12: Preserving warm overlay backend across swapchain resize; only backbuffer/sync resources were "
            "released (device=%p queue=%p fmt=%d)",
            dx12_hook_g_OverlayAdapterBackendDevice.load(std::memory_order_acquire),
            dx12_hook_g_OverlayAdapterBackendQueue.load(std::memory_order_acquire),
            dx12_hook_g_OverlayAdapterBackendFormat.load(std::memory_order_acquire));
    } else {
        dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(false, std::memory_order_release);
    }

    // g_LastSwapChain is stored as a raw (non-AddRef'd) pointer to avoid
    // interfering with FSR FG's reference count management.  Do NOT Release it.
    dx12_hook_g_PendingSwapChainCleanup = nullptr;
    dx12_hook_g_LastSwapChain = nullptr;
    HookLog("DX12: DX12_OnSwapchainResizeBegin - complete (GPU flushed)");
}


void DX12_OnSwapchainResizeEnd() {
    HookLog("DX12: DX12_OnSwapchainResizeEnd called");
    // Only clear if it was set - prevents unbalanced calls from clearing
    // prematurely
    if (dx12_hook_g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        dx12_hook_g_InSwapchainResizeCleanup.store(false, std::memory_order_release);
    }
    // g_PendingSwapChainCleanup is no longer used (swapchain stored without
    // AddRef), so nothing to release here.
    if (dx12_hook_g_PendingSwapChainCleanup) {
        dx12_hook_g_PendingSwapChainCleanup = nullptr;
    }
}


bool DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(IDXGISwapChain* pSwapChain, const char* source) {
    const bool keepAliveLatched = dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
    if (!pSwapChain || !keepAliveLatched || DXGIShared::WasPostSLOffKeepAlivePrePresentDrawn()) {
        return false;
    }

    ID3D12CommandQueue* lastWorkingQueue = nullptr;
    ID3D12CommandQueue* lockedQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        lastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
        lockedQueue = dx12_hook_g_PostSLLockedQueue;
    }

    const bool callbackInstalled =
        DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) == &PostSLOverlayRenderGated;
    const bool exactLastSuccessfulSwapchain =
        pSwapChain != nullptr && pSwapChain == dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldDriveExactPostSLOffKeepAliveBeforePresent(
            keepAliveLatched, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
            g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
            ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup(), IsStreamlineLoaded(),
            dx12_hook_g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire), callbackInstalled,
            lastWorkingQueue != nullptr || lockedQueue != nullptr, exactLastSuccessfulSwapchain)) {
        return false;
    }

    const uint64_t successfulSubmitSequenceBefore = dx12_hook_s_PostSLSuccessfulSubmitSequence;
    PostSLOverlayRenderGated(pSwapChain);
    const uint64_t successfulSubmitSequenceAfter = dx12_hook_s_PostSLSuccessfulSubmitSequence;
    const bool submitted = successfulSubmitSequenceAfter != successfulSubmitSequenceBefore;
    if (submitted) {
        DXGIShared::MarkPostSLOffKeepAlivePrePresentDrawn();
    } else {
        NoteDX12OverlayCoverageGate("postsl-pre-routing-exact-off-keepalive-submit-missed");
    }

    static std::atomic<int> s_preRoutingExactOffKeepAliveLogCount{0};
    const int logCount = s_preRoutingExactOffKeepAliveLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Pre-routing exact-proxy PostSL OFF keep-alive submit completed=%d sequence=%llu->%llu "
            "(source=%s sc=%p lastWorking=%p locked=%p log=%d)",
            submitted ? 1 : 0, successfulSubmitSequenceBefore, successfulSubmitSequenceAfter,
            source ? source : "Present", pSwapChain, lastWorkingQueue, lockedQueue, logCount + 1);
    }
    return submitted;
}


extern "C" __declspec(dllexport) void DX12_SubmitSteamDeferredOverlay() {
    if (dx12_hook_g_steamDeferredOverlay.pending && dx12_hook_g_steamDeferredOverlay.eclQueue) {
        SubmitSteamDeferredOverlay(dx12_hook_g_steamDeferredOverlay.eclQueue, "fallback");
    }
}


bool IsD3D12FocusLossPresentDeviceLostHRESULT(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}



extern "C" __declspec(dllexport) void DX12_NoteWrappedD3D12PresentResult(const char* presentName, int callCount,
                                                                         UINT syncInterval, UINT presentFlags,
                                                                         HRESULT presentHr, BOOL isFullscreen,
                                                                         BOOL isIconic, BOOL hasZeroSize,
                                                                         HWND gameWindow) {
    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    const DWORD currentProcessId = GetCurrentProcessId();
    const bool foregroundMatchesGame = processHasForeground;
    const bool presentSucceeded = SUCCEEDED(presentHr);
    const bool presentDeviceLost = IsD3D12FocusLossPresentDeviceLostHRESULT(presentHr);

    // Mark that we now have a trustworthy present-result-derived occlusion signal, so the
    // not-presentable backbuffer-work hold can engage regardless of present path (wrapped or
    // vtable DetourPresent).
    dx12_hook_g_HaveD3D12PresentResultSignal.store(true, std::memory_order_release);

    // Track swapchain visibility from the Present result. DXGI_STATUS_OCCLUDED (or
    // a minimized / zero-sized window) means the swapchain is not presentable and
    // the overlay is not visible to the user; that is the only state in which CE
    // holds backbuffer GPU work. A merely-unfocused but still-visible window keeps
    // presenting S_OK and must keep showing the overlay.
    const bool presentNotPresentable = (presentHr == DXGI_STATUS_OCCLUDED) || isIconic || hasZeroSize;
    const bool occlusionChanged =
        dx12_hook_g_SwapchainPresentOccluded.exchange(presentNotPresentable, std::memory_order_acq_rel) != presentNotPresentable;
    if (occlusionChanged) {
        // A presentable<->not-presentable transition is the risky DXGI iflip<->
        // composited mode switch where the device historically hung. Widen the
        // device-removal dump window so a hang at that edge is captured (DRED).
        dx12_hook_g_FocusLossRecentTransitionPresentWindow.store(dx12_hook_kFocusLossRecentTransitionDumpWindowFrames,
                                                       std::memory_order_release);
        HookLogImportant(
            "DX12: Swapchain presentability changed -> %s (present=%s#%d hr=0x%08X foreground=%d fullscreen=%d "
            "game=%p)",
            presentNotPresentable ? "NOT-PRESENTABLE (occluded/iconic/zero-size)" : "PRESENTABLE",
            presentName ? presentName : "Present", callCount, (unsigned)presentHr, processHasForeground ? 1 : 0,
            isFullscreen ? 1 : 0, gameWindow);
    }

    // v10 focus-transition hold: detect a foreground-change EDGE (gained or lost)
    // and arm a short backbuffer-work hold so CE does not touch the swapchain
    // backbuffer while the iflip<->composited mode switch is in flight (DRED proved
    // both a direct draw and an offscreen copy pure-hang there). The hold covers
    // BOTH directions; steady states render directly. Decrement once per wrapped
    // Present so the hold clears after the mode switch settles.
    {
        static std::atomic<int> s_lastForegroundState{-1};
        const int fgState = foregroundMatchesGame ? 1 : 0;
        const int prevState = s_lastForegroundState.exchange(fgState, std::memory_order_acq_rel);
        if (prevState != -1 && prevState != fgState) {
            dx12_hook_g_FocusTransitionHoldFrames.store(dx12_hook_kFocusTransitionHoldFrames, std::memory_order_release);
            HookLogImportant(
                "DX12: Focus-change edge (%s) — holding overlay/capture backbuffer work for up to %d Presents to "
                "clear the iflip<->composited mode switch (present=%s#%d game=%p)",
                fgState ? "regained foreground" : "lost foreground", dx12_hook_kFocusTransitionHoldFrames,
                presentName ? presentName : "Present", callCount, gameWindow);
        } else {
            int remaining = dx12_hook_g_FocusTransitionHoldFrames.load(std::memory_order_acquire);
            if (remaining > 0) {
                dx12_hook_g_FocusTransitionHoldFrames.store(remaining - 1, std::memory_order_release);
            }
        }
    }

    DX12WrappedPresentFocusLossContext presentContext;
    presentContext.valid = true;
    presentContext.presentName = presentName;
    presentContext.callCount = callCount;
    presentContext.syncInterval = syncInterval;
    presentContext.presentFlags = presentFlags;

    if (!foregroundMatchesGame) {
        dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining.store(dx12_hook_kFocusLossForegroundReacquirePresentProofFrames,
                                                                  std::memory_order_release);
        dx12_hook_g_FocusLossRecentTransitionPresentWindow.store(dx12_hook_kFocusLossRecentTransitionDumpWindowFrames,
                                                       std::memory_order_release);
    } else if (presentSucceeded && !presentDeviceLost && !isFullscreen && !isIconic && !hasZeroSize) {
        int remaining = dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining.load(std::memory_order_acquire);
        while (remaining > 0) {
            if (dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining.compare_exchange_weak(
                    remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                static std::atomic<int> s_focusReacquirePresentProofLogCount{0};
                const int logCount = s_focusReacquirePresentProofLogCount.fetch_add(1, std::memory_order_relaxed);
                if (remaining <= 3 || logCount < 20) {
                    HookLogImportant(
                        "DX12: Focus-loss foreground reacquire Present proof accepted "
                        "(present=%s#%d remaining=%d fg=%p/%lu game=%p/%lu sync=%u flags=0x%08X hr=0x%08X)",
                        presentName ? presentName : "Present", callCount, remaining - 1, foregroundWindow,
                        foregroundPid, gameWindow, currentProcessId, syncInterval, presentFlags, (unsigned)presentHr);
                }
                break;
            }
        }

        int recent = dx12_hook_g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire);
        while (recent > 0) {
            if (dx12_hook_g_FocusLossRecentTransitionPresentWindow.compare_exchange_weak(
                    recent, recent - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                break;
            }
        }
    }

    if (presentDeviceLost) {
        RequestFocusLossDeviceRemovalDumpOnce("D3D12 focus-transition device-lost Present result", presentHr,
                                              presentContext, foregroundWindow, foregroundPid, gameWindow,
                                              currentProcessId, g_CommandQueue.load(std::memory_order_acquire));
    }
}
