        _snprintf_s(
            d, sizeof(d), _TRUNCATE, "dev=%p hwnd=%p w=%u h=%u fmt=%d count=%u effect=%d flags=0x%X -> sc=%p hr=0x%08X",
            (void*)pDevice, (void*)hWnd, pDesc ? pDesc->Width : 0, pDesc ? pDesc->Height : 0,
            pDesc ? (int)pDesc->Format : -1, pDesc ? pDesc->BufferCount : 0, pDesc ? (int)pDesc->SwapEffect : -1,
            pDesc ? (unsigned)pDesc->Flags : 0u, (SUCCEEDED(hr) && ppSC) ? (void*)*ppSC : nullptr, (unsigned)hr);
        Dx12TraceLog("CreateSwapChainForHwnd", d);
    }

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED((*ppSC)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            sc3->Release();
        }
        // NOTE: Do NOT call DX12_SetSwapchainQueue here.  This factory vtable
        // hook fires for ALL callers (including Streamline/Social Club internal
        // swapchain operations).  Capturing queues from non-game swapchains
        // corrupts g_SwapchainQueue and causes ERR_GFX_STATE.  The inline and
        // global hooks already capture the queue for legitimate game/FG calls.
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* device,
                                                        const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                                        D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                                        D3D12_RESOURCE_STATES InitialResourceState,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                        REFIID riidResource, void** ppvResource) {
    HRESULT hr = oCreateCommittedResource
                     ? oCreateCommittedResource(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                                                pOptimizedClearValue, riidResource, ppvResource)
                     : E_FAIL;
    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_n{0};
        const int sn = s_n.fetch_add(1, std::memory_order_relaxed);
        if (sn < 300 || (sn % 200) == 0) {
            char d[256];
            _snprintf_s(d, sizeof(d), _TRUNCATE,
                        "heapType=%d dim=%d w=%llu h=%u fmt=%d resFlags=0x%X state=0x%X -> res=%p hr=0x%08X seq=%d",
                        pHeapProperties ? (int)pHeapProperties->Type : -1, pDesc ? (int)pDesc->Dimension : -1,
                        pDesc ? (unsigned long long)pDesc->Width : 0ull, pDesc ? pDesc->Height : 0,
                        pDesc ? (int)pDesc->Format : -1, pDesc ? (unsigned)pDesc->Flags : 0u,
                        (unsigned)InitialResourceState, (SUCCEEDED(hr) && ppvResource) ? *ppvResource : nullptr,
                        (unsigned)hr, sn);
            Dx12TraceLog("CreateCommittedResource", d);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourTraceCreateCommandQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* pDesc,
                                                        REFIID riid, void** ppQueue) {
    HRESULT hr = oTraceCreateCommandQueue ? oTraceCreateCommandQueue(device, pDesc, riid, ppQueue) : E_FAIL;
    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_n{0};
        const int sn = s_n.fetch_add(1, std::memory_order_relaxed);
        if (sn < 200) {
            char d[256];
            _snprintf_s(d, sizeof(d), _TRUNCATE,
                        "type=%d prio=%d flags=0x%X node=%u dev=%p -> queue=%p hr=0x%08X seq=%d",
                        pDesc ? (int)pDesc->Type : -1, pDesc ? (int)pDesc->Priority : 0,
                        pDesc ? (unsigned)pDesc->Flags : 0u, pDesc ? pDesc->NodeMask : 0u, (void*)device,
                        (SUCCEEDED(hr) && ppQueue) ? *ppQueue : nullptr, (unsigned)hr, sn);
            Dx12TraceLog("CreateCommandQueue", d);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourTraceCreateDescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc,
                                                          REFIID riid, void** ppHeap) {
    HRESULT hr = oTraceCreateDescriptorHeap ? oTraceCreateDescriptorHeap(device, pDesc, riid, ppHeap) : E_FAIL;
    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_n{0};
        const int sn = s_n.fetch_add(1, std::memory_order_relaxed);
        if (sn < 200) {
            char d[256];
            _snprintf_s(d, sizeof(d), _TRUNCATE, "type=%d num=%u flags=0x%X node=%u -> heap=%p hr=0x%08X seq=%d",
                        pDesc ? (int)pDesc->Type : -1, pDesc ? pDesc->NumDescriptors : 0u,
                        pDesc ? (unsigned)pDesc->Flags : 0u, pDesc ? pDesc->NodeMask : 0u,
                        (SUCCEEDED(hr) && ppHeap) ? *ppHeap : nullptr, (unsigned)hr, sn);
            Dx12TraceLog("CreateDescriptorHeap", d);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourTraceCommandQueueSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    HRESULT hr = oTraceCommandQueueSignal ? oTraceCommandQueueSignal(queue, fence, value) : E_FAIL;
    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_n{0};
        const int sn = s_n.fetch_add(1, std::memory_order_relaxed);
        if (sn < 80 || (sn % 300) == 0) {
            char d[160];
            _snprintf_s(d, sizeof(d), _TRUNCATE, "queue=%p fence=%p value=%llu hr=0x%08X seq=%d", (void*)queue,
                        (void*)fence, (unsigned long long)value, (unsigned)hr, sn);
            Dx12TraceLog("Signal", d);
        }
    }
    return hr;
}

void DX12Hook::Shutdown() {
    LogOverlayCoverageSummary("shutdown summary");
    ce::dx12_sampler_hooks::LogSummary("shutdown");
    HookLogImportant(
        "DX12: Shutdown — cleaning up FFX state (runtime=%s overlayInit=%d syncInit=%d "
        "fgOwned=%d nativeFGPath=%d progressResolved=%d callbackBridges=%zu)",
        ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), g_State.overlayInit ? 1 : 0,
        g_State.syncInit ? 1 : 0, g_FGRuntimeOwnsSwapchain ? 1 : 0, HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
        g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire) ? 1 : 0,
        g_FFXPresentCallbackBridges.size());

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
        std::lock_guard<std::mutex> lock(g_FFXPresentCallbackBridgeMutex);
        g_FFXPresentCallbackBridges.clear();
    }
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption("DX12: Shutdown");
    ResetFFXPresentCallbackFirstStallDetection();
    g_FFXPresentCallbackBridgeExpected.store(false, std::memory_order_release);
    g_NativeFSRInternalNoCallbackComposition.store(false, std::memory_order_release);
    g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
    g_PostNativeFSROffGameSwapchainRecoveryQueue.store(nullptr, std::memory_order_release);
    g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
    g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
    g_RenderWatchdog.SetRuntimePresentationMonitor(false);
    g_OverlaySuppressedSinceMs.store(0, std::memory_order_release);

    // Release overlay completion fence
    {
        ID3D12Fence* fence = g_OverlayCompletionFence.exchange(nullptr, std::memory_order_acq_rel);
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
    if (g_DrainFence) {
        g_DrainFence->Release();
        g_DrainFence = nullptr;
    }
    if (g_DrainEvent) {
        CloseHandle(g_DrainEvent);
        g_DrainEvent = nullptr;
    }
    g_DrainFenceValue = 0;
    g_NeedOffscreenOverlayAfterPostFSRNonFG = false;

    // Clean up prerender fences/events
    for (auto* fence : g_PrerenderFences) {
        if (fence)
            fence->Release();
    }
    g_PrerenderFences.clear();
    for (auto event : g_PrerenderEvents) {
        if (event)
            CloseHandle(event);
    }
    g_PrerenderEvents.clear();
    g_PrerenderFrameIndex = 0;
    if (g_PrerenderDevice) {
        g_PrerenderDevice->Release();
        g_PrerenderDevice = nullptr;
    }
    if (g_PrerenderQueue) {
        g_PrerenderQueue->Release();
        g_PrerenderQueue = nullptr;
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
    if (g_SwapchainQueue) {
        g_SwapchainQueue->Release();
        g_SwapchainQueue = nullptr;
        g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
    }
    // Disable post-SL overlay callback before tearing down D3D12 resources.
    SetPostSLCallbackInstalled(false, "DX12: Shutdown");
    WaitForInFlightPostSLCallbacks("DX12: Shutdown");
    g_PostSLDeferredQueueCleanupPending.store(false, std::memory_order_release);
    ClearPostSLQueues("DX12: Shutdown");
    ClearPostSLPinnedSLWrapperQueue("DX12: Shutdown");
    SetPostSLLastWorkingQueue(nullptr);
    if (auto* deferredLockedQueue = g_DeferredPostSLLockedQueueRelease.exchange(nullptr, std::memory_order_acq_rel)) {
        deferredLockedQueue->Release();
    }
    if (g_CommandQueue.load()) {
        g_CommandQueue.load()->Release();
        g_CommandQueue.store(nullptr);
    }
    if (auto* deferredCommandQueue = g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel)) {
        deferredCommandQueue->Release();
    }
    // The authoritative-baseline pointer is non-owning and backed by
    // g_OriginalGameQueue. Clear it before releasing that retained queue.
    g_QueueChangeHeuristicAuthoritativeBaseline.store(nullptr, std::memory_order_release);
    g_ResetQueueChangeHeuristic.store(false, std::memory_order_release);
    g_ResetECLPatternHeuristic.store(false, std::memory_order_release);
    if (g_OriginalGameQueue) {
        g_OriginalGameQueue->Release();
        g_OriginalGameQueue = nullptr;
    }
    if (g_PreFGGameQueue) {
        g_PreFGGameQueue->Release();
        g_PreFGGameQueue = nullptr;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_ExecuteCommandListsHookStateMutex);
        g_ExecuteCommandListsOriginalByVTable.clear();
        oExecuteCommandLists = nullptr;
    }
    g_LastExecuteCommandListsVTable.store(nullptr, std::memory_order_release);
    g_LastExecuteCommandListsOriginal.store(nullptr, std::memory_order_release);
    if (g_Device.load()) {
        g_Device.load()->Release();
        g_Device.store(nullptr);
    }
    // g_LastSwapChain is a raw (non-AddRef'd) pointer — do NOT Release
    if (g_LastSwapChain) {
        g_LastSwapChain = nullptr;
    }
    g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);
    g_HadSuccessfulPostSLPhase.store(false, std::memory_order_release);
    g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
    g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
    g_LastKnownSwapchainHDRStateValid.store(false, std::memory_order_release);
    g_LastKnownSwapchainIsHDR.store(false, std::memory_order_release);
    g_LastKnownSwapchainColorSpace.store(-1, std::memory_order_release);
    if (g_SharedCaptureD3D12.IsActive()) {
        std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
        g_SharedCaptureD3D12.Reset();
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

bool DX12Hook::IsRealFrame() const {
    return g_FGCompat.IsCurrentFrameReal();
}

void DX12Hook::ClassifyFrame(int commandListCount) {
    g_FGCompat.RecordFrame(commandListCount);
}

// FIXED: Clean up the global hook instance if allocated
// Service the deferred ECL probe: if ProbeRealD3D12ECL was skipped because
// the Streamline startup window was active, try to probe now that the window
// has expired.  Safe to call from any thread at any time.
void DX12_ServiceDeferredECLProbe() {
    if (!g_ProbeRealD3D12ECLDeferred.load(std::memory_order_acquire)) {
        return;
    }
    if (DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        return;
    }
    auto* srvDev = g_Device.load(std::memory_order_acquire);
    if (srvDev && IsStreamlineLoaded()) {
        ProbeRealD3D12ECL(srvDev);
        auto* srvProbed = (void*)g_RealD3D12ECL.load(std::memory_order_acquire);
        g_ProbeRealD3D12ECLDeferred.store(false, std::memory_order_release);
        HookLogImportant("DX12: ServiceDeferredECLProbe — realECL=%p", srvProbed);
    }
}

DWORD WINAPI UnloadThread(LPVOID lpParam) {
    Sleep(200);
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Shutdown();
        delete g_dx12HookInstance;
        g_dx12HookInstance = nullptr;
    }
    return 0;
}
