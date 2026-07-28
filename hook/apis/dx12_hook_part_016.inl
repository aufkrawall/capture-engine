
    DXGIShared::ResetStreamlineStartupTransitionState();
    if (!keepConfirmedPostSLAliveAcrossOff) {
        SetPostSLCallbackInstalled(false, "DX12: Streamline FG OFF");
        g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    } else {
        HookLogImportant(
            "DX12: Streamline FG OFF — PostSL callback stays installed for make-before-break keep-alive "
            "(confirmed rendering preserved)");
    }
    g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    g_PostSLStallCounter.store(0, std::memory_order_release);
    g_PostSLStableFrameCount.store(0, std::memory_order_release);
    g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
    g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: Streamline FG OFF");
    g_SLOffHeuristicGrace.store(600, std::memory_order_release);
    g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(false, std::memory_order_release);
    if (g_PostSLLastWorkingQueue) {
        MarkPostSLRecentTeardownActivity("DX12: Streamline FG OFF seeded recent PostSL teardown activity",
                                         g_PostSLLastWorkingQueue);
    }
    RequestFGDetectionHeuristicReset();
    g_FGCompat.SetHeuristicFSRFGActive(false);
    g_FGCompat.ClearNvidiaSMState();
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
        const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
        ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, 0.0f, 0.0f, 1, "DX12_OnStreamlineFGStateChanged");
    }
    InvalidateAllOverlayCachedFrames();
    g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);
    if (!keepConfirmedPostSLAliveAcrossOff) {
        // A lifecycle reset would force a fresh reactivation epoch/warm-up;
        // keep-alive must keep the continuously-live path untouched.
        ResetPostSLLifecycleForTransition("DX12: Streamline FG OFF transition", true, true);
    }

    if (g_HadFSRFGPhase) {
        ID3D12CommandQueue* staleScQueue = nullptr;
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        // Overlay fallback permission must not be reused as a proof that native
        // FSR ownership is stale.  Preserve ownership while FSR/native Present
        // state is still active and let the explicit native-FSR OFF path clear it.
        const bool preserveRuntimeOwnedFSRTakeover =
            ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
                g_FGRuntimeOwnsSwapchain, false, runtimeMode, g_FGCompat.IsFSRFGApiActive(),
                HookHasRuntimeOwnedNativeFGPresentPath(), false);
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);

            if (preserveRuntimeOwnedFSRTakeover) {
                HookLogImportant(
                    "DX12: Streamline FG OFF overlapped with authoritative/runtime-owned FSR takeover "
                    "(runtime=%s scQueue=%p origGame=%p) — preserving FSR swapchain ownership until native FSR "
                    "emits a stronger off signal",
                    ce::fg_runtime::GetRuntimeModeName(runtimeMode), g_SwapchainQueue, g_OriginalGameQueue);
            } else if (g_FGRuntimeOwnsSwapchain) {
                g_FGRuntimeOwnsSwapchain = false;
                DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                g_FGRuntimeOwnsSwapchainSince = 0;
                ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
                ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                if (g_FGCompat.IsFSRFGApiActive()) {
                    SetNativeFSRStartupConfigureArmingPending(false, "Streamline FG off cleared FSR ownership");
                    ClearOfficialFFXRuntimeOwnedPresentPathAssumption("Streamline FG off cleared FSR ownership");
                    g_FGCompat.SetFSRFGActive(false);
                    g_FGCompat.SetFSRFGMultiplier(0);
                    ResetAuthoritativeFSRRealFrameOnlyStreak();
                }
                HookLogImportant("DX12: Streamline FG OFF after FSR history — clearing lingering FG runtime ownership");
            }

            if (!preserveRuntimeOwnedFSRTakeover && g_SwapchainQueue && g_SwapchainQueue != g_OriginalGameQueue) {
                staleScQueue = g_SwapchainQueue;
                g_SwapchainQueue = nullptr;
                g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                g_SwapchainQueueCaptureTime = 0;
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — releasing stale swapchain queue %p so top-level "
                    "recovery can recapture the live non-FG queue (origGame=%p)",
                    staleScQueue, g_OriginalGameQueue);
            }
        }

        if (staleScQueue) {
            staleScQueue->Release();
        }

        if (!preserveRuntimeOwnedFSRTakeover) {
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — leaving swapchain queue uncaptured until a live non-FG "
                "queue is observed again (origGame=%p primary=%p cmdQ=%p)",
                g_OriginalGameQueue, g_PrimaryGameQueue.load(std::memory_order_acquire),
                g_CommandQueue.load(std::memory_order_acquire));
        }

        // A confirmed explicit-OFF keep-alive continues rendering on the exact
        // proxy swapchain/queue that succeeded one Present earlier. Preserve its
        // warm RTV/sync objects until a separately proven normal swapchain takes
        // ownership; tearing them down here defeats make-before-break and adds a
        // transition-time GPU drain/rebuild on the same route.
        const bool preserveConfirmedPostSLProxyResources =
            keepConfirmedPostSLAliveAcrossOff && g_State.overlayInit && g_State.syncInit &&
            g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire) != nullptr;

        // The post-FSR DLSS path rendered through Streamline/PostSL against a
        // different swapchain topology than the resumed non-FG path. Force a
        // swapchain-level reinit only when no exact confirmed-proxy keep-alive
        // remains; the normal-return proof retires the preserved state later.
        if (!preserveRuntimeOwnedFSRTakeover && g_State.overlayInit && !preserveConfirmedPostSLProxyResources) {
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — forcing overlay swapchain reinit for non-FG recovery");
            g_State.overlayInit = false;
            CleanupRTVs();
        }

        if (!preserveRuntimeOwnedFSRTakeover && preserveConfirmedPostSLProxyResources) {
            g_FGTransitionCooldown.store(0, std::memory_order_release);
            g_PostSLCooldownRemaining.store(0, std::memory_order_release);
            g_NeedOffscreenOverlayAfterPostFSRNonFG.store(true, std::memory_order_release);
            // The direct state-change callback already owns this OFF edge. Keep
            // the later ProcessFrame outer tracker from replaying a destructive
            // teardown once the native normal route eventually returns.
            g_OuterTrackedSLFGRunning.store(false, std::memory_order_release);
            g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release);
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — preserved warm confirmed-PostSL proxy resources "
                "for exact-swapchain keep-alive (proxy=%p queue=%p; no reinit/copy/wait)",
                g_LastSuccessfulPostSLSwapchain.load(std::memory_order_relaxed), g_PostSLLastWorkingQueue);
        } else if (!preserveRuntimeOwnedFSRTakeover &&
                   ce::dx12_overlay_policy::ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(
                       g_HadFSRFGPhase, g_State.overlayInit, g_State.syncInit)) {
            g_State.syncInit = false;
            g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
            g_OuterTrackedSLFGRunning.store(false, std::memory_order_release);
            g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release);
            auto* oldRealECL = (void*)g_RealD3D12ECL.load(std::memory_order_acquire);
            const ID3D12CommandQueue* currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
            const ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
            const bool commandQueueSettledToPrimary =
                currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
            // DLSS-FG SUSPEND with FSR history (slDLSSGSetOptions(off), proxy stays live):
            // when the make-before-break keep-alive is armed this is a CONFIRMED PostSL
            // suspension, not a teardown. PostSL confirmed rendering means the overlay ECL on
            // the runtime-owned SL queue already succeeded many times this epoch (device
            // demonstrably healthy on that exact path) and the proxy keeps presenting, so
            // there is no Streamline teardown for the 60-frame deferral to wait for — it only
            // blanks a provably-live overlay (session 20260613_202646: 60-present /
            // confirmedDuringStreak=1 blank, gate=overlay-backend-uninitialized). The bypass
            // mirrors ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown but is gated on
            // keepConfirmedPostSLAliveAcrossOff (captured at function entry, BEFORE the teardown
            // above nulled the swapchain queue / cleared overlayInit) plus a device-health guard;
            // a real FSR/native-FG takeover or removed device keeps the strict cooldown.
            auto* suspendDevice = g_Device.load(std::memory_order_acquire);
            const bool suspendDeviceRemoved =
                suspendDevice != nullptr && FAILED(suspendDevice->GetDeviceRemovedReason());
            const bool confirmedPostSLSuspensionImmediateReinit =
                keepConfirmedPostSLAliveAcrossOff && !suspendDeviceRemoved;
            const bool useShortPostFSRCooldown = ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                commandQueueSettledToPrimary, g_HadFSRFGPhase, true);
            const int cooldownFrames =
                confirmedPostSLSuspensionImmediateReinit ? 0 : (useShortPostFSRCooldown ? 15 : 60);
            g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                g_FGTransitionCooldown.load(std::memory_order_acquire), cooldownFrames,
                confirmedPostSLSuspensionImmediateReinit || useShortPostFSRCooldown);
            g_PostSLCooldownRemaining.store(g_FGTransitionCooldown.load(std::memory_order_acquire),
                                            std::memory_order_release);
            g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
            if (confirmedPostSLSuspensionImmediateReinit) {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — confirmed-PostSL suspension (proxy stays live, "
                    "keep-alive armed), immediate warm overlay reinit instead of 60-frame blank (cooldown=%d)",
                    g_FGTransitionCooldown.load(std::memory_order_acquire));
            } else {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — deferring non-FG overlay reinit for %d frames so "
                    "Talos/Streamline teardown can settle before pre-SL resources are rebuilt",
                    g_FGTransitionCooldown.load(std::memory_order_acquire));
            }
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — invalidated sync resources for delayed reinit");
            if (ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(
                    commandQueueSettledToPrimary, g_HadFSRFGPhase)) {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — preserving realECL %p for delayed non-FG "
                    "recovery because cmdQ=%p already settled to primary",
                    oldRealECL, currentCommandQueue);
            } else {
                g_RealD3D12ECL.store(nullptr, std::memory_order_release);
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — cleared realECL %p for delayed non-FG recovery",
                    oldRealECL);
            }
            g_NeedOffscreenOverlayAfterPostFSRNonFG = true;
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — enabled offscreen overlay compositing for non-FG "
                "recovery (backbuffer state indeterminate after FG teardown)");
        }
    }

    HookLogImportant("DX12: Streamline FG OFF — seeded heuristic reset/grace (slOffGrace=600)");
    HookLogImportant("DX12: Streamline FG OFF — applied PostSL callback/keep-alive state");
}

