#include "dxgi_shared_internal.h"
#include "../wrappers/vtable_hook_policy.h"

namespace {
enum class VTableDetachResult {
    Detached,
    ForeignPreserved,
    Failed,
};

struct PresentTrampolinePublication {
    PFN_Present fallback = nullptr;
};

struct Present1TrampolinePublication {
    PFN_Present1 fallback = nullptr;
};

void PublishPresentTrampoline(void* trampoline, void* context) {
    auto* publication = static_cast<PresentTrampolinePublication*>(context);
    if (trampoline) {
        DXGIShared::dxgi_shared_oPresent = reinterpret_cast<PFN_Present>(trampoline);
        MemoryBarrier();
        DXGIShared::dxgi_shared_oPresentTrampoline = reinterpret_cast<PFN_Present>(trampoline);
    } else {
        DXGIShared::dxgi_shared_oPresentTrampoline = nullptr;
        MemoryBarrier();
        DXGIShared::dxgi_shared_oPresent = publication->fallback;
    }
}

void PublishPresent1Trampoline(void* trampoline, void* context) {
    auto* publication = static_cast<Present1TrampolinePublication*>(context);
    if (trampoline) {
        DXGIShared::dxgi_shared_oPresent1 = reinterpret_cast<PFN_Present1>(trampoline);
        MemoryBarrier();
        DXGIShared::dxgi_shared_oPresent1Trampoline = reinterpret_cast<PFN_Present1>(trampoline);
    } else {
        DXGIShared::dxgi_shared_oPresent1Trampoline = nullptr;
        MemoryBarrier();
        DXGIShared::dxgi_shared_oPresent1 = publication->fallback;
    }
}

VTableDetachResult DetachOwnedVTableSlot(void** entry, void* detour, void* predecessor, const char* method) {
    // The hooked swapchain vtable is the class vftable inside the DXGI image,
    // whose page stays read-only between repair/detach operations. `lock
    // cmpxchg` requires write access even when it is only used as a read, so
    // observe with a plain volatile read; the atomic exchange below runs
    // inside the VirtualProtect region.
    void* current = *reinterpret_cast<void* volatile*>(entry);
    if (predecessor && current == predecessor)
        return VTableDetachResult::Detached;
    if (current != detour) {
        if (!predecessor)
            return VTableDetachResult::Detached;
        HookLogImportant("DXGIShared: Preserving foreign %s vtable replacement %p during detach", method, current);
        return VTableDetachResult::ForeignPreserved;
    }
    if (!predecessor) {
        HookLogImportant("DXGIShared: Cannot detach %s vtable hook without its predecessor", method);
        return VTableDetachResult::Failed;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), PAGE_READWRITE, &oldProtect))
        return VTableDetachResult::Failed;
    void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(entry), predecessor, detour);
    DWORD ignoredProtect = 0;
    VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), oldProtect, &ignoredProtect);
    if (replaced != detour) {
        HookLogImportant("DXGIShared: Preserving concurrent foreign %s vtable replacement %p during detach", method,
                         replaced);
        return VTableDetachResult::ForeignPreserved;
    }

    HookLog("DXGIShared: Detached %s vtable hook", method);
    return VTableDetachResult::Detached;
}

