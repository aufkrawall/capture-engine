                        return;
                    }
                    if (slFGAtDispatch && selectedQueueIsSwapchainQueue && selectedQueueOrigECLMatchesRealECL &&
                        selectedQueueOrigECL) {
                        submittedQueue = queue;
                        selectedQueueOrigECL(queue, 1, lists);
                        usedOrigECL = true;
                        static int s_noWrapperDirectSelectedQueueLog = 0;
                        if (s_noWrapperDirectSelectedQueueLog < 10 || (s_noWrapperDirectSelectedQueueLog % 200) == 0) {
                            HookLogImportant(
                                "DX12: PostSL no-wrapper direct selected-queue submit #%d on %p "
                                "(scQueue=%p origECL matches realECL)",
                                s_noWrapperDirectSelectedQueueLog, queue, scQueue);
                        }
                        s_noWrapperDirectSelectedQueueLog++;
                    } else {
                        s_insidePostSLOverlayECL = true;
                        queue->ExecuteCommandLists(1, lists);
                        s_insidePostSLOverlayECL = false;
                        usedVirtualCall = true;
                        static int s_noSlQ = 0;
                        if (s_noSlQ++ < 3)
                            HookLogImportant("DX12: PostSL no SL wrapper queue, using origGame %p", queue);
                    }
                }
            }
        } else if (isSLWrapperQ) {
            ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
            if (origECL) {
                origECL(queue, 1, lists);
                usedOrigECL = true;
            } else {
                queue->ExecuteCommandLists(1, lists);
                usedVirtualCall = true;
            }
        } else if (realECL) {
            realECL(queue, 1, lists);
            usedRealECL = true;
        } else {
            ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
            if (origECL) {
                origECL(queue, 1, lists);
                usedOrigECL = true;
            } else {
                queue->ExecuteCommandLists(1, lists);
                usedVirtualCall = true;
            }
        }
    }

    if (rendered) {
        const bool retiredOfficialUiCoverage =
            retireOfficialUiCoverageAfterExactDraw &&
            ce::dx12_streamline_ui_overlay::RetirePostSLCoverageForExactBackbufferTakeover();
        if (retireOfficialUiCoverageAfterExactDraw) {
            static std::atomic<int> s_exactTransportOverridesOfficialUiLogCount{0};
            const int logCount =
                s_exactTransportOverridesOfficialUiLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: PostSL first proven startup output received an exact backbuffer draw; "
                    "retiredOfficialUiCoverage=%d so later proxy buffers cannot inherit stale coverage "
                    "(transportForced=%d postFSR=%d explicitPureDLSSColdStart=%d call#=%d log=%d)",
                    retiredOfficialUiCoverage ? 1 : 0, g_RequireExactPostSLStartupTransportDraw ? 1 : 0,
                    g_HadFSRFGPhase ? 1 : 0, explicitEnablePureDLSSColdStartProof ? 1 : 0, s_callsSinceReactivation,
                    logCount);
            }
        }
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kPostSL);
        SharedMemoryLayout* postSLShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        OverlayConfig postSLOverlayCfg = GetActiveDX12OverlayConfig(postSLShm);
        bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
        if (postSLShm && g_IPC && g_IPC->IsRecording() && isRealFrame && postSLOverlayCfg.showOverlay &&
            postSLOverlayCfg.captureIncludeOverlay) {
            PublishDX12CapturedFrame(pSwapChain, postSLShm, submittedQueue, true, bufIdx);
        }
        const uint64_t postSLScreenshotRequestId = GetPendingScreenshotRequestId(postSLShm);
        if (postSLScreenshotRequestId != 0 && postSLOverlayCfg.showOverlay &&
            postSLOverlayCfg.screenshotIncludeOverlay) {
            CaptureRequestedDX12Screenshot(sc3, postSLShm, postSLScreenshotRequestId, submittedQueue);
        }
    }

    // Fence signal for allocator tracking.
    // CRITICAL: Signal on the SAME queue we submitted the command list to.
    // During SL FG with direct submission, use the real D3D12 queue behind SL's wrapper.
    bool slFGSubmit = cachedSLFGActive;
    if (g_State.fence) {
        UINT64 next = g_State.currentFenceValue + 1;
        ID3D12CommandQueue* submitQueue = submittedQueue ? submittedQueue : queue;
        HRESULT sigHr = submitQueue->Signal(g_State.fence, next);
        if (SUCCEEDED(sigHr)) {
            g_State.currentFenceValue = next;
            if (idx >= 0 && idx < (int)g_State.fenceValues.size())
                g_State.fenceValues[idx] = next;

            // Cross-queue GPU sync: only for non-SL-FG, non-same-queue scenarios
            bool crossQueueSafe = scQueue && scQueue != submitQueue && !slFGSubmit;
            if (crossQueueSafe) {
                HRESULT waitHr = scQueue->Wait(g_State.fence, next);
                crossQueueSynced = true;
                if (FAILED(waitHr)) {
                    static int s_waitFail = 0;
                    if (s_waitFail++ < 5)
                        HookLog(
                            "DX12: PostSL cross-queue Wait failed hr=0x%08X "
                            "(scQueue=%p fence=%p val=%llu)",
                            waitHr, scQueue, g_State.fence, (unsigned long long)next);
                }
            }
        }
    }

    // (Dedicated queue post-sync removed — no longer using dedicated queue.)

    // Periodic allocator fence health check — detect tracking issues before crash
    if (g_State.fence) {
        UINT64 completed = g_State.fence->GetCompletedValue();
        static int s_fenceHealthLog = 0;
        s_fenceHealthLog++;
        UINT64 expected = g_State.currentFenceValue;
        UINT64 gap = (expected > completed) ? (expected - completed) : 0;
        if (s_fenceHealthLog <= 10 || (s_fenceHealthLog % 200 == 0) || gap > 10) {
            HookLogImportant(
                "DX12: PostSL fence health #%d — completed=%llu current=%llu gap=%llu allocators=%d idx=%d",
                s_fenceHealthLog, completed, expected, gap, (int)g_State.allocators.size(), idx);
        }
    }

    // Diagnostic logging — log queue info and device health after submit
    static std::atomic<int> s_postSLRenderCount{0};
    int renderNum = s_postSLRenderCount.fetch_add(1, std::memory_order_relaxed) + 1;
    s_postSLRenders.fetch_add(1, std::memory_order_relaxed);
    HRESULT postDevReason = dev->GetDeviceRemovedReason();

    if (SUCCEEDED(postDevReason) && rendered && pSwapChain && submittedQueue) {
        ++s_PostSLSuccessfulSubmitSequence;
        if (!g_HadSuccessfulPostSLPhase.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "DX12: Latched first device-healthy PostSL submit for future repeated pure-DLSS handoff prewarm "
                "(swapchain=%p queue=%p)",
                pSwapChain, submittedQueue);
        }
        if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
            g_PostSLWarmResumePreservationPending.exchange(false, std::memory_order_acq_rel)) {
            HookLogImportant(
                "DX12: PostSL warm-resume preservation completed on first successful active submit "
                "(sc=%p queue=%p)",
                pSwapChain, submittedQueue);
        }
        IDXGISwapChain* previousSuccessfulPostSLSwapchain =
            g_LastSuccessfulPostSLSwapchain.exchange(pSwapChain, std::memory_order_acq_rel);
        if (previousSuccessfulPostSLSwapchain != pSwapChain) {
            HookLogImportant(
                "DX12: PostSL proved exact swapchain route %p on submitted queue %p "
                "(previousSwapchain=%p epoch=%d)",
                pSwapChain, submittedQueue, previousSuccessfulPostSLSwapchain, s_reactivationEpoch);
        }

        // The same COM identity may be rebound from the normal Present route
        // to a runtime proxy route. The newest successful submit is the useful
        // ownership proof; do not let its pre-FG identity classify it as normal
        // after Streamline is explicitly switched off. Publish this before the
        // confirmed-render release stores below so the OFF callback cannot see
        // confirmation without also seeing the exact swapchain proof.
        IDXGISwapChain* expectedNormalSwapchain = pSwapChain;
        if (g_LastProvenOriginalQueueSwapchain.compare_exchange_strong(
                expectedNormalSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
            HookLogImportant(
                "DX12: PostSL superseded remembered original-queue ownership for swapchain %p "
                "with a successful runtime-route submit",
                pSwapChain);
        }
    }

    // Mark PostSL as confirmed rendering — pre-SL draw can now be suppressed.
    // The first ECL just landed safely (devRemoved checked below): record it for this
    // reactivation epoch so the remaining reactivation warmup is confirmed-bypassed and a
    // live overlay is never re-blanked after the retained startup swapchain is released.
    g_PostSLConfirmedRenderInCurrentReactivationEpoch.store(true, std::memory_order_release);
    if (!g_PostSLConfirmedRendering.load(std::memory_order_relaxed)) {
        g_PostSLConfirmedRendering.store(true, std::memory_order_release);
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
        g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
        ReleaseStreamlineStartupActivationSwapchain("DX12: PostSL confirmed rendering");
        // kStreamlineStartupTransitionGraceMs from the SL FG activation arm covers the
        // remaining startup churn window. Streamline can still call Present briefly after
        // PostSL confirms; during that family CE keeps using the bypass trampoline for
        // Streamline-stack Presents and keeps stale OFF churn suppressed. The window
        // expires naturally from the arm time; the old ShouldClear... check at ~line 9220
        // is removed because it cleared the window too aggressively on the same call
        // where confirmed became true, re-exposing the startup churn race.
        HookLogImportant("DX12: PostSL CONFIRMED rendering via re-entrant Present — suppressing pre-SL draw");
    }
    // Reset stall counter — PostSL is actively rendering, no need for pre-SL fallback
    g_PostSLStallCounter.store(0, std::memory_order_release);
    // Track PostSL warmup — stable frame count since last FG transition.
    // Stall fallback is only enabled after this exceeds warmup threshold.
    const int stableFrameCount = g_PostSLStableFrameCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (stableFrameCount == 1) {
        ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLFirstConfirmedRender,
                                    "DX12::PostSLOverlayRender", submittedQueue, pSwapChain,
                                    g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(),
                                    HookHasExplicitStreamlineSetOptionsActivation());
    }
    const bool extendRuntimeStateStabilization =
        g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.load(std::memory_order_acquire);
    const int runtimeStateStabilizationLastFrame =
        ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationLastFrame(extendRuntimeStateStabilization);
    const bool runtimeStateStabilizing =
        ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(
            true, stableFrameCount, extendRuntimeStateStabilization);
    const bool runtimeStateStabilizingPreviousFrame =
        stableFrameCount > 1 && ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(
                                    true, stableFrameCount - 1, extendRuntimeStateStabilization);
    if (runtimeStateStabilizing && !runtimeStateStabilizingPreviousFrame) {
        g_PostSLRuntimeStateStabilizationLogged.store(true, std::memory_order_release);
        HookLogImportant(
            "DX12: PostSL confirmed startup rendering entered runtime-state stabilization "
            "(stableFrames=%d first=%d last=%d extended=%d epoch=%d)",
            stableFrameCount, ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
            runtimeStateStabilizationLastFrame, extendRuntimeStateStabilization ? 1 : 0, s_reactivationEpoch);
    } else if (!runtimeStateStabilizing && runtimeStateStabilizingPreviousFrame) {
        HookLogImportant(
            "DX12: PostSL confirmed startup rendering left runtime-state stabilization "
            "(stableFrames=%d last=%d extended=%d epoch=%d)",
            stableFrameCount, runtimeStateStabilizationLastFrame, extendRuntimeStateStabilization ? 1 : 0,
            s_reactivationEpoch);
    }
    // Track last working queue — survives FG transitions so we can prefer
    // a proven-safe queue when PostSL re-activates after FSR→DLSS switch.
    if (SUCCEEDED(postDevReason) && submittedQueue != g_PostSLLastWorkingQueue &&
        ce::dx12_overlay_policy::ShouldRememberPostSLLastWorkingQueue(isSLWrapperQ)) {
        HookLogImportant("DX12: PostSL updating lastWorkingQueue %p -> %p", g_PostSLLastWorkingQueue, submittedQueue);
        SetPostSLLastWorkingQueue(submittedQueue);
    }
    if (renderNum <= 20 || (renderNum % 10) == 0 || renderNum >= 1800 || FAILED(postDevReason)) {
        HookLogImportant(
            "DX12: Post-SL overlay SUBMIT #%d (bufIdx=%u queue=%p scQueue=%p slWrapper=%d rendered=%d "
            "virtualCall=%d realECL=%d origECL=%d xqSync=%d tid=0x%04X devRemoved=0x%08X epoch=%d)",
            renderNum, bufIdx, submittedQueue, scQueue, isSLWrapperQ ? 1 : 0, rendered ? 1 : 0, usedVirtualCall ? 1 : 0,
            usedRealECL ? 1 : 0, usedOrigECL ? 1 : 0, crossQueueSynced ? 1 : 0, GetCurrentThreadId(),
            (unsigned)postDevReason, s_reactivationEpoch);
    }
    // Early warning: if device just failed, log immediately
    if (FAILED(postDevReason)) {
        HookLogImportant(
            "DX12: DEVICE_REMOVED detected after PostSL ECL submit #%d "
            "(queue=%p scQueue=%p hr=0x%08X)",
            renderNum, submittedQueue, scQueue, (unsigned)postDevReason);
    }

    bb->Release();
}