// HWND → swapchain tracking for diagnostics and E_ACCESSDENIED recovery.
// We do NOT AddRef tracked swapchains — this avoids extending their lifetime
// beyond what the game intends, which previously caused UE5 assertion crashes
// during FG switching (our AddRef kept the old SC alive, holding the HWND,
// and our forced destruction happened at the wrong time in UE5's lifecycle).
static std::mutex s_hwndSwapchainMutex;
static std::map<HWND, std::vector<IDXGISwapChain*>> s_hwndSwapchainMap;

static void MarkThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath = nullptr) {
    DXGIShared::DX12_RegisterThirdPartyOverlaySwapchain(pSwapChain, creatorModulePath);
}

static void MarkThirdPartyOverlaySwapchain(IDXGISwapChain1* pSwapChain, const char* creatorModulePath = nullptr) {
    MarkThirdPartyOverlaySwapchain(static_cast<IDXGISwapChain*>(pSwapChain), creatorModulePath);
}

static void ForgetSwapchainFromTracking(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return;
    }

    DXGIShared::DX12_UnregisterThirdPartyOverlaySwapchain(pSwapChain);

    std::lock_guard<std::mutex> hwndLock(s_hwndSwapchainMutex);
    for (auto it = s_hwndSwapchainMap.begin(); it != s_hwndSwapchainMap.end();) {
        auto& vec = it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), pSwapChain), vec.end());
        if (vec.empty()) {
            it = s_hwndSwapchainMap.erase(it);
        } else {
            ++it;
        }
    }
}

