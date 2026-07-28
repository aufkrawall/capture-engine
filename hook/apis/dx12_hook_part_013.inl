
    // GTA can switch from the swapchain HWND to another same-process foreground
    // window while the Social Club startup path unwinds. Track either stable
    // candidate so the post-resume delay can count down instead of latching at
    // remaining=0ms forever when the exact swapchain window no longer owns the
    // foreground.
    const bool windowForeground = true;
    const HWND stableWindow = usingSameProcessForegroundWindow ? foregroundWindow : gameWindow;
    if (usingSameProcessForegroundWindow && s_loggedSameProcessResumeWindow != stableWindow) {
        HookLogImportant(
            "DX12: Startup-overlay resume tracking usable same-process foreground window %p instead of "
            "swapchain window %p (foregroundSize=%ldx%ld)",
            stableWindow, gameWindow, width, height);
        s_loggedSameProcessResumeWindow = stableWindow;
    } else if (!usingSameProcessForegroundWindow) {
        s_loggedSameProcessResumeWindow = nullptr;
        if (s_loggedUnusableResumeGameWindow != gameWindow) {
            HookLogImportant(
                "DX12: Startup-overlay resume falling back to swapchain window %p (size=%ldx%ld) because "
                "foreground window %p is not a usable same-process window",
                gameWindow, width, height, foregroundWindow);
            s_loggedUnusableResumeGameWindow = gameWindow;
        }
    }

    if (s_resumeWindow != stableWindow || s_resumeStableSinceMs == 0) {
        s_resumeWindow = stableWindow;
        s_resumeStableSinceMs = now;
    }

    const ULONGLONG msSinceResumeReady = now - s_resumeStableSinceMs;
    if (ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(
            processNeedsDelay, s_hadStartupSuppression, actualFGActive, runtimeOwnedSwapchainActive, windowForeground,
            width, height, msSinceResumeReady, kStartupOverlayPostResumeSettleMs)) {
        if (remainingMs) {
            *remainingMs =
                kStartupOverlayPostResumeSettleMs - std::min(msSinceResumeReady, kStartupOverlayPostResumeSettleMs);
        }
        return true;
    }

    s_hadStartupSuppression = false;
    s_resumeStableSinceMs = 0;
    s_resumeWindow = nullptr;
    return false;
}

static bool ApplyOverlayStartupCompatMode(HWND gameWindow) {
    const char* overlayModule = nullptr;
    ULONGLONG remainingMs = 0;
    ce::overlay_compat::AuxiliaryProcessWindowInfo activeWindow = {};
    const bool suppressOverlay =
        ShouldSuppressOverlayForStartupCompat(gameWindow, &overlayModule, &remainingMs, &activeWindow);
    const bool allowOverlay = !suppressOverlay;
    static bool s_overlayCompatSuppressed = false;
    static bool s_loggedVisibleWindowSuppression = false;
    static bool s_loggedKeepVisibleDuringSuppression = false;
    static HWND s_loggedWindowHandle = nullptr;

    if (!allowOverlay) {
        if (ce::overlay_compat::ShouldKeepDX12OverlayVisibleDuringStartupSuppression(g_State.overlayInit &&
                                                                                     g_State.syncInit)) {
            if (!s_loggedKeepVisibleDuringSuppression) {
                HookLogImportant(
                    "DX12: Continuing DX12 overlay submissions while startup-overlay compatibility window is active "
                    "(overlay=%s, backend already initialized)",
                    overlayModule ? overlayModule : "module");
                s_loggedKeepVisibleDuringSuppression = true;
            }
            return true;
        }
        if (activeWindow.hwnd) {
            if (!s_overlayCompatSuppressed || !s_loggedVisibleWindowSuppression ||
                s_loggedWindowHandle != activeWindow.hwnd) {
                HookLogImportant(
                    "DX12: Pausing DX12 overlay submissions while startup window from %s is visible "
                    "(hwnd=%p visible=%d class='%s' title='%s')",
                    overlayModule ? overlayModule : "module", activeWindow.hwnd, activeWindow.visible ? 1 : 0,
                    activeWindow.className[0] ? activeWindow.className : "<unknown>",
                    activeWindow.title[0] ? activeWindow.title : "<untitled>");
                s_loggedVisibleWindowSuppression = true;
                s_loggedWindowHandle = activeWindow.hwnd;
            }
        } else if (!s_overlayCompatSuppressed || s_loggedVisibleWindowSuppression) {
            HookLogImportant(
                "DX12: Keeping DX12 overlay submissions paused for startup-overlay warm-up/cool-down "
                "(overlay=%s remaining=%llums)",
                overlayModule ? overlayModule : "module", remainingMs);
            s_loggedVisibleWindowSuppression = false;
            s_loggedWindowHandle = nullptr;
        }
        if (!s_overlayCompatSuppressed) {
            s_overlayCompatSuppressed = true;
        }
        return false;
    }

    if (s_overlayCompatSuppressed) {
        HookLogImportant("DX12: Resuming DX12 overlay after startup overlay windows settled");
        s_overlayCompatSuppressed = false;
        s_loggedVisibleWindowSuppression = false;
        s_loggedKeepVisibleDuringSuppression = false;
        s_loggedWindowHandle = nullptr;
    } else if (s_loggedKeepVisibleDuringSuppression) {
        s_loggedKeepVisibleDuringSuppression = false;
    }

    return true;
}