static void PostSLOverlayRenderGated(IDXGISwapChain* pSwapChain) {
    // [OVERLAY COVERAGE] every SL-routed callback invocation with a real
    // swapchain is one presented frame reaching the screen through Streamline's
    // pipeline (synthetic re-entrant, startup normal-route, retained startup
    // activation service). These presents bypass DX12_ProcessFrameExternal, so
    // they are accounted here on every exit path. Null-swapchain invocations
    // (ECL-hook direct triggers) are not presents and are excluded.
    const bool accountCoverage = ce::dx12_overlay_policy::ShouldAccountPostSLCallbackAsSeparatePresent(
        pSwapChain != nullptr, HookOverlayObserverOnlyEnabled(), g_PostSLDrawBelongsToEnclosingProcessFramePresent);
    const bool officialUiCoverage = ce::dx12_streamline_ui_overlay::HasActiveCoverage();
    auto overlayCoverageGuard = ce::make_scope_guard([accountCoverage, officialUiCoverage]() {
        if (accountCoverage) {
            AccountPresentForOverlayCoverage(officialUiCoverage, "PostSL");
        }
    });

    if (!g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire)) {
        NoteDX12OverlayCoverageGate("postsl-execution-disabled");
        return;
    }

    const bool observerOnlyMode = HookOverlayObserverOnlyEnabled();
    const bool observerPolicyOnlyMode = HookOverlayObserverPolicyOnlyEnabled();
    if (observerOnlyMode) {
        static std::atomic<int> s_observerOnlyPostSLSkipLogCount{0};
        const int logCount = s_observerOnlyPostSLSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant("DX12: PostSL callback SKIPPED - observer-only mode active (swapchain=%p)",
                             (void*)pSwapChain);
        }
        EnsurePostSLDisabledForObserverOnly(
            "DX12: observer-only PostSL callback",
            ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(
                observerOnlyMode, observerPolicyOnlyMode));
        return;
    }

    if (g_DeviceRemoved.load(std::memory_order_relaxed)) {
        NoteDX12OverlayCoverageGate("device-removed");
        static std::atomic<int> s_deviceRemovedSkipLogCount{0};
        const int logCount = s_deviceRemovedSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DX12: PostSL callback SKIPPED — device already removed (ERR_GFX_STATE detected). "
                "Skipping callback to avoid crash during unstable FG transition.");
        }
        return;
    }

    const bool postSLKeepAliveArmed = g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    IDXGISwapChain* lastSuccessfulPostSLSwapchain = g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldRejectPostSLKeepAliveRenderForUnprovenSwapchain(
            postSLKeepAliveArmed, streamlineFGRunning, lastSuccessfulPostSLSwapchain != nullptr,
            pSwapChain != nullptr && pSwapChain == lastSuccessfulPostSLSwapchain)) {
        NoteDX12OverlayCoverageGate("postsl-keepalive-swapchain-unproven");
        static std::atomic<int> s_unprovenPostSLKeepAliveSwapchainLogCount{0};
        const int logCount = s_unprovenPostSLKeepAliveSwapchainLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: PostSL explicit-OFF keep-alive rejected an unproven swapchain "
                "(current=%p lastSuccessful=%p lastWorkingQueue=%p lockedQueue=%p log=%d)",
                pSwapChain, lastSuccessfulPostSLSwapchain, g_PostSLLastWorkingQueue, g_PostSLLockedQueue, logCount + 1);
        }
        return;
    }

    // A normal command-list submit inside a Streamline wrapper is NOT proof
    // that presentation ownership left the proxy: the wrapper may execute that
    // work and then present its exact previously-confirmed PostSL swapchain.
    // Retire here only when the Streamline stack itself is gone. A genuine
    // normal swapchain return is retired separately from authoritative
    // swapchain/queue identity evidence before normal routing begins.
    if (g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire) &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        const bool streamlineGone = !IsStreamlineLoaded();
        if (streamlineGone) {
            g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
            g_PostSLOverlayActive.store(false, std::memory_order_release);
            g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            SetPostSLCallbackInstalled(false, "DX12: PostSL keep-alive retired after Streamline unload");
            return;
        }
    }

    const bool startupTransitionWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool postSLConfirmedRendering = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool startupTopLevelPresentConsumed =
        DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    const bool wrapperProgressObserved =
        g_PostSLSyntheticStartupWrapperProgressCount.load(std::memory_order_acquire) > 0;
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActive = g_PostSLOverlayActive.load(std::memory_order_acquire);
    const bool explicitSetOptionsActivation = HookHasExplicitStreamlineSetOptionsActivation();
    const bool activeDLSSFGRuntimeSignalObserved = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool nullSwapChain = (pSwapChain == nullptr);

    // CRITICAL FIX: When ECL hook triggers callback with nullptr swapchain (due to direct
    // PostSL callback invocation bypassing ProcessFrame), we cannot safely enter the
    // normal PostSLOverlayRender path because:
    // 1. Bootstrap will fail with nullptr swapchain (pSwapChain->GetDesc() crash)
    // 2. Overlay state cannot be properly initialized
    // 3. This leads to "Present STALLED" because PostSL enters warmup but never renders
    //
    // Instead, we should NOT call PostSLOverlayRender with nullptr. The ECL hook has
    // already cleared the startup transition window, so the next normal ProcessFrame
    // call will properly enter PostSLOverlayRenderGated with a valid swapchain and
    // complete activation correctly.
    if (nullSwapChain) {
        static std::atomic<int> s_nullSwapChainSkipLogCount{0};
        const int logCount = s_nullSwapChainSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DX12: PostSL callback SKIPPED — null swapchain passed from ECL hook direct trigger "
                "(startupPending=%d active=%d windowActive=%d confirmed=%d). "
                "Waiting for normal ProcessFrame path with valid swapchain to complete activation.",
                startupActivationPending ? 1 : 0, postSLActive ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
                postSLConfirmedRendering ? 1 : 0);
        }
        // DO NOT call PostSLOverlayRender(nullptr) — it would crash or cause stall
        // The startup window has been cleared by the ECL hook, so the next
        // ProcessFrame call will properly complete activation with a valid swapchain
        return;
    }

    if (ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
            startupTransitionWindowActive, postSLConfirmedRendering, g_HadFSRFGPhase, startupTopLevelPresentConsumed,
            wrapperProgressObserved, explicitSetOptionsActivation, activeDLSSFGRuntimeSignalObserved,
            startupActivationPending, postSLActive)) {
        NoteDX12OverlayCoverageGate("postsl-startup-window-deferral");
        static std::atomic<int> s_postSLStartupWindowCallbackDeferralLogCount{0};
        const int logCount = s_postSLStartupWindowCallbackDeferralLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: PostSL gated callback deferred until startup transition window expires "
                "(startupPending=%d active=%d progress=%d consumed=%d windowActive=%d confirmed=%d "
                "explicitSetOptions=%d activeDLSSSignal=%d)",
                startupActivationPending ? 1 : 0, postSLActive ? 1 : 0, wrapperProgressObserved ? 1 : 0,
                startupTopLevelPresentConsumed ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
                postSLConfirmedRendering ? 1 : 0, explicitSetOptionsActivation ? 1 : 0,
                activeDLSSFGRuntimeSignalObserved ? 1 : 0);
        }
        return;
    }

    g_PostSLCallbackInFlight.fetch_add(1, std::memory_order_acq_rel);
    auto inFlightGuard =
        ce::make_scope_guard([]() { g_PostSLCallbackInFlight.fetch_sub(1, std::memory_order_acq_rel); });

    if (!g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire)) {
        return;
    }

    PostSLOverlayRender(pSwapChain);
}

