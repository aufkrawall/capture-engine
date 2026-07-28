    // (EnsureDescFreeBackendForDeviceAndFormat), x86 Texture2D selection, and
    // DX12Hook::Shutdown. The x86 Texture2D adapter (no DescFree backend
    // tracked) keeps its original swapchain-scoped teardown.
    if (!g_DescFreeBackend) {
        ShutdownDescFreeBackend("CleanupD3D11On12", true);
    }
    // Clean up SL FG D3D11On12 adapter
    if (g_SLFGAdapter.IsInitialized()) {
        g_SLFGAdapter.SetShutdownMode(true);
        g_SLFGAdapter.Shutdown();
    }
    // Device-level D3D11On12 cleanup happens in g_State.Cleanup()
}

void CreateRTVs(ID3D12Device* device, IDXGISwapChain3* swapChain, int bufferCount) {
    if (g_State.rtvDescHeap)
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
    HRESULT rtvHeapHr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_State.rtvDescHeap));
    if (FAILED(rtvHeapHr)) {
        HookLog("CreateRTVs: Failed to create RTV descriptor heap hr=0x%08X (count=%d)", rtvHeapHr, bufferCount);
        return;
    }
    g_State.bufferCount = bufferCount;
    g_State.cachedSwapChain = swapChain;
    g_State.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    // FG-SAFE: Do NOT hold persistent references on backbuffers.
    // FSR FG monitors backbuffer reference counts and crashes if extra refs are held.
    // Create RTVs and release immediately — re-acquire per-frame in render path.
    g_State.backBuffers.clear();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < bufferCount; i++) {
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) {
            device->CreateRenderTargetView(bb, nullptr, rtvHandle);
            bb->Release();  // Release immediately - RTV descriptor remains valid
        }
        rtvHandle.ptr += g_State.rtvDescriptorSize;
    }
    HookLogImportant("CreateRTVs: Created %d RTVs (no held refs)", bufferCount);
}

