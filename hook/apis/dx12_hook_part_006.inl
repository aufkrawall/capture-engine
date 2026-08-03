        HookLogImportant(
            "%s: Protected official FFX startup staged runtime queue %p until enabled ffxConfigure "
            "(module=%s caller=%s)",
            context && context[0] ? context : "CreateSwapChain", queue,
            modulePath && modulePath[0] ? modulePath : "unknown",
            captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
    } else {
        static std::atomic<int> s_stageQueueFailLogCount{0};
        const int logCount = s_stageQueueFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "%s: Protected official FFX startup could not stage runtime queue "
                "(createDevice=%p queue=%p qiHr=0x%08X queueType=%d module=%s log=%d)",
                context && context[0] ? context : "CreateSwapChain", createDevice, queue, (unsigned)qiHr,
                queue ? static_cast<int>(queueDesc.Type) : -1, modulePath && modulePath[0] ? modulePath : "unknown",
                logCount + 1);
        }
    }

    if (queue) {
        queue->Release();
    }
}

static bool ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup() {
    return ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(
        g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire),
        HasResolvedOfficialFFXStartupPath());
}

static bool ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
    IUnknown* pDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, ID3D12CommandQueue** queueOut) {
    if (queueOut) {
        *queueOut = nullptr;
    }
    if (!pDevice) {
        return false;
    }

    ID3D12CommandQueue* pQueue = nullptr;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))) || !pQueue) {
        return false;
    }

    ID3D12CommandQueue* originalGameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        originalGameQueue = g_OriginalGameQueue;
    }
    const bool streamlineRuntimeAvailable = IsStreamlineLoaded() || g_FGCompat.HasStreamlineSupport() ||
                                            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ||
                                            captureEvidence.authoritativeStreamlineRuntimeCreator;
    const bool deferRefresh = ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
        originalGameQueue != nullptr, pQueue == originalGameQueue, streamlineRuntimeAvailable, g_HadFSRFGPhase,
        g_FGCompat.IsFSRFGApiActive(), g_FGCompat.GetRuntimeMode());
    if (deferRefresh && queueOut) {
        *queueOut = pQueue;
    } else {
        pQueue->Release();
    }
    return deferRefresh;
}

static bool ShouldApplySwapchainDescriptorOverridesForCreate(
    const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
    return ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(
        captureEvidence.callerFromThirdPartyOverlay,
        captureEvidence.authoritativeFFXRuntimeCreator || captureEvidence.authoritativeStreamlineRuntimeCreator);
}

static void PrepareForAuthoritativeFFXSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence,
                                                      const char* context) {
    if (!ce::dx12_overlay_policy::ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(
            captureEvidence.authoritativeFFXRuntimeCreator, HasRetainedStreamlineStartupActivationSwapchain())) {
        return;
    }

    HookLogImportant(
        "%s: Authoritative FFX swapchain create is replacing a Streamline startup handoff — releasing retained "
        "Streamline activation swapchain before DXGI CreateSwapChainForHwnd to avoid stale HWND references "
        "(ffxModule=%s caller=%s)",
        context && context[0] ? context : "CreateSwapChainForHwnd",
        captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : "unknown",
        captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
    ReleaseStreamlineStartupActivationSwapchain("DX12: authoritative FFX swapchain create");
}

static void LogSkippedSwapchainDescriptorOverridesForRuntimeCreate(
    const char* context, const CreateSwapchainQueueCaptureEvidence& captureEvidence, UINT bufferCount, UINT flags,
    DXGI_SWAP_EFFECT swapEffect) {
    if (!captureEvidence.authoritativeFFXRuntimeCreator && !captureEvidence.authoritativeStreamlineRuntimeCreator) {
        return;
    }

    static std::atomic<int> s_runtimeDescriptorPassthroughLogCount{0};
    const int logCount = s_runtimeDescriptorPassthroughLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 128) == 0) {
        HookLogImportant(
            "%s: Preserving swapchain descriptor for authoritative FG runtime create "
            "(ffx=%d officialFFX=%d streamline=%d caller=%s BufferCount=%u Flags=0x%X SwapEffect=%d count=%d)",
            context && context[0] ? context : "CreateSwapChain", captureEvidence.authoritativeFFXRuntimeCreator ? 1 : 0,
            captureEvidence.officialAMDFFXRuntimeCreator ? 1 : 0,
            captureEvidence.authoritativeStreamlineRuntimeCreator ? 1 : 0,
            captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack", bufferCount, flags,
            static_cast<int>(swapEffect), logCount + 1);
    }
}

