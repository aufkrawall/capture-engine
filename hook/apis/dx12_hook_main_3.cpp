#include "dx12_hook_internal.h"
#include "dx12_hook_main_shared.h"


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