static void DisableDedicatedOverlayQueueForOverlayCompat() {
    // When FG goes inactive, we keep the dedicated overlay queue alive to avoid
    // a destructive teardown/rebuild cycle during FG mode switches (e.g. 2x→3x).
    // Destroying and recreating queue + fence + allocators mid-transition causes
    // ERR_GFX_STATE because InitOverlaySync releases D3D12 objects while the GPU
    // still has in-flight work (deferred Signal not yet flushed).
    //
    // The queue sits idle when FG is inactive (submissions go to the game queue).
    // When FG reactivates, the queue is ready — no reinit needed.
    if (ShouldUseDedicatedOverlayQueue()) {
        return;
    }

    if (!g_State.overlayQueue) {
        return;
    }

    static bool s_loggedSuspend = false;
    if (!s_loggedSuspend) {
        const char* overlayModule = nullptr;
        ShouldUseDedicatedOverlayQueue(&overlayModule);
        if (overlayModule) {
            HookLogImportant(
                "DX12: Suspending dedicated overlay queue (FG inactive, external overlay %s) — queue kept alive",
                overlayModule);
        } else {
            HookLogImportant("DX12: Suspending dedicated overlay queue (FG inactive) — queue kept alive");
        }
        s_loggedSuspend = true;
    }
}

static void EnsureDedicatedOverlayQueueForFGCompat() {
    if (!ShouldUseDedicatedOverlayQueue()) {
        return;
    }

    if (!g_State.syncInit || g_State.overlayQueue) {
        // Queue already exists or not yet initialized — nothing to do.
        return;
    }

    // Non-SL FG cases (e.g., FSR FG with third-party overlay) may still need
    // a dedicated queue.  For SL FG, ShouldUseDedicatedOverlayQueue() returns
    // false so we never reach here; overlay draws are skipped instead.
    HookLogImportant(
        "DX12: FG active with overlay compat — dedicated overlay queue not yet created, forcing sync reinit");
    g_State.syncInit = false;
    g_State.syncDevice = nullptr;
    g_State.overlayInit = false;
}

// Forward Declarations
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists);
void DX12_HookQueueVTable(ID3D12CommandQueue* queue);
void DX12_HookDeviceVTable(ID3D12Device* device);

static ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue) {
    if (!queue)
        return oExecuteCommandLists;

    void** vtbl = *reinterpret_cast<void***>(queue);
    if (!vtbl)
        return oExecuteCommandLists;

    void** cachedVtable = g_LastExecuteCommandListsVTable.load(std::memory_order_acquire);
    if (cachedVtable == vtbl) {
        ExecuteCommandListsPtr cachedOriginal = g_LastExecuteCommandListsOriginal.load(std::memory_order_acquire);
        if (cachedOriginal)
            return cachedOriginal;
    }

    ExecuteCommandListsPtr original = oExecuteCommandLists;
    {
        std::lock_guard<std::recursive_mutex> lock(g_ExecuteCommandListsHookStateMutex);
        auto it = g_ExecuteCommandListsOriginalByVTable.find(vtbl);
        if (it != g_ExecuteCommandListsOriginalByVTable.end())
            original = it->second;
    }

    if (original) {
        g_LastExecuteCommandListsOriginal.store(original, std::memory_order_release);
        g_LastExecuteCommandListsVTable.store(vtbl, std::memory_order_release);
    }
    return original;
}

