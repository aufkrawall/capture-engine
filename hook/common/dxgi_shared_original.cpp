#include "dxgi_shared_internal.h"

namespace DXGIShared {
HRESULT CallOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (HookIsShuttingDown()) {
        // A deep body hook means this call already came DOWN the foreign chain, so the only
        // remaining work is the real body. Checked before the entry forward below, which
        // would re-run Steam/RTSS and re-enter this same hook forever.
        if (dxgi_shared_oPresentDeepBody) {
            return dxgi_shared_oPresentDeepBody(pSwapChain, SyncInterval, Flags);
        }
        // CE owns no entry bytes in the left-to-foreign-chain mode; the live entry IS the
        // foreign chain, exactly as it would be without CE.
        if (IsPresentEntryLeftToForeignChain() && dxgi_shared_s_originalVtable8Present &&
            dxgi_shared_s_originalVtable8Present != DetourPresent) {
            return dxgi_shared_s_originalVtable8Present(pSwapChain, SyncInterval, Flags);
        }
        // The trampoline can re-enter Steam's chain when CE prepended over its
        // entry jump; during shutdown no VEH recovery is active, so prefer the
        // clean bypass for that transport.
        if (IsSteamExternalChainTrampoline((void*)dxgi_shared_oPresentTrampoline,
                                           (void*)dxgi_shared_g_externalOverlayPresentHook,
                                           DetectAPIType(pSwapChain) == APIType::D3D12) &&
            dxgi_shared_oPresentBypass) {
            return dxgi_shared_oPresentBypass(pSwapChain, SyncInterval, Flags);
        }
        if (dxgi_shared_oPresentTrampoline)
            return dxgi_shared_oPresentTrampoline(pSwapChain, SyncInterval, Flags);
        if (dxgi_shared_oPresent && dxgi_shared_oPresent != DetourPresent)
            return dxgi_shared_oPresent(pSwapChain, SyncInterval, Flags);
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

    // Multi-overlay foreign chain, CE intercepting BELOW it: control reached DetourPresent
    // through the deep body hook, so Steam and RTSS have already drawn above us and the frame
    // still needs the real dxgi!Present body. The deep trampoline is that body. Forwarding
    // through the live entry here instead would re-run the whole foreign chain and re-enter
    // this hook without end.
    if (dxgi_shared_oPresentDeepBody) {
        static std::atomic<int> s_deepBodyForwardCount{0};
        const int forwardNum = s_deepBodyForwardCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (forwardNum <= 5 || (forwardNum % 5000) == 0) {
            HookLogImportant("CallOriginalPresent: foreign-chain deep body forward #%d (trampoline=%p)", forwardNum,
                             (void*)dxgi_shared_oPresentDeepBody);
        }
        return dxgi_shared_oPresentDeepBody(pSwapChain, SyncInterval, Flags);
    }

    // Multi-overlay foreign chain without a deep body hook: CE deliberately owns no bytes at
    // the Present entry, so the only correct forward is the live entry itself. It runs
    // whatever Steam/RTSS have chained there right now, identical to a process without CE.
    // Every other transport below (trampoline, saved foreign target, DXGI bypass) is a
    // snapshot or a shortcut and would drop one of the overlays out of the chain.
    if (IsPresentEntryLeftToForeignChain()) {
        const PFN_Present liveEntry =
            dxgi_shared_s_originalVtable8Present ? dxgi_shared_s_originalVtable8Present : dxgi_shared_oPresent;
        if (liveEntry && liveEntry != DetourPresent) {
            static std::atomic<int> s_foreignChainEntryForwardCount{0};
            const int forwardNum = s_foreignChainEntryForwardCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (forwardNum <= 5 || (forwardNum % 5000) == 0) {
                HookLogImportant("CallOriginalPresent: foreign-chain entry forward #%d (entry=%p)", forwardNum,
                                 (void*)liveEntry);
            }
            return liveEntry(pSwapChain, SyncInterval, Flags);
        }
    }

    const PFN_Present presentTrampoline = dxgi_shared_oPresentTrampoline;
    const PFN_Present presentOriginal = dxgi_shared_oPresent;
    const PFN_Present presentBypass = EnsurePresentBypassTrampoline();
    bool slLoaded = IsSLInterposerLoaded();
    const char* forcedBypassOverlay = nullptr;
    bool isD3D12SteamSwapChain = false;
    const bool forceSteamDX12Bypass = ShouldForceSteamDX12Bypass(
        pSwapChain, presentBypass != nullptr, slLoaded, &forcedBypassOverlay, &isD3D12SteamSwapChain);

    // Steam can be entered explicitly, through its E9 hook in presentOriginal,
    // or through a pre-existing inline chain. Apply the runtime-worker boundary
    // before choosing any of those transports.
    if (presentBypass && IsSteamOverlayModule(forcedBypassOverlay)) {
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
        const bool runtimeCanPresentFromWorker = DXGIShared::CanRuntimePresentFromWorkerForExternalOverlay(
            isD3D12SteamSwapChain, false, streamlineFGRunning, postSLConfirmedRendering,
            ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode), g_FGCompat.IsFSRFGApiActive(),
            HookHasRuntimeOwnedNativeFGPresentPath(), DoesFGRuntimeOwnSwapchain());
        if (runtimeCanPresentFromWorker) {
            const uint32_t currentThreadId = GetCurrentThreadId();
            const uint32_t trackedSourcePresentThreadId = DX12_GetGamePresentThreadId();
            if (!DXGIShared::ShouldInvokeSynchronousExternalOverlayPresentForThreadState(
                    true, trackedSourcePresentThreadId, currentThreadId)) {
                static std::atomic<int> s_steamRuntimeWorkerBypassLogCount{0};
                const int bypassNum = s_steamRuntimeWorkerBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassNum <= 20 || bypassNum == 50 || (bypassNum % 500) == 0) {
                    HookLogImportant(
                        "CallOriginalPresent: refusing Steam Present transport on runtime worker #%d; using DXGI "
                        "bypass (runtime=%s slFG=%d confirmed=%d sourceTid=0x%04X currentTid=0x%04X)",
                        bypassNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode), streamlineFGRunning ? 1 : 0,
                        postSLConfirmedRendering ? 1 : 0, trackedSourcePresentThreadId, currentThreadId);
                }
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        }
    }

    // Inline-hook path: the trampoline bypasses the detour safely, except when
    // CE prepended over Steam's entry jump - then it re-enters Steam's chain,
    // which can fault through lazy NULL callbacks on a fresh swapchain
    // (DLSS->FSR switch, 20260811_195131). Run that transport under the same
    // NULL-callback VEH recovery as the other Steam invokes; the clean DXGI
    // bypass is the fail-closed fallback.
    if (presentTrampoline) {
        // When CE prepended over a foreign entry jump, the trampoline does not hold original
        // code bytes: it re-issues that exact jump. So it inherits the staleness of the target
        // captured at install time, and the owning overlay rebuilds or frees its thunk on its
        // own schedule. Prove the destination is still executable before jumping there, and
        // fall back to the clean DXGI bypass when it is not.
        const bool trampolineChainsToForeignEntry =
            TrampolineChainsToExternalOverlay((void*)presentTrampoline,
                                              (void*)dxgi_shared_g_externalOverlayPresentHook);
        if (trampolineChainsToForeignEntry && presentBypass &&
            !IsCallableForeignPresentHandler((void*)dxgi_shared_g_externalOverlayPresentHook)) {
            const PFN_Present refreshed = RefreshExternalOverlayPresentHookFromLiveEntry();
            static std::atomic<int> s_staleTrampolineLogCount{0};
            const int staleNum = s_staleTrampolineLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (staleNum <= 10 || (staleNum % 500) == 0) {
                HookLogImportant(
                    "CallOriginalPresent: preserved foreign entry jump is no longer callable #%d "
                    "(trampoline=%p refreshed=%p) - %s",
                    staleNum, (void*)presentTrampoline, (void*)refreshed,
                    refreshed ? "forwarding through the refreshed handler" : "using the DXGI bypass");
            }
            if (refreshed) {
                return refreshed(pSwapChain, SyncInterval, Flags);
            }
            return presentBypass(pSwapChain, SyncInterval, Flags);
        }
        if (IsSteamExternalChainTrampoline((void*)presentTrampoline,
                                           (void*)dxgi_shared_g_externalOverlayPresentHook,
                                           DetectAPIType(pSwapChain) == APIType::D3D12)) {
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,
                                                            "Steam trampoline chain", &guardedSteamHr)) {
                return guardedSteamHr;
            }
            return presentBypass(pSwapChain, SyncInterval, Flags);
        }
        // Non-Steam external chain (e.g. RTSS's thunk) with a single foreign overlay: the
        // preserved trampoline re-issues exactly the entry jump CE prepended over, which is
        // what the game itself would have executed. A multi-overlay chain never reaches this
        // branch — InstallPresentInlineHooks leaves that entry unpatched entirely, because no
        // forwarding choice from CE's side can repair a chain the other tools have already
        // re-linked through CE.
        static int s_copLogCount = 0;
        if (s_copLogCount++ < 5) {
            HookLog("CallOriginalPresent: trampoline path=%p", presentTrampoline);
        }
        return presentTrampoline(pSwapChain, SyncInterval, Flags);
    }

    if (forceSteamDX12Bypass) {
        // With Streamline loaded but FG not yet running, some Steam hook chains
        // can accept our direct guarded call without advancing the real Present.
        // Use the DXGI bypass until FG owns the chain. During FG, Steam is
        // serviced only on the verified source Present thread; runtime workers
        // keep using the bypass.
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
            if (!dxgi_shared_s_steamInitCrashed) {
                const bool needInit = !dxgi_shared_s_steamDX12InitAttempted.load(std::memory_order_acquire);
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
                    if (dxgi_shared_s_hookedVTable && IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 9 * sizeof(void*))) {
                        // Plain volatile read: the class vftable page is read-only
                        // outside VirtualProtect regions, and `lock cmpxchg` faults
                        // there even when used only as a read.
                        savedVtable8 = *reinterpret_cast<void* volatile*>(&dxgi_shared_s_hookedVTable[8]);
                        if (savedVtable8 == (void*)DetourPresent && presentOriginal &&
                            presentOriginal != (PFN_Present)DetourPresent) {
                            DWORD oldProtect = 0;
                            if (VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                                void* replaced = InterlockedCompareExchangePointer(
                                    reinterpret_cast<PVOID volatile*>(&dxgi_shared_s_hookedVTable[8]),
                                    (void*)presentOriginal, (void*)DetourPresent);
                                VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
                                if (replaced == (void*)DetourPresent) {
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
                                    HookLogImportant(
                                        "CallOriginalPresent: Preserving concurrent foreign vtable[8]=%p; "
                                        "skipping temporary Steam vtable handoff",
                                        replaced);
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
                            e9JmpIntact = (resolvedTarget == (void*)dxgi_shared_g_externalOverlayPresentHook);
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
                            (void*)presentOriginal, (void*)dxgi_shared_g_externalOverlayPresentHook, vtableRestored ? 1 : 0);
                    }

                    // Own a reference across the foreign call and its post-invoke diagnostics:
                    // an overlay chain can release/recreate the swapchain inside Present (see
                    // the guarded transport in dxgi_shared_steam.cpp, Talos 20260812_032301).
                    pSwapChain->AddRef();
                    auto swapChainLifetimeGuard =
                        ce::make_scope_guard([pSwapChain]() { pSwapChain->Release(); });

                    // Phase C: Invoke Steam's overlay handler through the E9 JMP
                    // at presentOriginal (dxgi!Present).  This ensures Steam's
                    // handler fires through the natural hook chain with the correct
                    // return address, so it chains to the original dxgi!Present
                    // after rendering Steam overlay.
                    if (presentOriginal && presentOriginal != (PFN_Present)DetourPresent) {
                        // No speculative writes into Steam's callback slots: pre-filling a slot
                        // Steam has not initialized makes it skip its own install and chain to a
                        // raw Present, dropping every overlay below Steam. The VEH below recovers
                        // the exact faulting slot if a NULL dispatch still happens.
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
                                e9IntactAfter = (resolvedTarget == (void*)dxgi_shared_g_externalOverlayPresentHook);
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
                    if (needVtableRestore && dxgi_shared_s_hookedVTable && IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 9 * sizeof(void*))) {
                        DWORD oldProtect = 0;
                        if (VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                            void* replaced = InterlockedCompareExchangePointer(
                                reinterpret_cast<PVOID volatile*>(&dxgi_shared_s_hookedVTable[8]),
                                (void*)DetourPresent, (void*)presentOriginal);
                            if (replaced == (void*)presentOriginal) {
                                VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
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
                                VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
                                static std::atomic<int> s_vtableModifiedLogCount{0};

                                if (s_vtableModifiedLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                                    HookLogImportant(
                                        "CallOriginalPresent: non-SL Steam — vtable[8] was "
                                        "modified during Steam invoke (current=%p, expected=%p)",
                                        replaced, (void*)presentOriginal);
                                }
                            }
                        } else {
                            HookLogImportant(
                                "CallOriginalPresent: CRITICAL — VirtualProtect failed to "
                                "re-hook vtable[8] to DetourPresent after Steam invoke!");
                        }
                    }

                    // Phase D: If Steam was successfully invoked, return its HRESULT.
                    if (steamInvoked) {
                        return steamHr;
                    }

                    // Phase E: Fallback — bypass trampoline (safe, preserves game + CE
                    // overlay but no Steam overlay).
                    if (presentBypass) {
                        static std::atomic<int> s_steamNonSLFallbackCount{0};
                        if (s_steamNonSLFallbackCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                            HookLogImportant(
                                "CallOriginalPresent: non-SL Steam overlay — Steam invoke "
                                "declined, using bypass trampoline at %p (presentOriginal=%p, "
                                "init done, tid=0x%04X)",
                                (void*)presentBypass, (void*)presentOriginal, GetCurrentThreadId());
                        }
                        return presentBypass(pSwapChain, SyncInterval, Flags);
                    }
                }
            }

            // Phase B: Bypass trampoline fallback (safe, no Steam overlay rendering).
            static std::atomic<int> s_steamNonSLFallbackCount{0};
            if (s_steamNonSLFallbackCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                HookLogImportant(
                    "CallOriginalPresent: non-SL Steam overlay — bypass trampoline at %p "
                    "(initAttempted=%d initCrashed=%d)",
                    (void*)presentBypass, dxgi_shared_s_steamDX12InitAttempted.load(std::memory_order_acquire) ? 1 : 0,
                    dxgi_shared_s_steamInitCrashed ? 1 : 0);
            }
        }

        static int s_forcedBypassLogCount = 0;
        if (s_forcedBypassLogCount++ < 10) {
            HookLogImportant("CallOriginalPresent: forcing DXGI bypass for %s (slLoaded=%d, bypass=%p)",
                             forcedBypassOverlay ? forcedBypassOverlay : "overlay", slLoaded ? 1 : 0,
                             (void*)presentBypass);
        }
        return presentBypass(pSwapChain, SyncInterval, Flags);
    }

    const char* thirdPartyBypassOverlay = nullptr;
    if (ShouldForceThirdPartyOverlayBypass(pSwapChain, presentBypass != nullptr, &thirdPartyBypassOverlay)) {
        static int s_wrapperBypassLogCount = 0;
        if (s_wrapperBypassLogCount++ < 10) {
            HookLogImportant("CallOriginalPresent: forcing bypass for wrapped present under overlay %s",
                             thirdPartyBypassOverlay);
        }
        return presentBypass(pSwapChain, SyncInterval, Flags);
    }

    // When SL is loaded (vtable hook mode), call oPresent directly.
    // Don't re-read vtable[8] — Steam or other overlays may have re-hooked it
    // after us, which would create a re-entrant loop:
    //   DetourPresent → vtable[8](SteamPresent) → Steam → DetourPresent → ...
    // oPresent = the saved original from vtable[8] at hook install time.
    //
    // SL-originated Steam bypass paths are handled before this fallback by
    // TryInvokeGuardedExternalSteamOverlayPresent. This branch preserves normal
    // vtable-chain behavior for ordinary Present calls.
    if (slLoaded && presentOriginal && presentOriginal != DetourPresent) {
        static std::atomic<int> s_copFastPathCount{0};
        int fastPathNum = s_copFastPathCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fastPathNum <= 10 || (fastPathNum % 1000) == 0) {
            HookLog("CallOriginalPresent: SL fast-path oPresent=%p (#%d, tid=0x%04X)", presentOriginal, fastPathNum,
                    GetCurrentThreadId());
        }

        // When a Steam overlay owns the E9 JMP on dxgi!Present, this transport is
        // the "natural E9" route from the third-party coexistence rules. It gets
        // the same two protections as every other Steam transport:
        //   1. Under an FG runtime that can Present from workers, Steam may only
        //      be touched on the verified source Present thread; unknown/worker
        //      provenance fails closed to the DXGI bypass trampoline.
        //   2. Steam's internal rendering callback can still be NULL on the real
        //      swapchain (the temp-swapchain pre-init initializes Steam's "next"
        //      handler but not the rendering callback), so the E9 call must run
        //      under the NULL-callback VEH recovery which patches the faulting
        //      slot and retries. Without it Steam crashes the render thread with
        //      RIP=0 (RoboCop: Rogue City session 20260809_140551).
        const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
        const bool steamOverlay = IsCurrentExternalPresentHookSteamChain();
        if (steamOverlay && presentBypass) {
            const auto runtimeMode = g_FGCompat.GetRuntimeMode();
            const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
            const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
            const bool isD3D12SwapChain = DetectAPIType(pSwapChain) == APIType::D3D12;
            const bool runtimeCanPresentFromWorker = DXGIShared::CanRuntimePresentFromWorkerForExternalOverlay(
                isD3D12SwapChain, false, streamlineFGRunning, postSLConfirmedRendering,
                ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode), g_FGCompat.IsFSRFGApiActive(),
                HookHasRuntimeOwnedNativeFGPresentPath(), DoesFGRuntimeOwnSwapchain());
            const uint32_t currentThreadId = GetCurrentThreadId();
            const uint32_t trackedSourcePresentThreadId = DX12_GetGamePresentThreadId();
            if (runtimeCanPresentFromWorker &&
                !DXGIShared::ShouldInvokeSynchronousExternalOverlayPresentForThreadState(
                    true, trackedSourcePresentThreadId, currentThreadId)) {
                static std::atomic<int> s_slFastPathWorkerBypassLogCount{0};
                const int bypassNum = s_slFastPathWorkerBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassNum <= 20 || bypassNum == 50 || (bypassNum % 500) == 0) {
                    HookLogImportant(
                        "CallOriginalPresent: SL fast-path refusing Steam Present transport on runtime worker #%d; "
                        "using DXGI bypass (runtime=%s slFG=%d confirmed=%d sourceTid=0x%04X currentTid=0x%04X)",
                        bypassNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode), streamlineFGRunning ? 1 : 0,
                        postSLConfirmedRendering ? 1 : 0, trackedSourcePresentThreadId, currentThreadId);
                }
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }

            // The entry bytes of presentOriginal carry the foreign overlay's live jump, whose
            // target can already be a freed thunk. Prove it is callable before entering it.
            if (!IsCallableForeignPresentHandler(reinterpret_cast<void*>(presentOriginal))) {
                static std::atomic<int> s_slFastPathStaleLogCount{0};
                const int staleNum = s_slFastPathStaleLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (staleNum <= 10 || (staleNum % 500) == 0) {
                    HookLogImportant(
                        "CallOriginalPresent: SL fast-path entry %p forwards to a freed foreign thunk #%d; using the "
                        "DXGI bypass",
                        (void*)presentOriginal, staleNum);
                }
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }

            // No speculative writes into Steam's callback slots here either - see
            // dxgi_shared_steam.cpp. The VEH recovery below resolves the exact faulting slot
            // if Steam still dispatches through NULL.
            ScopedSteamNullCallbackRecoveryGuard steamNullCallbackGuard(
                presentBypass != nullptr, "SL fast-path Steam Present", "SL fast-path E9 transport",
                reinterpret_cast<void*>(presentOriginal), reinterpret_cast<void*>(presentBypass), false, false);
        }

        return presentOriginal(pSwapChain, SyncInterval, Flags);
    }

    // Prefer the current object's vtable entry when it is not detoured.
    // This avoids mixing wrapper and real swapchain original function pointers.
    if (IsReadableMemory(pSwapChain, sizeof(void*))) {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(reinterpret_cast<const void*>(vtable), 9 * sizeof(void*)) && vtable[8]) {
            auto currentPresent = reinterpret_cast<PFN_Present>(vtable[8]);
            if (currentPresent != DetourPresent) {
                static int s_copLogCount3 = 0;
                if (s_copLogCount3++ < 5) {
                    HookLog("CallOriginalPresent: vtable[8] path=%p (slLoaded=%d, oPresent=%p)", currentPresent,
                            slLoaded, dxgi_shared_oPresent);
                }
                return currentPresent(pSwapChain, SyncInterval, Flags);
            }
        }
    }

    // Vtable-hook path fallback: use saved original only if it is not detoured.
    // NOTE: presentOriginal is dxgi!Present which may have an external overlay's
    // E9 JMP installed. Calling through it enters the external overlay hook chain
    // (e.g. Steam's gameoverlayrenderer64). This is safe only when the startup
    // compat pass has been blocked (see ShouldAllowDX12StartupPresentPassForState)
    // so that DetourPresent's full routing logic handles re-entrancy.
    if (presentOriginal && presentOriginal != DetourPresent) {
        static int s_copLogCount4 = 0;
        if (s_copLogCount4++ < 5) {
            const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
            const bool steamOverlay = IsCurrentExternalPresentHookSteamChain();
            HookLogImportant(
                "CallOriginalPresent: fallback oPresent=%p (trampoline=%p bypass=%p slLoaded=%d steamOverlay=%d "
                "overlay=%s)",
                presentOriginal, presentTrampoline, presentBypass, slLoaded, steamOverlay ? 1 : 0,
                overlayModule ? overlayModule : "none");
        }
        // When Steam overlay is loaded without Streamline, calling oPresent
        // (dxgi!Present with Steam's E9 JMP) re-enters Steam's overlay handler
        // which crashes because vtable[8] = DetourPresent. Use the bypass
        // trampoline instead to skip all in-memory hooks.
        if (!slLoaded && presentBypass) {
            const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
            if (IsCurrentExternalPresentHookSteamChain()) {
                static int s_steamNonSLBypassCount = 0;
                if (s_steamNonSLBypassCount++ < 10) {
                    HookLogImportant(
                        "CallOriginalPresent: Steam overlay without Streamline — using bypass trampoline %p instead of "
                        "oPresent %p to avoid Steam NULL-callback crash",
                        presentBypass, presentOriginal);
                }
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        }
        // Use the saved original COM method (s_originalVtable8Present) if available,
        // which ensures DXGI kernel state management runs before dxgi!Present is
        // called internally with Steam's E9 JMP.  Fall back to presentOriginal
        // (= dxgi!Present inner function with E9 JMP) if the COM method wasn't
        // captured (e.g. non-DX12 paths).
        PFN_Present comTarget = dxgi_shared_s_originalVtable8Present ? dxgi_shared_s_originalVtable8Present : presentOriginal;
        return comTarget(pSwapChain, SyncInterval, Flags);
    }

    // Last resort: if the bypass trampoline exists but was skipped
    // (ShouldForceSteamDX12Bypass returned false), try it directly.
    // This handles the case where SL DllMain sets runtime mode to
    // DLSSFG before FG actually starts running, causing the bypass
    // check to fail.
    if (presentBypass) {
        static int s_copBypassFallbackCount = 0;
        if (s_copBypassFallbackCount++ < 10) {
            HookLogImportant(
                "CallOriginalPresent: LAST RESORT bypass trampoline at %p (oPresent=%p, oPresentTrampoline=%p, "
                "slLoaded=%d)",
                presentBypass, presentOriginal, presentTrampoline, slLoaded);
        }
        return presentBypass(pSwapChain, SyncInterval, Flags);
    }

    HookLog("CallOriginalPresent: NO PATH AVAILABLE (oPresent=%p, oPresentTrampoline=%p, slLoaded=%d)", presentOriginal,
            presentTrampoline, slLoaded);
    return DXGI_ERROR_INVALID_CALL;
}
}
