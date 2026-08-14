#include "dx12_hook_internal.h"


bool IsCurrentECLCallerFromThirdPartyOverlay(char* modulePathOut, size_t modulePathOutCount) {
if (modulePathOut && modulePathOutCount > 0) {
    modulePathOut[0] = '\0';
}

const void* callerAddress = CE_RETURN_ADDRESS();
if (!callerAddress) {
    return false;
}

char localModulePath[MAX_PATH] = {};
char* targetBuffer = (modulePathOut && modulePathOutCount > 0) ? modulePathOut : localModulePath;
const size_t targetCount = (modulePathOut && modulePathOutCount > 0) ? modulePathOutCount : sizeof(localModulePath);
if (!TryGetModulePathFromCodeAddress(callerAddress, targetBuffer, targetCount)) {
    return false;
}

return ce::overlay_compat::IsThirdPartyOverlayModulePath(targetBuffer);
}


CreateSwapchainQueueCaptureEvidence BuildCreateSwapchainQueueCaptureEvidence(const void* callerAddress, bool callerFromThirdPartyOverlay, bool callerFromFFXFGModule, bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack, const char* callerModulePath, const char* ffxModulePath) {
CreateSwapchainQueueCaptureEvidence evidence = {};
evidence.callerAddress = callerAddress;
evidence.callerFromThirdPartyOverlay = callerFromThirdPartyOverlay;
evidence.authoritativeFFXRuntimeCreator =
    ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(callerFromFFXFGModule,
                                                                                ffxFrameGenerationInStack);
evidence.authoritativeStreamlineRuntimeCreator = callerFromStreamlineFGModule || streamlineFrameGenerationInStack;
evidence.callerFromStreamlineFGModule = callerFromStreamlineFGModule;
evidence.streamlineFrameGenerationInStack = streamlineFrameGenerationInStack;
if (callerModulePath && *callerModulePath) {
    strncpy_s(evidence.callerModulePath, sizeof(evidence.callerModulePath), callerModulePath, _TRUNCATE);
}
const char* authoritativeFFXPath = (ffxModulePath && *ffxModulePath)
                                       ? ffxModulePath
                                       : (callerFromFFXFGModule && callerModulePath ? callerModulePath : nullptr);
if (authoritativeFFXPath && *authoritativeFFXPath) {
    strncpy_s(evidence.ffxModulePath, sizeof(evidence.ffxModulePath), authoritativeFFXPath, _TRUNCATE);
    evidence.officialAMDFFXRuntimeCreator = ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(authoritativeFFXPath);
}
return evidence;
}


CreateSwapchainForHwndCallerContext ResolveCreateSwapchainForHwndCallerContext() {
CreateSwapchainForHwndCallerContext context = {};

char immediateCallerModulePath[MAX_PATH] = {};
const void* immediateCallerAddress = CE_RETURN_ADDRESS();
TryGetModulePathFromCodeAddress(immediateCallerAddress, immediateCallerModulePath,
                                sizeof(immediateCallerModulePath));

const char* effectiveCallerModulePath = ce::overlay_compat::GetEffectiveCreateSwapchainCallerModulePath(
    dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
if (effectiveCallerModulePath && *effectiveCallerModulePath) {
    strncpy_s(context.callerModulePath, sizeof(context.callerModulePath), effectiveCallerModulePath, _TRUNCATE);
}

context.callerAddress = dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath[0]
                            ? dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerAddress
                            : immediateCallerAddress;
context.callerFromFFXFGModule = ce::overlay_compat::IsFFXFrameGenerationModulePath(context.callerModulePath);
context.callerFromThirdPartyOverlay = ce::overlay_compat::IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(
    dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
return context;
}


bool ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(const char* context, bool rawCallerFromThirdPartyOverlay, bool callerFromFFXFGModule, bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack, const char* callerModulePath) {
const bool authoritativeFGRuntimeSwapchainCreator =
    ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
        callerFromFFXFGModule, ffxFrameGenerationInStack, callerFromStreamlineFGModule,
        streamlineFrameGenerationInStack);
if (rawCallerFromThirdPartyOverlay && authoritativeFGRuntimeSwapchainCreator) {
    static std::atomic<int> s_wrappedFGCreateCallerLogCount{0};
    const int logCount = s_wrappedFGCreateCallerLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20) {
        const char* runtimeKind = (callerFromFFXFGModule || ffxFrameGenerationInStack) ? "FFX" : "Streamline";
        if (callerFromStreamlineFGModule || callerFromFFXFGModule || ffxFrameGenerationInStack) {
            HookLogImportant(
                "%s: %s frame-generation stack detected behind third-party overlay caller %s — treating "
                "swapchain as authoritative runtime takeover",
                context ? context : "CreateSwapChain", runtimeKind,
                callerModulePath && *callerModulePath ? callerModulePath : "unknown");
        } else {
            HookLogImportant(
                "%s: Streamline stack detected behind third-party overlay caller %s — deferring takeover "
                "classification until queue identity is known",
                context ? context : "CreateSwapChain",
                callerModulePath && *callerModulePath ? callerModulePath : "unknown");
        }
    }
}

return rawCallerFromThirdPartyOverlay && !authoritativeFGRuntimeSwapchainCreator;
}


HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndInline(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
if (HookIsShuttingDown()) {
    if (dx12_hook_s_oCreateSCForHwndInline)
        return dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    return E_FAIL;
}

// Skip side-effects for temp swapchains created during hook installation
if (dx12_hook_g_CreatingTempSwapchain.load(std::memory_order_acquire)) {
    HookLog("CreateSwapChainForHwnd INLINE: Temp swapchain — passthrough");
    return dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
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
        deferredStreamlineHandoffQueue, dx12_hook_g_OriginalGameQueue,
        ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), g_FGCompat.IsFSRFGApiActive() ? 1 : 0,
        dx12_hook_g_HadFSRFGPhase ? 1 : 0, IsStreamlineLoaded() ? 1 : 0);
    // MAKE-BEFORE-BREAK for the runtime's replacement swapchain: the retained startup-activation
    // swapchain is an AddRef'd COM reference to the OLD (pre-handoff) chain. Holding it across this
    // create pins the old chain's HWND association, so DXGI fails the runtime's replacement create
    // with E_ACCESSDENIED and the game crashes dereferencing the null swapchain (GTA FSR->DLSS apply,
    // session 20260702_092933). Its purpose — PostSL startup recovery on the OLD chain — is moot once
    // the runtime replaces the swapchain, so release it BEFORE forwarding the create.
    ReleaseStreamlineStartupActivationSwapchain(
        "CreateSwapChainForHwnd INLINE: pre post-FSR Streamline runtime swapchain create");
}

HRESULT hr = dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
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

            HasRetainedStreamlineStartupActivationSwapchain() ? 1 : 0,
            callerModulePath[0] ? callerModulePath : "unknown");
        ReleaseStreamlineStartupActivationSwapchain(
            "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED runtime-managed minimal recovery");
        for (int attempt = 1; attempt <= 5 && hr == E_ACCESSDENIED; ++attempt) {
            Sleep(20);
            hr = dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
            if (SUCCEEDED(hr)) {
                HookLogImportant(
                    "CreateSwapChainForHwnd INLINE: runtime-managed minimal-recovery retry %d succeeded "
                    "hr=0x%08X sc=%p",
                    attempt, hr, (ppSC && *ppSC) ? (void*)*ppSC : nullptr);
            }
        }
        if (hr == E_ACCESSDENIED) {
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: runtime-managed minimal recovery still E_ACCESSDENIED — "
                "escalating to full overlay cleanup for HWND=%p",
                hWnd);
            {
                std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
                dx12_hook_g_LastSwapChain = nullptr;
                CleanupOverlay();
                CleanupRTVs();
                dx12_hook_g_State.overlayInit = false;
            }
            {
                std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
                dx12_hook_s_hwndSwapchainMap.erase(hWnd);
            }
            if (ppSC && *ppSC) {
                ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
            }
            for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant(
                        "CreateSwapChainForHwnd INLINE: escalated full-cleanup retry %d succeeded hr=0x%08X",
                        attempt, hr);
                }
            }
        }
        if (hr == E_ACCESSDENIED) {
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED persists after CE unpin + full cleanup — "
                "returning the error to the caller (HWND=%p)",
                hWnd);
            CaptureCreateSwapchainAccessDeniedExhaustedDump(hWnd, "CreateSwapChainForHwnd INLINE minimal recovery");
        }
    } else {
        HookLogImportant(
            "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED for HWND=%p — "
            "cleaning up overlay refs "
            "(slLoaded=%d slFG=%d startupPending=%d callerFFX=%d stackFFX=%d module=%s)",
            hWnd, streamlineModuleLoaded ? 1 : 0, streamlineFGRunning ? 1 : 0,
            streamlineStartupHandoffPending ? 1 : 0, callerFromFFXFGModule ? 1 : 0,
            ffxFrameGenerationInStack ? 1 : 0, callerModulePath[0] ? callerModulePath : "unknown");

        // Clean up ALL overlay resources — same sequence as deep hook and
        // DX12_OnSwapchainResizeBegin to fully release the HWND association.
        {
            std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
            dx12_hook_g_LastSwapChain = nullptr;
            CleanupOverlay();
            CleanupRTVs();
            dx12_hook_g_State.overlayInit = false;
        }
        // See the deep-hook recovery above: a retained startup-activation
        // swapchain pins the HWND association and makes every retry fail.
        ReleaseStreamlineStartupActivationSwapchain("CreateSwapChainForHwnd INLINE: E_ACCESSDENIED recovery");
        HookLogImportant("CreateSwapChainForHwnd INLINE: Released overlay + RTV refs for HWND=%p", hWnd);
        {
            std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
            dx12_hook_s_hwndSwapchainMap.erase(hWnd);
        }
        if (ppSC && *ppSC) {
            ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
        }

        // Retry: 10 attempts × 20ms = 200ms max
        for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
            Sleep(20);
            hr = dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
            if (SUCCEEDED(hr)) {
                HookLogImportant("CreateSwapChainForHwnd INLINE: Retry attempt %d succeeded hr=0x%08X", attempt,
                                 hr);
                break;
            }
        }
        if (FAILED(hr)) {
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: All retries exhausted — returning E_ACCESSDENIED to caller "
                "(HWND=%p)",
                hWnd);
            CaptureCreateSwapchainAccessDeniedExhaustedDump(hWnd, "CreateSwapChainForHwnd INLINE full recovery");
        }
    }
}

