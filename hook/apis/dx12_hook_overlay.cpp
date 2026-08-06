#include "dx12_hook_internal.h"
#include "dx12_hook_overlay_shared.h"


#include "dx12_hook_internal.h"

void ShutdownImGui() {
    if (!dx12_hook_g_State.overlayInit)
        return;

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
        dx12_hook_g_OverlayAdapterBackendDevice.store(nullptr, std::memory_order_release);
        dx12_hook_g_OverlayAdapterBackendQueue.store(nullptr, std::memory_order_release);
        dx12_hook_g_OverlayAdapterBackendFormat.store(static_cast<int>(DXGI_FORMAT_UNKNOWN), std::memory_order_release);
        dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(false, std::memory_order_release);
    }

    if (dx12_hook_g_State.srvDescHeap) {
        dx12_hook_g_State.srvDescHeap->Release();
        dx12_hook_g_State.srvDescHeap = nullptr;
    }
    dx12_hook_g_State.overlayInit = false;
}

bool InitImGui(ID3D12Device* device, int buffers, DXGI_FORMAT format, HWND hwnd) {
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);

    if (dx12_hook_g_State.overlayInit) {
        HookLog("InitImGui: Already initialized, returning early");
        return true;
    }

    HookLog(
        "InitImGui: Proceeding with initialization - buffers=%d, format=%d, "
        "hwnd=%p",
        buffers, format, hwnd);

    dx12_hook_g_State.format = format;

    // Use OverlayAdapter instead of ImGui
    // Rendering always goes through the game queue since GPU drivers require
    // swapchain writes from the owning queue. The overlay queue handles fence
    // management independently.
    // CRITICAL: Use same queue preference as ProcessFrame.
    // SL FG: use origGame (SL manages cross-queue internally).
    // FSR FG: use g_SwapchainQueue (FSR's swapchain uses FSR's queue;
    //   submitting on origGame causes cross-queue DEVICE_REMOVED).
    ID3D12CommandQueue* queueForBackend = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool fsrFGNow = IsFSRFrameGenerationActive();
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* currentPrimaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
        const bool postFSRInactiveRecoveryPending =
            dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
        const bool lastWorkingQueueStillActiveDuringRecentTeardown =
            dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
            GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);

        const auto routingDecision = ce::dx12_overlay_policy::DecideSwapchainOverlayRouting(
            dx12_hook_g_FGRuntimeOwnsSwapchain, slFGNow, fsrFGNow, dx12_hook_g_HadFSRFGPhase, dx12_hook_g_SwapchainQueue != nullptr,
            dx12_hook_g_OriginalGameQueue != nullptr, dx12_hook_g_PostSLLastWorkingQueue != nullptr, postFSRInactiveRecoveryPending,
            currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue,
            dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
            dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire));

        if (routingDecision == ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue) {
            // After FSR→DLSS: use scQueue (swapchain creation queue)
            queueForBackend = dx12_hook_g_SwapchainQueue ? dx12_hook_g_SwapchainQueue : dx12_hook_g_OriginalGameQueue;
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue) {
            // During SL FG, prefer scQueue when it differs from origGame.
            // SL may recreate the swapchain on its own internal queue.
            if (dx12_hook_g_SwapchainQueue && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue) {
                queueForBackend = dx12_hook_g_SwapchainQueue;
            } else if (dx12_hook_g_OriginalGameQueue) {
                queueForBackend = dx12_hook_g_OriginalGameQueue;
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue) {
            queueForBackend = dx12_hook_g_PostSLLastWorkingQueue;
            static std::atomic<int> s_postFSRBackendLastWorkingRouteLogCount{0};
            int logCount = s_postFSRBackendLastWorkingRouteLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: InitImGui — post-FSR inactive recovery epoch using preserved PostSL lastWorking queue %p "
                    "(cmdQ=%p origQ=%p primaryQ=%p recentTraffic=%d)",
                    dx12_hook_g_PostSLLastWorkingQueue, currentCommandQueue, dx12_hook_g_OriginalGameQueue, currentPrimaryQueue,
                    lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0);
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue) {
            const auto queueSource =
                ce::dx12_overlay_policy::DecidePostFSRInactiveRecoveryQueueSource(dx12_hook_g_OriginalGameQueue != nullptr);
            if (queueSource == ce::dx12_overlay_policy::PostFSRInactiveRecoveryQueueSource::kOriginalPresentQueue) {
                queueForBackend = dx12_hook_g_OriginalGameQueue;
                const bool explicitNativeFSROffPending =
                    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
                static std::atomic<int> s_postFSRBackendOrigRouteLogCount{0};
                int logCount = s_postFSRBackendOrigRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: InitImGui — post-FSR normal/recovery routing using original present queue %p "
                        "(cmdQ=%p primaryQ=%p recoveryPending=%d explicitNativeOff=%d)",
                        queueForBackend, currentCommandQueue, currentPrimaryQueue,
                        postFSRInactiveRecoveryPending ? 1 : 0, explicitNativeFSROffPending ? 1 : 0);
                }
            } else {
                queueForBackend = currentCommandQueue ? currentCommandQueue : currentPrimaryQueue;
                static std::atomic<int> s_postFSRBackendFallbackRouteLogCount{0};
                int logCount = s_postFSRBackendFallbackRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: InitImGui — post-FSR inactive recovery missing origGame, falling back to current "
                        "command queue %p (primaryQ=%p)",
                        queueForBackend, currentPrimaryQueue);
                }
            }
        } else if (routingDecision == ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue ||
                   routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseRuntimeOwnedSwapchainQueue) {
            queueForBackend = dx12_hook_g_SwapchainQueue;
        } else if (fsrFGNow && dx12_hook_g_OriginalGameQueue) {
            queueForBackend = dx12_hook_g_OriginalGameQueue;  // fallback
        } else {
            queueForBackend = dx12_hook_g_SwapchainQueue;
            if (!queueForBackend)
                queueForBackend = g_CommandQueue.load();
        }
    }
    HookLogImportant(
        "[Overlay] DX12: InitImGui backend queue=%p (origQ=%p, primaryQ=%p, scQueue=%p, cmdQueue=%p, "
        "lastWorkingQ=%p, slFG=%d, fgCooldown=%d)",
        queueForBackend, dx12_hook_g_OriginalGameQueue, dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), dx12_hook_g_SwapchainQueue,
        (void*)g_CommandQueue.load(), dx12_hook_g_PostSLLastWorkingQueue,
        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0,
        dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire));

    const bool preserveAcrossResize = dx12_hook_g_PreserveOverlayAdapterAcrossResize.exchange(false, std::memory_order_acq_rel);
    // Warm reuse is device+format scoped: the backend never uses its bound
    // queue (see CanReuseWarmDX12OverlayBackend), so FG transitions that move
    // the swapchain to a different queue still reuse the warm PSOs/font atlas.
    const bool canReuseWarmResizeBackend = ce::dx12_overlay_policy::CanReuseWarmDX12OverlayBackend(
        preserveAcrossResize, g_OverlayAdapter.IsInitialized(),
        dx12_hook_g_OverlayAdapterBackendDevice.load(std::memory_order_acquire) == device,
        dx12_hook_g_OverlayAdapterBackendFormat.load(std::memory_order_acquire) == static_cast<int>(format));
    if (g_OverlayAdapter.IsInitialized() && !canReuseWarmResizeBackend) {
        HookLogImportant(
            "InitImGui: OverlayAdapter already initialized, shutting down for re-init "
            "(preserveResize=%d device old=%p new=%p queue old=%p new=%p fmt old=%d new=%d)",
            preserveAcrossResize ? 1 : 0, dx12_hook_g_OverlayAdapterBackendDevice.load(std::memory_order_acquire), device,
            dx12_hook_g_OverlayAdapterBackendQueue.load(std::memory_order_acquire), queueForBackend,
            dx12_hook_g_OverlayAdapterBackendFormat.load(std::memory_order_acquire), static_cast<int>(format));
        g_OverlayAdapter.Shutdown();
        dx12_hook_g_OverlayAdapterBackendDevice.store(nullptr, std::memory_order_release);
        dx12_hook_g_OverlayAdapterBackendQueue.store(nullptr, std::memory_order_release);
        dx12_hook_g_OverlayAdapterBackendFormat.store(static_cast<int>(DXGI_FORMAT_UNKNOWN), std::memory_order_release);
    } else if (canReuseWarmResizeBackend) {
        HookLogImportant(
            "InitImGui: Reusing warm DX12 overlay backend across swapchain/queue transition "
            "(device=%p queue old=%p new=%p fmt=%d)",
            device, dx12_hook_g_OverlayAdapterBackendQueue.load(std::memory_order_acquire), queueForBackend,
            static_cast<int>(format));
    }

    g_OverlayAdapter.SetHwnd(hwnd);
    if (!g_OverlayAdapter.InitDX12(device, queueForBackend, format)) {
        HookLog("[Overlay] DX12: OverlayAdapter::InitDX12 FAILED (device=%p, queue=%p, fmt=%d)", device,
                queueForBackend, format);
        return false;
    }

    // OverlayAdapter handles its own initialization
    HookLog("[Overlay] DX12: OverlayAdapter::InitDX12 succeeded (hwnd=%p)", hwnd);
    dx12_hook_g_OverlayAdapterBackendDevice.store(device, std::memory_order_release);
    dx12_hook_g_OverlayAdapterBackendQueue.store(queueForBackend, std::memory_order_release);
    dx12_hook_g_OverlayAdapterBackendFormat.store(static_cast<int>(format), std::memory_order_release);

    InputManager::Get().HookWindow(hwnd);

    // We don't need SRV heap for ImGui anymore, OverlayAdapter manages its own
    // resources. But we might need it if we keep ImGui for menus? For now
    // assuming full replacement for overlay.

    dx12_hook_g_State.overlayInit = true;

    // Reset frame delay counter on reinitialization
    extern void DX12_ResetOverlayFrameDelay();
    DX12_ResetOverlayFrameDelay();

    return true;
}

