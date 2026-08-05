#include "dxgi_shared_internal.h"

#ifdef BUILDING_CAPTURE_HOOK
extern "C" void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pQueue);
extern "C" void DX12_FlushDeferredSignal();
extern "C" void DX12_SetDeferOverlaySubmitToSteamECL(bool defer);
extern "C" void DX12_SubmitSteamDeferredOverlay();
extern "C" bool DX12_IsDeferOverlaySubmitPending();
extern "C" void DX12_NoteWrappedD3D12PresentResult(const char* presentName, int callCount, UINT syncInterval,
                                                   UINT presentFlags, HRESULT presentHr, BOOL isFullscreen,
                                                   BOOL isIconic, BOOL hasZeroSize, HWND gameWindow);

static void InvokeDX12WaitForOverlayCompletion(ID3D12CommandQueue* pQueue) {
    DX12_WaitForOverlayCompletion(pQueue);
}
static void InvokeDX12FlushDeferredSignal() {
    DX12_FlushDeferredSignal();
}

// Feed the DX12 present result into the focus-transition / occlusion tracking for the vtable
// DetourPresent path. The CWrapDXGISwapChain wrapper already calls
// DX12_NoteWrappedD3D12PresentResult itself; IsInWrapperPresent() gives exactly-once
// semantics (the wrapper keeps g_InWrapperPresent set across its real Present, so a re-entry
// here is skipped; the delegated external-overlay path leaves it false and relies on this).
// Without this, vtable-hooked apps (e.g. dx12_test) never update g_SwapchainPresentOccluded
// and never engage the invisible-safe not-presentable backbuffer-work hold during the Alt+Tab
// iflip<->composited mode switch, so the overlay touches the backbuffer mid-switch and the GPU
// hangs (DEVICE_HUNG).
static void NoteDX12PresentResultForVtablePath(IDXGISwapChain* pSwapChain, const char* presentName, UINT SyncInterval,
                                               UINT Flags, HRESULT hr) {
    if (!pSwapChain || IsInWrapperPresent()) {
        return;
    }
    static std::atomic<int> s_vtablePresentResultCount{0};
    const int callCount = s_vtablePresentResultCount.fetch_add(1, std::memory_order_relaxed) + 1;
    DXGI_SWAP_CHAIN_DESC desc = {};
    HWND hwnd = nullptr;
    BOOL isFullscreen = FALSE;
    BOOL isIconic = FALSE;
    BOOL hasZeroSize = FALSE;
    if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
        hwnd = desc.OutputWindow;
        isFullscreen = desc.Windowed ? FALSE : TRUE;  // borderless-fullscreen reports Windowed=TRUE
        hasZeroSize = (desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0) ? TRUE : FALSE;
        isIconic = (hwnd && IsIconic(hwnd)) ? TRUE : FALSE;
    }
    DX12_NoteWrappedD3D12PresentResult(presentName, callCount, SyncInterval, Flags, hr, isFullscreen, isIconic,
                                       hasZeroSize, hwnd);
}

// Inline wrappers for the new Steam ECL deferred overlay functions.
// Since BUILDING_CAPTURE_HOOK is defined, these are direct calls to the exports.
static void InvokeDX12SetDeferOverlaySubmitToSteamECL(bool defer) {
    DX12_SetDeferOverlaySubmitToSteamECL(defer);
}
static void InvokeDX12SubmitSteamDeferredOverlay() {
    DX12_SubmitSteamDeferredOverlay();
}
static bool InvokeDX12IsDeferOverlaySubmitPending() {
    return DX12_IsDeferOverlaySubmitPending();
}
#else
using PFN_DX12WaitForOverlayCompletion = void (*)(ID3D12CommandQueue* pQueue);
using PFN_DX12FlushDeferredSignal = void (*)();

static PFN_DX12WaitForOverlayCompletion ResolveDX12WaitForOverlayCompletion() {
    static std::once_flag s_once;
    static PFN_DX12WaitForOverlayCompletion s_fn = nullptr;

    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook) {
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        }
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12WaitForOverlayCompletion>(
                GetProcAddress(hHook, "DX12_WaitForOverlayCompletion"));
        }
    });

    return s_fn;
}

static PFN_DX12FlushDeferredSignal ResolveDX12FlushDeferredSignal() {
    static std::once_flag s_once;
    static PFN_DX12FlushDeferredSignal s_fn = nullptr;

    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook) {
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        }
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12FlushDeferredSignal>(GetProcAddress(hHook, "DX12_FlushDeferredSignal"));
        }
    });

    return s_fn;
}

static void InvokeDX12WaitForOverlayCompletion(ID3D12CommandQueue* pQueue) {
    PFN_DX12WaitForOverlayCompletion fn = ResolveDX12WaitForOverlayCompletion();
    if (fn) {
        fn(pQueue);
    }
}