bool DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(IDXGISwapChain* pSwapChain, const char* source) {
    const bool keepAliveLatched = g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
    if (!pSwapChain || !keepAliveLatched || DXGIShared::WasPostSLOffKeepAlivePrePresentDrawn()) {
        return false;
    }

    ID3D12CommandQueue* lastWorkingQueue = nullptr;
    ID3D12CommandQueue* lockedQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        lastWorkingQueue = g_PostSLLastWorkingQueue;
        lockedQueue = g_PostSLLockedQueue;
    }

    const bool callbackInstalled =
        DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) == &PostSLOverlayRenderGated;
    const bool exactLastSuccessfulSwapchain =
        pSwapChain != nullptr && pSwapChain == g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldDriveExactPostSLOffKeepAliveBeforePresent(
            keepAliveLatched, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
            g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
            ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup(), IsStreamlineLoaded(),
            g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire), callbackInstalled,
            lastWorkingQueue != nullptr || lockedQueue != nullptr, exactLastSuccessfulSwapchain)) {
        return false;
    }

    const uint64_t successfulSubmitSequenceBefore = s_PostSLSuccessfulSubmitSequence;
    PostSLOverlayRenderGated(pSwapChain);
    const uint64_t successfulSubmitSequenceAfter = s_PostSLSuccessfulSubmitSequence;
    const bool submitted = successfulSubmitSequenceAfter != successfulSubmitSequenceBefore;
    if (submitted) {
        DXGIShared::MarkPostSLOffKeepAlivePrePresentDrawn();
    } else {
        NoteDX12OverlayCoverageGate("postsl-pre-routing-exact-off-keepalive-submit-missed");
    }

    static std::atomic<int> s_preRoutingExactOffKeepAliveLogCount{0};
    const int logCount = s_preRoutingExactOffKeepAliveLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Pre-routing exact-proxy PostSL OFF keep-alive submit completed=%d sequence=%llu->%llu "
            "(source=%s sc=%p lastWorking=%p locked=%p log=%d)",
            submitted ? 1 : 0, successfulSubmitSequenceBefore, successfulSubmitSequenceAfter,
            source ? source : "Present", pSwapChain, lastWorkingQueue, lockedQueue, logCount + 1);
    }
    return submitted;
}

