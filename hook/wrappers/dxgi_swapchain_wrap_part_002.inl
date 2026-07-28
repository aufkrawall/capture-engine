    m_OverlayResourcesValid.store(false, std::memory_order_release);
}

void CWrapDXGISwapChain::WaitFrameLatency() {
    const auto& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount))
        return;

    HANDLE waitable = EnsureFrameLatencyWaitable("backbuffer pacing");
    if (waitable && waitable != INVALID_HANDLE_VALUE) {
        DWORD waitResult = WaitForSingleObject(waitable, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            static std::atomic<int> s_waitFailLogCount{0};
            if (s_waitFailLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                WrapperLog("WaitFrameLatency: wait failed result=%lu error=%lu", waitResult, GetLastError());
            }
        }
    }
}

HANDLE CWrapDXGISwapChain::EnsureFrameLatencyWaitable(const char* reason) {
    if (m_FrameLatencyWaitableQueried) {
        return m_hFrameLatencyWaitable;
    }

    EnsurePromoted();
    m_FrameLatencyWaitableQueried = true;

    if (!m_pReal2) {
        static std::atomic<int> s_noSwapchain2Log{0};
        const int n = s_noSwapchain2Log.fetch_add(1, std::memory_order_relaxed);
        if (n < 10) {
            WrapperLog("Frame latency waitable unavailable for %s: IDXGISwapChain2 not available",
                       reason ? reason : "unknown");
        }
        m_hFrameLatencyWaitable = nullptr;
        return m_hFrameLatencyWaitable;
    }

    m_hFrameLatencyWaitable = m_pReal2->GetFrameLatencyWaitableObject();
    if (m_hFrameLatencyWaitable && m_hFrameLatencyWaitable != INVALID_HANDLE_VALUE) {
        static std::atomic<int> s_waitableLog{0};
        const int n = s_waitableLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20) {
            WrapperLog("Frame latency waitable obtained for %s (waitable=%p)", reason ? reason : "unknown",
                       m_hFrameLatencyWaitable);
        }
        return m_hFrameLatencyWaitable;
    }

    static std::atomic<int> s_invalidWaitableLog{0};
    const int n = s_invalidWaitableLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 20) {
        WrapperLog("Frame latency waitable unavailable for %s: GetFrameLatencyWaitableObject returned %p",
                   reason ? reason : "unknown", m_hFrameLatencyWaitable);
    }
    m_hFrameLatencyWaitable = nullptr;
    return m_hFrameLatencyWaitable;
}

void CWrapDXGISwapChain::WaitD3D12FocusLossOverlayFenceAfterPresent(
    const char* presentName, int callCount, UINT syncInterval, UINT presentFlags, HRESULT presentHr,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo& flushInfo) {
    if (!m_IsD3D12) {
        return;
    }

    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext context = {};
    context.presentName = presentName;
    context.callCount = callCount;
    context.isD3D12Swapchain = m_IsD3D12;
    context.isFullscreen = m_State.isFullscreen;
    context.processHasForeground = processHasForeground;
    context.isIconic = (m_hWnd != nullptr) && IsIconic(m_hWnd);
    context.hasZeroSize = (m_State.width == 0 || m_State.height == 0);
    context.presentSucceeded = SUCCEEDED(presentHr);
    context.presentDeviceLost = IsD3D12PresentDeviceLostHRESULT(presentHr);
    context.frameGenerationActive = g_FGCompat.IsFGActive();
    context.runtimeOwnedPresentation =
        DXGIShared::DoesFGRuntimeOwnSwapchain() || DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
    context.usingDedicatedQueue = false;
    context.foregroundWindow = foregroundWindow;
    context.foregroundPid = foregroundPid;
    context.gameWindow = m_hWnd;
    context.processId = GetCurrentProcessId();
    context.syncInterval = syncInterval;
    context.presentFlags = presentFlags;
    context.presentHr = presentHr;
    DX12_WaitForFocusLossOverlayFenceAfterPresent(&context, &flushInfo);
}

