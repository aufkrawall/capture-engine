                "DX12: Ignoring wrapper queue notify for non-classification queue "
                "%p (class=%p, primary=%p, orig=%p, num=%u)",
                pQueue, classificationQueue, g_PrimaryGameQueue.load(std::memory_order_relaxed), g_OriginalGameQueue,
                numCommandLists);
        }
        return;
    }

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_CommandListsExecutedThisFrame.fetch_add(numCommandLists, std::memory_order_relaxed);
}

// Legacy queue-less wrapper notify. Ignore it so stale helper traffic cannot
// mark auxiliary command queue work as a real frame.
void DX12_NotifyCommandLists(UINT numCommandLists) {
    static std::atomic<int> s_legacyNotifyLogCount{0};
    int logCount = s_legacyNotifyLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 5) {
        HookLog(
            "DX12: Ignoring legacy queue-less DX12_NotifyCommandLists(%u) to avoid "
            "false real-frame classification",
            numCommandLists);
    }
}

void DX12_OnSwapchainResizeEnd();
void CleanupOverlay(bool preserveNativeFSRPresentCallbackBackend);
void CleanupRTVs();
void DX12_InvalidateSwapchain();

// Helper to ensure global hook instance exists
void EnsureDX12Hook() {
    if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
    }
}

// Forward declarations
static void InstallGlobalVTableHooks();
static void HookSwapchainVTableViaTempSwapchain(bool presentOnly = false);
static void FindAndWrapPreExistingSwapchains();

void DX12Hook::Init() {
    EnsureDX12Hook();  // Self-init check
    static std::recursive_mutex s_InitMutex;
    static bool s_InitDone = false;
    std::lock_guard<std::recursive_mutex> lock(s_InitMutex);
    if (s_InitDone)
        return;
    s_InitDone = true;

    ce::fg_session::RegisterDX12LegacyStateProvider(&FillFGSessionLegacyStateView);
    DXGIShared::g_PostSLStartupActivationService.store(&DX12_TryInvokePostSLStartupActivationCallbackFromSharedService,
                                                       std::memory_order_release);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPresentObserved, "DX12Hook::Init");

    // CRITICAL FIX: Check if Vulkan is active before installing ANY DXGI hooks
    // Vulkan games using WSI-to-DXGI mapping can freeze if we hook DXGI
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (hVulkan) {
        HookLog(
            "DX12: Vulkan detected (vulkan-1.dll), SKIPPING ALL DXGI hook "
            "installation");
        return;
    }

    // Arm DRED auto-breadcrumbs + page-fault as early as possible. DRED is a
    // process-global setting that only affects devices created AFTER this call,
    // so it must run before the game creates its D3D12 device. The dedicated
    // Wrapped_D3D12CreateDevice arming point only exists when ENABLE_D3D12_WRAPPER
    // is built (it requires d3d12_wrappers.dll, which is absent in normal builds),
    // so for the common inject path this DX12Hook::Init() call — which runs on a
    // worker thread well before the game's device is created — is the real arming
    // site. Without it a device-hung yields no breadcrumbs (GetAutoBreadcrumbsOutput
    // fails because auto-breadcrumbs were never enabled for the game's device).
    ce::dx12_dred::ArmBeforeDeviceCreation();
    // Optional D3D12 debug layer (env CE_DX12_DEBUG_LAYER) — must be enabled before
    // the game's device is created. Off by default; used to capture the exact
    // resource-state/hazard behind the Alt+Tab overlay-draw hang.
    ce::dx12_dred::ArmDebugLayerBeforeDeviceCreation();

    // Note: Crash handler is installed in DllMain (hook/main.cpp)

    // Start freeze detection watchdog with dynamic timeout based on game engine
    // The watchdog auto-detects UE5, DLSS FG and uses extended timeouts
    double timeout = g_RenderWatchdog.GetRecommendedTimeout();
    g_RenderWatchdog.SetMonitoredThread(GetCurrentThreadId());
    g_RenderWatchdog.SetPreferredThreadProvider(&DX12_GetGamePresentThreadId);
    g_RenderWatchdog.Start(timeout);
    HookLog("DX12: Freeze watchdog started (%.0f second timeout)", timeout);

    // CRITICAL FIX: Install global swapchain vtable hooks by getting the vtable
    // directly from the DXGI module. This avoids creating a temp swapchain which
    // causes deadlocks with Steam overlay + Streamline.
    InstallGlobalVTableHooks();

