
    void* presentTrampoline = nullptr;
    if (!InlineHook::Install(presentAddr, (void*)DetourPresent, &presentTrampoline)) {
        HookLog("InstallPresentInlineHooks: Failed to install Present inline hook");
        return false;
    }
    oPresentTrampoline = (PFN_Present)presentTrampoline;
    oPresent = oPresentTrampoline;
    HookLogImportant(
        "InstallPresentInlineHooks: Present INLINE hook installed (addr=%p, "
        "trampoline=%p) — s_hookedVTable remains %p",
        presentAddr, presentTrampoline, s_hookedVTable);

    if (present1Addr) {
        void* present1Trampoline = nullptr;
        if (InlineHook::Install(present1Addr, (void*)DetourPresent1, &present1Trampoline)) {
            oPresent1Trampoline = (PFN_Present1)present1Trampoline;
            oPresent1 = oPresent1Trampoline;
            HookLog(
                "InstallPresentInlineHooks: Present1 inline hook installed "
                "(addr=%p, trampoline=%p)",
                present1Addr, present1Trampoline);
        }
    }

    s_inlineHooksInstalled = true;
    return true;
}

void RemovePresentHooks() {
    InlineHook::RemoveAll();
    oSetColorSpace1Trampoline.store(nullptr, std::memory_order_release);

    s_slRoutingActive.store(false, std::memory_order_release);
    oPresentBypass = nullptr;
    oPresent1Bypass = nullptr;

    if (!s_hookedVTable)
        return;

    DWORD oldProtect;
    if (oPresent && s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[8] = (void*)oPresent;
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (oPresent1 && s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[22] = (void*)oPresent1;
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }

}

void ReleaseSwapchainPresentVTableHooksForRuntimeHandoff(const char* reason) {
    if (!s_hookedVTable) {
        return;
    }
    if (!IsReadableMemory(s_hookedVTable, 23 * sizeof(void*))) {
        HookLogImportant(
            "DXGIShared: Cannot release Present vtable hooks for runtime handoff; vtable %p is not readable "
            "(reason=%s)",
            s_hookedVTable, reason ? reason : "unknown");
        return;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    DWORD oldProtect = 0;
    bool restoredPresent = false;
    bool restoredPresent1 = false;

    if (oPresent && s_hookedVTable[8] == (void*)DetourPresent &&
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        s_hookedVTable[8] = (void*)oPresent;
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
        restoredPresent = true;
    }

    if (oPresent1 && s_hookedVTable[22] == (void*)DetourPresent1 &&
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        s_hookedVTable[22] = (void*)oPresent1;
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), oldProtect, &oldProtect);
        restoredPresent1 = true;
    }

    if (restoredPresent || restoredPresent1) {
        HookLogImportant(
            "DXGIShared: Released swapchain Present vtable hooks for runtime handoff "
            "(present=%d present1=%d vtable=%p restored8=%p restored22=%p reason=%s)",
            restoredPresent ? 1 : 0, restoredPresent1 ? 1 : 0, s_hookedVTable,
            restoredPresent ? (void*)oPresent : s_hookedVTable[8],
            restoredPresent1 ? (void*)oPresent1 : s_hookedVTable[22], reason ? reason : "unknown");
        s_hookedVTable = nullptr;
        s_slRoutingActive.store(false, std::memory_order_release);
        oPresentBypass = nullptr;
        oPresent1Bypass = nullptr;
    }
}

