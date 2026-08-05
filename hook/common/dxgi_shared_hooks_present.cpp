#include "dxgi_shared_internal.h"

namespace DXGIShared {
bool InstallPresentInlineHooks(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return false;

    InstallSetColorSpace1InlineHook(pSwapChain, "present-bootstrap");

    void* presentAddr = GetPresentAddress(pSwapChain);
    void* present1Addr = GetPresent1Address(pSwapChain);

    if (!presentAddr) {
        HookLog("InstallPresentInlineHooks: Failed to get Present address");
        return false;
    }

    // Save original vtable[8] before any modifications. This captures the real
    // COM method (dxgi!CDXGISwapChain::Present or equivalent) from the temp
    // swapchain, before CE patches it to DetourPresent. Used later in
    // CallOriginalPresent and AttemptSteamDX12OverlayInit to ensure DXGI COM
    // method state management runs before dxgi!Present is called with Steam's
    // E9 JMP.
    {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(reinterpret_cast<const void*>(&vtable[8]), sizeof(void*))) {
            if (!dxgi_shared_s_originalVtable8Present) {
                dxgi_shared_s_originalVtable8Present = (PFN_Present)vtable[8];
                // Log the saved address and compare with GetPresentAddress
                HookLogImportant(
                    "InstallPresentInlineHooks: Saved s_originalVtable8Present=%p from temp swapchain %p "
                    "(presentAddr=%p, same=%d)",
                    (void*)dxgi_shared_s_originalVtable8Present, (void*)pSwapChain, presentAddr,
                    dxgi_shared_s_originalVtable8Present == (PFN_Present)presentAddr ? 1 : 0);
                // Log which module presentAddr belongs to for debugging
                HMODULE hAddrModule = nullptr;
                if (GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)presentAddr, &hAddrModule)) {
                    char modulePath[MAX_PATH] = {};
                    GetModuleFileNameA(hAddrModule, modulePath, sizeof(modulePath));
                    HookLogImportant("InstallPresentInlineHooks: presentAddr=%p is in module: %s", presentAddr,
                                     modulePath[0] ? modulePath : "(unknown)");
                }
            }
        } else {
            HookLog("InstallPresentInlineHooks: Cannot read vtable[8] from temp swapchain %p", (void*)pSwapChain);
        }
    }

    static bool s_inlineHooksInstalled = false;
    if (s_inlineHooksInstalled) {
        HookLog("InstallPresentInlineHooks: Inline hooks already installed");
        return true;
    }

    // CRITICAL: Check if an external overlay has already hooked Present
    // External overlays (NVIDIA, Steam, Discord, etc.) actively re-hook Present
    // Fighting them causes a hook war that corrupts the call chain
    const uint8_t* code = (const uint8_t*)presentAddr;
    bool externalJmpDetected = false;

    if (code[0] == 0xE9) {
        // JMP rel32 detected - check where it points
        int32_t disp;
        memcpy(&disp, code + 1, 4);
        uintptr_t jumpTarget = (uintptr_t)(code + 5) + disp;

        // Check if the jump target is outside dxgi.dll
        HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
        if (hDXGI) {
            MODULEINFO dxgiInfo;
            if (GetModuleInformation(GetCurrentProcess(), hDXGI, &dxgiInfo, sizeof(dxgiInfo))) {
                uintptr_t dxgiStart = (uintptr_t)hDXGI;
                uintptr_t dxgiEnd = dxgiStart + dxgiInfo.SizeOfImage;

                if (jumpTarget < dxgiStart || jumpTarget >= dxgiEnd) {
                    externalJmpDetected = true;
                    // JMP points outside dxgi.dll - external overlay detected
                    HookLog("InstallPresentInlineHooks: External overlay detected!");
                    HookLog("InstallPresentInlineHooks: JMP at %p targets %p (outside dxgi.dll %p-%p)", presentAddr,
                            (void*)jumpTarget, (void*)dxgiStart, (void*)dxgiEnd);

                    // Check if the target module is a known overlay
                    HMODULE hTargetModule = nullptr;
                    GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)jumpTarget, &hTargetModule);

                    if (hTargetModule) {
                        char moduleName[MAX_PATH] = {0};
                        GetModuleFileNameA(hTargetModule, moduleName, MAX_PATH);
                        HookLog("InstallPresentInlineHooks: External overlay module: %s", moduleName);

                        // Known overlay modules that we should cooperate with
                        std::string moduleLower = moduleName;
                        std::transform(moduleLower.begin(), moduleLower.end(), moduleLower.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                        if (moduleLower.find("nvidia") != std::string::npos ||
                            moduleLower.find("nvngx") != std::string::npos ||
                            moduleLower.find("steam") != std::string::npos ||
                            moduleLower.find("gameoverlay") != std::string::npos ||
                            moduleLower.find("discord") != std::string::npos ||
                            moduleLower.find("overlay") != std::string::npos) {
                            HookLog("InstallPresentInlineHooks: External hook detected - cooperating (known overlay)");
                        }
                    } else {
                        // Skip inline hooks to prevent prologue corruption (black screen fix)
                        HookLog("InstallPresentInlineHooks: Unknown external JMP - possibly stale hook");
                    }
                }
            }
        }
    }

    if (externalJmpDetected) {
#ifdef _WIN64
        constexpr bool kRequiresBypassTrampolineOnInstall = false;
#else
        constexpr bool kRequiresBypassTrampolineOnInstall = true;
#endif

        void* presentBypass = InlineHook::CreateBypassTrampoline(presentAddr);
        if (!presentBypass) {
            HookLog("InstallPresentInlineHooks: WARNING - Failed to create Present bypass trampoline");
            if (!CanSafelyInstallExternalPresentDetourPath(kRequiresBypassTrampolineOnInstall, false)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: External Present hook detected but no bypass trampoline is available - "
                    "skipping DXGI Present detour path");
                return false;
            }
        }

        void* present1Bypass = nullptr;
        if (present1Addr) {
            present1Bypass = InlineHook::CreateBypassTrampoline(present1Addr);
            if (!present1Bypass &&
                !CanSafelyInstallExternalPresentDetourPath(kRequiresBypassTrampolineOnInstall, false)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: External Present1 hook detected but no bypass trampoline is available "
                    "- skipping DXGI Present detour path");
                return false;
            }
        }

        HookLogImportant("InstallPresentInlineHooks: External E9 JMP detected — using vtable hook path");
        // Save the external overlay hook target (Steam's OverlayHookD3D3) so we
        // can invoke it explicitly later when SL FG routing bypasses Steam's JMP.
        // This is done BEFORE SL overwrites the JMP with its own.
        // If the JMP target is not resolved (e.g. non-E9 JMP or unknown pattern),
        // g_externalOverlayPresentHook stays NULL and Steam overlay will not
        // be explicitly invoked on the forced-bypass path — the overlay module
        // must hook a different Present entry point (e.g. vtable[8] or Present1).
        {
            void* hookTarget = ResolveE9JmpTarget(presentAddr);
            if (hookTarget) {
                dxgi_shared_g_externalOverlayPresentHook = (PFN_Present)hookTarget;
                HookLog("InstallPresentInlineHooks: External E9 JMP target = %p (saved, Steam overlay hook available)",
                        hookTarget);
            } else {
                HookLogImportant(
                    "InstallPresentInlineHooks: Could not resolve E9 JMP target at %p "
                    "(bytes: %02X %02X %02X %02X %02X) — external overlay hook not saved",
                    presentAddr, ((const uint8_t*)presentAddr)[0], ((const uint8_t*)presentAddr)[1],
                    ((const uint8_t*)presentAddr)[2], ((const uint8_t*)presentAddr)[3],
                    ((const uint8_t*)presentAddr)[4]);
            }
        }

        // External overlay (e.g. Streamline) has hooked Present with an E9 JMP.
        // DO NOT inline-hook the external detour — patching 14 bytes of the
        // external function's prologue corrupts its internal state and crashes
        // under Frame Generation (where more code paths are exercised).
        //
        // Instead, use a clean vtable hook:
        //   Game calls Present → vtable[8] → DetourPresent (our overlay) →
        //   CallOriginalPresent → oPresent (the SL-hooked function) →
        //   SL E9 JMP → SL processes FG normally → real Present
        //
        // Also create bypass trampolines from the original disk bytes so that
        // re-entrant Present calls (from SL's vtable callback) can call the real
        // DXGI Present without re-entering the external E9 JMP hook chain.

        void** vtable = *(void***)pSwapChain;
        if (!vtable) {
            HookLog("InstallPresentInlineHooks: External JMP detected but vtable is null");
            s_inlineHooksInstalled = true;
            return true;
        }

        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(vtable), 23 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            HookLog("InstallPresentInlineHooks: VirtualProtect failed for vtable hook");
            s_inlineHooksInstalled = true;
            return true;
        }

        dxgi_shared_s_hookedVTable = vtable;

        // === STEAM DX12 OVERLAY PRE-INITIALIZATION ===
        //
        // Steam's OverlayHookD3D3 lazily initializes internal Present-shaped
        // callback slots during its first E9 JMP entry. CE's vtable hook
        // (setting vtable[8] = DetourPresent below) prevents this natural
        // initialization because Steam's E9 JMP never fires on the game's
        // Present calls — they go through DetourPresent instead of dxgi!Present.
        //
        // Best-effort pre-init: Call Present on the temp swapchain through the
        // REAL dxgi!Present (vtable[8] is still unhooked at this point) BEFORE
        // installing our vtable hook.  This initializes Steam's "next" handler
        // but does NOT initialize every real game-swapchain callback slot
        // (the 2x2 hidden-window temp swapchain causes Steam to skip full init).
        //
        // The actual fix for the NULL rendering callback is the VEH-protected
        // call in AttemptSteamDX12OverlayInit (see below), which patches the
        // NULL pointer at crash time and lets Steam continue.
        //
        // Thread safety: InstallPresentInlineHooks runs once on the hook thread.
        // The temp swapchain is valid and vtable page is already writable
        // (from VirtualProtect at line ~2843).
            const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
            if (overlayModule && IsSteamOverlayModule(overlayModule)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: Pre-initializing Steam overlay on temp swapchain %p "
                    "(vtable[8]=%p = dxgi!Present, before CE vtable hook)",
                    pSwapChain, (void*)vtable[8]);

                PFN_Present realPresent = (PFN_Present)vtable[8];
                HRESULT steamInitHr = realPresent(pSwapChain, 0, 0);

                HookLogImportant(
                    "InstallPresentInlineHooks: Steam overlay pre-init on temp swapchain "
                    "returned hr=0x%08X — proceeding with vtable hook installation",
                    (unsigned)steamInitHr);
        }
        // === END STEAM PRE-INIT ===

        dxgi_shared_oPresent = (PFN_Present)vtable[8];
        vtable[8] = (void*)DetourPresent;
        HookLogImportant(
            "InstallPresentInlineHooks: VTable hook on Present (original=%p, vtable=%p) — "
            "external E9 JMP detected, using non-invasive hook for FG compat",
            dxgi_shared_oPresent, vtable);

        if (presentBypass) {
            dxgi_shared_oPresentBypass = (PFN_Present)presentBypass;
            HookLog("InstallPresentInlineHooks: Present bypass trampoline created at %p", presentBypass);
        }

        if (present1Addr) {
            dxgi_shared_oPresent1 = (PFN_Present1)vtable[22];
            vtable[22] = (void*)DetourPresent1;
            HookLog("InstallPresentInlineHooks: VTable hook on Present1 (original=%p)", dxgi_shared_oPresent1);

            if (present1Bypass) {
                dxgi_shared_oPresent1Bypass = (PFN_Present1)present1Bypass;
                HookLog("InstallPresentInlineHooks: Present1 bypass trampoline created at %p", present1Bypass);
            }
        }

        VirtualProtect(reinterpret_cast<void*>(vtable), 23 * sizeof(void*), oldProtect, &oldProtect);

        s_inlineHooksInstalled = true;
        return true;
    }


    void* presentTrampoline = nullptr;
    if (!InlineHook::Install(presentAddr, (void*)DetourPresent, &presentTrampoline)) {
        HookLog("InstallPresentInlineHooks: Failed to install Present inline hook");
        return false;
    }
    dxgi_shared_oPresentTrampoline = (PFN_Present)presentTrampoline;
    dxgi_shared_oPresent = dxgi_shared_oPresentTrampoline;
    HookLogImportant(
        "InstallPresentInlineHooks: Present INLINE hook installed (addr=%p, "
        "trampoline=%p) — s_hookedVTable remains %p",
        presentAddr, presentTrampoline, dxgi_shared_s_hookedVTable);

    if (present1Addr) {
        void* present1Trampoline = nullptr;
        if (InlineHook::Install(present1Addr, (void*)DetourPresent1, &present1Trampoline)) {
            dxgi_shared_oPresent1Trampoline = (PFN_Present1)present1Trampoline;
            dxgi_shared_oPresent1 = dxgi_shared_oPresent1Trampoline;
            HookLog(
                "InstallPresentInlineHooks: Present1 inline hook installed "
                "(addr=%p, trampoline=%p)",
                present1Addr, present1Trampoline);
        }
    }

    s_inlineHooksInstalled = true;
    return true;
}
}