static bool HasTrackedExecuteCommandListsOriginal(ID3D12CommandQueue* queue) {
    if (!queue) {
        return false;
    }

    void** vtbl = *reinterpret_cast<void***>(queue);
    if (!vtbl) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_ExecuteCommandListsHookStateMutex);
    return g_ExecuteCommandListsOriginalByVTable.find(vtbl) != g_ExecuteCommandListsOriginalByVTable.end();
}

static bool HookHasSafePostFSRBootstrapPathImpl() {
    if (!g_HadFSRFGPhase) {
        return false;
    }

    const bool hasRealQueueBehindWrapper = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire) != nullptr;
    const bool hasRealD3D12ECL = g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr;
    const bool hasSLWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire) != nullptr;
    const bool wrapperBootstrapSafe = !ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(
        g_HadFSRFGPhase, hasRealQueueBehindWrapper, hasRealD3D12ECL, hasSLWrapperQueue);
    if (wrapperBootstrapSafe) {
        return true;
    }

    ID3D12CommandQueue* swapchainQueue = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    ID3D12CommandQueue* originalGameQueue = nullptr;
    bool hasTrackedSwapchainQueueSubmitPath = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        swapchainQueue = g_SwapchainQueue;
        commandQueue = g_CommandQueue.load(std::memory_order_acquire);
        originalGameQueue = g_OriginalGameQueue;
        hasTrackedSwapchainQueueSubmitPath = HasTrackedExecuteCommandListsOriginal(swapchainQueue);
    }
    const bool hasRuntimeOwnedSwapchainQueue = swapchainQueue != nullptr && swapchainQueue != originalGameQueue;
    const bool hasRealD3D12SubmitPath = g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr;
    const bool hasSwapchainQueueSubmitPath = hasTrackedSwapchainQueueSubmitPath || hasRealD3D12SubmitPath;
    const bool commandQueueMatchesSwapchainQueue =
        commandQueue != nullptr && swapchainQueue != nullptr && commandQueue == swapchainQueue;
    const bool streamlineHandoffOrActive = DXGIShared::IsStreamlineStartupHandoffPending() ||
                                           DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool runtimeOwnedSwapchainBootstrapSafe =
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(
            g_HadFSRFGPhase, hasRuntimeOwnedSwapchainQueue, streamlineHandoffOrActive, hasSwapchainQueueSubmitPath);
    if (runtimeOwnedSwapchainBootstrapSafe &&
        !g_SafePostFSRRuntimeOwnedSwapchainBootstrapLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "DX12: Safe post-FSR bootstrap path available via runtime-owned Streamline swapchain queue "
            "(scQueue=%p cmdQ=%p origGame=%p realECL=%p wrapper=%p realBehindWrapper=%p trackedSubmit=%d "
            "cmdMatches=%d streamlineHandoffOrActive=%d)",
            swapchainQueue, commandQueue, originalGameQueue, (void*)g_RealD3D12ECL.load(std::memory_order_acquire),
            g_SLWrapperQueue.load(std::memory_order_acquire),
            g_RealQueueBehindSLWrapper.load(std::memory_order_acquire), hasTrackedSwapchainQueueSubmitPath ? 1 : 0,
            commandQueueMatchesSwapchainQueue ? 1 : 0, streamlineHandoffOrActive ? 1 : 0);
    } else if (hasRuntimeOwnedSwapchainQueue && streamlineHandoffOrActive && !runtimeOwnedSwapchainBootstrapSafe) {
        static std::atomic<int> s_runtimeOwnedBootstrapUnsafeLogCount{0};
        const int logCount = s_runtimeOwnedBootstrapUnsafeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 120) == 0) {
            HookLogImportant(
                "DX12: Runtime-owned Streamline swapchain queue not yet safe for post-FSR bootstrap "
                "(scQueue=%p cmdQ=%p origGame=%p realECL=%p trackedSubmit=%d cmdMatches=%d "
                "streamlineHandoffOrActive=%d log=%d)",
                swapchainQueue, commandQueue, originalGameQueue, (void*)g_RealD3D12ECL.load(std::memory_order_acquire),
                hasTrackedSwapchainQueueSubmitPath ? 1 : 0, commandQueueMatchesSwapchainQueue ? 1 : 0,
                streamlineHandoffOrActive ? 1 : 0, logCount + 1);
        }
    }
    return runtimeOwnedSwapchainBootstrapSafe;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut,
                                                       IDXGISwapChain1** ppSC);