void RepairVTableHooksIfNeeded() {
    // CRITICAL: Do NOT access the swapchain vtable during Streamline's critical
    // initialization window.  Inside Hooked_slDLSSGGetState (called during
    // sl_common!slGetPluginFunction from SL's DllMain), reading the vtable
    // triggers Steam's overlay hook chain (gameoverlayrenderer64!OverlayHookD3D3)
    // which may still be partially initialized and crash with a null function
    // pointer call (RIP=0, RAX=0).  This guard is state-based (PostSL confirmed
    // rendering) rather than timer-based because SL's background DllMain duration
    // varies and can exceed the startup window timer.
    if (DXGIShared::ShouldDeferVTableRepairDuringStreamlineStartup(
            g_StreamlineFGRunning.load(std::memory_order_acquire), DXGIShared::IsStreamlineStartupHandoffPending(),
            DXGIShared::IsStreamlineStartupTransitionWindowActive(), HookIsPostSLOverlayConfirmedRendering())) {
        return;
    }

    if (!s_hookedVTable) {
        static std::atomic<uint32_t> s_nullLogCount{0};
        if (s_nullLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable is NULL, cannot repair");
        }
        return;
    }
    if (!IsReadableMemory(s_hookedVTable, 23 * sizeof(void*))) {
        HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable %p not readable", s_hookedVTable);
        return;
    }

    bool repaired = false;
    DWORD oldProtect;

    // Check Present hook at vtable[8]
    if (s_hookedVTable[8] != (void*)DetourPresent) {
        HookLogImportant("DXGIShared: vtable[8] OVERWRITTEN! was=%p expected=%p — re-hooking", s_hookedVTable[8],
                         (void*)DetourPresent);
        oPresent = (PFN_Present)s_hookedVTable[8];
        if (VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            s_hookedVTable[8] = (void*)DetourPresent;
            VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
            repaired = true;
            HookLogImportant("DXGIShared: vtable[8] re-hooked (new oPresent=%p)", oPresent);
        }
    }

    // Check Present1 hook at vtable[22]
    if (s_hookedVTable[22] != (void*)DetourPresent1) {
        HookLogImportant("DXGIShared: vtable[22] OVERWRITTEN! was=%p expected=%p — re-hooking", s_hookedVTable[22],
                         (void*)DetourPresent1);
        oPresent1 = (PFN_Present1)s_hookedVTable[22];
        if (VirtualProtect(&s_hookedVTable[22], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            s_hookedVTable[22] = (void*)DetourPresent1;
            VirtualProtect(&s_hookedVTable[22], sizeof(void*), oldProtect, &oldProtect);
            repaired = true;
            HookLogImportant("DXGIShared: vtable[22] re-hooked (new oPresent1=%p)", oPresent1);
        }
    }

    static std::atomic<uint32_t> s_intactLogCount{0};
    if (repaired) {
        s_intactLogCount.store(0, std::memory_order_relaxed);
    } else {
        if (s_intactLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTableHooksIfNeeded — hooks intact (vtable=%p, [8]=%p, [22]=%p)",
                             s_hookedVTable, s_hookedVTable[8], s_hookedVTable[22]);
        }
    }
}

