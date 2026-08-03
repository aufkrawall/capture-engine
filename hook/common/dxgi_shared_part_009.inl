                                if (s_vtableModifiedLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                                    HookLogImportant(
                                        "CallOriginalPresent: non-SL Steam — vtable[8] was "
                                        "modified during Steam invoke (current=%p, expected=%p)",
                                        (void*)s_hookedVTable[8], (void*)presentOriginal);
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
                    (void*)presentBypass, s_steamDX12InitAttempted.load(std::memory_order_acquire) ? 1 : 0,
                    s_steamInitCrashed ? 1 : 0);
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
                            slLoaded, oPresent);
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
            const bool steamOverlay = IsSteamOverlayModule(overlayModule);
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
            if (IsSteamOverlayModule(overlayModule)) {
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
        PFN_Present comTarget = s_originalVtable8Present ? s_originalVtable8Present : presentOriginal;
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

HRESULT CallOriginalPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                             const DXGI_PRESENT_PARAMETERS* pParams) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    WaitBackbufferFrameLatency(pSwapChain);
    const PFN_Present1 present1Trampoline = oPresent1Trampoline;
    const PFN_Present1 present1Original = oPresent1;
    const PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
    bool slLoaded = IsSLInterposerLoaded();

    // Inline-hook path: trampoline always bypasses the detour safely.
    if (present1Trampoline) {
        return present1Trampoline(pSwapChain, SyncInterval, Flags, pParams);
    }

    const char* forcedBypassOverlay = nullptr;
    if (ShouldForceSteamDX12Bypass(pSwapChain, present1Bypass != nullptr, slLoaded, &forcedBypassOverlay)) {
        if (slLoaded) {
            // SL case: use bypass trampoline (same as before, no Present1 guard available).
            static int s_forcedBypass1LogCount = 0;
            if (s_forcedBypass1LogCount++ < 10) {
                HookLogImportant("CallOriginalPresent1: forcing DXGI bypass for %s (slLoaded=%d)",
                                 forcedBypassOverlay ? forcedBypassOverlay : "overlay", slLoaded ? 1 : 0);
            }
        } else {
            // Non-Streamline case (e.g. Strange Brigade DX12 with only Steam overlay):
            // Same root cause as CallOriginalPresent: Steam's OverlayHookD3D3
            // needs vtable[8] = dxgi!Present to initialize.  The init is handled
            // by CallOriginalPresent on the first Present call.  For Present1,
            // only route through oPresent1 if Steam init has been completed;
            // otherwise use the bypass trampoline (safe fallback).
            //
            // Steam does NOT hook Present1 with an E9 JMP, so calling
            // present1Original directly on an already-initialized Steam is safe.
            if (!s_steamInitCrashed && s_steamDX12InitAttempted.load(std::memory_order_acquire) && present1Original &&
                present1Original != DetourPresent1 && IsReadableMemory(pSwapChain, sizeof(void*))) {
                static std::atomic<int> s_steamNonSLPresent1ViaE9JmpCount{0};
                if (s_steamNonSLPresent1ViaE9JmpCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                    HookLogImportant(
                        "CallOriginalPresent1: routing non-SL Steam overlay through "
                        "present1Original at %p (Steam init done)",
                        (void*)present1Original);
                }
                return present1Original(pSwapChain, SyncInterval, Flags, pParams);
            }

            static std::atomic<int> s_steamNonSLPresent1FallbackCount{0};
            if (s_steamNonSLPresent1FallbackCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                HookLogImportant(
                    "CallOriginalPresent1: non-SL Steam overlay — Present1 bypass "
                    "(initAttempted=%d initCrashed=%d)",
                    s_steamDX12InitAttempted.load(std::memory_order_acquire) ? 1 : 0, s_steamInitCrashed ? 1 : 0);
            }
        }
        return present1Bypass(pSwapChain, SyncInterval, Flags, pParams);
    }

    const char* thirdPartyBypassOverlay = nullptr;
    if (ShouldForceThirdPartyOverlayBypass(pSwapChain, present1Bypass != nullptr, &thirdPartyBypassOverlay)) {
        static int s_wrapperBypassLogCount = 0;
        if (s_wrapperBypassLogCount++ < 10) {
            HookLogImportant("CallOriginalPresent1: forcing bypass for wrapped present under overlay %s",
                             thirdPartyBypassOverlay);
        }
        return present1Bypass(pSwapChain, SyncInterval, Flags, pParams);
    }

    // CRITICAL: SL worker thread guard — same as CallOriginalPresent.
    // When SL is loaded, call oPresent1 directly (same reason as Present).
    if (slLoaded && present1Original && present1Original != DetourPresent1) {
        return present1Original(pSwapChain, SyncInterval, Flags, pParams);
    }

    // Prefer the current object's Present1 slot when it is not detoured.
    if (IsReadableMemory(pSwapChain, sizeof(void*))) {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(reinterpret_cast<const void*>(vtable), 23 * sizeof(void*)) && vtable[22]) {
            auto currentPresent1 = reinterpret_cast<PFN_Present1>(vtable[22]);
            if (currentPresent1 != DetourPresent1) {
                return currentPresent1(pSwapChain, SyncInterval, Flags, pParams);
            }
        }
    }

    // Vtable-hook path fallback: use saved original only if it is not detoured.
    if (present1Original && present1Original != DetourPresent1) {
        return present1Original(pSwapChain, SyncInterval, Flags, pParams);
    }

    // Last resort: fall back to Present.
    return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
}

void DisableSLPresentRouting() {
    bool wasActive = s_slRoutingActive.exchange(false, std::memory_order_acq_rel);
    if (wasActive) {
        HookLogImportant(
            "SL routing DISABLED: Present calls will bypass SL hook chain and "
            "go through trampoline=%p directly (FSR FG or runtime-owned FG takeover)",
            oPresentTrampoline);
    }
}

}  // namespace DXGIShared