// REQUIRED EXPORTS
void DX12_AdjustWrapperResizeDepth(int delta) {
    if (delta > 0)
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_add(delta);
    else
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_sub(-delta);
}

// Forward declaration for CleanupRTVs
void CleanupRTVs();

void DX12_InvalidateSwapchain() {
    DXGIShared::g_SharedState.swapchainInvalid.store(true, std::memory_order_release);
    HookLog("DX12: Swapchain marked INVALID (FSR/FG transition detected)");
    ReleaseStreamlineStartupActivationSwapchain("DX12_InvalidateSwapchain");
    // The make-before-break keep-alive is bound to the live proxy swapchain;
    // a swapchain invalidation ends it.
    g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
    g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
    g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);
    g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
    g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kSwapchainInvalidation, "DX12_InvalidateSwapchain");
    // Log current state for debugging
    HookLog("DX12: Invalidating - overlayInit=%d, syncInit=%d, device=%p, queue=%p", g_State.overlayInit,
            g_State.syncInit, g_Device.load(), g_CommandQueue.load());

    // Only invalidate swapchain-level state, not device-level sync resources
    // This allows swapchain changes without full reinitialization
    if (g_State.overlayInit) {
        HookLog(
            "DX12: Invalidating swapchain resources (device-level resources "
            "preserved)");
        g_State.overlayInit = false;
        CleanupRTVs();
    }
}

void DX12_SignalFSR4SwapchainRecreated() {
    DXGIShared::g_SharedState.fsr4RecreationPending.store(true, std::memory_order_release);
    HookLog("DX12: FSR4 swapchain recreation signaled");
}

// Device-removed flag: once set, skip overlay rendering AND heartbeats so the
// freeze watchdog can detect the stuck state and create a diagnostic dump.
static std::atomic<bool> g_DeviceRemoved{false};

static bool ShouldReserveInactiveFGOverlaySpaceNow() {
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        currentSwapchainQueue = g_SwapchainQueue;
    }

    const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        g_HadFSRFGPhase, g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
        currentSwapchainQueue != nullptr);
    const bool recentStreamlineTeardown = g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
    const bool postSLRecentTeardownActivity =
        GetTickCount64() < g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
    return ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(
        postFSRNonFGRecovery, recentStreamlineTeardown, postSLRecentTeardownActivity);
}

static ID3D12CommandQueue* GetFrameClassificationQueue() {
    ID3D12CommandQueue* primaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* originalQueue = g_OriginalGameQueue;
    ID3D12CommandQueue* swapchainQueue = nullptr;
    bool actualFGActive = false;
    bool streamlineFGRunning = false;
    bool recoveringPostFSRNonFG = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        swapchainQueue = g_SwapchainQueue;
        actualFGActive = IsActualFrameGenerationActive();
        streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        recoveringPostFSRNonFG = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
            g_HadFSRFGPhase, g_NeedOffscreenOverlayAfterPostFSRNonFG, actualFGActive, streamlineFGRunning,
            swapchainQueue != nullptr);
    }

    if (ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
            recoveringPostFSRNonFG, actualFGActive, streamlineFGRunning, swapchainQueue != nullptr,
            originalQueue != nullptr, primaryQueue != nullptr, originalQueue == primaryQueue)) {
        static std::atomic<int> s_postFSRClassificationPrimaryLogCount{0};
        int logCount = s_postFSRClassificationPrimaryLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Frame classification using primary queue %p during post-FSR non-FG recovery "
                "(origGame=%p scQ=%p lastWorking=%p offscreen=%d)",
                primaryQueue, originalQueue, swapchainQueue, g_PostSLLastWorkingQueue,
                g_NeedOffscreenOverlayAfterPostFSRNonFG ? 1 : 0);
        }
        return primaryQueue;
    }

    if (originalQueue) {
        return originalQueue;
    }

    return primaryQueue;
}