static bool ShouldBypassInvisibleWindowCreateSwapchainSideEffects(HWND hWnd, IDXGISwapChain* swapchain,
                                                                  const char* context, HRESULT hr) {
    if (FAILED(hr) || !swapchain || !hWnd) {
        return false;
    }

    const bool outputWindowVisible = IsWindowVisible(hWnd) != FALSE;
    if (!ce::dx12_overlay_policy::ShouldSkipDX12CreateSwapchainSideEffectsForInvisibleWindowSwapchain(
            true, outputWindowVisible)) {
        return false;
    }

    static std::atomic<int> s_invisibleWindowCreateSkipLogCount{0};
    const int logCount = s_invisibleWindowCreateSkipLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 128) == 0) {
        HookLogImportant(
            "%s: Invisible-window swapchain %p for HWND=%p — bypassing CE swapchain side-effects "
            "(queue capture, Present refresh, cooldown, wrapper decisions skipped; hr=0x%08X count=%d)",
            context && context[0] ? context : "CreateSwapChainForHwnd", swapchain, hWnd, hr, logCount + 1);
    }
    return true;
}

static void QuiesceStreamlinePostSLForProtectedOfficialFFXStartup(
    IDXGISwapChain* swapchain, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
    const bool callbackInstalled = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    const bool postSLActive = g_PostSLOverlayActive.load(std::memory_order_acquire);
    const bool postSLConfirmed = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
            true, HasResolvedOfficialFFXStartupPath(), callbackInstalled, postSLActive, postSLConfirmed,
            streamlineFGRunning, startupActivationPending)) {
        return;
    }

    const char* source = context && context[0] ? context : "protected official FFX startup";
    SetPostSLCallbackInstalled(false, "DX12: protected official FFX startup");
    const bool staleStreamlineSignal = DXGIShared::g_StreamlineFGRunning.exchange(false, std::memory_order_acq_rel);
    g_FGCompat.SetStreamlineFGSignal(false);
    g_FGCompat.SetDLSSFGActive(false);
    g_PostSLOverlayActive.store(false, std::memory_order_release);
    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    g_PostSLStallCounter.store(0, std::memory_order_release);
    g_PostSLStableFrameCount.store(0, std::memory_order_release);
    g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    ResetPostSLLifecycleForTransition("DX12: protected official FFX startup", true, true);
    ReleaseStreamlineStartupActivationSwapchain("DX12: protected official FFX startup");
    StreamlineHook::OnAuthoritativeFFXTakeover();
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    DXGIShared::ResetStreamlineStartupTransitionState();
    DXGIShared::DisableSLPresentRouting();

    HookLogImportant(
        "%s: Protected official FFX startup immediately quiesced Streamline/PostSL before AMD swapchain takeover "
        "(sc=%p callback=%d active=%d confirmed=%d startupPending=%d staleSL=%d module=%s caller=%s)",
        source, swapchain, callbackInstalled ? 1 : 0, postSLActive ? 1 : 0, postSLConfirmed ? 1 : 0,
        startupActivationPending ? 1 : 0, staleStreamlineSignal ? 1 : 0,
        captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : "unknown",
        captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
}

static bool HandleProtectedOfficialFFXStartupSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence,
                                                             IUnknown* createDevice, IDXGISwapChain* swapchain,
                                                             const char* context) {
    if (!ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(captureEvidence)) {
        return false;
    }

    g_FGCompat.SetFSRFGSupportPresent(true);
    g_FGCompat.SetFSRFGMultiplier(2);
    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
    SetNativeFSRStartupConfigureArmingPending(true, "protected official FFX swapchain create");
    g_ProtectedOfficialFFXStartupSwapchainPending.store(true, std::memory_order_release);
    ArmProtectedOfficialFFXStartupProgressTracking("protected official FFX swapchain create");
    ResetAuthoritativeFSRRealFrameOnlyStreak();
    if (!g_HadFSRFGPhase) {
        g_HadFSRFGPhase = true;
        HookLogImportant(
            "DX12: Protected official FFX swapchain create implies FSR FG history — latching post-FSR handoff state");
    }

    StageProtectedOfficialFFXStartupQueueFromCreateDevice(createDevice, captureEvidence, context);
    QuiesceStreamlinePostSLForProtectedOfficialFFXStartup(swapchain, captureEvidence, context);

    HookLogImportant(
        "DX12: Protected official FFX startup swapchain pass-through via %s (sc=%p module=%s caller=%s) — "
        "deferring Present hook refresh, queue ownership, FFX export inspection, and heavy takeover side effects "
        "until enabled ffxConfigure; live Streamline/PostSL routing was quiesced immediately when present",
        context && context[0] ? context : "CreateSwapChain", swapchain,
        captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : "unknown",
        captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
    return true;
}

static void ApplyAuthoritativeFFXTakeoverSideEffects(ID3D12CommandQueue* capturedQueue, const char* callerModulePath,
                                                     const char* reason) {
    bool stagedQueueApplied = false;
    bool stagedQueueActivatedOwnership = false;
    ID3D12CommandQueue* liveSwapchainQueueAfterApply = nullptr;
    bool fgRuntimeOwnsAfterApply = false;
    if (capturedQueue) {
        stagedQueueActivatedOwnership = DX12_SetSwapchainQueue(capturedQueue, false, true);
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            liveSwapchainQueueAfterApply = g_SwapchainQueue;
            fgRuntimeOwnsAfterApply = g_FGRuntimeOwnsSwapchain;
        }
        stagedQueueApplied = liveSwapchainQueueAfterApply == capturedQueue;
        HookLogImportant(
            "DX12: FFX swapchain takeover applied staged runtime queue "
            "(captured=%p liveScQueue=%p applied=%d ownershipActivated=%d fgOwned=%d reason=%s)",
            capturedQueue, liveSwapchainQueueAfterApply, stagedQueueApplied ? 1 : 0,
            stagedQueueActivatedOwnership ? 1 : 0, fgRuntimeOwnsAfterApply ? 1 : 0,
            reason && reason[0] ? reason : "unknown");
    }

    const bool staleStreamlineSignal = DXGIShared::g_StreamlineFGRunning.exchange(false, std::memory_order_acq_rel);
    g_FGCompat.SetStreamlineFGSignal(false);
    g_FGCompat.SetDLSSFGActive(false);
    g_PostSLOverlayActive.store(false, std::memory_order_release);
    SetPostSLCallbackInstalled(false, "DX12: FFX swapchain takeover");
    ResetPostSLLifecycleForTransition("DX12: FFX swapchain takeover", true, true);
    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    g_PostSLStallCounter.store(0, std::memory_order_release);
    g_PostSLStableFrameCount.store(0, std::memory_order_release);
    g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: FFX swapchain takeover");
    ResetFFXPresentCallbackOverlayBackend("DX12: FFX swapchain takeover");
    StreamlineHook::OnAuthoritativeFFXTakeover();
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    DXGIShared::ResetStreamlineStartupTransitionState();
    HookLogImportant("DX12: FFX swapchain takeover — cleared stale Streamline startup handoff/transition state");
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeFFXTakeover,
                                "DX12::AuthoritativeFFXTakeover", capturedQueue, nullptr,
                                ce::fg_runtime::RuntimeMode::kFSRFG, true, true);
    DXGIShared::DisableSLPresentRouting();
    {
        ID3D12CommandQueue* oldWrapper = g_SLWrapperQueue.exchange(nullptr, std::memory_order_acq_rel);
        if (oldWrapper) {
            HookLogImportant("DX12: FFX swapchain takeover — released stale SL wrapper queue %p", oldWrapper);
            oldWrapper->Release();
        }
    }

    HookLogImportant(
        "DX12: FFX swapchain takeover via %s "
        "(queue=%p stagedQueueApplied=%d liveScQueue=%p staleSL=%d reason=%s) — cleared Streamline/PostSL ownership",
        callerModulePath && callerModulePath[0] ? callerModulePath : "unknown", capturedQueue,
        stagedQueueApplied ? 1 : 0, liveSwapchainQueueAfterApply, staleStreamlineSignal ? 1 : 0,
        reason && reason[0] ? reason : "unknown");

    if (!g_FGCompat.HasDirectFFXApiConfirmation()) {
        HookLogImportant(
            "DX12: Authoritative FFX takeover has no direct ffxConfigure confirmation yet; keeping FFX hooks armed "
            "and waiting for a real runtime configure instead of issuing a synthetic partial ffxConfigure");
        FFXHook::Init();
    }
}