void InitOverlaySync(ID3D12Device* device, int bufferCount, ID3D12CommandQueue* gameQueue) {
    HookLogImportant("InitOverlaySync: ENTER (syncInit=%d, device=%p, gameQueue=%p)", g_State.syncInit, device,
                     gameQueue);

    if (g_State.syncInit) {
        HookLogImportant("InitOverlaySync: Already initialized, returning early");
        return;
    }
    g_FocusLossImmediateFenceDumpRequested.store(false, std::memory_order_release);
    g_FocusLossDeviceRemovalDumpRequested.store(false, std::memory_order_release);
    g_FocusLossForegroundReacquirePresentProofRemaining.store(0, std::memory_order_release);
    g_FocusLossRecentTransitionPresentWindow.store(0, std::memory_order_release);

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
        UINT64 deferredVal = g_deferredSignalValue.load(std::memory_order_acquire);
        if (deferredVal != 0 && g_State.fence) {
            // Use the queue that submitted the deferred ECL, fall back to gameQueue
            ID3D12CommandQueue* sigQueue = g_deferredSignalQueue.load(std::memory_order_acquire);
            if (!sigQueue)
                sigQueue = gameQueue;
            if (sigQueue) {
                HRESULT hr = sigQueue->Signal(g_State.fence, deferredVal);
                if (SUCCEEDED(hr)) {
                    int allocIdx = g_deferredSignalAllocIdx.load(std::memory_order_acquire);
                    g_State.currentFenceValue = deferredVal;
                    if (allocIdx >= 0 && allocIdx < (int)g_State.fenceValues.size())
                        g_State.fenceValues[allocIdx] = deferredVal;
                }
            }
            g_deferredSignalValue.store(0, std::memory_order_release);
            g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
            g_deferredSignalQueue.store(nullptr, std::memory_order_release);
        }

        if (g_State.fence && g_State.currentFenceValue > 0) {
            ID3D12CommandQueue* flushQueue = g_State.overlayQueue ? g_State.overlayQueue : gameQueue;
            if (flushQueue) {
                UINT64 waitValue = g_State.currentFenceValue;
                if (g_State.fence->GetCompletedValue() < waitValue) {
                    HANDLE waitEvent =
                        g_State.fenceEvent ? g_State.fenceEvent : CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    bool createdEvent = (waitEvent != g_State.fenceEvent);
                    if (waitEvent) {
                        g_State.fence->SetEventOnCompletion(waitValue, waitEvent);
                        WaitForSingleObject(waitEvent, 200);
                        if (createdEvent)
                            CloseHandle(waitEvent);
                    }
                    HookLogImportant("InitOverlaySync: Waited for GPU completion (fenceValue=%llu, completed=%llu)",
                                     (unsigned long long)waitValue,
                                     (unsigned long long)g_State.fence->GetCompletedValue());
                }
            }
        }
    }

    // Release any previously allocated sync resources to prevent leaks when
    // syncInit was cleared by a resize or error path without calling Shutdown.
    if (g_State.fence) {
        if (g_State.fenceEvent) {
            CloseHandle(g_State.fenceEvent);
            g_State.fenceEvent = nullptr;
        }
        g_State.fence->Release();
        g_State.fence = nullptr;
    }
    if (g_State.cmdList) {
        g_State.cmdList->Release();
        g_State.cmdList = nullptr;
    }
    for (auto* a : g_State.allocators)
        if (a)
            a->Release();
    g_State.allocators.clear();
    g_State.fenceValues.clear();

    // Release previous overlay queue if any
    if (g_State.overlayQueue) {
        g_State.overlayQueue->Release();
        g_State.overlayQueue = nullptr;
    }
    // Release previous cross-queue fence and event if any
    if (g_State.crossQueueFenceEvent) {
        CloseHandle(g_State.crossQueueFenceEvent);
        g_State.crossQueueFenceEvent = nullptr;
    }
    if (g_State.crossQueueFence) {
        g_State.crossQueueFence->Release();
        g_State.crossQueueFence = nullptr;
    };

    g_State.crossQueueFenceValue = 0;

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
            g_State.currentFenceValue = 0;
            g_State.allocIndex = 0;
            g_State.syncInit = false;
            g_State.syncDevice = nullptr;
            if (queueDevice) {
                queueDevice->Release();
                queueDevice = nullptr;
            }
            return;
        } else if (IsActualFrameGenerationActive()) {
            HookLogImportant(
                "InitOverlaySync: FG active (dedicated queue disabled for runtime-owned/native FG), using "
                "single-queue overlay mode on game queue (fsrFG=%d, runtimeOwns=%d)",
                IsFSRFrameGenerationActive() ? 1 : 0, g_FGRuntimeOwnsSwapchain ? 1 : 0);
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

        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_State.overlayQueue)))) {
            HookLog("InitOverlaySync: FAILED to create overlay queue, falling back to single-queue");
            // Continue without overlay queue - will fall back to game queue
        } else {
            g_State.overlayQueue->SetName(L"CE_OverlayQueue");
            HookLogImportant("InitOverlaySync: Dedicated overlay queue created (ptr=%p, gameQueue=%p)",
                             g_State.overlayQueue, gameQueue);
        }
    }

    // Cross-queue fence: used for two purposes:
    // 1. Dedicated overlay queue: game queue signals before overlay queue starts
    // 2. PostSL cross-queue sync: scQueue signals before overlay draws on game queue
    // Always create regardless of overlayQueue — PostSL needs it after FG transitions
    // when scQueue (SL's FG queue) differs from the overlay submission queue.
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.crossQueueFence)))) {
        HookLog("InitOverlaySync: FAILED to create cross-queue fence");
    }
    g_State.crossQueueFenceValue = 0;
    if (g_State.crossQueueFence) {
        g_State.crossQueueFenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!g_State.crossQueueFenceEvent) {
            HookLog("InitOverlaySync: FAILED to create cross-queue fence event");
            g_State.crossQueueFence->Release();
            g_State.crossQueueFence = nullptr;
        }
    }

    // Overlay completion fence: separate from g_State.fence, used to track
    // overlay GPU work completion during FG modes.  The main g_State.fence
    // signal is skipped during FG (to avoid FG-pipeline desync), but this
    // fence is signaled via the raw D3D12 Signal pointer and CPU-waited to
    // ensure the overlay draws complete before the FG runtime reads the
    // swapchain backbuffer.  Created once per device session.
    if (!g_OverlayCompletionFence.load(std::memory_order_acquire)) {
        ID3D12Fence* fence = nullptr;
        if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            g_OverlayCompletionFence.store(fence, std::memory_order_release);
            HookLog("InitOverlaySync: Created overlay completion fence (ptr=%p)", fence);
        } else {
            HookLog("InitOverlaySync: FAILED to create overlay completion fence");
        }
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.fence))))
        return;

    size_t allocatorPoolSize = DX12OverlayState::ALLOC_POOL_SIZE;
    const bool startupOverlayPresent = (overlayModule != nullptr);
    const bool startupOverlayCompatSettled = s_startupOverlayCompatSettled.load(std::memory_order_acquire);
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

    g_State.allocators.resize(allocatorPoolSize);
    g_State.fenceValues.resize(allocatorPoolSize);

    // CRITICAL FIX: Reset all fence values to 0 for fresh start
    // After resize/reinit, old fence values could be stale and cause infinite
    // waits
    std::fill(g_State.fenceValues.begin(), g_State.fenceValues.end(), 0);
    g_State.currentFenceValue = 0;
    g_State.allocIndex = 0;

    HookLog("InitOverlaySync: Fence values reset to 0, currentFenceValue=0");

    bool success = true;
    for (size_t i = 0; i < allocatorPoolSize; i++) {
        if (FAILED(
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_State.allocators[i])))) {
            success = false;
            break;
        }
    }

    if (success) {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_State.allocators[0], nullptr,
                                             IID_PPV_ARGS(&g_State.cmdList)))) {
            success = false;
        }
    }

    if (success) {
        g_State.cmdList->Close();
        g_State.fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

        // Track which device owns these sync resources so we can detect
        // cross-device submission attempts in PostSLOverlayRender.
        g_State.syncDevice = device;

        // Name CE-owned overlay objects so DRED auto-breadcrumbs / page-fault
        // output identify the hung list/allocator/fence as ours vs the game's.
        g_State.fence->SetName(L"CE_OverlayFence");
        g_State.cmdList->SetName(L"CE_OverlayCmdList");
        for (size_t i = 0; i < g_State.allocators.size(); i++) {
            if (g_State.allocators[i]) {
                wchar_t allocName[48];
                swprintf(allocName, 48, L"CE_OverlayAlloc[%u]", (unsigned)i);
                g_State.allocators[i]->SetName(allocName);
            }
        }
        // Fresh sync/device epoch — allow a future device-removal to dump DRED again.
        ce::dx12_dred::ResetDumpEpoch();

        g_State.syncInit = true;
        HookLogImportant("InitOverlaySync: SUCCESS (syncDevice=%p, allocators=%d, fence=%p)", device,
                         (int)g_State.allocators.size(), g_State.fence);
    } else {
        // Cleanup partial initialization
        for (auto* alloc : g_State.allocators)
            if (alloc)
                alloc->Release();
        g_State.allocators.clear();
        g_State.fenceValues.clear();
        if (g_State.cmdList) {
            g_State.cmdList->Release();
            g_State.cmdList = nullptr;
        }
        if (g_State.fence) {
            g_State.fence->Release();
            g_State.fence = nullptr;
        }
    }

    // Release the extra ref from QI if we got the device from the queue
    if (queueDevice) {
        queueDevice->Release();
        queueDevice = nullptr;
    }
}

