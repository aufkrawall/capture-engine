    if (m_IsD3D12) {
        DX12_ProcessFrameExternal(pRealCached);
    } else {
        // DX11/DX10: DX11_ProcessFrameExternal handles both capture AND overlay
        DX11_ProcessFrameExternal(pRealCached);
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

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) {
    // CRITICAL FIX: Use safe accessor to prevent races with DestructionCallback
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    return pReal->GetBuffer(Buffer, riid, ppSurface);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) {
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = pReal->SetFullscreenState(Fullscreen, pTarget);
    if (SUCCEEDED(hr)) {
        // Update cached fullscreen state immediately so ALLOW_TEARING gate in
        // Present/Present1 uses accurate state even before ResizeBuffers is called.
        m_State.isFullscreen = (Fullscreen != FALSE);
        WrapperLog("SetFullscreenState: Fullscreen=%d hr=0x%08X", Fullscreen ? 1 : 0, hr);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) {
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    return pReal->GetFullscreenState(pFullscreen, ppTarget);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) {
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    return pReal->GetDesc(pDesc);
}

static std::atomic<bool> s_ResizeInProgress{false};

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                            DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    WrapperLog("CWrapDXGISwapChain::ResizeBuffers called - Width=%u, Height=%u", Width, Height);
    if (HasBackbufferCountOverride(GetActiveGraphicsConfig().backbufferCount))
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    static std::atomic<DWORD> s_resizeThreadId{0};
    static std::atomic<int> s_resizeDepth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_resizeDepth.load() > 0 && s_resizeThreadId.load() == currentId) {
        return m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    s_resizeThreadId.store(currentId);
    s_resizeDepth.fetch_add(1);

    bool expected = false;
    if (!s_ResizeInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        WrapperLog("ResizeBuffers: already in progress, forwarding to real swapchain");
        HRESULT concurrentHr = S_OK;
        {
            ScopedResizeGuard guard;
            concurrentHr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        if (s_resizeDepth.fetch_sub(1) == 1) {
            s_resizeThreadId.store(0);
        }
        return concurrentHr;
    }

    // Apply backbuffer count override from config
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            // Check swap effect from current swapchain desc
            DXGI_SWAP_CHAIN_DESC scDesc = {};
            bool isFlip = false;
            if (SUCCEEDED(m_pReal->GetDesc(&scDesc))) {
                isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                          scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            }
            UINT gameCount = BufferCount > 0 ? BufferCount : scDesc.BufferCount;
            if (isFlip && requested < gameCount) {
                WrapperLog(
                    "ResizeBuffers: Skipping BufferCount override %u < game's %u "
                    "(flip model)",
                    requested, gameCount);
            } else {
                BufferCount = requested;
                WrapperLog("ResizeBuffers: Overriding BufferCount to %u", BufferCount);
            }
        }
    }

    WrapperLog("ResizeBuffers: calling DX12_OnSwapchainResizeBegin");
    DX12_OnSwapchainResizeBegin();
    WrapperLog("ResizeBuffers: DX12_OnSwapchainResizeBegin returned");

    WrapperLog("ResizeBuffers: calling CleanupOverlayResources");
    CleanupOverlayResources();
    WrapperLog("ResizeBuffers: CleanupOverlayResources returned");

    // CRITICAL FIX: Release DX11 backbuffer RTV before ResizeBuffers.
    // CleanupOverlayResources() only sets a flag; the actual RTV holding a COM
    // reference to the backbuffer must be released or DXGI returns
    // DXGI_ERROR_INVALID_CALL (e.g. Trine 4 vsync toggle).
    if (!m_IsD3D12)
        DXGIShared::HandleDX11ResizeBegin();

    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    WrapperLog("ResizeBuffers: real ResizeBuffers returned hr=0x%08X", hr);

    WrapperLog("ResizeBuffers: calling DX12_OnSwapchainResizeEnd");
    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) {
        m_OverlayResourcesValid = true;
        m_hFrameLatencyWaitable = INVALID_HANDLE_VALUE;
        m_FrameLatencyWaitableQueried = false;
        // Refresh cached state (resolution, format, fullscreen, ALLOW_TEARING)
        DetectSwapChainState();
    }
    s_ResizeInProgress.store(false, std::memory_order_release);
    if (s_resizeDepth.fetch_sub(1) == 1) {
        s_resizeThreadId.store(0);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) {
    return m_pReal->ResizeTarget(pNewTargetParameters);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetContainingOutput(IDXGIOutput** ppOutput) {
    return m_pReal->GetContainingOutput(ppOutput);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) {
    return m_pReal->GetFrameStatistics(pStats);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetLastPresentCount(UINT* pLastPresentCount) {
    return m_pReal->GetLastPresentCount(pLastPresentCount);
}

// ============================================================================
// IDXGISwapChain1 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetDesc1(pDesc);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetFullscreenDesc(pDesc);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetHwnd(HWND* pHwnd) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetHwnd(pHwnd);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCoreWindow(REFIID refiid, void** ppUnk) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(UINT SyncInterval, UINT PresentFlags,
                                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        if (m_pReal1) {
            return m_pReal1->Present1(SyncInterval, PresentFlags, pPresentParameters);
        }
        return DXGI_ERROR_INVALID_CALL;
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
    if (m_IsD3D12 && ShouldDelegateDX12PresentToDetourHook(&delegationOverlayModule)) {
        static std::atomic<int> s_inlineRouteLogCount1{0};
        if (s_inlineRouteLogCount1.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present1: Delegating DX12 Present1 to detour hook for external overlay %s",
                       delegationOverlayModule ? delegationOverlayModule : "module");
        }
        const bool previousInWrapperPresent = g_InWrapperPresent;
        g_InWrapperPresent = false;
        auto delegateGuard =
            ::ce::make_scope_guard([previousInWrapperPresent]() { g_InWrapperPresent = previousInWrapperPresent; });
        return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }

    g_InWrapperPresent = true;
    auto wrapperPresentGuard = ::ce::make_scope_guard([&] { g_InWrapperPresent = false; });

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
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    DXGIShared::GetPerformanceMetrics()->Update(us);

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
        // DX12: Overlay rendering is handled by DX12_ProcessFrameExternal above
        // No additional overlay drawing needed here
    } else {
        // DX11/DX10: Call DX11_ProcessFrameExternal for capture AND overlay
        // NOTE: DX11_ProcessFrameExternal already calls DrawDX11Overlay internally,
        // so we do NOT call DrawOverlay() here to avoid double-counting frames
        DX11_ProcessFrameExternal(pReal1Cached);
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
    return hr;
}