bool IsDetached(VTableDetachResult result) {
    return result == VTableDetachResult::Detached;
}
}

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

    // Record the real function-entry addresses CE may prepend over. They are stable per
    // process (dxgi!Present / dxgi!Present1), so the first call wins. Consumed only by
    // MaybeTransitionPresentEntryToForeignChainForWrappedRuntimeSwapchain for the
    // ownership-checked un-prepend.
    if (!dxgi_shared_s_presentEntryAddress) {
        dxgi_shared_s_presentEntryAddress = presentAddr;
    }
    if (!dxgi_shared_s_present1EntryAddress && present1Addr) {
        dxgi_shared_s_present1EntryAddress = present1Addr;
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
    const bool entryUsesE9 = code[0] == 0xE9;
#ifdef _WIN64
    const bool entryUsesFF25 = code[0] == 0xFF && code[1] == 0x25;
#else
    const bool entryUsesFF25 = false;
#endif
    void* externalEntryTarget =
        entryUsesE9 ? ResolveE9JmpTarget(presentAddr)
                    : (entryUsesFF25 ? ResolveFF25JmpTarget(presentAddr) : nullptr);

    if (externalEntryTarget) {
        // Check if the jump target is outside dxgi.dll. Both the conventional
        // relative jump and the x64 indirect jump used by Detours are preserved.
        HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
        if (hDXGI) {
            MODULEINFO dxgiInfo;
            if (GetModuleInformation(GetCurrentProcess(), hDXGI, &dxgiInfo, sizeof(dxgiInfo))) {
                uintptr_t dxgiStart = (uintptr_t)hDXGI;
                uintptr_t dxgiEnd = dxgiStart + dxgiInfo.SizeOfImage;
                const uintptr_t jumpTarget = reinterpret_cast<uintptr_t>(externalEntryTarget);

                if (jumpTarget < dxgiStart || jumpTarget >= dxgiEnd) {
                    externalJmpDetected = true;
                    HookLog("InstallPresentInlineHooks: External overlay detected!");
                    HookLog("InstallPresentInlineHooks: %s at %p targets %p (outside dxgi.dll %p-%p)",
                            entryUsesE9 ? "E9" : "FF25", presentAddr, externalEntryTarget, (void*)dxgiStart,
                            (void*)dxgiEnd);

                    HMODULE hTargetModule = nullptr;
                    GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>(externalEntryTarget), &hTargetModule);

                    if (hTargetModule) {
                        char moduleName[MAX_PATH] = {0};
                        GetModuleFileNameA(hTargetModule, moduleName, MAX_PATH);
                        HookLog("InstallPresentInlineHooks: External overlay module: %s", moduleName);
                    } else {
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
        } else {
            dxgi_shared_oPresentBypass = (PFN_Present)presentBypass;
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
            if (present1Bypass)
                dxgi_shared_oPresent1Bypass = (PFN_Present1)present1Bypass;
        }

        // Save the external overlay hook target (Steam's OverlayHookD3D3) so we
        // can invoke it explicitly later when SL FG routing bypasses Steam's JMP.
        // This is done BEFORE SL overwrites the JMP with its own.
        // If the JMP target is not resolved (e.g. non-E9 JMP or unknown pattern),
        // g_externalOverlayPresentHook stays NULL and Steam overlay will not
        // be explicitly invoked on the forced-bypass path — the overlay module
        // must hook a different Present entry point (e.g. vtable[8] or Present1).
        {
            void* hookTarget = externalEntryTarget;
            if (hookTarget) {
                dxgi_shared_g_externalOverlayPresentHook = (PFN_Present)hookTarget;
                HookLog("InstallPresentInlineHooks: External %s target = %p (saved for guarded overlay routing)",
                        entryUsesE9 ? "E9" : "FF25", hookTarget);
                char hookOwnerPath[MAX_PATH] = {};
                if (ResolveExternalPresentHookOwnerPath(hookTarget, hookOwnerPath, sizeof(hookOwnerPath))) {
                    HookLogImportant("InstallPresentInlineHooks: External hook owner: %s (thunk resolved)", hookOwnerPath);
                } else {
                    HookLogImportant(
                        "InstallPresentInlineHooks: External hook owner: <unresolved thunk> "
                        "(lastLoadedOverlay=%s loadedOverlay=%s)",
                        ce::overlay_compat::GetLastLoadedTrackedOverlayModuleName()
                            ? ce::overlay_compat::GetLastLoadedTrackedOverlayModuleName()
                            : "none",
                        ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName()
                            ? ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName()
                            : "none");
                }
            } else {
                HookLogImportant(
                    "InstallPresentInlineHooks: Could not resolve external JMP target at %p "
                    "(bytes: %02X %02X %02X %02X %02X) — external overlay hook not saved",
                    presentAddr, ((const uint8_t*)presentAddr)[0], ((const uint8_t*)presentAddr)[1],
                    ((const uint8_t*)presentAddr)[2], ((const uint8_t*)presentAddr)[3],
                    ((const uint8_t*)presentAddr)[4]);
            }
        }
        // Two or more foreign overlays sharing this entry cannot survive a third
        // participant: each of them restores/re-installs those same bytes, and whichever one
        // (re-)hooks while CE's prepend is live records CE as its "next" and drops the other
        // overlay out of the chain. Leave the entry alone and intercept through
        // CWrapDXGISwapChain, which no byte patcher can observe.
        const size_t loadedOverlayCount =
            ce::overlay_compat::CountLoadedTrackedOverlayModules(ce::overlay_compat::TrackedOverlaySubset::kOverlay);
        const bool frameGenerationInterposerLoaded =
            ce::overlay_compat::IsStreamlineInterposerModuleLoaded() || g_FGCompat.IsNvPresentLoaded();
        if (ce::overlay_compat::ShouldLeavePresentEntryToForeignOverlayChain(true, loadedOverlayCount,
                                                                            frameGenerationInterposerLoaded)) {
            dxgi_shared_s_presentEntryLeftToForeignChain.store(true, std::memory_order_release);
            dxgi_shared_oPresent = (PFN_Present)presentAddr;
            if (present1Addr) {
                dxgi_shared_oPresent1 = (PFN_Present1)present1Addr;
            }
            HookLogImportant(
                "InstallPresentInlineHooks: %zu third-party overlays already share the Present entry "
                "(%s at %p -> %p, loadedOverlay=%s lastLoadedOverlay=%s) — CE stays out of the entry patch chain "
                "and intercepts through its swapchain wrapper",
                loadedOverlayCount, entryUsesE9 ? "E9" : "FF25", presentAddr, externalEntryTarget,
                ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName()
                    ? ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName()
                    : "none",
                ce::overlay_compat::GetLastLoadedTrackedOverlayModuleName()
                    ? ce::overlay_compat::GetLastLoadedTrackedOverlayModuleName()
                    : "none");
            s_inlineHooksInstalled = true;
            return true;
        }

        HookLogImportant(
            "InstallPresentInlineHooks: External %s detected — prepending CE at the original entry and "
            "forwarding through the exact foreign target (loadedOverlays=%zu fgInterposer=%d)",
            entryUsesE9 ? "E9" : "FF25", loadedOverlayCount, frameGenerationInterposerLoaded ? 1 : 0);
    }


    PresentTrampolinePublication presentPublication{dxgi_shared_oPresent};
    void* presentTrampoline = nullptr;
    if (!InlineHook::InstallPublished(presentAddr, (void*)DetourPresent, &presentTrampoline,
                                      PublishPresentTrampoline, &presentPublication)) {
        HookLog("InstallPresentInlineHooks: Failed to install Present inline hook");
        return false;
    }
    HookLogImportant(
        "InstallPresentInlineHooks: Present INLINE hook installed (addr=%p, "
        "trampoline=%p) — s_hookedVTable remains %p",
        presentAddr, presentTrampoline, dxgi_shared_s_hookedVTable);

    if (present1Addr) {
        Present1TrampolinePublication present1Publication{dxgi_shared_oPresent1};
        void* present1Trampoline = nullptr;
        if (InlineHook::InstallPublished(present1Addr, (void*)DetourPresent1, &present1Trampoline,
                                         PublishPresent1Trampoline, &present1Publication)) {
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

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (!dxgi_shared_s_hookedVTable)
        return;
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 23 * sizeof(void*)))
        return;
    DetachOwnedVTableSlot(&dxgi_shared_s_hookedVTable[8], (void*)DetourPresent, (void*)dxgi_shared_oPresent,
                          "Present");
    DetachOwnedVTableSlot(&dxgi_shared_s_hookedVTable[22], (void*)DetourPresent1, (void*)dxgi_shared_oPresent1,
                          "Present1");

}
}

namespace DXGIShared {
void ReleaseSwapchainPresentVTableHooksForRuntimeHandoff(const char* reason) {
    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (!dxgi_shared_s_hookedVTable) {
        return;
    }
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 40 * sizeof(void*))) {
        HookLogImportant(
            "DXGIShared: Cannot release Present vtable hooks for runtime handoff; vtable %p is not readable "
            "(reason=%s)",
            dxgi_shared_s_hookedVTable, reason ? reason : "unknown");
        return;
    }

    const VTableDetachResult presentResult = DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[8], (void*)DetourPresent, (void*)dxgi_shared_oPresent, "Present");
    const VTableDetachResult present1Result = DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[22], (void*)DetourPresent1, (void*)dxgi_shared_oPresent1, "Present1");
    const bool restoredPresent = IsDetached(presentResult);
    const bool restoredPresent1 = IsDetached(present1Result);
    const bool resizeChainRetained =
        dxgi_shared_oResizeBuffers && dxgi_shared_s_hookedVTable[13] != (void*)dxgi_shared_oResizeBuffers;
    const bool resize1ChainRetained =
        dxgi_shared_oResizeBuffers1 && dxgi_shared_s_hookedVTable[39] != (void*)dxgi_shared_oResizeBuffers1;

    if (restoredPresent || restoredPresent1) {
        HookLogImportant(
            "DXGIShared: Released swapchain Present vtable hooks for runtime handoff "
            "(present=%d present1=%d vtable=%p restored8=%p restored22=%p reason=%s)",
            restoredPresent ? 1 : 0, restoredPresent1 ? 1 : 0, dxgi_shared_s_hookedVTable,
            restoredPresent ? (void*)dxgi_shared_oPresent : dxgi_shared_s_hookedVTable[8],
            restoredPresent1 ? (void*)dxgi_shared_oPresent1 : dxgi_shared_s_hookedVTable[22], reason ? reason : "unknown");
        if (restoredPresent && restoredPresent1 && !resizeChainRetained && !resize1ChainRetained) {
            dxgi_shared_s_hookedVTable = nullptr;
            dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
            dxgi_shared_oPresentBypass = nullptr;
            dxgi_shared_oPresent1Bypass = nullptr;
        } else {
            HookLogImportant(
                "DXGIShared: Retaining vtable chain state because a foreign Present replacement may still call CE");
        }
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

    std::lock_guard<std::mutex> lock(g_SharedMutex);
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
    bool foreignPreserved = false;
    static std::atomic<uint32_t> s_foreignPreserveLogCount{0};
    const auto repairRestoredSlot = [&](void** entry, void* detour, void* predecessor, const char* method) {
        if (!predecessor)
            return false;
        // Same read-only class-vftable constraint as DetachOwnedVTableSlot:
        // observation must be a plain volatile read, never a locked operation,
        // which would fault on the read-only page before VirtualProtect runs.
        void* current = *reinterpret_cast<void* volatile*>(entry);
        if (current == detour)
            return false;
        if (ce::vtable_hook_policy::ShouldPreserveForeignFollower(current, detour, predecessor)) {
            foreignPreserved = true;
            const uint32_t occurrence = s_foreignPreserveLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (occurrence <= 3 || occurrence % 300 == 0) {
                HookLogImportant(
                    "DXGIShared: Preserving foreign %s vtable replacement %p; it may retain CE as its next link "
                    "(occurrence=%u)",
                    method, current, occurrence);
            }
            return false;
        }
        if (!ce::vtable_hook_policy::ShouldReclaimRestoredSlot(current, detour, predecessor))
            return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), PAGE_READWRITE, &oldProtect))
            return false;
        void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(entry), detour,
                                                           predecessor);
        DWORD ignoredProtect = 0;
        VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), oldProtect, &ignoredProtect);
        if (replaced != predecessor) {
            foreignPreserved = true;
            HookLogImportant("DXGIShared: Preserved concurrent %s replacement %p during repair", method, replaced);
            return false;
        }
        HookLogImportant("DXGIShared: Reclaimed %s slot after predecessor restoration (predecessor=%p)", method,
                         predecessor);
        return true;
    };

    repaired |= repairRestoredSlot(&dxgi_shared_s_hookedVTable[8], (void*)DetourPresent,
                                   (void*)dxgi_shared_oPresent, "Present");
    repaired |= repairRestoredSlot(&dxgi_shared_s_hookedVTable[22], (void*)DetourPresent1,
                                   (void*)dxgi_shared_oPresent1, "Present1");

    static std::atomic<uint32_t> s_intactLogCount{0};
    if (repaired) {
        s_intactLogCount.store(0, std::memory_order_relaxed);
    } else if (!foreignPreserved) {
        if (s_intactLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTableHooksIfNeeded — hooks intact (vtable=%p, [8]=%p, [22]=%p)",
                             dxgi_shared_s_hookedVTable, dxgi_shared_s_hookedVTable[8], dxgi_shared_s_hookedVTable[22]);
        }
    }
}
}