static bool ShouldSuppressLikelyDuplicateTopLevelPresent(IDXGISwapChain3* sc3, UINT backBufferIdx) {
    if (!sc3 || !g_IPC || !g_IPC->IsCaptureRequested()) {
        return false;
    }

    SharedMemoryLayout* shm = g_IPC->GetSharedMem();
    if (!shm) {
        return false;
    }

    const int captureFps = shm->fpsLimiter.GetCaptureFps();
    if (captureFps <= 0) {
        return false;
    }

    const int64_t targetIntervalUs = 1000000LL / static_cast<int64_t>(captureFps);
    const int64_t suppressWindowUs = std::clamp((targetIntervalUs * 3) / 4, 1500LL, 7000LL);
    const int64_t nowUs = PerfLogger::GetQpcUs();
    IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(sc3);

    static std::atomic<IDXGISwapChain*> s_lastAcceptedSwapchain{nullptr};
    static std::atomic<uint32_t> s_lastAcceptedBackBufferIdx{UINT32_MAX};
    static std::atomic<int64_t> s_lastAcceptedPresentUs{0};
    static std::atomic<uint64_t> s_suppressedPresentCount{0};

    IDXGISwapChain* lastSwapchain = s_lastAcceptedSwapchain.load(std::memory_order_acquire);
    uint32_t lastBackBufferIdx = s_lastAcceptedBackBufferIdx.load(std::memory_order_acquire);
    int64_t lastAcceptedPresentUs = s_lastAcceptedPresentUs.load(std::memory_order_acquire);
    int64_t sinceLastUs = nowUs - lastAcceptedPresentUs;

    if (lastSwapchain == swapchain && lastBackBufferIdx == backBufferIdx && lastAcceptedPresentUs != 0 &&
        sinceLastUs > 0 && sinceLastUs < suppressWindowUs) {
        uint64_t suppressCount = s_suppressedPresentCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (suppressCount <= 10 || (suppressCount % 1000) == 0) {
            HookLogImportant(
                "DX12: Suppressing likely duplicate top-level Present #%llu "
                "(sc=%p bb=%u since=%lldus window=%lldus captureFps=%d)",
                static_cast<unsigned long long>(suppressCount), swapchain, backBufferIdx,
                static_cast<long long>(sinceLastUs), static_cast<long long>(suppressWindowUs), captureFps);
        }
        return true;
    }

    s_lastAcceptedSwapchain.store(swapchain, std::memory_order_release);
    s_lastAcceptedBackBufferIdx.store(backBufferIdx, std::memory_order_release);
    s_lastAcceptedPresentUs.store(nowUs, std::memory_order_release);
    return false;
}

static bool ShouldSkipCaptureForTargetCadence() {
    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    return ShouldSkipCaptureForTargetCadence(shm, "DX12");
}

