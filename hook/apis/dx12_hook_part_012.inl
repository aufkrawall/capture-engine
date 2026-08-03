    if (actualFGActive) {
        s_startupOverlayObservedAnyFG.store(true, std::memory_order_release);
        s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
        ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
        g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(false, std::memory_order_release);
        return;
    }

    const bool startupBlockingOverlayLoaded = ce::overlay_compat::GetStartupBlockingOverlayModuleName() != nullptr;
    if (!startupBlockingOverlayLoaded || !g_FGRuntimeOwnsSwapchain) {
        s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
    }

    const bool observedAnyFrameGenerationActivity = s_startupOverlayObservedAnyFG.load(std::memory_order_acquire);
    const bool startupCompatSettled = s_startupOverlayCompatSettled.load(std::memory_order_acquire);
    const bool lateRuntimeOwnedHandoffJustObserved =
        s_pendingLateRuntimeOwnedStartupHandoff.exchange(false, std::memory_order_acq_rel);
    const bool preserveLiveOverlayDuringHandoff =
        ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
    if (!ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
            startupBlockingOverlayLoaded, actualFGActive, startupCompatSettled, g_FGRuntimeOwnsSwapchain,
            observedAnyFrameGenerationActivity, lateRuntimeOwnedHandoffJustObserved,
            preserveLiveOverlayDuringHandoff)) {
        if (lateRuntimeOwnedHandoffJustObserved && preserveLiveOverlayDuringHandoff) {
            HookLogImportant(
                "DX12: Keeping settled startup overlay live through runtime-inactive Streamline handoff "
                "(overlayInit=%d syncInit=%d runtime=%s origGame=%p)",
                g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0,
                ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), g_OriginalGameQueue);
        }
        return;
    }

    if (s_startupOverlayCompatSettled.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant(
            "DX12: Re-arming startup overlay compatibility after late runtime-owned swapchain handoff before any real "
            "FG activity");
        ResetStartupOverlayBackendActivationStage();
    }
}

static const char* GetStartupOverlayFirstDrawProbeStageName(StartupOverlayFirstDrawProbeStage stage) {
    switch (stage) {
        case StartupOverlayFirstDrawProbeStage::kBackbufferTouchOnly:
            return "backbuffer touch";
        case StartupOverlayFirstDrawProbeStage::kPipelineStateOnly:
            return "pipeline state setup";
        case StartupOverlayFirstDrawProbeStage::kActualRender:
            return "real overlay draw";
        case StartupOverlayFirstDrawProbeStage::kComplete:
            return "complete";
        case StartupOverlayFirstDrawProbeStage::kNone:
        default:
            return "overlay probe";
    }
}

static void ResetStartupOverlayBackendActivationStage() {
    s_startupOverlayActivationStage = StartupOverlayActivationStage::kNone;
    s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kNone;
    s_startupOverlayActivationStageMs = 0;
    s_startupOverlaySyncInitMs = 0;
    s_startupOverlayResourcePrimeMs = 0;
    s_startupOverlayFirstDrawProbeMs = 0;
    s_lastStartupBlockingRenderModuleActivityMs.store(0, std::memory_order_release);
}

static bool IsActualFrameGenerationActive() {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    return runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG || runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
}

static bool IsFSRFrameGenerationActive() {
    return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kFSRFG;
}

static bool IsNvidiaSmoothMotionActiveRuntime() {
    return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion;
}

static ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue);
static bool IsStreamlineLoaded();

