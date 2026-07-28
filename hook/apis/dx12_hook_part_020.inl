    }

    DXGIShared::g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();

    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

    // CRITICAL: Flush GPU before releasing resources.  In-flight overlay
    // commands still reference backbuffers; ResizeBuffers returns
    // E_ACCESSDENIED if any GPU references remain.
    CleanupOverlay();  // waits on fence, releases sync resources
    CleanupRTVs();
    g_State.overlayInit = false;
    if (g_OverlayAdapter.IsInitialized()) {
        g_PreserveOverlayAdapterAcrossResize.store(true, std::memory_order_release);
        HookLogImportant(
            "DX12: Preserving warm overlay backend across swapchain resize; only backbuffer/sync resources were "
            "released (device=%p queue=%p fmt=%d)",
            g_OverlayAdapterBackendDevice.load(std::memory_order_acquire),
            g_OverlayAdapterBackendQueue.load(std::memory_order_acquire),
            g_OverlayAdapterBackendFormat.load(std::memory_order_acquire));
    } else {
        g_PreserveOverlayAdapterAcrossResize.store(false, std::memory_order_release);
    }

    // g_LastSwapChain is stored as a raw (non-AddRef'd) pointer to avoid
    // interfering with FSR FG's reference count management.  Do NOT Release it.
    g_PendingSwapChainCleanup = nullptr;
    g_LastSwapChain = nullptr;
    HookLog("DX12: DX12_OnSwapchainResizeBegin - complete (GPU flushed)");
}

void DX12_OnSwapchainResizeEnd() {
    HookLog("DX12: DX12_OnSwapchainResizeEnd called");
    // Only clear if it was set - prevents unbalanced calls from clearing
    // prematurely
    if (g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        g_InSwapchainResizeCleanup.store(false, std::memory_order_release);
    }
    // g_PendingSwapChainCleanup is no longer used (swapchain stored without
    // AddRef), so nothing to release here.
    if (g_PendingSwapChainCleanup) {
        g_PendingSwapChainCleanup = nullptr;
    }
}

// --- CPU Prerender Limit Support (DX12) ---
static void ApplyPrerenderLimitDX12(float limit) {
    if (limit < 0.0f)
        return;
    // CRITICAL FIX: Use thread-safe accessor to prevent race conditions
    DX12Context ctx = GetDX12Context();
    if (!ctx.IsValid())
        return;

    std::lock_guard<std::mutex> lock(g_PrerenderMutex);

    if (g_PrerenderDevice != ctx.device || g_PrerenderQueue != ctx.queue) {
        for (auto* fence : g_PrerenderFences) {
            if (fence)
                fence->Release();
        }
        g_PrerenderFences.clear();
        for (HANDLE event : g_PrerenderEvents) {
            if (event)
                CloseHandle(event);
        }
        g_PrerenderEvents.clear();
        g_PrerenderFrameIndex = 0;
        if (g_PrerenderDevice)
            g_PrerenderDevice->Release();
        if (g_PrerenderQueue)
            g_PrerenderQueue->Release();
        g_PrerenderDevice = ctx.device;
        g_PrerenderQueue = ctx.queue;
        g_PrerenderDevice->AddRef();
        g_PrerenderQueue->AddRef();
        HookLogImportant("DX12: Prerender fence stream rebound device=%p queue=%p", ctx.device, ctx.queue);
    }

    // Initialize fence ring buffer if needed
    if (g_PrerenderFences.empty()) {
        for (int i = 0; i < 16; i++) {
            ID3D12Fence* fence = nullptr;
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (SUCCEEDED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                g_PrerenderFences.push_back(fence);
                g_PrerenderEvents.push_back(event);
            } else if (event) {
                CloseHandle(event);
            }
        }
        HookLog("DX12: Created prerender limit fence ring buffer (size: %d)", (int)g_PrerenderFences.size());
    }

    if (g_PrerenderFences.empty())
        return;

    static std::atomic<int> s_prerenderWarnLogs{0};
    auto waitForFence = [&](ID3D12Fence* fenceToWait, HANDLE waitEvent, uint64_t waitValue) -> bool {
        if (!fenceToWait || !waitEvent)
            return false;
        if (fenceToWait->GetCompletedValue() >= waitValue)
            return true;

        HRESULT setHr = fenceToWait->SetEventOnCompletion(waitValue, waitEvent);
        if (FAILED(setHr)) {
            if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Prerender SetEventOnCompletion failed hr=0x%08X value=%llu", setHr, waitValue);
            }
            return false;
        }

        DWORD waitResult = WaitForSingleObject(waitEvent, INFINITE);
        if (waitResult == WAIT_OBJECT_0)
            return true;

        if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender wait failed result=%lu error=%lu value=%llu", waitResult, GetLastError(),
                    waitValue);
        }
        return false;
    };

    size_t idx = g_PrerenderFrameIndex % g_PrerenderFences.size();
    ID3D12Fence* fence = g_PrerenderFences[idx];
    HANDLE event = g_PrerenderEvents[idx];

    if (limit == 0.0f) {
        // Strict Serial: Signal and immediately wait
        uint64_t value = g_PrerenderFrameIndex + 1;
        HRESULT signalHr = ctx.queue->Signal(fence, value);
        if (SUCCEEDED(signalHr)) {
            waitForFence(fence, event, value);
        } else if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, value);
        }
    } else {
        const int lookback = std::clamp(static_cast<int>(limit), 1, 6);

        // Signal current frame
        uint64_t signalValue = g_PrerenderFrameIndex + 1;
        HRESULT signalHr = ctx.queue->Signal(fence, signalValue);
        if (FAILED(signalHr)) {
            if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, signalValue);
            }
            g_PrerenderFrameIndex++;
            return;
        }

        // Wait on N frames ago
        if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
            size_t waitIdx = (g_PrerenderFrameIndex - lookback) % g_PrerenderFences.size();
            ID3D12Fence* waitFence = g_PrerenderFences[waitIdx];
            HANDLE waitEvent = g_PrerenderEvents[waitIdx];
            uint64_t waitValue = (g_PrerenderFrameIndex - lookback) + 1;

            if (waitFence->GetCompletedValue() < waitValue) {
                waitForFence(waitFence, waitEvent, waitValue);
            }
        }
    }

    g_PrerenderFrameIndex++;
}

