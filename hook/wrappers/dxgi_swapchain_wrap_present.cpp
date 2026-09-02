#include "dxgi_swapchain_wrap_internal.h"

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain* pReal, IUnknown* pDevice)
    : CWrapDXGISwapChain(pReal, pDevice, /*streamlineRuntimeNonRetaining=*/false) {
}

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain* pReal, IUnknown* pDevice, bool streamlineRuntimeNonRetaining)
    : m_pReal(pReal),
      m_pReal1(nullptr),
      m_pReal2(nullptr),
      m_pReal3(nullptr),
      m_pReal4(nullptr),
      m_pDevice(pDevice),
      m_pD3D12Queue(nullptr),
      m_RefCount(1),
      m_StreamlineRuntimeNonRetaining(streamlineRuntimeNonRetaining),
      m_hWnd(nullptr),
      m_Version(0),
      m_OverlayResourcesValid(false),
      m_IsD3D12(false),
      m_Promoted(false),
      m_DestructionCookie(0) {
    WrapperLog("SwapChain: CWrapDXGISwapChain CONSTRUCTOR called (real=%p, device=%p)", pReal, pDevice);
    if (pReal) {
        if (!m_StreamlineRuntimeNonRetaining) {
            // Retaining mode owns a real-swapchain reference for the wrapper lifetime.
            // Streamline-runtime mode borrows the CreateSwapChain reference instead: the
            // runtime releases/recreates its swapchain on FG transitions, and any extra ref
            // would pin the old swapchain so the recreate on the same HWND fails with
            // E_ACCESSDENIED (the historical DLSS-G handoff break).
            pReal->AddRef();
        }

        // FIX B: AddRef the device/queue if we store it
        if (m_pDevice) {
            m_pDevice->AddRef();
        }

        // Detect swapchain state (flip model, fullscreen, etc.)
        DetectSwapChainState();

        if (pDevice) {
            if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&m_pD3D12Queue)))) {
                m_IsD3D12 = true;
                DX12_SetCommandQueue(m_pD3D12Queue);
                // FIX: Mark overlay as ready - DX12 systems handle lazy initialization
                // internally
                m_OverlayResourcesValid.store(true, std::memory_order_release);
            }
        }

        // Register for destruction notification (DXGI 1.4+)
        if (!m_StreamlineRuntimeNonRetaining) {
            RegisterDestructionCallback();
        }

        // Store wrapper pointer on real swapchain for retrieval
        void* pThis = this;
        pReal->SetPrivateData(IID_CWrapDXGISwapChain, sizeof(void*), reinterpret_cast<void*>(&pThis));
    }

    WrapperStateManager::Get().RegisterSwapchain(this, pReal);
    WrapperLog("SwapChain: Created wrapper (real=%p, isD3D12=%d, flipModel=%d)", pReal, m_IsD3D12, m_FlipModel.active);
    if (pReal && !m_StreamlineRuntimeNonRetaining) {
        TryInstallSwapchainLifetimeAttribution(pReal);
        LogSwapChainLifetimeDiagnostics(pReal, "create");
    }
}

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain1* pReal, IUnknown* pDevice)
    : CWrapDXGISwapChain(pReal, pDevice, /*streamlineRuntimeNonRetaining=*/false) {
}

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain1* pReal, IUnknown* pDevice, bool streamlineRuntimeNonRetaining)
    : CWrapDXGISwapChain(static_cast<IDXGISwapChain*>(pReal), pDevice, streamlineRuntimeNonRetaining) {
    if (!m_pReal1 && pReal) {
        m_pReal1 = pReal;
        if (!m_StreamlineRuntimeNonRetaining) {
            m_pReal1->AddRef();
        }
        m_Version = 1;
    }
}

void CWrapDXGISwapChain::PromoteInterfaces() {
    if (!m_pReal)
        return;
    try {
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
            if (m_StreamlineRuntimeNonRetaining && m_pReal1) {
                // Borrowed interface: the transferred CreateSwapChain reference keeps the
                // object alive for the wrapper lifetime; never pin it with extra refs.
                m_pReal1->Release();
            }
            m_Version = 1;
        }
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) {
            if (m_StreamlineRuntimeNonRetaining && m_pReal2) {
                m_pReal2->Release();
            }
            m_Version = 2;
        }
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
            if (m_StreamlineRuntimeNonRetaining && m_pReal3) {
                m_pReal3->Release();
            }
            m_Version = 3;
        }
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
            if (m_StreamlineRuntimeNonRetaining && m_pReal4) {
                m_pReal4->Release();
            }
            m_Version = 4;
        }
    } catch (...) {
        // A foreign swapchain (Streamline/FFX proxies in particular) can throw out
        // of QueryInterface. Keeping the version reached so far is the right
        // fallback, but swallowing it silently hid why an FG proxy was treated as
        // an older interface than it really is.
        static std::atomic<uint64_t> s_promoteFailures{0};
        const uint64_t failures = s_promoteFailures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures <= 3 || (failures % 100ull) == 0ull) {
            HookLog("[DXGI] Swapchain interface promotion threw; retaining version %u (occurrence %llu)", m_Version,
                    static_cast<unsigned long long>(failures));
        }
    }
}