#ifdef ENABLE_D3D12_WRAPPER
    // When D3D12 wrapper is enabled, Present inline hooks are deferred to
    // EnsurePresentHooks() (called from Wrapped_D3D12CreateDevice) to avoid
    // creating a temp D3D12 device in DX11-only apps that load d3d12.dll via
    // D3D11On12.
    HookLog("DX12Hook: Initialized (factory hooks installed; Present hooks deferred to D3D12CreateDevice)");
#else
    const bool d3d12DeviceCreated = WasD3D12DeviceCreated();
    const char* startupOverlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    // Without D3D12 wrapper, D3D12CreateDevice isn't hooked and the deferred
    // trigger never fires.  Install Present inline hooks now via a temp
    // swapchain so pre-existing swapchains (created before injection) are
    // covered.  The temp device/swapchain is destroyed immediately after
    // hooking, so DX11 state corruption is not a concern.
    if (ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(d3d12DeviceCreated,
                                                                                     startupOverlayModule != nullptr)) {
        HookLogImportant(
            "DX12Hook: Deferring eager temp-swapchain Present hook install because third-party overlay %s is already "
            "loaded before the first real D3D12 device",
            startupOverlayModule);
    } else {
        HookLog("DX12Hook: Installing Present hooks eagerly (no D3D12 wrapper)");
        HookSwapchainVTableViaTempSwapchain();
    }
    if (DXGIShared::HasPresentInlineHooks() || DXGIShared::HasPresentDetourHooks()) {
        HookLog("DX12Hook: Initialized (factory + Present hooks installed)");
    } else {
        HookLogImportant(
            "DX12Hook: Initialized (factory hooks installed; Present hooks deferred to "
            "FindAndWrapPreExistingSwapchains)");
    }
#endif

    FindAndWrapPreExistingSwapchains();
}

void DX12Hook::EnsurePresentHooks() {
    static std::atomic<bool> s_done{false};
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // Already installed
    }
    HookLog("DX12: Installing Present inline hooks (D3D12 device created by game)");
    HookSwapchainVTableViaTempSwapchain();
    HookLog("DX12: Present inline hooks installed");
}

static void FindAndWrapPreExistingSwapchains() {
    if (DXGIShared::HasPresentInlineHooks() || DXGIShared::HasPresentDetourHooks()) {
        HookLog("DX12: Pre-existing swapchain support via inline Present hooks already active");
        return;
    }

    // Present hooks were deferred during DX12Hook::Init() because a third-party
    // overlay (e.g. nvspcap64.dll) was loaded before the game's first real D3D12
    // device existed.  The eager temp-swapchain approach was skipped to avoid
    // recursing through the overlay's startup hook chain.
    //
    // If the game has already created its swapchain during the injection window,
    // the CreateSwapChainForHwnd detours will never fire, and Present hooks
    // would remain uninstalled forever — the overlay would never render.
    //
    // Try installing Present hooks now via a second temp swapchain.  The
    // g_CreatingTempSwapchain guard prevents our own CreateSwapChainForHwnd
    // hooks from processing the temp swapchain's queue, and calling
    // oCreateSwapChainForHwndGlobal bypasses our hooks entirely.  At this point
    // the overlay's startup hook chain should be settled, so the recursion risk
    // is minimal.
    HookLogImportant(
        "DX12: Present hooks not installed during init — installing via "
        "postponed temp swapchain for pre-existing swapchain coverage");
    HookSwapchainVTableViaTempSwapchain();

    if (DXGIShared::HasPresentInlineHooks() || DXGIShared::HasPresentDetourHooks()) {
        HookLogImportant("DX12: Present hooks installed via postponed temp swapchain");
    } else {
        HookLogImportant(
            "DX12: Postponed temp swapchain also failed — pre-existing "
            "swapchains will not have overlay until a real CreateSwapChainForHwnd "
            "call is intercepted");
    }
}