static bool PrewarmPostSLOverlayForFreshStreamlineHandoff(IDXGISwapChain* swapChain, ID3D12CommandQueue* swapchainQueue,
                                                          const char* context) {
    if (!swapChain || !swapchainQueue) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    const HRESULT descHr = swapChain->GetDesc(&desc);
    if (FAILED(descHr) || desc.BufferCount == 0 || desc.BufferCount > 8) {
        HookLogImportant(
            "DX12: PostSL handoff prewarm refused invalid swapchain description "
            "(source=%s sc=%p queue=%p hr=0x%08X buffers=%u)",
            context ? context : "unknown", swapChain, swapchainQueue, (unsigned)descHr, desc.BufferCount);
        return false;
    }

    ID3D12Device* queueDevice = nullptr;
    const HRESULT deviceHr = swapchainQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
    IDXGISwapChain3* swapChain3 = nullptr;
    const HRESULT sc3Hr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
    if (FAILED(deviceHr) || !queueDevice || FAILED(sc3Hr) || !swapChain3) {
        HookLogImportant(
            "DX12: PostSL handoff prewarm missing exact queue/swapchain prerequisites "
            "(source=%s sc=%p queue=%p deviceHr=0x%08X sc3Hr=0x%08X)",
            context ? context : "unknown", swapChain, swapchainQueue, (unsigned)deviceHr, (unsigned)sc3Hr);
        if (swapChain3) {
            swapChain3->Release();
        }
        if (queueDevice) {
            queueDevice->Release();
        }
        return false;
    }

    const ULONGLONG startedMs = GetTickCount64();
    bool ready = false;
    bool overlayInit = false;
    bool syncInit = false;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    {
        std::lock_guard<std::recursive_mutex> overlayLock(g_OverlayMutex);
        const HRESULT deviceReason = queueDevice->GetDeviceRemovedReason();
        if (SUCCEEDED(deviceReason)) {
            // The adapter owns only device/format-scoped objects. Reuse it even when the Streamline proxy uses a
            // different queue, then create the new swapchain RTVs and allocator/fence set without recording or
            // submitting an overlay draw. This completes before slDLSSGSetOptions(ON), so the first generated
            // Present cannot race a backend shutdown/rebuild.
            g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
            g_State.cachedWidth = desc.BufferDesc.Width;
            g_State.cachedHeight = desc.BufferDesc.Height;
            g_State.format = desc.BufferDesc.Format;

            const bool backendReady =
                InitImGui(queueDevice, static_cast<int>(desc.BufferCount), desc.BufferDesc.Format, desc.OutputWindow);
            if (backendReady) {
                CreateRTVs(queueDevice, swapChain3, static_cast<int>(desc.BufferCount));
                if (g_State.rtvDescHeap) {
                    InitOverlaySync(queueDevice, static_cast<int>(desc.BufferCount), swapchainQueue);
                }
            }
            ready = backendReady && g_State.overlayInit && g_State.syncInit && g_State.rtvDescHeap && g_State.cmdList &&
                    !g_State.allocators.empty();
            if (!ready && !g_State.rtvDescHeap) {
                // Make the normal first-PostSL bootstrap retry the complete swapchain-scoped setup. Preserve the
                // warm adapter if initialization itself succeeded; InitImGui will reuse it on that retry.
                g_State.overlayInit = false;
                g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
            }
            overlayInit = g_State.overlayInit;
            syncInit = g_State.syncInit;
            rtvHeap = g_State.rtvDescHeap;
        } else {
            HookLogImportant(
                "DX12: PostSL handoff prewarm refused removed device "
                "(source=%s device=%p hr=0x%08X)",
                context ? context : "unknown", queueDevice, (unsigned)deviceReason);
        }
    }

    HookLogImportant(
        "DX12: PostSL handoff prewarm %s before DLSS enable "
        "(source=%s sc=%p queue=%p device=%p fmt=%d buffers=%u elapsed=%llums init=%d sync=%d rtv=%p)",
        ready ? "READY" : "INCOMPLETE", context ? context : "unknown", swapChain, swapchainQueue, queueDevice,
        static_cast<int>(desc.BufferDesc.Format), desc.BufferCount,
        static_cast<unsigned long long>(GetTickCount64() - startedMs), overlayInit ? 1 : 0, syncInit ? 1 : 0, rtvHeap);

    swapChain3->Release();
    queueDevice->Release();
    return ready;
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
        if (g_FFXPresentRtvHeap) {
            g_FFXPresentRtvHeap->Release();
            g_FFXPresentRtvHeap = nullptr;
        }
        if (g_FFXPresentOverlayAdapter.IsInitialized()) {
            g_FFXPresentOverlayAdapter.Shutdown();
        }
        g_FFXPresentOverlayDevice = nullptr;
        g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;
    };

    if (!g_State.syncInit) {
        cleanupFFXPresentCallbackBackend();
        return;
    }

    // Dedicated overlay queue: flush overlay queue instead of game queue
    ID3D12CommandQueue* queueToFlush = g_State.overlayQueue;
    if (!queueToFlush) {
        // Fallback to game queue if no dedicated overlay queue
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        queueToFlush = g_CommandQueue;
    }

    if (g_State.fence && queueToFlush) {
        UINT64 waitValue = g_State.currentFenceValue + 1;
        if (SUCCEEDED(queueToFlush->Signal(g_State.fence, waitValue))) {
            if (g_State.fence->GetCompletedValue() < waitValue) {
                g_State.fence->SetEventOnCompletion(waitValue, g_State.fenceEvent);
                WaitForSingleObject(g_State.fenceEvent, 200);
            }
        }
    }
    if (g_State.fenceEvent) {
        CloseHandle(g_State.fenceEvent);
        g_State.fenceEvent = NULL;
    }
    for (auto alloc : g_State.allocators)
        if (alloc)
            alloc->Release();
    g_State.allocators.clear();
    g_State.fenceValues.clear();
    if (g_State.cmdList) {
        g_State.cmdList->Release();
        g_State.cmdList = nullptr;
    }
    if (g_State.fence) {
        g_State.fence->Release();
        g_State.fence = nullptr;
    }
    // Release dedicated overlay queue
    if (g_State.overlayQueue) {
        g_State.overlayQueue->Release();
        g_State.overlayQueue = nullptr;
    }
    // Release cross-queue synchronization fence
    if (g_State.crossQueueFence) {
        g_State.crossQueueFence->Release();
        g_State.crossQueueFence = nullptr;
    }
    cleanupFFXPresentCallbackBackend();
    g_State.currentFenceValue = 0;
    g_State.crossQueueFenceValue = 0;
    g_State.allocIndex = 0;
    g_State.syncInit = false;
    g_State.syncDevice = nullptr;
    g_State.cachedSC3 = nullptr;
    // Discard any pending deferred Signal — fence is being released
    g_deferredSignalValue.store(0, std::memory_order_release);
    g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    g_deferredSignalQueue.store(nullptr, std::memory_order_release);
    ResetStartupOverlayBackendActivationStage();

    // Clean up piggyback state
    g_PiggybackOverlayActive.store(false, std::memory_order_relaxed);
}

