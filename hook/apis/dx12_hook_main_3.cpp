#include "dx12_hook_internal.h"
#include "dx12_hook_main_shared.h"


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