// ============================================================
// Steam ECL deferred overlay submission
// ============================================================
// Submit the deferred overlay command list to the specified queue.  Called from
// DetourExecuteCommandLists after Steam's overlay ECL returns, or as fallback
// from DetourPresent after CallOriginalPresent returns.  Submits CE overlay to
// the same queue Steam used, so CE overlay renders after Steam's clear.
// The callerContext distinguishes the two paths for diagnostic logging.
static bool SubmitSteamDeferredOverlay(ID3D12CommandQueue* submitQueue, const char* callerContext) {
    if (!g_steamDeferredOverlay.pending || !g_steamDeferredOverlay.cmdList) {
        return false;
    }

    ID3D12CommandList* list = g_steamDeferredOverlay.cmdList;
    int allocIdx = g_steamDeferredOverlay.allocIdx;

    HookLogImportant("DX12: [%s] Submitting Steam-deferred overlay ECL to queue %p (cmdList=%p, allocIdx=%d)",
                     callerContext ? callerContext : "unknown", submitQueue, list, allocIdx);

    ID3D12CommandList* lists[] = {list};

    // Prefer realECL (raw tracked D3D12 ECL from d3d12core.dll) to bypass all
    // hook layers including FG vtable hooks on this queue.
    ExecuteCommandListsPtr realECL = g_RealD3D12ECL.load(std::memory_order_acquire);
    {
        ScopedCEOverlayECLSubmission ceOverlayECLGuard("Steam-deferred overlay submit");
        if (realECL) {
            realECL(submitQueue, 1, lists);
            HookLog("DX12: [%s] used realECL=%p for ECL submit", callerContext ? callerContext : "unknown",
                    (void*)realECL);
        } else {
            // Use the per-queue original ECL (un-hooked) from the vtable hook.
            // This avoids re-entering DetourExecuteCommandLists via the vtable.
            ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(submitQueue);
            if (original) {
                original(submitQueue, 1, lists);
                HookLog("DX12: [%s] used GetOriginalExecuteCommandLists=%p for ECL submit",
                        callerContext ? callerContext : "unknown", (void*)original);
            } else {
                HookLogImportant("DX12: [%s] WARNING — no original ECL available, using vtable call (will recurse)",
                                 callerContext ? callerContext : "unknown");
                submitQueue->ExecuteCommandLists(1, lists);
            }
        }
    }
    NoteDX12OverlayRendered(DX12OverlayRenderRoute::kNormal);

    // Signal fence immediately (not deferred) since we need to wait before Present.
    if (g_State.fence) {
        UINT64 next = g_State.currentFenceValue + 1;
        HRESULT sigHr = submitQueue->Signal(g_State.fence, next);
        if (SUCCEEDED(sigHr)) {
            g_State.currentFenceValue = next;
            if (allocIdx >= 0 && allocIdx < static_cast<int>(g_State.fenceValues.size())) {
                g_State.fenceValues[allocIdx] = next;
            }
        } else {
            HookLog("DX12: Steam-deferred overlay fence Signal failed hr=0x%08X", (unsigned)sigHr);
        }
    }

    // Clear deferred state
    g_steamDeferredOverlay.pending = false;
    g_steamDeferredOverlay.cmdList = nullptr;
    g_steamDeferredOverlay.allocIdx = -1;
    g_steamDeferredOverlay.eclQueue = nullptr;

    static std::atomic<int> s_deferredSubmitLogCount{0};
    int logNum = s_deferredSubmitLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logNum <= 20 || (logNum % 200) == 0) {
        HookLogImportant("DX12: Steam-deferred overlay submitted #%d (queue=%p, fence=%llu)", logNum, submitQueue,
                         (unsigned long long)g_State.currentFenceValue);
    }

    return true;
}