if (SUCCEEDED(hr) && ppSC && *ppSC) {
    if (callerFromThirdPartyOverlay) {
        MarkThirdPartyOverlaySwapchain(*ppSC, callerModulePath);
        HookLogImportant(
            "CreateSwapChainForHwnd INLINE: Third-party overlay caller %s created swapchain %p for HWND=%p — "
            "leaving CE queue and transition state unchanged",
            callerModulePath[0] ? callerModulePath : "unknown", *ppSC, hWnd);
        return hr;
    }
    IDXGISwapChain* newSC = static_cast<IDXGISwapChain*>(*ppSC);
    if (ShouldBypassInvisibleWindowCreateSwapchainSideEffects(hWnd, newSC, "CreateSwapChainForHwnd INLINE", hr)) {
        return hr;
    }
    if (HandleProtectedOfficialFFXStartupSwapchainCreate(captureEvidence, pDevice, newSC,
                                                         "CreateSwapChainForHwnd INLINE")) {
        return hr;
    }

    TrackSwapchainHwnd(*ppSC, hWnd);
    HookLogImportant("CreateSwapChainForHwnd INLINE: Created swapchain %p for HWND=%p", *ppSC, hWnd);

    // A later runtime-created DX12 swapchain can expose a different Present
    // implementation than the one we patched during startup. Refresh the
    // full per-swapchain Present hook path here so top-level Present traffic
    // stays visible after a Streamline handoff.  For the post-FSR Streamline
    // handoff, however, Streamline must establish its outer Present chain
    // first; CE remains available through the inline/re-entrant PostSL path.
    if (deferPresentHookRefreshForStreamlineHandoff) {
        HookLogImportant(
            "CreateSwapChainForHwnd INLINE: Deferring CE Present hook refresh for post-FSR Streamline runtime "
            "handoff (sc=%p queue=%p)",
            newSC, deferredStreamlineHandoffQueue);
    } else {
        RefreshPresentHooksForRealSwapchain(newSC, "CreateSwapChainForHwnd INLINE");
    }

    CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSC, "CreateSwapChainForHwnd INLINE", captureEvidence);
}

return hr;
}


// Detour for global CreateSwapChain hook