static void EnsurePresentInlineHooksForRealSwapchain(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain || DXGIShared::HasPresentDetourHooks()) {
        return;
    }

    static std::atomic<int> s_installAttemptCount{0};
    const int attempt = s_installAttemptCount.fetch_add(1, std::memory_order_relaxed) + 1;
    HookLog("DX12: Installing Present inline hooks via %s swapchain #%d (swapchain=%p)", source ? source : "real",
            attempt, pSwapChain);

    if (!DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
        HookLog("DX12: Present inline hook installation via %s swapchain failed", source ? source : "real");
        return;
    }

    if (DXGIShared::HasPresentInlineHooks()) {
        HookLogImportant("DX12: Present inline hooks are active via %s swapchain", source ? source : "real");
    } else if (DXGIShared::HasPresentDetourHooks()) {
        HookLogImportant("DX12: Present detour hooks are active via %s swapchain (external overlay-compatible path)",
                         source ? source : "real");
    } else {
        HookLog("DX12: Present inline hook installation via %s swapchain deferred to existing external hook chain",
                source ? source : "real");
    }
}

static void RefreshPresentHooksForRealSwapchain(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain) {
        return;
    }

    HookLogImportant("DX12: Refreshing Present hook path via %s swapchain %p", source ? source : "real", pSwapChain);
    {
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
                static std::atomic<int> s_backbufferLogCount{0};
                int idx = s_backbufferLogCount.fetch_add(1, std::memory_order_relaxed);
                if (idx < 24) {
                    HookLogImportant(
                        "DX12: Swapchain buffer count source=%s sc=%p actual=%u requested=%d "
                        "size=%ux%u swapEffect=%d (#%d)",
                        source ? source : "real", pSwapChain, desc.BufferCount, gfx.backbufferCount,
                        desc.BufferDesc.Width, desc.BufferDesc.Height, desc.SwapEffect, idx + 1);
                }
            }
        }
    }
    EnsurePresentInlineHooksForRealSwapchain(pSwapChain, source);
    DXGIShared::InstallHooks(pSwapChain, /*presentOnly=*/true);
    DXGIShared::RepairVTableHooksIfNeeded();
}

// Function pointers for global factory vtable hooks
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

static PFN_CreateSwapChain oCreateSwapChainGlobal = nullptr;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwndGlobal = nullptr;

// Inline hook trampoline for CreateSwapChainForHwnd (code-level hook in dxgi.dll)
// This catches ALL calls regardless of which factory vtable is used (including
// Streamline SL proxy factories that bypass our vtable hooks).
static PFN_CreateSwapChainForHwnd s_oCreateSCForHwndInline = nullptr;

// Address of the real CreateSwapChainForHwnd in dxgi.dll (for deep hook removal)
static void* s_realCreateSCForHwndAddr = nullptr;

// Deep hook trampoline for calling the real CreateSwapChainForHwnd
static PFN_CreateSwapChainForHwnd s_deepHookTrampoline = nullptr;

// Overlay suspension: cooldown after swapchain creation (FG switch, resize, etc.)
// to reduce our D3D12 footprint while the game's internal state machine stabilizes.
static std::atomic<int64_t> g_OverlayCooldownUntilQpc{0};
static constexpr int64_t kTransitionCooldownMs = 1500;  // 1.5 s

static void StartTransitionCooldown() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    g_OverlayCooldownUntilQpc.store(now.QuadPart + freq.QuadPart * kTransitionCooldownMs / 1000,
                                    std::memory_order_release);
    // Discard any pending deferred Signal — the queue may change during FG switch
    g_deferredSignalValue.store(0, std::memory_order_release);
    g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    g_deferredSignalQueue.store(nullptr, std::memory_order_release);
    HookLogImportant("DX12: Overlay transition cooldown started (%lldms)", (long long)kTransitionCooldownMs);
}

void DX12_StartTransitionCooldown() {
    StartTransitionCooldown();
}

void DX12_BeginStreamlineEnableCall() {
    g_StreamlineEnableCallsInFlight.fetch_add(1, std::memory_order_acq_rel);
}