// Fallback: submit deferred overlay from the Present path if Steam never called ECL.
extern "C" __declspec(dllexport) void DX12_SubmitSteamDeferredOverlay() {
    if (g_steamDeferredOverlay.pending && g_steamDeferredOverlay.eclQueue) {
        SubmitSteamDeferredOverlay(g_steamDeferredOverlay.eclQueue, "fallback");
    }
}

// Steam module path suffix check: returns true if the given module path contains
// "gameoverlayrenderer" (Steam overlay DLL for x64 or x86).
static bool IsSteamOverlayModulePath(const char* modulePath) {
    if (!modulePath || !modulePath[0])
        return false;
    return strstr(modulePath, "gameoverlayrenderer") != nullptr;
}

static bool IsD3D12ModuleAddress(void* address) {
    if (!address) {
        return false;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        !module) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (!GetModuleFileNameA(module, modulePath, MAX_PATH)) {
        return false;
    }

    return strstr(modulePath, "d3d12") != nullptr || strstr(modulePath, "D3D12") != nullptr;
}

static bool ResolveCurrentProcessForeground(HWND* foregroundWindowOut = nullptr, DWORD* foregroundPidOut = nullptr) {
    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    bool processHasForeground = false;
    if (foregroundWindow) {
        GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
        processHasForeground = (foregroundPid == GetCurrentProcessId());
    }
    if (foregroundWindowOut) {
        *foregroundWindowOut = foregroundWindow;
    }
    if (foregroundPidOut) {
        *foregroundPidOut = foregroundPid;
    }
    return processHasForeground;
}