namespace DXGIShared {
void RemovePresentHooks() {
    InlineHook::RemoveAll();
    dxgi_shared_oSetColorSpace1Trampoline.store(nullptr, std::memory_order_release);

    dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
    dxgi_shared_oPresentBypass = nullptr;
    dxgi_shared_oPresent1Bypass = nullptr;

    if (!dxgi_shared_s_hookedVTable)
        return;

    DWORD oldProtect;
    if (dxgi_shared_oPresent && dxgi_shared_s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[8] = (void*)dxgi_shared_oPresent;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (dxgi_shared_oPresent1 && dxgi_shared_s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[22] = (void*)dxgi_shared_oPresent1;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }

}
}

namespace DXGIShared {
void ReleaseSwapchainPresentVTableHooksForRuntimeHandoff(const char* reason) {
    if (!dxgi_shared_s_hookedVTable) {
        return;
    }
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 23 * sizeof(void*))) {
        HookLogImportant(
            "DXGIShared: Cannot release Present vtable hooks for runtime handoff; vtable %p is not readable "
            "(reason=%s)",
            dxgi_shared_s_hookedVTable, reason ? reason : "unknown");
        return;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    DWORD oldProtect = 0;
    bool restoredPresent = false;
    bool restoredPresent1 = false;

    if (dxgi_shared_oPresent && dxgi_shared_s_hookedVTable[8] == (void*)DetourPresent &&
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        dxgi_shared_s_hookedVTable[8] = (void*)dxgi_shared_oPresent;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
        restoredPresent = true;
    }

    if (dxgi_shared_oPresent1 && dxgi_shared_s_hookedVTable[22] == (void*)DetourPresent1 &&
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        dxgi_shared_s_hookedVTable[22] = (void*)dxgi_shared_oPresent1;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), oldProtect, &oldProtect);
        restoredPresent1 = true;
    }

    if (restoredPresent || restoredPresent1) {
        HookLogImportant(
            "DXGIShared: Released swapchain Present vtable hooks for runtime handoff "
            "(present=%d present1=%d vtable=%p restored8=%p restored22=%p reason=%s)",
            restoredPresent ? 1 : 0, restoredPresent1 ? 1 : 0, dxgi_shared_s_hookedVTable,
            restoredPresent ? (void*)dxgi_shared_oPresent : dxgi_shared_s_hookedVTable[8],
            restoredPresent1 ? (void*)dxgi_shared_oPresent1 : dxgi_shared_s_hookedVTable[22], reason ? reason : "unknown");
        dxgi_shared_s_hookedVTable = nullptr;
        dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
        dxgi_shared_oPresentBypass = nullptr;
        dxgi_shared_oPresent1Bypass = nullptr;
    }
}
}