// Track a swapchain's HWND association (called from ProcessFrame and deep hook).
// NO AddRef — raw pointer tracking only. Pointers may become stale when the
// game destroys the swapchain, which is fine because we only use them for
// reactive E_ACCESSDENIED recovery with SEH protection.
static void TrackSwapchainHwnd(IDXGISwapChain* pSwapChain, HWND hWnd) {
    if (!hWnd || !pSwapChain)
        return;
    std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
    auto& vec = s_hwndSwapchainMap[hWnd];
    for (auto* sc : vec) {
        if (sc == pSwapChain)
            return;  // Already tracked
    }
    vec.push_back(pSwapChain);
}

// Forward declaration — defined below near DetourCreateSwapChainGlobal
static bool IsStreamlineLoaded();

// Deep hook wrapper for CreateSwapChainForHwnd.
// Intercepts ALL callers (including Streamline's internal trampoline calls).
// Uses REACTIVE E_ACCESSDENIED recovery: tries the call first, only intervenes
// if it fails. This avoids destroying swapchains prematurely (which caused UE5
// assertion crashes when our proactive pre-check destroyed SCs that UE5's
// deferred viewport code still referenced).
static HRESULT STDMETHODCALLTYPE DeepHookCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    if (HookIsShuttingDown()) {
        if (s_deepHookTrampoline)
            return s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
        return E_FAIL;
    }

    // Skip side-effects for temp swapchains created during hook installation
    if (g_CreatingTempSwapchain.load(std::memory_order_acquire)) {
        HookLog("DeepHook: Temp swapchain creation — passthrough (no tracking)");
        return s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    }

    const CreateSwapchainForHwndCallerContext callerContext = ResolveCreateSwapchainForHwndCallerContext();
    const void* callerAddress = callerContext.callerAddress;
    const bool callerFromFFXFGModule = callerContext.callerFromFFXFGModule;
    const bool rawCallerFromThirdPartyOverlay = callerContext.callerFromThirdPartyOverlay;
    const char* callerModulePath = callerContext.callerModulePath;
    char ffxStackModulePath[MAX_PATH] = {};
    const bool ffxFrameGenerationInStack =
        ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModulePath, sizeof(ffxStackModulePath));
    const bool callerFromStreamlineFGModule =
        callerAddress && ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(callerAddress);
    const bool streamlineFrameGenerationInStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack();
    const bool callerFromThirdPartyOverlay = ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
        "DeepHook", rawCallerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
        callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath);
    const auto captureEvidence = BuildCreateSwapchainQueueCaptureEvidence(
        callerAddress, callerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
        callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath, ffxStackModulePath);

    HookLogImportant("DeepHook: CreateSwapChainForHwnd ENTER factory=%p device=%p hwnd=%p BufferCount=%u SwapEffect=%d",
                     pThis, pDevice, hWnd, pDesc ? pDesc->BufferCount : 0, pDesc ? (int)pDesc->SwapEffect : -1);

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    const bool applyDescriptorOverrides = ShouldApplySwapchainDescriptorOverridesForCreate(captureEvidence);
    if (pDesc && !applyDescriptorOverrides) {
        LogSkippedSwapchainDescriptorOverridesForRuntimeCreate("DeepHook", captureEvidence, pDesc->BufferCount,
                                                               pDesc->Flags, pDesc->SwapEffect);
    }
    if (pDesc && applyDescriptorOverrides) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                HookLogImportant("DeepHook: Skipping BufferCount override %u < game's %u (flip model)", requested,
                                 modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
                HookLogImportant("DeepHook: Overriding BufferCount %u -> %u", modifiedDesc.BufferCount, requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    PrepareForAuthoritativeFFXSwapchainCreate(captureEvidence, "DeepHook");

    // Try the call first — let the game/SL handle SC lifecycle naturally
    HRESULT hr = s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
    HookLogImportant("DeepHook: Trampoline returned hr=0x%08X sc=%p", hr, (ppSC ? *ppSC : nullptr));

    const bool protectedOfficialFFXStartupCreate =
        ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(captureEvidence);
    if (SUCCEEDED(hr) && ppSC && *ppSC && !callerFromThirdPartyOverlay && !protectedOfficialFFXStartupCreate) {
        // Only start transition cooldown when a swapchain recreation actually
        // succeeded. Starting it on E_ACCESSDENIED leaves the overlay in a
        // half-transitioned state while Streamline/game keeps the old chain.
        StartTransitionCooldown();
    }

    // Reactive recovery: if E_ACCESSDENIED, an old SC still holds the HWND.
    // DON'T force-destroy — that invalidates game-held references and causes
    // delayed UE5 assertion crashes. For ordinary callers we can clean up our
    // overlay refs and do a very brief retry. For runtime-managed Streamline /
    // authoritative FFX takeover paths, return the error untouched so the
    // runtime can manage its own swapchain state machine.
    if (hr == E_ACCESSDENIED && hWnd) {
        const bool streamlineModuleLoaded = IsStreamlineLoaded();
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
        const bool passThroughForRuntimeManagedFG =
            ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(
                streamlineModuleLoaded, streamlineFGRunning, streamlineStartupHandoffPending, callerFromFFXFGModule,
                ffxFrameGenerationInStack);
        const char* passThroughReason =
            callerFromThirdPartyOverlay
                ? "third-party overlay caller"
                : ((callerFromFFXFGModule || ffxFrameGenerationInStack) ? "authoritative FFX takeover"
                                                                        : "Streamline active");
        if (ce::dx12_overlay_policy::ChooseCreateSwapchainAccessDeniedRecovery(passThroughForRuntimeManagedFG,
                                                                               callerFromThirdPartyOverlay) ==
            ce::dx12_overlay_policy::CreateSwapchainAccessDeniedRecovery::kMinimalCEReleaseThenEscalate) {
            // See the INLINE variant: a failed runtime-managed create is fatal to the game (null swapchain
            // deref, session 20260702_092933). Minimal CE unpin (retained startup-activation swapchain) +
            // retry first; full overlay cleanup only as the last resort before returning the error.
            HookLogImportant(
                "DeepHook: E_ACCESSDENIED for HWND=%p — %s, minimal CE unpin + retry "
                "(slFG=%d startupPending=%d callerFFX=%d stackFFX=%d retained=%d module=%s)",
                hWnd, passThroughReason, streamlineFGRunning ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                callerFromFFXFGModule ? 1 : 0, ffxFrameGenerationInStack ? 1 : 0,
                HasRetainedStreamlineStartupActivationSwapchain() ? 1 : 0,
                callerModulePath[0] ? callerModulePath : "unknown");
            ReleaseStreamlineStartupActivationSwapchain("DeepHook: E_ACCESSDENIED runtime-managed minimal recovery");
            for (int attempt = 1; attempt <= 5 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant("DeepHook: runtime-managed minimal-recovery retry %d succeeded hr=0x%08X sc=%p",
                                     attempt, hr, (ppSC && *ppSC) ? (void*)*ppSC : nullptr);
                }
            }
            if (hr == E_ACCESSDENIED) {
                HookLogImportant(
                    "DeepHook: runtime-managed minimal recovery still E_ACCESSDENIED — escalating to full overlay "
                    "cleanup for HWND=%p",
                    hWnd);
                {
                    std::lock_guard<std::recursive_mutex> overlayLock(g_OverlayMutex);
                    g_LastSwapChain = nullptr;
                    CleanupOverlay();
                    CleanupRTVs();
                    g_State.overlayInit = false;
                }
                {
                    std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
                    s_hwndSwapchainMap.erase(hWnd);
                }
                if (ppSC && *ppSC) {
                    ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
                }
                for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                    Sleep(20);
                    hr = s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                    if (SUCCEEDED(hr)) {
                        HookLogImportant("DeepHook: escalated full-cleanup retry %d succeeded hr=0x%08X", attempt, hr);
                    }
                }
            }
            if (hr == E_ACCESSDENIED) {
                HookLogImportant(
                    "DeepHook: E_ACCESSDENIED persists after CE unpin + full cleanup — returning the error to the "
                    "caller (HWND=%p)",
                    hWnd);
            }
        } else {
            HookLogImportant(
                "DeepHook: E_ACCESSDENIED for HWND=%p — cleaning up overlay refs "
                "(slLoaded=%d slFG=%d startupPending=%d callerFFX=%d stackFFX=%d module=%s)",
                hWnd, streamlineModuleLoaded ? 1 : 0, streamlineFGRunning ? 1 : 0,
                streamlineStartupHandoffPending ? 1 : 0, callerFromFFXFGModule ? 1 : 0,
                ffxFrameGenerationInStack ? 1 : 0, callerModulePath[0] ? callerModulePath : "unknown");

            // Clean up ALL overlay resources so we don't hold stale refs that
            // prevent DXGI from releasing the HWND association.  Must match the
            // cleanup sequence in DX12_OnSwapchainResizeBegin which successfully
            // avoids E_ACCESSDENIED before ResizeBuffers.
            {
                std::lock_guard<std::recursive_mutex> overlayLock(g_OverlayMutex);
                g_LastSwapChain = nullptr;
                CleanupOverlay();
                CleanupRTVs();
                g_State.overlayInit = false;
            }
            // The retained Streamline startup-activation swapchain is an
            // AddRef'd swapchain reference; while CE pins it, DXGI refuses a
            // new swapchain on the same HWND (session 20260613_032326: the
            // app's native recreate after DLSS->OFF failed E_ACCESSDENIED
            // through all retries and stopped its main loop).
            ReleaseStreamlineStartupActivationSwapchain("DeepHook: CreateSwapChainForHwnd E_ACCESSDENIED recovery");
            HookLogImportant("DeepHook: Released overlay + RTV refs for HWND=%p", hWnd);

            // Clear our tracking entries (raw pointers, no Release needed)
            {
                std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
                s_hwndSwapchainMap.erase(hWnd);
            }
            if (ppSC && *ppSC) {
                ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
            }

            // Retry: 10 attempts × 20ms = 200ms max.  FSR FG activation may
            // need time for the game to release its own swapchain refs after
            // we've released ours.
            for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant("DeepHook: Retry attempt %d succeeded hr=0x%08X sc=%p", attempt, hr,
                                     (ppSC ? *ppSC : nullptr));
                    break;
                }
            }
            if (FAILED(hr)) {
                HookLogImportant("DeepHook: All retries exhausted — returning E_ACCESSDENIED to caller (HWND=%p)",
                                 hWnd);
            }
        }
    }

    // Post-track: record the new swapchain for future reactive recovery
    if (SUCCEEDED(hr) && ppSC && *ppSC && hWnd) {
        if (callerFromThirdPartyOverlay) {
            MarkThirdPartyOverlaySwapchain(*ppSC, callerModulePath);
            HookLogImportant(
                "DeepHook: Third-party overlay caller %s created swapchain %p for HWND=%p — leaving CE queue and "
                "transition state unchanged",
                callerModulePath[0] ? callerModulePath : "unknown", *ppSC, hWnd);
            return hr;
        }
        IDXGISwapChain* newSC = static_cast<IDXGISwapChain*>(*ppSC);
        if (ShouldBypassInvisibleWindowCreateSwapchainSideEffects(hWnd, newSC, "DeepHook", hr)) {
            return hr;
        }
        if (HandleProtectedOfficialFFXStartupSwapchainCreate(captureEvidence, pDevice, newSC, "DeepHook")) {
            return hr;
        }

        TrackSwapchainHwnd(*ppSC, hWnd);
        HookLogImportant("DeepHook: Created & tracked swapchain %p for HWND=%p", *ppSC, hWnd);

        // SL (or game) just created a new swapchain. Refresh the full Present
        // hook path on it — the new swapchain may expose a different Present
        // implementation or vtable than the one we initially hooked.
        RefreshPresentHooksForRealSwapchain(newSC, "DeepHook");

        CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSC, "DeepHook", captureEvidence);
    } else if (FAILED(hr)) {
        HookLogImportant("DeepHook: CreateSwapChainForHwnd FAILED hr=0x%08X hwnd=%p", hr, hWnd);
    }

    return hr;
}