void CleanupRTVs() {
    // FG-SAFE: backBuffers no longer holds references (released at create time)
    g_State.backBuffers.clear();
    if (g_DummyBackBuffer) {
        g_DummyBackBuffer->Release();
        g_DummyBackBuffer = nullptr;
    }
    // Clean up D3D11On12 overlay bridge (holds wrapped backbuffer references)
    CleanupD3D11On12();
    // Release offscreen compositing resources
    if (g_State.offscreenRT) {
        g_State.offscreenRT->Release();
        g_State.offscreenRT = nullptr;
    }
    if (g_State.offscreenRtvHeap) {
        g_State.offscreenRtvHeap->Release();
        g_State.offscreenRtvHeap = nullptr;
    }
    g_State.offscreenWidth = 0;
    g_State.offscreenHeight = 0;
    g_State.offscreenFormat = DXGI_FORMAT_UNKNOWN;
    if (g_State.rtvDescHeap) {
        g_State.rtvDescHeap->Release();
        g_State.rtvDescHeap = nullptr;
    }
    if (g_State.srvDescHeap) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
    }
    g_State.bufferCount = 0;
    g_State.cachedSwapChain = nullptr;
    g_State.cachedSC3 = nullptr;
}

void DX12_OnSwapchainResizeBegin() {
    bool wasAlreadySet = g_InSwapchainResizeCleanup.exchange(true);
    HookLog("DX12: DX12_OnSwapchainResizeBegin called, wasAlreadySet=%d", wasAlreadySet);

    // Disable post-SL overlay rendering IMMEDIATELY to prevent rendering
    // to invalidated backbuffers during the resize.
    g_PostSLOverlayActive.store(false, std::memory_order_release);
    g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
    g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: swapchain resize");
    ResetPostSLLifecycleForTransition("DX12: swapchain resize", true);
    SetPostSLLastWorkingQueue(nullptr);  // Swapchain resize — rendering setup changed
    // Prevent recursion - if already in resize, return immediately
    if (wasAlreadySet) {
        HookLog(
            "DX12: DX12_OnSwapchainResizeBegin - already in resize, returning "
            "early");
        return;