void DX12_EndStreamlineEnableCall() {
    uint32_t current = g_StreamlineEnableCallsInFlight.load(std::memory_order_acquire);
    while (current != 0 && !g_StreamlineEnableCallsInFlight.compare_exchange_weak(
                               current, current - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {}
    if (current == 0) {
        HookLogImportant("DX12: Streamline enable-call tracking underflow — reset in-flight count");
    }
}

void DX12_PrepareForStreamlineEnableTransition() {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    if (runtimeMode != ce::fg_runtime::RuntimeMode::kFSRFG || !g_FGRuntimeOwnsSwapchain) {
        return;
    }

    DXGIShared::ArmStreamlineStartupTransitionWindow();
    StartTransitionCooldown();
    WaitForOverlayGpuIdle("DX12: Streamline enable prep");
    InvalidateAllOverlayCachedFrames();
    HookLogImportant(
        "DX12: Preparing for Streamline FG enable while live FSR runtime owns the swapchain "
        "(runtime=%s apiFSR=%d origGame=%p scQueue=%p cmdQ=%p)",
        ce::fg_runtime::GetRuntimeModeName(runtimeMode), g_FGCompat.IsFSRFGApiActive() ? 1 : 0, g_OriginalGameQueue,
        g_SwapchainQueue, g_CommandQueue.load(std::memory_order_acquire));
}

bool DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration() {
    const bool progressResolvedOfficialFFXPresentPath =
        g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);
    return ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(
        g_FGRuntimeOwnsSwapchain || progressResolvedOfficialFFXPresentPath,
        g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire));
}

DWORD DX12_GetGamePresentThreadId() {
    return g_GamePresentThreadId.load(std::memory_order_acquire);
}

static bool ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
    bool explicitSetOptionsActivation, bool authoritativeStreamlineHandoff, const char* source) {
    const bool streamlineStartupHandoffPending =
        DXGIShared::g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
    const bool nativeFSRInternalNoCallbackComposition = DX12_IsNativeFSRInternalNoCallbackCompositionActive();
    const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
    if (!ce::dx12_overlay_policy::ShouldClearStaleNativeFGPresentOwnershipOnStreamlineComeback(
            g_HadFSRFGPhase, explicitSetOptionsActivation, authoritativeStreamlineHandoff,
            authoritativeFSRActive, g_SwapchainQueue != nullptr,
            g_SwapchainQueue != nullptr && g_SwapchainQueue != g_OriginalGameQueue, streamlineStartupHandoffPending,
            runtimeOwnedNativeFGPresentPath,
            nativeFSRInternalNoCallbackComposition)) {
        return false;
    }

    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
        "proven Streamline takeover cleared stale native-FG Present ownership");
    ForceClearNativeFSRInternalNoCallbackComposition(
        "proven Streamline takeover cleared stale native-FG Present ownership");
    g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
    g_PostNativeFSROffGameSwapchainRecoveryQueue.store(nullptr, std::memory_order_release);
    HookLogImportant(
        "DX12: Proven Streamline takeover after FSR — cleared stale native-FG Present ownership "
        "(source=%s proof=%s explicit=%d authoritativeHandoff=%d fsrApi=%d handoffPending=%d "
        "scQueue=%p origGame=%p nativeFGPath=%d noCallback=%d)",
        source ? source : "Streamline activation",
        authoritativeStreamlineHandoff ? "authoritative-handoff" : "explicit-setoptions",
        explicitSetOptionsActivation ? 1 : 0, authoritativeStreamlineHandoff ? 1 : 0,
        authoritativeFSRActive ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0, g_SwapchainQueue,
        g_OriginalGameQueue, runtimeOwnedNativeFGPresentPath ? 1 : 0,
        nativeFSRInternalNoCallbackComposition ? 1 : 0);
    return true;
}

void DX12_OnStreamlineExplicitSetOptionsActivationConfirmed() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
        true, false, "already-live comeback upgraded by explicit SetOptions");
}