void CreateRTVs(ID3D12Device* device, IDXGISwapChain3* swapChain, int bufferCount) {
    if (dx12_hook_g_State.rtvDescHeap)
        return;

    HookLogImportant("CreateRTVs: ENTER (bufferCount=%d)", bufferCount);

    // DLSS FG FIX: Validate buffer count before creating RTVs
    if (bufferCount <= 0 || bufferCount > 8) {
        HookLog("CreateRTVs: Invalid buffer count %d, limiting to 3", bufferCount);
        bufferCount = 3;
    }

    // Always create with capacity for 8 buffers.  SL's DLSS FG can create new
    // swapchains with more buffers after FG mode switches (e.g., 3→4).  Rather
    // than re-creating the heap each time, we allocate the max upfront.
    constexpr UINT kMaxRTVSlots = 8;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxRTVSlots,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    HRESULT rtvHeapHr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&dx12_hook_g_State.rtvDescHeap));
    if (FAILED(rtvHeapHr)) {
        HookLog("CreateRTVs: Failed to create RTV descriptor heap hr=0x%08X (count=%d)", rtvHeapHr, bufferCount);
        return;
    }
    dx12_hook_g_State.bufferCount = bufferCount;
    dx12_hook_g_State.cachedSwapChain = swapChain;
    dx12_hook_g_State.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    // FG-SAFE: Do NOT hold persistent references on backbuffers.
    // FSR FG monitors backbuffer reference counts and crashes if extra refs are held.
    // Create RTVs and release immediately — re-acquire per-frame in render path.
    dx12_hook_g_State.backBuffers.clear();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < bufferCount; i++) {
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) {
            device->CreateRenderTargetView(bb, nullptr, rtvHandle);
            bb->Release();  // Release immediately - RTV descriptor remains valid
        }
        rtvHandle.ptr += dx12_hook_g_State.rtvDescriptorSize;
    }
    HookLogImportant("CreateRTVs: Created %d RTVs (no held refs)", bufferCount);
}