static bool ShouldUseDedicatedOverlayQueue(const char** disabledByOverlayModule = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    const bool processNeedsDelay = IsStartupOverlayCompatibilityActive();
    const bool actualFGActive = IsActualFrameGenerationActive();
    const bool fsrFGActive = IsFSRFrameGenerationActive();
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool runtimeOwnsSwapchain = g_FGRuntimeOwnsSwapchain;
    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();

    // When Streamline FG is active, do NOT use a dedicated overlay queue.
    // D3D12 rejects cross-queue access to swapchain backbuffers with
    // DXGI_ERROR_ACCESS_DENIED during SL FG (SL takes exclusive control
    // of the swapchain queue association).  Render on the game queue
    // instead, skipping fence operations to avoid interfering with SL's
    // internal frame synchronization.
    if (streamlineFGRunning) {
        if (disabledByOverlayModule)
            *disabledByOverlayModule = nullptr;
        return false;
    }

    if (ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
            actualFGActive, fsrFGActive, streamlineFGRunning, runtimeOwnsSwapchain, runtimeOwnedNativeFGPresentPath)) {
        static std::atomic<int> s_runtimeOwnedDedicatedQueueDisableLogCount{0};
        int logCount = s_runtimeOwnedDedicatedQueueDisableLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Dedicated overlay queue disabled for native/runtime-owned FG "
                "(fsrFG=%d runtimeOwns=%d nativePresentPath=%d scQueue=%p origGame=%p)",
                fsrFGActive ? 1 : 0, runtimeOwnsSwapchain ? 1 : 0, runtimeOwnedNativeFGPresentPath ? 1 : 0,
                g_SwapchainQueue, g_OriginalGameQueue);
        }
        if (disabledByOverlayModule)
            *disabledByOverlayModule = nullptr;
        return false;
    }

    const bool shouldUseDedicatedQueue =
        ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(actualFGActive, processNeedsDelay, overlayModule);
    if (disabledByOverlayModule) {
        *disabledByOverlayModule = shouldUseDedicatedQueue ? nullptr : overlayModule;
    }

    return shouldUseDedicatedQueue;
}

static bool WaitForGameQueueBeforeDedicatedOverlaySubmission(ID3D12CommandQueue* gameQueue, const char* phase) {
    if (!g_State.overlayQueue || !g_State.crossQueueFence || !g_State.crossQueueFenceEvent) {
        return true;
    }
    if (!gameQueue) {
        HookLogImportant("DX12: Cannot synchronize dedicated overlay queue before %s because the game queue is null",
                         phase ? phase : "overlay submission");
        return false;
    }

    const UINT64 waitValue = g_State.crossQueueFenceValue + 1;
    HRESULT signalHr = gameQueue->Signal(g_State.crossQueueFence, waitValue);
    if (FAILED(signalHr)) {
        HookLogImportant("DX12: Failed to signal game queue before %s on dedicated overlay queue hr=0x%08X",
                         phase ? phase : "overlay submission", signalHr);
        return false;
    }

    g_State.crossQueueFenceValue = waitValue;
    if (g_State.crossQueueFence->GetCompletedValue() >= waitValue) {
        return true;
    }

    HRESULT setHr = g_State.crossQueueFence->SetEventOnCompletion(waitValue, g_State.crossQueueFenceEvent);
    if (FAILED(setHr)) {
        HookLogImportant("DX12: Failed to arm cross-queue wait before %s hr=0x%08X",
                         phase ? phase : "overlay submission", setHr);
        return false;
    }

    DWORD waitHr = WaitForSingleObject(g_State.crossQueueFenceEvent, kOverlayCrossQueueWaitMs);
    if (waitHr == WAIT_OBJECT_0) {
        static std::atomic<int> s_crossQueueWaitSuccessLogCount{0};
        if (s_crossQueueWaitSuccessLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant("DX12: Dedicated overlay queue synchronized with game queue for %s (value=%llu)",
                             phase ? phase : "overlay submission", static_cast<unsigned long long>(waitValue));
        }
        return true;
    }

    if (waitHr == WAIT_TIMEOUT) {
        HookLogImportant("DX12: Timed out waiting for game queue before %s on dedicated overlay queue (value=%llu)",
                         phase ? phase : "overlay submission", static_cast<unsigned long long>(waitValue));
    } else {
        HookLogImportant("DX12: WaitForSingleObject failed before %s on dedicated overlay queue result=%lu",
                         phase ? phase : "overlay submission", waitHr);
    }
    return false;
}