// C Linkage Exports for cross-module calls (e.g. from C clients or
// GetProcAddress)
static __attribute__((noinline)) void DX12_SetCommandQueueInternal(ID3D12CommandQueue* pQueue,
                                                                   bool callerFromThirdPartyOverlay,
                                                                   const char* callerModulePath) {
    if (!pQueue)
        return;

    // Safety: during FG transitions, SL may call ECL on a queue that's
    // concurrently being freed.  Freed COM objects have null vtable pointers.
    // Use volatile to prevent compiler from caching the vtable across calls.
    auto vtblPtr = *reinterpret_cast<void* volatile const*>(pQueue);
    if (!vtblPtr)
        return;

    if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
        static std::atomic<int> s_protectedOfficialFFXSetQueueSkipLogCount{0};
        const int logCount = s_protectedOfficialFFXSetQueueSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: Protected official FFX startup pending - skipping SetCommandQueue side effects "
                "(queue=%p callerOverlay=%d caller=%s count=%d)",
                pQueue, callerFromThirdPartyOverlay ? 1 : 0,
                callerModulePath && callerModulePath[0] ? callerModulePath : "unknown", logCount + 1);
        }
        return;
    }

    // ExecuteCommandLists may hit this many times per frame on the same queue.
    // Once we've captured the active DIRECT queue, avoid the repeated GetDesc /
    // lock / QueryInterface work on the hot path.
    if (g_CommandQueue.load(std::memory_order_acquire) == pQueue)
        return;

    ID3D12CommandQueue* primaryQ = g_PrimaryGameQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        currentSwapchainQueue = g_SwapchainQueue;
    }

    if (ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(
            callerFromThirdPartyOverlay, g_OriginalGameQueue != nullptr, pQueue == primaryQ,
            pQueue == g_OriginalGameQueue, pQueue == currentSwapchainQueue)) {
        static std::atomic<int> s_overlayQueueIgnoreLogCount{0};
        int logCount = s_overlayQueueIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: SetCommandQueue ignoring foreign overlay queue %p from caller %s "
                "(primary=%p orig=%p scQ=%p current=%p)",
                pQueue, (callerModulePath && *callerModulePath) ? callerModulePath : "unknown", primaryQ,
                g_OriginalGameQueue, currentSwapchainQueue, g_CommandQueue.load(std::memory_order_acquire));
        }
        return;
    }

    const bool recentStreamlineTeardown = g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool postSLActive = g_PostSLOverlayActive.load(std::memory_order_acquire);
    const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        g_HadFSRFGPhase, g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
        currentSwapchainQueue != nullptr);
    const bool lastWorkingQueueStillActiveDuringRecentTeardown =
        g_PostSLLastWorkingQueue != nullptr &&
        GetTickCount64() < g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
            recentStreamlineTeardown, postFSRNonFGRecovery, lastWorkingQueueStillActiveDuringRecentTeardown,
            pQueue == primaryQ, pQueue == g_OriginalGameQueue, pQueue == currentSwapchainQueue,
            pQueue == g_PostSLLastWorkingQueue)) {
        if (ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(
                recentStreamlineTeardown, g_PostSLLastWorkingQueue && pQueue == g_PostSLLastWorkingQueue,
                streamlineFGRunning, postSLActive)) {
            MarkPostSLRecentTeardownActivity("DX12: SetCommandQueue recent PostSL teardown activity", pQueue);
        }
        static std::atomic<int> s_recentSLTeardownSetQueueIgnoreLogCount{0};
        int logCount = s_recentSLTeardownSetQueueIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: SetCommandQueue ignoring departed queue %p during Streamline teardown / post-FSR recovery "
                "(primary=%p orig=%p scQ=%p current=%p slOffGrace=%d postSLRecent=%d postFSR=%d)",
                pQueue, primaryQ, g_OriginalGameQueue, currentSwapchainQueue,
                g_CommandQueue.load(std::memory_order_acquire), g_SLOffHeuristicGrace.load(std::memory_order_acquire),
                lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0, postFSRNonFGRecovery ? 1 : 0);
        }
        return;
    }

    // CRITICAL FIX: Only allow DIRECT queues for overlay rendering.
    // Strange Brigade and other DX12 games use Async Compute queues.
    // Submitting overlay (Direct) commands to a Compute queue causes a device
    // lost/crash.
    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // HookLog("DX12: Ignoring non-direct queue (Type=%d)", desc.Type);
        return;
    }

    // Set primary game queue once — the first DIRECT queue seen is always the
    // game's queue (created before any FG runtime initializes).  Used to filter
    // ECL counting for accurate real-vs-interpolated frame classification.
    ID3D12CommandQueue* expected = nullptr;
    g_PrimaryGameQueue.compare_exchange_strong(expected, pQueue, std::memory_order_acq_rel);

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (g_CommandQueue.load() != pQueue) {
        if (g_CommandQueue.load())
            g_CommandQueue.load()->Release();
        g_CommandQueue.store(pQueue);
        pQueue->AddRef();

        // Re-check vtable before GetDevice — another thread may have freed
        // the queue between GetDesc and here.  Volatile prevents caching.
        auto vtblRecheck = *reinterpret_cast<void* volatile const*>(pQueue);
        if (!vtblRecheck) {
            HookLogImportant("DX12: SetCommandQueue — queue %p freed during registration (vtable null after store)",
                             pQueue);
            return;
        }

        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&dev)))) {
            DX12_PublishNativeLimiterDevice(dev, pQueue, "command queue");
            if (g_Device.load() != dev) {
                if (g_Device.load())
                    g_Device.load()->Release();
                g_Device.store(dev);

                // Clear device-removed flag — a new device means recovery.
                g_DeviceRemoved.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.deviceRemovedFatal.store(false, std::memory_order_release);
                g_RenderWatchdog.SetForceMonitor(false);

                // Reset primary game queue — new device means new queues.
                g_PrimaryGameQueue.store(pQueue, std::memory_order_release);
                g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);
                g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);

                // Report GPU LUID for host metrics (PDH counter filtering).
                // ID3D12Device has GetAdapterLuid() directly — don't use
                // IDXGIDevice (D3D12 devices don't implement it).
                LUID adapterLuid = dev->GetAdapterLuid();
                ReportLUID(adapterLuid.LowPart, adapterLuid.HighPart);
                HookLog("DX12: Reported LUID %08x-%08x", adapterLuid.HighPart, adapterLuid.LowPart);
            } else
                dev->Release();
        }
    }

    // CRITICAL FIX: Hook queue vtable lazily here instead of during swapchain
    // creation This prevents hangs during DXGI internal operations
    DX12_HookQueueVTable(pQueue);
}

