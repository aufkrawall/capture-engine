#include "dxgi_shared_internal.h"

namespace DXGIShared {
HRESULT ExecutePresentCore(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                  const PresentCallContext& ctx) {
    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook)
            g_SharedState.inPresentHook.store(false);
    });
    if (ctx.api == APIType::D3D12) {
        const char* overlayModule = nullptr;
        int startupPass = 0;
        DX12StartupPresentMode startupMode =
            GetDX12StartupPresentMode(dxgi_shared_oPresentBypass != nullptr, &overlayModule, &startupPass);
        if (startupMode == DX12StartupPresentMode::kPassThroughOriginal) {
            const bool steamOverlayPresent = IsSteamOverlayModule(overlayModule);
            const bool useBypass = steamOverlayPresent && dxgi_shared_oPresentBypass && !dxgi_shared_oPresentTrampoline;
            HookLogImportant(
                "DetourPresent: Startup compatibility pass #%d for third-party overlay %s "
                "(trampoline=%p bypass=%p steam=%d useBypass=%d)",
                startupPass, overlayModule ? overlayModule : "module", (void*)dxgi_shared_oPresentTrampoline, (void*)dxgi_shared_oPresentBypass,
                steamOverlayPresent ? 1 : 0, useBypass ? 1 : 0);
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply();
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            if (useBypass) {
                return dxgi_shared_oPresentBypass(pSwapChain, SyncInterval, Flags);
            }
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }
        const bool knownThirdPartyOverlaySwapchain = DXGIShared::DX12_IsThirdPartyOverlaySwapchain(pSwapChain);
        const bool startupBlockingOverlaySwapchainStillOwnsPresent =
            ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass(
                knownThirdPartyOverlaySwapchain, DXGIShared::DX12_IsStartupBlockingOverlayTaggedSwapchain(pSwapChain),
                HasStartupBlockingOverlayModuleInCurrentStack());
        if (ce::dx12_overlay_policy::ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(
                startupBlockingOverlaySwapchainStillOwnsPresent || ctx.callerFromThirdPartyOverlay)) {
            static std::atomic<int> s_overlayPresentBypassLogCount{0};
            const int logCount = s_overlayPresentBypassLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 256) == 0) {
                HookLogImportant(
                    "DetourPresent: Bypassing DX12 ProcessFrame for third-party overlay swapchain %p (caller=%s)",
                    pSwapChain, ctx.detourCallerModulePath[0] ? ctx.detourCallerModulePath : "tracked-overlay-swapchain");
            }
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply();
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }

        if (DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
                ctx.observerOnlyMode, ctx.api == APIType::D3D12, ctx.inWrapperPresent, ctx.wrappedSwapchain, dxgi_shared_oPresent != nullptr,
                ctx.streamlineFGRunning, dxgi_shared_s_slRoutingActive.load(std::memory_order_acquire), ctx.callerFromStreamlineModule,
                ctx.streamlineStartupHandoffInProgress, ctx.runtimeOwnedSwapchainActive, ctx.hadFSRFGPhase,
                ctx.safePostFSRBootstrapPath, ctx.postSLConfirmedRendering, ctx.startupTopLevelPresentAlreadyConsumed)) {
            bool expected = false;
            const bool markedStartupConsumed =
                g_SharedState.streamlineStartupTopLevelPresentConsumed.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "app-thread post-FSR Streamline startup handoff");
            static std::atomic<int> s_appThreadPostFSRStartupOverlaylessLogCount{0};
            const int handoffCount =
                s_appThreadPostFSRStartupOverlaylessLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (handoffCount <= 10 || (handoffCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: App-thread post-FSR Streamline startup-handoff overlayless SL route #%d "
                    "(markedConsumed=%d pending=%d transition=%d runtimeOwnsSwapchain=%d safeBootstrap=%d "
                    "confirmed=%d oPresent=%p owner=0x%04X depth=%d tid=0x%04X)",
                    handoffCount, markedStartupConsumed ? 1 : 0, ctx.streamlineStartupHandoffPending ? 1 : 0,
                    ctx.streamlineStartupTransitionWindowActive ? 1 : 0, ctx.runtimeOwnedSwapchainActive ? 1 : 0,
                    ctx.safePostFSRBootstrapPath ? 1 : 0, ctx.postSLConfirmedRendering ? 1 : 0, (void*)dxgi_shared_oPresent, ctx.presentOwner,
                    ctx.presentDepthVal, ctx.currentThreadId);
            }
            if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(
                    ctx.api == APIType::D3D12, ctx.postSLConfirmedRendering)) {
                DX12_RetainStreamlineStartupActivationSwapchain(
                    pSwapChain, "DetourPresent: app-thread post-FSR startup-handoff overlayless SL route");
            }
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply(true);
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            WaitBackbufferFrameLatency(pSwapChain);
            HRESULT handoffHr = dxgi_shared_oPresent(pSwapChain, SyncInterval, Flags);
            if (SUCCEEDED(handoffHr)) {
                g_SharedFpsLimiter.ApplyPostPresent();
            }
            return handoffHr;
        }
    }
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);
    UpdateDXGIPresentMetricsAndPublish(isFirstHook, "DXGIShared::DetourPresent");

    // Initialize performance metrics for CSV logging early so the scope guard
    // captures total frame time even if HandleDX11/12ProcessFrame or the FPS
    // limiter takes non-trivial time. This is the OUTER catch-all row: when the
    // dispatched work logs its own richer per-API ProcessFrame row (overlay/
    // capture breakdown), this row is SKIPPED — otherwise every such present
    // wrote TWO CSV rows (~50% zero-delta qpc pairs, sessions 20260702_094955/
    // 140811) and present-rate analysis from the CSV counted frames twice.
    const int64_t perfMetricsQpcUs = PerfLogger::GetQpcUs();
    static uint64_t s_perfFrameNum = 0;
    ++s_perfFrameNum;
    PerfLogger::BeginPresentRowScope();
    auto perfGuard = ce::make_scope_guard([&]() {
        if (PerfLogger::Get().IsEnabled() && !PerfLogger::InnerRowLoggedInPresentRowScope()) {
            FrameMetrics perfMetrics;
            perfMetrics.qpcUs = perfMetricsQpcUs;
            perfMetrics.totalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - perfMetricsQpcUs);
            perfMetrics.frameNum = s_perfFrameNum;
            if (ctx.api == APIType::D3D12)
                strncpy(perfMetrics.api, "DX12", sizeof(perfMetrics.api) - 1);
            else if (ctx.api == APIType::D3D11)
                strncpy(perfMetrics.api, "DX11", sizeof(perfMetrics.api) - 1);
            else if (ctx.api == APIType::D3D10)
                strncpy(perfMetrics.api, "DX10", sizeof(perfMetrics.api) - 1);
            else
                strncpy(perfMetrics.api, "DXGI", sizeof(perfMetrics.api) - 1);
            perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });

    if (!IsShuttingDown() && (dxgi_shared_oPresentTrampoline || dxgi_shared_oPresent)) {
        // Experimental: skip CE overlay rendering when Steam-only overlay test is active.
        // This lets us determine whether the black screen with Steam invoke is caused by
        // CE overlay + Steam overlay interaction or by Steam's handler alone.
        // Enable via environment variable: CE_STEAM_ONLY_OVERLAY=1
        {
            static std::once_flag s_steamOnlyFlag;
            std::call_once(s_steamOnlyFlag, []() {
                char envVal[32] = {};
                if (GetEnvironmentVariableA("CE_STEAM_ONLY_OVERLAY", envVal, sizeof(envVal)) > 0 && envVal[0] == '1') {
                    DXGIShared::GetSteamOnlyOverlayExperimentalFlag().store(true, std::memory_order_relaxed);
                    HookLogImportant(
                        "DetourPresent: CE_STEAM_ONLY_OVERLAY=1 detected — Steam-only "
                        "overlay test activated. CE overlay rendering will be skipped.");
                }
            });
        }
        const bool steamOnlyTest = DXGIShared::GetSteamOnlyOverlayExperimentalFlag().load(std::memory_order_relaxed);
        if (steamOnlyTest) {
            static std::atomic<int> s_steamOnlySkipLogCount{0};
            const int skipNum = s_steamOnlySkipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (skipNum < 5) {
                HookLogImportant(
                    "DetourPresent: Steam-only overlay test active — skipping ProcessFrame "
                    "#%d, Steam handler will be invoked in CallOriginalPresent",
                    skipNum + 1);
            }
        }
        // non-SL Steam path: log detection but DO NOT defer overlay ECL.
        // Deferral was attempted (builds 0.1.2960-2963) but the ECL-hook failed
        // because Steam's ECL hook only fires on frame #1 (before overlay init).
        // Every subsequent frame fell through to the fallback path (after Present).
        //
        // The real root cause was that CallOriginalPresent invoked Steam's
        // explicit hook (g_externalOverlayPresentHook) directly, skipping the E9
        // JMP chain.  Steam's handler DID NOT chain to dxgi!Present — the frame
        // was never presented, producing the black screen.
        //
        // Fix (build 0.1.2964, confirmed working on Strange Brigade DX12):
        // Call dxgi!Present through Steam's E9 JMP (presentOriginal).  Steam's
        // handler fires through the natural hook chain with the correct return
        // address and chains to the original dxgi!Present.  Overlay ECL is
        // submitted normally (non-deferred) during ProcessFrame.
        const bool nonSLSteamInvokePath =
            !steamOnlyTest && ctx.api == APIType::D3D12 && ctx.steamOverlayLoaded && !IsSLInterposerLoaded();
        if (nonSLSteamInvokePath) {
            static std::atomic<int> s_steamPathLog{0};
            if (s_steamPathLog.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant("DetourPresent: non-SL Steam path — SyncInterval=%u Flags=%u (normal overlay submit)",
                                 SyncInterval, Flags);
            }
        }

        if (!steamOnlyTest && ctx.api == APIType::D3D12) {
            // Near-passthrough during no-callback FSR FG: when the UI-texture bundle is active
            // (GTA-style), skip ProcessFrame entirely — even the minimal path's QueryInterface +
            // RecordFrame + inner ProcessFrame overhead on the runtime queue desyncs AMD's QPC-timed
            // pacing and freezes GTA (~900 frames). When the bundle is unavailable (no registered UI
            // texture intercepted — test app), call the minimal ProcessFrame path so the overlay
            // renders through the normal DX12 route.
            const bool noCallbackFSRFG = DX12_IsNativeFSRInternalNoCallbackCompositionActive();
            const bool frameGenerationPresentationActive =
                noCallbackFSRFG || ctx.streamlineFGRunning || ctx.runtimeOwnedSwapchainActive || ctx.callerFromStreamlineModule ||
                ctx.callerFromFFXFrameGenerationModule || HookHasRuntimeOwnedNativeFGPresentPath();
            // Non-FG calls keep existing queue-depth behavior. Once a runtime
            // can emit its own Presents, only the pre-FG game thread may pace.
            const bool applicationSourcePresent =
                ce::dx12_overlay_policy::ShouldApplyDX12PrerenderLimitOnPresent(
                    frameGenerationPresentationActive, DX12_GetGamePresentThreadId(), ctx.currentThreadId);
            {
                // Transition-edge diagnostic: the post-startup ProcessFrame route log is rate-limited, so the
                // FSR<->off handoff is otherwise invisible. Mark the exact edge + ownership/queue state so the
                // FSR->off recovery (does normal ProcessFrame resume, or does the runtime-owned latch stick?) is
                // attributable from the log alone.
                static std::atomic<bool> s_prevNoCallbackFSRFG{false};
                if (s_prevNoCallbackFSRFG.exchange(noCallbackFSRFG, std::memory_order_relaxed) != noCallbackFSRFG) {
                    HookLogImportant(
                        "DetourPresent: no-callback FSR FG window %s — overlay route is now %s (runtimeOwns=%d)",
                        noCallbackFSRFG ? "STARTED" : "ENDED",
                        noCallbackFSRFG ? "UI-resource bundle only (no backbuffer submit)"
                                        : "normal ProcessFrame backbuffer",
                        (DXGIShared::DoesFGRuntimeOwnSwapchain() || HookHasRuntimeOwnedNativeFGPresentPath()) ? 1 : 0);
                }
            }
            if (noCallbackFSRFG) {
                // CRASH BOUNDARY: under runtime-owned native FSR FG, CE must NEVER submit overlay GPU work on
                // AMD's backbuffer / runtime present queue (the documented ffxQuery null-deref AV, session
                // 20260621_191028). The overlay's only AMD-safe channel there is the UI-resource composition:
                // CE draws onto the registered/CE-substituted UI texture on its OWN fenced queue
                // (DX12_CompositeOverlayOntoCachedFFXUiResource) and AMD composites it post-interpolation, so
                // the route selector returns kSkipBundleCovers whenever AMD owns the swapchain.
                const bool runtimeOwnsSwapchain =
                    DXGIShared::DoesFGRuntimeOwnSwapchain() || HookHasRuntimeOwnedNativeFGPresentPath();
                // STALE-LATCH SIGNAL: during ACTIVE no-callback FSR FG the game presents on AMD's SEPARATE FG
                // queue (live swapchain queue != origGame). Once the game recreates a native swapchain on its
                // own queue (live swapchain queue == origGame), AMD's FG swapchain is gone — a still-set
                // no-callback latch is stale and the backbuffer route is safe again (FSR->off recovery).
                const bool liveSwapchainQueueIsOriginalGameQueue = DX12_IsLiveSwapchainQueueOriginalGameQueue();
                // SUSPEND SIGNAL: native FSR FG explicitly disabled while AMD still owns the swapchain (no-callback
                // suspension — AMD keeps the swapchain but is NOT interpolating). NOTE (session 20260703_210021):
                // the backbuffer submit is NOT safe during a suspension after all — AMD stops flushing its
                // runtime queue while suspended, so CE's overlay GPU-completion fence never signals and this
                // present stalls ~1s (app → ~1 fps). So the route keeps a runtime-owned suspension on the BUNDLE
                // (kSkipBundleCovers), same as active FG; the backbuffer is reached only once the game owns its
                // OWN native swapchain again (liveSwapchainQueueIsOriginalGameQueue). This flag no longer relaxes
                // the route toward the backbuffer.
                const bool fsrFGDisabledSuspendPending = DX12_IsNativeFSRFGSuspendedDisablePending();
                // Defensive guard-rail signal: AMD actively interpolating on its own FG queue (runtime-owned, NOT
                // suspended, live queue is AMD's separate FG queue). The route never yields kMinimalBackbuffer for
                // ANY runtime-owned state now, so reaching the backbuffer branch here would be a logic regression.
                const bool amdActivelyInterpolatingOnFGQueue =
                    runtimeOwnsSwapchain && !fsrFGDisabledSuspendPending && !liveSwapchainQueueIsOriginalGameQueue;
                // bundleOverlayActivelyFiring is hardwired false: the fenced composite is driven ONLY from the
                // kSkipBundleCovers arm below, and while AMD owns the swapchain the route selects kSkipBundleCovers
                // regardless of this arg (active OR suspended). It is consulted only in the non-runtime-owned
                // escape hatch, where AMD does not own the swapchain and the backbuffer is genuinely safe.
                const ce::dx12_overlay_policy::NoCallbackFSRFGOverlayRoute noCallbackRoute =
                    ce::dx12_overlay_policy::ChooseNoCallbackFSRFGOverlayRoute(
                        runtimeOwnsSwapchain, liveSwapchainQueueIsOriginalGameQueue, fsrFGDisabledSuspendPending,
                        DX12_IsFFXUiResourceCachedForBundle(), /*bundleOverlayActivelyFiring=*/false);
                if (noCallbackRoute == ce::dx12_overlay_policy::NoCallbackFSRFGOverlayRoute::kSkipBundleCovers) {
                    // The overlay rides AMD's UI-resource composition. PRIMARY driver: the FFX proxy-present
                    // prework already composited (and re-asserted the substitute registration) on the GAME
                    // thread before AMD's proxy Present ran. This present arrives on AMD's PRESENTER thread
                    // for AMD's internal real swapchain and must then stay hands-off: blocking CE work here
                    // stalls AMD's pacing-critical presenter, and the substitute re-assert from this thread
                    // deadlocks the game permanently (session 20260701_213656 freeze dump: AMD's Present holds
                    // its swapchain criticalSection on the game thread while fence-spinning without timeout;
                    // registerUiResource from the presenter thread closes the cycle). FALLBACK driver: while
                    // the proxy hook is not live (not installed / game not presenting through it), drive the
                    // composite from here on CE's OWN fenced queue — WITHOUT the re-assert (it hard-refuses
                    // outside the prework) — so the overlay is never silently blank.
                    static std::atomic<bool> s_proxyDrivingEdge{false};
                    const bool proxyDriving = DX12_IsFFXProxyPresentHookDriving();
                    if (s_proxyDrivingEdge.exchange(proxyDriving, std::memory_order_relaxed) != proxyDriving) {
                        HookLogImportant(
                            "DetourPresent: no-callback FSR FG composite driver is now %s",
                            proxyDriving
                                ? "the proxy-present prework (game thread) — presenter-thread present is passthrough"
                                : "the DetourPresent fallback (presenter thread, composite only, no re-assert)");
                    }
                    if (!proxyDriving) {
                        DX12_CompositeOverlayOntoCachedFFXUiResource();
                    }
                } else if (amdActivelyInterpolatingOnFGQueue) {
                    // DEFENSIVE GUARD RAIL: the backbuffer submit is forbidden ONLY while AMD is actively
                    // interpolating on its own FG queue. The route selector never produces kMinimalBackbuffer in
                    // that case, so reaching here is a logic regression at the exact crash boundary — log loudly
                    // and skip rather than risk the ffxQuery wedge. (A suspension, or a stale latch with the live
                    // present back on origGame, is NOT this case — those correctly take the backbuffer path below.)
                    static std::atomic<int> s_noCallbackBackbufferGuardLog{0};
                    const int guardLog = s_noCallbackBackbufferGuardLog.fetch_add(1, std::memory_order_relaxed);
                    if (guardLog < 20 || (guardLog % 300) == 0) {
                        HookLogImportant(
                            "DetourPresent: GUARD — refusing minimal backbuffer ProcessFrame while AMD actively "
                            "interpolates on its FG queue (crash boundary; overlay rides UI composition only) log=%d",
                            guardLog + 1);
                    }
                } else {
                    // Backbuffer submit is safe here: AMD does not own this swapchain (non-runtime-owned escape
                    // hatch), OR a no-callback SUSPENSION (FG disabled, AMD not interpolating), OR a STALE
                    // no-callback latch with the live present back on the game's own queue (FSR->off recovery).
                    // Draw via the minimal backbuffer path so the overlay is NEVER blank across these windows;
                    // once active interpolation resumes, control returns to the composite skip branch above.
                    DX12_ProcessFrameMinimal(pSwapChain, applicationSourcePresent,
                                             frameGenerationPresentationActive);
                }
            } else {
                HandleDX12ProcessFrame(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);
            }
        } else if (!steamOnlyTest && DXGIShared::ShouldRunSharedD3D10Or11ProcessFrame(ctx.api)) {
            if (ctx.api == APIType::D3D10) {
                static std::atomic<int> s_d3d10ProcessFrameLogCount{0};
                const int logCount = s_d3d10ProcessFrameLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 5) {
                    HookLogImportant(
                        "DetourPresent: Routing D3D10 swapchain through shared DX10/DX11 ProcessFrame path #%d",
                        logCount + 1);
                }
            }
            HandleDX11ProcessFrame(pSwapChain, true);
        }
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

    // Always wait for overlay fence before Present.  The overlay ECL was
    // submitted during ProcessFrame (non-deferred), so the fence signals
    // completion before the buffer flips.
    if (ctx.api == APIType::D3D12 && !DX12_IsNativeFSRInternalNoCallbackCompositionActive()) {
        // DIAGNOSTIC: time the overlay-completion wait (one half of the present-thread cost; the
        // other is the real Present call timed below). Compare 32-bit vs 64-bit; a multi-second
        // wait here means CE's overlay GPU work hung, vs a slow real Present means the swapchain
        // flip blocked on the hung GPU.
        LARGE_INTEGER diagWaitT0, diagWaitT1, diagWaitFreq;
        QueryPerformanceCounter(&diagWaitT0);
        InvokeDX12WaitForOverlayCompletion(nullptr);
        QueryPerformanceCounter(&diagWaitT1);
        QueryPerformanceFrequency(&diagWaitFreq);
        const double diagWaitMs =
            (double)(diagWaitT1.QuadPart - diagWaitT0.QuadPart) * 1000.0 / (double)diagWaitFreq.QuadPart;
        if (diagWaitMs >= 5.0) {
            static std::atomic<int> s_diagWaitLog{0};
            const int n = s_diagWaitLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 200 || (n % 50) == 0) {
                HookLogImportant("DX12 DIAG: overlay-completion wait SLOW %.1fms (tid=0x%04X)", diagWaitMs,
                                 GetCurrentThreadId());
            }
        }
    }

    // Streamline external-overlay transport guard. On Steam's E9/vtable-hook
    // topology s_slRoutingActive intentionally remains false for the complete
    // swapchain lifetime, so this is a topology boundary rather than a startup
    // lifetime test. The guarded Steam path is allowed only on the verified
    // application source-Present thread; runtime-generated worker Presents use
    // the bypass trampoline and cannot synchronously enter a foreign handler.
    if (ctx.callerFromStreamlineModule &&
        !dxgi_shared_s_slRoutingActive.load(std::memory_order_relaxed) &&
        ctx.steamOverlayLoaded) {
        HRESULT guardedSteamHr = S_OK;
        if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,
                                                        "SL external-overlay vtable transport",
                                                        &guardedSteamHr)) {
            if (ctx.api == APIType::D3D12) {
                InvokeDX12FlushDeferredSignal();
            }
            if (SUCCEEDED(guardedSteamHr)) {
                g_SharedFpsLimiter.ApplyPostPresent();
            }
            return guardedSteamHr;
        }

        PFN_Present bypass = EnsurePresentBypassTrampoline();
        if (bypass) {
            static std::atomic<int> s_streamlineExternalOverlayBypassCount{0};
            int bypassNum = s_streamlineExternalOverlayBypassCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent: Streamline external-overlay transport bypass #%d "
                    "(sourceTid=0x%04X tid=0x%04X)",
                    bypassNum, DX12_GetGamePresentThreadId(), GetCurrentThreadId());
            }
            HRESULT bypassHr = bypass(pSwapChain, SyncInterval, Flags);
            if (ctx.api == APIType::D3D12) {
                InvokeDX12FlushDeferredSignal();
            }
            if (SUCCEEDED(bypassHr)) {
                g_SharedFpsLimiter.ApplyPostPresent();
            }
            return bypassHr;
        }
    }

    HRESULT hr;
    if (dxgi_shared_s_slRoutingActive.load(std::memory_order_acquire)) {
        ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
        bool runtimeOwnedNativeFGPresentPath = false;
        // Safety: if SL routing is still active while the native FSR path owns
        // presentation, force-disable it. The effective runtime label alone is
        // not sufficient here because the explicit native-FSR OFF teardown window
        // can still be runtime-owned while temporarily publishing Off.
        if (ShouldKeepSLPresentRoutingDisabledNow(&runtimeMode, &runtimeOwnedNativeFGPresentPath)) {
            static int s_fsrlatchCount = 0;
            int latchNum = ++s_fsrlatchCount;
            if (latchNum <= 5) {
                HookLogImportant(
                    "DetourPresent: SL routing was active while native FG owned Present routing — "
                    "force-disabling SL routing (latch #%d, runtime=%s runtimeOwnedNativeFG=%d). Present will go "
                    "through trampoline directly.",
                    latchNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode), runtimeOwnedNativeFGPresentPath ? 1 : 0);
            }
            dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
            hr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        } else {
            // Route through oPresent which has SL's JMP (E9 or FF 25).  This
            // lets SL process FG.  SL's trampoline will re-enter DetourPresent
            // (handled above — forwarded to oPresentTrampoline for real Present).
            static std::atomic<int> s_slCallCount{0};
            int slCallNum = s_slCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (slCallNum <= 20 || (slCallNum % 500) == 0) {
                HookLog("DetourPresent: Calling oPresent=%p (SL route, call #%d, tid=0x%04X)", dxgi_shared_oPresent, slCallNum,
                        GetCurrentThreadId());
            }
            WaitBackbufferFrameLatency(pSwapChain);
            hr = dxgi_shared_oPresent(pSwapChain, SyncInterval, Flags);
            if (slCallNum <= 20 || (slCallNum % 500) == 0) {
                HookLog("DetourPresent: oPresent returned hr=0x%08X (call #%d)", hr, slCallNum);
            }
        }
    } else {
        static std::atomic<int> s_nonSlPresentCount{0};
        int nonSlNum = s_nonSlPresentCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (nonSlNum == 1 || (nonSlNum % 1000) == 0) {
            HookLog("DetourPresent: non-SL routing path (call #%d, slRouting=%d, tid=0x%04X)", nonSlNum,
                    dxgi_shared_s_slRoutingActive.load(std::memory_order_relaxed), GetCurrentThreadId());
        }
        hr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Note: The Steam ECL deferred overlay handling was removed in build 0.1.2964.
    // The ECL-hook approach (builds 0.1.2960-2963) was a dead end — Steam's ECL
    // only fires on frame #1, making deferral useless.  The real root cause was
    // the wrong Steam invocation path in CallOriginalPresent (explicit hook
    // skipped the E9 JMP chain).  Fix confirmed working on Strange Brigade DX12:
    // overlay ECL submitted normally, Steam called through E9 JMP at presentOriginal,
    // all three layers (game, CE overlay, Steam overlay) visible simultaneously.

    // Flush deferred overlay fence Signal AFTER Present.  The NVIDIA driver
    // stalls the GPU when Signal sits between our overlay ECL and Present.
    // Skip during no-callback FSR FG: the deferred Signal on the game queue is an extra
    // ID3D12CommandQueue::Signal on an AMD-tracked queue — exactly what wedges ffxQuery pacing.
    if (ctx.api == APIType::D3D12 && !DX12_IsNativeFSRInternalNoCallbackCompositionActive()) {
        InvokeDX12FlushDeferredSignal();
        // Feed the present result into focus-transition/occlusion tracking so vtable-hooked
        // DX12 apps engage the invisible-safe not-presentable hold during the Alt+Tab mode
        // switch (the wrapped path already does this via the wrapper).
        NoteDX12PresentResultForVtablePath(pSwapChain, "Present", SyncInterval, Flags, hr);
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