// Probe the real D3D12 ECL by creating a temporary COMPUTE queue.
// SL only vtable-hooks DIRECT queues for FG; COMPUTE queues keep the
// pristine d3d12.dll function pointer.  When DIRECT and COMPUTE queues
// share the same vtable (all hooks applied to the shared vtable), we
// fall back to scanning SL's hook for an indirect JMP/CALL target.
static void ProbeRealD3D12ECL(ID3D12Device* device) {
    if (g_RealD3D12ECL.load(std::memory_order_acquire))
        return;
    if (!device)
        return;

    // Create a temporary COMPUTE queue
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ID3D12CommandQueue* probeQueue = nullptr;
    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probeQueue));
    if (FAILED(hr) || !probeQueue) {
        HookLogImportant("DX12: ECL probe - COMPUTE queue creation failed (hr=0x%08X)", (unsigned)hr);
        return;
    }

    void** probeVtable = *(void***)probeQueue;
    void* probeECL = probeVtable[10];
    void* probeSignal = probeVtable[14];  // Signal is at vtable[14] on ID3D12CommandQueue

    // Check which module owns the COMPUTE queue's ECL
    HMODULE probeModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)probeECL, &probeModule);
    char probeMod[MAX_PATH] = {};
    if (probeModule)
        GetModuleFileNameA(probeModule, probeMod, MAX_PATH);

    // Check which module owns the COMPUTE queue's Signal
    HMODULE probeSignalModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)probeSignal, &probeSignalModule);
    char probeSignalMod[MAX_PATH] = {};
    if (probeSignalModule)
        GetModuleFileNameA(probeSignalModule, probeSignalMod, MAX_PATH);

    // Compare with the current DIRECT queue's vtable[10] (our hooked version)
    ID3D12CommandQueue* directQueue = g_SwapchainQueue;
    void* directECL = nullptr;
    char directMod[MAX_PATH] = {};
    if (directQueue) {
        void** directVtable = *(void***)directQueue;
        directECL = directVtable[10];
        HMODULE dMod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)directECL, &dMod);
        if (dMod)
            GetModuleFileNameA(dMod, directMod, MAX_PATH);
    }

    bool sameVtable = (probeVtable == (directQueue ? *(void***)directQueue : nullptr));
    bool sameECL = (probeECL == directECL);
    bool probeIsD3D12 = (strstr(probeMod, "d3d12") != nullptr || strstr(probeMod, "D3D12") != nullptr);
    bool probeSignalIsD3D12 =
        (strstr(probeSignalMod, "d3d12") != nullptr || strstr(probeSignalMod, "D3D12") != nullptr);

    HookLogImportant("DX12: ECL probe - COMPUTE ECL=%p (%s), DIRECT ECL=%p (%s), sameVtable=%d sameECL=%d isD3D12=%d",
                     probeECL, probeMod, directECL, directMod, sameVtable ? 1 : 0, sameECL ? 1 : 0,
                     probeIsD3D12 ? 1 : 0);

    if (probeIsD3D12) {
        g_RealD3D12ECL.store((ExecuteCommandListsPtr)probeECL, std::memory_order_release);
        HookLogImportant("DX12: Real D3D12 ECL found via COMPUTE probe: %p", probeECL);
    }

    // Probe the real D3D12 Signal from the COMPUTE queue's vtable
    if (probeSignalIsD3D12 && !g_RealD3D12Signal.load(std::memory_order_acquire)) {
        g_RealD3D12Signal.store(reinterpret_cast<SignalPtr>(probeSignal), std::memory_order_release);
        HookLogImportant("DX12: Real D3D12 Signal found via COMPUTE probe: %p (%s)", probeSignal, probeSignalMod);
    }

    // Always check saved original — in GTA V both COMPUTE and DIRECT share
    // the same vtable (sameECL=1) so our hook is on both, but
    // oExecuteCommandLists still holds the real D3D12 function.
    if (!g_RealD3D12ECL.load(std::memory_order_acquire)) {
        ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
        if (savedOrig) {
            HMODULE origMod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)savedOrig, &origMod);
            char origModName[MAX_PATH] = {};
            if (origMod)
                GetModuleFileNameA(origMod, origModName, MAX_PATH);
            bool origIsD3D12 = (strstr(origModName, "d3d12") != nullptr || strstr(origModName, "D3D12") != nullptr);
            HookLogImportant("DX12: ECL probe - saved oECL=%p (%s) isD3D12=%d", (void*)savedOrig, origModName,
                             origIsD3D12 ? 1 : 0);
            if (origIsD3D12) {
                g_RealD3D12ECL.store(savedOrig, std::memory_order_release);
                HookLogImportant("DX12: Real D3D12 ECL found via saved original: %p", (void*)savedOrig);
            }
        }
    }

    // If still not found, try to follow the saved original's JMP chain
    if (!g_RealD3D12ECL.load(std::memory_order_acquire)) {
        ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
        if (savedOrig) {
            const uint8_t* fn = (const uint8_t*)savedOrig;
            void* target = nullptr;
            // Check for E9 rel32 (JMP rel32) — SL's hook might be a simple JMP
            if (fn[0] == 0xE9) {
                int32_t rel = *(const int32_t*)(fn + 1);
                target = (void*)(fn + 5 + rel);
            }
            // Check for FF 25 (JMP [rip+disp32]) — indirect JMP
            else if (fn[0] == 0xFF && fn[1] == 0x25) {
                int32_t disp = *(const int32_t*)(fn + 2);
                void** addr = (void**)(fn + 6 + disp);
                target = *addr;
            }
            // Check for 48 FF 25 (REX.W JMP [rip+disp32])
            else if (fn[0] == 0x48 && fn[1] == 0xFF && fn[2] == 0x25) {
                int32_t disp = *(const int32_t*)(fn + 3);
                void** addr = (void**)(fn + 7 + disp);
                target = *addr;
            }

            if (target) {
                HMODULE targetMod = nullptr;
                GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)target, &targetMod);
                char targetModName[MAX_PATH] = {};
                if (targetMod)
                    GetModuleFileNameA(targetMod, targetModName, MAX_PATH);
                bool isD3D12 = (strstr(targetModName, "d3d12") != nullptr || strstr(targetModName, "D3D12") != nullptr);
                HookLogImportant("DX12: ECL probe - followed JMP chain: target=%p (%s) isD3D12=%d", target,
                                 targetModName, isD3D12 ? 1 : 0);
                if (isD3D12) {
                    g_RealD3D12ECL.store((ExecuteCommandListsPtr)target, std::memory_order_release);
                    HookLogImportant("DX12: Real D3D12 ECL found via JMP chain: %p", target);
                }
            }
        }
    }

    if (!g_RealD3D12ECL.load(std::memory_order_acquire)) {
        HookLogImportant(
            "DX12: ECL probe - FAILED to find real D3D12 ECL! "
            "Overlay will be disabled during SL FG to prevent crash");
    }

    probeQueue->Release();
}