// ============================================================================
// Post-SL FG overlay renderer.
//
// Called from the RE-ENTRANT Present path (dxgi_shared.cpp) — i.e. AFTER
// Streamline's FG pipeline has finished generating/presenting its frames.
// By rendering here we avoid submitting extra ECLs before SL sees Present,
// which is what caused DXGI_ERROR_DEVICE_REMOVED with every previous approach.
//
// Flow:
//   1. Game calls Present → our DetourPresent → ProcessFrame (skips overlay
//      draw because SL FG is active) → calls oPresent (enters SL via E9 JMP)
//   2. SL processes FG → for each output frame SL calls Present via vtable
//   3. Re-entrant DetourPresent → g_PostSLOverlayRenderCallback → THIS function
//   4. We render overlay on the current backbuffer → bypass trampoline → real DXGI Present
//
// This matches the standard inject-overlay strategy: overlay is drawn after FG, before the real Present.
//
// KEY DESIGN DECISIONS (confirmed by diagnostics):
//
// 1. DIRECT QUEUE SUBMISSION: We submit ECL via g_RealD3D12ECL(g_RealQueueBehindSLWrapper)
//    instead of slQueue->ExecuteCommandLists().  SL's COM wrapper adds internal
//    metadata per ECL that accumulates and causes DEVICE_REMOVED after ~500-2000
//    frames.  Direct submission bypasses this — proven stable 16,798+ frames.
//
// 2. UAV BARRIERS (not state transitions): We use UAV barriers (global GPU flush)
//    instead of PRESENT→RT / RT→PRESENT state transitions.  State transition type
//    doesn't affect the cumulative crash (confirmed: all barrier types crash at
//    similar timing through SL's wrapper).  UAV barriers avoid resource state
//    tracking conflicts with SL's internal state management.
//
// 3. CACHED FG STATE: g_StreamlineFGRunning is cached ONCE at function entry
//    into cachedSLFGActive.  Reading it multiple times caused mid-function
//    transition races where barrier/queue selection became inconsistent →
//    instant DEVICE_REMOVED on the first inconsistent frame.
//
// 4. FG DEACTIVATION SUSPEND: When cachedSLFGActive transitions true→false,
//    PostSL suspends permanently (s_postSLFGSuspended=true) until FG reactivates.
//    This prevents using stale queue/state from the FG phase.  Pre-SL path
//    takes over for non-FG rendering.
//
// 5. FG "SUSPENSION" FALLBACK: When g_StreamlineFGRunning stays true but SL stops
//    generating re-entrant Present calls (game menu/pause), PostSL never fires.
//    ProcessFrame detects this via g_PostSLStallCounter and falls back to pre-SL.
//    When PostSL fires again (FG resumes), it resets the counter and takes over.
//
// COMPATIBILITY:
//   - GTA V Enhanced: DLSS FG with SL, menu pauses FG (stall fallback needed)
//   - Talos Principle Reawakened: DLSS FG + FSR FG, continuous rendering
//   - Both need the direct queue bypass to avoid cumulative SL damage
// ============================================================================
static void PostSLOverlayRender(IDXGISwapChain* pSwapChain) {
    // --- PostSL per-frame statistics (declared early for lock-skip path) ---
    static std::atomic<int> s_postSLCalls{0};
    static std::atomic<int> s_postSLRenders{0};
    static std::atomic<int> s_postSLSkipLock{0};
    static std::atomic<int> s_postSLSkipFence{0};
    static std::atomic<int> s_postSLSkipOther{0};

    // Snapshot before this callback records anything. If the normal path has
    // already drawn since the last presented-frame accounting boundary, the
    // current present is covered and the same-queue startup handoff must not
    // draw a second overlay on top of it.
    const bool normalRouteDrawPendingAtEntry = g_OverlayCoverageDrawCount.load(std::memory_order_acquire) !=
                                               g_OverlayCoverageLastSeenDrawCount.load(std::memory_order_acquire);

    // THREAD SAFETY: During FG, SL may fire Present from multiple threads.
    // Our rendering resources (allocators, command list, descriptor heap) are NOT
    // thread-safe. Use a try-lock to ensure only one thread renders at a time.
    if (!g_PostSLRenderMutex.try_lock()) {
        s_postSLSkipLock.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-render-lock");
        static int s_lockSkip = 0;
        if (s_lockSkip++ < 10)
            HookLogImportant("DX12: PostSL SKIP — another thread already rendering (tid=0x%04X)", GetCurrentThreadId());
        return;
    }
    // RAII unlock — ensures s_renderLock is released on ALL exit paths
    auto renderLockGuard = ce::make_scope_guard([]() { g_PostSLRenderMutex.unlock(); });
    const uint32_t entryLifecycleEpoch = g_PostSLLifecycleEpoch.load(std::memory_order_acquire);

    // Cache FG state ONCE at function entry to avoid mid-function transition races.
    // g_StreamlineFGRunning can change between reads if FG transitions during PostSL.
    const bool cachedSLFGActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    constexpr ULONGLONG kDormantProcessFrameThresholdMs = 100;
    const ULONGLONG nowMs = GetTickCount64();
    const ULONGLONG lastProcessFrameTickMs = g_LastProcessFrameTickMs.load(std::memory_order_acquire);
    const bool processFrameRecentlySeen = lastProcessFrameTickMs != 0 && nowMs >= lastProcessFrameTickMs &&
                                          (nowMs - lastProcessFrameTickMs) < kDormantProcessFrameThresholdMs;
    const bool startupTopLevelPresentConsumed =
        DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    const int startupWrapperProgressCount =
        g_PostSLSyntheticStartupWrapperProgressCount.load(std::memory_order_acquire);
    const bool useTopLevelHandoffWrapperProgress =
        ce::dx12_overlay_policy::ShouldUseTopLevelHandoffWrapperProgressForSyntheticPostSLActivation(
            g_HadFSRFGPhase, startupTopLevelPresentConsumed, startupWrapperProgressCount > 0);
    const bool safePostFSRBootstrapPathForPostSL = HookHasSafePostFSRBootstrapPath();
    // Pure-DLSS engage proof: explicit slDLSSGSetOptions(ON) provenance for the
    // CURRENT comeback + retained startup activation swapchain + installed
    // callback (we are inside one — SL's present pipeline is live). Gates the
    // no-blank engage path for the synthetic-startup countdown and cold-start
    // warmup; GetState-only enables keep both protections.
    const bool explicitEnablePureDLSSColdStartProof = ce::dx12_overlay_policy::HasExplicitEnablePureDLSSColdStartProof(
        g_HadFSRFGPhase, HookHasExplicitStreamlineSetOptionsActivation(),
        HasRetainedStreamlineStartupActivationSwapchain(),
        DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr);

    // --- PostSL periodic stats logging ---
    int callNum = s_postSLCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((callNum % 500) == 0) {
        int renders = s_postSLRenders.load(std::memory_order_relaxed);
        int skipL = s_postSLSkipLock.load(std::memory_order_relaxed);
        int skipF = s_postSLSkipFence.load(std::memory_order_relaxed);
        int skipO = s_postSLSkipOther.load(std::memory_order_relaxed);
        HookLogImportant(
            "DX12: PostSL stats: calls=%d renders=%d skipLock=%d skipFence=%d skipOther=%d (render%%=%.0f%%)", callNum,
            renders, skipL, skipF, skipO, callNum > 0 ? (renders * 100.0 / callNum) : 0.0);
    }

    // Reactivation tracking: log the first N calls after reactivation to diagnose
    // silent early returns.  All early-return paths use HookLog (not in hook_debug.log),
    // so without this, PostSL failures after FG transitions are invisible.
    static int s_reactivationEpoch = 0;
    static int s_callsSinceReactivation = 0;
    static int s_postSLProbeFrames = 0;
    static bool s_wasActive = false;
    static uint32_t s_seenLifecycleEpoch = 0;
    static HANDLE s_dedicatedFenceEvent = nullptr;
    static ID3D12Fence* s_dedicatedSyncFence = nullptr;
    static UINT64 s_dedicatedSyncFenceValue = 0;

    // Streamline signal guard: a real FG shutdown must stop PostSL immediately,
    // but a transient signal drop during reactivation must not permanently strand
    // PostSL in a locally suspended state while synthetic re-entrant Presents are
    // still arriving.
    static bool s_wasSLFGActive = false;
    static bool s_postSLFGSuspended = false;
    const bool postSLActive = g_PostSLOverlayActive.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    if (cachedSLFGActive) {
        s_wasSLFGActive = true;
        s_postSLFGSuspended = false;
    } else if (s_wasSLFGActive) {
        s_wasSLFGActive = false;
        s_postSLFGSuspended = ce::dx12_overlay_policy::ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(
            cachedSLFGActive, postSLActive, postSLConfirmedRendering, startupActivationPending);
        HookLogImportant("DX12: PostSL FG signal dropped — %s (active=%d confirmed=%d startupPending=%d)",
                         s_postSLFGSuspended ? "suspending until clean reactivation"
                                             : "treating as transient and waiting for signal recovery",
                         postSLActive ? 1 : 0, postSLConfirmedRendering ? 1 : 0, startupActivationPending ? 1 : 0);
    }
    // Make-before-break keep-alive: a confirmed PostSL path renders across the
    // explicit OFF edge until the normal route recovers (gates below honor it).
    // It renders exactly what it rendered one present earlier on the same
    // proven queue/swapchain; PostSLOverlayRenderGated retires the latch on
    // normal-route recovery or Streamline unload.
    const bool keepAliveRenderAfterExplicitOff =
        ce::dx12_overlay_policy::ShouldAllowPostSLKeepAliveRenderAfterExplicitOff(
            g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire), cachedSLFGActive, IsStreamlineLoaded());
    const bool exactExplicitOffKeepAliveSwapchain =
        keepAliveRenderAfterExplicitOff && pSwapChain != nullptr &&
        pSwapChain == g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
    if ((!cachedSLFGActive || s_postSLFGSuspended) && !keepAliveRenderAfterExplicitOff) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-sl-signal-inactive");
        static int s_suspendLog = 0;
        if (s_suspendLog < 5 || (s_suspendLog % 500 == 0)) {
            HookLog("DX12: PostSL SKIP — Streamline FG signal inactive (latched=%d frame=%d)",
                    s_postSLFGSuspended ? 1 : 0, s_suspendLog);
        }
        s_suspendLog++;
        return;
    }
    if (keepAliveRenderAfterExplicitOff) {
        static std::atomic<int> s_keepAliveRenderLogCount{0};
        const int logCount = s_keepAliveRenderLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: PostSL keep-alive render after explicit Streamline OFF #%d (confirmed=%d active=%d)",
                logCount + 1, g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
                g_PostSLOverlayActive.load(std::memory_order_relaxed) ? 1 : 0);
        }
    }

    // Same-queue pure-DLSS cold start proof (Talos startup: scQueue==origGame, no separate command/SL
    // wrapper queue). When DLSS FG runs on the game's own single queue there is no separate DLSS-G
    // proxy-init pipeline for CE's overlay ECL to corrupt, so the synthetic-startup countdown and the
    // cold-start warmup (the GTA separate-queue init protection) can be bypassed safely — the overlay
    // ECL is the same no-FG-route submit on the game's queue. Re-evaluated every callback so a title
    // that later creates a separate runtime queue (GTA) flips this false and the protections resume.
    bool sameQueuePureDLSSColdStartSafe = false;
    {
        ID3D12CommandQueue* sqScQueue = nullptr;
        ID3D12CommandQueue* sqOrigQueue = nullptr;
        ID3D12CommandQueue* sqCmdQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            sqScQueue = g_SwapchainQueue;
            sqOrigQueue = g_OriginalGameQueue;
            sqCmdQueue = g_CommandQueue.load(std::memory_order_acquire);
        }
        ID3D12CommandQueue* sqSLWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);
        auto* sqDev = g_Device.load(std::memory_order_acquire);
        const bool sqDeviceRemoved = sqDev && FAILED(sqDev->GetDeviceRemovedReason());
        sameQueuePureDLSSColdStartSafe = ce::dx12_overlay_policy::ShouldTreatSameQueuePureDLSSColdStartAsSafe(
            g_HadFSRFGPhase, sqScQueue != nullptr && sqScQueue == sqOrigQueue,
            sqCmdQueue == nullptr || sqCmdQueue == sqOrigQueue, sqSLWrapperQueue != nullptr, sqDeviceRemoved);
    }

    bool syntheticStartupActivatedThisCall = false;
    bool immediateSameQueueStartupTakeover = false;
    {
        immediateSameQueueStartupTakeover =
            sameQueuePureDLSSColdStartSafe && processFrameRecentlySeen && startupActivationPending;
        if (ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire),
                cachedSLFGActive, g_PostSLOverlayActive.load(std::memory_order_acquire), processFrameRecentlySeen,
                useTopLevelHandoffWrapperProgress, sameQueuePureDLSSColdStartSafe)) {
            if (!g_PostSLSyntheticStartupTakeoverLogged.exchange(true, std::memory_order_acq_rel)) {
                if (immediateSameQueueStartupTakeover) {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup immediate same-queue takeover — callback proves the "
                        "Streamline handoff before the ProcessFrame dormant timer (normalDrawPending=%d "
                        "cooldown=%d)",
                        normalRouteDrawPendingAtEntry ? 1 : 0,
                        g_PostSLCooldownRemaining.load(std::memory_order_relaxed));
                } else {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup takeover — ProcessFrame dormant for %llums (cooldown=%d)",
                        lastProcessFrameTickMs != 0 && nowMs >= lastProcessFrameTickMs
                            ? (nowMs - lastProcessFrameTickMs)
                            : 0,
                        g_PostSLCooldownRemaining.load(std::memory_order_relaxed));
                }
            }

            int cooldownLeft = g_PostSLCooldownRemaining.load(std::memory_order_acquire);
            if (cooldownLeft > 0) {
                if (ce::dx12_overlay_policy::ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(
                        g_HadFSRFGPhase, safePostFSRBootstrapPathForPostSL)) {
                    g_PostSLCooldownRemaining.fetch_sub(1, std::memory_order_acq_rel);
                    if (cooldownLeft > 1) {
                        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
                        NoteDX12OverlayCoverageGate("postsl-startup-countdown");
                        return;
                    }
                } else if (safePostFSRBootstrapPathForPostSL) {
                    static int s_safePostFSRActivationLogCount = 0;
                    if (s_safePostFSRActivationLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup bypassing repeated-callback cooldown after safe post-FSR "
                            "bootstrap proof (cooldown=%d progress=%d)",
                            cooldownLeft, startupWrapperProgressCount);
                    }
                    s_safePostFSRActivationLogCount++;
                    g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                } else if (useTopLevelHandoffWrapperProgress) {
                    static int s_wrapperProgressActivationLogCount = 0;
                    if (s_wrapperProgressActivationLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup using wrapper ECL progress after top-level handoff "
                            "(cooldown=%d progress=%d)",
                            cooldownLeft, startupWrapperProgressCount);
                    }
                    s_wrapperProgressActivationLogCount++;
                    g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                } else if (explicitEnablePureDLSSColdStartProof) {
                    // Proof-gated no-blank engage: the current comeback was
                    // activated by an explicit slDLSSGSetOptions(ON) edge and
                    // CE retains the runtime-owned startup activation
                    // swapchain, so this callback is a real Streamline-routed
                    // present of the live proxy. Activate from callback #1
                    // instead of blanking through the 8-callback countdown.
                    // GetState-only enables (the historical GTA startup-churn
                    // family) never reach this branch.
                    static int s_explicitEnableCountdownBypassLogCount = 0;
                    if (s_explicitEnableCountdownBypassLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup bypassing pure-DLSS countdown after explicit "
                            "slDLSSGSetOptions(ON) proof (cooldown=%d retainedStartupSwapchain=1)",
                            cooldownLeft);
                    }
                    s_explicitEnableCountdownBypassLogCount++;
                    g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                } else if (sameQueuePureDLSSColdStartSafe) {
                    // Same-queue pure-DLSS cold start (Talos): DLSS FG runs on the game's OWN single
                    // queue (scQueue==origGame, no separate command/SL-wrapper queue), so there is no
                    // separate DLSS-G proxy-init pipeline for CE's ECL to corrupt — activate from
                    // callback #1 instead of blanking through the countdown. The documented GTA hang
                    // family creates a SEPARATE runtime-owned queue during init (this proof is re-checked
                    // every callback and flips false the moment that happens, restoring the countdown).
                    static int s_sameQueueColdStartCountdownBypassLogCount = 0;
                    if (s_sameQueueColdStartCountdownBypassLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup bypassing pure-DLSS countdown — same-queue topology "
                            "(scQueue==origGame, no separate command/SL-wrapper queue): overlay ECL lands on the "
                            "game's own queue, not a separate DLSS-G init pipeline (cooldown=%d)",
                            cooldownLeft);
                    }
                    s_sameQueueColdStartCountdownBypassLogCount++;
                    g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                } else {
                    // Pure DLSS cold start without explicit-enable proof: use a
                    // shorter stabilization period instead of bypassing
                    // entirely.  DLSS FG needs a few callbacks to initialize
                    // its internal pipeline (queue setup, mutex state, fence
                    // tracking) before our ECL can safely land on its queue.
                    // Without this, the very first PostSL render can corrupt
                    // DLSS FG state and cause a hang (observed in GTA V
                    // Enhanced with GetState-only activation evidence).
                    constexpr int kPureDLSSMinCooldown = 8;
                    int clamped = std::min(cooldownLeft, kPureDLSSMinCooldown);
                    int remaining = clamped > 0 ? clamped - 1 : 0;
                    g_PostSLCooldownRemaining.store(remaining, std::memory_order_release);
                    static int s_pureDLSSCooldownLogCount = 0;
                    if (s_pureDLSSCooldownLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup reduced cooldown for pure DLSS cold start "
                            "(original=%d clamped=%d remaining=%d)",
                            cooldownLeft, clamped, remaining);
                    }
                    s_pureDLSSCooldownLogCount++;
                    if (clamped > 1) {
                        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
                        NoteDX12OverlayCoverageGate("postsl-startup-countdown");
                        return;
                    }
                }
            }

            auto* probeDev = g_Device.load(std::memory_order_acquire);
            const bool startupWindowActiveForProbe = DXGIShared::IsStreamlineStartupTransitionWindowActive();
            if (!g_RealD3D12ECL.load(std::memory_order_acquire) && probeDev && IsStreamlineLoaded()) {
                if (!startupWindowActiveForProbe) {
                    ProbeRealD3D12ECL(probeDev);
                    HookLogImportant("DX12: PostSL synthetic startup activation probed realECL=%p",
                                     (void*)g_RealD3D12ECL.load(std::memory_order_acquire));
                } else {
                    g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                    HookLogImportant(
                        "DX12: PostSL synthetic startup activation deferred ECL probe "
                        "(startup window active, will probe after window expires)");
                }
            }

            ID3D12CommandQueue* directQueue = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
            ExecuteCommandListsPtr directECL = g_RealD3D12ECL.load(std::memory_order_acquire);
            ID3D12CommandQueue* slWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);
            if (ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(
                    g_HadFSRFGPhase, directQueue != nullptr, directECL != nullptr, slWrapperQueue != nullptr,
                    safePostFSRBootstrapPathForPostSL)) {
                static int s_waitForSafePathLog = 0;
                if (s_waitForSafePathLog < 10 || (s_waitForSafePathLog % 100) == 0) {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup waiting for safe bootstrap path after FSR phase "
                        "(realQ=%p realECL=%p slWrapper=%p safeBootstrap=%d)",
                        directQueue, (void*)directECL, slWrapperQueue, safePostFSRBootstrapPathForPostSL ? 1 : 0);
                }
                s_waitForSafePathLog++;
                s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
                NoteDX12OverlayCoverageGate("postsl-wait-safe-bootstrap");
                return;
            }

            const bool enterSyntheticStartupActivation =
                ce::dx12_overlay_policy::ShouldEnterSyntheticPostSLStartupActivation(
                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire),
                    postSLActiveButUnconfirmed, postSLConfirmedRendering);
            g_PostSLOverlayActive.store(true, std::memory_order_release);
            g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
            if (enterSyntheticStartupActivation) {
                syntheticStartupActivatedThisCall = true;
                g_PostSLSyntheticStartupActivatedButUnconfirmed.store(true, std::memory_order_release);
                g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
                DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
                // Startup is still half-armed until the first real PostSL render confirms
                // that the path is actually safe. Activation alone is not enough.
                HookLogImportant("DX12: PostSL synthetic startup activation complete — enabling PostSL rendering");
                ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLActivationComplete,
                                            "DX12::PostSLSyntheticStartupActivation", directQueue, pSwapChain,
                                            g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(),
                                            HookHasExplicitStreamlineSetOptionsActivation());
            } else {
                static int s_repeatSyntheticStartupActivationLog = 0;
                if (s_repeatSyntheticStartupActivationLog < 10 || (s_repeatSyntheticStartupActivationLog % 200) == 0) {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup activation already half-armed — preserving warm-up progress "
                        "(pending=%d unconfirmed=%d confirmed=%d repeat=%d)",
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                            std::memory_order_relaxed)
                            ? 1
                            : 0,
                        postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                        s_repeatSyntheticStartupActivationLog + 1);
                }
                s_repeatSyntheticStartupActivationLog++;
            }
        }
    }

    if (syntheticStartupActivatedThisCall && immediateSameQueueStartupTakeover && normalRouteDrawPendingAtEntry) {
        // The normal route already covered this exact present. Leave PostSL
        // active for the next callback, but do not render twice during the
        // make-before-break boundary. PostSLOverlayRenderGated's scope guard
        // accounts the pending normal draw on return.
        NoteDX12OverlayCoverageGate("postsl-same-queue-make-before-break");
        HookLogImportant(
            "DX12: PostSL immediate same-queue takeover preserved the current normal-route draw — first PostSL "
            "draw moves to the next present (no blank, no double draw)");
        return;
    }

    uint32_t lifecycleEpoch = entryLifecycleEpoch;
    bool lifecycleChanged = lifecycleEpoch != s_seenLifecycleEpoch;
    if (lifecycleChanged) {
        s_wasActive = false;
        s_seenLifecycleEpoch = lifecycleEpoch;
    }

    bool active = g_PostSLOverlayActive.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldTreatPostSLAsReactivated(active, s_wasActive, lifecycleChanged)) {
        s_reactivationEpoch++;
        s_callsSinceReactivation = 0;
        s_postSLProbeFrames = 0;  // Reset probe counter for new reactivation
        // Epoch-scoped: a genuine reactivation must re-prove the first ECL is safe before
        // the warmup can be confirmed-bypassed. Cleared here so a confirmed render from a
        // previous epoch can never bypass a real cold-start warmup.
        g_PostSLConfirmedRenderInCurrentReactivationEpoch.store(false, std::memory_order_release);
        const bool previouslyConfirmed = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
        const int previousStableFrameCount = g_PostSLStableFrameCount.exchange(0, std::memory_order_acq_rel);
        const int previousStallCount = g_PostSLStallCounter.exchange(0, std::memory_order_acq_rel);
        const bool previousRuntimeStateStabilizationLogged =
            g_PostSLRuntimeStateStabilizationLogged.exchange(false, std::memory_order_acq_rel);
        const bool extendRuntimeStateStabilization =
            ce::dx12_overlay_policy::ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(
                previousStableFrameCount);
        g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(extendRuntimeStateStabilization,
                                                                       std::memory_order_release);
        // Clean up dedicated queue from previous epochs (no longer used — virtual
        // call through SL's COM wrapper is now the primary submission path).
        ClearPostSLQueues("DX12: PostSL reactivation");
        if (s_dedicatedSyncFence) {
            s_dedicatedSyncFence->Release();
            s_dedicatedSyncFence = nullptr;
        }
        if (s_dedicatedFenceEvent) {
            CloseHandle(s_dedicatedFenceEvent);
            s_dedicatedFenceEvent = nullptr;
        }
        s_dedicatedSyncFenceValue = 0;
        if (ce::dx12_overlay_policy::ShouldResetPostSLStartupProgressOnReactivation(
                previouslyConfirmed, previousStableFrameCount, previousStallCount,
                previousRuntimeStateStabilizationLogged)) {
            HookLogImportant(
                "DX12: PostSL reactivation reset confirmed-startup progress "
                "(epoch=%d confirmed=%d stableFrames=%d stallCount=%d stabilizing=%d extendStaleOff=%d)",
                s_reactivationEpoch, previouslyConfirmed ? 1 : 0, previousStableFrameCount, previousStallCount,
                previousRuntimeStateStabilizationLogged ? 1 : 0, extendRuntimeStateStabilization ? 1 : 0);
        }
        if (extendRuntimeStateStabilization) {
            HookLogImportant(
                "DX12: PostSL reactivation extended runtime-state stabilization for churned startup "
                "(epoch=%d previousStableFrames=%d previousStallCount=%d proofThreshold=%d)",
                s_reactivationEpoch, previousStableFrameCount, previousStallCount,
                ce::dx12_overlay_policy::GetConfirmedPostSLWarmupProofFrameThreshold());
        }
        HookLogImportant("DX12: PostSL REACTIVATED (epoch=%d hadFSR=%d origGame=%p)", s_reactivationEpoch,
                         g_HadFSRFGPhase ? 1 : 0, g_OriginalGameQueue);
        // Arm the verbose overlay-handoff diagnostic so the next presents log per-present coverage
        // detail. prevRoute distinguishes off->DLSS (prevRoute=normal, native->fresh-proxy — the
        // reported slight-flash case) from FSR->DLSS (prevRoute=post-sl/ffx, warm proxy).
        {
            const uint32_t prevRoute = g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
            g_OverlayHandoffVerbosePrevRoute.store(prevRoute, std::memory_order_relaxed);
            g_OverlayHandoffVerboseLogPresents.store(16, std::memory_order_relaxed);
            HookLogImportant(
                "[OVERLAY HANDOFF] PostSL reactivation armed verbose window (epoch=%d hadFSR=%d prevRoute=%s "
                "swapchain=%p) — logging the next 16 presents to pinpoint an off->DLSS fresh-proxy overlay flash",
                s_reactivationEpoch, g_HadFSRFGPhase ? 1 : 0, DX12OverlayRenderRouteName(prevRoute), (void*)pSwapChain);
        }
        // Reset ECL diagnostic counter for fresh diagnostics after transition
        g_PostSLECLDiagCount.store(0, std::memory_order_relaxed);
