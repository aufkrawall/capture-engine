                        HookLogImportant("DX12: [outer] SL FG ON — probed realECL=%p (dev=%p)", probed, dev);
                    } else {
                        g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                        HookLogImportant("DX12: [outer] SL FG ON — deferred ECL probe (startup window active, dev=%p)",
                                         dev);
                    }
                } else {
                    HookLogImportant("DX12: [outer] SL FG ON — skipped ECL probe (dev=%p, SL=%d)", dev,
                                     IsStreamlineLoaded() ? 1 : 0);
                }
            }
        }

        // Cooldown countdown — must always tick even when overlay blocked
        if (g_FGTransitionCooldown > 0 && !allowOverlayRender) {
            // Only decrement here when the inner block won't run.
            // The inner block (inside allowOverlayRender gate) has its own
            // countdown logic.  Avoid double-decrementing.
            g_FGTransitionCooldown.fetch_sub(1, std::memory_order_acq_rel);
            const bool preserveActivePostSLDuringBlockedCooldown =
                ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(
                    outerSLFGRunning, g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                    HookIsPostSLOverlayActiveButUnconfirmed());
            if (!preserveActivePostSLDuringBlockedCooldown) {
                g_PostSLOverlayActive.store(false, std::memory_order_release);
            }
            g_PostSLCooldownRemaining.store(g_FGTransitionCooldown.load(std::memory_order_acquire),
                                            std::memory_order_release);
            if (g_FGTransitionCooldown == 0) {
                HookLogImportant("DX12: [outer] FG transition cooldown complete (slFG=%d)", outerSLFGRunning ? 1 : 0);
                if (outerSLFGRunning) {
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
                    g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                    std::memory_order_release);
                    if (!preserveSyntheticStartupState) {
                        g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                        DXGIShared::ResetStreamlineStartupTransitionState();
                    }
                }
            }
        }

        // PostSL callback management — register when SL FG active, even if overlay blocked
        if (outerSLFGRunning && g_FGTransitionCooldown == 0) {
            if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
                &PostSLOverlayRenderGated) {
                const bool preserveSyntheticStartupState =
                    ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                            std::memory_order_acquire),
                        HookIsPostSLOverlayActiveButUnconfirmed(),
                        g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                        HookIsPostSLOverlayConfirmedButStartupSettling());
                const bool keepStartupHandoffPending =
                    ce::dx12_overlay_policy::ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                            std::memory_order_acquire),
                        HookIsPostSLOverlayActiveButUnconfirmed(),
                        g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                        HookIsPostSLOverlayConfirmedButStartupSettling());
                SetPostSLCallbackInstalled(true, "DX12: [outer] Registered PostSL callback");
                g_PostSLOverlayActive.store(true, std::memory_order_release);
                if (!preserveSyntheticStartupState) {
                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false,
                                                                                            std::memory_order_release);
                    g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                }
                g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                std::memory_order_release);
                if (!preserveSyntheticStartupState) {
                    DXGIShared::ResetStreamlineStartupTransitionState();
                }
                HookLogImportant("DX12: [outer] Registered PostSL callback (overlay blocked, SL FG active)");
            }
        } else if (!outerSLFGRunning && g_FGTransitionCooldown == 0 &&
                   !g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {
            // Make-before-break: while the keep-alive latch is set, the
            // installed callback IS the coverage for the proxy's remaining
            // presents; PostSLOverlayRenderGated retires it on normal-route
            // recovery or Streamline unload.
            if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
                SetPostSLCallbackInstalled(false, "DX12: [outer] cooldown complete");
                g_PostSLOverlayActive.store(false, std::memory_order_release);
                g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
            }
        }
    }

    // [OVERLAY COVERAGE] attribute the responsible gate when this present cannot
    // reach the overlay draw section below (condition mirrors the if that follows).
    if (!(allowOverlayRender && !suspendOverlayRender && !s_insideECL && g_State.overlayInit && g_State.syncInit &&
          !delayOverlayRenderAfterResume && !delayOverlayRenderAfterSyncInit &&
          !suppressOverlayRenderForLoadedStartupOverlay && !delayOverlayRenderAfterResourcePrime &&
          !delayOverlayRenderAfterFirstDrawProbe)) {
        NoteDX12OverlayCoverageGate(!allowOverlayRender    ? "overlay-render-suppressed"
                                    : suspendOverlayRender ? "swapchain-not-drawable"
                                    : s_insideECL          ? "inside-ecl-reentry"
                                    : !g_State.overlayInit ? "overlay-backend-uninitialized"
                                    : !g_State.syncInit    ? "overlay-sync-uninitialized"
                                                           : "startup-render-delay");
    }

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && g_State.overlayInit && g_State.syncInit &&
        !delayOverlayRenderAfterResume && !delayOverlayRenderAfterSyncInit &&
        !suppressOverlayRenderForLoadedStartupOverlay && !delayOverlayRenderAfterResourcePrime &&
        !delayOverlayRenderAfterFirstDrawProbe) {
        // Single log on first successful overlay render
        static int s_firstOverlayLogged = 0;
        if (s_firstOverlayLogged == 0) {
            s_firstOverlayLogged = 1;
            HookLogImportant(
                "DX12: ProcessFrame - first overlay render attempt (fence=%p, "
                "cmdList=%p, fgActive=%d, fgType=%s)",
                g_State.fence, g_State.cmdList, g_FGCompat.IsFGActive() ? 1 : 0,
                g_FGCompat.GetFGTypeName(g_FGCompat.GetActiveFGType()));
        }

        // FG state transition cooldown: skip overlay draws for a brief window
        // after FG mode changes to let Streamline stabilize its internal state.
        // Unlike the old cooldown (which did teardown/reinit and caused resource
        // churn crashes), this only pauses the draw — no resources are destroyed.
        static bool s_lastFGActive = false;
        static ce::fg_runtime::RuntimeMode s_lastRuntimeMode = ce::fg_runtime::RuntimeMode::kOff;
        static bool s_lastSLFGRunning = false;
        static ce::dx12_fg_transition::State s_transitionState;
        // NOTE: FG transition cooldown is now file-scope g_FGTransitionCooldown
        // so swapchain-change detection (earlier in ProcessFrame) can check it.
        bool currentFGActive = g_FGCompat.IsFGActive();
        auto currentRuntimeMode = g_FGCompat.GetRuntimeMode();
        bool currentSLFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);

        // Grace period counter — declared here so epoch sync can reference it.
        static int s_slOffGraceFrames = 0;

        // Epoch sync: when the outer FG state management block has already
        // processed an SL FG transition, bring our tracking variables in sync
        // to avoid redundant transition processing (double cooldowns, duplicate
        // GPU drain, swapchain queue re-clearing).
        static uint32_t s_innerSyncedEpoch = 0;
        uint32_t outerEpoch = g_OuterSLTransitionEpoch.load(std::memory_order_acquire);
        if (s_innerSyncedEpoch != outerEpoch) {
            bool wasSlOn = s_lastSLFGRunning;
            s_lastFGActive = currentFGActive;
            s_lastRuntimeMode = currentRuntimeMode;
            s_lastSLFGRunning = currentSLFGRunning;
            s_innerSyncedEpoch = outerEpoch;
            // If SL turned off, start grace period (mirroring normal detection)
            if (wasSlOn && !currentSLFGRunning) {
                s_slOffGraceFrames = 300;
            }
            HookLogImportant(
                "DX12: [inner] Synced tracking to outer epoch %u (fgActive=%d runtime=%s slFG=%d grace=%d)", outerEpoch,
                currentFGActive ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode),
                currentSLFGRunning ? 1 : 0, s_slOffGraceFrames);
        }

        // If SL directly signals FG is running, force currentFGActive true.
        // Heuristic detection may lag behind the SL hook's immediate signal,
        // creating a gap where IsFGActive() returns false even though SL FG
        // is already processing frames.
        if (currentSLFGRunning && !currentFGActive) {
            currentFGActive = true;
            if (!ce::fg_runtime::IsRuntimeFGActive(currentRuntimeMode)) {
                currentRuntimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG;
            }
        }

        // When SL signal goes from ON→OFF, the ECL heuristic may briefly
        // false-positive as FSR_FG (elevated ECL count from departing DLSS FG
        // looks like frame generation).  Suppress non-API FG detection for a
        // grace period after SL deactivates.  Also suppresses NVIDIA_SM false
        // positives from the cached 2× multiplier.
        if (s_lastSLFGRunning && !currentSLFGRunning) {
            // SL just turned OFF — start grace period.
            // 300 frames covers the slow ECL ratio decay after DLSS FG shutdown.
            s_slOffGraceFrames = 300;
            HookLogImportant("DX12: SL FG OFF — suppressing heuristic FG for 300 frames");
        }
        if (s_slOffGraceFrames > 0) {
            s_slOffGraceFrames--;
            // During grace period after SL FG OFF, suppress ALL non-API-confirmed
            // FG types.  The cached 2× multiplier from departing DLSS FG falsely
            // activates NVIDIA_SM detection, and elevated ECL counts falsely
            // trigger heuristic FSR_FG.  Only trust explicit API hooks
            // (fsrFGApiActive from ffxCreateContext).  DLSS_FG API requires SL
            // running, which is false during this grace period.
            if (!currentSLFGRunning && currentFGActive) {
                bool fsrApiConfirmed = g_FGCompat.IsFSRFGApiActive();
                if (!fsrApiConfirmed) {
                    currentFGActive = false;
                    currentRuntimeMode = ce::fg_runtime::RuntimeMode::kOff;
                }
            }
        }

        HookUpdatePreferredOverlayFGPublicationState(currentFGActive, currentRuntimeMode, "DX12::ProcessFrame");
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
            // Publish the locally-computed FG state so per-frame suppression
            // (e.g. SL-off grace period) is reflected in the overlay.
            if (ce::dx12_overlay_policy::DoOverlayFGPublishedTypesDiffer(plan.publishFGActive, plan.publishRuntimeMode,
                                                                         currentFGActive, currentRuntimeMode)) {
                HookLogImportant(
                    "DX12::ProcessFrame overlay divergence: plan(active=%d mode=%s) vs local(active=%d mode=%s)",
                    plan.publishFGActive ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(plan.publishRuntimeMode),
                    currentFGActive ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
            }
            ce::overlay_metrics::PublicationInput input;
            input.effectiveFGActive = currentFGActive;
            input.runtimeMode = currentRuntimeMode;
            input.outputFPS = g_FGCompat.GetOutputFPS();
            input.baseFPS = g_FGCompat.GetBaseFPS();
            input.multiplier = g_FGCompat.GetFGMultiplier();
            input.publicationSource = "DX12::ProcessFrame";
            ce::overlay_metrics::PublishOverlayFGMetrics(perf, input);
        }

        ID3D12CommandQueue* transitionSwapchainQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            transitionSwapchainQueue = g_SwapchainQueue;
        }
        const bool transitionRecoveringPostFSRNonFG = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
            g_HadFSRFGPhase, g_NeedOffscreenOverlayAfterPostFSRNonFG, currentFGActive, currentSLFGRunning,
            transitionSwapchainQueue != nullptr);
        const bool transitionStartupBypassActive = startupOverlayCompatibilityActive && !currentFGActive;
        s_transitionState = ce::dx12_fg_transition::Reduce(
            s_transitionState, {
                                   .runtimeMode = currentRuntimeMode,
                                   .effectiveFGActive = currentFGActive,
                                   .streamlineFGRunning = currentSLFGRunning,
                                   .streamlineLoaded = g_FGCompat.HasStreamlineSupport(),
                                   .runtimeOwnsSwapchain = g_FGRuntimeOwnsSwapchain,
                                   .hadFSRPhase = g_HadFSRFGPhase,
                                   .recoveringPostFSRNonFG = transitionRecoveringPostFSRNonFG,
                                   .startupBypassActive = transitionStartupBypassActive,
                                   .overlaySuppressed = !allowOverlayRender,
                               });

        // Detect FG on/off changes AND FG type changes (e.g., FSR FG → DLSS FG)
        // Also detect SL FG signal changes (immediate from SL hook)
        bool fgChanged = (currentFGActive != s_lastFGActive);
        bool runtimeModeChanged = (currentRuntimeMode != s_lastRuntimeMode);
        bool slSignalChanged = (currentSLFGRunning != s_lastSLFGRunning);

        if (fgChanged || runtimeModeChanged || slSignalChanged) {
            auto s_lastRuntimeMode_saved = s_lastRuntimeMode;  // save before update for syncInit logic
            const bool previousWasFG = ce::fg_runtime::IsActualGeneratedFrameMode(s_lastRuntimeMode_saved);
            const bool targetIsFGOff = !ce::fg_runtime::IsActualGeneratedFrameMode(currentRuntimeMode);
            ID3D12CommandQueue* transitionOverlayBackendQueue =
                g_OverlayAdapterBackendQueue.load(std::memory_order_acquire);
            const bool liveNoCallbackNativeFSRSuspensionToggle =
                !slSignalChanged &&
                ce::dx12_overlay_policy::IsLiveNoCallbackNativeFSRSuspensionToggle(
                    s_lastRuntimeMode_saved, currentRuntimeMode, currentSLFGRunning,
                    g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire), g_FGRuntimeOwnsSwapchain,
                    transitionSwapchainQueue != nullptr, g_State.overlayInit,
                    transitionSwapchainQueue != nullptr && transitionOverlayBackendQueue == transitionSwapchainQueue);
            ID3D12CommandQueue* postNativeFSROffRecoveryQueue =
                g_PostNativeFSROffGameSwapchainRecoveryQueue.load(std::memory_order_acquire);
            const bool gameSwapchainRecoveryToggleAfterNativeFSROff =
                !slSignalChanged &&
                ce::dx12_overlay_policy::IsGameSwapchainRecoveryToggleAfterNativeFSROff(
                    s_lastRuntimeMode_saved, currentRuntimeMode, currentSLFGRunning,
                    transitionSwapchainQueue != nullptr && postNativeFSROffRecoveryQueue == transitionSwapchainQueue);
            const bool heuristicOnlyRuntimeModeFlip =
                !slSignalChanged &&
                ce::dx12_overlay_policy::IsHeuristicOnlyRuntimeModeFlip(
                    s_lastSLFGRunning, currentSLFGRunning, g_FGRuntimeOwnsSwapchain, g_FGCompat.IsFSRFGApiActive(),
                    transitionSwapchainQueue != nullptr, g_State.overlayInit,
                    transitionSwapchainQueue != nullptr && transitionOverlayBackendQueue == transitionSwapchainQueue);
            const bool shouldStartFGTransitionCooldown =
                ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
                    s_lastRuntimeMode_saved, currentRuntimeMode, s_lastFGActive, currentFGActive, s_lastSLFGRunning,
                    currentSLFGRunning,
                    liveNoCallbackNativeFSRSuspensionToggle || gameSwapchainRecoveryToggleAfterNativeFSROff ||
                        heuristicOnlyRuntimeModeFlip);
            if (!shouldStartFGTransitionCooldown) {
                HookLogImportant(
                    "DX12: Runtime state changed without generated-frame transition "
                    "(prev_mode=%s next_mode=%s active=%d->%d sl_signal=%d->%d "
                    "liveNoCallbackNativeFSRSuspensionToggle=%d gameSwapchainRecoveryToggle=%d "
                    "heuristicOnlyFlip=%d backendQ=%p "
                    "scQueue=%p recoveryQ=%p) — overlay remains live",
                    ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode_saved),
                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), s_lastFGActive ? 1 : 0,
                    currentFGActive ? 1 : 0, s_lastSLFGRunning ? 1 : 0, currentSLFGRunning ? 1 : 0,
                    liveNoCallbackNativeFSRSuspensionToggle ? 1 : 0,
                    gameSwapchainRecoveryToggleAfterNativeFSROff ? 1 : 0, heuristicOnlyRuntimeModeFlip ? 1 : 0,
                    transitionOverlayBackendQueue, transitionSwapchainQueue, postNativeFSROffRecoveryQueue);
                LogOverlayCoverageSummary("FG transition edge (overlay remains live)");
                s_lastFGActive = currentFGActive;
                s_lastRuntimeMode = currentRuntimeMode;
                s_lastSLFGRunning = currentSLFGRunning;
            } else {
                const ID3D12CommandQueue* currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
                const ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
                const bool commandQueueSettledToPrimary =
                    currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
                const int transitionCooldownFrames = ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                                                         commandQueueSettledToPrimary, g_HadFSRFGPhase,
                                                         previousWasFG && targetIsFGOff && !currentSLFGRunning)
                                                         ? 15
                                                         : 60;
                HookLogImportant(
                    "DX12: FG transition prev_mode=%s next_mode=%s phase=%d ownership=%d render_mode=%d callback=%d "
                    "publish_active=%d sl_signal=%d->%d cooldown=%d epoch=%u",
                    ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode),
                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode),
                    static_cast<int>(s_transitionState.snapshot.phase),
                    static_cast<int>(s_transitionState.snapshot.ownership),
                    static_cast<int>(s_transitionState.snapshot.renderMode),
                    s_transitionState.snapshot.shouldInstallPostSLCallback ? 1 : 0,
                    s_transitionState.snapshot.publishFGActive ? 1 : 0, s_lastSLFGRunning ? 1 : 0,
                    currentSLFGRunning ? 1 : 0, transitionCooldownFrames, s_transitionState.snapshot.epoch);
                LogOverlayCoverageSummary("FG transition edge");
                s_lastFGActive = currentFGActive;
                s_lastRuntimeMode = currentRuntimeMode;
                s_lastSLFGRunning = currentSLFGRunning;
                g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                    g_FGTransitionCooldown.load(std::memory_order_acquire), transitionCooldownFrames,
                    ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                        commandQueueSettledToPrimary, g_HadFSRFGPhase,
                        previousWasFG && targetIsFGOff && !currentSLFGRunning));
                g_PostSLCooldownRemaining.store(g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                std::memory_order_release);
                g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);

                // Immediately disable post-SL rendering during FG transitions.
                // Keep the callback installed when Streamline is still running so
                // synthetic startup presents can route through PostSL safely while
                // the active gate and cooldown still suppress real rendering.
                // Make-before-break: the explicit-OFF keep-alive owns the PostSL
                // path until the normal route recovers — do not disable it here.
                const bool innerKeepConfirmedPostSLAliveAcrossOff =
                    !currentSLFGRunning && g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
                if (!innerKeepConfirmedPostSLAliveAcrossOff) {
                    g_PostSLOverlayActive.store(false, std::memory_order_release);
                    if (!DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(currentSLFGRunning)) {
                        SetPostSLCallbackInstalled(false, "DX12: inner FG transition");
                    }
                }
                g_PostSLStallCounter.store(0, std::memory_order_release);      // Fresh start after transition
                g_PostSLStableFrameCount.store(0, std::memory_order_release);  // Reset warmup counter
                g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);

                // When SL FG turns ON or OFF, clear any false heuristic FSR FG state.
                // SL's queue changes trigger the queue-change heuristic, causing
                // false FSR FG detection.  Clear it during ANY SL FG transition:
                //   - ON: SL creates new queues → queue-change heuristic fires falsely
                //   - OFF: SL's queue was "current FG queue" → not cleared until
                //          consecutive initial-queue frames pass the threshold
                if (g_FGCompat.IsHeuristicFSRFGActive()) {
                    g_FGCompat.SetHeuristicFSRFGActive(false);
                    HookLogImportant("DX12: Cleared heuristic FSR FG during SL FG %s transition",
                                     currentSLFGRunning ? "ON" : "OFF");
                }

                // Reset the queue-change heuristic's internal state so it re-captures
                // the "initial queue" after the transition.  SL's leftover queue would
                // otherwise persist as s_initialQueue/s_currentFGQueue and immediately
                // re-trigger false FSR FG detection.
                RequestFGDetectionHeuristicReset();

                // Drain in-flight overlay GPU work on ANY FG transition.
                // When FG activates (especially FSR FG), it may use the same queue
                // our overlay was rendering on.  In-flight overlay ECLs on that queue
                // can cause FSR's internal synchronization to deadlock (spin-wait in
                // ffxQuery).  Drain ensures the queue is clean before FG takes over.
                //
                // Original: only drained on SL OFF.  Extended to all transitions
                // because FSR FG also needs a clean queue at activation.
                if (g_State.fence && g_State.currentFenceValue > 0) {
                    UINT64 lastVal = g_State.currentFenceValue;
                    HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    if (drainEvent) {
                        HRESULT drainHr = g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
                        if (SUCCEEDED(drainHr)) {
                            DWORD waitResult = WaitForSingleObject(drainEvent, 200);
                            HookLogImportant(
                                "DX12: FG transition — drained overlay GPU work (fenceVal=%llu wait=%u "
                                "slSignalChanged=%d "
                                "fgChanged=%d)",
                                (unsigned long long)lastVal, waitResult, slSignalChanged ? 1 : 0, fgChanged ? 1 : 0);
                        } else {
                            HookLogImportant("DX12: FG transition — fence drain failed hr=0x%08X", drainHr);
                        }
                        CloseHandle(drainEvent);
                    }
                }

                ClearPostSLQueues("DX12: FG transition queue reset");
                // Keep the SL wrapper queue alive while Streamline still owns the
                // presentation path, even if FG is temporarily idle.
                {
                    bool targetUsesStreamline = ce::fg_runtime::RuntimeModeUsesStreamline(currentRuntimeMode);
                    if (!targetUsesStreamline) {
                        ID3D12CommandQueue* oldWrapper = g_SLWrapperQueue.exchange(nullptr, std::memory_order_acq_rel);
                        if (oldWrapper) {
                            oldWrapper->Release();
                            HookLogImportant("DX12: runtime=%s — released SL wrapper queue %p",
                                             ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), oldWrapper);
                        }
                    } else {
                        ID3D12CommandQueue* kept = g_SLWrapperQueue.load(std::memory_order_acquire);
                        HookLogImportant("DX12: runtime=%s — keeping SL wrapper queue %p alive",
                                         ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), kept);
                    }
                }
                if (!innerKeepConfirmedPostSLAliveAcrossOff) {
                    g_PostSLConfirmedRendering.store(false, std::memory_order_release);  // Re-probe needed
                }
                if (!ce::dx12_overlay_policy::ShouldPreservePostSLLastWorkingQueueForPostFSROffRecovery(
                        g_HadFSRFGPhase, previousWasFG, targetIsFGOff)) {
                    // Old SL queues may be destroyed after most FG mode switches
                    // (e.g., DLSS FG phase 1 -> FSR FG -> DLSS FG phase 2). Keep the
                    // last validated queue only for the immediate post-FSR FG-off
                    // recovery window, where it is the only queue that already proved
                    // safe for the live swapchain.
                    SetPostSLLastWorkingQueue(nullptr);
                } else {
                    HookLogImportant(
                        "DX12: Preserving PostSL lastWorkingQueue %p for immediate post-FSR FG-off recovery",
                        g_PostSLLastWorkingQueue);
                }

                // Save the current ProcessFrame gameQueue as a pre-FG snapshot.
                // When PostSL activates after the cooldown, g_CommandQueue may have
                // been polluted by SL's internal queues.  gameQueue (resolved at the
                // top of ProcessFrame from scQueue or cmdQueue) is still the game's
                // real queue at this point.
                if (g_PreFGGameQueue)
                    g_PreFGGameQueue->Release();
                g_PreFGGameQueue = gameQueue;
                if (gameQueue)
                    gameQueue->AddRef();

                // Force sync resources re-initialization on next overlay render.
                // After FG type transitions (e.g., FSR→DLSS), the sync resources
                // (allocators, fence, cmdList) were used on a different queue during
                // the previous FG phase.  Re-using them on a new queue after swapchain
                // recreation causes DEVICE_REMOVED.  Fresh resources avoid this.
                //
                // EXCEPTION 1: FG→off transitions do NOT invalidate sync.  There is no
                // swapchain recreation when FG simply turns off, and the allocators/
                // fence are device-level objects that work on any DIRECT queue.  The
                // GPU drain above ensures all in-flight work completes.
                //
                // EXCEPTION 2: off→on transitions (None→FSR_FG or None→DLSS_FG) also
                // do NOT invalidate sync.  The existing resources are device-level and
                // work on any DIRECT queue.  Forcing re-init here is unnecessary and
                // causes the "FG→FG" misclassification for what is really "off→on".
                bool actualFGToFG = previousWasFG && !targetIsFGOff;
                if (g_State.syncInit && actualFGToFG) {
                    HookLogImportant("DX12: FG transition (FG→FG: %s→%s) — forcing syncInit=false for fresh resources",
                                     ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode_saved),
                                     ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
                    g_State.syncInit = false;
                } else if (g_State.syncInit && targetIsFGOff) {
                    HookLogImportant("DX12: FG→off transition — keeping syncInit=true (reusing existing resources)");
                } else if (g_State.syncInit && !previousWasFG && !targetIsFGOff) {
                    HookLogImportant(
                        "DX12: FG off→on transition (%s→%s) — keeping syncInit=true (resources work on any queue)",
                        ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode_saved),
                        ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
                }

                // Clear stale swapchain queue only when transitioning TO SL-based FG
                // if we've never had an FSR FG phase. If FSR FG already ran, SL might
                // reuse FSR's swapchain (no new CreateSwapChainForHwnd), so scQueue is
                // the CORRECT queue for backbuffer access. Keep it alive via AddRef.
                if (runtimeModeChanged) {
                    bool newTypeNeedsScQueue = (currentRuntimeMode == ce::fg_runtime::RuntimeMode::kFSRFG);
                    bool targetIsNone = !ce::fg_runtime::IsActualGeneratedFrameMode(currentRuntimeMode);
                    if (targetIsNone && !g_HadFSRFGPhase) {
                        // FG→off: keep g_SwapchainQueue as-is.  Same rationale as
                        // the outer slTurnedOff handler: SL's swapchain may persist
                        // after FG teardown, so g_SwapchainQueue (set by the
                        // CreateSwapChainForHwnd hook) already points to the correct
                        // queue.  Restoring to origGame causes queue/swapchain
                        // mismatch → DXGI_ERROR_ACCESS_DENIED.
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        // FG is off — FG runtime no longer owns the queue
                        if (g_FGRuntimeOwnsSwapchain) {
                            g_FGRuntimeOwnsSwapchain = false;
                            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                            g_FGRuntimeOwnsSwapchainSince = 0;
                            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
                            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                            HookLogImportant("DX12: FG→off — clearing FG runtime ownership of swapchain queue");
                        }
                        HookLogImportant("DX12: FG→off — keeping g_SwapchainQueue %p (origGame=%p)", g_SwapchainQueue,
                                         g_OriginalGameQueue);
                        if (!g_SwapchainQueue && g_OriginalGameQueue) {
                            // Swapchain queue not captured yet — fall back to origGame
                            g_OriginalGameQueue->AddRef();
                            g_SwapchainQueue = g_OriginalGameQueue;
                            g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                            HookLogImportant("DX12: FG→off — scQueue was null, falling back to origGame %p",
                                             g_OriginalGameQueue);
                        }
                    } else if (!newTypeNeedsScQueue && !g_HadFSRFGPhase) {
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        // Clear FG runtime ownership when transitioning away from FSR FG
                        if (g_FGRuntimeOwnsSwapchain && targetIsNone) {
                            g_FGRuntimeOwnsSwapchain = false;
                            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                            g_FGRuntimeOwnsSwapchainSince = 0;
                            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
                            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                            HookLogImportant("DX12: FG type change to None — clearing FG runtime ownership");
                        }
                        if (g_SwapchainQueue) {
                            // Protect recently-captured scQueue from phantom FG detections.
                            // After swapchain recreation, the heuristic may briefly detect
                            // NVIDIA_SM (false positive from Present rate measurement).
                            // Clearing scQueue in that window causes ProcessFrame to fall
                            // back to origGame, which FSR FG uses internally → deadlock.
                            ULONGLONG age = GetTickCount64() - g_SwapchainQueueCaptureTime;
                            if (age < 5000) {
                                HookLogImportant(
                                    "DX12: FG type change to %s — PRESERVING g_SwapchainQueue %p (captured %llu ms "
                                    "ago, "
                                    "too recent to clear)",
                                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), g_SwapchainQueue, age);
                            } else {
                                HookLogImportant(
                                    "DX12: FG type change to %s — clearing stale g_SwapchainQueue %p (no FSR history, "
                                    "age=%llu ms)",
                                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), g_SwapchainQueue, age);
                                g_SwapchainQueue->Release();
                                g_SwapchainQueue = nullptr;
                                g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                            }
                        }
                    } else if (!newTypeNeedsScQueue && g_HadFSRFGPhase) {
                        if (targetIsNone) {
                            // FSR→DLSS→Off: wait for live non-FG command traffic to
                            // prove which queue owns the resumed Present path again.
                            // Forcing origGame back into g_SwapchainQueue here caused
                            // Talos to submit the first recovered non-FG overlay ECL
                            // on the wrong queue/backbuffer pairing, immediately
                            // triggering DEVICE_REMOVED.
                            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                            const bool preserveRuntimeOwnedFSRTeardown =
                                ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(
                                    targetIsNone, g_HadFSRFGPhase, g_FGRuntimeOwnsSwapchain,
                                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                            if (preserveRuntimeOwnedFSRTeardown) {
                                HookLogImportant(
                                    "DX12: FG→off after FSR phase classified while runtime still owns swapchain — "
                                    "preserving FSR queue ownership until a stronger off signal appears "
                                    "(scQ=%p origGame=%p primary=%p cmdQ=%p)",
                                    g_SwapchainQueue, g_OriginalGameQueue,
                                    g_PrimaryGameQueue.load(std::memory_order_acquire),
                                    g_CommandQueue.load(std::memory_order_acquire));
                                return;
                            }
                            if (g_FGRuntimeOwnsSwapchain) {
                                g_FGRuntimeOwnsSwapchain = false;
                                DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false,
                                                                                       std::memory_order_release);
                                g_FGRuntimeOwnsSwapchainSince = 0;
                                ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
                                ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                                const bool preserveAuthoritativeFSRDuringTransition =
                                    ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(
                                        g_FGCompat.IsFSRFGApiActive(), targetIsNone,
                                        g_FGTransitionCooldown.load(std::memory_order_acquire));
                                if (preserveAuthoritativeFSRDuringTransition) {
                                    HookLogImportant(
                                        "DX12: FG→off after FSR phase detected during active transition cooldown — "
                                        "preserving authoritative FSR state until queue topology settles (cooldown=%d "
                                        "scQ=%p origGame=%p primary=%p cmdQ=%p)",
                                        g_FGTransitionCooldown.load(std::memory_order_acquire), g_SwapchainQueue,
                                        g_OriginalGameQueue, g_PrimaryGameQueue.load(std::memory_order_acquire),
                                        g_CommandQueue.load(std::memory_order_acquire));
                                } else if (g_FGCompat.IsFSRFGApiActive()) {
                                    SetNativeFSRStartupConfigureArmingPending(false,
                                                                              "runtime mode transition cleared FSR");
                                    ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
                                        "runtime mode transition cleared FSR");
                                    g_FGCompat.SetFSRFGActive(false);
                                    g_FGCompat.SetFSRFGMultiplier(0);
                                }
                                HookLogImportant(
                                    "DX12: FG→off after FSR phase — clearing FG runtime ownership of swapchain queue");
                            }
                            if (g_SwapchainQueue && g_SwapchainQueue != g_OriginalGameQueue) {
                                HookLogImportant(
                                    "DX12: FG→off after FSR phase — releasing stale g_SwapchainQueue %p and waiting "
                                    "for "
                                    "the live non-FG queue to be recaptured (origGame=%p primary=%p cmdQ=%p)",
                                    g_SwapchainQueue, g_OriginalGameQueue,
                                    g_PrimaryGameQueue.load(std::memory_order_acquire),
                                    g_CommandQueue.load(std::memory_order_acquire));
                                g_SwapchainQueue->Release();
                                g_SwapchainQueue = nullptr;
                                g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                                g_SwapchainQueueCaptureTime = 0;
                            }
                            if (!g_SwapchainQueue) {
                                HookLogImportant(
                                    "DX12: FG→off after FSR phase — keeping g_SwapchainQueue null until non-wrapper "
                                    "command traffic settles (origGame=%p primary=%p cmdQ=%p)",
                                    g_OriginalGameQueue, g_PrimaryGameQueue.load(std::memory_order_acquire),
                                    g_CommandQueue.load(std::memory_order_acquire));
                            }
                        } else {
                            // FSR→DLSS transition: the swapchain was created on FSR's queue
                            // (g_SwapchainQueue), so backbuffers belong to it. Keep it alive.
                            // Render pre-SL on scQueue — the swapchain's own queue has
                            // authorized access to backbuffers without cross-queue issues.
                            HookLogImportant(
                                "DX12: FG type change to %s — KEEPING g_SwapchainQueue %p for backbuffer access (it's "
                                "the "
                                "swapchain creation queue)",
                                ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), g_SwapchainQueue);
                            g_NeedGPUDrainBeforeRender = false;
                        }
                    } else {
                        HookLogImportant("DX12: FG type change to %s — keeping g_SwapchainQueue %p (FSR needs it)",
                                         ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), g_SwapchainQueue);
                    }
                }

                // Probe real D3D12 ECL when SL FG first activates.  Must happen
                // before the first overlay ECL submission so we have the bypass
                // ready.
                if (currentFGActive && IsStreamlineLoaded()) {
                    auto* dev = g_Device.load(std::memory_order_acquire);
                    if (dev)
                        ProbeRealD3D12ECL(dev);
