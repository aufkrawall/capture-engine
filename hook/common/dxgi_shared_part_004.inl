        }
        if (oPresentBypass) {
            return oPresentBypass(pSwapChain, SyncInterval, Flags);
        }
        // No bypass available - return S_OK to break recursion loop
        return S_OK;
    }

    static int s_entryCount = 0;
    int entryNum = ++s_entryCount;

    // Present-call heartbeat diagnostic:
    // Logs periodically (every 1000th call) and whenever there's a gap >250ms.
    // Purpose: Detect whether the game stops calling Present during menus/pauses.
    //
    // GTA V Enhanced: During pause menu, Present calls stop entirely (10+ second
    // gaps observed).  This means our overlay can't render unless we detect the
    // gap and use an alternative rendering mechanism (like the pre-SL stall
    // fallback in ProcessFrame).
    //
    // Also logs: IsRecursivePresent (SL FG re-entrant calls), g_StreamlineFGRunning
    // (whether SL thinks FG is active), and thread ID (SL uses worker threads).
    {
        static LARGE_INTEGER s_lastPresentTime = {};
        static int s_heartbeatCount = 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (s_lastPresentTime.QuadPart != 0) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            double gapMs = (double)(now.QuadPart - s_lastPresentTime.QuadPart) * 1000.0 / freq.QuadPart;
            static constexpr double kLargePresentGapMs = 250.0;

            // Treat quarter-second Present gaps as scene/load transitions. This
            // is conservative enough to ignore ordinary jitter while still
            // catching save-load handoff disruptions.
            if (gapMs > kLargePresentGapMs || (s_heartbeatCount % 1000 == 0)) {
                if (gapMs > kLargePresentGapMs) {
                    MarkLargePresentGap();
                }
                // READ-ONLY state peek: DO NOT call IsRecursivePresent() here!
                // IsRecursivePresent() has side effects (CAS on g_presentThreadId)
                // and would permanently corrupt the present ownership tracking,
                // making ALL subsequent calls appear recursive and blocking
                // ProcessFrame from ever running again.
                DWORD presentOwner = g_presentThreadId.load(std::memory_order_relaxed);
                int presentDepthVal = g_presentDepth.load(std::memory_order_relaxed);
                HookLogImportant(
                    "DetourPresent: heartbeat #%d gap=%.0fms presentOwner=0x%04X depth=%d slFG=%d tid=0x%04X",
                    s_heartbeatCount, gapMs, presentOwner, presentDepthVal,
                    g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0, GetCurrentThreadId());
            }
        }
        s_lastPresentTime = now;
        s_heartbeatCount++;
    }

    if (entryNum <= 10) {
        HookLog(
            "DetourPresent: ENTRY #%d (pSwapChain=%p, IsInWrapper=%d, "
            "trampoline=%p)",
            entryNum, pSwapChain, IsInWrapperPresent() ? 1 : 0, oPresentTrampoline);
    }

    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    const APIType api = DetectAPIType(pSwapChain);
    if (api == APIType::D3D12 && ShouldBypassDX12InvisibleWindowPresent(pSwapChain, "DetourPresent")) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }
    BeginPostSLOffKeepAlivePresentScope();
    auto postSLOffKeepAlivePresentScopeGuard = ce::make_scope_guard([]() { EndPostSLOffKeepAlivePresentScope(); });
    if (api == APIType::D3D12) {
        DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(pSwapChain,
                                                           "DXGIShared::DetourPresent pre-routing");
    }

    // Capture the caller here, not in a helper. We need the code that called
    // into DetourPresent, not the helper's own return address inside this DLL.
    const void* detourCallerAddress = CE_CAPTURE_RETURN_ADDRESS();
    char detourCallerModulePath[MAX_PATH] = {};
    const bool callerFromThirdPartyOverlay =
        TryGetModulePathFromCodeAddress(detourCallerAddress, detourCallerModulePath, sizeof(detourCallerModulePath)) &&
        ce::overlay_compat::IsThirdPartyOverlayModulePath(detourCallerModulePath);
    const bool streamlineStartupHandoffPending = (api == APIType::D3D12) && IsStreamlineStartupHandoffPending();
    const bool streamlineStartupTransitionWindowActive =
        (api == APIType::D3D12) && IsStreamlineStartupTransitionWindowActive();
    const bool streamlineStartupHandoffInProgress =
        streamlineStartupHandoffPending || streamlineStartupTransitionWindowActive;
    const DWORD currentThreadId = GetCurrentThreadId();
    const DWORD presentOwner = g_presentThreadId.load(std::memory_order_relaxed);
    const int presentDepthVal = g_presentDepth.load(std::memory_order_relaxed);
    const bool presentOwnershipActive = presentOwner != 0 || presentDepthVal > 0;
    const DWORD expectedPresentThreadId = g_RenderWatchdog.GetMonitoredThreadId();
    const bool matchesExpectedPresentThread =
        expectedPresentThreadId == 0 || expectedPresentThreadId == currentThreadId;
    const bool callerFromStreamlineModule = IsCodeAddressFromStreamlineModule(detourCallerAddress);
    const bool callerFromFFXFrameGenerationModule =
        ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(detourCallerAddress);
    const bool recentLargePresentGap = HasRecentLargePresentGap(500);
    const bool startupTopLevelPresentAlreadyConsumed =
        g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    const bool postSLStartupActivationPending =
        g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActiveButUnconfirmed = api == APIType::D3D12 && HookIsPostSLOverlayActiveButUnconfirmed();
    const bool postSLStartupActivationEntered =
        api == APIType::D3D12 && HookHasPostSLSyntheticStartupActivationEntered();
    const bool postSLConfirmedRendering = api == APIType::D3D12 && HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling =
        api == APIType::D3D12 && HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool hadFSRFGPhase = api == APIType::D3D12 && HookHasFSRFGHistory();
    const bool explicitSetOptionsActivation = api == APIType::D3D12 && HookHasExplicitStreamlineSetOptionsActivation();
    const bool activeDLSSFGRuntimeSignalObserved =
        api == APIType::D3D12 && g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool safePostFSRBootstrapPath = api == APIType::D3D12 && HookHasSafePostFSRBootstrapPath();
    const bool steamOverlayLoaded = IsSteamOverlayModule(ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName());
    if (s_externalOverlayPresentInvokeDepth > 0) {
        PFN_Present recursiveBypass = EnsurePresentBypassTrampoline();
        if (DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(true, recursiveBypass != nullptr)) {
            static std::atomic<int> s_recursiveExternalOverlayBypassLogCount{0};
            const int bypassNum = s_recursiveExternalOverlayBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent: Bypassing recursive external-overlay Present #%d while guarded Steam hook is "
                    "active (depth=%d bypass=%p tid=0x%04X)",
                    bypassNum, s_externalOverlayPresentInvokeDepth, (void*)recursiveBypass, currentThreadId);
            }
            return recursiveBypass(pSwapChain, SyncInterval, Flags);
        }
    }
    // Log Steam overlay state once for diagnostics.
    static std::atomic<uint32_t> s_steamStateLogCount{0};
    if (s_steamStateLogCount.fetch_add(1, std::memory_order_relaxed) == 0) {
        const char* overlayModuleName = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
        HookLogImportant(
            "DetourPresent: Steam overlay state: steamLoaded=%d overlayModule=%s g_externalOverlayHook=%p "
            "oPresentTrampoline=%p oPresentBypass=%p slLoaded=%d streamlineFGRunning=%d",
            steamOverlayLoaded ? 1 : 0, overlayModuleName ? overlayModuleName : "none",
            (void*)g_externalOverlayPresentHook, (void*)oPresentTrampoline, (void*)oPresentBypass,
            IsSLInterposerLoaded() ? 1 : 0, g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0);
    }
    const bool runtimeOwnedSwapchainActive = api == APIType::D3D12 && DoesFGRuntimeOwnSwapchain();
    const bool presentBypassAvailable = EnsurePresentBypassTrampoline() != nullptr;
    const bool staleThirdPartyPresentHookRisk =
        api == APIType::D3D12 && ShouldForceSteamDX12Bypass(pSwapChain, presentBypassAvailable, IsSLInterposerLoaded());
    const bool observerOnlyMode = HookOverlayObserverOnlyEnabled();
    const bool observerStartupPresentOnlyMode = HookOverlayObserverStartupPresentOnlyEnabled();
    const bool ffxStartupBypass = ShouldBypassFFXPresentDuringStreamlineStartup(
        api == APIType::D3D12, callerFromFFXFrameGenerationModule,
        streamlineStartupHandoffPending, streamlineStartupTransitionWindowActive, observerOnlyMode,
        observerStartupPresentOnlyMode);
    if (ffxStartupBypass) {
        g_FGCompat.SetFSRFGSupportPresent(true);
        PFN_Present presentBypass = EnsurePresentBypassTrampoline();
        if (presentBypass) {
            static std::atomic<int> s_ffxStartupBypassLogCount{0};
            int bypassNum = s_ffxStartupBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 200) == 0) {
                HookLogImportant(
                    "DetourPresent: Treating FFX-originated Present as startup handoff bypass #%d (bypass=%p, "
                    "tid=0x%04X)",
                    bypassNum, (void*)presentBypass, GetCurrentThreadId());
            }
            return presentBypass(pSwapChain, SyncInterval, Flags);
        }
    }
    bool streamlineSyntheticReentrant =
        ShouldAllowSpecialStreamlinePresentRouting(observerOnlyMode) &&
        ShouldTreatStreamlinePresentAsSyntheticReentrant(
            api == APIType::D3D12, streamlineFGRunning, callerFromStreamlineModule, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, streamlineStartupHandoffInProgress, presentOwnershipActive,
            recentLargePresentGap, matchesExpectedPresentThread, startupTopLevelPresentAlreadyConsumed);
    const bool startupTopLevelCandidate = DXGIShared::ShouldUseStreamlineStartupTopLevelCandidate(
        observerOnlyMode, streamlineSyntheticReentrant, callerFromStreamlineModule, api == APIType::D3D12,
        streamlineFGRunning, streamlineStartupHandoffInProgress, recentLargePresentGap, matchesExpectedPresentThread,
        postSLConfirmedRendering);
    const bool stalePostFSRStartupHandoffPresentHookRisk =
        api == APIType::D3D12 && ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
                                     presentBypassAvailable, steamOverlayLoaded, api == APIType::D3D12,
                                     inWrapperPresent, wrappedSwapchain, hadFSRFGPhase, startupTopLevelCandidate);
    const bool startupHandoffSteamRisk = staleThirdPartyPresentHookRisk || stalePostFSRStartupHandoffPresentHookRisk;
    const bool postFSRRuntimeStartupHandoffRisk = ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
        api == APIType::D3D12, hadFSRFGPhase, startupTopLevelCandidate, safePostFSRBootstrapPath,
        startupHandoffSteamRisk);
    const bool streamlineStartupHandoffTransportRisk = ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(
        api == APIType::D3D12, inWrapperPresent, wrappedSwapchain, presentBypassAvailable, callerFromStreamlineModule,
        streamlineStartupHandoffInProgress, runtimeOwnedSwapchainActive, startupTopLevelCandidate,
        postFSRRuntimeStartupHandoffRisk, startupHandoffSteamRisk);
    if (startupTopLevelCandidate) {
        bool expected = false;
        if (g_SharedState.streamlineStartupTopLevelPresentConsumed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            static std::atomic<int> s_streamlineStartupSuppressedTopLevelLogCount{0};
            int logCount = s_streamlineStartupSuppressedTopLevelLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: Keeping Streamline startup-handoff Present on the normal SL route #%d "
                    "(owner=0x%04X depth=%d expectedTid=0x%04X currentTid=0x%04X recentGap=1)"
                    " — top-level promotion disabled; relying on startup-policy + wrapper-progress activation",
                    logCount, presentOwner, presentDepthVal, expectedPresentThreadId, currentThreadId);
            }
        }

        if (ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(
                api == APIType::D3D12, startupTopLevelCandidate, streamlineStartupHandoffTransportRisk,
                postFSRRuntimeStartupHandoffRisk || startupHandoffSteamRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline startup-handoff Present");
            if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(
                    api == APIType::D3D12, postSLConfirmedRendering)) {
                DX12_RetainStreamlineStartupActivationSwapchain(
                    pSwapChain, "DetourPresent: startup-handoff normal-route transport");
            }
            bool exactStartupTransportDrawn = false;
            if (DXGIShared::ShouldRenderExactPostSLBeforeStartupHandoffTransport(
                    api == APIType::D3D12, hadFSRFGPhase, safePostFSRBootstrapPath, streamlineFGRunning,
                    startupTopLevelCandidate, postSLConfirmedRendering)) {
                exactStartupTransportDrawn = DX12_TryRenderExactPostSLBeforeStartupHandoffPresent(
                    pSwapChain, "DetourPresent/startup-handoff-transport");
            }
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,
                                                            "Streamline startup-handoff Present", &guardedSteamHr)) {
                if (SUCCEEDED(guardedSteamHr)) {
                    DX12_AccountOverlayTransportPresent(exactStartupTransportDrawn,
                                                        "streamline-startup-handoff-transport",
                                                        "DetourPresent/guarded-startup-handoff");
                }
                return guardedSteamHr;
            }

            PFN_Present presentBypass = EnsurePresentBypassTrampoline();
            if (presentBypass) {
                static std::atomic<int> s_streamlineStartupHandoffBypassLogCount{0};
                int bypassCount = s_streamlineStartupHandoffBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent: Streamline startup-handoff normal-route bypass #%d "
                        "(hadFSR=%d runtimeOwnsSwapchain=%d transportRisk=%d fsrHandoffRisk=%d steamRisk=%d "
                        "startupPending=%d confirmed=%d settling=%d bypass=%p owner=0x%04X depth=%d tid=0x%04X)",
                        bypassCount, hadFSRFGPhase ? 1 : 0, runtimeOwnedSwapchainActive ? 1 : 0,
                        streamlineStartupHandoffTransportRisk ? 1 : 0, postFSRRuntimeStartupHandoffRisk ? 1 : 0,
                        startupHandoffSteamRisk ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                        postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                        (void*)presentBypass, presentOwner, presentDepthVal, currentThreadId);
                }
                MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(pSwapChain, api == APIType::D3D12,
                                                                   streamlineFGRunning, postSLConfirmedRendering,
                                                                   hadFSRFGPhase, "startupHandoffNormalRoute");
                const HRESULT hr = presentBypass(pSwapChain, SyncInterval, Flags);
                if (SUCCEEDED(hr)) {
                    DX12_AccountOverlayTransportPresent(exactStartupTransportDrawn,
                                                        "streamline-startup-handoff-transport",
                                                        "DetourPresent/startup-handoff-bypass");
                }
                return hr;
            }
        } else if (runtimeOwnedSwapchainActive) {
            static std::atomic<int> s_streamlineStartupHandoffNormalTransportAllowedLogCount{0};
            int allowedCount =
                s_streamlineStartupHandoffNormalTransportAllowedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (allowedCount <= 10 || (allowedCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: Streamline startup-handoff normal-route transport allowed #%d "
                    "(hadFSR=%d runtimeOwnsSwapchain=%d transportRisk=%d fsrHandoffRisk=%d steamRisk=%d "
                    "startupPending=%d "
                    "confirmed=%d settling=%d owner=0x%04X depth=%d tid=0x%04X)",
                    allowedCount, hadFSRFGPhase ? 1 : 0, runtimeOwnedSwapchainActive ? 1 : 0,
                    streamlineStartupHandoffTransportRisk ? 1 : 0, postFSRRuntimeStartupHandoffRisk ? 1 : 0,
                    startupHandoffSteamRisk ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                    postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0, presentOwner,
                    presentDepthVal, currentThreadId);
            }
        }
    }
    const bool keepStartupPresentOnNormalRoute = DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(
        observerOnlyMode, hadFSRFGPhase, explicitSetOptionsActivation, safePostFSRBootstrapPath,
        g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire),
        callerFromStreamlineModule, postSLStartupActivationPending, postSLActiveButUnconfirmed,
        postSLConfirmedButStartupSettling, streamlineSyntheticReentrant);
    const bool stalePostFSRStartupNormalRoutePresentHookRisk =
        api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
            presentBypassAvailable, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent, wrappedSwapchain,
            hadFSRFGPhase, keepStartupPresentOnNormalRoute);
    const bool startupNormalRouteSteamRisk =
        staleThirdPartyPresentHookRisk || stalePostFSRStartupNormalRoutePresentHookRisk;
    const bool postFSRRuntimeStartupNormalRouteRisk =
        api == APIType::D3D12 && hadFSRFGPhase && safePostFSRBootstrapPath && keepStartupPresentOnNormalRoute;
    const bool streamlineStartupNormalRouteTransportRisk = ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(
        api == APIType::D3D12, inWrapperPresent, wrappedSwapchain, presentBypassAvailable, callerFromStreamlineModule,
        streamlineStartupHandoffInProgress, runtimeOwnedSwapchainActive, keepStartupPresentOnNormalRoute,
        postFSRRuntimeStartupNormalRouteRisk, startupNormalRouteSteamRisk);
    if (keepStartupPresentOnNormalRoute) {
        const bool shouldInvokePostSLCallbackOnNormalRoute =
            DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
                observerOnlyMode, hadFSRFGPhase, explicitSetOptionsActivation, activeDLSSFGRuntimeSignalObserved,
                safePostFSRBootstrapPath, postSLStartupActivationPending, postSLActiveButUnconfirmed,
                postSLStartupActivationEntered, postSLConfirmedButStartupSettling, streamlineSyntheticReentrant);
        static std::atomic<int> s_streamlineSyntheticStartupNormalRouteLogCount{0};
        int logCount = s_streamlineSyntheticStartupNormalRouteLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DetourPresent: Keeping decisive synthetic Streamline startup Present on the normal SL route #%d "
                "(startupPending=%d unconfirmed=%d activationEntered=%d settling=%d callbackOnNormal=%d consumed=%d "
                "hadFSR=%d activeDLSSSignal=%d windowActive=%d tid=0x%04X)",
                logCount, postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                postSLStartupActivationEntered ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                shouldInvokePostSLCallbackOnNormalRoute ? 1 : 0,
                g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_relaxed) ? 1 : 0,
                hadFSRFGPhase ? 1 : 0, activeDLSSFGRuntimeSignalObserved ? 1 : 0,
                streamlineStartupTransitionWindowActive ? 1 : 0, currentThreadId);
        }
        if (shouldInvokePostSLCallbackOnNormalRoute) {
            auto postSLCallback = g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
            if (postSLCallback) {
                static std::atomic<int> s_unconfirmedStartupNormalRouteCallbackLogCount{0};
                const int callbackLogCount =
                    s_unconfirmedStartupNormalRouteCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
                if (postSLStartupActivationEntered && postSLActiveButUnconfirmed &&
                    (callbackLogCount < 10 || (callbackLogCount % 100) == 0)) {
                    HookLogImportant(
                        "DetourPresent: Invoking PostSL on activated-but-unconfirmed Streamline startup normal route "
                        "#%d (startupPending=%d hadFSR=%d owner=0x%04X depth=%d tid=0x%04X)",
                        callbackLogCount + 1, postSLStartupActivationPending ? 1 : 0, hadFSRFGPhase ? 1 : 0,
                        presentOwner, presentDepthVal, currentThreadId);
                }
                if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(
                        api == APIType::D3D12, true, postSLStartupActivationPending, postSLActiveButUnconfirmed,
                        postSLConfirmedRendering)) {
                    DX12_RetainStreamlineStartupActivationSwapchain(
                        pSwapChain, "DetourPresent: startup normal-route PostSL callback");
                }
                postSLCallback(pSwapChain);
            }
        } else {
            static std::atomic<int> s_skipPostSLCallbackLogCount{0};
            int skipCount = s_skipPostSLCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
            if (skipCount < 5 || (skipCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: PostSL callback skipped on normal route despite startup present kept "
                    "(startupPending=%d unconfirmed=%d activationEntered=%d hadFSR=%d explicitSetOptions=%d "
                    "activeDLSSSignal=%d safeBootstrap=%d tid=0x%04X)",
                    postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                    postSLStartupActivationEntered ? 1 : 0, hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivation ? 1 : 0,
                    activeDLSSFGRuntimeSignalObserved ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0, currentThreadId);
            }
        }
        if (DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(
                api == APIType::D3D12, keepStartupPresentOnNormalRoute, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, streamlineStartupNormalRouteTransportRisk,
                startupNormalRouteSteamRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline startup normal-route Present");
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(
                    pSwapChain, SyncInterval, Flags, "Streamline startup normal-route Present", &guardedSteamHr)) {
                return guardedSteamHr;
            }

            PFN_Present presentBypass = EnsurePresentBypassTrampoline();
            if (presentBypass) {
                static std::atomic<int> s_streamlineStartupNormalRouteBypassLogCount{0};
                int bypassCount =
                    s_streamlineStartupNormalRouteBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent: Streamline startup normal-route bypass #%d "
                        "(hadFSR=%d runtimeOwnsSwapchain=%d transportRisk=%d steamRisk=%d "
                        "startupPending=%d unconfirmed=%d confirmed=%d settling=%d bypass=%p owner=0x%04X depth=%d "
                        "tid=0x%04X)",
                        bypassCount, hadFSRFGPhase ? 1 : 0, runtimeOwnedSwapchainActive ? 1 : 0,
                        streamlineStartupNormalRouteTransportRisk ? 1 : 0, startupNormalRouteSteamRisk ? 1 : 0,
                        postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                        postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                        (void*)presentBypass, presentOwner, presentDepthVal, currentThreadId);
                }
                if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(
                        api == APIType::D3D12, postSLConfirmedRendering)) {
                    DX12_RetainStreamlineStartupActivationSwapchain(pSwapChain,
                                                                    "DetourPresent: startup normal-route bypass");
                }
                MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(pSwapChain, api == APIType::D3D12,
                                                                   streamlineFGRunning, postSLConfirmedRendering,
                                                                   hadFSRFGPhase, "keepStartupNormalRoute");
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        } else if (runtimeOwnedSwapchainActive && callerFromStreamlineModule) {
            static std::atomic<int> s_streamlineStartupNormalTransportAllowedLogCount{0};
            int allowedCount =
                s_streamlineStartupNormalTransportAllowedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (allowedCount <= 10 || (allowedCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: Streamline startup normal-route transport allowed #%d "
                    "(hadFSR=%d runtimeOwnsSwapchain=%d transportRisk=%d steamRisk=%d startupPending=%d "
                    "unconfirmed=%d confirmed=%d settling=%d owner=0x%04X depth=%d tid=0x%04X)",
                    allowedCount, hadFSRFGPhase ? 1 : 0, runtimeOwnedSwapchainActive ? 1 : 0,
                    streamlineStartupNormalRouteTransportRisk ? 1 : 0, startupNormalRouteSteamRisk ? 1 : 0,
                    postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                    postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0, presentOwner,
                    presentDepthVal, currentThreadId);
            }
        }
        streamlineSyntheticReentrant = false;
    }
    const bool shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute =
        DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
            observerOnlyMode, api == APIType::D3D12, streamlineFGRunning, callerFromStreamlineModule,
            postSLConfirmedRendering, postSLConfirmedButStartupSettling, presentOwnershipActive,
            streamlineSyntheticReentrant);
    const bool stalePostFSRConfirmedStandalonePresentHookRisk =
        api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
            EnsurePresentBypassTrampoline() != nullptr, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent,
            wrappedSwapchain, hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute);
    if (shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute) {
        auto postSLCallback = g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback) {
            static std::atomic<int> s_confirmedStandaloneNormalRouteCallbackLogCount{0};
            int logCount = s_confirmedStandaloneNormalRouteCallbackLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: Invoking PostSL on confirmed standalone Streamline Present while keeping the "
                    "normal "
                    "SL route #%d (settling=%d owner=0x%04X depth=%d tid=0x%04X)",
                    logCount, postSLConfirmedButStartupSettling ? 1 : 0, presentOwner, presentDepthVal,
                    currentThreadId);
            }
            postSLCallback(pSwapChain);
        }

        if (DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(
                api == APIType::D3D12, hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute,
                staleThirdPartyPresentHookRisk || stalePostFSRConfirmedStandalonePresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR confirmed standalone Present");
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,
                                                            "post-FSR confirmed standalone Present", &guardedSteamHr)) {
                return guardedSteamHr;
            }

            PFN_Present presentBypass = EnsurePresentBypassTrampoline();
            if (presentBypass) {
                static std::atomic<int> s_confirmedStandaloneNormalRouteBypassLogCount{0};
                int bypassCount =
                    s_confirmedStandaloneNormalRouteBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent: Post-FSR confirmed standalone normal-route bypass #%d "
                        "(owner=0x%04X depth=%d tid=0x%04X)",
                        bypassCount, presentOwner, presentDepthVal, currentThreadId);
                }
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        }
    }
    if (streamlineSyntheticReentrant) {
        auto postSLCallback =
            observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback && !WasPostSLOffKeepAlivePrePresentDrawn()) {
            postSLCallback(pSwapChain);
        } else if (postSLCallback) {
            static std::atomic<int> s_postSLOffKeepAliveNestedDedupLogCount{0};
            const int logCount = s_postSLOffKeepAliveNestedDedupLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DetourPresent: Skipping nested PostSL callback because the exact-proxy explicit-OFF "
                    "keep-alive already drew before this Present (sc=%p log=%d)",
                    pSwapChain, logCount + 1);
            }
        }

        if (api == APIType::D3D12) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline synthetic Present");
        }

        // Service the deferred ECL probe: ProcessFrame may be dormant during
        // synthetic re-entrant Present routing, so the ProcessFrame-based
        // deferred probe check would never fire here.  The ECL detour also
        // services it, but may not fire if PostSL submits are being skipped.
        DX12_ServiceDeferredECLProbe();

        HRESULT guardedSteamHr = S_OK;
        if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags, "Streamline synthetic Present",
                                                        &guardedSteamHr)) {
            return guardedSteamHr;
        }

        PFN_Present presentBypass = EnsurePresentBypassTrampoline();
        if (presentBypass) {
            static std::atomic<int> s_streamlineSyntheticPresentLogCount{0};
            int syntheticNum = s_streamlineSyntheticPresentLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (syntheticNum <= 10 || syntheticNum == 50 || (syntheticNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #%d "
                    "(postSL=%p, bypass=%p, tid=0x%04X)",
                    syntheticNum, (void*)postSLCallback, (void*)presentBypass, GetCurrentThreadId());
            }
            return presentBypass(pSwapChain, SyncInterval, Flags);
        }
    }

    g_SharedState.presentInFlightDepth.fetch_add(1, std::memory_order_acq_rel);
    auto presentInFlightGuard =
        ce::make_scope_guard([]() { g_SharedState.presentInFlightDepth.fetch_sub(1, std::memory_order_acq_rel); });

    if (IsShuttingDown()) {
        if (IsReadableMemory(pSwapChain, sizeof(void*))) {
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!IsReadableMemory(pSwapChain, sizeof(void*))) {
        RequestHookShutdown();
        return DXGI_ERROR_INVALID_CALL;
    }
    void** vtable = *(void***)pSwapChain;
    if (!vtable || !IsReadableMemory(reinterpret_cast<const void*>(vtable), 9 * sizeof(void*)) || !vtable[8]) {
        RequestHookShutdown();
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!oPresentTrampoline && !oPresent) {
        HookLog("DetourPresent: No original Present function available");
        return DXGI_ERROR_INVALID_CALL;
    }

    // Apply SetMaximumFrameLatency override (must be BEFORE wrapper/recursive checks)
    ApplyPresentFrameLatencyOverrides(pSwapChain);

    // Query-based CPU prerender limit for D3D11 (fallback when IDXGISwapChain2 is unavailable).
    // Applied for all D3D11 games regardless of wrapper/vtable path.
    if (api == APIType::D3D11 && g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        float prerenderLimit = GetActivePrerenderLimit();
        if (prerenderLimit >= 0.0f) {
            ApplyPrerenderLimit(pSwapChain, prerenderLimit);
        }
    }

    if (wrappedSwapchain) {
        if (api == APIType::D3D12) {
            DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(
                pSwapChain, "DXGIShared::DetourPresent wrapped-swapchain pass-through");
        }
        static int s_wrappedPassCount = 0;
        if (s_wrappedPassCount < 5) {
            s_wrappedPassCount++;
            HookLogImportant("DetourPresent: WRAPPED swapchain early return #%d", s_wrappedPassCount);
        }
        HRESULT wrappedHr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        if (api == APIType::D3D12) {
            InvokeDX12FlushDeferredSignal();
        }
        if (SUCCEEDED(wrappedHr)) {
            g_SharedFpsLimiter.ApplyPostPresent();
        }
        return wrappedHr;
    }

    if (inWrapperPresent) {
        if (api == APIType::D3D12) {
            DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(
                pSwapChain, "DXGIShared::DetourPresent wrapper re-entry pass-through");
        }
        static int s_inWrapperPassCount = 0;
        if (s_inWrapperPassCount < 5) {
            s_inWrapperPassCount++;
            HookLogImportant("DetourPresent: IsInWrapperPresent early return #%d", s_inWrapperPassCount);
        }
        HRESULT wrapperHr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        if (api == APIType::D3D12) {
            InvokeDX12FlushDeferredSignal();
        }
        if (SUCCEEDED(wrapperHr)) {
            g_SharedFpsLimiter.ApplyPostPresent();
        }
        return wrapperHr;
    }

    // Re-entrant Present call. When SL is loaded, calling oPresent enters SL's
    // E9 hook, and SL may call pSwapChain->Present() via the vtable for FG frames.
    // That vtable call hits Steam → DetourPresent → re-entrant. If we forward
    // to oPresent here, it re-enters SL → infinite loop / stack overflow.
    //
    // Solution: use the bypass trampoline which executes original Present bytes
    // from disk, jumping past SL's E9 JMP. This actually presents the frame
    // without re-entering the external hook chain.
    if (IsRecursivePresent()) {
        // DEBUG: Log that we're treating this as recursive
        static std::atomic<int> s_recurseCount{0};
        int rc = s_recurseCount.fetch_add(1, std::memory_order_relaxed);
        if (rc == 0) {
            HookLogImportant("DetourPresent: IsRecursivePresent=TRUE - returning early");
        }

        // Post-SL overlay rendering: when SL FG is active, the overlay is
        // rendered HERE (after SL's FG interpolation), not in ProcessFrame
        // (which runs before SL).  This matches the standard inject-overlay approach — overlay
        // appears on both real and interpolated frames without interfering
        // with SL's FG pipeline.
        auto postSLCallback =
            observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback && !WasPostSLOffKeepAlivePrePresentDrawn()) {
            postSLCallback(pSwapChain);
        } else if (postSLCallback) {
            static std::atomic<int> s_postSLOffKeepAliveRecursiveDedupLogCount{0};
            const int logCount = s_postSLOffKeepAliveRecursiveDedupLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DetourPresent: Skipping re-entrant PostSL callback because the exact-proxy explicit-OFF "
                    "keep-alive already drew in this top-level Present (sc=%p log=%d)",
                    pSwapChain, logCount + 1);
            }
        }
        if (api == APIType::D3D12) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "re-entrant Present");
        }
        static std::atomic<int> s_reentrantLogCount{0};
        int reentrantNum = s_reentrantLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (reentrantNum <= 10 || reentrantNum == 50 || reentrantNum == 100 || (reentrantNum % 500) == 0) {
            HookLogImportant("DetourPresent: Re-entrant #%d (postSL=%p, trampoline=%p, bypass=%p, tid=0x%04X)",
                             reentrantNum, (void*)postSLCallback, (void*)oPresentTrampoline, (void*)oPresentBypass,
                             GetCurrentThreadId());
        }
        if (oPresentTrampoline) {
            return oPresentTrampoline(pSwapChain, SyncInterval, Flags);
        }
        if (oPresentBypass) {
            return oPresentBypass(pSwapChain, SyncInterval, Flags);
        }
        if (reentrantNum <= 10) {
            HookLogImportant("DetourPresent: Re-entrant #%d → S_OK (no bypass trampoline)", reentrantNum);
        }
        return S_OK;
    }
    auto presentDepthGuard = ce::make_scope_guard([]() { ReleasePresent(); });
    // Detect SL's E9 JMP on Present if not already detected. DetectSLPresentHook
    // itself owns the native-FSR suppression rule so the explicit native-FSR OFF
    // teardown window stays protected too.
    if (!s_slRoutingActive.load(std::memory_order_relaxed)) {
        static int s_slCheckCount = 0;
        bool slLoaded = IsSLInterposerLoaded();
        if (s_slCheckCount++ < 10) {
            HookLogImportant("DetourPresent: SL check #%d (slLoaded=%d, oPresent=%p, oPresentTrampoline=%p)",
                             s_slCheckCount, slLoaded ? 1 : 0, oPresent, oPresentTrampoline);
        }
        if (slLoaded) {
            DetectSLPresentHook();
        }
    }

    static int s_processCount = 0;
    if (s_processCount < 5) {
        s_processCount++;
        HookLog("DetourPresent: Processing frame #%d (not wrapped, not in wrapper)", s_processCount);