static bool MaybeFinalizeProtectedOfficialFFXStartupAfterSustainedProgress(const char* source) {
    if (!g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire) ||
        HasResolvedOfficialFFXStartupPath()) {
        return false;
    }

    const uint32_t processFrameSkips = g_ProtectedOfficialFFXStartupProcessFrameSkips.load(std::memory_order_acquire);
    const uint32_t eclPassThroughs = g_ProtectedOfficialFFXStartupECLPassThroughs.load(std::memory_order_acquire);
    if (processFrameSkips < ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupProcessFrameProgressThreshold() &&
        eclPassThroughs < ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupECLProgressThreshold()) {
        return false;
    }

    if (ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
            true, false, processFrameSkips, eclPassThroughs)) {
        // The policy currently forbids progress-only finalization. Keep this
        // branch as a guardrail if that policy is ever revisited.
        return false;
    }

    static std::atomic<int> s_protectedOfficialFFXProgressOnlyLogCount{0};
    const int logCount = s_protectedOfficialFFXProgressOnlyLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 600) == 0) {
        const ULONGLONG nowMs = GetTickCount64();
        const ULONGLONG beginMs = g_ProtectedOfficialFFXStartupBeginMs.load(std::memory_order_acquire);
        HookLogImportant(
            "DX12: Protected official FFX startup has sustained frame progress but remains quiesced until direct "
            "ffxConfigure/present-callback proof (source=%s elapsed=%llums processFrameSkips=%u eclPassThroughs=%u "
            "log=%d)",
            source && source[0] ? source : "unknown", beginMs ? static_cast<unsigned long long>(nowMs - beginMs) : 0ULL,
            processFrameSkips, eclPassThroughs, logCount + 1);
    }
    return false;
}