// Inline hook detour for CreateSwapChainForHwnd.
// This code-level hook fires for ALL calls to the real DXGI function,
// including internal calls by Streamline's DLFG module (linkSwapchainToCmdQueue).
// When E_ACCESSDENIED occurs (HWND already has a flip-model swapchain), we
// only do CE-owned cleanup/retry for non-runtime-managed cases. Streamline and
// authoritative FFX takeover paths must manage that handoff themselves.
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndInline(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                    const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                    IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    if (HookIsShuttingDown()) {
        if (s_oCreateSCForHwndInline)
            return s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
        return E_FAIL;
    }

    // Skip side-effects for temp swapchains created during hook installation
    if (g_CreatingTempSwapchain.load(std::memory_order_acquire)) {
        HookLog("CreateSwapChainForHwnd INLINE: Temp swapchain — passthrough");
        return s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    }

    MarkForwardedCreateSwapchainForHwndInlineSideEffectsHandled();

    const CreateSwapchainForHwndCallerContext callerContext = ResolveCreateSwapchainForHwndCallerContext();
    const void* callerAddress = callerContext.callerAddress;
    const bool callerFromFFXFGModule = callerContext.callerFromFFXFGModule;
    const bool rawCallerFromThirdPartyOverlay = callerContext.callerFromThirdPartyOverlay;
    const char* callerModulePath = callerContext.callerModulePath;
    char ffxStackModulePath[MAX_PATH] = {};
    const bool ffxFrameGenerationInStack =
        ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModulePath, sizeof(ffxStackModulePath));
    const bool callerFromStreamlineFGModule =
        callerAddress && ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(callerAddress);
    const bool streamlineFrameGenerationInStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack();
    const bool callerFromThirdPartyOverlay = ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
        "CreateSwapChainForHwnd INLINE", rawCallerFromThirdPartyOverlay, callerFromFFXFGModule,
        ffxFrameGenerationInStack, callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath);
    const auto captureEvidence = BuildCreateSwapchainQueueCaptureEvidence(
        callerAddress, callerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
        callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath, ffxStackModulePath);

    HookLogImportant("CreateSwapChainForHwnd INLINE: factory=%p device=%p hwnd=%p", pThis, pDevice, hWnd);

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    const bool applyDescriptorOverrides = ShouldApplySwapchainDescriptorOverridesForCreate(captureEvidence);
    if (pDesc && !applyDescriptorOverrides) {
        LogSkippedSwapchainDescriptorOverridesForRuntimeCreate("CreateSwapChainForHwnd INLINE", captureEvidence,
                                                               pDesc->BufferCount, pDesc->Flags, pDesc->SwapEffect);
    }
    if (pDesc && applyDescriptorOverrides) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                HookLogImportant("INLINE: Skipping BufferCount override %u < game's %u (flip model)", requested,
                                 modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
                HookLogImportant("INLINE: Overriding BufferCount %u -> %u", modifiedDesc.BufferCount, requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    PrepareForAuthoritativeFFXSwapchainCreate(captureEvidence, "CreateSwapChainForHwnd INLINE");

    ID3D12CommandQueue* deferredStreamlineHandoffQueue = nullptr;
    const bool deferPresentHookRefreshForStreamlineHandoff =
        ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(pDevice, captureEvidence,
                                                                        &deferredStreamlineHandoffQueue);
    auto deferredStreamlineHandoffQueueRelease = ce::make_scope_guard([&]() {
        if (deferredStreamlineHandoffQueue) {
            deferredStreamlineHandoffQueue->Release();
            deferredStreamlineHandoffQueue = nullptr;
        }
    });
    if (deferPresentHookRefreshForStreamlineHandoff) {
        DXGIShared::ReleaseSwapchainPresentVTableHooksForRuntimeHandoff("post-FSR Streamline runtime swapchain create");
        HookLogImportant(
            "CreateSwapChainForHwnd INLINE: Released CE Present vtable ownership before post-FSR Streamline runtime "
            "handoff (queue=%p origGame=%p runtime=%s fsrApi=%d hadFSR=%d streamlineLoaded=%d)",
            deferredStreamlineHandoffQueue, g_OriginalGameQueue,
            ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), g_FGCompat.IsFSRFGApiActive() ? 1 : 0,
            g_HadFSRFGPhase ? 1 : 0, IsStreamlineLoaded() ? 1 : 0);
        // MAKE-BEFORE-BREAK for the runtime's replacement swapchain: the retained startup-activation
        // swapchain is an AddRef'd COM reference to the OLD (pre-handoff) chain. Holding it across this
        // create pins the old chain's HWND association, so DXGI fails the runtime's replacement create
        // with E_ACCESSDENIED and the game crashes dereferencing the null swapchain (GTA FSR->DLSS apply,
        // session 20260702_092933). Its purpose — PostSL startup recovery on the OLD chain — is moot once
        // the runtime replaces the swapchain, so release it BEFORE forwarding the create.
        ReleaseStreamlineStartupActivationSwapchain(
            "CreateSwapChainForHwnd INLINE: pre post-FSR Streamline runtime swapchain create");
    }

    HRESULT hr = s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
    HookLogImportant("CreateSwapChainForHwnd INLINE: result hr=0x%08X sc=%p", hr, (ppSC && *ppSC) ? *ppSC : nullptr);

    if (hr == E_ACCESSDENIED && hWnd) {
        // When a frame-generation runtime is managing swapchain lifecycle,
        // don't interfere. Our CleanupOverlay() flushes the GPU (200ms
        // Signal+Wait) and destroys overlay resources, which disrupts the
        // runtime's internal handoff state machine.
        const bool streamlineModuleLoaded = IsStreamlineLoaded();
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
        const bool passThroughForRuntimeManagedFG =
            ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(
                streamlineModuleLoaded, streamlineFGRunning, streamlineStartupHandoffPending, callerFromFFXFGModule,
                ffxFrameGenerationInStack);
        const char* passThroughReason =
            callerFromThirdPartyOverlay
                ? "third-party overlay caller"
                : ((callerFromFFXFGModule || ffxFrameGenerationInStack) ? "authoritative FFX takeover"
                                                                        : "Streamline active");
        if (ce::dx12_overlay_policy::ChooseCreateSwapchainAccessDeniedRecovery(passThroughForRuntimeManagedFG,
                                                                               callerFromThirdPartyOverlay) ==
            ce::dx12_overlay_policy::CreateSwapchainAccessDeniedRecovery::kMinimalCEReleaseThenEscalate) {
            // A failed runtime-managed create is FATAL to the game — GTA dereferences the null swapchain
            // and crashes (session 20260702_092933) — so the old blind pass-through is not acceptable.
            // Recover with the MINIMAL CE-owned unpin first: drop the retained startup-activation
            // swapchain reference (an AddRef'd COM ref that pins the old chain's HWND association) and
            // retry, WITHOUT the full overlay teardown / GPU flush that could disturb the runtime's own
            // handoff state machine. Escalate to the full cleanup only if the HWND stays pinned — a
            // disturbed handoff beats a guaranteed crash.
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED for HWND=%p — %s, minimal CE unpin + retry "
                "(slFG=%d startupPending=%d callerFFX=%d stackFFX=%d retained=%d module=%s)",
                hWnd, passThroughReason, streamlineFGRunning ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                callerFromFFXFGModule ? 1 : 0, ffxFrameGenerationInStack ? 1 : 0,
