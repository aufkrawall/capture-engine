            if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(
                    api == APIType::D3D12, postSLConfirmedRendering)) {
                DX12_RetainStreamlineStartupActivationSwapchain(
                    pSwapChain, "DetourPresent1: startup-handoff normal-route transport");
            }
            bool exactStartupTransportDrawn = false;
            if (DXGIShared::ShouldRenderExactPostSLBeforeStartupHandoffTransport(
                    api == APIType::D3D12, hadFSRFGPhase, safePostFSRBootstrapPath, streamlineFGRunning,
                    startupTopLevelCandidate, postSLConfirmedRendering)) {
                exactStartupTransportDrawn = DX12_TryRenderExactPostSLBeforeStartupHandoffPresent(
                    pSwapChain, "DetourPresent1/startup-handoff-transport");
            }
            PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
            if (present1Bypass) {
                static std::atomic<int> s_streamlineStartupHandoffBypassLogCount1{0};
                int bypassCount = s_streamlineStartupHandoffBypassLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent1: Streamline startup-handoff normal-route bypass #%d "
                        "(hadFSR=%d runtimeOwnsSwapchain=%d transportRisk=%d fsrHandoffRisk=%d steamRisk=%d "
                        "startupPending=%d confirmed=%d settling=%d bypass=%p owner=0x%04X depth=%d tid=0x%04X)",
                        bypassCount, hadFSRFGPhase ? 1 : 0, runtimeOwnedSwapchainActive ? 1 : 0,
                        streamlineStartupHandoffTransportRisk ? 1 : 0, postFSRRuntimeStartupHandoffRisk ? 1 : 0,
                        startupHandoffSteamRisk ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                        postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                        (void*)present1Bypass, presentOwner, presentDepthVal, currentThreadId);
                }
                const HRESULT hr = present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
                if (SUCCEEDED(hr)) {
                    DX12_AccountOverlayTransportPresent(exactStartupTransportDrawn,
                                                        "streamline-startup-handoff-transport",
                                                        "DetourPresent1/startup-handoff-bypass");
                }
                return hr;
            }
        } else if (runtimeOwnedSwapchainActive) {
            static std::atomic<int> s_streamlineStartupHandoffNormalTransportAllowedLogCount1{0};
            int allowedCount =
                s_streamlineStartupHandoffNormalTransportAllowedLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
            if (allowedCount <= 10 || (allowedCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent1: Streamline startup-handoff normal-route transport allowed #%d "
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
            present1BypassAvailable, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent, wrappedSwapchain,
            hadFSRFGPhase, keepStartupPresentOnNormalRoute);
    const bool startupNormalRouteSteamRisk =
        staleThirdPartyPresentHookRisk || stalePostFSRStartupNormalRoutePresentHookRisk;
    const bool postFSRRuntimeStartupNormalRouteRisk =
        api == APIType::D3D12 && hadFSRFGPhase && safePostFSRBootstrapPath && keepStartupPresentOnNormalRoute;
    const bool streamlineStartupNormalRouteTransportRisk = ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(
        api == APIType::D3D12, inWrapperPresent, wrappedSwapchain, present1BypassAvailable, callerFromStreamlineModule,
        streamlineStartupHandoffInProgress, runtimeOwnedSwapchainActive, keepStartupPresentOnNormalRoute,
        postFSRRuntimeStartupNormalRouteRisk, startupNormalRouteSteamRisk);
    if (keepStartupPresentOnNormalRoute) {
        const bool shouldInvokePostSLCallbackOnNormalRoute =
            DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
                observerOnlyMode, hadFSRFGPhase, explicitSetOptionsActivation, activeDLSSFGRuntimeSignalObserved,
                safePostFSRBootstrapPath, postSLStartupActivationPending, postSLActiveButUnconfirmed,
                postSLStartupActivationEntered, postSLConfirmedButStartupSettling, streamlineSyntheticReentrant);
        static std::atomic<int> s_streamlineSyntheticStartupNormalRouteLogCount1{0};
        int logCount = s_streamlineSyntheticStartupNormalRouteLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DetourPresent1: Keeping decisive synthetic Streamline startup Present1 on the normal SL route #%d "
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
                static std::atomic<int> s_unconfirmedStartupNormalRouteCallbackLogCount1{0};
                const int callbackLogCount =
                    s_unconfirmedStartupNormalRouteCallbackLogCount1.fetch_add(1, std::memory_order_relaxed);
                if (postSLStartupActivationEntered && postSLActiveButUnconfirmed &&
                    (callbackLogCount < 10 || (callbackLogCount % 100) == 0)) {
                    HookLogImportant(
                        "DetourPresent1: Invoking PostSL on activated-but-unconfirmed Streamline startup normal route "
                        "#%d (startupPending=%d hadFSR=%d owner=0x%04X depth=%d tid=0x%04X)",
                        callbackLogCount + 1, postSLStartupActivationPending ? 1 : 0, hadFSRFGPhase ? 1 : 0,
                        presentOwner, presentDepthVal, currentThreadId);
                }
                if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(
                        api == APIType::D3D12, true, postSLStartupActivationPending, postSLActiveButUnconfirmed,
                        postSLConfirmedRendering)) {
                    DX12_RetainStreamlineStartupActivationSwapchain(
                        pSwapChain, "DetourPresent1: startup normal-route PostSL callback");
                }
                postSLCallback(pSwapChain);
            }
        } else {
            static std::atomic<int> s_skipPostSLCallbackLogCount1{0};
            int skipCount = s_skipPostSLCallbackLogCount1.fetch_add(1, std::memory_order_relaxed);
            if (skipCount < 5 || (skipCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent1: PostSL callback skipped on normal route despite startup present kept "
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
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline startup normal-route Present1");
            PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
            if (present1Bypass) {
                static std::atomic<int> s_streamlineStartupNormalRouteBypassLogCount1{0};
                int bypassCount =
                    s_streamlineStartupNormalRouteBypassLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent1: Streamline startup normal-route bypass #%d "
                        "(hadFSR=%d runtimeOwnsSwapchain=%d transportRisk=%d steamRisk=%d "
                        "startupPending=%d unconfirmed=%d confirmed=%d settling=%d bypass=%p owner=0x%04X depth=%d "
                        "tid=0x%04X)",
                        bypassCount, hadFSRFGPhase ? 1 : 0, runtimeOwnedSwapchainActive ? 1 : 0,
                        streamlineStartupNormalRouteTransportRisk ? 1 : 0, startupNormalRouteSteamRisk ? 1 : 0,
                        postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                        postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                        (void*)present1Bypass, presentOwner, presentDepthVal, currentThreadId);
                }
                if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(
                        api == APIType::D3D12, postSLConfirmedRendering)) {
                    DX12_RetainStreamlineStartupActivationSwapchain(pSwapChain,
                                                                    "DetourPresent1: startup normal-route bypass");
                }
                return present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
            }
        } else if (runtimeOwnedSwapchainActive && callerFromStreamlineModule) {
            static std::atomic<int> s_streamlineStartupNormalTransportAllowedLogCount1{0};
            int allowedCount =
                s_streamlineStartupNormalTransportAllowedLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
            if (allowedCount <= 10 || (allowedCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent1: Streamline startup normal-route transport allowed #%d "
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
            EnsurePresent1BypassTrampoline() != nullptr, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent,
            wrappedSwapchain, hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute);
    if (shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute) {
        auto postSLCallback = g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback) {
            static std::atomic<int> s_confirmedStandaloneNormalRouteCallbackLogCount1{0};
            int logCount =
                s_confirmedStandaloneNormalRouteCallbackLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent1: Invoking PostSL on confirmed standalone Streamline Present1 while keeping the "
                    "normal SL route #%d (settling=%d owner=0x%04X depth=%d tid=0x%04X)",
                    logCount, postSLConfirmedButStartupSettling ? 1 : 0, presentOwner, presentDepthVal,
                    currentThreadId);
            }
            postSLCallback(pSwapChain);
        }

        if (DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(
                api == APIType::D3D12, hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute,
                staleThirdPartyPresentHookRisk || stalePostFSRConfirmedStandalonePresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR confirmed standalone Present1");
            PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
            if (present1Bypass) {
                static std::atomic<int> s_confirmedStandaloneNormalRouteBypassLogCount1{0};
                int bypassCount =
                    s_confirmedStandaloneNormalRouteBypassLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent1: Post-FSR confirmed standalone normal-route bypass #%d "
                        "(owner=0x%04X depth=%d tid=0x%04X)",
                        bypassCount, presentOwner, presentDepthVal, currentThreadId);
                }
                return present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
            }
        }
    }
    if (streamlineSyntheticReentrant) {
        auto postSLCallback =
            observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback && !WasPostSLOffKeepAlivePrePresentDrawn()) {
            postSLCallback(pSwapChain);
        } else if (postSLCallback) {
            static std::atomic<int> s_postSLOffKeepAliveNestedDedupLogCount1{0};
            const int logCount = s_postSLOffKeepAliveNestedDedupLogCount1.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DetourPresent1: Skipping nested PostSL callback because the exact-proxy explicit-OFF "
                    "keep-alive already drew before this Present (sc=%p log=%d)",
                    pSwapChain, logCount + 1);
            }
        }

        if (api == APIType::D3D12) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline synthetic Present1");
        }

        PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
        if (present1Bypass) {
            static std::atomic<int> s_streamlineSyntheticPresent1LogCount{0};
            int syntheticNum = s_streamlineSyntheticPresent1LogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (syntheticNum <= 10 || syntheticNum == 50 || (syntheticNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent1: Treating Streamline-originated Present1 as synthetic re-entrant #%d "
                    "(postSL=%p, bypass=%p, tid=0x%04X)",
                    syntheticNum, (void*)postSLCallback, (void*)present1Bypass, GetCurrentThreadId());
            }
            return present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
    }

    g_SharedState.presentInFlightDepth.fetch_add(1, std::memory_order_acq_rel);
    auto presentInFlightGuard =
        ce::make_scope_guard([]() { g_SharedState.presentInFlightDepth.fetch_sub(1, std::memory_order_acq_rel); });

    if (IsShuttingDown()) {
        if (IsReadableMemory(pSwapChain, sizeof(void*))) {
            return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!IsReadableMemory(pSwapChain, sizeof(void*))) {
        RequestHookShutdown();
        return DXGI_ERROR_INVALID_CALL;
    }
    void** vtable = *(void***)pSwapChain;
    if (!vtable || !IsReadableMemory(vtable, 23 * sizeof(void*)) || !vtable[22]) {
        RequestHookShutdown();
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!oPresent1Trampoline && !oPresent1) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Apply SetMaximumFrameLatency override (must be BEFORE wrapper/recursive checks)
    ApplyPresentFrameLatencyOverrides(pSwapChain);

    // Query-based CPU prerender limit for D3D11 (fallback when IDXGISwapChain2 is unavailable).
    if (api == APIType::D3D11 && g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        float prerenderLimit = GetActivePrerenderLimit();
        if (prerenderLimit >= 0.0f) {
            ApplyPrerenderLimit(pSwapChain, prerenderLimit);
        }
    }

    void* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapper))) {
        ((IUnknown*)pWrapper)->Release();
        if (api == APIType::D3D12) {
            DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(
                pSwapChain, "DXGIShared::DetourPresent1 wrapped-swapchain pass-through");
        }
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    if (IsInWrapperPresent()) {
        if (api == APIType::D3D12) {
            DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(
                pSwapChain, "DXGIShared::DetourPresent1 wrapper re-entry pass-through");
        }
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    // Recursive external-overlay Present1 (e.g. Steam overlay called Present1
    // which re-entered through vtable[22]).  Same guard as DetourPresent.
    if (s_externalOverlayPresentInvokeDepth > 0) {
        PFN_Present1 recursiveBypass1 = EnsurePresent1BypassTrampoline();
        if (recursiveBypass1) {
            static std::atomic<int> s_recursiveExternalOverlayPresent1BypassLogCount{0};
            const int bypassNum =
                s_recursiveExternalOverlayPresent1BypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent1: Bypassing recursive external-overlay Present1 #%d while guarded Steam hook is "
                    "active (depth=%d bypass=%p tid=0x%04X)",
                    bypassNum, s_externalOverlayPresentInvokeDepth, (void*)recursiveBypass1, GetCurrentThreadId());
            }
            return recursiveBypass1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
    }

    // Re-entrant Present1 call — same logic as DetourPresent.
    if (IsRecursivePresent()) {
        // Post-SL overlay rendering (same as DetourPresent).
        auto postSLCallback =
            observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback && !WasPostSLOffKeepAlivePrePresentDrawn()) {
            postSLCallback(pSwapChain);
        } else if (postSLCallback) {
            static std::atomic<int> s_postSLOffKeepAliveRecursiveDedupLogCount1{0};
            const int logCount = s_postSLOffKeepAliveRecursiveDedupLogCount1.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DetourPresent1: Skipping re-entrant PostSL callback because the exact-proxy explicit-OFF "
                    "keep-alive already drew in this top-level Present (sc=%p log=%d)",
                    pSwapChain, logCount + 1);
            }
        }
        if (api == APIType::D3D12) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "re-entrant Present1");
        }
        static std::atomic<int> s_reentrantLogCount1{0};
        int reentrantNum1 = s_reentrantLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
        if (reentrantNum1 <= 10 || reentrantNum1 == 50 || reentrantNum1 == 100 || (reentrantNum1 % 500) == 0) {
            HookLog("DetourPresent1: Re-entrant #%d (postSL=%p, trampoline=%p, bypass=%p)", reentrantNum1,
                    (void*)postSLCallback, (void*)oPresent1Trampoline, (void*)oPresent1Bypass);
        }
        if (oPresent1Trampoline) {
            return oPresent1Trampoline(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        if (oPresent1Bypass) {
            return oPresent1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        if (reentrantNum1 <= 10) {
            HookLog("DetourPresent1: Re-entrant #%d → S_OK (no bypass trampoline)", reentrantNum1);
        }
        return S_OK;
    }
    auto presentDepthGuard = ce::make_scope_guard([]() { ReleasePresent(); });
    // Detect SL's E9 JMP if not yet done (same as DetourPresent). DetectSLPresentHook
    // itself owns the native-FSR suppression rule.
    if (!s_slRoutingActive.load(std::memory_order_relaxed) && IsSLInterposerLoaded()) {
        DetectSLPresentHook();
    }

    // Only send heartbeat if device is healthy — after device removal,
    // suppressing heartbeats lets the freeze watchdog fire and create a dump.
    if (!g_SharedState.deviceRemovedFatal.load(std::memory_order_relaxed))
        g_RenderWatchdog.HeartbeatFromHelperThread();

    // Periodic flush: forward any suppressed slDLSSGSetOptions(OFF) call that was
    // buffered during the DLSS FG startup transition window now that the window
    // has expired.
    StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded();

    if (IsVulkanActive()) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    // NVIDIA Smooth Motion compatibility: skip for invisible windows
    if (g_FGCompat.IsNvPresentLoaded()) {
        DXGI_SWAP_CHAIN_DESC smDesc = {};
        if (SUCCEEDED(pSwapChain->GetDesc(&smDesc))) {
            if (!smDesc.OutputWindow || !IsWindowVisible(smDesc.OutputWindow)) {
                return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
            }
        }
    }

    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook)
            g_SharedState.inPresentHook.store(false);
    });

    if (g_SharedState.deviceRemovedFatal.load() || g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    if (api == APIType::D3D12) {
        const char* overlayModule = nullptr;
        int startupPass = 0;
        DX12StartupPresentMode startupMode =
            GetDX12StartupPresentMode(oPresent1Bypass != nullptr, &overlayModule, &startupPass);
        if (startupMode == DX12StartupPresentMode::kPassThroughOriginal) {
            HookLogImportant("DetourPresent1: Startup compatibility pass #%d for third-party overlay %s", startupPass,
                             overlayModule ? overlayModule : "module");
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply();
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        const bool knownThirdPartyOverlaySwapchain = DXGIShared::DX12_IsThirdPartyOverlaySwapchain(pSwapChain);
        const bool startupBlockingOverlaySwapchainStillOwnsPresent =
            ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass(
                knownThirdPartyOverlaySwapchain, DXGIShared::DX12_IsStartupBlockingOverlayTaggedSwapchain(pSwapChain),
                HasStartupBlockingOverlayModuleInCurrentStack());
        if (ce::dx12_overlay_policy::ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(
                startupBlockingOverlaySwapchainStillOwnsPresent || callerFromThirdPartyOverlay)) {
            static std::atomic<int> s_overlayPresent1BypassLogCount{0};
            const int logCount = s_overlayPresent1BypassLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 256) == 0) {
                HookLogImportant(
                    "DetourPresent1: Bypassing DX12 ProcessFrame for third-party overlay swapchain %p (caller=%s)",
                    pSwapChain, detourCallerModulePath[0] ? detourCallerModulePath : "tracked-overlay-swapchain");
            }
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply();
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }

        if (DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
                observerOnlyMode, api == APIType::D3D12, inWrapperPresent, wrappedSwapchain, oPresent1 != nullptr,
                streamlineFGRunning, s_slRoutingActive.load(std::memory_order_acquire), callerFromStreamlineModule,
                streamlineStartupHandoffInProgress, runtimeOwnedSwapchainActive, hadFSRFGPhase,
                safePostFSRBootstrapPath, postSLConfirmedRendering, startupTopLevelPresentAlreadyConsumed)) {
            bool expected = false;
            const bool markedStartupConsumed =
                g_SharedState.streamlineStartupTopLevelPresentConsumed.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain,
                                                        "app-thread post-FSR Streamline startup handoff Present1");
            static std::atomic<int> s_appThreadPostFSRStartupOverlaylessLogCount1{0};
            const int handoffCount =
                s_appThreadPostFSRStartupOverlaylessLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
            if (handoffCount <= 10 || (handoffCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent1: App-thread post-FSR Streamline startup-handoff overlayless SL route #%d "
                    "(markedConsumed=%d pending=%d transition=%d runtimeOwnsSwapchain=%d safeBootstrap=%d "
                    "confirmed=%d oPresent1=%p owner=0x%04X depth=%d tid=0x%04X)",
                    handoffCount, markedStartupConsumed ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                    streamlineStartupTransitionWindowActive ? 1 : 0, runtimeOwnedSwapchainActive ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0, postSLConfirmedRendering ? 1 : 0, (void*)oPresent1, presentOwner,
                    presentDepthVal, currentThreadId);
            }
            if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(
                    api == APIType::D3D12, postSLConfirmedRendering)) {
                DX12_RetainStreamlineStartupActivationSwapchain(
                    pSwapChain, "DetourPresent1: app-thread post-FSR startup-handoff overlayless SL route");
            }
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply(true);
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            WaitBackbufferFrameLatency(pSwapChain);
            HRESULT handoffHr = oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
            if (SUCCEEDED(handoffHr)) {
                g_SharedFpsLimiter.ApplyPostPresent();
            }
            return handoffHr;
        }
    }
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);
    UpdateDXGIPresentMetricsAndPublish(isFirstHook, "DXGIShared::DetourPresent1");

    if (api == APIType::D3D12) {
        HandleDX12ProcessFrame(pSwapChain, true);
    } else if (DXGIShared::ShouldRunSharedD3D10Or11ProcessFrame(api)) {
        if (api == APIType::D3D10) {
            static std::atomic<int> s_d3d10ProcessFrameLogCount1{0};
            const int logCount = s_d3d10ProcessFrameLogCount1.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 5) {
                HookLogImportant(
                    "DetourPresent1: Routing D3D10 swapchain through shared DX10/DX11 ProcessFrame path #%d",
                    logCount + 1);
            }
        }
        HandleDX11ProcessFrame(pSwapChain, true);
    }

    // FPS Limiter - arm frame pacing before present. Explicit CE-owned Reflex
    // cadence is finished after Present returns so the wait happens before the
    // game starts building the next frame.
    if (g_IPC) {
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply(true);
        ApplyPresentFrameLatencyOverrides(pSwapChain);
    }

    ProcessVSyncOverride(SyncInterval, Flags);

    if (api == APIType::D3D12) {
        InvokeDX12WaitForOverlayCompletion(nullptr);
    }

    // CRITICAL: SL thread Steam bypass — handled in CallOriginalPresent1.

    HRESULT hr;
    if (s_slRoutingActive.load(std::memory_order_acquire) && oPresent1) {
        ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
        bool runtimeOwnedNativeFGPresentPath = false;
        if (ShouldKeepSLPresentRoutingDisabledNow(&runtimeMode, &runtimeOwnedNativeFGPresentPath)) {
            static int s_present1FsrlatchCount = 0;
            int latchNum = ++s_present1FsrlatchCount;
            if (latchNum <= 5) {
                HookLogImportant(
                    "DetourPresent1: SL routing was active while native FG owned Present routing — "
                    "force-disabling SL routing (latch #%d, runtime=%s runtimeOwnedNativeFG=%d). Present1 will go "
                    "through trampoline directly.",
                    latchNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode), runtimeOwnedNativeFGPresentPath ? 1 : 0);
            }
            s_slRoutingActive.store(false, std::memory_order_release);
            hr = CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        } else {
            WaitBackbufferFrameLatency(pSwapChain);
            hr = oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
    } else {
        hr = CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    // Flush deferred overlay fence Signal AFTER Present.
    if (api == APIType::D3D12) {
        InvokeDX12FlushDeferredSignal();
    }

    if (SUCCEEDED(hr)) {
        g_SharedFpsLimiter.ApplyPostPresent();
    }

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        if (!g_SharedState.deviceRemovedFatal.exchange(true)) {
            HookLog("DXGI: Device removed (hr=0x%08X), disabling hooks", hr);
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (IsShuttingDown()) {
        if (oResizeBuffers) {
            return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        return S_OK;
    }

    // Apply backbuffer count override from config
    // When the game calls ResizeBuffers (window resize, alt-tab, resolution change),
    // this ensures our buffer count is applied even if CreateSwapChain override was missed.
    {
        const auto& cfg = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(cfg.backbufferCount)) {
            UINT requested = static_cast<UINT>(cfg.backbufferCount);
            if (requested > 0 && requested != BufferCount) {
                // Check swap effect for flip-model safety
                DXGI_SWAP_CHAIN_DESC scDesc = {};
                bool canOverride = true;
                if (SUCCEEDED(pSwapChain->GetDesc(&scDesc))) {
                    bool isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                                   scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
                    if (isFlip && requested < BufferCount) {
                        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                        canOverride = false;
                        HookLog("DetourResizeBuffers: Skipping BufferCount override %u < game's %u (flip model)",
                                requested, BufferCount);
                    }
                }
                if (canOverride) {
                    HookLogImportant("DetourResizeBuffers: Overriding BufferCount %u -> %u", BufferCount, requested);
                    BufferCount = requested;
                }
            }
        }
    }

    // CRITICAL FIX: When Vulkan is active, pass through DXGI ResizeBuffers calls
    if (IsVulkanActive()) {
        return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass
        // Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];  // ResizeBuffers is at index 13
        return originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);

    APIType api = DetectAPIType(pSwapChain);

    // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
    // Some games call ResizeBuffers immediately after CreateSwapChain
    static std::atomic<int> s_initialResizeCount{0};
    if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
        HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
        // Call directly through vtable to bypass any hook chain issues
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
        HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