static void ClearStaleStreamlineOwnershipForFSRTakeover(const CreateSwapchainQueueCaptureEvidence& captureEvidence,
                                                        bool runtimeOwnsSwapchain, bool runtimeOwnershipJustActivated,
                                                        ID3D12CommandQueue* capturedQueue) {
    char callerModulePath[MAX_PATH] = {};
    if (captureEvidence.callerModulePath[0]) {
        strncpy_s(callerModulePath, sizeof(callerModulePath), captureEvidence.callerModulePath, _TRUNCATE);
    }

    const bool callerFromFFXFGModule = captureEvidence.authoritativeFFXRuntimeCreator;
    if (callerFromFFXFGModule &&
        (!callerModulePath[0] || ce::overlay_compat::IsThirdPartyOverlayModulePath(callerModulePath))) {
        strncpy_s(callerModulePath, sizeof(callerModulePath), "FFX frame-generation runtime", _TRUNCATE);
    }
    char ffxModulePath[MAX_PATH] = {};
    if (captureEvidence.ffxModulePath[0]) {
        strncpy_s(ffxModulePath, sizeof(ffxModulePath), captureEvidence.ffxModulePath, _TRUNCATE);
    }
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
    const bool staleStreamlineOwnershipCandidate = runtimeOwnsSwapchain && streamlineFGRunning &&
                                                   !streamlineStartupHandoffPending && runtimeOwnershipJustActivated;
    if (!ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(
            runtimeOwnsSwapchain, callerFromFFXFGModule, streamlineFGRunning, streamlineStartupHandoffPending,
            runtimeOwnershipJustActivated)) {
        if (staleStreamlineOwnershipCandidate && !callerFromFFXFGModule) {
            static std::atomic<int> s_nonFfxTakeoverPreserveLogCount{0};
            const int logCount = s_nonFfxTakeoverPreserveLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10) {
                HookLogImportant(
                    "DX12: Runtime-owned swapchain transition on %p while Streamline FG is active had no FFX FG "
                    "module in caller stack (caller=%s) — preserving existing Streamline/PostSL ownership",
                    capturedQueue, callerModulePath[0] ? callerModulePath : "unknown");
            }
        }
        return;
    }

    if (!callerModulePath[0] && runtimeOwnershipJustActivated) {
        strncpy_s(callerModulePath, sizeof(callerModulePath), "runtime-owned swapchain transition", _TRUNCATE);
    }

    if (ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(captureEvidence)) {
        g_FGCompat.SetFSRFGSupportPresent(true);
        g_FGCompat.SetFSRFGMultiplier(2);
        ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
        SetNativeFSRStartupConfigureArmingPending(true, "protected official FFX queue capture");
        g_ProtectedOfficialFFXStartupSwapchainPending.store(true, std::memory_order_release);
        ArmProtectedOfficialFFXStartupProgressTracking("protected official FFX queue capture");
        ResetAuthoritativeFSRRealFrameOnlyStreak();
        if (!g_HadFSRFGPhase) {
            g_HadFSRFGPhase = true;
            HookLogImportant(
                "DX12: Protected official FFX queue capture implies FSR FG history — latching post-FSR handoff state");
        }
        StoreDeferredOfficialFFXTakeoverSideEffects(capturedQueue, ffxModulePath[0] ? ffxModulePath : callerModulePath,
                                                    "protected official FFX queue capture");
        HookLogImportant(
            "DX12: Official FFX queue capture is protected until enabled ffxConfigure (queue=%p runtimeOwned=%d "
            "ffxModule=%s) — skipping FFX export inspection and Streamline/PostSL teardown",
            capturedQueue, runtimeOwnsSwapchain ? 1 : 0, ffxModulePath[0] ? ffxModulePath : "unknown");
        return;
    }

    // Native FFX can be unloaded and reloaded across repeated FG runs. Refresh
    // the FFX API hooks immediately on authoritative takeover so the next
    // configure call can re-arm the present-callback bridge on the live module
    // instead of waiting for the background hook scan.
    FFXHook::Init();

    g_FGCompat.SetFSRFGSupportPresent(true);
    g_FGCompat.SetFSRFGMultiplier(2);
    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
    SetNativeFSRStartupConfigureArmingPending(true, "authoritative FFX swapchain takeover");
    ResetAuthoritativeFSRRealFrameOnlyStreak();
    if (!g_HadFSRFGPhase) {
        g_HadFSRFGPhase = true;
        HookLogImportant("DX12: FFX swapchain takeover implies FSR FG history — latching post-FSR handoff state");
    }

    if (ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(
            runtimeOwnsSwapchain, callerFromFFXFGModule, captureEvidence.officialAMDFFXRuntimeCreator,
            g_FGCompat.HasDirectFFXApiConfirmation())) {
        StoreDeferredOfficialFFXTakeoverSideEffects(capturedQueue, ffxModulePath[0] ? ffxModulePath : callerModulePath,
                                                    "authoritative official FFX swapchain takeover");
        HookLogImportant(
            "DX12: Official FFX takeover is in startup-arming mode; Streamline/PostSL teardown and SL route disable "
            "are deferred until enabled ffxConfigure (queue=%p runtimeOwned=%d ffxModule=%s)",
            capturedQueue, runtimeOwnsSwapchain ? 1 : 0, ffxModulePath[0] ? ffxModulePath : "unknown");
        return;
    }

    g_FGCompat.SetFSRFGActive(true);
    ApplyAuthoritativeFFXTakeoverSideEffects(capturedQueue, callerModulePath, "authoritative FFX swapchain takeover");
}