namespace {
// Restore CE's own Present/Present1 claim on `vtable` back to the slot's original value.
// Used only when leaving the entry to a multi-overlay foreign chain after the FG runtime
// swapchain got wrapped: the leave-entry invariant requires the swapchain class vftable to
// stay pristine (Steam resolves its own "next Present" from vtable[8], so a CE detour there
// re-inserts CE into exactly the chain the mode exists to stay out of). Foreign replacements
// are preserved untouched — CE never overwrites bytes it does not own.
bool DetachPresentVTableSlotsForForeignChain(void** vtable, void* originalPresent, void* originalPresent1,
                                             const char* source) {
    if (!vtable || !originalPresent) {
        return false;
    }
    const auto detachSlot = [source](void** entry, void* detour, void* original, const char* method) {
        void* current = *reinterpret_cast<void* volatile*>(entry);
        if (current == original) {
            return true;  // already pristine
        }
        if (current != detour) {
            HookLogImportant(
                "DXGIShared: Preserving foreign %s vtable replacement %p during leave-entry detach "
                "(source=%s)",
                method, current, source ? source : "runtime wrap");
            return true;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            return false;
        }
        void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(entry), original,
                                                           detour);
        DWORD ignoredProtect = 0;
        VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), oldProtect, &ignoredProtect);
        if (replaced != detour) {
            HookLogImportant(
                "DXGIShared: Preserving concurrent foreign %s vtable replacement %p during leave-entry detach "
                "(source=%s)",
                method, replaced, source ? source : "runtime wrap");
            return false;
        }
        HookLog("DXGIShared: Restored pristine %s vtable slot %p (source=%s)", method, original,
                source ? source : "runtime wrap");
        return true;
    };
    const bool presentRestored =
        detachSlot(&vtable[8], (void*)DXGIShared::DetourPresent, originalPresent, "Present");
    const bool present1Restored = originalPresent1
                                      ? detachSlot(&vtable[22], (void*)DXGIShared::DetourPresent1, originalPresent1,
                                                   "Present1")
                                      : true;
    return presentRestored && present1Restored;
}
}  // namespace