HRESULT STDMETHODCALLTYPE DetourCreateSwapChainGlobal(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
// CRITICAL: Pass through during shutdown
if (HookIsShuttingDown()) {
    if (dx12_hook_oCreateSwapChainGlobal)
        return dx12_hook_oCreateSwapChainGlobal(pThis, pDevice, pDesc, ppSwapChain);
    return E_FAIL;
}

if (DX12_IsInternalDXGISwapchainProbe()) {
    HookLog("DetourCreateSwapChainGlobal: Internal D3D10/11 probe — passthrough without DX12 side-effects");
    return dx12_hook_oCreateSwapChainGlobal(pThis, pDevice, pDesc, ppSwapChain);
}

HookLog("DetourCreateSwapChainGlobal: CALLED (factory=%p, device=%p, swapEffect=%d)", pThis, pDevice,
        pDesc ? (int)pDesc->SwapEffect : -1);

const void* callerAddress = CE_RETURN_ADDRESS();
char callerModulePath[MAX_PATH] = {};
const bool rawCallerFromThirdPartyOverlay =
    callerAddress && TryGetModulePathFromCodeAddress(callerAddress, callerModulePath, sizeof(callerModulePath)) &&
    ce::overlay_compat::IsThirdPartyOverlayModulePath(callerModulePath);
const bool callerFromFFXFGModule =
    callerAddress && ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(callerAddress);
char ffxStackModulePath[MAX_PATH] = {};
const bool ffxFrameGenerationInStack =
    ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModulePath, sizeof(ffxStackModulePath));
const bool callerFromStreamlineFGModule =
    callerAddress && ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(callerAddress);
const bool streamlineFrameGenerationInStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack();
const bool callerFromThirdPartyOverlay = ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
    "DetourCreateSwapChainGlobal", rawCallerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
    callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath);
const auto captureEvidence = BuildCreateSwapchainQueueCaptureEvidence(
    callerAddress, callerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
    callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath, ffxStackModulePath);

// Apply backbuffer count override from config
DXGI_SWAP_CHAIN_DESC modifiedDesc;
DXGI_SWAP_CHAIN_DESC* pDescToUse = pDesc;
const bool applyDescriptorOverrides = ShouldApplySwapchainDescriptorOverridesForCreate(captureEvidence);
if (pDesc && !applyDescriptorOverrides) {
    LogSkippedSwapchainDescriptorOverridesForRuntimeCreate("DetourCreateSwapChainGlobal", captureEvidence,
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
            HookLogImportant(
                "DetourCreateSwapChainGlobal: Skipping BufferCount override %u < game's %u (flip model)", requested,
                modifiedDesc.BufferCount);
        } else if (modifiedDesc.BufferCount != requested) {
            HookLogImportant("DetourCreateSwapChainGlobal: Overriding BufferCount %u -> %u",
                             modifiedDesc.BufferCount, requested);
            modifiedDesc.BufferCount = requested;
        }
    }
    pDescToUse = &modifiedDesc;
}

// Call original with (possibly) modified descriptor
HRESULT hr = dx12_hook_oCreateSwapChainGlobal(pThis, pDevice, pDescToUse, ppSwapChain);

if (FAILED(hr)) {
    static std::atomic<int> s_createFailureLogCount{0};
    const int failureNum = s_createFailureLogCount.fetch_add(1, std::memory_order_relaxed);
    if (failureNum < 20 || (failureNum % 100) == 0) {
        HookLogImportant(
            "DetourCreateSwapChainGlobal: original CreateSwapChain failed hr=0x%08X "
            "(factory=%p device=%p caller=%s failure #%d) — a failed FG-runtime swapchain create can be "
            "fatal to the game",
            static_cast<unsigned>(hr), pThis, pDevice, callerModulePath[0] ? callerModulePath : "stack",
            failureNum + 1);
    }
}