void DX12_OnStreamlineFGStateChanged(bool active) {
    const auto visibleRuntimeMode = active ? ce::fg_runtime::RuntimeMode::kDLSSFG : g_FGCompat.GetRuntimeMode();
    const bool visibleFGActive = active ? true : g_FGCompat.IsFGActive();
    HookUpdatePreferredOverlayFGPublicationState(visibleFGActive, visibleRuntimeMode,
                                                 "DX12_OnStreamlineFGStateChanged");

    ce::fg_session::EmitFGEvent(active ? ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate
                                       : ce::fg_session::FGEventKind::kTransitionCooldownComplete,
                                "DX12_OnStreamlineFGStateChanged", nullptr, nullptr,
                                active ? ce::fg_runtime::RuntimeMode::kDLSSFG : g_FGCompat.GetRuntimeMode(), active,
                                HookHasExplicitStreamlineSetOptionsActivation());

    const bool observerOnly = HookOverlayObserverOnlyEnabled();
    const bool observerPolicyOnly = HookOverlayObserverPolicyOnlyEnabled();
    const bool observerStartupPresentOnly = HookOverlayObserverStartupPresentOnlyEnabled();
    if (observerOnly) {
        const auto cleanup =
            ce::streamline_runtime_policy::ResolveObserverOnlyHeuristicCleanupForStreamlineSignalTransition(active);
        if (cleanup.clearRecentTeardownGrace) {
            const int previousHeuristicGrace = g_SLOffHeuristicGrace.exchange(0, std::memory_order_acq_rel);
            const int previousSwapchainGrace = g_SLOffSwapchainReinitGrace.exchange(0, std::memory_order_acq_rel);
            if (ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(
                    true, previousHeuristicGrace > 0, previousSwapchainGrace > 0)) {
                HookLogImportant(
                    "DX12: Observer-only Streamline FG ON - cleared stale teardown grace before fresh activation "
                    "(slOffGrace=%d swapchainGrace=%d)",
                    previousHeuristicGrace, previousSwapchainGrace);
            }
        }
        if (cleanup.seedRecentTeardownGrace) {
            g_SLOffHeuristicGrace.store(600, std::memory_order_release);
            g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);
        }
        if (cleanup.resetQueueChangeHeuristic) {
            RequestFGDetectionHeuristicReset();
        }
        if (cleanup.clearHeuristicFSR && g_FGCompat.IsHeuristicFSRFGActive()) {
            g_FGCompat.SetHeuristicFSRFGActive(false);
            HookLogImportant("DX12: Observer-only cleared heuristic FSR FG during Streamline %s transition",
                             active ? "ON" : "OFF");
        }
        if (cleanup.clearNvidiaSmoothMotion) {
            g_FGCompat.ClearNvidiaSMState();
        }
        if (active) {
            HookLogImportant(
                observerPolicyOnly
                    ? (observerStartupPresentOnly
                           ? "DX12: Streamline FG ON observed in observer-startup-present-only mode - keeping PostSL "
                             "and special Streamline Present routing passive while preserving startup-policy and "
                             "non-Streamline startup-Present probe state"
                           : "DX12: Streamline FG ON observed in observer-policy-only mode - keeping PostSL/startup "
                             "Present passive while preserving Streamline startup-policy state")
                    : "DX12: Streamline FG ON observed in observer-only mode - skipping PostSL startup routing/state "
                      "mutation");
        } else {
            HookLogImportant(
                observerPolicyOnly
                    ? (observerStartupPresentOnly
                           ? "DX12: Streamline FG OFF observed in observer-startup-present-only mode - keeping PostSL "
                             "and special Streamline Present routing passive while preserving startup-policy and "
                             "non-Streamline startup-Present probe state"
                           : "DX12: Streamline FG OFF observed in observer-policy-only mode - keeping PostSL/startup "
                             "Present passive while preserving Streamline startup-policy state")
                    : "DX12: Streamline FG OFF observed in observer-only mode - keeping PostSL disabled and clearing "
                      "startup state");
        }
        EnsurePostSLDisabledForObserverOnly(
            "DX12: observer-only mode",
            ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(observerOnly,
                                                                                                   observerPolicyOnly));
        return;
    }

    if (active) {
        g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
        const int previousHeuristicGrace = g_SLOffHeuristicGrace.exchange(0, std::memory_order_acq_rel);
        const int previousSwapchainGrace = g_SLOffSwapchainReinitGrace.exchange(0, std::memory_order_acq_rel);
        if (ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(
                true, previousHeuristicGrace > 0, previousSwapchainGrace > 0)) {
            HookLogImportant(
                "DX12: Streamline FG ON — cleared stale teardown grace before fresh activation "
                "(slOffGrace=%d swapchainGrace=%d)",
                previousHeuristicGrace, previousSwapchainGrace);
        }

        const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        const ULONGLONG startupWindowRemainingMs =
            startupWindowActive
                ? (DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire) -
                   GetTickCount64())
                : 0;
        const bool startupTopLevelPresentConsumed =
            DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
        const bool wrapperProgressObserved =
            g_PostSLSyntheticStartupWrapperProgressCount.load(std::memory_order_acquire) > 0;
        HookLogImportant(
            "DX12: Streamline FG ON — GetState transition STARTING "
            "(startupWindowActive=%d startupRemaining=%lldms consumed=%d wrapperProgress=%d)",
            startupWindowActive ? 1 : 0, (long long)startupWindowRemainingMs, startupTopLevelPresentConsumed ? 1 : 0,
            wrapperProgressObserved ? 1 : 0);

        const bool callbackAlreadyInstalled =
            DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr;
        const bool resumeConfirmedPostSLFromKeepAlive =
            ce::dx12_overlay_policy::ShouldResumeConfirmedPostSLFromKeepAliveOnStreamlineOn(
                g_PostSLExplicitOffKeepAlive.exchange(false, std::memory_order_acq_rel),
                g_PostSLConfirmedRendering.load(std::memory_order_acquire));
        g_PostSLWarmResumePreservationPending.store(callbackAlreadyInstalled && resumeConfirmedPostSLFromKeepAlive,
                                                    std::memory_order_release);

        if (callbackAlreadyInstalled && resumeConfirmedPostSLFromKeepAlive) {
            // Suspend -> resume cycle bridged by the make-before-break
            // keep-alive: PostSL stayed confirmed-and-renderable the whole
            // time, so this is a RESUME of a continuously-live path, not a
            // cold start. No synthetic-startup pending dance, no countdown
            // re-arm, no lifecycle reset — the first re-entrant present after
            // the resume renders immediately.
            g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
            g_PostSLOverlayActive.store(true, std::memory_order_release);
            g_PostSLStallCounter.store(0, std::memory_order_release);
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
            // Keep churn protection armed: a quick OFF right after this resume
            // must take the churn path, not a full teardown.
            DXGIShared::ArmStreamlineStartupTransitionWindow();
            HookLogImportant(
                "DX12: Streamline FG ON — warm PostSL resume from make-before-break keep-alive "
                "(confirmed rendering preserved, no countdown/warm-up re-arm)");
        } else if (callbackAlreadyInstalled) {
            g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
            g_PostSLOverlayActive.store(false, std::memory_order_release);
            g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
            g_PostSLStallCounter.store(0, std::memory_order_release);
            g_PostSLStableFrameCount.store(0, std::memory_order_release);
            g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
            g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
            g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
            g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
            g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
            int cooldownLeft = g_PostSLCooldownRemaining.load(std::memory_order_acquire);
            while (cooldownLeft < 60 && !g_PostSLCooldownRemaining.compare_exchange_weak(
                                            cooldownLeft, 60, std::memory_order_acq_rel, std::memory_order_acquire)) {}
            HookLogImportant(
                "DX12: Streamline FG ON — re-enabling dormant PostSL callback for startup routing "
                "(churn re-activation, cooldown=%d)",
                g_PostSLCooldownRemaining.load(std::memory_order_relaxed));
            // Re-arm the startup transition window: churn re-activation means
            // DLSS FG is still in its initialization dance (game bouncing
            // ON/OFF/ON).  The original window from the first ON may have expired,
            // leaving no protection for OFF signals during this new cycle.
            DXGIShared::ArmStreamlineStartupTransitionWindow();
            HookLogImportant("DX12: Streamline FG ON churn — re-armed startup transition window");
            ResetPostSLLifecycleForTransition("DX12: Streamline FG ON churn re-activation", true);
        } else {
            SetPostSLCallbackInstalled(true, "DX12: Streamline FG ON");
            HookLogImportant("DX12: Streamline FG ON — pre-armed PostSL callback for startup routing");
            int cooldownLeft = g_PostSLCooldownRemaining.load(std::memory_order_acquire);
            while (cooldownLeft < 60 && !g_PostSLCooldownRemaining.compare_exchange_weak(
                                            cooldownLeft, 60, std::memory_order_acq_rel, std::memory_order_acquire)) {}
            g_PostSLOverlayActive.store(false, std::memory_order_release);
            g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
            g_PostSLStallCounter.store(0, std::memory_order_release);
            g_PostSLStableFrameCount.store(0, std::memory_order_release);
            g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
            g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
            g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
            g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
            g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
            ResetPostSLLifecycleForTransition("DX12: Streamline FG ON transition", true);
        }
        if (g_HadFSRFGPhase) {
            ID3D12CommandQueue* staleScQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                const bool streamlineStartupHandoffPending =
                    DXGIShared::g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
                const bool explicitSetOptionsActivation = HookHasExplicitStreamlineSetOptionsActivation();
                ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
                    explicitSetOptionsActivation, false, "fresh Streamline active edge");
                if (ce::dx12_overlay_policy::ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(
                        g_HadFSRFGPhase, g_SwapchainQueue != nullptr,
                        g_SwapchainQueue != nullptr && g_SwapchainQueue != g_OriginalGameQueue,
                        streamlineStartupHandoffPending, resumeConfirmedPostSLFromKeepAlive)) {
                    staleScQueue = g_SwapchainQueue;
                    g_SwapchainQueue = nullptr;
                    g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                    g_SwapchainQueueCaptureTime = 0;
                    g_FGRuntimeOwnsSwapchain = false;
                    DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                    if (g_FGCompat.IsFSRFGApiActive()) {
                        SetNativeFSRStartupConfigureArmingPending(false,
                                                                  "Streamline FG comeback cleared FSR ownership");
                        ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
                            "Streamline FG comeback cleared FSR ownership");
                        g_FGCompat.SetFSRFGActive(false);
                        g_FGCompat.SetFSRFGMultiplier(0);
                        ResetAuthoritativeFSRRealFrameOnlyStreak();
                    }
                    HookLogImportant(
                        "DX12: Streamline FG ON after FSR — cleared stale FSR swapchain queue %p (origGame=%p) "
                        "to prevent DEVICE_REMOVED on FSR→DLSS transition",
                        staleScQueue, g_OriginalGameQueue);
                } else if (g_SwapchainQueue && g_SwapchainQueue != g_OriginalGameQueue &&
                           streamlineStartupHandoffPending) {
                    if (ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
                            g_HadFSRFGPhase, g_SwapchainQueue != nullptr,
                            g_SwapchainQueue != nullptr && g_SwapchainQueue != g_OriginalGameQueue,
                            streamlineStartupHandoffPending, g_PostSLLastWorkingQueue != nullptr,
                            g_SwapchainQueue != nullptr && g_SwapchainQueue == g_PostSLLastWorkingQueue)) {
                        HookLogImportant(
                            "DX12: Streamline FG ON after FSR — cleared stale PostSL lastWorking queue %p because "
                            "fresh Streamline handoff moved to new scQueue %p (origGame=%p)",
                            g_PostSLLastWorkingQueue, g_SwapchainQueue, g_OriginalGameQueue);
                        SetPostSLLastWorkingQueue(nullptr);
                    }
                    HookLogImportant(
                        "DX12: Streamline FG ON after FSR — preserving freshly handed-off Streamline swapchain queue "
                        "%p "
                        "during active startup handoff (origGame=%p)",
                        g_SwapchainQueue, g_OriginalGameQueue);
                }
            }
            if (staleScQueue) {
                staleScQueue->Release();
            }
        }

        ID3D12CommandQueue* resumeSwapchainQueue = nullptr;
        ID3D12CommandQueue* resumeLastWorkingQueue = nullptr;
        ID3D12CommandQueue* resumeOriginalGameQueue = nullptr;
        ID3D12CommandQueue* resumeCommandQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            resumeSwapchainQueue = g_SwapchainQueue;
            resumeLastWorkingQueue = g_PostSLLastWorkingQueue;
            resumeOriginalGameQueue = g_OriginalGameQueue;
            resumeCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        }
        if (ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
                g_HadFSRFGPhase, resumeLastWorkingQueue != nullptr, resumeSwapchainQueue != nullptr,
                resumeSwapchainQueue != nullptr && resumeSwapchainQueue == resumeLastWorkingQueue,
                g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.load(std::memory_order_acquire),
                resumeOriginalGameQueue != nullptr,
                resumeOriginalGameQueue != nullptr && resumeCommandQueue == resumeOriginalGameQueue)) {
            DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.store(true, std::memory_order_release);
            HookLogImportant(
                "DX12: Streamline FG ON — seeded startup bootstrap as already consumed for confirmed PostSL resume "
                "(scQueue=%p lastWorking=%p clearedStaleNoFG=%d origGame=%p cmdQ=%p)",
                resumeSwapchainQueue, resumeLastWorkingQueue,
                g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.load(std::memory_order_relaxed) ? 1 : 0,
                resumeOriginalGameQueue, resumeCommandQueue);
        }
        g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(false, std::memory_order_release);
        return;
    }
    if (!active) {
        g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
    }

    // Make-before-break: a CONFIRMED PostSL path stays armed-and-rendering
    // across the explicit OFF edge — the proxy swapchain keeps presenting
    // after slDLSSGSetOptions(off) (menus/suspension) and tearing PostSL down
    // here is what blanks those presents until an authoritative normal
    // swapchain/queue return. Never while an FSR/native-FG takeover is in play
    // (the quiesce invariant wins).
    g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
    const bool keepConfirmedPostSLAliveAcrossOff =
        ce::dx12_overlay_policy::ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(
            g_PostSLConfirmedRendering.load(std::memory_order_acquire), g_FGCompat.IsFSRFGApiActive(),
            HookHasRuntimeOwnedNativeFGPresentPath(), ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup());
    if (keepConfirmedPostSLAliveAcrossOff) {
        g_PostSLExplicitOffKeepAlive.store(true, std::memory_order_release);
        HookLogImportant(
            "DX12: Streamline FG OFF — keeping confirmed PostSL armed-and-rendering until an authoritative "
            "normal swapchain/queue return (make-before-break keep-alive)");
    } else {
        g_PostSLOverlayActive.store(false, std::memory_order_release);
    }

    const bool inStartupChurnWindow = DXGIShared::IsStreamlineStartupTransitionWindowActive();

    if (inStartupChurnWindow) {
        if (!keepConfirmedPostSLAliveAcrossOff) {
            g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);
        }
        HookLogImportant(
            "DX12: Streamline FG OFF during startup transition — keeping PostSL callback %s "
            "(churn suppression, epoch=%u keepAlive=%d)",
            keepConfirmedPostSLAliveAcrossOff ? "armed for keep-alive rendering" : "dormant",
            g_PostSLLifecycleEpoch.load(std::memory_order_acquire), keepConfirmedPostSLAliveAcrossOff ? 1 : 0);
        // Drop the AddRef'd startup-activation swapchain even on the churn
        // path: pinning it costs nothing on a quick re-ON (every startup-route
        // present re-retains it), but if the game proceeds to a full native
        // teardown instead, CE's reference makes the app's
        // CreateSwapChainForHwnd on the same HWND fail E_ACCESSDENIED through
        // all retries (session 20260613_032326: DLSS->OFF stopped the app's
        // main loop with "no swapchain after OFF request").
        ReleaseStreamlineStartupActivationSwapchain("DX12: Streamline FG OFF (startup churn)");
        g_SLOffHeuristicGrace.store(600, std::memory_order_release);
        RequestFGDetectionHeuristicReset();
        g_FGCompat.SetHeuristicFSRFGActive(false);
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
            ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, 0.0f, 0.0f, 1, "DX12_OnStreamlineFGStateChanged");
        }
        return;
    }