static bool SubmitOverlayCommandList(ID3D12CommandQueue* gameQueue, ID3D12CommandList* list, int allocatorIndex,
                                     const char* phase, bool requireGameQueueDrain) {
    // Use the dedicated queue only when FG is actually active.  The queue stays
    // alive across FG mode switches to avoid destructive reinit, but submissions
    // go to the game queue when FG is inactive.
    bool useDedicated = g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
    ID3D12CommandQueue* submitQueue = useDedicated ? g_State.overlayQueue : gameQueue;
    if (!submitQueue || !list) {
        HookLogImportant("DX12: Cannot submit %s (submitQueue=%p, list=%p)", phase ? phase : "overlay command list",
                         submitQueue, list);
        return false;
    }

    if (requireGameQueueDrain && submitQueue != gameQueue &&
        !WaitForGameQueueBeforeDedicatedOverlaySubmission(gameQueue, phase)) {
        return false;
    }

    static std::atomic<int> s_submitLogCount{0};
    if (s_submitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
        HookLogImportant("DX12: Submitting %s on %s queue (submitQueue=%p, gameQueue=%p, allocator=%d)",
                         phase ? phase : "overlay command list",
                         submitQueue == gameQueue ? "game" : "dedicated overlay", submitQueue, gameQueue,
                         allocatorIndex);
    }

    ID3D12CommandList* lists[] = {list};

    // When using the dedicated overlay queue during SL FG, use the REAL
    // D3D12 ECL (bypassing SL's vtable hook) to prevent SL's internal
    // state tracking from seeing our overlay command lists.
    // ALSO prefer realECL in non-FG mode to avoid going through stale
    // SL/hook vtable entries after FG teardown (same logic as main path).
    ExecuteCommandListsPtr realECL = g_RealD3D12ECL.load(std::memory_order_acquire);
    bool slActive = IsStreamlineLoaded() && IsActualFrameGenerationActive();
    {
        ScopedCEOverlayECLSubmission ceOverlayECLGuard(phase ? phase : "overlay command list");
        if (realECL && (!useDedicated || slActive)) {
            realECL(submitQueue, 1, lists);
        } else {
            ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(submitQueue);
            if (origECL) {
                origECL(submitQueue, 1, lists);
            } else {
                submitQueue->ExecuteCommandLists(1, lists);
            }
        }
    }

    if (g_State.fence) {
        UINT64 next = g_State.currentFenceValue + 1;
        HRESULT signalHr = submitQueue->Signal(g_State.fence, next);
        if (SUCCEEDED(signalHr)) {
            g_State.currentFenceValue = next;
            if (allocatorIndex >= 0 && allocatorIndex < static_cast<int>(g_State.fenceValues.size())) {
                g_State.fenceValues[allocatorIndex] = next;
            }
        } else {
            HookLog("DX12: Overlay fence signal failed for %s hr=0x%08X", phase ? phase : "overlay command list",
                    signalHr);
        }
    }

    return true;
}