// LOCK HIERARCHY (MUST be acquired in this order to prevent deadlocks):
// 1. g_OverlayMutex (outermost - protects overlay state)
// 2. g_CommandQueueMutex (protects command queue pointer)
// 3. g_DX12CaptureMutex (innermost - protects capture state)
//
// Rule: When acquiring multiple locks, always acquire in order above.
//       Use std::lock_guard with std::adopt_lock when using try_lock().
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
static std::recursive_mutex g_OverlayMutex;

static bool PrewarmPostSLOverlayForFreshStreamlineHandoff(IDXGISwapChain* swapChain, ID3D12CommandQueue* swapchainQueue,
                                                          const char* context);
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
static std::recursive_mutex g_DX12CaptureMutex;

static OverlayConfig GetActiveDX12OverlayConfig(SharedMemoryLayout* shm) {
    OverlayConfig cfg{};
    cfg.captureIncludeOverlay = true;
    cfg.screenshotIncludeOverlay = true;
    if (shm) {
        cfg = shm->ReadOverlayConfig();
    }
    return cfg;
}

static bool IsDX12ObserverOnlyModeActive(SharedMemoryLayout* shm) {
    return IsOverlayObserverOnly(GetActiveDX12OverlayConfig(shm));
}

static bool IsDX12ObserverPolicyOnlyModeActive(SharedMemoryLayout* shm) {
    return IsOverlayObserverPolicyOnly(GetActiveDX12OverlayConfig(shm));
}

static bool IsDX12ObserverStartupPresentOnlyModeActive(SharedMemoryLayout* shm) {
    return IsOverlayObserverStartupPresentOnly(GetActiveDX12OverlayConfig(shm));
}

static void EnsurePostSLDisabledForObserverOnly(const char* reason, bool preserveStartupTransitionWindow = false) {
    g_PostSLOverlayActive.store(false, std::memory_order_release);
    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);
    g_PostSLStallCounter.store(0, std::memory_order_release);
    g_PostSLStableFrameCount.store(0, std::memory_order_release);
    g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain(reason);
    if (!preserveStartupTransitionWindow) {
        DXGIShared::ResetStreamlineStartupTransitionState();
    }
    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
        SetPostSLCallbackInstalled(false, reason);
    }
}

static bool ShouldUseConfirmedPostSLForOverlayIncludedWork(const OverlayConfig& cfg) {
    return cfg.showOverlay && g_PostSLOverlayActive.load(std::memory_order_acquire) &&
           g_PostSLConfirmedRendering.load(std::memory_order_acquire);
}

static void CaptureRequestedDX12Screenshot(IDXGISwapChain3* sc3, SharedMemoryLayout* shm, uint64_t requestId,
                                           ID3D12CommandQueue* queueOverride = nullptr) {
    if (!sc3 || !shm || requestId == 0)
        return;

    bool queued = false;
    ID3D12Device* dx12Device = g_Device.load();
    ID3D12CommandQueue* dx12Queue = queueOverride ? queueOverride : g_CommandQueue.load();
    if (dx12Device && dx12Queue) {
        UINT bbIdx = sc3->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = nullptr;
        if (SUCCEEDED(sc3->GetBuffer(bbIdx, IID_PPV_ARGS(&backBuffer)))) {
            const D3D12_RESOURCE_DESC resourceDesc = backBuffer->GetDesc();
            const auto presentationEncoding = DXGIShared::ResolveSwapChainPresentationEncoding(
                static_cast<IDXGISwapChain*>(sc3), resourceDesc.Format);
            queued = SaveDX12TextureAsScreenshotRaw(dx12Device, dx12Queue, backBuffer, shm, requestId,
                                                    presentationEncoding);
            backBuffer->Release();
        }
    }
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
}