void CWrapDXGISwapChain::ProbeD3D12FocusLossFrameLatencyAfterPresent(const char* presentName, int callCount,
                                                                     UINT syncInterval, UINT presentFlags,
                                                                     HRESULT presentHr) {
    if (!m_IsD3D12) {
        return;
    }

    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    const bool isIconic = (m_hWnd != nullptr) && IsIconic(m_hWnd);
    const bool hasZeroSize = (m_State.width == 0 || m_State.height == 0);
    const bool presentSucceeded = SUCCEEDED(presentHr);
    const bool frameGenerationActive = g_FGCompat.IsFGActive();
    const bool runtimeOwnedPresentation =
        DXGIShared::DoesFGRuntimeOwnSwapchain() || DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();

    const bool focusLossTelemetryCandidate = m_IsD3D12 && !m_State.isFullscreen && !processHasForeground && !isIconic &&
                                             !hasZeroSize && presentSucceeded && !frameGenerationActive &&
                                             !runtimeOwnedPresentation;
    HANDLE waitable = INVALID_HANDLE_VALUE;
    const bool hasFrameLatencyWaitable = waitable && waitable != INVALID_HANDLE_VALUE;
    const bool shouldWait = DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        m_IsD3D12, m_State.isFullscreen, processHasForeground, isIconic, hasZeroSize, presentSucceeded,
        frameGenerationActive, runtimeOwnedPresentation, hasFrameLatencyWaitable);

    if (!shouldWait) {
        if (!processHasForeground) {
            static std::atomic<int> s_focusSkipLog{0};
            const int n = s_focusSkipLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 20 || (n % 1000) == 0) {
                WrapperLog(
                    "%s#%d: D3D12 focus-loss frame-latency waitable telemetry probe skipped "
                    "(fg=%p/%lu ours=%p/%lu sync=%u flags=0x%08X presentHr=0x%08X fullscreen=%d iconic=%d "
                    "zeroSize=%d fgActive=%d runtimeOwned=%d waitable=%p available=%d candidate=%d "
                    "reason=present-passthrough-v7)",
                    presentName, callCount, foregroundWindow, foregroundPid, m_hWnd, GetCurrentProcessId(),
                    syncInterval, presentFlags, (unsigned)presentHr, m_State.isFullscreen ? 1 : 0, isIconic ? 1 : 0,
                    hasZeroSize ? 1 : 0, frameGenerationActive ? 1 : 0, runtimeOwnedPresentation ? 1 : 0,
                    hasFrameLatencyWaitable ? waitable : nullptr, hasFrameLatencyWaitable ? 1 : 0,
                    focusLossTelemetryCandidate ? 1 : 0);
            }
        }
        return;
    }

    constexpr DWORD kFocusLossFrameLatencyProbeMs = 0;
    DWORD waitResult = WaitForSingleObject(waitable, kFocusLossFrameLatencyProbeMs);
    DWORD waitLastError = (waitResult == WAIT_FAILED) ? GetLastError() : 0;

    static std::atomic<int> s_focusWaitLog{0};
    const int n = s_focusWaitLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 50 || waitResult != WAIT_OBJECT_0 || (n % 300) == 0) {
        WrapperLog(
            "%s#%d: D3D12 focus-loss frame-latency waitable telemetry probe result=%s(0x%08lX) "
            "fg=%p/%lu ours=%p/%lu sync=%u flags=0x%08X waitable=%p available=1 timeoutMs=%lu gle=%lu",
            presentName, callCount, WaitResultName(waitResult), waitResult, foregroundWindow, foregroundPid, m_hWnd,
            GetCurrentProcessId(), syncInterval, presentFlags, waitable, kFocusLossFrameLatencyProbeMs, waitLastError);
    }
}

void CWrapDXGISwapChain::DrawOverlay() {
    static int s_DrawCount = 0;
    bool shouldLog = (++s_DrawCount <= 10);

    if (!g_OverlayEnabled) {
        if (shouldLog)
            WrapperLog("DrawOverlay: skipped (overlay disabled)");
        return;
    }
    if (shouldLog)
        WrapperLog("DrawOverlay: m_IsD3D12=%d", m_IsD3D12);
    if (m_IsD3D12) {
        // DX12: ProcessFrameExternal is now called directly from Present/Present1
        // to ensure capture works even when overlay is disabled
    } else {
        DrawDX11Overlay(m_pReal);
    }
}