static void NoteStartupBlockingRenderModuleActivityFromECL(ID3D12CommandQueue* queue, const void* callerAddress) {
    // Fast early-out: once overlay probe is complete, no need to track anymore
    if (s_startupOverlayFirstDrawProbeStage == StartupOverlayFirstDrawProbeStage::kComplete) {
        return;
    }

    if (!queue || !callerAddress ||
        !ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) ||
        IsActualFrameGenerationActive() ||
        g_FGCompat.IsFGActive()) {  // Also skip for heuristic FG (FSR FG) — avoids GetModuleHandleExA overhead
        return;
    }

    const char* blockingRenderModule = ce::overlay_compat::GetStartupBlockingOverlayRenderModuleName();
    if (!blockingRenderModule) {
        return;
    }

    // Cache the blocking module handle to avoid GetModuleHandleA kernel call
    // on every ECL invocation. The module won't unload during gameplay.
    static HMODULE s_cachedBlockingModule = nullptr;
    static bool s_cachedBlockingModuleLookedUp = false;
    if (!s_cachedBlockingModuleLookedUp) {
        s_cachedBlockingModule = GetModuleHandleA(blockingRenderModule);
        s_cachedBlockingModuleLookedUp = true;
    }
    HMODULE blockingModuleHandle = s_cachedBlockingModule;
    if (!blockingModuleHandle) {
        // Module not loaded yet — retry next time
        s_cachedBlockingModuleLookedUp = false;
        return;
    }

    HMODULE callerModuleHandle = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(callerAddress), &callerModuleHandle) ||
        !callerModuleHandle || callerModuleHandle != blockingModuleHandle) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    s_lastStartupBlockingRenderModuleActivityMs.store(now, std::memory_order_release);

    static std::atomic<int> s_blockingModuleActivityLogCount{0};
    if (s_blockingModuleActivityLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
        char modulePath[MAX_PATH] = {};
        const char* moduleForLog = blockingRenderModule;
        if (GetModuleFileNameA(callerModuleHandle, modulePath, MAX_PATH) > 0) {
            moduleForLog = modulePath;
        }
        HookLogImportant(
            "DX12: Startup-blocking render module activity detected via ExecuteCommandLists (module=%s, queue=%p, "
            "caller=%p)",
            moduleForLog, queue, callerAddress);
    }
}

