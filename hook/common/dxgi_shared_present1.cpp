#include "dxgi_shared_internal.h"

#include "fg_cost_probe.h"

#include "../wrappers/vulkan_dxgi_fifo_present.h"

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                         const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (HookIsShuttingDown())
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    if (ce::fg_cost_probe::Active(ce::fg_cost_probe::kPresentPassthrough))
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    // Same single parameter decision as DetourPresent: the scoped Vulkan FIFO
    // backstop is consulted once, at the top, before reentrancy/forwarding.
    ce::vulkan_dxgi_fifo::ApplyFinalPresentPolicy(pSwapChain, SyncInterval, Flags,
                                                  ce::vulkan_dxgi_fifo::FinalPresentVariant::kPresent1);
    g_PresentCallCounter.fetch_add(1, std::memory_order_relaxed);

    const APIType api = DetectAPIType(pSwapChain);
    if (api == APIType::D3D12 && ShouldBypassDX12InvisibleWindowPresent(pSwapChain, "DetourPresent1")) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }
    BeginPostSLOffKeepAlivePresentScope();
    auto postSLOffKeepAlivePresentScopeGuard = ce::make_scope_guard([]() { EndPostSLOffKeepAlivePresentScope(); });
    if (api == APIType::D3D12) {
        DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(pSwapChain,
                                                           "DXGIShared::DetourPresent1 pre-routing");
    }

    const void* detourCallerAddress = CE_CAPTURE_RETURN_ADDRESS();
    char detourCallerModulePath[MAX_PATH] = {};
    // Same rule as DetourPresent: below a foreign chain the immediate caller is always a
    // foreign overlay module, so it cannot classify the swapchain. See
    // CapturePresentCallContext in dxgi_shared_present.cpp.
    const bool callerFromThirdPartyOverlay =
        TryGetModulePathFromCodeAddress(detourCallerAddress, detourCallerModulePath, sizeof(detourCallerModulePath)) &&
        ce::overlay_compat::IsThirdPartyOverlayModulePath(detourCallerModulePath) &&
        !IsPresentInterceptedBelowForeignChain();
    const bool streamlineStartupHandoffPending = (api == APIType::D3D12) && IsStreamlineStartupHandoffPending();
    const bool streamlineStartupTransitionWindowActive =
        (api == APIType::D3D12) && IsStreamlineStartupTransitionWindowActive();
    const bool streamlineStartupHandoffInProgress =
        streamlineStartupHandoffPending || streamlineStartupTransitionWindowActive;
    const DWORD currentThreadId = GetCurrentThreadId();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool wrappedSwapchain = IsWrappedSwapChainObject(pSwapChain);
    const bool inWrapperPresent = IsInWrapperPresent();
    const DWORD presentOwner = dxgi_shared_g_presentThreadId.load(std::memory_order_relaxed);
    const int presentDepthVal = dxgi_shared_g_presentDepth.load(std::memory_order_relaxed);
    const bool presentOwnershipActive = presentOwner != 0 || presentDepthVal > 0;
    const DWORD expectedPresentThreadId = g_RenderWatchdog.GetMonitoredThreadId();
    const bool matchesExpectedPresentThread =
        expectedPresentThreadId == 0 || expectedPresentThreadId == currentThreadId;
    // Same below-the-chain provenance rule as DetourPresent: the originator is a few frames
    // further out once foreign overlays sit above CE.
    bool originatorFromStreamline = false;
    bool originatorFromFFXFrameGeneration = false;
    if (IsPresentInterceptedBelowForeignChain()) {
        ResolvePresentOriginatorBelowForeignChain(&originatorFromStreamline, &originatorFromFFXFrameGeneration);
    }
    const bool callerFromStreamlineModule =
        originatorFromStreamline || IsCodeAddressFromStreamlineModule(detourCallerAddress);
    const bool callerFromFFXFrameGenerationModule =
        originatorFromFFXFrameGeneration ||
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
    const bool runtimeOwnedSwapchainActive = api == APIType::D3D12 && DoesFGRuntimeOwnSwapchain();
    const bool present1BypassAvailable = EnsurePresent1BypassTrampoline() != nullptr;
    const bool staleThirdPartyPresentHookRisk =
        api == APIType::D3D12 &&
        ShouldForceSteamDX12Bypass(pSwapChain, present1BypassAvailable, IsSLInterposerLoaded());
    const bool observerOnlyMode = HookOverlayObserverOnlyEnabled();
    const bool observerStartupPresentOnlyMode = HookOverlayObserverStartupPresentOnlyEnabled();
    const bool ffxStartupBypass = ShouldBypassFFXPresentDuringStreamlineStartup(
        api == APIType::D3D12, callerFromFFXFrameGenerationModule,
        streamlineStartupHandoffPending, streamlineStartupTransitionWindowActive, observerOnlyMode,
        observerStartupPresentOnlyMode);
    if (ffxStartupBypass) {
        g_FGCompat.SetFSRFGSupportPresent(true);
        PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
        if (present1Bypass) {
            static std::atomic<int> s_ffxStartupBypassLogCount1{0};
            int bypassNum = s_ffxStartupBypassLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 200) == 0) {
                HookLogImportant(
                    "DetourPresent1: Treating FFX-originated Present1 as startup handoff bypass #%d (bypass=%p, "
                    "tid=0x%04X)",
                    bypassNum, (void*)present1Bypass, GetCurrentThreadId());
            }
            return present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
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
                                     present1BypassAvailable, steamOverlayLoaded, api == APIType::D3D12,
                                     inWrapperPresent, wrappedSwapchain, hadFSRFGPhase, startupTopLevelCandidate);
    const bool startupHandoffSteamRisk = staleThirdPartyPresentHookRisk || stalePostFSRStartupHandoffPresentHookRisk;
    const bool postFSRRuntimeStartupHandoffRisk = ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
        api == APIType::D3D12, hadFSRFGPhase, startupTopLevelCandidate, safePostFSRBootstrapPath,
        startupHandoffSteamRisk);
    const bool streamlineStartupHandoffTransportRisk = ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(
        api == APIType::D3D12, inWrapperPresent, wrappedSwapchain, present1BypassAvailable, callerFromStreamlineModule,
        streamlineStartupHandoffInProgress, runtimeOwnedSwapchainActive, startupTopLevelCandidate,
        postFSRRuntimeStartupHandoffRisk, startupHandoffSteamRisk);
    if (startupTopLevelCandidate) {
        bool expected = false;
        if (g_SharedState.streamlineStartupTopLevelPresentConsumed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            static std::atomic<int> s_streamlineStartupSuppressedTopLevelLogCount1{0};
            int logCount = s_streamlineStartupSuppressedTopLevelLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent1: Keeping Streamline startup-handoff Present1 on the normal SL route #%d "
                    "(owner=0x%04X depth=%d expectedTid=0x%04X currentTid=0x%04X recentGap=1)"
                    " — top-level promotion disabled; relying on startup-policy + wrapper-progress activation",
                    logCount, presentOwner, presentDepthVal, expectedPresentThreadId, currentThreadId);
            }
        }

        if (ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(
                api == APIType::D3D12, startupTopLevelCandidate, streamlineStartupHandoffTransportRisk,
                postFSRRuntimeStartupHandoffRisk || startupHandoffSteamRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline startup-handoff Present1");

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
                InvokePostSLCallbackForFinalOutputPresent(postSLCallback, pSwapChain);
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
            InvokePostSLCallbackForFinalOutputPresent(postSLCallback, pSwapChain);
        }

        if (DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(
                api == APIType::D3D12, hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute,
                staleThirdPartyPresentHookRisk || stalePostFSRConfirmedStandalonePresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR confirmed standalone Present1");
            ProcessVSyncOverride(SyncInterval, Flags);
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
            InvokePostSLCallbackForFinalOutputPresent(postSLCallback, pSwapChain);
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
        ProcessVSyncOverride(SyncInterval, Flags);

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
        return DXGI_ERROR_INVALID_CALL;
    }
    void** vtable = *(void***)pSwapChain;
    if (!vtable || !IsReadableMemory(reinterpret_cast<const void*>(vtable), 23 * sizeof(void*)) || !vtable[22]) {
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!dxgi_shared_oPresent1Trampoline && !dxgi_shared_oPresent1) {
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
    if (dxgi_shared_s_externalOverlayPresentInvokeDepth > 0) {
        PFN_Present1 recursiveBypass1 = EnsurePresent1BypassTrampoline();
        if (recursiveBypass1) {
            static std::atomic<int> s_recursiveExternalOverlayPresent1BypassLogCount{0};
            const int bypassNum =
                s_recursiveExternalOverlayPresent1BypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent1: Bypassing recursive external-overlay Present1 #%d while guarded Steam hook is "
                    "active (depth=%d bypass=%p tid=0x%04X)",
                    bypassNum, dxgi_shared_s_externalOverlayPresentInvokeDepth, (void*)recursiveBypass1, GetCurrentThreadId());
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
            InvokePostSLCallbackForFinalOutputPresent(postSLCallback, pSwapChain);
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
        ProcessVSyncOverride(SyncInterval, Flags);
        static std::atomic<int> s_reentrantLogCount1{0};
        int reentrantNum1 = s_reentrantLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
        if (reentrantNum1 <= 10 || reentrantNum1 == 50 || reentrantNum1 == 100 || (reentrantNum1 % 500) == 0) {
            HookLog("DetourPresent1: Re-entrant #%d (postSL=%p, trampoline=%p, bypass=%p)", reentrantNum1,
                    (void*)postSLCallback, (void*)dxgi_shared_oPresent1Trampoline, (void*)dxgi_shared_oPresent1Bypass);
        }
        if (dxgi_shared_oPresent1Trampoline) {
            return dxgi_shared_oPresent1Trampoline(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        if (dxgi_shared_oPresent1Bypass) {
            return dxgi_shared_oPresent1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        if (reentrantNum1 <= 10) {
            HookLog("DetourPresent1: Re-entrant #%d → S_OK (no bypass trampoline)", reentrantNum1);
        }
        return S_OK;
    }
    auto presentDepthGuard = ce::make_scope_guard([]() { ReleasePresent(); });
    // Detect SL's E9 JMP if not yet done (same as DetourPresent). DetectSLPresentHook
    // itself owns the native-FSR suppression rule.
    if (!dxgi_shared_s_slRoutingActive.load(std::memory_order_relaxed) && IsSLInterposerLoaded()) {
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
            GetDX12StartupPresentMode(dxgi_shared_oPresent1Bypass != nullptr, &overlayModule, &startupPass);
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
                observerOnlyMode, api == APIType::D3D12, inWrapperPresent, wrappedSwapchain, dxgi_shared_oPresent1 != nullptr,
                streamlineFGRunning, dxgi_shared_s_slRoutingActive.load(std::memory_order_acquire), callerFromStreamlineModule,
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
                    safePostFSRBootstrapPath ? 1 : 0, postSLConfirmedRendering ? 1 : 0, (void*)dxgi_shared_oPresent1, presentOwner,
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
            HRESULT handoffHr = dxgi_shared_oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
            if (SUCCEEDED(handoffHr)) {
                g_SharedFpsLimiter.ApplyPostPresent();
            }
            return handoffHr;
        }
    }
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);
    UpdateDXGIPresentMetricsAndPublish(isFirstHook, "DXGIShared::DetourPresent1");

    if (api == APIType::D3D12) {
        const bool frameGenerationPresentationActive =
            streamlineFGRunning || runtimeOwnedSwapchainActive || callerFromStreamlineModule ||
            callerFromFFXFrameGenerationModule || HookHasRuntimeOwnedNativeFGPresentPath();
        // See DetourPresent: the runtime-generated classification excludes
        // callerFromStreamlineModule so the game's real-frame presents through
        // the wrapper can establish the source Present thread and unblock the
        // guarded Steam overlay invoke.
        const bool runtimeGeneratedFrame = ce::dx12_overlay_policy::IsRuntimeGeneratedFrame(
            /*noCallbackFSR=*/false, streamlineFGRunning, runtimeOwnedSwapchainActive,
            callerFromFFXFrameGenerationModule, HookHasRuntimeOwnedNativeFGPresentPath());
        const bool applicationSourcePresent = ce::dx12_overlay_policy::ShouldApplyDX12PrerenderLimitOnPresent(
            runtimeGeneratedFrame, DX12_GetGamePresentThreadId(), currentThreadId);
        HandleDX12ProcessFrame(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);
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
    if (dxgi_shared_s_slRoutingActive.load(std::memory_order_acquire) && dxgi_shared_oPresent1) {
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
            dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
            hr = CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        } else {
            WaitBackbufferFrameLatency(pSwapChain);
            hr = dxgi_shared_oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
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
}