if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    if (callerFromThirdPartyOverlay) {
        MarkThirdPartyOverlaySwapchain(*ppSwapChain, callerModulePath);
        HookLogImportant(
            "DetourCreateSwapChainGlobal: Third-party overlay caller %s created swapchain %p — bypassing CE "
            "swapchain side-effects",
            callerModulePath[0] ? callerModulePath : "unknown", *ppSwapChain);
        CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSwapChain, "CreateSwapChain Global overlay bypass",
                                              captureEvidence);
        return hr;
    }
    if (pDesc && ShouldBypassInvisibleWindowCreateSwapchainSideEffects(
                     pDesc->OutputWindow, *ppSwapChain, "DetourCreateSwapChainGlobal", hr)) {
        return hr;
    }

    // Log swapchain details
    if (pDesc) {
        HookLog("DetourCreateSwapChainGlobal: Creating swapchain %ux%u", pDesc->BufferDesc.Width,
                pDesc->BufferDesc.Height);
    }

    if (HandleProtectedOfficialFFXStartupSwapchainCreate(captureEvidence, pDevice, *ppSwapChain,
                                                         "CreateSwapChain")) {
        return hr;
    }

    RefreshPresentHooksForRealSwapchain(*ppSwapChain, "CreateSwapChain");

    // Streamline runtime-owned swapchains are wrapped with the non-retaining wrapper so CE sees
    // every runtime Present (game frames AND generated frames) without patching the shared
    // dxgi!Present entry — the precondition for leaving that entry to a multi-overlay foreign
    // chain in FG games. The wrapper borrows the runtime's CreateSwapChain reference and adds no
    // refs of its own, so Streamline's release/recreate on an FG transition stays byte-identical
    // to a process without CE; a retaining wrapper would pin the old swapchain and break the
    // DLSS-G handoff with E_ACCESSDENIED on the same HWND.
    if (IsStreamlineLoaded()) {
        if (DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(true)) {
            CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSwapChain,
                                                  "CreateSwapChain Global Streamline fallback", captureEvidence);
        }
        if (ShouldWrapStreamlineRuntimeSwapchainForForeignChainView() && IsStreamlineRuntimeSwapchainWrappable(pDevice)) {
            void* pExistingWrapper = nullptr;
            if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
                ((IUnknown*)pExistingWrapper)->Release();
                HookLog("DetourCreateSwapChainGlobal: Streamline swapchain already wrapped, skipping double-wrap");
                return hr;
            }
            IDXGISwapChain* pRealSwapChain = *ppSwapChain;
            auto* wrapper = new CWrapDXGISwapChain(pRealSwapChain, pDevice,
                                                   /*streamlineRuntimeNonRetaining=*/true);
            *ppSwapChain = wrapper;
            HookLogImportant(
                "DetourCreateSwapChainGlobal: Wrapped Streamline runtime swapchain (real=%p wrapper=%p)",
                pRealSwapChain, wrapper);
            DXGIShared::MaybeTransitionPresentEntryToForeignChainForWrappedRuntimeSwapchain(
                pRealSwapChain, "CreateSwapChain Global Streamline wrap");
        } else {
            HookLog(
                "DetourCreateSwapChainGlobal: Streamline present, skipping wrap "
                "(sc=%p loadedOverlays=%zu)",
                *ppSwapChain,
                ce::overlay_compat::CountLoadedTrackedOverlayModules(
                    ce::overlay_compat::TrackedOverlaySubset::kOverlay));
        }
        return hr;
    }

    if (ShouldPreserveDX12SwapchainIdentityForForeignChain(pDevice)) {
        HookLogImportant(
            "DetourCreateSwapChainGlobal: Preserving real DX12 swapchain identity below the "
            "foreign Present chain (sc=%p) — deep Present interception already covers CE",
            *ppSwapChain);
        return hr;
    }

    // The wrapper remains the fallback when CE has no deep Present view below a foreign
    // chain. Do not use it merely as a second view when the deep hook already covers CE.

    // CRITICAL: Check if this swapchain is already wrapped
    // This prevents double-wrapping which causes infinite Present recursion
    void* pExistingWrapper = nullptr;
    if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
        ((IUnknown*)pExistingWrapper)->Release();
        HookLog(
            "DetourCreateSwapChainGlobal: Swapchain already wrapped, "
            "skipping double-wrap");
        return hr;
    }

    // Wrap the swapchain with CWrapDXGISwapChain
    HookLog("DetourCreateSwapChainGlobal: Wrapping swapchain %p", *ppSwapChain);
    auto* wrapper = new CWrapDXGISwapChain(*ppSwapChain, pDevice);
    *ppSwapChain = wrapper;
    HookLog("DetourCreateSwapChainGlobal: Swapchain wrapped successfully");

    // Don't capture queue here — global hooks fire for non-game swapchains
    // (e.g. Social Club internal).  The inline hook handles queue capture.
}

return hr;
}


// Detour for global CreateSwapChainForHwnd hook


HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndGlobal(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
// CRITICAL: Pass through during shutdown
if (HookIsShuttingDown()) {
    if (dx12_hook_oCreateSwapChainForHwndGlobal)
        return dx12_hook_oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    return E_FAIL;
}

HookLogImportant(
    "DetourCreateSwapChainForHwndGlobal: CALLED (factory=%p, device=%p, "
    "hwnd=%p)",
    pThis, pDevice, hWnd);

const void* callerAddress = CE_RETURN_ADDRESS();
char callerModulePath[MAX_PATH] = {};
const bool rawCallerFromThirdPartyOverlay =
    callerAddress && TryGetModulePathFromCodeAddress(callerAddress, callerModulePath, sizeof(callerModulePath)) &&
    ce::overlay_compat::IsThirdPartyOverlayModulePath(callerModulePath);
const bool callerFromFFXFGModule =
    callerAddress && ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(callerAddress);
char ffxStackModulePath[MAX_PATH] = {};
const bool ffxFrameGenerationInStack =
    ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModulePath, sizeof(ffxStackModulePath));
const bool callerFromStreamlineFGModule =
    callerAddress && ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(callerAddress);
const bool streamlineFrameGenerationInStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack();
const bool callerFromThirdPartyOverlay = ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
    "DetourCreateSwapChainForHwndGlobal", rawCallerFromThirdPartyOverlay, callerFromFFXFGModule,
    ffxFrameGenerationInStack, callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath);
const auto captureEvidence = BuildCreateSwapchainQueueCaptureEvidence(
    callerAddress, callerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
    callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath, ffxStackModulePath);

// Apply backbuffer count override from config
DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
const bool applyDescriptorOverrides = ShouldApplySwapchainDescriptorOverridesForCreate(captureEvidence);
if (pDesc && !applyDescriptorOverrides) {
    LogSkippedSwapchainDescriptorOverridesForRuntimeCreate("DetourCreateSwapChainForHwndGlobal", captureEvidence,
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
            HookLogImportant(
                "DetourCreateSwapChainForHwndGlobal: Skipping BufferCount override %u < game's %u (flip model)",
                requested, modifiedDesc.BufferCount);
        } else if (modifiedDesc.BufferCount != requested) {
            HookLogImportant("DetourCreateSwapChainForHwndGlobal: Overriding BufferCount %u -> %u",
                             modifiedDesc.BufferCount, requested);
            modifiedDesc.BufferCount = requested;
        }
    }
    pDescToUse = &modifiedDesc;
}

PrepareForAuthoritativeFFXSwapchainCreate(captureEvidence, "DetourCreateSwapChainForHwndGlobal");

// Forward the original external caller through the DXGI vtable -> real DXGI
// function chain so our inline/deep hooks don't misclassify CE's own detour
// frame as the authoritative CreateSwapChainForHwnd caller.
ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard inlineSideEffectGuard;
ScopedForwardedCreateSwapchainForHwndCallerContext forwardedCallerContext(callerAddress, callerModulePath);
HRESULT hr = dx12_hook_oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);

if (ce::dx12_overlay_policy::ShouldSkipGlobalCreateSwapchainForHwndSideEffectsAfterInlineForward(
        inlineSideEffectGuard.InlineHandledForwardedCall())) {
    static std::atomic<int> s_inlineHandledForwardedGlobalLogCount{0};
    const int logCount = s_inlineHandledForwardedGlobalLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 128) == 0) {
        HookLogImportant(
            "DetourCreateSwapChainForHwndGlobal: inline CreateSwapChainForHwnd hook already handled forwarded "
            "swapchain side-effects (hr=0x%08X sc=%p hwnd=%p caller=%s count=%d) — skipping duplicate global "
            "processing",
            hr, (ppSC && *ppSC) ? *ppSC : nullptr, hWnd, callerModulePath[0] ? callerModulePath : "unknown",
            logCount + 1);
    }
    return hr;
}

