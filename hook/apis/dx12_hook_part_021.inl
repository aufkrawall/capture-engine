        // Reset post-FSR probe state for fresh graduated probing
        g_PostFSRProbeLevel.store(0, std::memory_order_release);
        g_PostFSRProbeFrames.store(0, std::memory_order_release);
        g_PostFSRDescFreeRecreated = false;
    }
    s_wasActive = active;
    s_callsSinceReactivation++;

    // Gate: only render when explicitly enabled (not during cooldown / resize).
    // The make-before-break keep-alive renders regardless: it is the same
    // confirmed path that rendered one present earlier.
    if (!active && !keepAliveRenderAfterExplicitOff) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-inactive");
        static int s_gateSkip = 0;
        if (s_gateSkip++ < 5)
            HookLog("DX12: PostSL SKIP — g_PostSLOverlayActive=false");
        return;
    }

    // Secondary gate: don't render during FG transition cooldown.
    // g_PostSLOverlayActive may be stale if set before ProcessFrame disables it.
    int cooldownLeft = g_PostSLCooldownRemaining.load(std::memory_order_acquire);
    if (cooldownLeft > 0 && !keepAliveRenderAfterExplicitOff) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-fg-transition-cooldown");
        static int s_cooldownSkip = 0;
        if (s_cooldownSkip++ < 5)
            HookLog("DX12: PostSL SKIP — FG transition cooldown active (%d frames left)", cooldownLeft);
        return;
    }

    // Post-reactivation warm-up: after FG transition reactivation, skip rendering
    // for the first N frames to let DLSS FG's internal pipeline fully stabilize.
    // Observed: first ECL on origGame queue after FSR→DLSS switch causes
    // DEVICE_REMOVED, even with correct queue and no cross-queue sync.
    // Waiting ~30 frames lets SL's FG pipeline establish its internal state.
    //
    // Cold-start DLSS (epoch 1): a shorter warmup gives DLSS FG time to initialize
    // its internal pipeline (queue setup, mutex state, fence tracking) before our
    // first ECL submission.  Without this, the very first PostSL render can corrupt
    // DLSS FG's state and cause a hang/crash (observed in GTA V Enhanced).
    constexpr int kPostSLReactivationWarmup = 30;
    constexpr int kPostSLColdStartWarmup = 15;
    const int warmupThreshold = (s_reactivationEpoch > 1) ? kPostSLReactivationWarmup : kPostSLColdStartWarmup;
    ID3D12CommandQueue* warmupSwapchainQueue = nullptr;
    ID3D12CommandQueue* warmupLastWorkingQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        warmupSwapchainQueue = g_SwapchainQueue;
        warmupLastWorkingQueue = g_PostSLLastWorkingQueue;
    }
    const bool confirmedPureStreamlineResumeWarmupProof =
        ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(
            g_HadFSRFGPhase, cachedSLFGActive, startupTopLevelPresentConsumed, warmupLastWorkingQueue != nullptr,
            warmupSwapchainQueue != nullptr,
            warmupLastWorkingQueue != nullptr && warmupSwapchainQueue != nullptr &&
                warmupLastWorkingQueue == warmupSwapchainQueue);
    const bool postSLConfirmedRenderThisEpoch =
        g_PostSLConfirmedRenderInCurrentReactivationEpoch.load(std::memory_order_acquire);
    const bool bypassReactivationWarmup = ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup(
        g_HadFSRFGPhase, useTopLevelHandoffWrapperProgress, safePostFSRBootstrapPathForPostSL,
        confirmedPureStreamlineResumeWarmupProof, explicitEnablePureDLSSColdStartProof, postSLConfirmedRenderThisEpoch,
        sameQueuePureDLSSColdStartSafe);
    if (s_callsSinceReactivation <= warmupThreshold && !bypassReactivationWarmup && !keepAliveRenderAfterExplicitOff) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-reactivation-warmup");
        if (s_callsSinceReactivation <= 5 || s_callsSinceReactivation == warmupThreshold) {
            // Log the proof inputs that resolved bypassReactivationWarmup=false so a
            // true->false flip (e.g. the explicit-enable proof dropping when the retained
            // startup swapchain is released after frame 1) is visible inline on the first
            // skipped frame, without cross-referencing the SUBMIT/release lines.
            HookLogImportant(
                "DX12: PostSL warm-up after reactivation epoch=%d frame=%d/%d (coldStart=%d hadFSR=%d "
                "safeBootstrap=%d confirmedResume=%d explicitEnableColdStart=%d confirmedThisEpoch=%d "
                "retainedSwapchain=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, warmupThreshold, s_reactivationEpoch <= 1 ? 1 : 0,
                g_HadFSRFGPhase ? 1 : 0, safePostFSRBootstrapPathForPostSL ? 1 : 0,
                confirmedPureStreamlineResumeWarmupProof ? 1 : 0, explicitEnablePureDLSSColdStartProof ? 1 : 0,
                postSLConfirmedRenderThisEpoch ? 1 : 0, HasRetainedStreamlineStartupActivationSwapchain() ? 1 : 0);
        }
        return;
    }
    if (s_callsSinceReactivation <= warmupThreshold && bypassReactivationWarmup) {
        static int s_bypassWarmupLogCount = 0;
        if (s_bypassWarmupLogCount < 10) {
            HookLogImportant(
                "DX12: PostSL bypassing reactivation warm-up after safe startup proof "
                "(epoch=%d frame=%d/%d hadFSR=%d wrapperProgress=%d safeBootstrap=%d confirmedResume=%d "
                "explicitEnableColdStart=%d confirmedThisEpoch=%d sameQueueColdStart=%d scQ=%p lastWorkingQ=%p)",
                s_reactivationEpoch, s_callsSinceReactivation, warmupThreshold, g_HadFSRFGPhase ? 1 : 0,
                useTopLevelHandoffWrapperProgress ? 1 : 0, safePostFSRBootstrapPathForPostSL ? 1 : 0,
                confirmedPureStreamlineResumeWarmupProof ? 1 : 0, explicitEnablePureDLSSColdStartProof ? 1 : 0,
                postSLConfirmedRenderThisEpoch ? 1 : 0, sameQueuePureDLSSColdStartSafe ? 1 : 0, warmupSwapchainQueue,
                warmupLastWorkingQueue);
        }
        s_bypassWarmupLogCount++;
    }

    // DEBUG: Log when warmup completes and we're about to proceed to actual rendering
    if (s_callsSinceReactivation == warmupThreshold + 1 ||
        (bypassReactivationWarmup && s_callsSinceReactivation == 1)) {
        const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        HookLogImportant(
            "DX12: PostSL WARMUP COMPLETE — proceeding to render submission "
            "(epoch=%d warmupFrames=%d confirmed=%d startupWindowActive=%d overlayInit=%d syncInit=%d "
            "swapchain=%p dev=%p bypassed=%d)",
            s_reactivationEpoch, warmupThreshold, g_PostSLConfirmedRendering.load(std::memory_order_acquire) ? 1 : 0,
            startupWindowActive ? 1 : 0, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0, (void*)pSwapChain,
            (void*)(g_State.syncDevice ? g_State.syncDevice : g_Device.load(std::memory_order_acquire)),
            bypassReactivationWarmup ? 1 : 0);
    }

    // Startup transition window rendering gate: while the startup transition
    // window is active, DLSS FG is still initializing its internal pipeline.
    // Submitting ECL on the SL-owned swapchain queue during this phase can
    // corrupt DLSS FG's internal state (mutex tracking, fence state), leading
    // to hangs/crashes.  Wait for the window to expire before submitting GPU work.
    // Once PostSL has confirmed stable rendering (from a previous cycle), this
    // gate no longer applies — the pipeline is proven stable.
    const bool startupTransitionWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool postSLWarmupComplete = bypassReactivationWarmup || s_callsSinceReactivation > warmupThreshold;
    if (startupTransitionWindowActive && !g_PostSLConfirmedRendering.load(std::memory_order_acquire) &&
        safePostFSRBootstrapPathForPostSL) {
        static int s_bypassStartupWindowGuardLog = 0;
        if (s_bypassStartupWindowGuardLog < 10) {
            HookLogImportant(
                "DX12: PostSL bypassing startup transition window deferral after safe post-FSR bootstrap proof "
                "(epoch=%d call#=%d wrapperProgress=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, useTopLevelHandoffWrapperProgress ? 1 : 0);
        }
        s_bypassStartupWindowGuardLog++;
    }
    if (startupTransitionWindowActive && !g_PostSLConfirmedRendering.load(std::memory_order_acquire) &&
        !safePostFSRBootstrapPathForPostSL && cachedSLFGActive && postSLWarmupComplete) {
        static int s_activeRuntimeStartupWindowGuardLog = 0;
        if (s_activeRuntimeStartupWindowGuardLog < 10) {
            HookLogImportant(
                "DX12: PostSL bypassing startup transition window deferral after active DLSS FG runtime proof "
                "(epoch=%d call#=%d warmup=%d/%d wrapperProgress=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, warmupThreshold, warmupThreshold,
                useTopLevelHandoffWrapperProgress ? 1 : 0);
        }
        s_activeRuntimeStartupWindowGuardLog++;
    }
    if (ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(
            startupTransitionWindowActive, g_PostSLConfirmedRendering.load(std::memory_order_acquire),
            useTopLevelHandoffWrapperProgress, safePostFSRBootstrapPathForPostSL, cachedSLFGActive,
            postSLWarmupComplete)) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        static int s_startupWindowGuardLog = 0;
        if (s_startupWindowGuardLog < 10 || (s_startupWindowGuardLog % 200) == 0) {
            HookLogImportant(
                "DX12: PostSL SKIP — startup transition window active, deferring ECL until DLSS FG stabilizes "
                "(epoch=%d call#=%d activeDLSSSignal=%d warmupComplete=%d safeBootstrap=%d wrapperProgress=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, cachedSLFGActive ? 1 : 0, postSLWarmupComplete ? 1 : 0,
                safePostFSRBootstrapPathForPostSL ? 1 : 0, useTopLevelHandoffWrapperProgress ? 1 : 0);
        }
        s_startupWindowGuardLog++;
        return;
    }

    // Use the sync device (the one that created allocators/cmdList/fence) for all
    // per-frame D3D12 operations.  g_Device may have been updated by the ECL hook
    // to a different device pointer (SL wraps devices), causing cross-device
    // CreateRenderTargetView or descriptor heap access → DEVICE_REMOVED.
    auto* dev = g_State.syncDevice;
    if (!dev)
        dev = g_Device.load(std::memory_order_acquire);

    if (dev && ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(
                   cachedSLFGActive, active, g_State.overlayInit, processFrameRecentlySeen, startupActivationPending,
                   postSLActiveButUnconfirmed, postSLConfirmedRendering)) {
        if (!pSwapChain) {
            static int s_nullBootstrapLog = 0;
            if (s_nullBootstrapLog < 5) {
                HookLogImportant("DX12: PostSL bootstrap skipped — pSwapChain is nullptr");
                ++s_nullBootstrapLog;
            }
        } else {
            DXGI_SWAP_CHAIN_DESC bootstrapDesc = {};
            const HRESULT descHr = pSwapChain->GetDesc(&bootstrapDesc);
            if (SUCCEEDED(descHr)) {
                IDXGISwapChain3* bootstrapSc3 = nullptr;
                const HRESULT sc3Hr = pSwapChain->QueryInterface(IID_PPV_ARGS(&bootstrapSc3));
                if (SUCCEEDED(sc3Hr) && bootstrapSc3) {
                    ID3D12CommandQueue* bootstrapScQueue = nullptr;
                    ID3D12CommandQueue* bootstrapCmdQueue = nullptr;
                    ID3D12CommandQueue* bootstrapOrigQueue = nullptr;
                    {
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        bootstrapScQueue = g_SwapchainQueue;
                        bootstrapCmdQueue = g_CommandQueue.load(std::memory_order_acquire);
                        bootstrapOrigQueue = g_OriginalGameQueue;
                    }

                    g_State.cachedWidth = bootstrapDesc.BufferDesc.Width;
                    g_State.cachedHeight = bootstrapDesc.BufferDesc.Height;
                    g_State.format = bootstrapDesc.BufferDesc.Format;

                    HookLogImportant(
                        "DX12: PostSL bootstrap — rebuilding torn-down overlay state after dormant reactivation "
                        "(fmt=%d buffers=%u hwnd=%p scQueue=%p cmdQueue=%p origQueue=%p)",
                        (int)bootstrapDesc.BufferDesc.Format, bootstrapDesc.BufferCount, bootstrapDesc.OutputWindow,
                        bootstrapScQueue, bootstrapCmdQueue, bootstrapOrigQueue);
                    // Attribution for the FSR->DLSS-comeback floor gap: when this reactivation
                    // present rebuilds the backend before the first confirmed PostSL draw lands
                    // (the draw covers the NEXT present), the present is uncovered. Label the
                    // coverage gate here so that documented 1-present floor reports
                    // `postsl-bootstrap-reactivation` instead of `unknown` (session
                    // 20260613_211048: the sole gate=unknown streak). Read only if uncovered.
                    NoteDX12OverlayCoverageGate("postsl-bootstrap-reactivation");

                    if (InitImGui(dev, (int)bootstrapDesc.BufferCount, bootstrapDesc.BufferDesc.Format,
                                  bootstrapDesc.OutputWindow)) {
                        int actualBufferCount = (int)bootstrapDesc.BufferCount;
                        if (actualBufferCount > 8) {
                            actualBufferCount = 8;
                        }
                        CreateRTVs(dev, bootstrapSc3, actualBufferCount);

                        ID3D12CommandQueue* bootstrapQueue = bootstrapScQueue;
                        if (!bootstrapQueue) {
                            bootstrapQueue = bootstrapCmdQueue;
                        }
                        if (!bootstrapQueue) {
                            bootstrapQueue = bootstrapOrigQueue;
                        }

                        if (bootstrapQueue && g_State.rtvDescHeap) {
                            HookLogImportant(
                                "DX12: PostSL bootstrap — inline InitOverlaySync (queue=%p overlayInit=%d syncInit=%d)",
                                bootstrapQueue, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0);
                            InitOverlaySync(dev, (int)bootstrapDesc.BufferCount, bootstrapQueue);
                            dev = g_State.syncDevice;
                            if (!dev) {
                                dev = g_Device.load(std::memory_order_acquire);
                            }
                        } else {
                            HookLogImportant(
                                "DX12: PostSL bootstrap — waiting for missing init prerequisites (queue=%p rtvHeap=%p)",
                                bootstrapQueue, g_State.rtvDescHeap);
                        }
                    } else {
                        HookLogImportant("DX12: PostSL bootstrap — InitImGui failed (fmt=%d buffers=%u hwnd=%p)",
                                         (int)bootstrapDesc.BufferDesc.Format, bootstrapDesc.BufferCount,
                                         bootstrapDesc.OutputWindow);
                    }

                    bootstrapSc3->Release();
                } else {
                    HookLogImportant("DX12: PostSL bootstrap — swapchain3 query failed hr=0x%08X", (unsigned)sc3Hr);
                }
            } else {
                HookLogImportant("DX12: PostSL bootstrap — swapchain desc unavailable hr=0x%08X", (unsigned)descHr);
            }
        }
    }

    // After FG type transitions, syncInit is reset to force fresh sync resources.
    // PostSL re-initializes inline with the current queue (scQueue or g_CommandQueue).
    if (dev && g_State.overlayInit && !g_State.syncInit) {
        ID3D12CommandQueue* reinitQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            reinitQueue = g_SwapchainQueue;
            if (!reinitQueue)
                reinitQueue = g_CommandQueue.load(std::memory_order_acquire);
        }
        if (reinitQueue) {
            HookLogImportant("DX12: PostSL triggering inline InitOverlaySync (queue=%p dev=%p)", reinitQueue, dev);
            InitOverlaySync(dev, g_State.bufferCount, reinitQueue);
            dev = g_State.syncDevice;
            if (!dev)
                dev = g_Device.load(std::memory_order_acquire);
        }
    }

    if (!dev || !g_State.overlayInit || !g_State.syncInit || !g_State.cmdList || g_State.allocators.empty()) {
        static int s_stateSkip = 0;
        const int stateSkip = s_stateSkip++;
        if (stateSkip < 5 || s_callsSinceReactivation <= 20) {
            HookLogImportant(
                "DX12: PostSL SKIP — state unavailable (epoch=%d call#=%d dev=%p syncDev=%p init=%d sync=%d "
                "list=%p alloc=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, (void*)g_Device.load(), dev, g_State.overlayInit ? 1 : 0,
                g_State.syncInit ? 1 : 0, g_State.cmdList, (int)g_State.allocators.size());
        }
        return;
    }

    // Don't render if device is removed
    if (g_DeviceRemoved.load(std::memory_order_relaxed))
        return;

    HRESULT devReason = dev->GetDeviceRemovedReason();
    if (FAILED(devReason)) {
        g_DeviceRemoved.store(true, std::memory_order_release);
        DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
        HookLogImportant("DX12: PostSLOverlayRender — device removed (0x%08X), disabling", (unsigned)devReason);
        g_PostSLOverlayActive.store(false, std::memory_order_release);
        return;
    }

    // Don't render if swapchain is being resized
    if (DXGIShared::g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        static int s_scInvalid = 0;
        if (s_scInvalid++ < 5)
            HookLog("DX12: PostSL SKIP — swapchainInvalid=true");
        return;
    }

    // The first DLSS-G input frame may already carry CE's overlay through Streamline's
    // official UIColorAndAlpha tag. That route covers generated output before PostSL can
    // possibly run. It is not proof that the first proxy output reaching PostSL contains
    // the tag, though: cold OFF->DLSS activation can expose that output first. Once a
    // proven safe PostSL callback exists, take over its exact backbuffer immediately and
    // retire the bounded UI handoff. GetState-only activation keeps the conservative tag
    // consumption path because it lacks explicit current-activation provenance.
    const bool officialUiCoverageActive = ce::dx12_streamline_ui_overlay::HasActiveCoverage();
    const bool requireExactPostSLStartupOutputDraw =
        ce::dx12_overlay_policy::ShouldRequireExactPostSLBackbufferDrawForStartup(
            g_RequireExactPostSLStartupTransportDraw, g_HadFSRFGPhase, safePostFSRBootstrapPathForPostSL,
            explicitEnablePureDLSSColdStartProof, officialUiCoverageActive);
    const bool retireOfficialUiCoverageAfterExactDraw = requireExactPostSLStartupOutputDraw && officialUiCoverageActive;
    if (!requireExactPostSLStartupOutputDraw && ce::dx12_streamline_ui_overlay::ConsumePostSLCoverage()) {
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kStreamlineUI);
        return;
    }
    // After FSR→DLSS: PostSL rendering causes DEVICE_REMOVED. Use graduated
    // probes so we do not jump directly from an empty submit to a full
    // copy-render-copy overlay pass on the first real PostSL frame.

    // Scene transition cooldown: skip overlay during scene loads/transitions
    int cd = g_SceneTransitionCooldown.load(std::memory_order_acquire);
    if (cd > 0) {
        g_SceneTransitionCooldown.store(cd - 1, std::memory_order_release);
        if (cd == 1) {
            ID3D12CommandQueue* resumeQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                resumeQueue = g_PostSLLastWorkingQueue;
                if (!resumeQueue)
                    resumeQueue = g_CommandQueue.load(std::memory_order_acquire);
                if (!resumeQueue)
                    resumeQueue = g_SwapchainQueue;
            }
            HookLogImportant(
                "DX12: Post-SL scene transition cooldown complete — resuming overlay "
                "(queue=%p overlayInit=%d syncInit=%d bufCount=%d)",
                resumeQueue, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0, g_State.bufferCount);
        }
        return;
    }

    // Get the submission queue for PostSL overlay.
    //
    // CRITICAL: Lock to the first queue that works and DON'T follow g_CommandQueue
    // changes.  During DLSS FG, SL creates internal FG worker queues and starts
    // calling ECL from them.  Our DetourECL hook updates g_CommandQueue to these
    // new SL queues, but they may be COM wrapper/aggregation objects incompatible
    // with realECL.  The game's original queue (captured at the start of FG) is
    // a real D3D12 queue that works with realECL.
    //
    // Queue selection:
    // 1. exact explicit-OFF keep-alive: g_PostSLLastWorkingQueue (the retained
    //    direct queue which successfully rendered this exact proxy)
    // 2. g_PostSLLockedQueue — the ordinary proven queue for this epoch
    // 3. g_OriginalGameQueue — the game's very first queue (SL synchronizes with it)
    // 4. g_CommandQueue — last resort, may be SL's internal queue during FG
    //
    // After FG transitions (FSR→DLSS), the NVIDIA driver's internal state for
    // existing queues (including origGame) can become corrupted.  On reactivation
    // PostSL queue selection strategy:
    //
    // PREFER scQueue (swapchain queue): SL transitions the backbuffer to PRESENT
    // on scQueue before calling Present.  By submitting our ECL on scQueue too,
    // D3D12 resource state tracking is correct (same queue = serialized execution).
    // PRESENT→RT and RT→PRESENT barriers work reliably because the before-state
    // matches.  A dedicated queue would break state tracking (different queue
    // doesn't know the resource's current state), causing DEVICE_REMOVED even
    // if the queue itself is healthy.
    //
    // For scQueue, use origECL (SL's original ECL captured from the vtable hook)
    // instead of realECL, because scQueue may be an SL COM wrapper object whose
    // memory layout differs from raw D3D12 CCommandQueue.
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* scQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        scQueue = g_SwapchainQueue;

        // Queue selection strategy for PostSL:
        //
        // DLSS FG (no prior FSR FG): use origGame.  It's the swapchain creation
        //   queue with valid NVIDIA driver state and authorized backbuffer access.
        //
        // DLSS FG (after FSR FG was active): prefer the runtime-owned swapchain
        //   queue or a captured direct queue behind SL's wrapper. Keeping PostSL
        //   locked to the wrapper itself can poison long-running FG state and later
        //   crash on teardown, so wrapper use is bootstrap-only at most.
        //
        // Outside SL FG: exact OFF keep-alive lastWorking > locked > scQueue >
        // origGame > preFG > cmdQueue.
        bool slFGNow = cachedSLFGActive;
        // GTA V's DLSS FG activation triggers a heuristic FSR ghost (brief swapchain
        // queue change) that clears within frames.  Setting hadFSR from heuristic forces
        // PostSL onto SL's internal queues which causes DEVICE_HUNG.
        if (ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(g_FGCompat.IsFSRFGApiActive(), false)) {
            if (!g_HadFSRFGPhase) {
                g_HadFSRFGPhase = true;
                HookLogImportant("DX12: PostSL — FSR FG history confirmed, origGame driver state may be stale");
            }
        }

        ID3D12CommandQueue* directQueueBehindWrapper = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
        ID3D12CommandQueue* latestSLWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* validatedCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        ExecuteCommandListsPtr currentRealECL = g_RealD3D12ECL.load(std::memory_order_acquire);
        const bool validatedCommandQueueIsWrapper =
            validatedCommandQueue && validatedCommandQueue != g_OriginalGameQueue && validatedCommandQueue != scQueue;
        ID3D12CommandQueue* wrapperBootstrapQueue = latestSLWrapperQueue;
        if (ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(
                g_HadFSRFGPhase, slFGNow, directQueueBehindWrapper != nullptr, validatedCommandQueueIsWrapper,
                scQueue != nullptr && scQueue != g_OriginalGameQueue,
                HookHasExplicitStreamlineSetOptionsActivation())) {
            wrapperBootstrapQueue = validatedCommandQueue;
        }
        bool hasDirectQueueBehindWrapper = directQueueBehindWrapper != nullptr;
        const bool hasRuntimeOwnedSwapchainQueue = scQueue != nullptr && scQueue != g_OriginalGameQueue;
        const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
        const bool explicitSetOptionsActivation = HookHasExplicitStreamlineSetOptionsActivation();
        bool preferRealQueueBehindWrapper = ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(
            g_HadFSRFGPhase, slFGNow, hasDirectQueueBehindWrapper);
        const bool preferValidatedDirectQueueForLock =
            ce::dx12_overlay_policy::ShouldPreferValidatedDirectQueueForPostFSRLock(g_HadFSRFGPhase, slFGNow,
                                                                                    hasDirectQueueBehindWrapper);
        bool allowWrapperBootstrapQueue = ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(
            g_HadFSRFGPhase, slFGNow, hasDirectQueueBehindWrapper, wrapperBootstrapQueue != nullptr,
            hasRuntimeOwnedSwapchainQueue, explicitSetOptionsActivation, safePostFSRBootstrapPath);
        const bool resumeOnValidatedLastWorkingQueue = ce::dx12_overlay_policy::
            ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                g_HadFSRFGPhase, g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire),
                g_PostSLLastWorkingQueue != nullptr, scQueue != nullptr, explicitSetOptionsActivation,
                safePostFSRBootstrapPath);
        const bool lockedQueueIsSLWrapper =
            g_PostSLLockedQueue && g_PostSLLockedQueue != g_OriginalGameQueue && g_PostSLLockedQueue != scQueue;
        ExecuteCommandListsPtr scQueueOrigECL = scQueue ? GetOriginalExecuteCommandLists(scQueue) : nullptr;
        const bool hasSwapchainQueueSubmitPath = scQueue && (scQueueOrigECL != nullptr || currentRealECL != nullptr);
        const bool hasWrapperDerivedDirectPath = directQueueBehindWrapper != nullptr && currentRealECL != nullptr;
        const bool selectDirectQueueInsteadOfLockedWrapper =
            ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
                g_PostSLLockedQueue != nullptr, g_HadFSRFGPhase, slFGNow, lockedQueueIsSLWrapper,
                hasDirectQueueBehindWrapper);
        const bool selectSwapchainQueueInsteadOfLockedWrapper =
            ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
                g_PostSLLockedQueue != nullptr, g_HadFSRFGPhase, slFGNow, lockedQueueIsSLWrapper, scQueue != nullptr,
                scQueue != g_OriginalGameQueue, hasSwapchainQueueSubmitPath, hasWrapperDerivedDirectPath);

        if (preferValidatedDirectQueueForLock && directQueueBehindWrapper) {
            queue = directQueueBehindWrapper;
            static int s_directQueuePreferredLog = 0;
            if (s_directQueuePreferredLog++ < 10) {
                HookLogImportant(
                    "DX12: PostSL queue candidate — validated direct queue %p preferred over scQueue %p after FSR",
                    queue, scQueue);
            }
        } else if (selectDirectQueueInsteadOfLockedWrapper) {
            queue = directQueueBehindWrapper;
            static int s_promoteSelectionLog = 0;
            if (s_promoteSelectionLog++ < 5) {
                HookLog("DX12: PostSL queue candidate — direct real queue %p replacing locked wrapper %p", queue,
                        g_PostSLLockedQueue);
            }
        } else if (selectSwapchainQueueInsteadOfLockedWrapper) {
            queue = scQueue;
            static int s_swapchainSelectionLog = 0;
            if (s_swapchainSelectionLog++ < 10) {
                HookLogImportant(
                    "DX12: PostSL queue candidate — swapchain queue %p replacing locked wrapper %p after FSR", queue,
                    g_PostSLLockedQueue);
            }
        } else if (ce::dx12_overlay_policy::ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(
                       keepAliveRenderAfterExplicitOff, exactExplicitOffKeepAliveSwapchain,
                       g_PostSLLastWorkingQueue != nullptr)) {
            queue = g_PostSLLastWorkingQueue;
            static std::atomic<int> s_exactOffKeepAliveLastWorkingQueueLogCount{0};
            const int logCount = s_exactOffKeepAliveLastWorkingQueueLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: PostSL exact-proxy explicit-OFF keep-alive selecting last successful direct queue %p "
                    "ahead of locked queue %p (sc=%p log=%d)",
                    queue, g_PostSLLockedQueue, pSwapChain, logCount + 1);
            }
        } else if (g_PostSLLockedQueue) {
            queue = g_PostSLLockedQueue;
        } else if (resumeOnValidatedLastWorkingQueue) {
            queue = g_PostSLLastWorkingQueue;
            static int s_postFSRResumeQueueLog = 0;
            if (s_postFSRResumeQueueLog++ < 10) {
                HookLogImportant(
                    "DX12: PostSL queue — reusing validated lastWorking queue %p for resumed DLSS activation during "
                    "post-FSR inactive recovery (origGame=%p explicit=%d safeBootstrap=%d)",
                    queue, g_OriginalGameQueue, explicitSetOptionsActivation ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0);
            }
        } else if (slFGNow) {
            if (preferRealQueueBehindWrapper) {
                queue = directQueueBehindWrapper;
                static int s_realQueueLog = 0;
                if (s_realQueueLog++ < 5) {
                    HookLog("DX12: PostSL queue — realQueueBehindWrapper %p (scQueue=%p hadFSR=%d)", queue, scQueue,
                            g_HadFSRFGPhase ? 1 : 0);
                }
            } else if (allowWrapperBootstrapQueue && wrapperBootstrapQueue &&
                       wrapperBootstrapQueue != g_OriginalGameQueue && wrapperBootstrapQueue != scQueue) {
                queue = wrapperBootstrapQueue;
                static int s_wrapperBootstrapLog = 0;
                if (s_wrapperBootstrapLog++ < 10) {
                    HookLogImportant(
                        "DX12: PostSL queue — wrapper bootstrap %p (validatedCmdQ=%p latestWrapper=%p scQueue=%p "
                        "hadFSR=%d)",
                        queue, validatedCommandQueue, latestSLWrapperQueue, scQueue, g_HadFSRFGPhase ? 1 : 0);
                }
            } else if (scQueue && scQueue != g_OriginalGameQueue) {
                if (g_HadFSRFGPhase) {
                    HookLogImportant(
                        "DX12: PostSL queue — WARNING: falling back to scQueue %p in post-FSR DLSS path "
                        "(origGame=%p, hadFSR=%d, no wrapper/direct queue available)",
                        scQueue, g_OriginalGameQueue, g_HadFSRFGPhase ? 1 : 0);
                }
                queue = scQueue;
                static int s_scQLog = 0;
                if (s_scQLog++ < 5)
                    HookLog("DX12: PostSL queue — scQueue %p (SL swapchain, origGame=%p, hadFSR=%d)", queue,
                            g_OriginalGameQueue, g_HadFSRFGPhase ? 1 : 0);
            } else if (g_OriginalGameQueue) {
                queue = g_OriginalGameQueue;
                static int s_origLog = 0;
                if (s_origLog++ < 5)
                    HookLog("DX12: PostSL queue — origGame %p (slFG, hadFSR=%d)", queue, g_HadFSRFGPhase ? 1 : 0);
            }
        } else if (scQueue) {
            queue = scQueue;
        } else if (g_OriginalGameQueue) {
            queue = g_OriginalGameQueue;
        } else if (g_PreFGGameQueue) {
            queue = g_PreFGGameQueue;
        } else {
            queue = g_CommandQueue.load(std::memory_order_acquire);
        }

        // AddRef the selected queue under the mutex to prevent it from being
        // freed by DX12_SetCommandQueue (which also uses this mutex) or SL's
        // internal cleanup while we use it.  Released by scope guard below.
        if (queue)
            queue->AddRef();
    }
    // Scope guard ensures Release on all exit paths
    auto queueReleaseGuard = ce::make_scope_guard([&]() {
        if (queue)
            queue->Release();
    });

    if (!queue) {
        static int s_noQueue = 0;
        if (s_noQueue++ < 5)
            HookLog("DX12: PostSL SKIP — no queue (cmdQueue=%p scQueue=%p)", (void*)g_CommandQueue.load(),
                    g_SwapchainQueue);
        return;
    }

    // Lock to the selected queue for the current epoch, but allow a one-time
    // post-FSR migration from the wrapper bootstrap queue to the captured real
    // queue behind it once the ECL detour has observed that path.
    {
        ID3D12CommandQueue* oldLockedQueue = nullptr;
        bool lockedQueueWasUpdated = false;
        bool shouldKeepExistingLockedQueue = false;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            ID3D12CommandQueue* directQueueBehindWrapper = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
            ExecuteCommandListsPtr currentRealECL = g_RealD3D12ECL.load(std::memory_order_acquire);
            ExecuteCommandListsPtr lockedScQueueOrigECL = scQueue ? GetOriginalExecuteCommandLists(scQueue) : nullptr;
            const bool lockedQueueIsSLWrapper =
                g_PostSLLockedQueue && g_PostSLLockedQueue != g_OriginalGameQueue && g_PostSLLockedQueue != scQueue;
            const bool hasSwapchainQueueSubmitPath =
                scQueue && (lockedScQueueOrigECL != nullptr || currentRealECL != nullptr);
            const bool hasWrapperDerivedDirectPath = directQueueBehindWrapper != nullptr && currentRealECL != nullptr;
            bool shouldReplaceLockedQueue =
                ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(
                    g_HadFSRFGPhase, cachedSLFGActive, lockedQueueIsSLWrapper, directQueueBehindWrapper != nullptr) &&
                queue == directQueueBehindWrapper;
            shouldReplaceLockedQueue =
                shouldReplaceLockedQueue ||
                (ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
                     g_PostSLLockedQueue != nullptr, g_HadFSRFGPhase, cachedSLFGActive, lockedQueueIsSLWrapper,
                     scQueue != nullptr, scQueue != g_OriginalGameQueue, hasSwapchainQueueSubmitPath,
                     hasWrapperDerivedDirectPath) &&
                 queue == scQueue);
            shouldReplaceLockedQueue =
                shouldReplaceLockedQueue ||
                (ce::dx12_overlay_policy::ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(
                     keepAliveRenderAfterExplicitOff, exactExplicitOffKeepAliveSwapchain,
                     g_PostSLLastWorkingQueue != nullptr) &&
                 queue == g_PostSLLastWorkingQueue);
            const bool selectedQueueMatchesLockedQueue = queue == g_PostSLLockedQueue;

            if (ce::dx12_overlay_policy::ShouldMutatePostSLLockedQueue(
                    g_PostSLLockedQueue != nullptr, selectedQueueMatchesLockedQueue, shouldReplaceLockedQueue)) {
                oldLockedQueue = g_PostSLLockedQueue;
                g_PostSLLockedQueue = queue;
                queue->AddRef();  // prevent locked queue from being freed between PostSL calls
                lockedQueueWasUpdated = true;

                if (oldLockedQueue) {
                    if (queue == directQueueBehindWrapper) {
                        HookLogImportant(
                            "DX12: PostSL promoting locked queue %p -> real queue behind wrapper %p after post-FSR "
                            "bootstrap",
                            oldLockedQueue, directQueueBehindWrapper);
                    } else if (exactExplicitOffKeepAliveSwapchain && queue == g_PostSLLastWorkingQueue) {
                        HookLogImportant(
                            "DX12: PostSL replacing stale locked queue %p -> retained exact-proxy queue %p for "
                            "explicit-OFF keep-alive",
                            oldLockedQueue, queue);
                    } else {
                        HookLogImportant(
                            "DX12: PostSL replacing locked queue %p -> swapchain queue %p after post-FSR direct path "
                            "recovery",
                            oldLockedQueue, queue);
                    }
                } else {
                    bool usingSLWrapper = (queue != g_OriginalGameQueue && queue != scQueue);
                    bool slFGAtLock = cachedSLFGActive;
                    HookLogImportant(
                        "DX12: PostSL locked to queue %p (origGame=%p scQueue=%p cmdQueue=%p preFG=%p epoch=%d "
                        "slWrapper=%d slFG=%d hadFSR=%d)",
                        queue, g_OriginalGameQueue, scQueue, (void*)g_CommandQueue.load(), g_PreFGGameQueue,
                        s_reactivationEpoch, usingSLWrapper ? 1 : 0, slFGAtLock ? 1 : 0, g_HadFSRFGPhase ? 1 : 0);
                }
            } else if (!selectedQueueMatchesLockedQueue) {
                shouldKeepExistingLockedQueue = true;
                queue->Release();  // Release per-call AddRef on the rejected queue
                queue = g_PostSLLockedQueue;
                if (queue) {
                    queue->AddRef();  // Per-call AddRef on the locked queue instead
                }
            }
        }