namespace DXGIShared {
void MaybeTransitionPresentEntryToForeignChainForWrappedRuntimeSwapchain(IDXGISwapChain* pRealSwapChain,
                                                                         const char* source) {
    if (dxgi_shared_s_presentEntryLeftToForeignChain.load(std::memory_order_acquire)) {
        return;  // already left (install-time leave-entry mode)
    }
    const size_t loadedOverlayCount =
        ce::overlay_compat::CountLoadedTrackedOverlayModules(ce::overlay_compat::TrackedOverlaySubset::kOverlay);
    const bool frameGenerationInterposerLoaded =
        ce::overlay_compat::IsStreamlineInterposerModuleLoaded() || g_FGCompat.IsNvPresentLoaded();
    if (!ce::overlay_compat::ShouldLeavePresentEntryToForeignOverlayChain(
            /*foreignEntryJumpDetected=*/true, loadedOverlayCount, frameGenerationInterposerLoaded,
            /*hasNonEntryRuntimePresentView=*/true)) {
        static std::atomic<int> s_keepEntryAfterWrapLogCount{0};
        const int logCount = s_keepEntryAfterWrapLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 500) == 0) {
            HookLogImportant(
                "DXGIShared: Keeping CE Present entry hook after wrapped FG runtime swapchain "
                "(loadedOverlays=%zu fgInterposer=%d source=%s) — wrapper delegates to the detour hook "
                "when present routing needs the entry",
                loadedOverlayCount, frameGenerationInterposerLoaded ? 1 : 0, source ? source : "runtime wrap");
        }
        return;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (dxgi_shared_s_presentEntryLeftToForeignChain.load(std::memory_order_acquire)) {
        return;
    }
    if (!dxgi_shared_s_presentEntryAddress) {
        return;
    }
    const bool entryPatchIntact =
        InlineHook::IsInstalledEntryPatchIntact(dxgi_shared_s_presentEntryAddress, nullptr);
    if (!ce::overlay_compat::ShouldLeavePresentEntryAfterRuntimeSwapchainWrap(
            entryPatchIntact, /*foreignEntryJumpDetected=*/true, loadedOverlayCount,
            frameGenerationInterposerLoaded, /*alreadyLeftToForeignChain=*/false)) {
        if (!entryPatchIntact) {
            HookLogImportant(
                "DXGIShared: Cannot leave the Present entry after wrapped FG runtime swapchain — a foreign "
                "re-hook already took the entry from CE (source=%s); CE cannot repair the foreign saved chains",
                source ? source : "runtime wrap");
        }
        return;
    }