// ============================================================================
// IUnknown Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;
    EnsurePromoted();

    // CRITICAL FIX: Block Streamline base interface to prevent FSR FG / DLSS FG
    // from unwrapping
    if (IsEqualGUID(riid, IID_IStreamlineBaseInterface)) {
        WrapperLog(
            "SwapChain: BLOCKED Streamline interface query (FSR FG/DLSS FG "
            "unwrap attempt)");
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    // Allow retrieval of wrapper from real swapchain (internal use only)
    if (IsEqualGUID(riid, IID_CWrapDXGISwapChain)) {
        AddRef();
        *ppvObj = this;  // Return wrapper, NOT real swapchain
        WrapperLog(
            "SwapChain: QueryInterface for IID_CWrapDXGISwapChain - "
            "returning wrapper %p",
            this);
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_IDXGIObject || riid == IID_IDXGIDeviceSubObject ||
        riid == IID_IDXGISwapChain) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGISwapChain1 && m_Version >= 1) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain1*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain2 && m_Version >= 2) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain2*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain3 && m_Version >= 3) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain3*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain4 && m_Version >= 4) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    // Log unknown interface queries for debugging
    static std::atomic<int> s_LogCount{0};
    if (s_LogCount < 20) {
        s_LogCount++;
        LPOLESTR strIID = nullptr;
        if (SUCCEEDED(StringFromIID(riid, &strIID))) {
            WrapperLog("SwapChain: QueryInterface for unknown IID: %S", strIID);
            CoTaskMemFree(strIID);
        }
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::AddRef() {
    // SAFETY: Check if we're in shutdown - if so, return fake reference count
    if (g_WrapperShutdown.load(std::memory_order_acquire)) {
        return 1;
    }

    // Also check if this wrapper has already been destroyed
    if (m_RefCount == 0) {
        return 1;  // Already destroyed or never initialized
    }

    ULONG refs = InterlockedIncrement(&m_RefCount);
    // Also AddRef the real swapchain to track total references
    if (m_pReal) {
        m_pReal->AddRef();
        m_RealSwapchainRefs.fetch_add(1);
    }
    return refs;
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::Release() {
    // SAFETY: Check if we're in shutdown - if so, just return without touching
    // anything
    if (g_WrapperShutdown.load(std::memory_order_acquire)) {
        return 0;
    }

    // Also check if this wrapper has already been destroyed
    if (m_RefCount == 0) {
        // Already destroyed or never initialized
        return 0;
    }

    ULONG xrefs = InterlockedDecrement(&m_RefCount);  // External references

    // When external refs reach 0, game expects SwapChain destruction
    if (xrefs == 0) {
        WrapperLog(
            "SwapChain: External refs reached 0, preparing for destruction "
            "(wrapper=%p)",
            this);
        // CRITICAL: Mark releasing BEFORE calling CleanupOverlayResources and
        // m_pReal->Release(). During these calls, D3D12/DXGI cleanup can trigger
        // re-entrant calls back through the wrapper (e.g. SetPrivateData via
        // Streamline interposer callbacks). IsWrapperZombie() checks this flag
        // and rejects forwarding to the already-destroyed swapchain.
        m_Releasing.store(true, std::memory_order_release);
        CleanupOverlayResources();
    }

    // Release the real swapchain (if not already nulled by DestructionCallback)
    ULONG refs = 0;
    if (m_pReal) {
        refs = m_pReal->Release();
        m_RealSwapchainRefs.fetch_sub(1);
    }

    // CRITICAL FIX: Only delete when external refs are 0
    // The real swapchain's refcount is independent - it will be released when its
    // own refcount reaches 0. We must NOT wait for refs == 0 because:
    // 1. DestructionCallback may have nulled m_pReal before we could Release()
    // 2. Waiting for refs == 0 would cause the wrapper to leak if real swapchain
    //    has external refs (e.g., from GPU, other COM clients, etc.)
    // 3. The wrapper's lifetime is controlled by external AddRef/Release on the
    // wrapper
    if (xrefs == 0) {
        WrapperLog("SwapChain: Deleting wrapper %p (real refs=%u, wrapper refs=%u)", this, refs, xrefs);
        delete this;
    }

    return xrefs;  // Return external ref count
}

IDXGISwapChain* CWrapDXGISwapChain::GetRealSafe() {
    if (m_SwapchainDestroyed.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return m_pReal;
}

// Shutdown safety function - sets the global shutdown flag
void SetSwapchainWrapperShutdown() {
    g_WrapperShutdown.store(true, std::memory_order_release);
    WrapperLog("SwapChain: Wrapper shutdown flag set");
}

// ============================================================================
// IDXGIObject Implementation
// ============================================================================

// ============================================================================
// IDXGIObject Implementation
// ============================================================================

inline bool CWrapDXGISwapChain::IsWrapperZombie() const {
    return m_Releasing.load(std::memory_order_acquire) || m_RefCount == 0 ||
           m_DestructorCalled.load(std::memory_order_acquire) || g_WrapperShutdown.load(std::memory_order_acquire);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    ScopedAvGuard guard;
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    ScopedAvGuard guard;
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    if (IsUnwrapAttemptGUID(Name))
        return DXGI_ERROR_NOT_FOUND;
    ScopedAvGuard guard;
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetParent(REFIID riid, void** ppParent) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    ScopedAvGuard guard;
    return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIDeviceSubObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDevice(REFIID riid, void** ppDevice) {
    return m_pReal->GetDevice(riid, ppDevice);
}

// ============================================================================
// IDXGISwapChain Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {
    PresentDebugSample debugSample = {};
    PresentDebugSample* activeDebugSample = nullptr;
    int64_t presentStartUs = 0;
    FrameMetrics perfMetrics = {};
    const bool perfLoggingEnabled = !m_IsD3D12 && PerfLogger::Get().IsEnabled();
    const bool phaseTimingEnabled = perfLoggingEnabled;
    if (perfLoggingEnabled) {
        perfMetrics.qpcUs = PerfLogger::GetQpcUs();
        strncpy(perfMetrics.api, DetectWrappedSwapchainApi(m_pDevice, m_IsD3D12), sizeof(perfMetrics.api) - 1);
        perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
    }
    // Outermost present row: skip when the forwarded present logged an inner row (DetourPresent catch-all
    // or a per-API ProcessFrame row) so a present never writes two CSV rows (present-rate dedup).
    if (perfLoggingEnabled) {
        PerfLogger::BeginPresentRowScope();
    }
    auto perfGuard = ::ce::make_scope_guard([&] {
        if (perfLoggingEnabled && !PerfLogger::InnerRowLoggedInPresentRowScope()) {
            perfMetrics.totalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - perfMetrics.qpcUs);
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });
    if (m_IsD3D12 && PerfLogger::Get().IsEnabled()) {
        static std::atomic<uint64_t> s_presentDebugFrame{0};
        uint64_t debugFrameNum = s_presentDebugFrame.fetch_add(1, std::memory_order_relaxed) + 1;
        if (PerfLogger::Get().ShouldSampleDetailedFrame(debugFrameNum)) {
            activeDebugSample = &debugSample;
            activeDebugSample->frameNum = debugFrameNum;
            strncpy(activeDebugSample->api, "DX12", sizeof(activeDebugSample->api) - 1);
            activeDebugSample->api[sizeof(activeDebugSample->api) - 1] = '\0';
            presentStartUs = PerfLogger::GetQpcUs();
            PerfLogger::Get().ActivateDebugSample(activeDebugSample);
        }
    }
    auto debugSampleGuard = ::ce::make_scope_guard([&] {
        if (activeDebugSample) {
            activeDebugSample->wrapperTotalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - presentStartUs);
            PerfLogger::Get().DeactivateDebugSample(activeDebugSample);
            PerfLogger::Get().CommitDebugSample(*activeDebugSample);
        }
    });

    DXGIShared::g_SharedState.presentInFlightDepth.fetch_add(1, std::memory_order_acq_rel);
    auto presentInFlightGuard = ::ce::make_scope_guard(
        []() { DXGIShared::g_SharedState.presentInFlightDepth.fetch_sub(1, std::memory_order_acq_rel); });

    // EXTREME DEBUG: Log entry with full state
    DWORD threadId = GetCurrentThreadId();
    static std::atomic<int> s_presentCallCount{0};
    int callCount = s_presentCallCount.fetch_add(1);

    SharedMemoryLayout* debugSharedMem = (g_IPC && g_IPC->GetSharedMem()) ? g_IPC->GetSharedMem() : g_pSharedMem;
    if (perfLoggingEnabled && debugSharedMem) {
        perfMetrics.sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        perfMetrics.sourceCapturePhase = debugSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = debugSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (debugSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags =
            debugSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (perfLoggingEnabled) {
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(perf->GetCurrentFPS() * 100.0f + 0.5f);
            perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(perf->Get1PercentLowFPS() * 100.0f + 0.5f);
            perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(perf->Get01PercentLowFPS() * 100.0f + 0.5f);
            perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(perf->GetWindowStdDev() + 0.5);
        }
    }
    if (activeDebugSample && debugSharedMem) {
        activeDebugSample->sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        activeDebugSample->capturePhase = debugSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        activeDebugSample->encoderQueueDepth = debugSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        activeDebugSample->muxQueueKb =
            (debugSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        activeDebugSample->overloadFlags =
            debugSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }

    // CRITICAL FIX: Lock mutex to protect swapchain pointer access
    // This prevents race conditions with DestructionCallback running on another
    // thread
    IDXGISwapChain* pRealCached = nullptr;
    const int64_t swapchainAcquireStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    {
        std::lock_guard<std::mutex> lock(m_ResourceLock);

        // CRITICAL FIX: Cache the pointer while holding the mutex
        // This ensures we have a valid pointer even if DestructionCallback nulls
        // m_pReal
        pRealCached = m_pReal;
        m_pRealCached = pRealCached;  // Store for potential future use

        // CRITICAL FIX: AddRef to keep the swapchain alive while we're using it
        // This prevents use-after-free if DestructionCallback runs after we release
        // the mutex
        if (pRealCached) {
            pRealCached->AddRef();
        }
    }
    if (activeDebugSample) {
        activeDebugSample->swapchainAcquireUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - swapchainAcquireStartUs);
    }
    // CRITICAL FIX: RAII guard to ensure Release is always called
    auto realSwapchainGuard = ::ce::make_scope_guard([&] {
        if (pRealCached) {
            pRealCached->Release();
        }
    });

    if (callCount < 10) {
        WrapperLog("Present ENTRY #%d - Thread=%lu, m_pReal=%p, m_IsD3D12=%d", callCount, threadId, pRealCached,
                   m_IsD3D12);
        WrapperLog("Present state - fgActive=%d, flipModel=%d", g_FGCompat.IsFGActive(), m_FlipModel.active);
    }

    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        if (pRealCached) {
            return pRealCached->Present(SyncInterval, Flags);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    // CRITICAL FIX: Check if real swapchain has been destroyed (e.g., by FSR FG
    // recreation) If so, just pass through to avoid crashes with stale pointer
    // NOTE: We check m_SwapchainDestroyed but we still have a valid ref on
    // pRealCached thanks to AddRef, so it's safe to use
    if (!pRealCached || m_SwapchainDestroyed.load()) {
        // Real swapchain was destroyed, wrapper is now invalid
        // This happens when FSR FG creates a new swapchain
        WrapperLog("Present: Real swapchain destroyed (FSR FG swapchain recreation)");
        if (pRealCached) {
            // Safe to use because we AddRef'd it
            return pRealCached->Present(SyncInterval, Flags);
        }
        WrapperLog("Present: pRealCached is null, returning error");
        return DXGI_ERROR_INVALID_CALL;
    }

    // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
    // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG
    // active.  BUT skip heartbeat after device removal so the watchdog can fire.
    if (!DXGIShared::g_SharedState.deviceRemovedFatal.load(std::memory_order_relaxed))
        g_RenderWatchdog.HeartbeatFromHelperThread();

    // FSR FG FIX: Skip overlay processing on FSR internal swapchains
    // FSR creates internal swapchains for frame generation that we should not
    // interfere with
    if (IsFSRInternalSwapchain()) {
        if (callCount < 10) {
            WrapperLog("Present: Skipping FSR internal swapchain processing");
        }
        HRESULT hr = pRealCached->Present(SyncInterval, Flags);
        return hr;
    }

    // NVIDIA Smooth Motion compatibility: skip overlay/processing for invisible
    // windows (NvPresent64 may create them for DX12 frame interpolation too)
    if (g_FGCompat.IsNvPresentLoaded() && m_hWnd && !IsWindowVisible(m_hWnd)) {
        return pRealCached->Present(SyncInterval, Flags);
    }

    if (ShouldYieldToVulkanLayer()) {
        static std::atomic<int> s_vulkanYieldLog{0};
        if (s_vulkanYieldLog.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present: Vulkan layer is presenting, bypassing DXGI wrapper path");
        }
        return pRealCached->Present(SyncInterval, Flags);
    }

    const char* delegationOverlayModule = nullptr;
    if (m_IsD3D12 && ShouldDelegateDX12PresentToDetourHook(&delegationOverlayModule)) {
        static std::atomic<int> s_inlineRouteLogCount{0};
        if (s_inlineRouteLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present: Delegating DX12 Present to detour hook for external overlay %s",
                       delegationOverlayModule ? delegationOverlayModule : "module");
        }
        const bool previousInWrapperPresent = g_InWrapperPresent;
        g_InWrapperPresent = false;
        auto delegateGuard =
            ::ce::make_scope_guard([previousInWrapperPresent]() { g_InWrapperPresent = previousInWrapperPresent; });
        return pRealCached->Present(SyncInterval, Flags);
    }

    // Only advertise wrapper-managed Present after ruling out the delegated
    // external-overlay path. Otherwise DetourPresent sees a wrapper call and
    // bounces back into the original chain immediately.
    g_InWrapperPresent = true;
    auto wrapperPresentGuard = ::ce::make_scope_guard([&] { g_InWrapperPresent = false; });

    // DEBUG: Log first few Present calls to verify wrapper is being invoked
    if (callCount < 10) {
        WrapperLog("Present: call#%d (m_IsD3D12=%d, flipModel=%d, FG=%d)", callCount, m_IsD3D12, m_FlipModel.active,
                   g_FGCompat.IsFGActive());
    }

    // FSR FG/DLSS FG compatibility: don't override sync interval when FG is
    // active — it can break frame pacing. Still process capture though.
    bool fgActive = g_FGCompat.IsFGActive();

    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    // Using atomic+threadId instead of thread_local to avoid static destructor
    // issues
    static std::atomic<DWORD> s_presentThreadId{0};
    static std::atomic<int> s_presentDepth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_presentDepth.load() > 0 && s_presentThreadId.load() == currentId) {
        WrapperLog("Present: Recursion detected, passing through to real swapchain");
        return pRealCached->Present(SyncInterval, Flags);
    }
    s_presentThreadId.store(currentId);
    s_presentDepth.fetch_add(1);
    auto depthGuard = ::ce::make_scope_guard([&] {
        if (s_presentDepth.fetch_sub(1) == 1)
            s_presentThreadId.store(0);
    });
    DXGIShared::BeginPostSLOffKeepAlivePresentScope();
    // Span both ProcessFrame and the real Present re-entry: DLSS can report its
    // suspend edge between them, and either side may be the first safe draw.
    auto postSLOffKeepAlivePresentScopeGuard =
        ::ce::make_scope_guard([]() { DXGIShared::EndPostSLOffKeepAlivePresentScope(); });

    if (callCount < 20) {
        WrapperLog("Present: Processing call#%d", callCount);
    }

    // Update performance metrics for FPS calculation
    static int64_t qpcFreq = 0;
    const int64_t metricsUpdateStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    DXGIShared::GetPerformanceMetrics()->Update(us);
    if (activeDebugSample) {
        activeDebugSample->metricsUpdateUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - metricsUpdateStartUs);
    }

    // Apply VSync override from config (skip if FG is active - can break frame
    // pacing)
    if (!fgActive) {
        ProcessVSyncOverride(SyncInterval, Flags);
    } else if (callCount < 20) {
        WrapperLog("Present: Skipping VSync override because FG is active");
    }

    // Process frame for capture BEFORE calling real Present
    const int64_t processFrameStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    bool dx12PresentContextArmed = false;
    auto dx12PresentContextGuard = ::ce::make_scope_guard([&] {
        if (dx12PresentContextArmed) {
            DX12_ClearWrappedPresentFocusLossContext();
        }
    });
    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present", callCount, SyncInterval, Flags);
        dx12PresentContextArmed = true;
    }