static void ClearFocusLossPendingOverlayFence(const char* reason, UINT64 fenceValue, UINT64 completedValue) {
    UINT64 expected = g_FocusLossPendingOverlayFenceValue.load(std::memory_order_acquire);
    while (expected != 0 && expected <= fenceValue) {
        if (g_FocusLossPendingOverlayFenceValue.compare_exchange_weak(expected, 0, std::memory_order_acq_rel)) {
            HookLogImportant("DX12: Focus-loss overlay fence hold cleared (%s fence=%llu completed=%llu)",
                             reason ? reason : "unknown", (unsigned long long)fenceValue,
                             (unsigned long long)completedValue);
            return;
        }
    }
}

static bool ShouldHoldOverlayDrawForPendingFocusLossFence() {
    const UINT64 pendingFenceValue = g_FocusLossPendingOverlayFenceValue.load(std::memory_order_acquire);
    if (pendingFenceValue == 0) {
        return false;
    }

    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    if (processHasForeground) {
        ClearFocusLossPendingOverlayFence("process foreground restored", pendingFenceValue, 0);
        return false;
    }

    ID3D12Fence* fence = g_State.fence;
    if (!fence) {
        ClearFocusLossPendingOverlayFence("overlay fence unavailable", pendingFenceValue, 0);
        return false;
    }

    const UINT64 completedValue = fence->GetCompletedValue();
    const bool pendingFenceComplete = completedValue >= pendingFenceValue;
    if (pendingFenceComplete) {
        ClearFocusLossPendingOverlayFence("pending fence completed", pendingFenceValue, completedValue);
        return false;
    }

    if (ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(processHasForeground, true,
                                                                                       pendingFenceComplete)) {
        static std::atomic<int> s_focusLossHoldLogCount{0};
        const int logCount = s_focusLossHoldLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Holding focus-loss overlay/capture backbuffer work until prior overlay fence completes "
                "(fence=%llu completed=%llu fg=%p/%lu log=%d); backend/resources preserved",
                (unsigned long long)pendingFenceValue, (unsigned long long)completedValue, foregroundWindow,
                foregroundPid, logCount + 1);
        }
        return true;
    }

    return false;
}

static const char* DescribeFocusLossImmediateFenceSkip(bool isWrappedD3D12Present, bool isFullscreen,
                                                       bool processHasForeground, bool isIconic, bool hasZeroSize,
                                                       bool overlaySubmitSucceeded, bool deviceLost,
                                                       bool frameGenerationActive, bool runtimeOwnedPresentation,