static void InvokeDX12FlushDeferredSignal() {
    PFN_DX12FlushDeferredSignal fn = ResolveDX12FlushDeferredSignal();
    if (fn) {
        fn();
    }
}

// Steam ECL deferred overlay functions (stubs for non-hook builds).
// In the test stub build these should never be called meaningfully.
using PFN_DX12SetDeferOverlay = void (*)(bool);
using PFN_DX12SubmitDeferredOverlay = void (*)();
using PFN_DX12IsDeferOverlayPending = bool (*)();

static PFN_DX12SetDeferOverlay ResolveDX12SetDeferOverlay() {
    static std::once_flag s_once;
    static PFN_DX12SetDeferOverlay s_fn = nullptr;
    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook)
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12SetDeferOverlay>(
                GetProcAddress(hHook, "DX12_SetDeferOverlaySubmitToSteamECL"));
        }
    });
    return s_fn;
}

static PFN_DX12SubmitDeferredOverlay ResolveDX12SubmitSteamDeferredOverlay() {
    static std::once_flag s_once;
    static PFN_DX12SubmitDeferredOverlay s_fn = nullptr;
    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook)
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12SubmitDeferredOverlay>(
                GetProcAddress(hHook, "DX12_SubmitSteamDeferredOverlay"));
        }
    });
    return s_fn;
}

static PFN_DX12IsDeferOverlayPending ResolveDX12IsDeferOverlayPending() {
    static std::once_flag s_once;
    static PFN_DX12IsDeferOverlayPending s_fn = nullptr;
    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook)
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12IsDeferOverlayPending>(
                GetProcAddress(hHook, "DX12_IsDeferOverlaySubmitPending"));
        }
    });
    return s_fn;
}

static void InvokeDX12SetDeferOverlaySubmitToSteamECL(bool defer) {
    PFN_DX12SetDeferOverlay fn = ResolveDX12SetDeferOverlay();
    if (fn)
        fn(defer);
}
static void InvokeDX12SubmitSteamDeferredOverlay() {
    PFN_DX12SubmitDeferredOverlay fn = ResolveDX12SubmitSteamDeferredOverlay();
    if (fn)
        fn();
}
static bool InvokeDX12IsDeferOverlaySubmitPending() {
    PFN_DX12IsDeferOverlayPending fn = ResolveDX12IsDeferOverlayPending();
    return fn ? fn() : false;
}