void CWrapDXGISwapChain::DrawOverlay() {
    static int s_DrawCount = 0;
    bool shouldLog = (++s_DrawCount <= 10);

    if (!dxgi_swapchain_wrap_g_OverlayEnabled) {
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

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {
    if (HookIsShuttingDown()) {
        IDXGISwapChain* real = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_ResourceLock);
            real = m_pReal;
            if (real)
                real->AddRef();
        }
        if (!real)
            return DXGI_ERROR_INVALID_CALL;
        const HRESULT result = real->Present(SyncInterval, Flags);
        real->Release();
        return result;
    }
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

    SharedMemoryLayout* debugSharedMem = GetHookSharedMemory();
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
    perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(std::lround(perf->GetCurrentFPS() * 100.0f));
    perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get1PercentLowFPS() * 100.0f));
    perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get01PercentLowFPS() * 100.0f));
    perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(std::lround(perf->GetWindowStdDev()));
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
    if (m_IsD3D12 && ShouldDelegateDX12PresentToDetourHook(&delegationOverlayModule, m_StreamlineRuntimeNonRetaining)) {
        static std::atomic<int> s_inlineRouteLogCount{0};
        if (s_inlineRouteLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present: Delegating DX12 Present to detour hook for external overlay %s%s",
                       delegationOverlayModule ? delegationOverlayModule : "module",
                       m_StreamlineRuntimeNonRetaining ? " (Streamline-runtime wrapper passthrough)" : "");
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

    // The wrapper sits ABOVE the dxgi!Present entry, so anything hooking that entry composites
    // after CE does. Record the site so a session log states the overlay layering outright
    // instead of leaving it to be reconstructed from the install-time hook lines.
    DXGIShared::NoteOverlayCompositeSite(DXGIShared::OverlayCompositeSite::kSwapchainWrapper,
                                         "CWrapDXGISwapChain::Present");

    // In leave-entry mode the Streamline-runtime wrapper is CE's ONLY present entry point
    // (DetourPresent never runs), so feed the Streamline present-stall detector here. Without
    // this, slDLSSGSetOptions compares a frozen counter and falsely dumps "Present STALLED
    // for 30 frames — vtable hook bypassed?" (session 20260812_040330).
    if (m_StreamlineRuntimeNonRetaining) {
        DXGIShared::g_PresentCallCounter.fetch_add(1, std::memory_order_relaxed);
    }

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
    const int64_t metricsUpdateStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    DXGIShared::GetPerformanceMetrics()->Update(PerfLogger::GetQpcUs());
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

    if (m_IsD3D12) {
        DX12_ProcessFrameExternal(pRealCached);
        if (m_StreamlineRuntimeNonRetaining) {
            DXGIShared::MaybeInvokePostSLOverlayRenderFromWrappedRuntimePresent(
                pRealCached, "Wrapped Streamline runtime Present");
        }
    } else {
        // DX11/DX10: DX11_ProcessFrameExternal handles both capture AND overlay
        DX11_ProcessFrameExternal(pRealCached);
        if (DX11Hook_ShouldPassThroughCurrentPresent())
            return pRealCached->Present(SyncInterval, Flags);
        if (perfLoggingEnabled) {
            perfMetrics.captureUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - processFrameStartUs);
        }
    }
    if (activeDebugSample) {
        activeDebugSample->processFrameExternalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - processFrameStartUs);
    }

    // FPS Limiter - arm frame pacing before present. Explicit CE-owned Reflex
    // cadence is finished after Present returns so the wait happens before the
    // game starts building the next frame.
    // This applies to both DX11 and DX12.
    const int64_t limiterStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    if (g_IPC) {
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply(true);
        DXGIShared::ApplyPresentFrameLatencyOverrides(pRealCached);
    }

    if (activeDebugSample) {
        activeDebugSample->fpsLimiterUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - limiterStartUs);
    }
    if (perfLoggingEnabled) {
        perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - limiterStartUs);
    }

    // When the FPS limiter is active, override SyncInterval to 0 for precise frame pacing.
    // SyncInterval=0 on FLIP model means "present at next vblank, non-blocking if queue not full."
    // This is tear-free (frames still sync to vblank) but avoids the vsync blocking that
    // would absorb the limiter's delay. The present queue drains at display refresh rate;
    // since our limiter targets <= display rate, the queue never saturates and Present
    // returns immediately, letting the limiter control the actual frame cadence.
    // We do NOT use DXGI_PRESENT_ALLOW_TEARING — it bypasses vblank sync entirely and
    // causes visible tearing with DirectFlip (even in windowed/borderless mode).
    if (g_SharedFpsLimiter.IsActivelyLimiting() && !m_State.isFullscreen) {
        static int s_syncLog = 0;
        if (s_syncLog++ < 30) {
            WrapperLog("Present: Limiter active, SyncInterval %u->0 (vblank-synced, tear-free)", SyncInterval);
        }
        SyncInterval = 0;
    }

    // When non-DX12 flip-model apps are not in the foreground, the GPU can be
    // throttled by the driver. Present() with SyncInterval>0 may block waiting
    // for the flip queue to drain, so DX11/DX10 can use DO_NOT_WAIT. D3D12 is
    // different: the app often keeps building command lists while unfocused, and
    // forcing DO_NOT_WAIT can create an unbounded ECL/Present loop that hangs
    // the device. Preserve D3D12 Present pacing and let the overlay visually
    // stall instead of disappearing or destabilizing the queue.
    UINT presentFlags = Flags;
    if (m_hWnd && !m_State.isFullscreen) {
        HWND foreground = GetForegroundWindow();
        if (foreground != m_hWnd) {
            static std::atomic<int> s_focusLossLog{0};
            int n = s_focusLossLog.fetch_add(1, std::memory_order_relaxed);
            const bool applyDoNotWait = DXGIShared::ShouldApplyUnfocusedFlipModelDoNotWait(
                m_IsD3D12, m_State.isFullscreen, false, presentFlags);
            if (n == 0 || n % 300 == 0) {
                WrapperLog("Present#%d: Not foreground (fg=%p vs ours=%p), %s", callCount, foreground, m_hWnd,
                           applyDoNotWait ? "SyncInterval ->0 + DO_NOT_WAIT (non-DX12 GPU throttle protection)"
                                          : "preserving Present pacing (D3D12 focus-loss safety)");
            }
            if (applyDoNotWait) {
                SyncInterval = 0;
                presentFlags |= 0x00000008U;  // DO_NOT_WAIT
            }
        }
    }

    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present", callCount, SyncInterval, presentFlags);
        DX12_WaitForOverlayCompletion(nullptr);
    }

    const int64_t presentCallStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    HRESULT hr = pRealCached->Present(SyncInterval, presentFlags);
    if (m_IsD3D12) {
        const BOOL isIconic = (m_hWnd != nullptr) ? IsIconic(m_hWnd) : FALSE;
        const BOOL hasZeroSize = (m_State.width == 0 || m_State.height == 0) ? TRUE : FALSE;
        DX12_NoteWrappedD3D12PresentResult("Present", callCount, SyncInterval, presentFlags, hr,
                                           m_State.isFullscreen ? TRUE : FALSE, isIconic, hasZeroSize, m_hWnd);
    }
    const bool flushProcessHasForeground = m_IsD3D12 ? ResolveCurrentProcessForeground(nullptr, nullptr) : true;
    const bool focusLostForFlush = m_IsD3D12 && !flushProcessHasForeground && !m_State.isFullscreen;
    const auto flushInfo =
        FlushDeferredDX12OverlaySignalAfterWrappedPresent(m_IsD3D12, "Present", callCount, focusLostForFlush);
    LogD3D12PresentDeviceLostHRESULT(m_IsD3D12, "Present", callCount, hr);
    WaitD3D12FocusLossOverlayFenceAfterPresent("Present", callCount, SyncInterval, presentFlags, hr, flushInfo);
    if (SUCCEEDED(hr)) {
        // A Present hook is the earliest boundary before the game starts the
        // next frame. Waiting here prevents simulation/render work from being
        // queued behind a full vsync present queue.
        WaitFrameLatency();
        g_SharedFpsLimiter.ApplyPostPresent();
    }
    ProbeD3D12FocusLossFrameLatencyAfterPresent("Present", callCount, SyncInterval, presentFlags, hr);
    // Everything the game's next frame has to wait for is behind us: the real
    // Present, the frame-latency wait and any CE-imposed pacing. This is the
    // earliest the next simulation can start, and it is the PC-latency
    // estimate's frame-begin anchor whenever no low-latency runtime provides a
    // later one. See system_latency_frame_begin.h.
    ce::system_latency::NoteFrameBegin(PerfLogger::GetQpcUs(),
                                       ce::system_latency::FrameBeginKind::PresentReturn);
    if (activeDebugSample) {
        activeDebugSample->presentCallUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - presentCallStartUs);
    }
    if (perfLoggingEnabled) {
        perfMetrics.presentCallUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - presentCallStartUs);
    }

    // DXGI_ERROR_WAS_STILL_DRAWING: the previous flip is still pending (GPU throttled).
    // Drop this frame silently — the overlay remains visible from the last rendered frame.
    if (hr == (HRESULT)0x887A000A) {  // DXGI_ERROR_WAS_STILL_DRAWING
        static std::atomic<int> s_dropLog{0};
        if (s_dropLog.fetch_add(1, std::memory_order_relaxed) < 10)
            WrapperLog("Present#%d: WAS_STILL_DRAWING — frame dropped (GPU throttled)", callCount);
        hr = S_OK;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(UINT SyncInterval, UINT PresentFlags,
                                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        IDXGISwapChain1* real = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_ResourceLock);
            real = m_pReal1;
            if (real)
                real->AddRef();
        }
        if (!real)
            return DXGI_ERROR_INVALID_CALL;
        const HRESULT result = real->Present1(SyncInterval, PresentFlags, pPresentParameters);
        real->Release();
        return result;
    }

    // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
    // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG
    // active.  BUT skip heartbeat after device removal so the watchdog can fire.
    if (!DXGIShared::g_SharedState.deviceRemovedFatal.load(std::memory_order_relaxed))
        g_RenderWatchdog.Heartbeat();

    // CRITICAL FIX: Lock mutex to protect swapchain pointer access
    std::lock_guard<std::mutex> lock(m_ResourceLock);

    // CRITICAL FIX: Cache the pointer while holding the mutex
    IDXGISwapChain1* pReal1Cached = m_pReal1;
    if (!pReal1Cached) {
        return DXGI_ERROR_INVALID_CALL;
    }
    static std::atomic<int> s_present1CallCount{0};
    int callCount = s_present1CallCount.fetch_add(1, std::memory_order_relaxed);

    // NVIDIA Smooth Motion compatibility: skip overlay for invisible windows
    if (g_FGCompat.IsNvPresentLoaded() && m_hWnd && !IsWindowVisible(m_hWnd)) {
        return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }

    if (ShouldYieldToVulkanLayer()) {
        static std::atomic<int> s_vulkanYieldLog1{0};
        if (s_vulkanYieldLog1.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present1: Vulkan layer is presenting, bypassing DXGI wrapper path");
        }
        return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }

    const char* delegationOverlayModule = nullptr;
    if (m_IsD3D12 && ShouldDelegateDX12PresentToDetourHook(&delegationOverlayModule, m_StreamlineRuntimeNonRetaining)) {
        static std::atomic<int> s_inlineRouteLogCount1{0};
        if (s_inlineRouteLogCount1.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present1: Delegating DX12 Present1 to detour hook for external overlay %s%s",
                       delegationOverlayModule ? delegationOverlayModule : "module",
                       m_StreamlineRuntimeNonRetaining ? " (Streamline-runtime wrapper passthrough)" : "");
        }
        const bool previousInWrapperPresent = g_InWrapperPresent;
        g_InWrapperPresent = false;
        auto delegateGuard =
            ::ce::make_scope_guard([previousInWrapperPresent]() { g_InWrapperPresent = previousInWrapperPresent; });
        return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }

    g_InWrapperPresent = true;
    auto wrapperPresentGuard = ::ce::make_scope_guard([&] { g_InWrapperPresent = false; });

    if (m_StreamlineRuntimeNonRetaining) {
        DXGIShared::g_PresentCallCounter.fetch_add(1, std::memory_order_relaxed);
    }

    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    static std::atomic<DWORD> s_present1ThreadId{0};
    static std::atomic<int> s_present1Depth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_present1Depth.load() > 0 && s_present1ThreadId.load() == currentId) {
        return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }
    s_present1ThreadId.store(currentId);
    s_present1Depth.fetch_add(1);
    auto depthGuard = ::ce::make_scope_guard([&] {
        if (s_present1Depth.fetch_sub(1) == 1)
            s_present1ThreadId.store(0);
    });
    DXGIShared::BeginPostSLOffKeepAlivePresentScope();
    // Present1 needs the same outer lifetime as Present for exact-proxy dedup.
    auto postSLOffKeepAlivePresentScopeGuard =
        ::ce::make_scope_guard([]() { DXGIShared::EndPostSLOffKeepAlivePresentScope(); });

    // Update performance metrics for FPS calculation
    DXGIShared::GetPerformanceMetrics()->Update(PerfLogger::GetQpcUs());

    // Apply VSync override from config (skip if FG is active - can break frame
    // pacing)
    if (!g_FGCompat.IsFGActive()) {
        ProcessVSyncOverride(SyncInterval, PresentFlags);
    }

    // CRITICAL: Process frame for capture BEFORE calling real Present
    // This must happen regardless of overlay state - capture works independently
    bool dx12PresentContextArmed = false;
    auto dx12PresentContextGuard = ::ce::make_scope_guard([&] {
        if (dx12PresentContextArmed) {
            DX12_ClearWrappedPresentFocusLossContext();
        }
    });
    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present1", callCount, SyncInterval, PresentFlags);
        dx12PresentContextArmed = true;
    }
    if (m_IsD3D12) {
        // Use base interface for ProcessFrameExternal (it takes IDXGISwapChain*)
        DX12_ProcessFrameExternal(pReal1Cached);
        if (m_StreamlineRuntimeNonRetaining) {
            DXGIShared::MaybeInvokePostSLOverlayRenderFromWrappedRuntimePresent(
                pReal1Cached, "Wrapped Streamline runtime Present1");
        }
        // DX12: Overlay rendering is handled by DX12_ProcessFrameExternal above
        // No additional overlay drawing needed here
    } else {
        // DX11/DX10: Call DX11_ProcessFrameExternal for capture AND overlay
        // NOTE: DX11_ProcessFrameExternal already calls DrawDX11Overlay internally,
        // so we do NOT call DrawOverlay() here to avoid double-counting frames
        DX11_ProcessFrameExternal(pReal1Cached);
        if (DX11Hook_ShouldPassThroughCurrentPresent())
            return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }

    // FPS Limiter - arm frame pacing before present. Explicit CE-owned Reflex
    // cadence is finished after Present returns so the wait happens before the
    // game starts building the next frame.
    // This applies to both DX11 and DX12.
    if (g_IPC) {
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply(true);
        DXGIShared::ApplyPresentFrameLatencyOverrides(pReal1Cached);
    }

    // Same SyncInterval=0 override as Present() — tear-free via vblank sync.
    if (g_SharedFpsLimiter.IsActivelyLimiting() && !m_State.isFullscreen) {
        static int s_syncLog1 = 0;
        if (s_syncLog1++ < 30) {
            WrapperLog("Present1: Limiter active, SyncInterval %u->0 (vblank-synced, tear-free)", SyncInterval);
        }
        SyncInterval = 0;
    }

    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present1", callCount, SyncInterval, PresentFlags);
        DX12_WaitForOverlayCompletion(nullptr);
    }

    HRESULT hr = pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    if (m_IsD3D12) {
        const BOOL isIconic = (m_hWnd != nullptr) ? IsIconic(m_hWnd) : FALSE;
        const BOOL hasZeroSize = (m_State.width == 0 || m_State.height == 0) ? TRUE : FALSE;
        DX12_NoteWrappedD3D12PresentResult("Present1", callCount, SyncInterval, PresentFlags, hr,
                                           m_State.isFullscreen ? TRUE : FALSE, isIconic, hasZeroSize, m_hWnd);
    }
    const bool flushProcessHasForeground = m_IsD3D12 ? ResolveCurrentProcessForeground(nullptr, nullptr) : true;
    const bool focusLostForFlush = m_IsD3D12 && !flushProcessHasForeground && !m_State.isFullscreen;
    const auto flushInfo =
        FlushDeferredDX12OverlaySignalAfterWrappedPresent(m_IsD3D12, "Present1", callCount, focusLostForFlush);
    LogD3D12PresentDeviceLostHRESULT(m_IsD3D12, "Present1", callCount, hr);
    WaitD3D12FocusLossOverlayFenceAfterPresent("Present1", callCount, SyncInterval, PresentFlags, hr, flushInfo);
    if (SUCCEEDED(hr)) {
        WaitFrameLatency();
        g_SharedFpsLimiter.ApplyPostPresent();
    }
    ProbeD3D12FocusLossFrameLatencyAfterPresent("Present1", callCount, SyncInterval, PresentFlags, hr);
    ce::system_latency::NoteFrameBegin(PerfLogger::GetQpcUs(),
                                       ce::system_latency::FrameBeginKind::PresentReturn);
    return hr;
}
