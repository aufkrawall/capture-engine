#include "dxgi_swapchain_wrap_internal.h"
#include "hook/present/translated_swapchain_owner.h"
#include "hook/present/dxgi_shared_detail/present_pacing_scope.h"
#include "hook/present/present_stall_attribution.h"
#include "hook/runtime/hook_clock.h"
#include "hook/metrics/present_phase_clock.h"

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
    // A present interposer (NVIDIA Smooth Motion) implements the application-facing Present in
    // its own module and presents its private chain to dxgi. CE's deep body hook then only ever
    // sees that private chain, so this wrapper is CE's ONLY view of the game's presents and must
    // never delegate them to the detour.
    // Only a Present deep view can be "the interposer's chain, not this one"; a lone Present1 deep
    // hook means CE has no Present detour at all, which the per-method delegation gate handles.
    m_PresentInvisibleToDetourHook = pReal != nullptr && DXGIShared::GetPresentMethodViews().presentDeepBody &&
                                     !DXGIShared::IsSwapchainPresentCoveredByDeepBodyHook(pReal);
    if (m_PresentInvisibleToDetourHook) {
        WrapperLog(
            "SwapChain: Present for real=%p is implemented above dxgi (present interposer) — this wrapper is CE's "
            "only Present view and composites the overlay itself",
            pReal);
    }
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
        if (!m_pReal1 && SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
            if (m_StreamlineRuntimeNonRetaining && m_pReal1) {
                // Borrowed interface: the transferred CreateSwapChain reference keeps the
                // object alive for the wrapper lifetime; never pin it with extra refs.
                m_pReal1->Release();
            }
            m_Version = 1;
        }
        if (!m_pReal2 && SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) {
            if (m_StreamlineRuntimeNonRetaining && m_pReal2) {
                m_pReal2->Release();
            }
            m_Version = 2;
        }
        if (!m_pReal3 && SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
            if (m_StreamlineRuntimeNonRetaining && m_pReal3) {
                m_pReal3->Release();
            }
            m_Version = 3;
        }
        if (!m_pReal4 && SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
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
    IDXGISwapChain* pRealCached = nullptr;
    const bool perfLoggingEnabled = !m_IsD3D12 && PerfLogger::Get().IsEnabled();
    // A D3D12 frame's CSV row is ProcessFrame's; this present only owes it the detailed debug sample, picked
    // before the acquire so the sample times the acquire too. A pass-through below takes its tick without
    // committing a sample; a status probe presents nothing and takes none.
    uint64_t debugFrameNum = 0;
    if (m_IsD3D12 && (Flags & DXGI_PRESENT_TEST) == 0 && PerfLogger::Get().IsEnabled()) {
        static std::atomic<uint64_t> s_presentDebugFrame{0};
        debugFrameNum = s_presentDebugFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    const bool sampleThisFrame = debugFrameNum != 0 && PerfLogger::Get().ShouldSampleDetailedFrame(debugFrameNum);
    // The phases below are read by the DX11 CSV row or by this frame's debug sample, by nothing else.
    const ce::PresentPhaseClock phaseClock(perfLoggingEnabled || sampleThisFrame);
    const int64_t acquireStartUs = phaseClock.Now();
    {
        std::lock_guard<std::mutex> lock(m_ResourceLock);
        pRealCached = m_pReal;
        m_pRealCached = pRealCached;
        if (pRealCached) pRealCached->AddRef();
    }
    auto realSwapchainGuard = ::ce::make_scope_guard([&] { if (pRealCached) pRealCached->Release(); });
    const int32_t acquireUs = phaseClock.ElapsedUs(acquireStartUs);
    DXGIShared::detail::PresentPacingScope pacing(pRealCached, (Flags & DXGI_PRESENT_TEST) == 0);
    if ((Flags & DXGI_PRESENT_TEST) || HookIsShuttingDown() || ce::translated_swapchain::HasNativeOwner(pRealCached)) {
        pacing.Suppress();
        return pRealCached ? pRealCached->Present(SyncInterval, Flags) : DXGI_ERROR_INVALID_CALL;
    }
    PresentDebugSample debugSample = {};
    PresentDebugSample* activeDebugSample = nullptr;
    int64_t presentStartUs = 0;
    // The present's DX11/DX10 perf CSV row (a D3D12 frame's row is ProcessFrame's), logged on every exit.
    DXGIShared::DX11PresentPerfRow perfRow(perfLoggingEnabled,
                                           perfLoggingEnabled ? DetectWrappedSwapchainApi(m_pDevice, m_IsD3D12) : "",
                                           "CWrapDXGISwapChain::Present");
    if (sampleThisFrame) {
        activeDebugSample = &debugSample;
        activeDebugSample->frameNum = debugFrameNum;
        strncpy(activeDebugSample->api, "DX12", sizeof(activeDebugSample->api) - 1);
        activeDebugSample->api[sizeof(activeDebugSample->api) - 1] = '\0';
        presentStartUs = PerfLogger::GetQpcUs();
        PerfLogger::Get().ActivateDebugSample(activeDebugSample);
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
        []() {
            DXGIShared::g_SharedState.lastPresentReturnTickMs.store(ce::hook_clock::TickCount64(),
                                                                    std::memory_order_release);
            DXGIShared::g_SharedState.presentInFlightDepth.fetch_sub(1, std::memory_order_acq_rel);
        });

    // EXTREME DEBUG: Log entry with full state
    DWORD threadId = GetCurrentThreadId();
    static std::atomic<int> s_presentCallCount{0};
    int callCount = s_presentCallCount.fetch_add(1);

    SharedMemoryLayout* debugSharedMem = GetHookSharedMemory();
    if (activeDebugSample && debugSharedMem) {
        activeDebugSample->sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        activeDebugSample->capturePhase = debugSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        activeDebugSample->encoderQueueDepth = debugSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        activeDebugSample->muxQueueKb =
            (debugSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        activeDebugSample->overloadFlags =
            debugSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }

    if (activeDebugSample)
        activeDebugSample->swapchainAcquireUs = acquireUs;

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

    // The application's own present stream, which is what the interposer's output chain is measured
    // against. Counted before the delegation decision, because the delegated path is the normal one
    // under an interposer and it returns without reaching the draw below.
    if (m_PresentInvisibleToDetourHook) {
        DXGIShared::NoteApplicationPresentUnderPresentInterposer();
    }

    const char* delegationOverlayModule = nullptr;
    if (m_IsD3D12 && ShouldDelegateDX12PresentToDetourHook(&delegationOverlayModule, m_StreamlineRuntimeNonRetaining,
                                                          m_PresentInvisibleToDetourHook)) {
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

    // In leave-entry mode the Streamline-runtime wrapper is CE's only present entry point
    // (DetourPresent never runs), so feed the Streamline present-stall detector here to
    // prevent false present stall detections in slDLSSGSetOptions.
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
    const int64_t metricsUpdateStartUs = phaseClock.Now();
    DXGIShared::GetPerformanceMetrics()->Update(PerfLogger::GetQpcUs());
    if (activeDebugSample) {
        activeDebugSample->metricsUpdateUs = phaseClock.ElapsedUs(metricsUpdateStartUs);
    }

    // Apply VSync override from config (skip if FG is active - can break frame
    // pacing)
    if (!fgActive) {
        DXGIShared::ProcessPresentVSyncOverride(SyncInterval, Flags, pRealCached);
    } else if (callCount < 20) {
        WrapperLog("Present: Skipping VSync override because FG is active");
    }

    // Process frame for capture BEFORE calling real Present
    const int64_t processFrameStartUs = phaseClock.Now();
    bool dx12PresentContextArmed = false;
    auto dx12PresentContextGuard = ::ce::make_scope_guard([&] {
        if (dx12PresentContextArmed) {
            DX12_ClearWrappedPresentFocusLossContext();
        }
    });
    // One overlay-coverage scope per physical present: the wrapper's ProcessFrameExternal and the DetourPresent
    // the real Present re-enters below judge the same present (a second scope there read as unaccounted).
    bool overlayPresentScopeOpen = false;
    auto overlayPresentScopeGuard = ::ce::make_scope_guard([&] {
        if (overlayPresentScopeOpen) {
            DX12_EndOverlayPresentScope();
        }
    });
    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present", callCount, SyncInterval, Flags);
        dx12PresentContextArmed = true;
        DX12_BeginOverlayPresentScope(pRealCached);
        overlayPresentScopeOpen = true;
    }

    // ProcessFrameExternal captures before the limiter stage's pre-present wait; see ScopedPreLimiterCapture.
    ce::present_release_forecast::ScopedPreLimiterCapture preLimiterCapture;
    if (m_IsD3D12) {
        DX12_ProcessFrameExternal(pRealCached);
        if (m_StreamlineRuntimeNonRetaining) {
            DXGIShared::MaybeInvokePostSLOverlayRenderFromWrappedRuntimePresent(
                pRealCached, "Wrapped Streamline runtime Present");
        }
    } else {
        // DX11/DX10: DX11_ProcessFrameExternal handles both capture AND overlay
        DX11_ProcessFrameExternal(pRealCached);
        if (DX11Hook_ShouldPassThroughCurrentPresent()) {
            preLimiterCapture.Close();
            return pRealCached->Present(SyncInterval, Flags);
        }
        // This row is the present's only one: the DXGI Present detour the real Present re-enters logs none.
        perfRow.TakeFramePhases();
    }
    if (activeDebugSample) {
        activeDebugSample->processFrameExternalUs = phaseClock.ElapsedUs(processFrameStartUs);
    }

    // FPS Limiter - arm frame pacing before present. Explicit CE-owned Reflex
    // cadence is finished after Present returns so the wait happens before the
    // game starts building the next frame.
    // This applies to both DX11 and DX12.
    //
    // Unique application-present boundary: the DXGI detours take their
    // IsInWrapperPresent() early return while this wrapper owns the present,
    // so exactly one Apply() runs per presented frame and the cadence grid
    // gates every entry instead of a duplicate-present time window.
    const int64_t limiterStartUs = phaseClock.Now();
    DXGIShared::NoteApplicationPresentPacedOnSwapchain(pRealCached, "CE's swapchain wrapper");
    DXGIShared::ApplyFpsLimiterStage(pRealCached, true,
                                     ce::fps_limiter_policy::PresentSite::kUniqueApplicationPresent);
    preLimiterCapture.Close();

    if (activeDebugSample) {
        activeDebugSample->fpsLimiterUs = phaseClock.ElapsedUs(limiterStartUs);
    }
    perfRow.NoteFpsLimitWaitSince(limiterStartUs);

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
        HWND foreground = HookForegroundWindow();
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

    const int64_t presentCallStartUs = phaseClock.Now();
    // The down-call alone; what CE does after it (frame-latency wait, limiter) is post_present_us.
    ce::present_stall::DownCallProbe stallProbe(m_IsD3D12);
    perfRow.BeginPresentCall();
    HRESULT hr = pRealCached->Present(SyncInterval, presentFlags);
    perfRow.EndPresentCall();
    stallProbe.Finish();
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
    DXGIShared::NotePresentDeviceResult(hr, "wrapped Present");
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
        activeDebugSample->presentCallUs = phaseClock.ElapsedUs(presentCallStartUs);
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
    IDXGISwapChain1* pReal1Cached = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_ResourceLock);
        pReal1Cached = m_pReal1;
        if (pReal1Cached) pReal1Cached->AddRef();
    }
    auto realSwapchainGuard = ::ce::make_scope_guard([&] { if (pReal1Cached) pReal1Cached->Release(); });
    DXGIShared::detail::PresentPacingScope pacing(pReal1Cached, (PresentFlags & DXGI_PRESENT_TEST) == 0);
    if ((PresentFlags & DXGI_PRESENT_TEST) || HookIsShuttingDown() || ce::translated_swapchain::HasNativeOwner(pReal1Cached)) {
        pacing.Suppress();
        return pReal1Cached ? pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters) : DXGI_ERROR_INVALID_CALL;
    }
    // Same row as Present's: until 2026-10-17 a Present1 game's DX11 presents wrote none.
    const bool perfLoggingEnabled = !m_IsD3D12 && PerfLogger::Get().IsEnabled();
    DXGIShared::DX11PresentPerfRow perfRow(perfLoggingEnabled,
                                           perfLoggingEnabled ? DetectWrappedSwapchainApi(m_pDevice, m_IsD3D12) : "",
                                           "CWrapDXGISwapChain::Present1");

    // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
    // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG
    // active.  BUT skip heartbeat after device removal so the watchdog can fire.
    if (!DXGIShared::g_SharedState.deviceRemovedFatal.load(std::memory_order_relaxed))
        g_RenderWatchdog.Heartbeat();

    // CRITICAL FIX: Lock mutex to protect swapchain pointer access
    std::lock_guard<std::mutex> lock(m_ResourceLock);

    // CRITICAL FIX: Cache the pointer while holding the mutex
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

    if (m_PresentInvisibleToDetourHook) {
        DXGIShared::NoteApplicationPresentUnderPresentInterposer();
    }

    const char* delegationOverlayModule = nullptr;
    if (m_IsD3D12 && ShouldDelegateDX12PresentToDetourHook(&delegationOverlayModule, m_StreamlineRuntimeNonRetaining,
                                                          m_PresentInvisibleToDetourHook,
                                                          /*delegatedMethodIsPresent1=*/true)) {
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
        DXGIShared::ProcessPresentVSyncOverride(SyncInterval, PresentFlags, pReal1Cached);
    }

    // CRITICAL: Process frame for capture BEFORE calling real Present
    // This must happen regardless of overlay state - capture works independently
    bool dx12PresentContextArmed = false;
    auto dx12PresentContextGuard = ::ce::make_scope_guard([&] {
        if (dx12PresentContextArmed) {
            DX12_ClearWrappedPresentFocusLossContext();
        }
    });
    // One overlay-coverage scope per physical present: the wrapper's ProcessFrameExternal and the DetourPresent
    // the real Present1 re-enters below judge the same present (a second scope there read as unaccounted).
    bool overlayPresentScopeOpen = false;
    auto overlayPresentScopeGuard = ::ce::make_scope_guard([&] {
        if (overlayPresentScopeOpen) {
            DX12_EndOverlayPresentScope();
        }
    });
    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present1", callCount, SyncInterval, PresentFlags);
        dx12PresentContextArmed = true;
        DX12_BeginOverlayPresentScope(pReal1Cached);
        overlayPresentScopeOpen = true;
    }
    // ProcessFrameExternal captures before the limiter stage's pre-present wait; see ScopedPreLimiterCapture.
    ce::present_release_forecast::ScopedPreLimiterCapture preLimiterCapture;
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
        if (DX11Hook_ShouldPassThroughCurrentPresent()) {
            preLimiterCapture.Close();
            return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
        }
        perfRow.TakeFramePhases();
    }

    // FPS Limiter - arm frame pacing before present. Explicit CE-owned Reflex
    // cadence is finished after Present returns so the wait happens before the
    // game starts building the next frame.
    // This applies to both DX11 and DX12. Unique application-present boundary
    // for the same reason as Present().
    const int64_t limiterStartUs = perfRow.NowUs();
    DXGIShared::NoteApplicationPresentPacedOnSwapchain(pReal1Cached, "CE's swapchain wrapper");
    DXGIShared::ApplyFpsLimiterStage(pReal1Cached, true,
                                     ce::fps_limiter_policy::PresentSite::kUniqueApplicationPresent);
    preLimiterCapture.Close();
    perfRow.NoteFpsLimitWaitSince(limiterStartUs);

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

    ce::present_stall::DownCallProbe stallProbe(m_IsD3D12);
    perfRow.BeginPresentCall();
    HRESULT hr = pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    perfRow.EndPresentCall();
    stallProbe.Finish();
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
    DXGIShared::NotePresentDeviceResult(hr, "wrapped Present1");
    WaitD3D12FocusLossOverlayFenceAfterPresent("Present1", callCount, SyncInterval, PresentFlags, hr, flushInfo);
    if (SUCCEEDED(hr)) {
        WaitFrameLatency();
        g_SharedFpsLimiter.ApplyPostPresent();
    }
    ProbeD3D12FocusLossFrameLatencyAfterPresent("Present1", callCount, SyncInterval, PresentFlags, hr);
    return hr;
}
