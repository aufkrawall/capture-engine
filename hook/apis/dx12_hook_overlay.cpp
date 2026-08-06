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

void InitOverlaySync(ID3D12Device* device, int bufferCount, ID3D12CommandQueue* gameQueue) {
    HookLogImportant("InitOverlaySync: ENTER (syncInit=%d, device=%p, gameQueue=%p)", dx12_hook_g_State.syncInit, device,
                     gameQueue);

    if (dx12_hook_g_State.syncInit) {
        HookLogImportant("InitOverlaySync: Already initialized, returning early");
        return;
    }
    dx12_hook_g_FocusLossImmediateFenceDumpRequested.store(false, std::memory_order_release);
    dx12_hook_g_FocusLossDeviceRemovalDumpRequested.store(false, std::memory_order_release);
    dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining.store(0, std::memory_order_release);
    dx12_hook_g_FocusLossRecentTransitionPresentWindow.store(0, std::memory_order_release);

    // CRITICAL: Prefer the queue's own device over g_Device.  After swapchain
    // recreation (e.g. FSR→DLSS switch), g_Device may still point to the old
    // device from the ECL hook while the swapchain queue belongs to a different
    // D3D12 device (SL wraps/creates devices independently).  Submitting command
    // lists created on device A to a queue on device B → DEVICE_REMOVED.
    ID3D12Device* queueDevice = nullptr;
    if (gameQueue) {
        if (SUCCEEDED(gameQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice) {
            if (queueDevice != device) {
                HookLogImportant("InitOverlaySync: Queue device %p DIFFERS from g_Device %p — using queue device",
                                 queueDevice, device);
                device = queueDevice;
            } else {
                HookLogImportant("InitOverlaySync: Queue device matches g_Device (%p)", device);
                queueDevice->Release();
                queueDevice = nullptr;
            }
        }
    }

    // CRITICAL: Flush any pending deferred Signal and wait for all GPU work to
    // complete BEFORE releasing D3D12 objects.  Without this, allocators/fences
    // can be released while the GPU is still executing commands that reference
    // them, causing ERR_GFX_STATE (especially during FG mode switches where
    // EnsureDedicatedOverlayQueueForFGCompat triggers a reinit).
    {
        UINT64 deferredVal = dx12_hook_g_deferredSignalValue.load(std::memory_order_acquire);
        if (deferredVal != 0 && dx12_hook_g_State.fence) {
            // Use the queue that submitted the deferred ECL, fall back to gameQueue
            ID3D12CommandQueue* sigQueue = dx12_hook_g_deferredSignalQueue.load(std::memory_order_acquire);
            if (!sigQueue)
                sigQueue = gameQueue;
            if (sigQueue) {
                HRESULT hr = sigQueue->Signal(dx12_hook_g_State.fence, deferredVal);
                if (SUCCEEDED(hr)) {
                    int allocIdx = dx12_hook_g_deferredSignalAllocIdx.load(std::memory_order_acquire);
                    dx12_hook_g_State.currentFenceValue = deferredVal;
                    if (allocIdx >= 0 && allocIdx < (int)dx12_hook_g_State.fenceValues.size())
                        dx12_hook_g_State.fenceValues[allocIdx] = deferredVal;
                }
            }
            dx12_hook_g_deferredSignalValue.store(0, std::memory_order_release);
            dx12_hook_g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
            dx12_hook_g_deferredSignalQueue.store(nullptr, std::memory_order_release);
        }

        if (dx12_hook_g_State.fence && dx12_hook_g_State.currentFenceValue > 0) {
            ID3D12CommandQueue* flushQueue = dx12_hook_g_State.overlayQueue ? dx12_hook_g_State.overlayQueue : gameQueue;
            if (flushQueue) {
                UINT64 waitValue = dx12_hook_g_State.currentFenceValue;
                if (dx12_hook_g_State.fence->GetCompletedValue() < waitValue) {
                    HANDLE waitEvent =
                        dx12_hook_g_State.fenceEvent ? dx12_hook_g_State.fenceEvent : CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    bool createdEvent = (waitEvent != dx12_hook_g_State.fenceEvent);
                    if (waitEvent) {
                        dx12_hook_g_State.fence->SetEventOnCompletion(waitValue, waitEvent);
                        WaitForSingleObject(waitEvent, 200);
                        if (createdEvent)
                            CloseHandle(waitEvent);
                    }
                    HookLogImportant("InitOverlaySync: Waited for GPU completion (fenceValue=%llu, completed=%llu)",
                                     (unsigned long long)waitValue,
                                     (unsigned long long)dx12_hook_g_State.fence->GetCompletedValue());
                }
            }
        }
    }

    // Release any previously allocated sync resources to prevent leaks when
    // syncInit was cleared by a resize or error path without calling Shutdown.
    if (dx12_hook_g_State.fence) {
        if (dx12_hook_g_State.fenceEvent) {
            CloseHandle(dx12_hook_g_State.fenceEvent);
            dx12_hook_g_State.fenceEvent = nullptr;
        }
        dx12_hook_g_State.fence->Release();
        dx12_hook_g_State.fence = nullptr;
    }
    if (dx12_hook_g_State.cmdList) {
        dx12_hook_g_State.cmdList->Release();
        dx12_hook_g_State.cmdList = nullptr;
    }
    for (auto* a : dx12_hook_g_State.allocators)
        if (a)
            a->Release();
    dx12_hook_g_State.allocators.clear();
    dx12_hook_g_State.fenceValues.clear();

    // Release previous overlay queue if any
    if (dx12_hook_g_State.overlayQueue) {
        dx12_hook_g_State.overlayQueue->Release();
        dx12_hook_g_State.overlayQueue = nullptr;
    }
    // Release previous cross-queue fence and event if any
    if (dx12_hook_g_State.crossQueueFenceEvent) {
        CloseHandle(dx12_hook_g_State.crossQueueFenceEvent);
        dx12_hook_g_State.crossQueueFenceEvent = nullptr;
    }
    if (dx12_hook_g_State.crossQueueFence) {
        dx12_hook_g_State.crossQueueFence->Release();
        dx12_hook_g_State.crossQueueFence = nullptr;
    };

    dx12_hook_g_State.crossQueueFenceValue = 0;

    // Dedicated overlay queue: when FG is active, overlay commands execute on
    // this queue instead of the game queue.  This avoids interfering with
    // Streamline's game queue management.  CPU-side fence waits provide
    // cross-queue synchronization (GPU-side Wait was removed due to NVIDIA
    // WaitImpl Alt+Tab hangs). Third-party overlays can also insert their own
    // queue transitions, so stay on the game queue in that compatibility mode.
    const char* overlayModule = nullptr;
    const char* skipSeparateOverlayGpuReason = nullptr;
    const bool skipSeparateOverlayGpuWork =
        ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(&skipSeparateOverlayGpuReason);
    if (!ShouldUseDedicatedOverlayQueue(&overlayModule)) {
        if (skipSeparateOverlayGpuWork) {
            static std::atomic<int> s_runtimeOwnedDedicatedQueueSkipLogCount{0};
            const int logCount = s_runtimeOwnedDedicatedQueueSkipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "InitOverlaySync: Dedicated queue intentionally disabled because %s is active; keeping "
                    "single-queue sync resources idle (log=%d)",
                    skipSeparateOverlayGpuReason ? skipSeparateOverlayGpuReason : "runtime-owned FG", logCount + 1);
            }
            dx12_hook_g_State.currentFenceValue = 0;
            dx12_hook_g_State.allocIndex = 0;
            dx12_hook_g_State.syncInit = false;
            dx12_hook_g_State.syncDevice = nullptr;
            if (queueDevice) {
                queueDevice->Release();
                queueDevice = nullptr;
            }
            return;
        } else if (IsActualFrameGenerationActive()) {
            HookLogImportant(
                "InitOverlaySync: FG active (dedicated queue disabled for runtime-owned/native FG), using "
                "single-queue overlay mode on game queue (fsrFG=%d, runtimeOwns=%d)",
                IsFSRFrameGenerationActive() ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0);
        } else if (overlayModule) {
            HookLogImportant(
                "InitOverlaySync: Real FG inactive while external overlay %s is present, using single-queue overlay "
                "mode",
                overlayModule);
        } else {
            HookLogImportant("InitOverlaySync: Real FG inactive, using single-queue overlay mode");
        }
    } else {
        const char* startupOverlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
        const bool processNeedsStartupCompatDelay =
            ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
        if (!IsActualFrameGenerationActive() && processNeedsStartupCompatDelay && startupOverlayModule) {
            HookLogImportant("InitOverlaySync: Using dedicated overlay queue for %s startup compatibility with %s",
                             g_ProcessName, startupOverlayModule);
        } else {
            HookLog("InitOverlaySync: Creating dedicated overlay command queue");
        }
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (g_pSharedMem) {
            int32_t copyPrio = g_pSharedMem->GetCopyQueuePriority();
            if (copyPrio == 2) {
                queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
                HookLog("InitOverlaySync: Using high priority for overlay queue (copy_queue_priority=high)");
            }
        }
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        // Get the node mask from the game queue if available
        if (gameQueue) {
            D3D12_COMMAND_QUEUE_DESC gameQueueDesc = gameQueue->GetDesc();
            queueDesc.NodeMask = gameQueueDesc.NodeMask;
            HookLog("InitOverlaySync: Using node mask 0x%X from game queue", queueDesc.NodeMask);
        }

        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dx12_hook_g_State.overlayQueue)))) {
            HookLog("InitOverlaySync: FAILED to create overlay queue, falling back to single-queue");
            // Continue without overlay queue - will fall back to game queue
        } else {
            dx12_hook_g_State.overlayQueue->SetName(L"CE_OverlayQueue");
            HookLogImportant("InitOverlaySync: Dedicated overlay queue created (ptr=%p, gameQueue=%p)",
                             dx12_hook_g_State.overlayQueue, gameQueue);
        }
    }

    // Cross-queue fence: used for two purposes:
    // 1. Dedicated overlay queue: game queue signals before overlay queue starts
    // 2. PostSL cross-queue sync: scQueue signals before overlay draws on game queue
    // Always create regardless of overlayQueue — PostSL needs it after FG transitions
    // when scQueue (SL's FG queue) differs from the overlay submission queue.
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx12_hook_g_State.crossQueueFence)))) {
        HookLog("InitOverlaySync: FAILED to create cross-queue fence");
    }
    dx12_hook_g_State.crossQueueFenceValue = 0;
    if (dx12_hook_g_State.crossQueueFence) {
        dx12_hook_g_State.crossQueueFenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!dx12_hook_g_State.crossQueueFenceEvent) {
            HookLog("InitOverlaySync: FAILED to create cross-queue fence event");
            dx12_hook_g_State.crossQueueFence->Release();
            dx12_hook_g_State.crossQueueFence = nullptr;
        }
    }

    // Overlay completion fence: separate from g_State.fence, used to track
    // overlay GPU work completion during FG modes.  The main g_State.fence
    // signal is skipped during FG (to avoid FG-pipeline desync), but this
    // fence is signaled via the raw D3D12 Signal pointer and CPU-waited to
    // ensure the overlay draws complete before the FG runtime reads the
    // swapchain backbuffer.  Created once per device session.
    if (!dx12_hook_g_OverlayCompletionFence.load(std::memory_order_acquire)) {
        ID3D12Fence* fence = nullptr;
        if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            dx12_hook_g_OverlayCompletionFence.store(fence, std::memory_order_release);
            HookLog("InitOverlaySync: Created overlay completion fence (ptr=%p)", fence);
        } else {
            HookLog("InitOverlaySync: FAILED to create overlay completion fence");
        }
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx12_hook_g_State.fence))))
        return;

    size_t allocatorPoolSize = DX12OverlayState::ALLOC_POOL_SIZE;
    const bool startupOverlayPresent = (overlayModule != nullptr);
    const bool startupOverlayCompatSettled = dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire);
    const bool processNeedsStartupCompatDelay =
        ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
    allocatorPoolSize = ce::overlay_compat::GetStartupCompatibleDX12AllocatorPoolSize(
        processNeedsStartupCompatDelay, startupOverlayPresent, IsActualFrameGenerationActive(),
        startupOverlayCompatSettled, DX12OverlayState::ALLOC_POOL_SIZE);
    if (allocatorPoolSize < DX12OverlayState::ALLOC_POOL_SIZE) {
        HookLogImportant("InitOverlaySync: Using minimal %u-slot allocator pool for startup overlay %s",
                         static_cast<unsigned>(allocatorPoolSize), overlayModule ? overlayModule : "module");
    } else if (startupOverlayPresent && startupOverlayCompatSettled) {
        static std::atomic<int> s_startupSettledAllocatorLogCount{0};
        if (s_startupSettledAllocatorLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant(
                "InitOverlaySync: Startup overlay %s already settled - keeping full %u-slot allocator pool",
                overlayModule ? overlayModule : "module", static_cast<unsigned>(DX12OverlayState::ALLOC_POOL_SIZE));
        }
    }

    dx12_hook_g_State.allocators.resize(allocatorPoolSize);
    dx12_hook_g_State.fenceValues.resize(allocatorPoolSize);

    // CRITICAL FIX: Reset all fence values to 0 for fresh start
    // After resize/reinit, old fence values could be stale and cause infinite
    // waits
    std::fill(dx12_hook_g_State.fenceValues.begin(), dx12_hook_g_State.fenceValues.end(), 0);
    dx12_hook_g_State.currentFenceValue = 0;
    dx12_hook_g_State.allocIndex = 0;

    HookLog("InitOverlaySync: Fence values reset to 0, currentFenceValue=0");

    bool success = true;
    for (size_t i = 0; i < allocatorPoolSize; i++) {
        if (FAILED(
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dx12_hook_g_State.allocators[i])))) {
            success = false;
            break;
        }
    }

    if (success) {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dx12_hook_g_State.allocators[0], nullptr,
                                             IID_PPV_ARGS(&dx12_hook_g_State.cmdList)))) {
            success = false;
        }
    }

    if (success) {
        dx12_hook_g_State.cmdList->Close();
        dx12_hook_g_State.fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

        // Track which device owns these sync resources so we can detect
        // cross-device submission attempts in PostSLOverlayRender.
        dx12_hook_g_State.syncDevice = device;

        // Name CE-owned overlay objects so DRED auto-breadcrumbs / page-fault
        // output identify the hung list/allocator/fence as ours vs the game's.
        dx12_hook_g_State.fence->SetName(L"CE_OverlayFence");
        dx12_hook_g_State.cmdList->SetName(L"CE_OverlayCmdList");
        for (size_t i = 0; i < dx12_hook_g_State.allocators.size(); i++) {
            if (dx12_hook_g_State.allocators[i]) {
                wchar_t allocName[48];
                swprintf(allocName, 48, L"CE_OverlayAlloc[%u]", (unsigned)i);
                dx12_hook_g_State.allocators[i]->SetName(allocName);
            }
        }
        // Fresh sync/device epoch — allow a future device-removal to dump DRED again.
        ce::dx12_dred::ResetDumpEpoch();

        dx12_hook_g_State.syncInit = true;
        HookLogImportant("InitOverlaySync: SUCCESS (syncDevice=%p, allocators=%d, fence=%p)", device,
                         (int)dx12_hook_g_State.allocators.size(), dx12_hook_g_State.fence);
    } else {
        // Cleanup partial initialization
        for (auto* alloc : dx12_hook_g_State.allocators)
            if (alloc)
                alloc->Release();
        dx12_hook_g_State.allocators.clear();
        dx12_hook_g_State.fenceValues.clear();
        if (dx12_hook_g_State.cmdList) {
            dx12_hook_g_State.cmdList->Release();
            dx12_hook_g_State.cmdList = nullptr;
        }
        if (dx12_hook_g_State.fence) {
            dx12_hook_g_State.fence->Release();
            dx12_hook_g_State.fence = nullptr;
        }
    }

    // Release the extra ref from QI if we got the device from the queue
    if (queueDevice) {
        queueDevice->Release();
        queueDevice = nullptr;
    }
}
static bool DrainCommandQueue(ID3D12CommandQueue* queue, ID3D12Device* device) {
    if (!queue || !device)
        return false;

    // NON-BLOCKING DRAIN: Use a flush approach instead of waiting
    // to avoid deadlocking when called from the submit thread.
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;

    // Signal the fence but don't wait - if we're on the submit thread,
    // waiting would deadlock. The fence will be processed when the
    // game next submits work.
    queue->Signal(fence, 1);

    // Quick check if already completed (GPU was idle)
    if (fence->GetCompletedValue() >= 1) {
        fence->Release();
        return true;
    }

    // Optional: very short wait for already-in-flight work (1ms)
    // This helps if the GPU is just finishing up, without blocking
    // the submit thread for long.
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (event) {
        if (fence->SetEventOnCompletion(1, event) == S_OK) {
            WaitForSingleObject(event, 1);  // 1ms non-blocking wait
        }
        CloseHandle(event);
    }
    fence->Release();
    return true;
}
void CleanupOverlay(bool preserveNativeFSRPresentCallbackBackend) {
    auto cleanupFFXPresentCallbackBackend = [preserveNativeFSRPresentCallbackBackend]() {
        if (preserveNativeFSRPresentCallbackBackend) {
            return;
        }
        if (dx12_hook_g_FFXPresentRtvHeap) {
            dx12_hook_g_FFXPresentRtvHeap->Release();
            dx12_hook_g_FFXPresentRtvHeap = nullptr;
        }
        if (dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized()) {
            dx12_hook_g_FFXPresentOverlayAdapter.Shutdown();
        }
        dx12_hook_g_FFXPresentOverlayDevice = nullptr;
        dx12_hook_g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;
    };

    if (!dx12_hook_g_State.syncInit) {
        cleanupFFXPresentCallbackBackend();
        return;
    }

    // Dedicated overlay queue: flush overlay queue instead of game queue
    ID3D12CommandQueue* queueToFlush = dx12_hook_g_State.overlayQueue;
    if (!queueToFlush) {
        // Fallback to game queue if no dedicated overlay queue
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        queueToFlush = g_CommandQueue;
    }

    if (dx12_hook_g_State.fence && queueToFlush) {
        UINT64 waitValue = dx12_hook_g_State.currentFenceValue + 1;
        if (SUCCEEDED(queueToFlush->Signal(dx12_hook_g_State.fence, waitValue))) {
            if (dx12_hook_g_State.fence->GetCompletedValue() < waitValue) {
                dx12_hook_g_State.fence->SetEventOnCompletion(waitValue, dx12_hook_g_State.fenceEvent);
                WaitForSingleObject(dx12_hook_g_State.fenceEvent, 200);
            }
        }
    }
    if (dx12_hook_g_State.fenceEvent) {
        CloseHandle(dx12_hook_g_State.fenceEvent);
        dx12_hook_g_State.fenceEvent = NULL;
    }
    for (auto alloc : dx12_hook_g_State.allocators)
        if (alloc)
            alloc->Release();
    dx12_hook_g_State.allocators.clear();
    dx12_hook_g_State.fenceValues.clear();
    if (dx12_hook_g_State.cmdList) {
        dx12_hook_g_State.cmdList->Release();
        dx12_hook_g_State.cmdList = nullptr;
    }
    if (dx12_hook_g_State.fence) {
        dx12_hook_g_State.fence->Release();
        dx12_hook_g_State.fence = nullptr;
    }
    // Release dedicated overlay queue
    if (dx12_hook_g_State.overlayQueue) {
        dx12_hook_g_State.overlayQueue->Release();
        dx12_hook_g_State.overlayQueue = nullptr;
    }
    // Release cross-queue synchronization fence
    if (dx12_hook_g_State.crossQueueFence) {
        dx12_hook_g_State.crossQueueFence->Release();
        dx12_hook_g_State.crossQueueFence = nullptr;
    }
    cleanupFFXPresentCallbackBackend();
    dx12_hook_g_State.currentFenceValue = 0;
    dx12_hook_g_State.crossQueueFenceValue = 0;
    dx12_hook_g_State.allocIndex = 0;
    dx12_hook_g_State.syncInit = false;
    dx12_hook_g_State.syncDevice = nullptr;
    dx12_hook_g_State.cachedSC3 = nullptr;
    // Discard any pending deferred Signal — fence is being released
    dx12_hook_g_deferredSignalValue.store(0, std::memory_order_release);
    dx12_hook_g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    dx12_hook_g_deferredSignalQueue.store(nullptr, std::memory_order_release);
    ResetStartupOverlayBackendActivationStage();

    // Clean up piggyback state
    dx12_hook_g_PiggybackOverlayActive.store(false, std::memory_order_relaxed);
}
void CleanupRTVs() {
    // FG-SAFE: backBuffers no longer holds references (released at create time)
    dx12_hook_g_State.backBuffers.clear();
    if (g_DummyBackBuffer) {
        g_DummyBackBuffer->Release();
        g_DummyBackBuffer = nullptr;
    }
    // Clean up D3D11On12 overlay bridge (holds wrapped backbuffer references)
    CleanupD3D11On12();
    // Release offscreen compositing resources
    if (dx12_hook_g_State.offscreenRT) {
        dx12_hook_g_State.offscreenRT->Release();
        dx12_hook_g_State.offscreenRT = nullptr;
    }
    if (dx12_hook_g_State.offscreenRtvHeap) {
        dx12_hook_g_State.offscreenRtvHeap->Release();
        dx12_hook_g_State.offscreenRtvHeap = nullptr;
    }
    dx12_hook_g_State.offscreenWidth = 0;
    dx12_hook_g_State.offscreenHeight = 0;
    dx12_hook_g_State.offscreenFormat = DXGI_FORMAT_UNKNOWN;
    if (dx12_hook_g_State.rtvDescHeap) {
        dx12_hook_g_State.rtvDescHeap->Release();
        dx12_hook_g_State.rtvDescHeap = nullptr;
    }
    if (dx12_hook_g_State.srvDescHeap) {
        dx12_hook_g_State.srvDescHeap->Release();
        dx12_hook_g_State.srvDescHeap = nullptr;
    }
    dx12_hook_g_State.bufferCount = 0;
    dx12_hook_g_State.cachedSwapChain = nullptr;
    dx12_hook_g_State.cachedSC3 = nullptr;
}