extern "C" {
// NOINLINE: Prevents LTO from inlining into the ECL detour, which would
// allow the compiler to merge vtable reads and optimize away our safety checks.
__attribute__((noinline)) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue) {
    DX12_SetCommandQueueInternal(pQueue, false, nullptr);
}

}  // extern "C" (DX12_SetCommandQueue)

// Capture the queue that was passed to CreateSwapChain* so we can prefer it
// for overlay submission.  Only accepts DIRECT queues (same rule as
// DX12_SetCommandQueue).  Also hooks the queue vtable for ECL interception.
static bool DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue, bool authoritativeStreamlineRuntimeQueue,
                                   bool authoritativeFFXRuntimeQueue, bool gameCreatedSwapchain,
                                   IDXGISwapChain* associatedSwapchain, bool authoritativeNormalSwapchainReturn) {
    if (!pQueue)
        return false;

    // Safety: freed COM objects have null vtable — skip
    void** vtblCheck = *reinterpret_cast<void***>(pQueue);
    if (!vtblCheck)
        return false;

    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;

    bool runtimeOwnershipJustActivated = false;

    // Diagnostic: log the queue's device to detect cross-device issues
    ID3D12Device* queueDev = nullptr;
    if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&queueDev)))) {
        auto* curDev = g_Device.load(std::memory_order_acquire);
        if (queueDev != curDev) {
            HookLogImportant("DX12: SetSwapchainQueue — queue %p device %p DIFFERS from g_Device %p", pQueue, queueDev,
                             curDev);
        }
        DX12_PublishNativeLimiterDevice(queueDev, pQueue, "swapchain queue");
        queueDev->Release();
    }

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    g_LastSwapchainQueueCaptureSwapchain.store(associatedSwapchain, std::memory_order_release);
    if (associatedSwapchain && g_OriginalGameQueue && pQueue != g_OriginalGameQueue) {
        IDXGISwapChain* expectedOriginalSwapchain = associatedSwapchain;
        if (g_LastProvenOriginalQueueSwapchain.compare_exchange_strong(
                expectedOriginalSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
            HookLogImportant(
                "DX12: Non-original queue association superseded remembered native ownership for swapchain %p "
                "(queue=%p origGame=%p)",
                associatedSwapchain, pQueue, g_OriginalGameQueue);
        }
    }
    if (g_SwapchainQueue != pQueue) {
        if (g_SwapchainQueue)
            g_SwapchainQueue->Release();
        g_SwapchainQueue = pQueue;
        g_SwapchainQueue->AddRef();
        g_SwapchainQueueCaptureTime = GetTickCount64();
