#include "dx12_hook_internal.h"
#include "dx12_hook_overlay_shared.h"


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