    // Restore the runtime swapchain's own vtable slot(s) when CE claimed them. The original slot
    // value is what InstallHooks saved as the predecessor (`dxgi_shared_oPresent` at claim time).
    void** claimedVTable = pRealSwapChain ? *reinterpret_cast<void***>(pRealSwapChain) : nullptr;
    const bool claimedThisVTable = claimedVTable && claimedVTable == dxgi_shared_s_hookedVTable;
    void* pristinePresent = nullptr;
    void* pristinePresent1 = nullptr;
    if (claimedThisVTable) {
        pristinePresent = reinterpret_cast<void*>(dxgi_shared_oPresent);
        pristinePresent1 = reinterpret_cast<void*>(dxgi_shared_oPresent1);
        if (!DetachPresentVTableSlotsForForeignChain(claimedVTable, pristinePresent, pristinePresent1, source)) {
            HookLogImportant(
                "DXGIShared: Aborting leave-entry transition — runtime swapchain vtable slot detach did not "
                "fully restore the pristine chain (source=%s)",
                source ? source : "runtime wrap");
            return;
        }
        dxgi_shared_s_hookedVTable = nullptr;
    }

    const bool presentRemoved = InlineHook::Remove(dxgi_shared_s_presentEntryAddress);
    bool present1Removed = true;
    if (dxgi_shared_s_present1EntryAddress) {
        present1Removed = InlineHook::Remove(dxgi_shared_s_present1EntryAddress);
    }
    if (!presentRemoved || !present1Removed) {
        HookLogImportant(
            "DXGIShared: Leave-entry transition aborted — CE no longer owns all Present entry bytes "
            "(present=%d present1=%d source=%s); retaining current chain state",
            presentRemoved ? 1 : 0, present1Removed ? 1 : 0, source ? source : "runtime wrap");
        return;
    }