namespace DXGIShared {
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

    if (!dxgi_shared_s_hookedVTable) {
        static std::atomic<uint32_t> s_nullLogCount{0};
        if (s_nullLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable is NULL, cannot repair");
        }
        return;
    }
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 23 * sizeof(void*))) {
        HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable %p not readable", dxgi_shared_s_hookedVTable);
        return;
    }

    bool repaired = false;
    DWORD oldProtect;

    // Check Present hook at vtable[8]
    if (dxgi_shared_s_hookedVTable[8] != (void*)DetourPresent) {
        HookLogImportant("DXGIShared: vtable[8] OVERWRITTEN! was=%p expected=%p — re-hooking", dxgi_shared_s_hookedVTable[8],
                         (void*)DetourPresent);
        dxgi_shared_oPresent = (PFN_Present)dxgi_shared_s_hookedVTable[8];
        if (VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            dxgi_shared_s_hookedVTable[8] = (void*)DetourPresent;
            VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
            repaired = true;
            HookLogImportant("DXGIShared: vtable[8] re-hooked (new oPresent=%p)", dxgi_shared_oPresent);
        }
    }

    // Check Present1 hook at vtable[22]
    if (dxgi_shared_s_hookedVTable[22] != (void*)DetourPresent1) {
        HookLogImportant("DXGIShared: vtable[22] OVERWRITTEN! was=%p expected=%p — re-hooking", dxgi_shared_s_hookedVTable[22],
                         (void*)DetourPresent1);
        dxgi_shared_oPresent1 = (PFN_Present1)dxgi_shared_s_hookedVTable[22];
        if (VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            dxgi_shared_s_hookedVTable[22] = (void*)DetourPresent1;
            VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), oldProtect, &oldProtect);
            repaired = true;
            HookLogImportant("DXGIShared: vtable[22] re-hooked (new oPresent1=%p)", dxgi_shared_oPresent1);
        }
    }

    static std::atomic<uint32_t> s_intactLogCount{0};
    if (repaired) {
        s_intactLogCount.store(0, std::memory_order_relaxed);
    } else {
        if (s_intactLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTableHooksIfNeeded — hooks intact (vtable=%p, [8]=%p, [22]=%p)",
                             dxgi_shared_s_hookedVTable, dxgi_shared_s_hookedVTable[8], dxgi_shared_s_hookedVTable[22]);
        }
    }
}
}