// Stub for non-hook/test builds; present-result occlusion tracking lives in the hook DLL.
static void NoteDX12PresentResultForVtablePath(IDXGISwapChain*, const char*, UINT, UINT, HRESULT) {}
#endif

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    g_PresentCallCounter.fetch_add(1, std::memory_order_relaxed);

    // DIAGNOSTIC: time the WHOLE DetourPresent call. The ECL diagnostic proved the Alt+Tab
    // freeze stall is NOT in ExecuteCommandLists, so it is in the present path. With the
    // ProcessFrame (overlay) and overlay-completion-wait phase timers, a slow total here with
    // NO matching slow ProcessFrame/wait log means the stall is the real Present call blocking
    // on the hung GPU (the iflip<->composited mode-switch GPU TDR). Compare 32-bit vs 64-bit.
    LARGE_INTEGER diagPresentT0;
    QueryPerformanceCounter(&diagPresentT0);
    auto diagPresentTimer = ce::make_scope_guard([&]() {
        LARGE_INTEGER diagPresentT1, diagPresentFreq;
        QueryPerformanceCounter(&diagPresentT1);
        QueryPerformanceFrequency(&diagPresentFreq);
        const double diagPresentMs =
            (double)(diagPresentT1.QuadPart - diagPresentT0.QuadPart) * 1000.0 / (double)diagPresentFreq.QuadPart;
        if (diagPresentMs >= 20.0) {
            static std::atomic<int> s_diagPresentLog{0};
            const int n = s_diagPresentLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 200 || (n % 50) == 0) {
                HookLogImportant("DX12 DIAG: DetourPresent TOTAL SLOW %.1fms (tid=0x%04X)", diagPresentMs,
                                 GetCurrentThreadId());
            }
        }
    });

    // CRITICAL: Recursion guard using thread-local depth counter.
    // When using vtable hooking with Steam overlay, Steam's trampoline can call
    // back into DetourPresent via early-return paths (wrapped swapchain, shutdown, etc.)
    // before reaching the normal IsRecursivePresent() check at line ~530.
    // This caused stack overflow crashes (0xC00000FD) with ~500 recursive frames.
    static thread_local int s_presentRecurseDepth = 0;
    const bool wrappedSwapchain = IsWrappedSwapChainObject(pSwapChain);
    const bool inWrapperPresent = IsInWrapperPresent();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool isReentrant =
        (s_presentRecurseDepth > 0) && DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(
                                           dxgi_shared_oPresentTrampoline != nullptr, dxgi_shared_oPresentBypass != nullptr, inWrapperPresent,
                                           wrappedSwapchain, streamlineFGRunning);
    s_presentRecurseDepth++;
    auto depthGuard = ce::make_scope_guard([]() { s_presentRecurseDepth--; });

    if (isReentrant) {
        // Re-entrant call - forward directly to bypass or return S_OK
        if (dxgi_shared_oPresentTrampoline) {
            return dxgi_shared_oPresentTrampoline(pSwapChain, SyncInterval, Flags);

        }
        if (dxgi_shared_oPresentBypass) {
            return dxgi_shared_oPresentBypass(pSwapChain, SyncInterval, Flags);
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
                DWORD presentOwner = dxgi_shared_g_presentThreadId.load(std::memory_order_relaxed);
                int presentDepthVal = dxgi_shared_g_presentDepth.load(std::memory_order_relaxed);
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
            entryNum, pSwapChain, IsInWrapperPresent() ? 1 : 0, dxgi_shared_oPresentTrampoline);
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
    const DWORD presentOwner = dxgi_shared_g_presentThreadId.load(std::memory_order_relaxed);
    const int presentDepthVal = dxgi_shared_g_presentDepth.load(std::memory_order_relaxed);
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
    if (dxgi_shared_s_externalOverlayPresentInvokeDepth > 0) {
        PFN_Present recursiveBypass = EnsurePresentBypassTrampoline();
        if (DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(true, recursiveBypass != nullptr)) {
            static std::atomic<int> s_recursiveExternalOverlayBypassLogCount{0};
            const int bypassNum = s_recursiveExternalOverlayBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent: Bypassing recursive external-overlay Present #%d while guarded Steam hook is "
                    "active (depth=%d bypass=%p tid=0x%04X)",
                    bypassNum, dxgi_shared_s_externalOverlayPresentInvokeDepth, (void*)recursiveBypass, currentThreadId);
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
            (void*)dxgi_shared_g_externalOverlayPresentHook, (void*)dxgi_shared_oPresentTrampoline, (void*)dxgi_shared_oPresentBypass,
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

    if (!dxgi_shared_oPresentTrampoline && !dxgi_shared_oPresent) {
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
                             reentrantNum, (void*)postSLCallback, (void*)dxgi_shared_oPresentTrampoline, (void*)dxgi_shared_oPresentBypass,
                             GetCurrentThreadId());
        }
        if (dxgi_shared_oPresentTrampoline) {
            return dxgi_shared_oPresentTrampoline(pSwapChain, SyncInterval, Flags);
        }
        if (dxgi_shared_oPresentBypass) {
            return dxgi_shared_oPresentBypass(pSwapChain, SyncInterval, Flags);
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
                return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
            }
        }
    }

    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook)
            g_SharedState.inPresentHook.store(false);
    });

    g_SharedState.presentCallCount.fetch_add(1, std::memory_order_relaxed);

    if (g_SharedState.deviceRemovedFatal.load()) {
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
                return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
            }
        } else {
            s_invalidSinceQpc = 0;
        }
    }

    if (api == APIType::D3D12) {
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
                startupBlockingOverlaySwapchainStillOwnsPresent || callerFromThirdPartyOverlay)) {
            static std::atomic<int> s_overlayPresentBypassLogCount{0};
            const int logCount = s_overlayPresentBypassLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 256) == 0) {
                HookLogImportant(
                    "DetourPresent: Bypassing DX12 ProcessFrame for third-party overlay swapchain %p (caller=%s)",
                    pSwapChain, detourCallerModulePath[0] ? detourCallerModulePath : "tracked-overlay-swapchain");
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
                observerOnlyMode, api == APIType::D3D12, inWrapperPresent, wrappedSwapchain, dxgi_shared_oPresent != nullptr,
                streamlineFGRunning, dxgi_shared_s_slRoutingActive.load(std::memory_order_acquire), callerFromStreamlineModule,
                streamlineStartupHandoffInProgress, runtimeOwnedSwapchainActive, hadFSRFGPhase,
                safePostFSRBootstrapPath, postSLConfirmedRendering, startupTopLevelPresentAlreadyConsumed)) {
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
                    handoffCount, markedStartupConsumed ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                    streamlineStartupTransitionWindowActive ? 1 : 0, runtimeOwnedSwapchainActive ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0, postSLConfirmedRendering ? 1 : 0, (void*)dxgi_shared_oPresent, presentOwner,
                    presentDepthVal, currentThreadId);
            }
            if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(
                    api == APIType::D3D12, postSLConfirmedRendering)) {
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
            if (api == APIType::D3D12)
                strncpy(perfMetrics.api, "DX12", sizeof(perfMetrics.api) - 1);
            else if (api == APIType::D3D11)
                strncpy(perfMetrics.api, "DX11", sizeof(perfMetrics.api) - 1);
            else if (api == APIType::D3D10)
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
            !steamOnlyTest && api == APIType::D3D12 && steamOverlayLoaded && !IsSLInterposerLoaded();
        if (nonSLSteamInvokePath) {
            static std::atomic<int> s_steamPathLog{0};
            if (s_steamPathLog.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant("DetourPresent: non-SL Steam path — SyncInterval=%u Flags=%u (normal overlay submit)",
                                 SyncInterval, Flags);
            }
        }

        if (!steamOnlyTest && api == APIType::D3D12) {
            // Near-passthrough during no-callback FSR FG: when the UI-texture bundle is active
            // (GTA-style), skip ProcessFrame entirely — even the minimal path's QueryInterface +
            // RecordFrame + inner ProcessFrame overhead on the runtime queue desyncs AMD's QPC-timed
            // pacing and freezes GTA (~900 frames). When the bundle is unavailable (no registered UI
            // texture intercepted — test app), call the minimal ProcessFrame path so the overlay
            // renders through the normal DX12 route.
            const bool noCallbackFSRFG = DX12_IsNativeFSRInternalNoCallbackCompositionActive();
            const bool frameGenerationPresentationActive =
                noCallbackFSRFG || streamlineFGRunning || runtimeOwnedSwapchainActive || callerFromStreamlineModule ||
                callerFromFFXFrameGenerationModule || HookHasRuntimeOwnedNativeFGPresentPath();
            // Non-FG calls keep existing queue-depth behavior. Once a runtime
            // can emit its own Presents, only the pre-FG game thread may pace.
            const bool applicationSourcePresent =
                ce::dx12_overlay_policy::ShouldApplyDX12PrerenderLimitOnPresent(
                    frameGenerationPresentationActive, DX12_GetGamePresentThreadId(), currentThreadId);
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
        } else if (!steamOnlyTest && DXGIShared::ShouldRunSharedD3D10Or11ProcessFrame(api)) {
            if (api == APIType::D3D10) {
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
    if (api == APIType::D3D12 && !DX12_IsNativeFSRInternalNoCallbackCompositionActive()) {
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

    // CRITICAL: SL startup guard.  During SL DllMain / startup and subsequent
    // SL-originated Present calls, Steam may query Streamline from inside its
    // overlay Present hook.  Once CE has hooked Streamline's plugin lookup, the
    // guarded Steam path can safely return no-op SL callbacks for that
    // re-entrant query. Until then, fall back to the bypass trampoline so the
    // game remains stable.
    if (callerFromStreamlineModule && !dxgi_shared_s_slRoutingActive.load(std::memory_order_relaxed) && steamOverlayLoaded) {
        HRESULT guardedSteamHr = S_OK;
        if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags, "SL startup bypass",
                                                        &guardedSteamHr)) {
            if (api == APIType::D3D12) {
                InvokeDX12FlushDeferredSignal();
            }
            if (SUCCEEDED(guardedSteamHr)) {
                g_SharedFpsLimiter.ApplyPostPresent();
            }
            return guardedSteamHr;
        }

        PFN_Present bypass = EnsurePresentBypassTrampoline();
        if (bypass) {
            static std::atomic<int> s_startupBypassCount{0};
            int bypassNum = s_startupBypassCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant("DetourPresent: Startup bypass #%d (tid=0x%04X)", bypassNum, GetCurrentThreadId());
            }
            HRESULT bypassHr = bypass(pSwapChain, SyncInterval, Flags);
            if (api == APIType::D3D12) {
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
    if (api == APIType::D3D12 && !DX12_IsNativeFSRInternalNoCallbackCompositionActive()) {
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

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                         const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    g_PresentCallCounter.fetch_add(1, std::memory_order_relaxed);

    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

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
    const bool callerFromThirdPartyOverlay =
        TryGetModulePathFromCodeAddress(detourCallerAddress, detourCallerModulePath, sizeof(detourCallerModulePath)) &&
        ce::overlay_compat::IsThirdPartyOverlayModulePath(detourCallerModulePath);
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
    if (!vtable || !IsReadableMemory(reinterpret_cast<const void*>(vtable), 23 * sizeof(void*)) || !vtable[22]) {
        RequestHookShutdown();
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
        const bool applicationSourcePresent = ce::dx12_overlay_policy::ShouldApplyDX12PrerenderLimitOnPresent(
            frameGenerationPresentationActive, DX12_GetGamePresentThreadId(), currentThreadId);
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