void RemoveSwapchainVTableHooks() {
    InlineHook::RemoveAll();
    oSetColorSpace1Trampoline.store(nullptr, std::memory_order_release);

    s_slRoutingActive.store(false, std::memory_order_release);
    oPresentBypass = nullptr;
    oPresent1Bypass = nullptr;

    if (!s_hookedVTable)
        return;

    DWORD oldProtect;

    if (oPresent && s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[8] = (void*)oPresent;
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (oPresent1 && s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[22] = (void*)oPresent1;
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }

    if (oResizeBuffers && s_hookedVTable[13] == (void*)DetourResizeBuffers) {
        VirtualProtect(&s_hookedVTable[13], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[13] = (void*)oResizeBuffers;
        VirtualProtect(&s_hookedVTable[13], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers vtable hook");
    }

    if (oResizeBuffers1 && s_hookedVTable[39] == (void*)DetourResizeBuffers1) {
        VirtualProtect(&s_hookedVTable[39], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[39] = (void*)oResizeBuffers1;
        VirtualProtect(&s_hookedVTable[39], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers1 vtable hook");
    }

    s_hookedVTable = nullptr;
    HookLog("DXGIShared: All swapchain vtable hooks removed");
}

// SAFETY NET: Attempt one-time Steam DX12 overlay initialization.
//
// The PRIMARY fix (InstallPresentInlineHooks) pre-initializes Steam overlay on
// the temp swapchain BEFORE our vtable hook is installed.  This function is a
// fallback for cases where pre-init didn't occur:
//   - Steam overlay loaded AFTER hook installation
//   - Another thread/process context
//
// It temporarily restores vtable[8] to the real dxgi!Present, calls through
// Steam's E9 JMP, then re-hooks vtable[8] to DetourPresent. If Steam still
// reaches a lazy NULL callback on the real swapchain, the scoped VEH guard
// patches the exact faulting slot to CE's DXGI bypass Present and retries.
//
// Thread safety: only one thread wins the compare-exchange.  The brief window
// where vtable[8] is unhooked is microseconds wide and limited to frame 1.
//
// Returns true if this thread performed the init call (result in *resultOut).
// Returns false if another thread won the init race or if init was skipped.
static bool AttemptSteamDX12OverlayInit(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                        PFN_Present presentOriginal, PFN_Present presentBypass, HRESULT* resultOut) {
    if (!pSwapChain || !resultOut || !s_hookedVTable || !presentOriginal || s_steamInitCrashed) {
        return false;
    }

    if (!IsReadableMemory(s_hookedVTable, 9 * sizeof(void*))) {
        return false;
    }

    // Only one thread wins the init race
    bool expected = false;
    if (!s_steamDX12InitAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
        return false;  // Another thread is already handling init
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: VirtualProtect failed to unhook vtable[8] — will retry on next frame");
        s_steamDX12InitAttempted.store(false, std::memory_order_release);
        return false;
    }

    // Save current vtable[8] (= DetourPresent) and restore to the real dxgi!Present
    void* savedVtable8 = s_hookedVTable[8];
    s_hookedVTable[8] = (void*)presentOriginal;
    VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);

    HookLogImportant(
        "AttemptSteamDX12OverlayInit: vtable[8] temporarily restored to dxgi!Present=%p — "
        "calling through E9 JMP for Steam overlay init (with VEH protection) "
        "[s_originalVtable8Present=%p, same=%d]",
        (void*)presentOriginal, (void*)s_originalVtable8Present, s_originalVtable8Present == presentOriginal ? 1 : 0);

    // Call through oPresent (E9 JMP at dxgi!Present) WITH VEH protection.
    //
    // Steam's OverlayHookD3D3 can still have lazy NULL callback slots on first
    // entry through the E9 JMP on a REAL game swapchain (the temp swapchain pre-
    // init in InstallPresentInlineHooks doesn't trigger full initialization
    // because Steam skips rendering on a 2x2 hidden-window swapchain).
    //
    // The SteamOverlayInitVehHandler catches this specific crash (RIP=0, RAX=0,
    // return address inside gameoverlayrenderer64.dll), patches the exact NULL
    // slot to CE's bypass Present when possible, and retries the `call rax` so
    // Steam completes its initialization and real Present chaining survives.
    //
    // If the crash is NOT the expected NULL callback (e.g. a different Steam bug),
    // the handler returns EXCEPTION_CONTINUE_SEARCH and CE's existing VEH crash
    // handler catches it and writes a crash dump.
    ScopedSteamNullCallbackRecoveryGuard steamInitGuard(true, "non-SL Steam init", "AttemptSteamDX12OverlayInit",
                                                        reinterpret_cast<void*>(presentOriginal),
                                                        reinterpret_cast<void*>(presentBypass), false, false);
    HRESULT initHr = presentOriginal(pSwapChain, SyncInterval, Flags);

    // Re-hook vtable[8] with DetourPresent (our vtable hook)
    if (VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        s_hookedVTable[8] = (void*)DetourPresent;
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
    } else {
        // CRITICAL: VirtualProtect for re-hook failed — vtable[8] is exposed.
        // Our DetourPresent hook may be lost. Log prominently and continue.
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: CRITICAL — VirtualProtect failed to re-hook vtable[8]! "
            "CE overlay may be disabled for this session.");
    }

    // Check what Steam's legacy known callback slot contains after the init call.
    // New Steam builds can use nearby slots too; the VEH log reports the exact
    // dynamically resolved slot when it differs from this legacy address.
    {
        HMODULE steamMod = GetModuleHandleW(L"gameoverlayrenderer64.dll");
        if (steamMod) {
            void** steamCallbackPtr = (void**)((uintptr_t)steamMod + 0x1621d8);
            if (IsReadableMemory(steamCallbackPtr, sizeof(void*))) {
                void* callbackAfterInit = *steamCallbackPtr;
                if (callbackAfterInit != nullptr && callbackAfterInit != (void*)SteamDummyRenderingCallback &&
                    callbackAfterInit != (void*)presentBypass) {
                    HookLogImportant(
                        "AttemptSteamDX12OverlayInit: Steam legacy callback slot contains Steam-owned function %p "
                        "(bypass=%p dummy=%p)",
                        callbackAfterInit, (void*)presentBypass, (void*)SteamDummyRenderingCallback);
                } else {
                    HookLogImportant(
                        "AttemptSteamDX12OverlayInit: Steam legacy callback slot is %s (%p) "
                        "(bypass=%p dummy=%p)",
                        callbackAfterInit == nullptr
                            ? "NULL"
                            : (callbackAfterInit == (void*)presentBypass ? "CE bypass" : "CE dummy"),
                        callbackAfterInit, (void*)presentBypass, (void*)SteamDummyRenderingCallback);
                }
            } else {
                HookLog("AttemptSteamDX12OverlayInit: Cannot read Steam callback pointer (not readable)");
            }
        } else {
            HookLog("AttemptSteamDX12OverlayInit: gameoverlayrenderer64.dll not loaded");
        }
    }

    HookLogImportant(
        "AttemptSteamDX12OverlayInit: Steam overlay init completed (hr=0x%08X) — "
        "vtable[8] re-hooked to DetourPresent.  Subsequent frames will invoke Steam "
        "overlay via g_externalOverlayPresentHook (explicit hook target, bypass trampoline fallback).",
        (unsigned)initHr);

    *resultOut = initHr;
    return true;
}

HRESULT CallOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    // Inline wait for DWM flip queue room (no separate function call)
    {
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            IDXGISwapChain2* pSC2 = nullptr;
            HRESULT hrQI = pSwapChain->QueryInterface(IID_PPV_ARGS(&pSC2));
            if (SUCCEEDED(hrQI) && pSC2) {
                HANDLE hWaitable = pSC2->GetFrameLatencyWaitableObject();
                if (hWaitable && hWaitable != INVALID_HANDLE_VALUE) {
                    WaitForSingleObject(hWaitable, 16);
                }
                pSC2->Release();
            }
        }
    }

    const PFN_Present presentTrampoline = oPresentTrampoline;
    const PFN_Present presentOriginal = oPresent;
    const PFN_Present presentBypass = EnsurePresentBypassTrampoline();
    bool slLoaded = IsSLInterposerLoaded();

    // Inline-hook path: trampoline always bypasses the detour safely.
    if (presentTrampoline) {
        static int s_copLogCount = 0;
        if (s_copLogCount++ < 5) {
            HookLog("CallOriginalPresent: trampoline path=%p", presentTrampoline);
        }
        return presentTrampoline(pSwapChain, SyncInterval, Flags);
    }

    const char* forcedBypassOverlay = nullptr;
    if (ShouldForceSteamDX12Bypass(pSwapChain, presentBypass != nullptr, slLoaded, &forcedBypassOverlay)) {
        // With Streamline loaded but FG not yet running, some Steam hook chains
        // can accept our direct guarded call without advancing the real Present.
        // Use the DXGI bypass until FG owns the chain; once FG is running the
        // guarded path keeps Steam's overlay in the generated-frame path.
        const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        const bool nativeFSRPresentationActive = ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode) ||
                                                 g_FGCompat.IsFSRFGApiActive() ||
                                                 HookHasRuntimeOwnedNativeFGPresentPath();
        if (slLoaded && DXGIShared::ShouldInvokeGuardedSteamPresentDuringForcedBypass(slLoaded, streamlineFGRunning,
                                                                                      nativeFSRPresentationActive)) {
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags, "Steam DX12 forced bypass",
                                                            &guardedSteamHr)) {
                return guardedSteamHr;
            }
        } else if (slLoaded) {
            static std::atomic<int> s_streamlineLoadedSteamBypassOnlyLogCount{0};
            const int bypassOnlyCount =
                s_streamlineLoadedSteamBypassOnlyLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassOnlyCount <= 10 || (bypassOnlyCount % 500) == 0) {
                HookLogImportant(
                    "CallOriginalPresent: Streamline loaded but FG is not running; using DXGI bypass without "
                    "direct Steam hook invoke #%d (overlay=%s bypass=%p runtime=%s nativeFSR=%d)",
                    bypassOnlyCount, forcedBypassOverlay ? forcedBypassOverlay : "Steam", (void*)presentBypass,
                    ce::fg_runtime::GetRuntimeModeName(runtimeMode), nativeFSRPresentationActive ? 1 : 0);
            }
        } else {
            // Non-Streamline case (e.g. Strange Brigade DX12 with only Steam overlay):
            //
            // Steam's OverlayHookD3D3 can still have lazy NULL callback slots
            // after hidden temp-swapchain pre-init; Steam only reaches all of
            // them when rendering on a real game swapchain.
            //
            // AttemptSteamDX12OverlayInit handles the real fix: it temporarily
            // restores vtable[8] to dxgi!Present and calls Steam's E9 JMP with
            // VEH protection.  The VEH handler (SteamOverlayInitVehHandler)
            // catches a NULL callback crash, patches the exact faulting slot to
            // CE's DXGI bypass Present when possible, and retries the call so
            // Steam can keep its "next Present" chain alive.
            //
            // After init, subsequent frames keep the same guarded E9 JMP route;
            // if Steam exposes a new lazy NULL slot, that frame repairs it too.

            // Phase A: One-time Steam DX12 overlay initialization.
            // If pre-init didn't happen (unusual), AttemptSteamDX12OverlayInit
            // handles it here. Only one thread wins the race; losers bypass.
            if (!s_steamInitCrashed) {
                const bool needInit = !s_steamDX12InitAttempted.load(std::memory_order_acquire);
                if (needInit) {
                    HRESULT initHr = S_OK;
                    if (AttemptSteamDX12OverlayInit(pSwapChain, SyncInterval, Flags, presentOriginal, presentBypass,
                                                    &initHr)) {
                        // This thread performed init successfully
                        return initHr;
                    }
                    // Race loser or init failed — use bypass for this frame.
                    // Next frame will check s_steamDX12InitAttempted and (if another
                    // thread's init succeeded) use the E9 JMP path.
                    static std::atomic<int> s_steamNonSLInitWaitCount{0};
                    if (s_steamNonSLInitWaitCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                        HookLogImportant(
                            "CallOriginalPresent: non-SL Steam overlay — init race loser, "
                            "using bypass trampoline for this frame");
                    }
                } else {
                    // Init already attempted (by another thread or a prior frame).
                    // Steam's overlay init completed successfully (VEH handled the
                    // lazy NULL callback slot).
                    //
                    // == STEAM OVERLAY INVOCATION (non-SL path) ==
                    //
                    // Strategy: Temporarily restore vtable[8] to the original dxgi!Present
                    // (which has Steam's E9 JMP) before invoking Steam's overlay handler.
                    //
                    // Why: Steam's DX12 overlay handler (gameoverlayrenderer64!OverlayHookD3D3)
                    // may internally call pSwapChain->Present() as part of its hook chain
                    // protocol (e.g. for post-overlay fence wait and Present sequencing).
                    // When vtable[8] = DetourPresent (CE's hook), such internal Present calls
                    // re-enter DetourPresent → recursive bypass → the Present skips Steam's
                    // E9 JMP chain entirely, and Steam's "next" handler never fires.
                    // By temporarily restoring vtable[8] to dxgi!Present, Steam's internal
                    // Present flows through the natural E9 JMP → Steam handler (re-entrant)
                    // → Steam's saved "next" → real Present body, correctly completing
                    // both overlay rendering and buffer presentation.
                    //
                    // After Steam's handler returns, re-hook vtable[8] to DetourPresent
                    // to restore CE's overlay hook for the next frame.
                    //
                    // Fallback: If TryInvokeGuardedExternalSteamOverlayPresent declines or
                    // fails, fall back to the bypass trampoline (game content + CE overlay,
                    // no Steam overlay).  This preserves a working game/CE session even
                    // when Steam overlay cannot be rendered.
                    bool vtableRestored = false;
                    bool steamInvoked = false;
                    HRESULT steamHr = S_OK;

                    // Phase A: Temporarily restore vtable[8] from DetourPresent to original
                    // Present function.  This lets Steam's internal Present calls flow
                    // through the natural E9 JMP hook chain instead of re-entering CE.
                    bool needVtableRestore = false;
                    void* savedVtable8 = nullptr;
                    if (s_hookedVTable && IsReadableMemory(s_hookedVTable, 9 * sizeof(void*))) {
                        savedVtable8 = s_hookedVTable[8];
                        if (savedVtable8 == (void*)DetourPresent && presentOriginal &&
                            presentOriginal != (PFN_Present)DetourPresent) {
                            DWORD oldProtect = 0;
                            if (VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                                s_hookedVTable[8] = (void*)presentOriginal;
                                VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
                                needVtableRestore = true;
                                vtableRestored = true;
                                static std::atomic<int> s_vtableRestoreLogCount{0};
                                if (s_vtableRestoreLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                                    HookLogImportant(
                                        "CallOriginalPresent: non-SL Steam — temp vtable[8]=%p "
                                        "(was DetourPresent) for Steam overlay invoke",
                                        (void*)presentOriginal);
                                }
                            } else {
                                static std::atomic<int> s_vtableRestoreFailCount{0};
                                if (s_vtableRestoreFailCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                                    HookLogImportant(
                                        "CallOriginalPresent: non-SL Steam — VirtualProtect failed "
                                        "for vtable[8] restore, proceeding without vtable restore");
                                }
                            }
                        }
                    }

                    // Phase B: Diagnostic — log state before Steam invoke.
                    // Check: E9 JMP at dxgi!Present integrity, swapchain buffer index,
                    // swapchain description, Present call counter.
                    UINT bbIdxBefore = UINT_MAX;
                    UINT bbCountBefore = 0;
                    DXGI_SWAP_CHAIN_DESC scDescBefore = {};
                    {
                        IDXGISwapChain3* sc3 = nullptr;
                        if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                            bbIdxBefore = sc3->GetCurrentBackBufferIndex();
                            sc3->Release();
                        }
                        if (SUCCEEDED(pSwapChain->GetDesc(&scDescBefore))) {
                            bbCountBefore = scDescBefore.BufferCount;
                        }
                    }
                    // Check E9 JMP integrity at presentOriginal (= dxgi!Present)
                    const void* presentOrigPtr = (const void*)presentOriginal;
                    uint8_t presentBytes[5] = {};
                    bool e9JmpIntact = false;
                    if (presentOrigPtr && IsReadableMemory(presentOrigPtr, 5)) {
                        memcpy(presentBytes, presentOrigPtr, 5);
                        if (presentBytes[0] == 0xE9) {
                            int32_t relOffset;
                            memcpy(&relOffset, presentBytes + 1, sizeof(relOffset));
                            void* resolvedTarget = (uint8_t*)presentOrigPtr + 5 + relOffset;
                            e9JmpIntact = (resolvedTarget == (void*)g_externalOverlayPresentHook);
                        }
                    }
                    uint64_t presentCallCount = g_SharedState.presentCallCount.load(std::memory_order_relaxed);
                    static std::atomic<int> s_steamNonSLDiagCount{0};
                    int diagNum = s_steamNonSLDiagCount.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (diagNum <= 20 || (diagNum % 200) == 0) {
                        HookLogImportant(
                            "CallOriginalPresent: DIAG #%d — bbIdx=%u bbCount=%u presentCalls=%llu "
                            "e9Bytes=%02X%02X%02X%02X%02X e9Intact=%d presentOrig=%p hookTarget=%p "
                            "vtableRestored=%d",
                            diagNum, bbIdxBefore, bbCountBefore, (unsigned long long)presentCallCount, presentBytes[0],
                            presentBytes[1], presentBytes[2], presentBytes[3], presentBytes[4], e9JmpIntact ? 1 : 0,
                            (void*)presentOriginal, (void*)g_externalOverlayPresentHook, vtableRestored ? 1 : 0);
                    }

                    // Phase C: Invoke Steam's overlay handler through the E9 JMP
                    // at presentOriginal (dxgi!Present).  This ensures Steam's
                    // handler fires through the natural hook chain with the correct
                    // return address, so it chains to the original dxgi!Present
                    // after rendering Steam overlay.
                    if (presentOriginal && presentOriginal != (PFN_Present)DetourPresent) {
                        ScopedSteamNullCallbackRecoveryGuard steamInvokeGuard(
                            presentBypass != nullptr, "non-SL Steam Present", "E9 JMP steady Present",
                            reinterpret_cast<void*>(presentOriginal), reinterpret_cast<void*>(presentBypass), false,
                            false);
                        StreamlineHook::ExternalOverlayPresentGuard slGuard;
                        steamHr = presentOriginal(pSwapChain, SyncInterval, Flags);
                        steamInvoked = true;

                        // Diagnostic — log state after Steam invoke.
                        UINT bbIdxAfter = UINT_MAX;
                        DXGI_SWAP_CHAIN_DESC scDescAfter = {};
                        {
                            IDXGISwapChain3* sc3 = nullptr;
                            if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                                bbIdxAfter = sc3->GetCurrentBackBufferIndex();
                                sc3->Release();
                            }
                            pSwapChain->GetDesc(&scDescAfter);
                        }
                        uint8_t presentBytesAfter[5] = {};
                        bool e9IntactAfter = false;
                        const void* presentOrigPtr2 = (const void*)presentOriginal;
                        if (presentOrigPtr2 && IsReadableMemory(presentOrigPtr2, 5)) {
                            memcpy(presentBytesAfter, presentOrigPtr2, 5);
                            if (presentBytesAfter[0] == 0xE9) {
                                int32_t relOffset;
                                memcpy(&relOffset, presentBytesAfter + 1, sizeof(relOffset));
                                void* resolvedTarget = (uint8_t*)presentOrigPtr2 + 5 + relOffset;
                                e9IntactAfter = (resolvedTarget == (void*)g_externalOverlayPresentHook);
                            }
                        }
                        uint64_t presentCallCountAfter = g_SharedState.presentCallCount.load(std::memory_order_relaxed);
                        static std::atomic<int> s_steamNonSLInvokeCount{0};
                        int invokeNum = s_steamNonSLInvokeCount.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (invokeNum <= 20 || (invokeNum % 200) == 0) {
                            HookLogImportant(
                                "CallOriginalPresent: non-SL Steam — E9 JMP invoke #%d "
                                "hr=0x%08X bbIdx=%u->%u bufCount=%u->%u presentCalls=%llu "
                                "e9Intact=%d->%d e9BytesAfter=%02X%02X%02X%02X%02X "
                                "vtableRestored=%d",
                                invokeNum, (unsigned)steamHr, bbIdxBefore, bbIdxAfter, bbCountBefore,
                                scDescAfter.BufferCount, (unsigned long long)presentCallCountAfter, e9JmpIntact ? 1 : 0,
                                e9IntactAfter ? 1 : 0, presentBytesAfter[0], presentBytesAfter[1], presentBytesAfter[2],
                                presentBytesAfter[3], presentBytesAfter[4], vtableRestored ? 1 : 0);
                        }
                        // If the backbuffer index didn't advance, Steam's handler
                        // didn't chain to the original dxgi!Present.  Fall back to
                        // the bypass trampoline to ensure the frame is presented.
                        if (bbIdxAfter == bbIdxBefore && presentBypass) {
                            static std::atomic<int> s_steamE9JMPFallbackCount{0};
                            if (s_steamE9JMPFallbackCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                                HookLogImportant(
                                    "CallOriginalPresent: non-SL Steam — E9 JMP did not advance "
                                    "bbIdx (%u->%u), using bypass trampoline to ensure Present",
                                    bbIdxBefore, bbIdxAfter);
                            }
                            steamHr = presentBypass(pSwapChain, SyncInterval, Flags);
                        }
                    } else {
                        static std::atomic<int> s_steamNonSLDeclineCount{0};
                        int declineNum = s_steamNonSLDeclineCount.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (declineNum <= 20 || (declineNum % 200) == 0) {
                            HookLogImportant(
                                "CallOriginalPresent: non-SL Steam — E9 JMP invoke "
                                "declined #%d (presentOriginal=%p, vtableRestored=%d, presentBypass=%p, tid=0x%04X)",
                                declineNum, (void*)presentOriginal, vtableRestored ? 1 : 0, (void*)presentBypass,
                                GetCurrentThreadId());
                        }
                    }

                    // Phase C: Restore vtable[8] to DetourPresent (CE's hook) AFTER
                    // Steam's handler returns.  This ensures CE's overlay hook is
                    // active for the next frame.
                    if (needVtableRestore && s_hookedVTable && IsReadableMemory(s_hookedVTable, 9 * sizeof(void*))) {
                        DWORD oldProtect = 0;
                        if (VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                            if (s_hookedVTable[8] == (void*)presentOriginal) {
                                s_hookedVTable[8] = (void*)DetourPresent;
                                VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
                                static std::atomic<int> s_vtableRehookLogCount{0};
                                if (s_vtableRehookLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                                    HookLogImportant(
                                        "CallOriginalPresent: non-SL Steam — vtable[8] re-hooked "
                                        "to DetourPresent after Steam invoke (was=%p)",
                                        savedVtable8);
                                }
                            } else {
                                // Another component modified vtable[8] while CE's back was
                                // turned.  Log it but don't force re-hook — the current
                                // vtable[8] may have been deliberately changed by Steam or
                                // another overlay.
                                VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
                                static std::atomic<int> s_vtableModifiedLogCount{0};
