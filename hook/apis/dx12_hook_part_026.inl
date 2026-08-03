        static bool s_prevSLFG = false;
        if (s_prevSLFG && !slFGNow) {
            s_postFGOffFrames.store(0, std::memory_order_release);
        }
        s_prevSLFG = slFGNow;

        int pfCount = s_postFGOffFrames.load(std::memory_order_acquire);
        if (pfCount >= 0 && pfCount < 300) {
            s_postFGOffFrames.store(pfCount + 1, std::memory_order_release);
            auto* pfDev = g_Device.load(std::memory_order_acquire);
            HRESULT pfDevHr = pfDev ? pfDev->GetDeviceRemovedReason() : E_FAIL;
            // Log first 50 every frame, then every 10th frame up to 300
            if (pfCount < 50 || pfCount % 10 == 0) {
                HookLogImportant(
                    "DX12: PostFGOff-PF #%d (overlayInit=%d syncInit=%d cooldown=%d "
                    "slFG=%d fgActive=%d devRemoved=0x%08X tid=0x%04X)",
                    pfCount + 1, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0,
                    g_FGTransitionCooldown.load(std::memory_order_acquire), slFGNow ? 1 : 0,
                    g_FGCompat.IsFGActive() ? 1 : 0, (unsigned)pfDevHr, GetCurrentThreadId());
            }
            // Immediately abort overlay rendering if device was removed
            if (FAILED(pfDevHr)) {
                HookLogImportant("DX12: PostFGOff-PF #%d DEVICE REMOVED 0x%08X — aborting overlay", pfCount + 1,
                                 (unsigned)pfDevHr);
                g_DeviceRemoved.store(true, std::memory_order_release);
                return;
            }
        }
    }

    bool inResize = g_InSwapchainResizeCleanup.load(std::memory_order_acquire);
    if (!pSwapChain || inResize) {
        HookLog("DX12: ProcessFrame - early return (null=%d, inResize=%d)", !pSwapChain, inResize);
        return;
    }

    DXGI_SWAP_CHAIN_DESC frameDesc = {};
    if (FAILED(pSwapChain->GetDesc(&frameDesc))) {
        HookLog("DX12: ProcessFrame - failed to get swapchain desc (precheck)");
        return;
    }

    {
        DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        bool hasTrackedColorSpace = false;
        const auto encoding = DXGIShared::ResolveSwapChainPresentationEncoding(
            pSwapChain, frameDesc.BufferDesc.Format, &colorSpace, &hasTrackedColorSpace);
        const bool dependsOnPresentationContract = ce::dx12_overlay_policy::IsPresentationContractDependentFormat(
            static_cast<int>(frameDesc.BufferDesc.Format));
        const bool reliableState = encoding != ce::presentation_color::Encoding::Unsupported &&
                                   (!dependsOnPresentationContract || hasTrackedColorSpace);
        if (reliableState) {
            const bool isHdr = ce::presentation_color::IsHDR(encoding);
            const int recordedColorSpace = hasTrackedColorSpace ? static_cast<int>(colorSpace) : -1;
            const bool previousValid = g_LastKnownSwapchainHDRStateValid.load(std::memory_order_acquire);
            const bool stateChanged = !previousValid ||
                                      g_LastKnownSwapchainIsHDR.load(std::memory_order_acquire) != isHdr ||
                                      g_LastKnownSwapchainColorSpace.load(std::memory_order_acquire) !=
                                          recordedColorSpace;
            UpdateLastKnownSwapchainHDRStateCache(frameDesc.BufferDesc.Format, isHdr, recordedColorSpace, true);
            g_OverlayAdapter.SetHDR(isHdr, static_cast<int>(frameDesc.BufferDesc.Format));
            if (g_pSharedMem)
                g_pSharedMem->SetIsHDR(isHdr);
            if (stateChanged) {
                HookLogImportant("DX12: Presentation color state changed format=%d tracked=%d colorSpace=%d "
                                 "encoding=%s hdr=%d",
                                 static_cast<int>(frameDesc.BufferDesc.Format), hasTrackedColorSpace ? 1 : 0,
                                 recordedColorSpace, ce::presentation_color::Describe(encoding), isHdr ? 1 : 0);
            }
        }
    }

    const bool hasOutputWindow = frameDesc.OutputWindow != nullptr;
    const bool outputWindowVisible = hasOutputWindow && IsWindowVisible(frameDesc.OutputWindow) != FALSE;
    if (ce::dx12_overlay_policy::ShouldSkipDX12PresentProcessingForInvisibleWindowSwapchain(hasOutputWindow,
                                                                                            outputWindowVisible)) {
        static std::atomic<int> s_invisibleSwapchainSkipLogCount{0};
        const int logCount = s_invisibleSwapchainSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: Skipping overlay/capture processing for invisible-window swapchain "
                "(sc=%p hwnd=%p size=%ux%u log=%d)",
                pSwapChain, frameDesc.OutputWindow, frameDesc.BufferDesc.Width, frameDesc.BufferDesc.Height,
                logCount + 1);
        }
        return;
    }

    // Track HWND → swapchain mapping for FG-switch recovery
    if (frameDesc.OutputWindow) {
        TrackSwapchainHwnd(pSwapChain, frameDesc.OutputWindow);
    }

    const bool zeroSizedSwapchain = (frameDesc.BufferDesc.Width == 0 || frameDesc.BufferDesc.Height == 0);
    const bool iconicWindow = frameDesc.OutputWindow && IsIconic(frameDesc.OutputWindow);
    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const DWORD currentProcessId = GetCurrentProcessId();
    bool processHasForeground = true;
    if (frameDesc.OutputWindow) {
        foregroundWindow = GetForegroundWindow();
        processHasForeground = false;
        if (foregroundWindow) {
            GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
            processHasForeground = (foregroundPid == currentProcessId);
        }
    }

    // Transition cooldown: after CreateSwapChainForHwnd, pause overlay D3D12
    // work so we don't interfere with the game's internal state machine (FG
    // switch, native limiter teardown, etc.).
    bool inTransitionCooldown = false;
    {
        int64_t cooldownEnd = g_OverlayCooldownUntilQpc.load(std::memory_order_acquire);
        if (cooldownEnd > 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart < cooldownEnd) {
                inTransitionCooldown = true;
            } else {
                g_OverlayCooldownUntilQpc.store(0, std::memory_order_release);
            }
        }
    }

    // Focus tracking: ordinary foreground changes must keep the overlay visible.
    // Invalid swapchain states below still take the heavy suspension path, but
    // Alt+Tab/focus loss alone should not create a blank overlay interval.
    if (frameDesc.OutputWindow) {
        static bool s_focusStateInitialized = false;
        static bool s_gameWasForeground = false;

        bool gameIsForeground = (foregroundWindow == frameDesc.OutputWindow);
        if (!s_focusStateInitialized) {
            s_focusStateInitialized = true;
            s_gameWasForeground = gameIsForeground;
        } else if (gameIsForeground != s_gameWasForeground) {
            s_gameWasForeground = gameIsForeground;
            HookLog("DX12: Foreground changed (gameForeground=%d, hwnd=%p, fg=%p); overlay rendering remains active",
                    gameIsForeground ? 1 : 0, frameDesc.OutputWindow, foregroundWindow);
        }
    }

    // Heavy suspension is reserved for non-drawable swapchain states. A
    // transition cooldown may still gate specific FG routing paths later, but
    // it should not blank a valid game backbuffer during focus/swapchain churn.
    const bool suspendOverlayHeavy =
        ce::dx12_overlay_policy::ShouldHeavySuspendDX12OverlayForSwapchainState(zeroSizedSwapchain, iconicWindow);
    const bool suspendOverlayRender = suspendOverlayHeavy;

    static bool s_swapchainSuspended = false;
    if (suspendOverlayHeavy) {
        if (!s_swapchainSuspended) {
            s_swapchainSuspended = true;
            g_State.overlayInit = false;
            CleanupRTVs();
            HookLog("DX12: ProcessFrame - suspending overlay (w=%u h=%u iconic=%d cooldown=%d)",
                    frameDesc.BufferDesc.Width, frameDesc.BufferDesc.Height, iconicWindow ? 1 : 0,
                    inTransitionCooldown ? 1 : 0);
        }
    } else if (s_swapchainSuspended) {
        s_swapchainSuspended = false;
        HookLog("DX12: ProcessFrame - resuming overlay after drawable swapchain restored");
    }

    // CPU Prerender Limit - apply only to application source frames. FG
    // runtimes also call Present for generated outputs, and waiting for a
    // queue-depth fence from those workers can form a cycle with the runtime's
    // own Present completion.
    float prerenderLimit = GetActivePrerenderLimit();
    if (prerenderLimit >= 0.0f && applicationSourcePresent) {
        int64_t prerenderStartUs = PerfLogger::GetQpcUs();
        ApplyPrerenderLimitDX12(prerenderLimit, frameGenerationPresentationActive);
        perfMetrics.prerenderWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - prerenderStartUs);
    } else if (prerenderLimit >= 0.0f) {
        static std::atomic<int> s_runtimePrerenderSkipLogs{0};
        const int skipCount = s_runtimePrerenderSkipLogs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipCount <= 20 || (skipCount % 600) == 0) {
            HookLogImportant(
                "DX12: Skipping CPU prerender limiter on FG runtime-generated Present #%d "
                "(limit=%.2f currentTid=0x%04X gameTid=0x%04X)",
                skipCount, prerenderLimit, GetCurrentThreadId(), DX12_GetGamePresentThreadId());
        }
    }

    // A post-FSR OFF edge can rotate the top-level Present from the FFX proxy
    // back to an already-existing Streamline proxy without recreating either
    // swapchain. Pointer inequality is therefore not queue-ownership proof.
    // Resolve the exact route before taking the overlay lock or touching any
    // backbuffer: a confirmed Streamline proxy stays on its warm PostSL route;
    // a proven normal swapchain proceeds below; an unknown identity stays
    // GPU-quiet instead of guessing a queue and removing the device.
    bool postFSRNormalRouteExplicitQueueProof = false;
    bool postFSRNormalRouteRememberedSwapchainProof = false;
    bool postFSRNormalRouteOwnershipProven = false;
    bool authoritativeDLSSOffNormalReturnReinitializedThisPresent = false;
    auto routeInactiveDLSSPresentBeforeBackbufferAccess = [&]() -> bool {
        const bool postFSRRecoveryPending = g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
        const bool explicitOffKeepAlivePending = g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
        if (!postFSRRecoveryPending && !explicitOffKeepAlivePending) {
            return false;
        }

        ID3D12CommandQueue* recoverySwapchainQueue = nullptr;
        ID3D12CommandQueue* recoveryOriginalGameQueue = nullptr;
        ID3D12CommandQueue* recoveryPostSLLastWorkingQueue = nullptr;
        ID3D12CommandQueue* recoveryPostSLLockedQueue = nullptr;
        IDXGISwapChain* queueAssociatedSwapchain = nullptr;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            recoverySwapchainQueue = g_SwapchainQueue;
            recoveryOriginalGameQueue = g_OriginalGameQueue;
            recoveryPostSLLastWorkingQueue = g_PostSLLastWorkingQueue;
            recoveryPostSLLockedQueue = g_PostSLLockedQueue;
            queueAssociatedSwapchain = g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire);
        }

        IDXGISwapChain* rememberedOriginalSwapchain =
            g_LastProvenOriginalQueueSwapchain.load(std::memory_order_acquire);
        IDXGISwapChain* lastSuccessfulPostSLSwapchain = g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
        postFSRNormalRouteExplicitQueueProof =
            recoverySwapchainQueue != nullptr && recoveryOriginalGameQueue != nullptr &&
            recoverySwapchainQueue == recoveryOriginalGameQueue && queueAssociatedSwapchain == pSwapChain;
        postFSRNormalRouteRememberedSwapchainProof =
            rememberedOriginalSwapchain != nullptr && pSwapChain == rememberedOriginalSwapchain;
        postFSRNormalRouteOwnershipProven = ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(
            recoverySwapchainQueue != nullptr, recoveryOriginalGameQueue != nullptr,
            recoverySwapchainQueue != nullptr && recoverySwapchainQueue == recoveryOriginalGameQueue,
            queueAssociatedSwapchain == pSwapChain, postFSRNormalRouteRememberedSwapchainProof);

        const bool actualFGActive = IsActualFrameGenerationActive();
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool postSLKeepAliveArmed = explicitOffKeepAlivePending;
        const bool postSLCallbackReady =
            g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire) &&
            DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) == &PostSLOverlayRenderGated;
        const bool hasPostSLRenderQueue =
            recoveryPostSLLastWorkingQueue != nullptr || recoveryPostSLLockedQueue != nullptr;
        const auto recoveryRoute = ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute(
            true, actualFGActive, streamlineFGRunning, postFSRNormalRouteOwnershipProven, postSLKeepAliveArmed,
            postSLCallbackReady, hasPostSLRenderQueue,
            lastSuccessfulPostSLSwapchain != nullptr && pSwapChain == lastSuccessfulPostSLSwapchain);
        if (recoveryRoute == ce::dx12_overlay_policy::InactiveDLSSPresentRoute::kConfirmedPostSLKeepAlive) {
            const bool preRoutingKeepAliveAlreadySubmitted = DXGIShared::WasPostSLOffKeepAlivePrePresentDrawn();
            const bool shouldSubmitKeepAlive =
                ce::dx12_overlay_policy::ShouldSubmitInactiveDLSSExactPostSLKeepAlive(
                    preRoutingKeepAliveAlreadySubmitted);
            const uint64_t successfulSubmitSequenceBefore = s_PostSLSuccessfulSubmitSequence;
            uint64_t successfulSubmitSequenceAfter = successfulSubmitSequenceBefore;
            bool fallbackKeepAliveDrawSucceeded = false;
            if (shouldSubmitKeepAlive) {
                const bool previousInlineCoverageOwner = g_PostSLDrawBelongsToEnclosingProcessFramePresent;
                g_PostSLDrawBelongsToEnclosingProcessFramePresent = true;
                auto inlineCoverageOwnerGuard = ce::make_scope_guard([previousInlineCoverageOwner]() {
                    g_PostSLDrawBelongsToEnclosingProcessFramePresent = previousInlineCoverageOwner;
                });
                PostSLOverlayRenderGated(pSwapChain);
                successfulSubmitSequenceAfter = s_PostSLSuccessfulSubmitSequence;
                fallbackKeepAliveDrawSucceeded =
                    successfulSubmitSequenceAfter != successfulSubmitSequenceBefore;
                if (fallbackKeepAliveDrawSucceeded) {
                    DXGIShared::MarkPostSLOffKeepAlivePrePresentDrawn();
                } else {
                    NoteDX12OverlayCoverageGate("postsl-exact-off-keepalive-submit-missed");
                }
            }
            const bool keepAliveCoverageSucceeded =
                preRoutingKeepAliveAlreadySubmitted || fallbackKeepAliveDrawSucceeded;

            static std::atomic<int> s_confirmedPostSLProxyKeepAliveLogCount{0};
            const int logCount = s_confirmedPostSLProxyKeepAliveLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Inactive-DLSS Present remains on exact confirmed PostSL proxy — keep-alive "
                    "coverage completed=%d preRouting=%d fallbackSubmit=%d sequence=%llu->%llu "
                    "(sc=%p lastWorking=%p locked=%p origGame=%p; no copy/reinit/wait log=%d)",
                    keepAliveCoverageSucceeded ? 1 : 0, preRoutingKeepAliveAlreadySubmitted ? 1 : 0,
                    fallbackKeepAliveDrawSucceeded ? 1 : 0, successfulSubmitSequenceBefore,
                    successfulSubmitSequenceAfter, pSwapChain, recoveryPostSLLastWorkingQueue,
                    recoveryPostSLLockedQueue, recoveryOriginalGameQueue, logCount + 1);
            }
            // Streamline's explicit-OFF pass-through does not reliably issue a
            // later callback. The pre-routing hook normally submitted once on the
            // exact previously successful PostSL route; this ProcessFrame route
            // submits only as a fallback if that attempt missed. If Streamline
            // nests synchronously, dxgi_shared suppresses that same-Present
            // duplicate too.
            return true;
        }
        if (recoveryRoute == ce::dx12_overlay_policy::InactiveDLSSPresentRoute::kAwaitNormalOwnershipProof) {
            NoteDX12OverlayCoverageGate(postFSRRecoveryPending ? "postfsr-normal-ownership-unproven"
                                                               : "postsl-off-normal-ownership-unproven");
            static std::atomic<int> s_unprovenPostFSRNormalOwnershipLogCount{0};
            const int logCount = s_unprovenPostFSRNormalOwnershipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Inactive-DLSS Present has no exact queue-ownership proof — keeping normal route GPU quiet "
                    "(sc=%p queueAssociatedSC=%p rememberedOrigSC=%p lastPostSLSC=%p scQueue=%p origGame=%p "
                    "lastWorking=%p "
                    "locked=%p keepAlive=%d callbackReady=%d log=%d)",
                    pSwapChain, queueAssociatedSwapchain, rememberedOriginalSwapchain, lastSuccessfulPostSLSwapchain,
                    recoverySwapchainQueue, recoveryOriginalGameQueue, recoveryPostSLLastWorkingQueue,
                    recoveryPostSLLockedQueue, postSLKeepAliveArmed ? 1 : 0, postSLCallbackReady ? 1 : 0, logCount + 1);
            }
            return true;
        }
        return false;
    };
    if (routeInactiveDLSSPresentBeforeBackbufferAccess()) {
        return;
    }

    // PERFORMANCE FIX: Use try_lock instead of blocking lock_guard
    // This prevents stalling the render thread if another thread holds the lock
    if (!g_OverlayMutex.try_lock()) {
        // Another thread is processing, skip this frame
        if (activeDebugSample) {
            activeDebugSample->flags |= kPresentSampleFlagMutexBusy;
        }
        HookLog("DX12: ProcessFrame - mutex busy, skipping frame");
        return;
    }
    // RAII unlock when we exit
    std::unique_lock<std::recursive_mutex> lock(g_OverlayMutex, std::adopt_lock);

    // Close the only transition race left by the non-blocking overlay-lock
    // acquisition: an OFF callback may have armed recovery after the first
    // route snapshot. Re-evaluate outside the overlay lock so the PostSL
    // render->overlay lock order remains intact and no stale cleanup occurs.
    if (g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire) ||
        g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {
        lock.unlock();
        if (routeInactiveDLSSPresentBeforeBackbufferAccess()) {
            return;
        }
        lock.lock();
    }

    // SAFETY: Check device state after acquiring lock
    if (g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        HookLog("DX12: ProcessFrame - in resize cleanup, returning");
        return;
    }

    UpdateStartupOverlayCompatibilityState();
    bool allowOverlayRender = ApplyOverlayStartupCompatMode(frameDesc.OutputWindow);
    SharedMemoryLayout* observerModeShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const bool observerOnlyMode = IsDX12ObserverOnlyModeActive(observerModeShm);
    const bool observerPolicyOnlyMode = IsDX12ObserverPolicyOnlyModeActive(observerModeShm);
    const bool observerStartupPresentOnlyMode = IsDX12ObserverStartupPresentOnlyModeActive(observerModeShm);
    if (observerOnlyMode) {
        allowOverlayRender = false;
        EnsurePostSLDisabledForObserverOnly(
            "DX12: ProcessFrame observer-only mode",
            ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(
                observerOnlyMode, observerPolicyOnlyMode));
    }

    ULONGLONG postResumeSettleRemainingMs = 0;
    const bool startupOverlayCompatibilityActive = IsStartupOverlayCompatibilityActive();
    const ULONGLONG runtimeOwnedSwapchainActiveMs = (g_FGRuntimeOwnsSwapchain && g_FGRuntimeOwnsSwapchainSince != 0)
                                                        ? (GetTickCount64() - g_FGRuntimeOwnsSwapchainSince)
                                                        : kStartupOverlayPostResumeSettleMs;
    const bool runtimeOwnedSwapchainNeedsExtraResumeSettle =
        ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(
            startupOverlayCompatibilityActive, g_FGRuntimeOwnsSwapchain, runtimeOwnedSwapchainActiveMs,
            kStartupOverlayPostResumeSettleMs, s_startupOverlayCompatSettled.load(std::memory_order_acquire),
            ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff());
    const bool deferOverlayWorkAfterResume = ShouldDelayOverlayInitAfterStartupResumeCompat(
        allowOverlayRender, frameDesc.OutputWindow, runtimeOwnedSwapchainNeedsExtraResumeSettle,
        &postResumeSettleRemainingMs);
    const bool processNeedsStartupOverlayInitDelay = startupOverlayCompatibilityActive;
    if (processNeedsStartupOverlayInitDelay) {
        if ((!allowOverlayRender || deferOverlayWorkAfterResume) &&
            s_startupOverlayActivationStage == StartupOverlayActivationStage::kNone) {
            s_startupOverlayActivationStage = StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit;
        }
    } else {
        ResetStartupOverlayBackendActivationStage();
    }
    DisableDedicatedOverlayQueueForOverlayCompat();
    if (!observerOnlyMode) {
        EnsureDedicatedOverlayQueueForFGCompat();
    }

    // Dedicated overlay queue architecture: when FG is active, overlay commands
    // execute on the overlay queue with CPU-side fence sync to avoid interfering
    // with Streamline.  When FG is not active, commands go on the game queue.

    // FG-SAFE device resolution: avoid COM calls through swapchain when FG is
    // active.  FSR FG wraps the swapchain, and QueryInterface/GetDevice through
    // the wrapper can corrupt FG state (causing a delayed null-deref crash in
    // game code ~2 seconds later when FG activates).
    //
    // Instead, we rely on device/queue already captured by our hook infrastructure:
    //   - g_Device is set from CreateSwapChainForHwnd detour or ECL detour
    //   - g_CommandQueue / g_SwapchainQueue are set from ECL/CreateSwapChain hooks
    //
    // The only thing we need from ProcessFrame is swapchain change tracking.
    // FG-SAFE swapchain change tracking: track pointer value only, no AddRef/Release.
    // FSR FG wraps the swapchain and monitors reference counts. Holding an extra
    // reference prevents FSR FG from properly transitioning, causing a delayed
    // null-deref crash in game code ~2s later when FG activates.

    // Update the FG recency counter BEFORE swapchain change check.
    {
        bool fgNow =
            IsActualFrameGenerationActive() || DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (fgNow)
            g_FramesSinceFGActive = 0;
        else if (g_FramesSinceFGActive < 9999)
            ++g_FramesSinceFGActive;

        int slOffSwapchainGrace = g_SLOffSwapchainReinitGrace.load(std::memory_order_acquire);
        if (slOffSwapchainGrace > 0) {
            g_SLOffSwapchainReinitGrace.store(slOffSwapchainGrace - 1, std::memory_order_release);
        }
    }

    const bool exactPostDLSSOffNormalReturnSwapchainProof =
        g_PostDLSSOffAuthoritativeNormalReturnSwapchain.load(std::memory_order_acquire) == pSwapChain;
    const bool exactPrewarmedPostSLHandoffSwapchainProof =
        g_PrewarmedPostSLHandoffSwapchain.load(std::memory_order_acquire) == pSwapChain;
    const bool processLogicalSwapchainReplacement = ce::dx12_overlay_policy::ShouldProcessLogicalSwapchainReplacement(
        pSwapChain != g_LastSwapChain,
        exactPostDLSSOffNormalReturnSwapchainProof || exactPrewarmedPostSLHandoffSwapchainProof);
    if (processLogicalSwapchainReplacement) {
        if (pSwapChain == g_LastSwapChain &&
            (exactPostDLSSOffNormalReturnSwapchainProof || exactPrewarmedPostSLHandoffSwapchainProof)) {
            HookLogImportant(
                "[OVERLAY VISIBILITY] Authoritative %s swapchain creation reused the previous COM pointer "
                "address; processing it as a new lifetime (swapchain=%p)",
                exactPostDLSSOffNormalReturnSwapchainProof ? "native-return" : "prewarmed-PostSL", pSwapChain);
        }
        bool deferredFreshStreamlineNoFGSwapchainCleanup = false;
        bool preserveConfirmedPostSLSwapchainChange = false;
        if (g_LastSwapChain) {
            const auto runtimeMode = g_FGCompat.GetRuntimeMode();
            const uint32_t streamlineNoFGPresentCount =
                g_RuntimeOwnedStreamlineNoFGPresentCount.load(std::memory_order_acquire);
            const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            deferredFreshStreamlineNoFGSwapchainCleanup =
                ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
                    g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, streamlineNoFGPresentCount,
                    kRuntimeOwnedStreamlineNoFGSettlePresents);
            const bool preserveLiveStreamlineNoFGOverlayResources =
                ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
            ID3D12CommandQueue* preserveSwapchainQueue = nullptr;
            ID3D12CommandQueue* preserveOriginalGameQueue = nullptr;
            ID3D12CommandQueue* preserveCommandQueue = nullptr;
            ID3D12CommandQueue* preserveLastWorkingPostSLQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                preserveSwapchainQueue = g_SwapchainQueue;
                preserveOriginalGameQueue = g_OriginalGameQueue;
                preserveCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
                preserveLastWorkingPostSLQueue = g_PostSLLastWorkingQueue;
            }
            const bool postSLConfirmedForSwapchainChange = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
            const int postSLStableFramesForSwapchainChange = g_PostSLStableFrameCount.load(std::memory_order_acquire);
            const bool confirmedPostSLBackendWarmupProtected =
                ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLBackendAsWarmupProtected(
                    postSLConfirmedForSwapchainChange, postSLStableFramesForSwapchainChange);
            preserveConfirmedPostSLSwapchainChange =
                ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
                    streamlineFGRunning, postSLConfirmedForSwapchainChange, confirmedPostSLBackendWarmupProtected,
                    g_HadFSRFGPhase, g_FGRuntimeOwnsSwapchain, preserveSwapchainQueue != nullptr,
                    preserveOriginalGameQueue != nullptr,
                    preserveSwapchainQueue != nullptr && preserveOriginalGameQueue != nullptr &&
                        preserveSwapchainQueue != preserveOriginalGameQueue,
                    g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
                    pSwapChain != nullptr &&
                        pSwapChain == g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire),
                    preserveLastWorkingPostSLQueue != nullptr,
                    g_PostSLWarmResumePreservationPending.load(std::memory_order_acquire));
            const bool preserveProtectedOfficialFFXStartupSwapchainChange =
                ce::dx12_overlay_policy::ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(
                    g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire),
                    HasResolvedOfficialFFXStartupPath());
            auto* prewarmedHandoffDevice = g_Device.load(std::memory_order_acquire);
            const bool prewarmedHandoffDeviceRemoved =
                prewarmedHandoffDevice != nullptr && FAILED(prewarmedHandoffDevice->GetDeviceRemovedReason());
            const bool preserveExactPrewarmedPostSLHandoffBackend =
                ce::dx12_overlay_policy::ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(
                    exactPrewarmedPostSLHandoffSwapchainProof, g_State.overlayInit, g_State.syncInit,
                    g_State.rtvDescHeap != nullptr, g_State.cmdList != nullptr, g_FGRuntimeOwnsSwapchain,
                    preserveSwapchainQueue != nullptr,
                    g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire) == pSwapChain,
                    g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
                    prewarmedHandoffDeviceRemoved);
            if (exactPrewarmedPostSLHandoffSwapchainProof) {
                IDXGISwapChain* expectedSwapchain = pSwapChain;
                g_PrewarmedPostSLHandoffSwapchain.compare_exchange_strong(
                    expectedSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
            }
            if (preserveExactPrewarmedPostSLHandoffBackend) {
                HookLogImportant(
                    "[OVERLAY VISIBILITY] First exact prewarmed PostSL handoff Present preserved its ready "
                    "overlay backend (oldSC=%p newSC=%p scQueue=%p origGame=%p cmdQ=%p)",
                    g_LastSwapChain, pSwapChain, preserveSwapchainQueue, preserveOriginalGameQueue,
                    preserveCommandQueue);
            } else if (preserveProtectedOfficialFFXStartupSwapchainChange) {
                // Keep the old backend warm without retargeting it to an unproven nested FFX swapchain.
                // Proxy-backbuffer prework supplies startup visibility until enabled configure resolves
                // the route; the normal backend can then be rebound by the established transition path.
                g_State.cachedSwapChain = nullptr;
                g_State.cachedSC3 = nullptr;
                static std::atomic<int> s_preservedProtectedFFXStartupSwapchainCleanupLogCount{0};
                const int logCount =
                    s_preservedProtectedFFXStartupSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Preserving overlay backend across protected official FFX startup swapchain change "
                        "until enabled ffxConfigure/present-callback proof (oldSC=%p newSC=%p scQueue=%p "
                        "origGame=%p cmdQ=%p log=%d)",
                        g_LastSwapChain, pSwapChain, preserveSwapchainQueue, preserveOriginalGameQueue,
                        preserveCommandQueue, logCount + 1);
                }
            } else if (preserveLiveStreamlineNoFGOverlayResources) {
                g_State.cachedSwapChain = nullptr;
                g_State.cachedSC3 = nullptr;
                static std::atomic<int> s_preservedFreshSLNoFGSwapchainCleanupLogCount{0};
                const int logCount =
                    s_preservedFreshSLNoFGSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Preserving live overlay resources during runtime-inactive Streamline no-FG "
                        "swapchain handoff (oldSC=%p newSC=%p scQueue=%p origGame=%p cmdQ=%p log=%d)",
                        g_LastSwapChain, pSwapChain, g_SwapchainQueue, g_OriginalGameQueue,
                        g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
                }
            } else if (preserveConfirmedPostSLSwapchainChange) {
                g_State.cachedSwapChain = nullptr;
                g_State.cachedSC3 = nullptr;
                static std::atomic<int> s_preservedConfirmedPostSLSwapchainCleanupLogCount{0};
                const int logCount =
                    s_preservedConfirmedPostSLSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Preserving confirmed PostSL backend during active Streamline FG swapchain change "
                        "(oldSC=%p newSC=%p stableFrames=%d warmupProtected=%d hadFSR=%d fgOwned=%d scQueue=%p "
                        "origGame=%p cmdQ=%p lastWorking=%p exactSuccessful=%d cooldown=%d log=%d)",
                        g_LastSwapChain, pSwapChain, postSLStableFramesForSwapchainChange,
                        confirmedPostSLBackendWarmupProtected ? 1 : 0, g_HadFSRFGPhase ? 1 : 0,
                        g_FGRuntimeOwnsSwapchain ? 1 : 0, preserveSwapchainQueue, preserveOriginalGameQueue,
                        preserveCommandQueue, preserveLastWorkingPostSLQueue,
                        pSwapChain == g_LastSuccessfulPostSLSwapchain.load(std::memory_order_relaxed) ? 1 : 0,
                        g_FGTransitionCooldown.load(std::memory_order_acquire), logCount + 1);
                }
            } else if (deferredFreshStreamlineNoFGSwapchainCleanup) {
                static std::atomic<int> s_deferredFreshSLNoFGSwapchainCleanupLogCount{0};
                const int logCount =
                    s_deferredFreshSLNoFGSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Deferring swapchain-change cleanup during fresh runtime-owned Streamline no-FG "
                        "handoff (presentCount=%u settlePresents=%u oldSC=%p newSC=%p scQueue=%p origGame=%p "
                        "cmdQ=%p log=%d)",
                        streamlineNoFGPresentCount, kRuntimeOwnedStreamlineNoFGSettlePresents, g_LastSwapChain,
                        pSwapChain, g_SwapchainQueue, g_OriginalGameQueue,
                        g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
                }
            } else {
                CleanupRTVs();
                {
                    std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
                    g_SharedCaptureD3D12.Reset();
                }
                g_State.overlayInit = false;
                ResetStartupOverlayBackendActivationStage();

                // FG TRANSITION PROTECTION: If FG is currently active (or was recently
                // active per the cooldown), the swapchain change is likely caused by an
                // FG mode switch (e.g., FSR FG → DLSS FG).  SL / FSR runtimes need time
                // to finish initializing before we reinit overlay resources on the new
                // swapchain.  Set a transition cooldown so the reinit path (below) defers
                // until the FG runtime is stable.
                bool fgCurrentlyActive = IsActualFrameGenerationActive() ||
                                         DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                // Also protect if FG was active within the last ~5 seconds (~300 frames).
                // When switching FG modes, the game may disable one FG type many frames
                // before the swapchain actually changes.  Heuristic detection goes inactive
                // immediately, but the swapchain change is delayed.
                constexpr int kFGRecentWindowFrames = 300;
                bool fgRecentlyWasActive = (g_FramesSinceFGActive < kFGRecentWindowFrames);
                ID3D12CommandQueue* currentSwapchainQueue = nullptr;
                ID3D12CommandQueue* currentOriginalGameQueue = nullptr;
                ID3D12CommandQueue* currentCommandQueue = nullptr;
                ID3D12CommandQueue* currentPrimaryQueue = nullptr;
                IDXGISwapChain* currentQueueAssociatedSwapchain = nullptr;
                {
                    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                    currentSwapchainQueue = g_SwapchainQueue;
                    currentOriginalGameQueue = g_OriginalGameQueue;
                    currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
                    currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
                    currentQueueAssociatedSwapchain =
                        g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire);
                }
                postFSRNormalRouteExplicitQueueProof =
                    currentSwapchainQueue != nullptr && currentOriginalGameQueue != nullptr &&
                    currentSwapchainQueue == currentOriginalGameQueue && currentQueueAssociatedSwapchain == pSwapChain;
                postFSRNormalRouteRememberedSwapchainProof =
                    g_LastProvenOriginalQueueSwapchain.load(std::memory_order_acquire) == pSwapChain;
                postFSRNormalRouteOwnershipProven = ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(
                    currentSwapchainQueue != nullptr, currentOriginalGameQueue != nullptr,
                    currentSwapchainQueue != nullptr && currentSwapchainQueue == currentOriginalGameQueue,
                    currentQueueAssociatedSwapchain == pSwapChain, postFSRNormalRouteRememberedSwapchainProof);
                int slOffSwapchainGrace = g_SLOffSwapchainReinitGrace.load(std::memory_order_acquire);
                const bool commandQueueSettledToPrimary =
                    currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
                bool guardSwapchainReinit = ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(
                    fgCurrentlyActive, fgRecentlyWasActive, g_FGTransitionCooldown > 0, slOffSwapchainGrace > 0,
                    g_FGRuntimeOwnsSwapchain, currentSwapchainQueue != nullptr, currentOriginalGameQueue != nullptr,
                    currentSwapchainQueue != nullptr && currentOriginalGameQueue != nullptr &&
                        currentSwapchainQueue != currentOriginalGameQueue);
                const bool immediateReinitAfterNoCallbackFFXTakeover =
                    ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(
                        g_FGCompat.HasDirectFFXApiConfirmation(), g_FGCompat.IsFSRFGApiActive(),
                        g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                        g_FGRuntimeOwnsSwapchain, currentSwapchainQueue != nullptr,
                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                const bool immediateReinitAfterGameSwapchainRecovery =
                    ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff(
                        currentSwapchainQueue != nullptr && g_PostNativeFSROffGameSwapchainRecoveryQueue.load(
                                                                std::memory_order_acquire) == currentSwapchainQueue,
                        g_FGCompat.IsFSRFGApiActive(),
                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                auto* swapchainChangeDevice = g_Device.load(std::memory_order_acquire);
                const bool swapchainChangeDeviceRemoved =
                    swapchainChangeDevice != nullptr && FAILED(swapchainChangeDevice->GetDeviceRemovedReason());
                const bool immediateReinitAfterAuthoritativeDLSSOffNormalReturn =
                    ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(
                        exactPostDLSSOffNormalReturnSwapchainProof, postFSRNormalRouteOwnershipProven,
                        fgCurrentlyActive, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                        g_FGCompat.IsFSRFGApiActive(),
                        g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                        g_FGRuntimeOwnsSwapchain, swapchainChangeDeviceRemoved);
                authoritativeDLSSOffNormalReturnReinitializedThisPresent =
                    immediateReinitAfterAuthoritativeDLSSOffNormalReturn;
                // DLSS-FG SUSPEND (slDLSSGSetOptions(off), proxy stays live): the active-FG
                // preserve path can't fire (streamlineFGRunning already false), so a fresh
                // proxy swapchain pointer on the same live queue used to blank the live overlay
                // for the full 90-frame cooldown. The make-before-break keep-alive latch marks
                // a CONFIRMED PostSL path that is merely suspended (never set during an FSR/
                // native-FG takeover), so reinit the warm backend immediately on its live queue.
                const bool immediateReinitAfterConfirmedPostSLSuspension = ce::dx12_overlay_policy::
                    ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
                        g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire),
                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                        g_FGCompat.IsFSRFGApiActive(),
                        g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                        g_FGRuntimeOwnsSwapchain,
                        currentSwapchainQueue != nullptr && currentCommandQueue != nullptr &&
                            currentSwapchainQueue == currentCommandQueue,
                        // The DLSS-G proxy renders the overlay on g_PostSLLastWorkingQueue
                        // (== scQueue), which persists across a suspend even when the live
                        // wrapper cmdQueue differs. Accept it as the confirmed PostSL queue.
                        currentSwapchainQueue != nullptr && g_PostSLLastWorkingQueue != nullptr &&
                            currentSwapchainQueue == g_PostSLLastWorkingQueue);
                // DLSS-FG OFF over a runtime-owned (FSR-history) swapchain whose ownership latch is
                // STALE: DLSS-PostSL was the actual presenter (change queue == g_PostSLLastWorkingQueue),
                // but the keep-alive could not arm (blocked by runtimeOwnedNativeFGPresentPath), so the
                // suspension predicate above misses it. FSR is not actually presenting (api inactive,
                // present callback quiet), so reinit the warm backend immediately on the same queue
                // instead of the 90-frame cooldown (session 20260614_023730: 89/90-present blanks).
                const ULONGLONG lastFFXCallbackTickMs = g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
                const bool ffxPresentCallbackActiveForDLSSOff =
                    lastFFXCallbackTickMs != 0 && (GetTickCount64() - lastFFXCallbackTickMs) < 1000;
                const bool immediateReinitAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue = ce::dx12_overlay_policy::
                    ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                        g_FGCompat.IsFSRFGApiActive(),
                        g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                        ffxPresentCallbackActiveForDLSSOff, g_FGRuntimeOwnsSwapchain,
                        currentSwapchainQueue != nullptr && g_PostSLLastWorkingQueue != nullptr &&