static void PublishDX12CapturedFrame(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm,
                                     ID3D12CommandQueue* captureQueue, bool hasCurrentBackBufferIdx,
                                     UINT currentBackBufferIdx) {
    if (!pSwapChain || !shm || !captureQueue)
        return;
    if (shm->throttleCapture.load(std::memory_order_acquire))
        return;

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    auto presentationEncoding = ce::presentation_color::Encoding::Unsupported;
    if (SUCCEEDED(pSwapChain->GetDesc(&swapChainDesc))) {
        presentationEncoding =
            DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, swapChainDesc.BufferDesc.Format);
    }
    shm->SetIsHDR(ce::presentation_color::IsHDR(presentationEncoding));

    std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
    ID3D12Device* captureDevice = g_Device.load(std::memory_order_acquire);
    if (!g_SharedCaptureD3D12.IsInitializedFor(captureDevice, pSwapChain)) {
        if (!g_SharedCaptureD3D12.Initialize(captureDevice, pSwapChain)) {
            return;
        }
        HookLogImportant("DX12: Shared capture initialized for swapchain generation sc=%p device=%p", pSwapChain,
                         captureDevice);
    }

    UINT bbIdx = 0;
    if (hasCurrentBackBufferIdx) {
        bbIdx = currentBackBufferIdx;
    } else {
        IDXGISwapChain3* sc3 = nullptr;
        pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
        bbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
        if (sc3)
            sc3->Release();
    }

    if (!g_SharedCaptureD3D12.CaptureFrame(captureQueue, bbIdx))
        return;

    SharedFrameDescriptor desc;
    if (!g_SharedCaptureD3D12.GetCurrentFrame(&desc))
        return;

    for (UINT i = 0; i < SharedCaptureD3D12::kSharedTextureCount; ++i) {
        shm->SetSharedHandle(static_cast<int>(i), (uint64_t)g_SharedCaptureD3D12.GetSharedHandle((int)i));
    }
    shm->SetFenceShareHandle((uint64_t)g_SharedCaptureD3D12.GetFenceShareHandle());
    shm->SetWidth(desc.width);
    shm->SetHeight(desc.height);
    shm->SetFormat(desc.format);

    uint32_t wIdx = shm->frameRing.writeIndex.load(std::memory_order_acquire);
    uint32_t rIdx = shm->frameRing.readIndex.load(std::memory_order_acquire);
    if ((uint32_t)(wIdx - rIdx) < (uint32_t)FRAME_RING_SIZE) {
        const bool ringWasEmpty = wIdx == shm->frameRing.ingestIndex.load(std::memory_order_acquire);
        FrameSlot& slot = shm->frameRing.slots[wIdx % FRAME_RING_SIZE];
        slot.fenceValue = desc.fenceValue;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        slot.timestamp = desc.presentTime;
        slot.frameIndex = desc.frameNumber;
        slot.textureIndex = desc.textureIndex;
        slot.sourcePid = GetCurrentProcessId();
        std::atomic_thread_fence(std::memory_order_release);
        slot.valid.store(1, std::memory_order_release);
        shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
        if (ringWasEmpty && g_IPC) {
            g_IPC->SignalInjectFrameReady();
        }
        DXGIShared::SetLatestSourceFrameIndex(desc.frameNumber);
        static uint64_t s_lastPublishLineageLogTick = 0;
        uint64_t nowTick = GetTickCount64();
        if (nowTick - s_lastPublishLineageLogTick >= 1000) {
            HookLog("DX12: Publish frame=%u ring=%u tex=%d fence=%llu ts=%llu bb=%u depth=%u", desc.frameNumber, wIdx,
                    desc.textureIndex, static_cast<unsigned long long>(desc.fenceValue),
                    static_cast<unsigned long long>(desc.presentTime), bbIdx, static_cast<unsigned>(wIdx - rIdx));
            s_lastPublishLineageLogTick = nowTick;
        }
    } else {
        shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
        shm->runtimeState.injectProducerMetadataFullDrops.fetch_add(1, std::memory_order_relaxed);
    }
}

static std::atomic<bool> g_InSwapchainResizeCleanup{false};
static std::atomic<bool> g_PreserveOverlayAdapterAcrossResize{false};
static std::atomic<ID3D12Device*> g_OverlayAdapterBackendDevice{nullptr};
static std::atomic<ID3D12CommandQueue*> g_OverlayAdapterBackendQueue{nullptr};
static std::atomic<int> g_OverlayAdapterBackendFormat{static_cast<int>(DXGI_FORMAT_UNKNOWN)};

