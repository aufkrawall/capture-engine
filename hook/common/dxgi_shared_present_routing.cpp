#include "dxgi_shared_internal.h"

namespace DXGIShared {
HRESULT ExecuteStartupRouting(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                      PresentCallContext& ctx, bool* earlyReturn) {
    *earlyReturn = false;
    if (ctx.startupTopLevelCandidate) {
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
                    logCount, ctx.presentOwner, ctx.presentDepthVal, ctx.expectedPresentThreadId, ctx.currentThreadId);
            }
        }

        if (ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(
                ctx.api == APIType::D3D12, ctx.startupTopLevelCandidate, ctx.streamlineStartupHandoffTransportRisk,
                ctx.postFSRRuntimeStartupHandoffRisk || ctx.startupHandoffSteamRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline startup-handoff Present");
            if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(
                    ctx.api == APIType::D3D12, ctx.postSLConfirmedRendering)) {
                DX12_RetainStreamlineStartupActivationSwapchain(
                    pSwapChain, "DetourPresent: startup-handoff normal-route transport");
            }
            bool exactStartupTransportDrawn = false;
            if (DXGIShared::ShouldRenderExactPostSLBeforeStartupHandoffTransport(
                    ctx.api == APIType::D3D12, ctx.hadFSRFGPhase, ctx.safePostFSRBootstrapPath, ctx.streamlineFGRunning,
                    ctx.startupTopLevelCandidate, ctx.postSLConfirmedRendering)) {
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
                *earlyReturn = true;
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
                        bypassCount, ctx.hadFSRFGPhase ? 1 : 0, ctx.runtimeOwnedSwapchainActive ? 1 : 0,
                        ctx.streamlineStartupHandoffTransportRisk ? 1 : 0, ctx.postFSRRuntimeStartupHandoffRisk ? 1 : 0,
                        ctx.startupHandoffSteamRisk ? 1 : 0, ctx.streamlineStartupHandoffPending ? 1 : 0,
                        ctx.postSLConfirmedRendering ? 1 : 0, ctx.postSLConfirmedButStartupSettling ? 1 : 0,
                        (void*)presentBypass, ctx.presentOwner, ctx.presentDepthVal, ctx.currentThreadId);
                }
                MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(pSwapChain, ctx.api == APIType::D3D12,
                                                                   ctx.streamlineFGRunning, ctx.postSLConfirmedRendering,
                                                                   ctx.hadFSRFGPhase, "startupHandoffNormalRoute");
                const HRESULT hr = presentBypass(pSwapChain, SyncInterval, Flags);
                if (SUCCEEDED(hr)) {
                    DX12_AccountOverlayTransportPresent(exactStartupTransportDrawn,
                                                        "streamline-startup-handoff-transport",
                                                        "DetourPresent/startup-handoff-bypass");
                }
                *earlyReturn = true;
                return hr;
            }
        } else if (ctx.runtimeOwnedSwapchainActive) {
            static std::atomic<int> s_streamlineStartupHandoffNormalTransportAllowedLogCount{0};
            int allowedCount =
                s_streamlineStartupHandoffNormalTransportAllowedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (allowedCount <= 10 || (allowedCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: Streamline startup-handoff normal-route transport allowed #%d "
                    "(hadFSR=%d runtimeOwnsSwapchain=%d transportRisk=%d fsrHandoffRisk=%d steamRisk=%d "
                    "startupPending=%d "
                    "confirmed=%d settling=%d owner=0x%04X depth=%d tid=0x%04X)",
                    allowedCount, ctx.hadFSRFGPhase ? 1 : 0, ctx.runtimeOwnedSwapchainActive ? 1 : 0,
                    ctx.streamlineStartupHandoffTransportRisk ? 1 : 0, ctx.postFSRRuntimeStartupHandoffRisk ? 1 : 0,
                    ctx.startupHandoffSteamRisk ? 1 : 0, ctx.streamlineStartupHandoffPending ? 1 : 0,
                    ctx.postSLConfirmedRendering ? 1 : 0, ctx.postSLConfirmedButStartupSettling ? 1 : 0, ctx.presentOwner,
                    ctx.presentDepthVal, ctx.currentThreadId);
            }
        }
    }
    const bool keepStartupPresentOnNormalRoute = DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(
        ctx.observerOnlyMode, ctx.hadFSRFGPhase, ctx.explicitSetOptionsActivation, ctx.safePostFSRBootstrapPath,
        g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire),
        ctx.callerFromStreamlineModule, ctx.postSLStartupActivationPending, ctx.postSLActiveButUnconfirmed,
        ctx.postSLConfirmedButStartupSettling, ctx.streamlineSyntheticReentrant);
    const bool stalePostFSRStartupNormalRoutePresentHookRisk =
        ctx.api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
            ctx.presentBypassAvailable, ctx.steamOverlayLoaded, ctx.api == APIType::D3D12, ctx.inWrapperPresent, ctx.wrappedSwapchain,
            ctx.hadFSRFGPhase, keepStartupPresentOnNormalRoute);
    const bool startupNormalRouteSteamRisk =
        ctx.staleThirdPartyPresentHookRisk || stalePostFSRStartupNormalRoutePresentHookRisk;
    const bool postFSRRuntimeStartupNormalRouteRisk =
        ctx.api == APIType::D3D12 && ctx.hadFSRFGPhase && ctx.safePostFSRBootstrapPath && keepStartupPresentOnNormalRoute;
    const bool streamlineStartupNormalRouteTransportRisk = ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(
        ctx.api == APIType::D3D12, ctx.inWrapperPresent, ctx.wrappedSwapchain, ctx.presentBypassAvailable, ctx.callerFromStreamlineModule,
        ctx.streamlineStartupHandoffInProgress, ctx.runtimeOwnedSwapchainActive, keepStartupPresentOnNormalRoute,
        postFSRRuntimeStartupNormalRouteRisk, startupNormalRouteSteamRisk);
    if (keepStartupPresentOnNormalRoute) {
        const bool shouldInvokePostSLCallbackOnNormalRoute =
            DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
                ctx.observerOnlyMode, ctx.hadFSRFGPhase, ctx.explicitSetOptionsActivation, ctx.activeDLSSFGRuntimeSignalObserved,
                ctx.safePostFSRBootstrapPath, ctx.postSLStartupActivationPending, ctx.postSLActiveButUnconfirmed,
                ctx.postSLStartupActivationEntered, ctx.postSLConfirmedButStartupSettling, ctx.streamlineSyntheticReentrant);
        static std::atomic<int> s_streamlineSyntheticStartupNormalRouteLogCount{0};
        int logCount = s_streamlineSyntheticStartupNormalRouteLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DetourPresent: Keeping decisive synthetic Streamline startup Present on the normal SL route #%d "
                "(startupPending=%d unconfirmed=%d activationEntered=%d settling=%d callbackOnNormal=%d consumed=%d "
                "hadFSR=%d activeDLSSSignal=%d windowActive=%d tid=0x%04X)",
                logCount, ctx.postSLStartupActivationPending ? 1 : 0, ctx.postSLActiveButUnconfirmed ? 1 : 0,
                ctx.postSLStartupActivationEntered ? 1 : 0, ctx.postSLConfirmedButStartupSettling ? 1 : 0,
                shouldInvokePostSLCallbackOnNormalRoute ? 1 : 0,
                g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_relaxed) ? 1 : 0,
                ctx.hadFSRFGPhase ? 1 : 0, ctx.activeDLSSFGRuntimeSignalObserved ? 1 : 0,
                ctx.streamlineStartupTransitionWindowActive ? 1 : 0, ctx.currentThreadId);
        }
        if (shouldInvokePostSLCallbackOnNormalRoute) {
            auto postSLCallback = g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
            if (postSLCallback) {
                static std::atomic<int> s_unconfirmedStartupNormalRouteCallbackLogCount{0};
                const int callbackLogCount =
                    s_unconfirmedStartupNormalRouteCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
                if (ctx.postSLStartupActivationEntered && ctx.postSLActiveButUnconfirmed &&
                    (callbackLogCount < 10 || (callbackLogCount % 100) == 0)) {
                    HookLogImportant(
                        "DetourPresent: Invoking PostSL on activated-but-unconfirmed Streamline startup normal route "
                        "#%d (startupPending=%d hadFSR=%d owner=0x%04X depth=%d tid=0x%04X)",
                        callbackLogCount + 1, ctx.postSLStartupActivationPending ? 1 : 0, ctx.hadFSRFGPhase ? 1 : 0,
                        ctx.presentOwner, ctx.presentDepthVal, ctx.currentThreadId);
                }
                if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(
                        ctx.api == APIType::D3D12, true, ctx.postSLStartupActivationPending, ctx.postSLActiveButUnconfirmed,
                        ctx.postSLConfirmedRendering)) {
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
                    ctx.postSLStartupActivationPending ? 1 : 0, ctx.postSLActiveButUnconfirmed ? 1 : 0,
                    ctx.postSLStartupActivationEntered ? 1 : 0, ctx.hadFSRFGPhase ? 1 : 0, ctx.explicitSetOptionsActivation ? 1 : 0,
                    ctx.activeDLSSFGRuntimeSignalObserved ? 1 : 0, ctx.safePostFSRBootstrapPath ? 1 : 0, ctx.currentThreadId);
            }
        }
        if (DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(
                ctx.api == APIType::D3D12, keepStartupPresentOnNormalRoute, ctx.postSLConfirmedRendering,
                ctx.postSLConfirmedButStartupSettling, streamlineStartupNormalRouteTransportRisk,
                startupNormalRouteSteamRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline startup normal-route Present");
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(
                    pSwapChain, SyncInterval, Flags, "Streamline startup normal-route Present", &guardedSteamHr)) {
                *earlyReturn = true;
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
                        bypassCount, ctx.hadFSRFGPhase ? 1 : 0, ctx.runtimeOwnedSwapchainActive ? 1 : 0,
                        streamlineStartupNormalRouteTransportRisk ? 1 : 0, startupNormalRouteSteamRisk ? 1 : 0,
                        ctx.postSLStartupActivationPending ? 1 : 0, ctx.postSLActiveButUnconfirmed ? 1 : 0,
                        ctx.postSLConfirmedRendering ? 1 : 0, ctx.postSLConfirmedButStartupSettling ? 1 : 0,
                        (void*)presentBypass, ctx.presentOwner, ctx.presentDepthVal, ctx.currentThreadId);
                }
                if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(
                        ctx.api == APIType::D3D12, ctx.postSLConfirmedRendering)) {
                    DX12_RetainStreamlineStartupActivationSwapchain(pSwapChain,
                                                                    "DetourPresent: startup normal-route bypass");
                }
                MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(pSwapChain, ctx.api == APIType::D3D12,
                                                                   ctx.streamlineFGRunning, ctx.postSLConfirmedRendering,
                                                                   ctx.hadFSRFGPhase, "keepStartupNormalRoute");
                *earlyReturn = true;
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        } else if (ctx.runtimeOwnedSwapchainActive && ctx.callerFromStreamlineModule) {
            static std::atomic<int> s_streamlineStartupNormalTransportAllowedLogCount{0};
            int allowedCount =
                s_streamlineStartupNormalTransportAllowedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (allowedCount <= 10 || (allowedCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: Streamline startup normal-route transport allowed #%d "
                    "(hadFSR=%d runtimeOwnsSwapchain=%d transportRisk=%d steamRisk=%d startupPending=%d "
                    "unconfirmed=%d confirmed=%d settling=%d owner=0x%04X depth=%d tid=0x%04X)",
                    allowedCount, ctx.hadFSRFGPhase ? 1 : 0, ctx.runtimeOwnedSwapchainActive ? 1 : 0,
                    streamlineStartupNormalRouteTransportRisk ? 1 : 0, startupNormalRouteSteamRisk ? 1 : 0,
                    ctx.postSLStartupActivationPending ? 1 : 0, ctx.postSLActiveButUnconfirmed ? 1 : 0,
                    ctx.postSLConfirmedRendering ? 1 : 0, ctx.postSLConfirmedButStartupSettling ? 1 : 0, ctx.presentOwner,
                    ctx.presentDepthVal, ctx.currentThreadId);
            }
        }
        ctx.streamlineSyntheticReentrant = false;
    }
    const bool shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute =
        DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
            ctx.observerOnlyMode, ctx.api == APIType::D3D12, ctx.streamlineFGRunning, ctx.callerFromStreamlineModule,
            ctx.postSLConfirmedRendering, ctx.postSLConfirmedButStartupSettling, ctx.presentOwnershipActive,
            ctx.streamlineSyntheticReentrant);
    const bool stalePostFSRConfirmedStandalonePresentHookRisk =
        ctx.api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
            EnsurePresentBypassTrampoline() != nullptr, ctx.steamOverlayLoaded, ctx.api == APIType::D3D12, ctx.inWrapperPresent,
            ctx.wrappedSwapchain, ctx.hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute);
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
                    logCount, ctx.postSLConfirmedButStartupSettling ? 1 : 0, ctx.presentOwner, ctx.presentDepthVal,
                    ctx.currentThreadId);
            }
            postSLCallback(pSwapChain);
        }

        if (DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(
                ctx.api == APIType::D3D12, ctx.hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute,
                ctx.staleThirdPartyPresentHookRisk || stalePostFSRConfirmedStandalonePresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR confirmed standalone Present");
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,
                                                            "post-FSR confirmed standalone Present", &guardedSteamHr)) {
                *earlyReturn = true;
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
                        bypassCount, ctx.presentOwner, ctx.presentDepthVal, ctx.currentThreadId);
                }
                *earlyReturn = true;
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        }
    }
    if (ctx.streamlineSyntheticReentrant) {
        auto postSLCallback =
            ctx.observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
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

        if (ctx.api == APIType::D3D12) {
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
            *earlyReturn = true;
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
            *earlyReturn = true;
            return presentBypass(pSwapChain, SyncInterval, Flags);
        }
    }

    g_SharedState.presentInFlightDepth.fetch_add(1, std::memory_order_acq_rel);
    auto presentInFlightGuard =
        ce::make_scope_guard([]() { g_SharedState.presentInFlightDepth.fetch_sub(1, std::memory_order_acq_rel); });

    if (IsShuttingDown()) {
        if (IsReadableMemory(pSwapChain, sizeof(void*))) {
            *earlyReturn = true;
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }
        *earlyReturn = true;
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!IsReadableMemory(pSwapChain, sizeof(void*))) {
        *earlyReturn = true;
        return DXGI_ERROR_INVALID_CALL;
    }
    void** vtable = *(void***)pSwapChain;
    if (!vtable || !IsReadableMemory(reinterpret_cast<const void*>(vtable), 9 * sizeof(void*)) || !vtable[8]) {
        *earlyReturn = true;
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!dxgi_shared_oPresentTrampoline && !dxgi_shared_oPresent) {
        HookLog("DetourPresent: No original Present function available");
        *earlyReturn = true;
        return DXGI_ERROR_INVALID_CALL;
    }

    // Apply SetMaximumFrameLatency override (must be BEFORE wrapper/recursive checks)
    ApplyPresentFrameLatencyOverrides(pSwapChain);

    // Query-based CPU prerender limit for D3D11 (fallback when IDXGISwapChain2 is unavailable).
    // Applied for all D3D11 games regardless of wrapper/vtable path.
    if (ctx.api == APIType::D3D11 && g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        float prerenderLimit = GetActivePrerenderLimit();
        if (prerenderLimit >= 0.0f) {
            ApplyPrerenderLimit(pSwapChain, prerenderLimit);
        }
    }

    if (ctx.wrappedSwapchain) {
        if (ctx.api == APIType::D3D12) {
            DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(
                pSwapChain, "DXGIShared::DetourPresent wrapped-swapchain pass-through");
        }
        static int s_wrappedPassCount = 0;
        if (s_wrappedPassCount < 5) {
            s_wrappedPassCount++;
            HookLogImportant("DetourPresent: WRAPPED swapchain early return #%d", s_wrappedPassCount);
        }
        HRESULT wrappedHr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        if (ctx.api == APIType::D3D12) {
            InvokeDX12FlushDeferredSignal();
        }
        if (SUCCEEDED(wrappedHr)) {
            g_SharedFpsLimiter.ApplyPostPresent();
        }
        *earlyReturn = true;
        return wrappedHr;
    }

    if (ctx.inWrapperPresent) {
        if (ctx.api == APIType::D3D12) {
            DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(
                pSwapChain, "DXGIShared::DetourPresent wrapper re-entry pass-through");
        }
        static int s_inWrapperPassCount = 0;
        if (s_inWrapperPassCount < 5) {
            s_inWrapperPassCount++;
            HookLogImportant("DetourPresent: IsInWrapperPresent early return #%d", s_inWrapperPassCount);
        }
        HRESULT wrapperHr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        if (ctx.api == APIType::D3D12) {
            InvokeDX12FlushDeferredSignal();
        }
        if (SUCCEEDED(wrapperHr)) {
            g_SharedFpsLimiter.ApplyPostPresent();
        }
        *earlyReturn = true;
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
            ctx.observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
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
        if (ctx.api == APIType::D3D12) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "re-entrant Present");
        }
        static std::atomic<int> s_reentrantLogCount{0};
        int reentrantNum = s_reentrantLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (reentrantNum <= 10 || reentrantNum == 50 || reentrantNum == 100 || (reentrantNum % 500) == 0) {
            HookLogImportant("DetourPresent: Re-entrant #%d (postSL=%p, trampoline=%p, bypass=%p, tid=0x%04X)",
                             reentrantNum, (void*)postSLCallback, (void*)dxgi_shared_oPresentTrampoline, (void*)dxgi_shared_oPresentBypass,
                             GetCurrentThreadId());
        }
        if (dxgi_shared_oPresentTrampoline) {
            *earlyReturn = true;
            return dxgi_shared_oPresentTrampoline(pSwapChain, SyncInterval, Flags);
        }
        if (dxgi_shared_oPresentBypass) {
            *earlyReturn = true;
            return dxgi_shared_oPresentBypass(pSwapChain, SyncInterval, Flags);
        }
        if (reentrantNum <= 10) {
            HookLogImportant("DetourPresent: Re-entrant #%d → S_OK (no bypass trampoline)", reentrantNum);
        }
        *earlyReturn = true;
        return S_OK;
    }
    auto presentDepthGuard = ce::make_scope_guard([]() { ReleasePresent(); });
    // Detect SL's E9 JMP on Present if not already detected. DetectSLPresentHook
    // itself owns the native-FSR suppression rule so the explicit native-FSR OFF
    // teardown window stays protected too.
    if (!dxgi_shared_s_slRoutingActive.load(std::memory_order_relaxed)) {
        static int s_slCheckCount = 0;
        bool slLoaded = IsSLInterposerLoaded();
        if (s_slCheckCount++ < 10) {
            HookLogImportant("DetourPresent: SL check #%d (slLoaded=%d, oPresent=%p, oPresentTrampoline=%p)",
                             s_slCheckCount, slLoaded ? 1 : 0, dxgi_shared_oPresent, dxgi_shared_oPresentTrampoline);
        }
        if (slLoaded) {
            DetectSLPresentHook();
        }
    }

    static int s_processCount = 0;
    if (s_processCount < 5) {
        s_processCount++;
        HookLog("DetourPresent: Processing frame #%d (not wrapped, not in wrapper)", s_processCount);

    }

    // Only send heartbeat if device is healthy — after device removal,
    // suppressing heartbeats lets the freeze watchdog fire and create a dump.
    if (!g_SharedState.deviceRemovedFatal.load(std::memory_order_relaxed))
        g_RenderWatchdog.HeartbeatFromHelperThread();

    // Periodic flush: forward any suppressed slDLSSGSetOptions(OFF) call that was
    // buffered during the DLSS FG startup transition window now that the window
    // has expired.  This ensures the real Streamline runtime eventually receives
    // the OFF signal even if slDLSSGGetState/SetOptions calls are infrequent.
    StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded();

    if (IsVulkanActive()) {
        *earlyReturn = true;
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Lazy check for NvPresent64.dll (may load after our hooks)
    g_FGCompat.CheckForNvPresent();

    // NVIDIA Smooth Motion compatibility: skip overlay/processing for invisible
    // windows. NvPresent64 creates invisible-window swapchains for DX11 frame
    // interpolation — processing them corrupts NvPresent64's internal state.
    if (g_FGCompat.IsNvPresentLoaded()) {
        DXGI_SWAP_CHAIN_DESC smDesc = {};
        if (SUCCEEDED(pSwapChain->GetDesc(&smDesc))) {
            if (!smDesc.OutputWindow || !IsWindowVisible(smDesc.OutputWindow)) {
                static int s_smSkipCount = 0;
                if (s_smSkipCount < 5) {
                    s_smSkipCount++;
                    HookLog(
                        "DetourPresent: Skipping invisible window (SM compat, "
                        "hwnd=%p) #%d",
                        smDesc.OutputWindow, s_smSkipCount);
                }
                *earlyReturn = true;
                return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
            }
        }
    }


    g_SharedState.presentCallCount.fetch_add(1, std::memory_order_relaxed);

    if (g_SharedState.deviceRemovedFatal.load()) {
        *earlyReturn = true;
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    {
        // Safety: auto-clear swapchainInvalid after 3 seconds if no resize arrives.
        // This prevents permanent overlay death from invalidation without a matching
        // ResizeBuffers (e.g., FG type transitions that don't recreate the swapchain).
        static int64_t s_invalidSinceQpc = 0;
        if (g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
            if (s_invalidSinceQpc == 0) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                s_invalidSinceQpc = now.QuadPart;
            }
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            double elapsedMs = (double)(now.QuadPart - s_invalidSinceQpc) * 1000.0 / (double)freq.QuadPart;
            if (elapsedMs > 3000.0) {
                HookLogImportant("DetourPresent: swapchainInvalid auto-cleared after %.0fms (no resize arrived)",
                                 elapsedMs);
                g_SharedState.swapchainInvalid.store(false, std::memory_order_release);
                s_invalidSinceQpc = 0;
                // Fall through to normal processing
            } else {
                *earlyReturn = true;
                return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
            }
        } else {
            s_invalidSinceQpc = 0;
        }
    }

    *earlyReturn = false;
    return S_OK;
}
}