if (SUCCEEDED(hr) && ppSC && *ppSC) {
    if (callerFromThirdPartyOverlay) {
        MarkThirdPartyOverlaySwapchain(*ppSC, callerModulePath);
        HookLogImportant(
            "DetourCreateSwapChainForHwndGlobal: Third-party overlay caller %s created swapchain %p for HWND=%p "
            "— bypassing CE swapchain side-effects",
            callerModulePath[0] ? callerModulePath : "unknown", *ppSC, hWnd);
        CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSC, "CreateSwapChainForHwnd Global overlay bypass",
                                              captureEvidence);
        return hr;
    }
    if (ShouldBypassInvisibleWindowCreateSwapchainSideEffects(hWnd, *ppSC, "DetourCreateSwapChainForHwndGlobal",
                                                              hr)) {
        return hr;
    }

    // Log swapchain details
    if (pDesc) {
        HookLog("DetourCreateSwapChainForHwndGlobal: Creating swapchain %ux%u", pDesc->Width, pDesc->Height);
    }

    if (HandleProtectedOfficialFFXStartupSwapchainCreate(captureEvidence, pDevice, *ppSC,
                                                         "CreateSwapChainForHwnd")) {
        return hr;
    }

    StartTransitionCooldown();

    RefreshPresentHooksForRealSwapchain(*ppSC, "CreateSwapChainForHwnd");

    // Streamline runtime-owned swapchains are wrapped with the non-retaining wrapper so CE sees
    // every runtime Present (game frames AND generated frames) without patching the shared
    // dxgi!Present entry — the precondition for leaving that entry to a multi-overlay foreign
    // chain in FG games. The wrapper borrows the runtime's CreateSwapChain reference and adds no
    // refs of its own, so Streamline's release/recreate on an FG transition stays byte-identical
    // to a process without CE; a retaining wrapper would pin the old swapchain and break the
    // DLSS-G handoff with E_ACCESSDENIED on the same HWND.
    if (IsStreamlineLoaded()) {
        if (DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(true)) {
            CaptureSwapchainQueueFromCreateDevice(
                pDevice, *ppSC, "CreateSwapChainForHwnd Global Streamline fallback", captureEvidence);
        }
        if (ShouldWrapStreamlineRuntimeSwapchainForForeignChainView() && IsStreamlineRuntimeSwapchainWrappable(pDevice)) {
            void* pExistingWrapper = nullptr;
            if (SUCCEEDED((*ppSC)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
                ((IUnknown*)pExistingWrapper)->Release();
                HookLog("DetourCreateSwapChainForHwndGlobal: Streamline swapchain already wrapped, skipping "
                        "double-wrap");
                return hr;
            }
            IDXGISwapChain* pRealSwapChain = static_cast<IDXGISwapChain*>(*ppSC);
            auto* wrapper = new CWrapDXGISwapChain(*ppSC, pDevice, /*streamlineRuntimeNonRetaining=*/true);
            *ppSC = static_cast<IDXGISwapChain1*>(wrapper);
            HookLogImportant(
                "DetourCreateSwapChainForHwndGlobal: Wrapped Streamline runtime swapchain "
                "(real=%p wrapper=%p hwnd=%p)",
                pRealSwapChain, wrapper, hWnd);
            DXGIShared::MaybeTransitionPresentEntryToForeignChainForWrappedRuntimeSwapchain(
                pRealSwapChain, "CreateSwapChainForHwnd Global Streamline wrap");
        } else {
            HookLog(
                "DetourCreateSwapChainForHwndGlobal: Streamline present, skipping wrap "
                "(sc=%p loadedOverlays=%zu)",
                *ppSC,
                ce::overlay_compat::CountLoadedTrackedOverlayModules(
                    ce::overlay_compat::TrackedOverlaySubset::kOverlay));
        }
        return hr;
    }

    if (ShouldPreserveDX12SwapchainIdentityForForeignChain(pDevice)) {
        HookLogImportant(
            "DetourCreateSwapChainForHwndGlobal: Preserving real DX12 swapchain identity below the "
            "foreign Present chain (sc=%p hwnd=%p) — deep Present interception already covers CE",
            *ppSC, hWnd);
        return hr;
    }

    // The wrapper remains the fallback when CE has no deep Present view below a foreign
    // chain. Do not use it merely as a second view when the deep hook already covers CE.

    // CRITICAL: Check if this swapchain is already wrapped
    // This prevents double-wrapping which causes infinite Present recursion
    void* pExistingWrapper = nullptr;
    if (SUCCEEDED((*ppSC)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
        ((IUnknown*)pExistingWrapper)->Release();
        HookLog(
            "DetourCreateSwapChainForHwndGlobal: Swapchain already wrapped, "
            "skipping double-wrap");
        return hr;
    }

    HookLog("DetourCreateSwapChainForHwndGlobal: Wrapping swapchain %p", *ppSC);
    auto* wrapper = new CWrapDXGISwapChain(*ppSC, pDevice);
    *ppSC = (IDXGISwapChain1*)wrapper;
    HookLog("DetourCreateSwapChainForHwndGlobal: Swapchain wrapped successfully");

    // Don't capture queue here — inline hook handles queue capture for all
    // CreateSwapChainForHwnd calls, including FG runtime swapchains.
}

return hr;
}
