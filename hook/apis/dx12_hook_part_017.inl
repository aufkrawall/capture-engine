                HasRetainedStreamlineStartupActivationSwapchain() ? 1 : 0,
                callerModulePath[0] ? callerModulePath : "unknown");
            ReleaseStreamlineStartupActivationSwapchain(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED runtime-managed minimal recovery");
            for (int attempt = 1; attempt <= 5 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
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
                    hr = s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
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
                std::lock_guard<std::recursive_mutex> overlayLock(g_OverlayMutex);
                g_LastSwapChain = nullptr;
                CleanupOverlay();
                CleanupRTVs();
                g_State.overlayInit = false;
            }
            // See the deep-hook recovery above: a retained startup-activation
            // swapchain pins the HWND association and makes every retry fail.
            ReleaseStreamlineStartupActivationSwapchain("CreateSwapChainForHwnd INLINE: E_ACCESSDENIED recovery");
            HookLogImportant("CreateSwapChainForHwnd INLINE: Released overlay + RTV refs for HWND=%p", hWnd);
            {
                std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
                s_hwndSwapchainMap.erase(hWnd);
            }
            if (ppSC && *ppSC) {
                ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
            }

            // Retry: 10 attempts × 20ms = 200ms max
            for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
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

// Check if Streamline (DLSS FG interposer) is loaded.
// When present, we MUST NOT wrap swapchains with CWrapDXGISwapChain because:
// - Streamline manages the real swapchain lifecycle internally
// - Our wrapper adds an extra COM ref layer that prevents Streamline from
//   destroying the old SC before creating the FG SC on the same HWND
// - This causes E_ACCESSDENIED when DLSS FG tries to activate
// The inline Present hooks (installed on the real DXGI function) provide the
// same interception without interfering with Streamline's lifecycle management.
static bool IsStreamlineLoaded() {
    static bool detected = false;
    if (detected)
        return true;
    if (GetModuleHandleA("sl.interposer.dll") != nullptr) {
        detected = true;
        HookLogImportant("DX12: Streamline interposer detected — skipping swapchain wrapping for FG compat");
        return true;
    }
    return false;
}

// Detour for global CreateSwapChain hook
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainGlobal(IDXGIFactory* pThis, IUnknown* pDevice,
                                                             DXGI_SWAP_CHAIN_DESC* pDesc,
                                                             IDXGISwapChain** ppSwapChain) {
    // CRITICAL: Pass through during shutdown
    if (HookIsShuttingDown()) {
        if (oCreateSwapChainGlobal)
            return oCreateSwapChainGlobal(pThis, pDevice, pDesc, ppSwapChain);
        return E_FAIL;
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
    HRESULT hr = oCreateSwapChainGlobal(pThis, pDevice, pDescToUse, ppSwapChain);

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

        // When Streamline is loaded, skip wrapping to avoid blocking FG swapchain
        // lifecycle management.  Inline Present hooks provide the same interception.
        if (IsStreamlineLoaded()) {
            HookLog("DetourCreateSwapChainGlobal: Streamline present, skipping wrap (sc=%p)", *ppSwapChain);
            if (DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(true)) {
                CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSwapChain,
                                                      "CreateSwapChain Global Streamline fallback", captureEvidence);
            }
            return hr;
        }

        // NOTE: We don't install global Present vtable hooks for DX12.
        // The wrapper (CWrapDXGISwapChain) handles all Present interception.
        // This avoids conflicts between vtable hooks and wrapper interception
        // that caused stack overflow crashes.

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
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndGlobal(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                    const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                    IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    // CRITICAL: Pass through during shutdown
    if (HookIsShuttingDown()) {
        if (oCreateSwapChainForHwndGlobal)
            return oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
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
    HRESULT hr = oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);

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

        // When Streamline is loaded, skip wrapping to avoid blocking FG swapchain
        // lifecycle management.  Inline Present hooks provide the same interception.
        if (IsStreamlineLoaded()) {
            HookLog("DetourCreateSwapChainForHwndGlobal: Streamline present, skipping wrap (sc=%p)", *ppSC);
            if (DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(true)) {
                CaptureSwapchainQueueFromCreateDevice(
                    pDevice, *ppSC, "CreateSwapChainForHwnd Global Streamline fallback", captureEvidence);
            }
            return hr;
        }

        // NOTE: We don't install global Present vtable hooks for DX12.
        // The wrapper (CWrapDXGISwapChain) handles all Present interception.
        // This avoids conflicts between vtable hooks and wrapper interception
        // that caused stack overflow crashes.

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
// This hooks the factory vtable directly in the DXGI module
static void InstallGlobalVTableHooks() {
    HookLog("DX12: InstallGlobalVTableHooks called");

    // CRITICAL: Install global factory vtable hooks to catch swapchain creation
    // even for factories created before our IAT hooks were installed.
    // This ensures ALL swapchains get wrapped regardless of timing.

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        HookLog("DX12: DXGI module not loaded, skipping factory vtable hooks");
        return;
    }

    // Get CreateDXGIFactory1 export to create a temp factory
    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateFactory) {
        HookLog("DX12: CreateDXGIFactory1 not found");
        return;
    }

    // Create a temp factory to get its vtable
    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        HookLog("DX12: Failed to create temp factory for vtable extraction");
        return;
    }

    // Get the vtable - ALL IDXGIFactory instances share this vtable
    void** vtable = *(void***)pFactory;
    HookLog("DX12: Factory vtable at %p", vtable);

    // Save the real CreateSwapChainForHwnd address BEFORE vtable patching
    void* realCreateSCForHwndAddr = vtable[15];
    s_realCreateSCForHwndAddr = realCreateSCForHwndAddr;

    // Hook CreateSwapChain (vtable[10] for IDXGIFactory)
    // Hook CreateSwapChainForHwnd (vtable[15] for IDXGIFactory2)
    if (VTableHook::Create(&vtable[10], (LPVOID)DetourCreateSwapChainGlobal, (LPVOID*)&oCreateSwapChainGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChain at vtable[10]");
    }

    if (VTableHook::Create(&vtable[15], (LPVOID)DetourCreateSwapChainForHwndGlobal,
                           (LPVOID*)&oCreateSwapChainForHwndGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChainForHwnd at vtable[15]");
    }

    pFactory->Release();

    // Also hook IDXGIFactory4 and IDXGIFactory6 vtables to catch games that
    // QueryInterface for higher factory versions (different vtable pointers).
    // CreateSwapChainForHwnd is at the same slot (15) in all factory versions
    // because IDXGIFactory4 inherits from IDXGIFactory3 → IDXGIFactory2.
    IDXGIFactory4* pFactory4 = nullptr;
    if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory4)))) {
        void** vtable4 = *(void***)pFactory4;
        HookLog("DX12: IDXGIFactory4 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable4, vtable,
                (int)(vtable4 == vtable));
        if (vtable4 != vtable) {  // Different vtable pointer
            VTableHook::Create(&vtable4[10], (LPVOID)DetourCreateSwapChainGlobal, nullptr);
            VTableHook::Create(&vtable4[15], (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
            HookLog("DX12: Hooked IDXGIFactory4 vtable[10] and vtable[15]");
        }
        pFactory4->Release();
    } else {
        HookLog("DX12: IDXGIFactory4 not available");
    }

    IDXGIFactory6* pFactory6 = nullptr;
    if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory6)))) {
        void** vtable6 = *(void***)pFactory6;
        HookLog("DX12: IDXGIFactory6 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable6, vtable,
                (int)(vtable6 == vtable));
        if (vtable6 != vtable) {  // Different vtable pointer
            VTableHook::Create(&vtable6[10], (LPVOID)DetourCreateSwapChainGlobal, nullptr);
            VTableHook::Create(&vtable6[15], (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
            HookLog("DX12: Hooked IDXGIFactory6 vtable[10] and vtable[15]");
        }
        pFactory6->Release();
    } else {
        HookLog("DX12: IDXGIFactory6 not available");
    }

    // Install inline hook on CreateSwapChainForHwnd in dxgi.dll.
    // VTable hooks only patch a single vtable and miss calls through
    // Streamline's SL proxy factory (different COM vtable). Inline hooks
    // patch the actual function code and catch ALL callers.
    if (realCreateSCForHwndAddr && !s_oCreateSCForHwndInline) {
        void* trampoline = nullptr;
        if (InlineHook::Install(realCreateSCForHwndAddr, (void*)DetourCreateSwapChainForHwndInline, &trampoline)) {
            s_oCreateSCForHwndInline = (PFN_CreateSwapChainForHwnd)trampoline;
            HookLog("DX12: Installed INLINE hook on CreateSwapChainForHwnd at %p", realCreateSCForHwndAddr);
        } else {
            HookLog("DX12: FAILED to install inline hook on CreateSwapChainForHwnd");
        }
    }

    // Install DEEP hook on CreateSwapChainForHwnd.
    // When Streamline hooks CreateSwapChainForHwnd at byte 0 and uses a saved
    // trampoline for internal calls (bypassing both our vtable and inline hooks),
    // the deep hook patches the function body past Streamline's JMP so ALL
    // callers are intercepted — including Streamline's linkSwapchainToCmdQueue.
    // The full wrapper pre-releases stale swapchains AND post-tracks new ones,
    // ensuring SL's shadow swapchains are tracked for subsequent releases.
    if (realCreateSCForHwndAddr) {
        void* trampoline = InlineHook::InstallDeepHook(realCreateSCForHwndAddr, (void*)DeepHookCreateSwapChainForHwnd);
        if (trampoline) {
            s_deepHookTrampoline = (PFN_CreateSwapChainForHwnd)trampoline;
            HookLog("DX12: Installed DEEP hook on CreateSwapChainForHwnd at %p (trampoline=%p)",
                    realCreateSCForHwndAddr, trampoline);
        } else {
            HookLog("DX12: Deep hook not needed or failed for CreateSwapChainForHwnd");
        }
    }

    HookLog("DX12: Global factory vtable hooks installed");
}

void RemoveGlobalVTableHooks() {
    // Remove deep hook first (patches function body past external JMP)
    if (s_realCreateSCForHwndAddr) {
        InlineHook::RemoveDeepHook(s_realCreateSCForHwndAddr);
        s_realCreateSCForHwndAddr = nullptr;
        s_deepHookTrampoline = nullptr;
    }

    // Remove inline CreateSwapChainForHwnd hook
    if (s_oCreateSCForHwndInline) {
        // InlineHook::RemoveAll() is called from dxgi_shared.cpp during shutdown,
        // but we also null our trampoline pointer to prevent use-after-free.
        s_oCreateSCForHwndInline = nullptr;
        HookLog("DX12: Cleared inline CreateSwapChainForHwnd hook trampoline");
    }

    // Clear tracked swapchains (no Release needed — we don't AddRef tracked SCs)
    {
        std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
        s_hwndSwapchainMap.clear();
    }
    {
        for (const auto& entry : s_hwndSwapchainMap) {
            for (IDXGISwapChain* swapchain : entry.second) {
                DXGIShared::DX12_UnregisterThirdPartyOverlaySwapchain(swapchain);
            }
        }
    }

    if (!oCreateSwapChainGlobal && !oCreateSwapChainForHwndGlobal) {
        return;
    }

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        HookLog("DX12: DXGI module not loaded, skipping vtable hook removal");
        return;
    }

    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateFactory) {
        HookLog("DX12: CreateDXGIFactory1 not found for vtable hook removal");
        return;
    }

    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        HookLog("DX12: Failed to create factory for vtable hook removal");
        return;
    }

    void** vtable = *(void***)pFactory;

    if (oCreateSwapChainGlobal) {
        VTableHook::Remove(&vtable[10], (void*)oCreateSwapChainGlobal);
        HookLog("DX12: Removed CreateSwapChain vtable hook");
        oCreateSwapChainGlobal = nullptr;
    }

    if (oCreateSwapChainForHwndGlobal) {
        VTableHook::Remove(&vtable[15], (void*)oCreateSwapChainForHwndGlobal);
        HookLog("DX12: Removed CreateSwapChainForHwnd vtable hook");
        oCreateSwapChainForHwndGlobal = nullptr;
    }

    pFactory->Release();
    HookLog("DX12: Global factory vtable hooks removed");
}

// Install Present vtable hooks for pre-existing swapchains (late injection)
// DISABLED: Global Present vtable hooks cause shutdown crashes
// Factory wrapping is now the primary mechanism for intercepting swapchains
void DX12_InstallPresentHooksForSwapchain(IDXGISwapChain* pSwapChain) {
    // DISABLED: Present vtable hooks are disabled to prevent crashes
    // Pre-existing swapchains (created before injection) won't have overlay