namespace DXGIShared {
void RemoveSwapchainVTableHooks() {
    InlineHook::RemoveAll();
    dxgi_shared_oSetColorSpace1Trampoline.store(nullptr, std::memory_order_release);

    dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
    dxgi_shared_oPresentBypass = nullptr;
    dxgi_shared_oPresent1Bypass = nullptr;

    if (!dxgi_shared_s_hookedVTable)
        return;

    DWORD oldProtect;

    if (dxgi_shared_oPresent && dxgi_shared_s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[8] = (void*)dxgi_shared_oPresent;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (dxgi_shared_oPresent1 && dxgi_shared_s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[22] = (void*)dxgi_shared_oPresent1;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }

    if (dxgi_shared_oResizeBuffers && dxgi_shared_s_hookedVTable[13] == (void*)DetourResizeBuffers) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[13]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[13] = (void*)dxgi_shared_oResizeBuffers;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[13]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers vtable hook");
    }

    if (dxgi_shared_oResizeBuffers1 && dxgi_shared_s_hookedVTable[39] == (void*)DetourResizeBuffers1) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[39]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[39] = (void*)dxgi_shared_oResizeBuffers1;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[39]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers1 vtable hook");
    }

    dxgi_shared_s_hookedVTable = nullptr;
    HookLog("DXGIShared: All swapchain vtable hooks removed");
}
}