static bool ShouldSuppressOverlayForStartupCompat(
    HWND gameWindow, const char** overlayModule = nullptr, ULONGLONG* remainingMs = nullptr,
    ce::overlay_compat::AuxiliaryProcessWindowInfo* activeWindow = nullptr) {
    const bool startupCompatActive = IsStartupOverlayCompatibilityActive();
    const char* blockingOverlayModule =
        startupCompatActive ? ce::overlay_compat::GetStartupBlockingOverlayModuleName() : nullptr;
    const bool actualFGActive = IsActualFrameGenerationActive();
    static ULONGLONG s_firstOverlayDetectedMs = 0;
    static ULONGLONG s_lastPollMs = 0;
    static ULONGLONG s_lastVisibleMs = 0;
    static bool s_auxiliaryWindowVisible = false;
    static ce::overlay_compat::AuxiliaryProcessWindowInfo s_auxiliaryWindow = {};
    if (overlayModule) {
        *overlayModule = blockingOverlayModule;
    }
    if (remainingMs) {
        *remainingMs = 0;
    }
    if (activeWindow) {
        *activeWindow = {};
    }

    if (!startupCompatActive || !blockingOverlayModule || actualFGActive || !IsWindow(gameWindow)) {
        s_firstOverlayDetectedMs = 0;
        s_lastPollMs = 0;
        s_lastVisibleMs = 0;
        s_auxiliaryWindowVisible = false;
        s_auxiliaryWindow = {};
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (s_firstOverlayDetectedMs == 0) {
        s_firstOverlayDetectedMs = now;
    }

    if (s_lastPollMs == 0 || now - s_lastPollMs >= kStartupOverlayWindowPollMs) {
        s_lastPollMs = now;

        ce::overlay_compat::AuxiliaryProcessWindowInfo visibleWindow = {};
        s_auxiliaryWindowVisible =
            ce::overlay_compat::FindAuxiliaryProcessWindow(GetCurrentProcessId(), gameWindow, &visibleWindow);
        if (s_auxiliaryWindowVisible) {
            s_lastVisibleMs = now;
            s_auxiliaryWindow = visibleWindow;
        } else {
            s_auxiliaryWindow = {};
        }
    }

    if (activeWindow) {
        *activeWindow = s_auxiliaryWindow;
    }

    const ULONGLONG msSinceOverlayDetected = now - s_firstOverlayDetectedMs;
    const ULONGLONG msSinceLastVisible = s_lastVisibleMs == 0 ? kStartupOverlayQuietPeriodMs : (now - s_lastVisibleMs);
    const bool suppress = ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(
        true, actualFGActive, s_auxiliaryWindowVisible, msSinceOverlayDetected, kStartupOverlayWarmupMs,
        msSinceLastVisible, kStartupOverlayQuietPeriodMs);
    if (remainingMs && suppress) {
        ULONGLONG warmupRemaining =
            msSinceOverlayDetected < kStartupOverlayWarmupMs ? (kStartupOverlayWarmupMs - msSinceOverlayDetected) : 0;
        ULONGLONG quietRemaining = !s_auxiliaryWindowVisible && msSinceLastVisible < kStartupOverlayQuietPeriodMs
                                       ? (kStartupOverlayQuietPeriodMs - msSinceLastVisible)
                                       : 0;
        *remainingMs = std::max(warmupRemaining, quietRemaining);
    }
    return suppress;
}

static bool ShouldDeferOverlayInitForStartupCompat(HWND gameWindow, ULONGLONG* remainingMs = nullptr) {
    static ULONGLONG s_firstDeferredInitEligibleMs = 0;
    if (remainingMs) {
        *remainingMs = 0;
    }

    if (!IsStartupOverlayCompatibilityActive() || !IsWindow(gameWindow)) {
        s_firstDeferredInitEligibleMs = 0;
        return false;
    }

    if (g_State.overlayInit || ce::overlay_compat::GetStartupBlockingOverlayModuleName()) {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (s_firstDeferredInitEligibleMs == 0) {
        s_firstDeferredInitEligibleMs = now;
    }

    const ULONGLONG elapsedMs = now - s_firstDeferredInitEligibleMs;
    if (elapsedMs >= kStartupOverlayInitGraceMs) {
        return false;
    }

    if (remainingMs) {
        *remainingMs = kStartupOverlayInitGraceMs - elapsedMs;
    }
    return true;
}

static bool ShouldDelayOverlayInitAfterStartupResumeCompat(bool allowOverlayRender, HWND gameWindow,
                                                           bool runtimeOwnedSwapchainActive,
                                                           ULONGLONG* remainingMs = nullptr) {
    static bool s_hadStartupSuppression = false;
    static ULONGLONG s_resumeStableSinceMs = 0;
    static HWND s_resumeWindow = nullptr;
    static HWND s_loggedSameProcessResumeWindow = nullptr;
    static HWND s_loggedUnusableResumeGameWindow = nullptr;
    static HWND s_loggedUnusableResumeForegroundWindow = nullptr;
    if (remainingMs) {
        *remainingMs = 0;
    }

    const bool processNeedsDelay = IsStartupOverlayCompatibilityActive();
    const bool actualFGActive = IsActualFrameGenerationActive();
    if (!processNeedsDelay || actualFGActive) {
        s_hadStartupSuppression = false;
        s_resumeStableSinceMs = 0;
        s_resumeWindow = nullptr;
        s_loggedSameProcessResumeWindow = nullptr;
        s_loggedUnusableResumeGameWindow = nullptr;
        s_loggedUnusableResumeForegroundWindow = nullptr;
        return false;
    }

    if (!allowOverlayRender) {
        s_hadStartupSuppression = true;
        s_resumeStableSinceMs = 0;
        s_resumeWindow = nullptr;
        s_loggedSameProcessResumeWindow = nullptr;
        s_loggedUnusableResumeGameWindow = nullptr;
        s_loggedUnusableResumeForegroundWindow = nullptr;
        return false;
    }

    if (!s_hadStartupSuppression || !IsWindow(gameWindow)) {
        return false;
    }

    if (runtimeOwnedSwapchainActive) {
        // Require a fresh stable post-startup window after runtime queue
        // ownership returns to the game. GTA 5 Enhanced can briefly leave the
        // Social Club startup window while the live swapchain is still bound to
        // a runtime-owned queue, and drawing in that handoff window trips
        // ERR_GFX_STATE.
        s_resumeStableSinceMs = 0;
        s_resumeWindow = gameWindow;
        return true;
    }

    RECT clientRect = {};
    LONG width = 0;
    LONG height = 0;
    if (GetClientRect(gameWindow, &clientRect)) {
        width = clientRect.right - clientRect.left;
        height = clientRect.bottom - clientRect.top;
    }

    const DWORD expectedProcessId = GetCurrentProcessId();
    const HWND foregroundWindow = GetForegroundWindow();
    LONG foregroundWidth = 0;
    LONG foregroundHeight = 0;
    const bool exactWindowForeground = (foregroundWindow == gameWindow);
    const ULONGLONG now = GetTickCount64();
    const bool usableSameProcessForegroundWindow =
        !exactWindowForeground && ce::overlay_compat::IsUsableSameProcessForegroundWindow(
                                      foregroundWindow, expectedProcessId, &foregroundWidth, &foregroundHeight);
    bool usingSameProcessForegroundWindow = false;
    const bool trackableForegroundWindow = ce::overlay_compat::ResolveDX12OverlayStartupResumeForegroundWindowMetrics(
        exactWindowForeground, usableSameProcessForegroundWindow, width, height, foregroundWidth, foregroundHeight,
        &width, &height, &usingSameProcessForegroundWindow);
    if (!trackableForegroundWindow) {
        if (s_loggedUnusableResumeGameWindow != gameWindow ||
            s_loggedUnusableResumeForegroundWindow != foregroundWindow) {
            HookLogImportant(
                "DX12: Startup-overlay resume still waiting for a usable foreground window "
                "(swapchainWindow=%p foregroundWindow=%p exact=%d sameProcessUsable=%d gameSize=%ldx%ld "
                "foregroundSize=%ldx%ld)",
                gameWindow, foregroundWindow, exactWindowForeground ? 1 : 0, usableSameProcessForegroundWindow ? 1 : 0,
                width, height, foregroundWidth, foregroundHeight);
            s_loggedUnusableResumeGameWindow = gameWindow;
            s_loggedUnusableResumeForegroundWindow = foregroundWindow;
        }
        s_resumeStableSinceMs = 0;
        s_resumeWindow = gameWindow;
        return true;
    }

    s_loggedUnusableResumeGameWindow = nullptr;
    s_loggedUnusableResumeForegroundWindow = nullptr;
