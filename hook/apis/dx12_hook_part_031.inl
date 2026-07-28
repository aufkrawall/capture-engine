                }

                // Flush any pending deferred signal immediately so GPU work from the
                // previous frame completes before SL reconfigures.
                UINT64 deferredVal = g_deferredSignalValue.load(std::memory_order_acquire);
                if (deferredVal != 0 && g_State.fence) {
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
            }
        }
        bool skipOverlayDraw = false;
        if (holdFocusLossBackbufferWork) {
            skipOverlayDraw = true;
            NoteDX12OverlayCoverageGate("focus-loss-hold");
        }
        if (g_FGTransitionCooldown > 0) {
            // PRINCIPLE: never blank a live overlay. When the backend is live and
            // the normal route is its transport (OFF / no-callback FSR), the
            // transition only retargets the submit queue (auto-resolved per
            // frame on device-level sync resources) — the draw cooldown is pure
            // gratuitous suppression, so keep drawing this very frame.
            const bool appCallbackBridgeFSRActive =
                g_FGCompat.IsFSRFGApiActive() &&
                !g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
            const bool keepDrawingLiveOverlayThroughCooldown =
                ce::dx12_overlay_policy::ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown(
                    g_State.overlayInit, g_State.syncInit, protectedOfficialFFXStartupOverlayOnly, currentSLFGRunning,
                    appCallbackBridgeFSRActive);
            if (keepDrawingLiveOverlayThroughCooldown) {
                static std::atomic<int> s_keepDrawingLiveOverlayLogCount{0};
                const int logCount = s_keepDrawingLiveOverlayLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: Keeping live overlay drawing through FG transition cooldown — no blank "
                        "(oldCooldown=%d queue=%p scQueue=%p fsrApi=%d noCallback=%d log=%d)",
                        g_FGTransitionCooldown.load(std::memory_order_acquire), gameQueue, transitionSwapchainQueue,
                        g_FGCompat.IsFSRFGApiActive() ? 1 : 0,
                        g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire) ? 1 : 0, logCount + 1);
                }
                // The transition bookkeeping (GPU drain, queue/heuristic resets)
                // already ran in the arming block; only the multi-frame draw
                // suppression is removed. PostSL is not the transport here, so
                // leave its state untouched.
                g_FGTransitionCooldown.store(0, std::memory_order_release);
                // A stale scene-transition cooldown (e.g. armed during the prior FSR
                // phase) must never blank the live overlay C1 just decided to keep
                // drawing — clearing it here is the safety net for the phantom-arming
                // path (session 20260613_202646, 14-present FSR->OFF blank).
                if (g_SceneTransitionCooldown.load(std::memory_order_acquire) > 0) {
                    g_SceneTransitionCooldown.store(0, std::memory_order_release);
                    HookLogImportant(
                        "DX12: Cleared stale scene-transition cooldown at FG transition keep-drawing edge — "
                        "live overlay is never blanked");
                }
                NoteDX12OverlayCoverageGate("live-overlay-kept-drawing");
            } else {
                g_FGTransitionCooldown.fetch_sub(1, std::memory_order_acq_rel);
                const bool preserveActivePostSLDuringCooldown =
                    ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(
                        currentSLFGRunning, g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                        HookIsPostSLOverlayActiveButUnconfirmed());
                if (preserveActivePostSLDuringCooldown) {
                    // Synthetic startup can already have an active PostSL path before
                    // the game-thread cooldown has fully counted down. Do not let the
                    // slower ProcessFrame cooldown re-disable that same path and force
                    // a second reactivation/warm-up epoch before first confirmation.
                    g_PostSLOverlayActive.store(true, std::memory_order_release);
                    g_PostSLCooldownRemaining.store(0, std::memory_order_release);

                    static int s_preserveActivePostSLLog = 0;
                    if (s_preserveActivePostSLLog < 10 || g_FGTransitionCooldown == 0) {
                        HookLogImportant(
                            "DX12: FG cooldown preserving active PostSL path "
                            "(remaining=%d slSignal=%d confirmed=%d unconfirmed=%d)",
                            g_FGTransitionCooldown.load(std::memory_order_acquire), currentSLFGRunning ? 1 : 0,
                            g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
                            HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0);
                    }
                    s_preserveActivePostSLLog++;
                } else {
                    // During cooldown, suppress BOTH pre-SL and post-SL rendering.
                    g_PostSLOverlayActive.store(false, std::memory_order_release);
                    g_PostSLCooldownRemaining.store(g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                    std::memory_order_release);
                }

                // Periodic device-removed check during cooldown to pinpoint
                // when the device dies (overlay is NOT rendering during this time).
                if ((g_FGTransitionCooldown % 10) == 0) {
                    auto* cooldownDev = g_Device.load(std::memory_order_acquire);
                    if (cooldownDev) {
                        HRESULT cooldownDevHr = cooldownDev->GetDeviceRemovedReason();
                        if (FAILED(cooldownDevHr)) {
                            HookLogImportant(
                                "DX12: DEVICE REMOVED DURING COOLDOWN (cooldown=%d devRemoved=0x%08X tid=0x%04X)",
                                g_FGTransitionCooldown.load(std::memory_order_acquire), (unsigned)cooldownDevHr,
                                GetCurrentThreadId());
                        }
                    }
                }

                NoteDX12OverlayCoverageGate("fg-transition-cooldown");
                if (g_FGTransitionCooldown == 0) {
                    auto fgType = g_FGCompat.GetActiveFGType();
                    bool slFG = currentFGActive && DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                    HookLogImportant(
                        "DX12: FG transition cooldown complete — resuming overlay (slFG=%d, fgType=%s, slSignal=%d)",
                        slFG ? 1 : 0, g_FGCompat.GetFGTypeName(fgType),
                        DXGIShared::g_StreamlineFGRunning.load() ? 1 : 0);
                    // Re-enable post-SL rendering if SL FG is active
                    if (slFG) {
                        const bool preserveSyntheticStartupState =
                            ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                    std::memory_order_acquire),
                                HookIsPostSLOverlayActiveButUnconfirmed(),
                                g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                HookIsPostSLOverlayConfirmedButStartupSettling());
                        const bool keepStartupHandoffPending = ce::dx12_overlay_policy::
                            ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                    std::memory_order_acquire),
                                HookIsPostSLOverlayActiveButUnconfirmed(),
                                g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                HookIsPostSLOverlayConfirmedButStartupSettling());
                        g_PostSLOverlayActive.store(true, std::memory_order_release);
                        if (!preserveSyntheticStartupState) {
                            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(
                                false, std::memory_order_release);
                            g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                            DXGIShared::ResetStreamlineStartupTransitionState();
                        }
                        g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                        DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                        std::memory_order_release);
                    }
                }
                skipOverlayDraw = true;
            }
        }

        // POST-SL overlay rendering during SL FG:
        //
        // Why post-SL: Pre-SL rendering submits ECLs on the game queue before SL
        // processes Present.  This crashes at ~600-770 frames because the extra ECL
        // perturbs SL's frame generation pipeline (confirmed by binary search: empty
        // ECL=no crash, drawing ECL=crash, regardless of barriers/fences).
        //
        // Post-SL rendering submits the ECL in the re-entrant Present callback —
        // after SL's FG work but before the real Present flip.  Post-SL empty ECL
        // was proven stable (∞ frames).  The overlay draws to the backbuffer that SL
        // is about to present, using implicit state promotion (no explicit barriers).
        // Real D3D12 ECL bypasses all hooks.  No fence signal.
        // SL captures our overlay as part of the scene, FG interpolates it naturally.
        // Overlay appears on all output frames (real + interpolated).
        const bool slFGActive = currentFGActive && DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        // x86 text is emitted as solid glyph spans by the DX12 backend.  That
        // keeps no-FG rendering on the fast native direct path; the independent
        // post-FSR offscreen path below remains only for the existing FG handoff
        // state where backbuffer ownership/state can be indeterminate.
        {
            if (slFGActive) {
                // SL FG active: enable POST-SL overlay rendering.
                // The overlay ECL is submitted in the re-entrant Present callback
                // AFTER SL's FG processing but BEFORE the real Present call.
                // This avoids submitting extra ECLs before SL processes the frame
                // (which caused crashes at ~600-770 frames with pre-SL rendering).
                //
                // EXCEPTION: After FSR→DLSS transition, PostSL rendering causes
                // DEVICE_HUNG because the backbuffer resource state is invalid from
                // any queue we have (FSR created the swapchain, SL resized it).
                // In this case, keep rendering pre-SL: the overlay is rendered
                // BEFORE SL's FG pipeline processes the frame, so origGame's state
                // tracking is still valid (game just finished rendering on it).

                // CRITICAL FIX: PostSL rendering now works for pure DLSS FG
                // (no prior FSR) because IsRecursivePresent() correctly treats
                // SL's cross-thread FG Presents as re-entrant.
                //
                // For post-FSR DLSS FG, we use pre-SL rendering instead.
                // PostSL has irreconcilable cross-queue issues: SL's FG pipeline
                // uses its own internal queue, but the swapchain was created by
                // FSR on scQueue.  All queue options (scQueue, origGame, SL
                // wrapper) cause DEVICE_HUNG from cross-queue backbuffer conflicts.
                // POST-SL overlay for ALL SL FG modes (pure DLSS and post-FSR DLSS).
                //
                // Uses origGame queue: SL routes everything through origGame
                // (via its COM wrapper).  PostSL fires in the re-entrant Present
                // path AFTER SL's FG processing is complete.  The backbuffer is
                // in PRESENT state on origGame — same as pure DLSS.
                //
                // Previously we disabled PostSL for post-FSR and used pre-SL,
                // but SL intercepts the game thread's Present at the COM wrapper
                // level during DLSS FG — only SL's worker threads reach our
                // detour.  PostSL (in the re-entrant path) is the only reliable
                // rendering timing for DLSS FG.
                {
                    // Step 1: Register callback (idempotent)
                    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
                        &PostSLOverlayRenderGated) {
                        SetPostSLCallbackInstalled(true, "DX12: SL FG active");
                        HookLogImportant("DX12: SL FG active - registered POST-SL overlay callback (hadFSR=%d)",
                                         g_HadFSRFGPhase ? 1 : 0);
                    }

                    // Step 2: Activate PostSL rendering
                    if (!skipOverlayDraw) {
                        if (!g_PostSLOverlayActive.load(std::memory_order_acquire)) {
                            const bool preserveSyntheticStartupState =
                                ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                        std::memory_order_acquire),
                                    HookIsPostSLOverlayActiveButUnconfirmed(),
                                    g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                    HookIsPostSLOverlayConfirmedButStartupSettling());
                            const bool keepStartupHandoffPending = ce::dx12_overlay_policy::
                                ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                        std::memory_order_acquire),
                                    HookIsPostSLOverlayActiveButUnconfirmed(),
                                    g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                    HookIsPostSLOverlayConfirmedButStartupSettling());
                            g_PostSLOverlayActive.store(true, std::memory_order_release);
                            if (!preserveSyntheticStartupState) {
                                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(
                                    false, std::memory_order_release);
                                g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                            }
                            g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                            std::memory_order_release);
                            if (!preserveSyntheticStartupState) {
                                DXGIShared::ResetStreamlineStartupTransitionState();
                            }
                            HookLogImportant("DX12: SL FG active - activated POST-SL overlay rendering (hadFSR=%d)",
                                             g_HadFSRFGPhase ? 1 : 0);
                        }
                    } else {
                        const bool preserveActivePostSL =
                            ce::dx12_overlay_policy::ShouldPreserveActivePostSLWhenPreSLDrawIsSkipped(
                                currentSLFGRunning, g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                HookIsPostSLOverlayActiveButUnconfirmed());
                        if (preserveActivePostSL) {
                            static int s_preservePostSLOnSkippedPreSLDrawLog = 0;
                            if (s_preservePostSLOnSkippedPreSLDrawLog < 10 ||
                                (s_preservePostSLOnSkippedPreSLDrawLog % 200) == 0) {
                                HookLogImportant(
                                    "DX12: Preserving active PostSL while pre-SL draw is skipped "
                                    "(confirmed=%d unconfirmed=%d skip=%d)",
                                    g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
                                    HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0,
                                    s_preservePostSLOnSkippedPreSLDrawLog + 1);
                            }
                            s_preservePostSLOnSkippedPreSLDrawLog++;
                        } else if (g_PostSLOverlayActive.load(std::memory_order_acquire)) {
                            g_PostSLOverlayActive.store(false, std::memory_order_release);
                        }
                    }
                    ExecuteCommandListsPtr currentRealECL = g_RealD3D12ECL.load(std::memory_order_acquire);
                    ExecuteCommandListsPtr selectedQueueOrigECL = GetOriginalExecuteCommandLists(gameQueue);
                    const bool keepPostSLWithoutRealECL =
                        ce::dx12_overlay_policy::ShouldKeepPostSLActiveWhenRealECLUnavailable(
                            currentRealECL != nullptr, selectedQueueOrigECL != nullptr,
                            g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                            HookIsPostSLOverlayActiveButUnconfirmed());
                    if (!keepPostSLWithoutRealECL) {
                        static bool s_noRealECLLogged = false;
                        if (!s_noRealECLLogged) {
                            s_noRealECLLogged = true;
                            HookLogImportant("DX12: No real D3D12 ECL available - disabling overlay during SL FG");
                        }
                        g_PostSLOverlayActive.store(false, std::memory_order_release);
                    } else if (!currentRealECL) {
                        static std::atomic<int> s_keepPostSLWithoutRealECLLogCount{0};
                        const int logCount = s_keepPostSLWithoutRealECLLogCount.fetch_add(1, std::memory_order_relaxed);
                        if (logCount < 10 || (logCount % 240) == 0) {
                            HookLogImportant(
                                "DX12: Keeping PostSL active without realECL "
                                "(queue=%p origECL=%p confirmed=%d unconfirmed=%d log=%d)",
                                gameQueue, (void*)selectedQueueOrigECL,
                                g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
                                HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0, logCount + 1);
                        }
                    }
                    if (g_PostSLConfirmedRendering.load(std::memory_order_acquire)) {
                        // FG "SUSPENSION" STALL DETECTION:
                        //
                        // PostSL was previously confirmed rendering, but it may have
                        // stalled.  This happens when:
                        //   1. DLSS FG is "nominally on" (g_StreamlineFGRunning=true,
                        //      slDLSSGSetOptions was NOT called with mode=0)
                        //   2. But SL stops generating re-entrant Present calls
                        //      (game menu, pause, loading screen)
                        //
                        // In this state, BOTH rendering paths are blocked:
                        //   - Pre-SL: suppressed by skipOverlayDraw (PostSL confirmed)
                        //   - PostSL: never fires (no re-entrant Present from SL)
                        //
                        // FIX: Count consecutive Present calls without PostSL firing.
                        // PostSLOverlayRender resets g_PostSLStallCounter to 0 on
                        // each successful render.  After kPostSLStallThreshold frames
                        // without a reset, allow pre-SL as fallback.
                        //
                        // WARMUP GUARD: The stall fallback is ONLY safe when SL's FG
                        // pipeline is genuinely idle (suspension).  During FG warmup
                        // (just after OFF→ON or re-confirmation), SL's pipeline is
                        // actively processing, and pre-SL ECLs on origGame cause
                        // DEVICE_HUNG.  g_PostSLStableFrameCount tracks consecutive
                        // PostSL frames since the last FG transition.  Fallback is
                        // only enabled after kPostSLWarmupThreshold frames of stable
                        // PostSL rendering, proving the FG pipeline is fully operational.
                        //
                        // TESTED: GTA V Enhanced (menu pauses FG), Talos Reawakened
                        // (continuous FG — stall never triggers during normal play).
                        constexpr int kPostSLStallThreshold = 5;
                        const int kPostSLWarmupThreshold =
                            ce::dx12_overlay_policy::GetConfirmedPostSLWarmupProofFrameThreshold();
                        int stableFrames = g_PostSLStableFrameCount.load(std::memory_order_acquire);
                        int stallCount = g_PostSLStallCounter.fetch_add(1, std::memory_order_acq_rel) + 1;

                        if (stableFrames < kPostSLWarmupThreshold) {
                            // FG pipeline still warming up — don't fall back to pre-SL.
                            // Just skip rendering until PostSL stabilizes.
                            skipOverlayDraw = true;
                            if (stableFrames > 0 && stallCount > kPostSLStallThreshold &&
                                (stallCount == kPostSLStallThreshold + 1 || (stallCount % 30) == 0)) {
                                const bool serviced = DX12_TryInvokePostSLStartupActivationCallback(
                                    "DX12::PostSL warmup stall service", false, true);
                                static int s_warmupServiceLog = 0;
                                if (serviced || s_warmupServiceLog < 10 || (s_warmupServiceLog % 100) == 0) {
                                    HookLogImportant(
                                        "DX12: PostSL warmup stall service %s "
                                        "(stableFrames=%d stallCount=%d threshold=%d serviceLog=%d)",
                                        serviced ? "rendered via retained callback" : "could not run", stableFrames,
                                        stallCount, kPostSLWarmupThreshold, s_warmupServiceLog + 1);
                                }
                                ++s_warmupServiceLog;
                            }
                            static int s_warmupSuppressLog = 0;
                            if (s_warmupSuppressLog++ < 5 || (s_warmupSuppressLog % 200) == 0) {
                                HookLogImportant(
                                    "DX12: PostSL warmup — suppressing stall fallback "
                                    "(stableFrames=%d stallCount=%d threshold=%d) #%d",
                                    stableFrames, stallCount, kPostSLWarmupThreshold, s_warmupSuppressLog);
                            }
                        } else if (stallCount <= kPostSLStallThreshold) {
                            skipOverlayDraw = true;  // PostSL recently active — suppress pre-SL
                        } else {
                            // PostSL has stalled — SL FG is nominally on but not generating
                            // frames.  Allow pre-SL rendering as fallback.
                            static int s_stallFallbackLog = 0;
                            if (s_stallFallbackLog < 10 || (s_stallFallbackLog % 200) == 0) {
                                HookLogImportant(
                                    "DX12: PostSL stalled (%d frames, stableFrames=%d) — falling back to pre-SL "
                                    "rendering #%d",
                                    stallCount, stableFrames, s_stallFallbackLog);
                            }
                            s_stallFallbackLog++;
                            // Don't skip pre-SL draw — it will render the overlay
                        }
                    }
                }
            } else {
                // SL FG not active (FSR FG, no FG, suspension, etc.): render pre-SL.
                // Make-before-break: while a CONFIRMED-PostSL suspension keep-alive
                // is active (DLSS SetOptions(off) churn / menu suspend), keep the
                // callback installed and the confirmed flag set. The normal route
                // still draws this frame (the overlay stays visible during the
                // suspension), and the rapid re-ON then takes the warm-resume path
                // instead of a fresh reactivation epoch — eliminating the 1-present
                // PostSL reactivation/probe gap on every SL-signal churn cycle
                // (session 20260613_041204). PostSLOverlayRenderGated retires the
                // keep-alive on genuine teardown (Streamline unload / swapchain
                // invalidation), and SetPostSLCallbackInstalled(false) on any
                // authoritative disable still clears it.
                if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr &&
                    !g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {
                    SetPostSLCallbackInstalled(false, "DX12: pre-SL fallback");
                    g_PostSLOverlayActive.store(false, std::memory_order_release);
                    // Reset PostSL confirmed flag so pre-SL rendering resumes immediately
                    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                    g_PostSLStallCounter.store(0, std::memory_order_release);
                    g_PostSLStableFrameCount.store(0, std::memory_order_release);
                    g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
                    HookLogImportant("DX12: Disabled post-SL callback — rendering pre-SL in ProcessFrame (fgType=%s)",
                                     g_FGCompat.GetFGTypeName(g_FGCompat.GetActiveFGType()));
                } else if (g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {
                    static std::atomic<int> s_preSLFallbackKeepAliveLogCount{0};
                    const int logCount = s_preSLFallbackKeepAliveLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 10 || (logCount % 300) == 0) {
                        HookLogImportant(
                            "DX12: pre-SL fallback rendering normal route during confirmed-PostSL suspension "
                            "keep-alive — callback stays installed for warm re-ON (log=%d)",
                            logCount + 1);
                    }
                }
            }
        }

        // Periodic routing state diagnostic (every 300 frames)
        {
            static uint64_t s_routingFrameCount = 0;
            ++s_routingFrameCount;
            if ((s_routingFrameCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Routing state: frame=%llu fgActive=%d slFGActive=%d slSignal=%d "
                    "cooldown=%d sceneCool=%d postSLCallback=%d postSLActive=%d skip=%d stallCount=%d stableFrames=%d "
                    "runtime=%s",
                    s_routingFrameCount, currentFGActive ? 1 : 0, slFGActive ? 1 : 0, currentSLFGRunning ? 1 : 0,
                    g_FGTransitionCooldown.load(std::memory_order_acquire),
                    g_SceneTransitionCooldown.load(std::memory_order_relaxed),
                    DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr ? 1 : 0,
                    g_PostSLOverlayActive.load(std::memory_order_relaxed) ? 1 : 0, skipOverlayDraw ? 1 : 0,
                    g_PostSLStallCounter.load(std::memory_order_relaxed),
                    g_PostSLStableFrameCount.load(std::memory_order_relaxed),
                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
            }
        }

        // Scene transition cooldown: detect large frametime gaps (loading screens,
        // scene changes) and skip overlay rendering briefly.  This runs BEFORE
        // the skipOverlayDraw check so it works for both pre-SL (normal) and
        // post-SL (SL FG) overlay paths.
        {
            static LARGE_INTEGER s_lastProcessFrameTime = {};
            static bool s_lastSceneBlockSuppressedRoute = false;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);

            // While the overlay is presented via a runtime-owned / FSR-callback / PostSL
            // route, this scene block runs at a reduced cadence, so its delta is a
            // measurement artifact, not a real stall. Do not arm on such a route, and
            // discard any gap that SPANS such a route (previous run was suppressed) so the
            // first normal-route frame after the route change can't false-arm either.
            const bool runtimeOwnedOverlayRoute =
                ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute(
                    g_FGRuntimeOwnsSwapchain, g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
                    ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup());

            if (s_lastProcessFrameTime.QuadPart != 0 && !runtimeOwnedOverlayRoute && !s_lastSceneBlockSuppressedRoute) {
                LARGE_INTEGER freq;
                QueryPerformanceFrequency(&freq);
                double deltaMs =
                    (double)(now.QuadPart - s_lastProcessFrameTime.QuadPart) * 1000.0 / (double)freq.QuadPart;

                const bool startupActivationPending =
                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
                const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
                const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
                const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
                const bool suppressSceneCooldownForSyntheticStartup =
                    ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(
                        currentSLFGRunning, startupActivationPending, postSLActiveButUnconfirmed,
                        postSLConfirmedRendering, postSLConfirmedButStartupSettling);
                const bool suppressSceneCooldownForStablePostSLGap =
                    ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForStablePostSLGap(
                        currentSLFGRunning, postSLConfirmedRendering, g_PostSLLastWorkingQueue != nullptr,
                        DXGIShared::g_SharedState.swapchainInvalid.load(std::memory_order_acquire),
                        g_DeviceRemoved.load(std::memory_order_acquire));

                if (deltaMs > 1000.0 && currentFGActive) {
                    if (suppressSceneCooldownForSyntheticStartup) {
                        static std::atomic<int> s_syntheticStartupSceneCooldownSkipLogCount{0};
                        const int logCount =
                            s_syntheticStartupSceneCooldownSkipLogCount.fetch_add(1, std::memory_order_relaxed);
                        if (logCount < 10 || (logCount % 100) == 0) {
                            HookLogImportant(
                                "DX12: Suppressing scene transition cooldown during half-armed synthetic PostSL "
                                "startup "
                                "(gap=%.0fms pending=%d unconfirmed=%d confirmed=%d settling=%d)",
                                deltaMs, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                                postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0);
                        }
                    } else if (suppressSceneCooldownForStablePostSLGap) {
                        static std::atomic<int> s_stablePostSLSceneCooldownSkipLogCount{0};
                        const int logCount =
                            s_stablePostSLSceneCooldownSkipLogCount.fetch_add(1, std::memory_order_relaxed);
                        if (logCount < 10 || (logCount % 100) == 0) {
                            HookLogImportant(
                                "DX12: Suppressing scene transition cooldown after stable PostSL gap "
                                "(gap=%.0fms lastWorkingQ=%p)",
                                deltaMs, g_PostSLLastWorkingQueue);
                        }
                    } else {
                        int cooldown = 30;
                        g_SceneTransitionCooldown.store(cooldown, std::memory_order_release);
                        HookLogImportant(
                            "DX12: Scene transition detected (gap=%.0fms) during FG — overlay cooldown %d frames",
                            deltaMs, cooldown);
                    }
                }
            } else if (runtimeOwnedOverlayRoute && s_lastProcessFrameTime.QuadPart != 0) {
                static std::atomic<int> s_runtimeRouteSceneCooldownSuppressLogCount{0};
                const int logCount =
                    s_runtimeRouteSceneCooldownSuppressLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: Suppressing scene transition cooldown arming on runtime-owned/FSR-callback overlay "
                        "route (cadence artifact, not a stall) — fsrApi=%d runtimeOwns=%d log=%d",
                        g_FGCompat.IsFSRFGApiActive() ? 1 : 0, g_FGRuntimeOwnsSwapchain ? 1 : 0, logCount + 1);
                }
            }
            s_lastProcessFrameTime = now;
            s_lastSceneBlockSuppressedRoute = runtimeOwnedOverlayRoute;
        }

        if (captureBeforeOverlay) {
            int64_t captureStartUs = PerfLogger::GetQpcUs();
            PublishDX12CapturedFrame(pSwapChain, captureShm, gameQueue, hasCurrentBackBufferIdx, currentBackBufferIdx);
            const int64_t captureUs = PerfLogger::GetQpcUs() - captureStartUs;
            perfMetrics.captureUs = static_cast<int32_t>(captureUs);
            if (diagnostics) {
                diagnostics->captureUs += captureUs;
            }
        }

        if (!skipOverlayDraw) {
            const char* skipSeparateOverlayGpuReason = nullptr;
            if (ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(&skipSeparateOverlayGpuReason)) {
                static std::atomic<int> s_runtimeOwnedOverlayDrawSkipLogCount{0};
                int logCount = s_runtimeOwnedOverlayDrawSkipLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: Skipping separate overlay GPU draw because %s is active "
                        "(runtime=%s scQueue=%p origGame=%p cmdQ=%p postSL=%d)",
                        skipSeparateOverlayGpuReason ? skipSeparateOverlayGpuReason : "runtime-owned swapchain",
                        ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), g_SwapchainQueue,
                        g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire),
                        g_PostSLOverlayActive.load(std::memory_order_acquire) ? 1 : 0);
                }
                goto skip_overlay_draw;
            }

            // PRE-SL RENDERING GATE — controls when pre-SL overlay is suppressed during SL FG.
            //
            // Two suppression points, both with stall fallback:
            //
            // 1. HERE (render site): Suppresses when SL FG is on and PostSL hasn't
            //    confirmed yet.  Gives PostSL ~5 frames to fire before falling back.
            //
            // 2. ABOVE (routing logic, line ~6305): Suppresses via skipOverlayDraw when
            //    PostSL IS confirmed but the stall counter exceeds threshold.
            //
            // Both use kPostSLStallThreshold/kPreSLFallbackThreshold (same value) to
            // detect "FG suspension" (SL nominally on, but not generating frames).
            //
            // PRE-SL RENDERING DURING FG SUSPENSION:
            // When pre-SL fallback activates, the overlay renders BEFORE SL's Present
            // trampoline.  This is safe because SL's FG pipeline is idle (not generating
            // frames).  The game's Present call goes through:
            //   ProcessFrame (overlay renders) → oPresent → SL passes through → real Present
            // Resource state is correct: game transitioned BB to PRESENT before Present,
            // we do PRESENT→RT→PRESENT round-trip, then SL sees PRESENT state.
            //
            // REGRESSION RISK: In Talos Reawakened, SL FG runs continuously (no menu
            // suspension).  The stall counter should never exceed 5 during normal play
            // because PostSL fires multiple times per game Present (real + interpolated).
            // If this regresses, increase kPreSLFallbackThreshold.
            {
                bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                bool postSLActive = g_PostSLOverlayActive.load(std::memory_order_acquire);
                bool postSLConfirmed = g_PostSLConfirmedRendering.load(std::memory_order_relaxed);
                auto postSLCallback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed);
                int stallCount = g_PostSLStallCounter.load(std::memory_order_relaxed);
                constexpr int kPreSLFallbackThreshold = 5;

                if (slFGNow && !postSLConfirmed) {
                    // SL FG active, PostSL never confirmed yet (FG STARTUP, not suspension).
                    //
                    // CRITICAL: Do NOT fall back to pre-SL rendering here!
                    // During FG startup:
                    //   - SL creates a new swapchain with its own queue
                    //   - Backbuffers belong to SL's swapchain queue
                    //   - Pre-SL renders on origGame queue → cross-queue access → DEVICE_HUNG
                    //   - SL's FG pipeline hasn't started generating re-entrant Presents yet
                    //
                    // Pre-SL fallback is ONLY safe during FG SUSPENSION (PostSL was confirmed
                    // working but stopped firing — the game's backbuffer state is still valid
                    // on the game's queue because SL's FG pipeline is idle).
                    //
                    // During startup, we simply wait for PostSL to confirm. The overlay is
                    // invisible for a few frames during FG initialization. A pre-SL make-before-
                    // break here is NOT possible on the DLSS-startup path: the game's Present is
                    // consumed by Streamline's proxy, so all startup presents are SL re-entrant
                    // (PostSL) and ProcessFrameExternal is dormant — there is no pre-SL frame to
                    // draw. The only overlay route is PostSL, gated by the cold-start warmup
                    // (the documented GTA DLSS-init crash protection). See guardrails.md.
                    // Round 4: RTSS-style exception. When DLSS FG is toggled ON at runtime with NO
                    // separate Streamline queue (the present swapchain queue is still the game's own
                    // original queue), keep the already-live pre-SL overlay drawing instead of
                    // suppressing it, so the frame DLSS-G freezes on during init still carries the
                    // overlay. The overlay ECL lands on the game's own queue (same as the no-FG normal
                    // route) → no cross-queue DEVICE_HUNG, and it is NOT the PostSL re-entrant ECL the
                    // cold-start warmup protects. Opt-in (default OFF) + pure-DLSS + same-queue gated.
                    ID3D12CommandQueue* eagerSwapchainQueue = nullptr;
                    ID3D12CommandQueue* eagerOriginalGameQueue = nullptr;
                    {
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        eagerSwapchainQueue = g_SwapchainQueue;
                        eagerOriginalGameQueue = g_OriginalGameQueue;
                    }
                    const bool eagerToggleOnDraw =
                        ce::dx12_overlay_policy::ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(
                            DXGIShared::IsDlssToggleEagerOverlayEnabled(), g_HadFSRFGPhase, g_FGRuntimeOwnsSwapchain,
                            g_State.overlayInit, g_State.syncInit,
                            eagerSwapchainQueue != nullptr && eagerSwapchainQueue == eagerOriginalGameQueue);
                    if (eagerToggleOnDraw) {
                        NoteDX12OverlayCoverageGate("dlss-toggle-on-eager-presl-draw");
                        static int s_eagerToggleOnLog = 0;
                        if (s_eagerToggleOnLog++ < 10 || (s_eagerToggleOnLog % 300) == 0) {
                            HookLogImportant(
                                "DX12: Keeping pre-SL overlay live during DLSS toggle-on (RTSS-style, "
                                "scQueue==origGame=%p postSLActive=%d stallCount=%d) #%d",
                                (void*)eagerSwapchainQueue, postSLActive ? 1 : 0, stallCount, s_eagerToggleOnLog);
                        }
                        // Fall through to the normal pre-SL draw below (do NOT goto skip_overlay_draw).
                    } else {
                        static int s_preSLSuppressLog = 0;
                        if (s_preSLSuppressLog++ < 10 || (s_preSLSuppressLog % 300) == 0) {
                            HookLogImportant(
                                "DX12: Suppressing pre-SL draw during SL FG startup — waiting for PostSL "
                                "(postSLCallback=%d postSLActive=%d hadFSR=%d stallCount=%d) #%d",
                                postSLCallback ? 1 : 0, postSLActive ? 1 : 0, g_HadFSRFGPhase ? 1 : 0, stallCount,
                                s_preSLSuppressLog);
                        }
                        goto skip_overlay_draw;
                    }
                }
            }

            // Check scene transition cooldown for pre-SL path
            {
                int cd = g_SceneTransitionCooldown.load(std::memory_order_acquire);
                if (cd > 0) {
                    g_SceneTransitionCooldown.store(cd - 1, std::memory_order_release);
                    if (cd == 1)