    // Publish the leave-entry state: forwards must run the live entry (never a trampoline, a
    // saved foreign target, or the DXGI bypass), and the wrapper becomes CE's interception.
    dxgi_shared_s_presentEntryLeftToForeignChain.store(true, std::memory_order_release);
    dxgi_shared_oPresent = reinterpret_cast<PFN_Present>(dxgi_shared_s_presentEntryAddress);
    dxgi_shared_oPresentTrampoline = nullptr;
    if (dxgi_shared_s_present1EntryAddress) {
        dxgi_shared_oPresent1 = reinterpret_cast<PFN_Present1>(dxgi_shared_s_present1EntryAddress);
        dxgi_shared_oPresent1Trampoline = nullptr;
    }
    dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);

    HookLogImportant(
        "DXGIShared: Wrapped FG runtime swapchain %p — CE left the Present entry to the foreign "
        "overlay chain (loadedOverlays=%zu entry=%p present1=%p vtableRestored=%d source=%s); "
        "wrapper-only interception active",
        pRealSwapChain, loadedOverlayCount, dxgi_shared_s_presentEntryAddress,
        dxgi_shared_s_present1EntryAddress, claimedThisVTable ? 1 : 0, source ? source : "runtime wrap");
}
}

namespace DXGIShared {
void RemoveSwapchainVTableHooks() {
    InlineHook::RemoveAll();
    dxgi_shared_oSetColorSpace1Trampoline.store(nullptr, std::memory_order_release);

    dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
    dxgi_shared_oPresentBypass = nullptr;
    dxgi_shared_oPresent1Bypass = nullptr;

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (!dxgi_shared_s_hookedVTable)
        return;
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 40 * sizeof(void*))) {
        HookLogImportant("DXGIShared: Cannot detach unreadable swapchain vtable %p", dxgi_shared_s_hookedVTable);
        return;
    }
    const bool presentDetached = IsDetached(DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[8], (void*)DetourPresent, (void*)dxgi_shared_oPresent, "Present"));
    const bool present1Detached = IsDetached(DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[22], (void*)DetourPresent1, (void*)dxgi_shared_oPresent1, "Present1"));
    const bool resizeDetached = IsDetached(DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[13], (void*)DetourResizeBuffers, (void*)dxgi_shared_oResizeBuffers,
        "ResizeBuffers"));
    const bool resize1Detached = IsDetached(DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[39], (void*)DetourResizeBuffers1, (void*)dxgi_shared_oResizeBuffers1,
        "ResizeBuffers1"));

    if (presentDetached && present1Detached && resizeDetached && resize1Detached) {
        dxgi_shared_s_hookedVTable = nullptr;
        HookLog("DXGIShared: All swapchain vtable hooks detached");
    } else {
        HookLogImportant("DXGIShared: Retaining vtable chain state for foreign followers");
    }
}
}
