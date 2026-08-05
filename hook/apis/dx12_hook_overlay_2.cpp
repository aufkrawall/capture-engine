#include "dx12_hook_internal.h"
#include "dx12_hook_overlay_shared.h"


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