BOOL STDMETHODCALLTYPE CWrapDXGISwapChain::IsTemporaryMonoSupported() {
    if (!m_pReal1)
        return FALSE;
    return m_pReal1->IsTemporaryMonoSupported();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRestrictToOutput(ppRestrictToOutput);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetBackgroundColor(const DXGI_RGBA* pColor) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetBackgroundColor(pColor);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBackgroundColor(DXGI_RGBA* pColor) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetBackgroundColor(pColor);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetRotation(DXGI_MODE_ROTATION Rotation) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetRotation(Rotation);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRotation(DXGI_MODE_ROTATION* pRotation) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRotation(pRotation);
}

// ============================================================================
// IDXGISwapChain2 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetSourceSize(UINT Width, UINT Height) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetSourceSize(Width, Height);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetSourceSize(UINT* pWidth, UINT* pHeight) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetSourceSize(pWidth, pHeight);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMaximumFrameLatency(UINT MaxLatency) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;

    // Apply frame latency override from config
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.frameLatency > 0) {
            MaxLatency = (UINT)gfx.frameLatency;
            WrapperLog("SetMaximumFrameLatency: Overriding to %u", MaxLatency);
        }
    }

    return m_pReal2->SetMaximumFrameLatency(MaxLatency);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMaximumFrameLatency(UINT* pMaxLatency) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMaximumFrameLatency(pMaxLatency);
}
HANDLE STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameLatencyWaitableObject() {
    if (!m_pReal2)
        return nullptr;
    return m_pReal2->GetFrameLatencyWaitableObject();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetMatrixTransform(pMatrix);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMatrixTransform(pMatrix);
}