// Frame counter for post-ImGui-init delay (skip first frame to let GPU
// stabilize)
static std::atomic<int> s_framesSinceInit{0};
static std::atomic<int> s_framesBeforeInit{0};

// Use pointer to prevent static destructor execution in non-game processes
// (Explorer fix)
DX12Hook* g_dx12HookInstance = nullptr;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_DeviceQueuesMutex;
std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

// CPU Prerender Limit State (DX12)
static std::vector<ID3D12Fence*> g_PrerenderFences;
static std::vector<HANDLE> g_PrerenderEvents;
static uint64_t g_PrerenderFrameIndex = 0;
static std::mutex g_PrerenderMutex;
static ID3D12Device* g_PrerenderDevice = nullptr;
static ID3D12CommandQueue* g_PrerenderQueue = nullptr;

// ECL piggyback overlay: for games (like GTA5 Enhanced) that reject separate ECL
// submissions touching backbuffers, render the overlay by appending our command
// list to the game's own ExecuteCommandLists call.
static std::atomic<bool> g_PiggybackOverlayActive{false};
static std::atomic<bool> g_PiggybackDrawnThisFrame{false};
static ID3D12DescriptorHeap* g_FFXPresentRtvHeap = nullptr;

struct FFXPresentCallbackBridgeState {
    ce::ffx_api::PresentCallback originalCallback = nullptr;
    void* originalUserContext = nullptr;
    bool installed = false;
};

static std::mutex g_FFXPresentCallbackBridgeMutex;
static std::unordered_map<void*, FFXPresentCallbackBridgeState> g_FFXPresentCallbackBridges;

// Deferred Signal: avoid the NVIDIA driver stall caused by Signal between our
// overlay ECL and Present.  Instead of calling Signal immediately after our ECL,
// wrapped Present paths flush it immediately after the real Present returns.
// This keeps the ECL->Present path clean while still giving focus-loss handling
// an authoritative fence for the overlay work that just touched the backbuffer.
static std::atomic<UINT64> g_deferredSignalValue{0};
static std::atomic<int> g_deferredSignalAllocIdx{-1};
// Track which queue the deferred ECL was submitted on, so the deferred Signal
// goes to the same queue.  When FG runtimes create swapchains with their own
// queue, this may differ from g_CommandQueue.
static std::atomic<ID3D12CommandQueue*> g_deferredSignalQueue{nullptr};
static std::atomic<UINT64> g_FocusLossPendingOverlayFenceValue{0};
static std::atomic<bool> g_FocusLossImmediateFenceDumpRequested{false};
static std::atomic<bool> g_FocusLossDeviceRemovalDumpRequested{false};
static constexpr int kFocusLossForegroundReacquirePresentProofFrames = 16;
static constexpr int kFocusLossRecentTransitionDumpWindowFrames = 300;
static std::atomic<int> g_FocusLossForegroundReacquirePresentProofRemaining{0};
static std::atomic<int> g_FocusLossRecentTransitionPresentWindow{0};

// Swapchain visibility, tracked from the wrapped Present HRESULT.
// DXGI_STATUS_OCCLUDED means the window is fully covered/minimized and the
// present is a no-op; the overlay is not visible to the user in that state. A
// merely-unfocused window that is STILL VISIBLE (e.g. a borderless background
// window or a window on another monitor) keeps presenting S_OK, so the overlay
// must keep rendering. CE holds backbuffer GPU work only when the swapchain is
// not presentable, never merely because focus moved elsewhere.
#ifndef DXGI_STATUS_OCCLUDED
#define DXGI_STATUS_OCCLUDED ((HRESULT)0x087A0001L)
#endif
static std::atomic<bool> g_SwapchainPresentOccluded{false};

// True once a real Present result has been observed via DX12_NoteWrappedD3D12PresentResult
// from ANY present path (the CWrapDXGISwapChain wrapper OR the vtable DetourPresent path).
// The not-presentable backbuffer-work hold needs a trustworthy present-result-derived