// ============================================================================
// IDXGISwapChain3 Implementation
// ============================================================================

UINT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCurrentBackBufferIndex() {
    if (!m_pReal3)
        return 0;
    return m_pReal3->GetCurrentBackBufferIndex();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                     UINT* pColorSpaceSupport) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    return DXGIShared::SetSwapChainColorSpaceFromWrapper(m_pReal3, m_pReal, ColorSpace);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                             DXGI_FORMAT Format, UINT SwapChainFlags,
                                                             const UINT* pCreationNodeMask,
                                                             IUnknown* const* ppPresentQueue) {
    if (HasBackbufferCountOverride(GetActiveGraphicsConfig().backbufferCount))
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    static std::atomic<DWORD> s_resize1ThreadId{0};
    static std::atomic<int> s_resize1Depth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_resize1Depth.load() > 0 && s_resize1ThreadId.load() == currentId) {
        return m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }
    s_resize1ThreadId.store(currentId);
    s_resize1Depth.fetch_add(1);

    bool expected = false;
    if (!s_ResizeInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        HRESULT concurrentHr = S_OK;
        {
            ScopedResizeGuard guard;
            concurrentHr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags,
                                                    pCreationNodeMask, ppPresentQueue);
        }
        if (s_resize1Depth.fetch_sub(1) == 1) {
            s_resize1ThreadId.store(0);
        }
        return concurrentHr;
    }

    DX12_OnSwapchainResizeBegin();
    CleanupOverlayResources();
    // CRITICAL FIX: Release DX11 backbuffer RTV before ResizeBuffers (same as above).
    if (!m_IsD3D12)
        DXGIShared::HandleDX11ResizeBegin();

    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);
    }

    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) {
        m_OverlayResourcesValid = true;
        m_hFrameLatencyWaitable = INVALID_HANDLE_VALUE;
        m_FrameLatencyWaitableQueried = false;
        DetectSwapChainState();
    }
    s_ResizeInProgress.store(false, std::memory_order_release);
    if (s_resize1Depth.fetch_sub(1) == 1) {
        s_resize1ThreadId.store(0);
    }
    return hr;
}

// ============================================================================
// IDXGISwapChain4 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) {
    if (!m_pReal4)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->SetHDRMetaData(Type, Size, pMetaData);
}

// ============================================================================
// WrapperStateManager Implementation
// ============================================================================

void WrapperStateManager::RegisterSwapchain(CWrapDXGISwapChain* pWrapper, IDXGISwapChain* pReal) {
    ScopedExclusiveLock lock(m_Lock);  // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == nullptr) {
            m_Wrappers[i] = pWrapper;
            m_RealSwapchains[i] = pReal;
            return;
        }
    }
}

void WrapperStateManager::UnregisterSwapchain(CWrapDXGISwapChain* pWrapper) {
    ScopedExclusiveLock lock(m_Lock);  // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == pWrapper) {
            m_Wrappers[i] = nullptr;
            m_RealSwapchains[i] = nullptr;
            return;
        }
    }
}

CWrapDXGISwapChain* WrapperStateManager::FindWrapper(IDXGISwapChain* pReal) {
    ScopedSharedLock lock(m_Lock);  // Shared lock for reading (Concurrent Access)
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_RealSwapchains[i] == pReal || m_Wrappers[i] == (CWrapDXGISwapChain*)pReal) {
            return m_Wrappers[i];
        }
