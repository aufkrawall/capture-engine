#include "dxgi_shared_internal.h"

namespace {
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

void PublishDeepPresentBody(void* trampoline, void*) {
    DXGIShared::dxgi_shared_oPresentDeepBody = reinterpret_cast<PFN_Present>(trampoline);
}

void PublishDeepPresent1Body(void* trampoline, void*) {
    DXGIShared::dxgi_shared_oPresent1DeepBody = reinterpret_cast<PFN_Present1>(trampoline);
}

// CE left the Present entry to a chain two or more foreign overlays share, so it owns no
// entry bytes there. Without a second view CE sees nothing at all whenever the presenting
// swapchain was created before injection (WMI process-start latency alone is enough), and
// no wrapper can be attached to it retroactively — the exact state session
// 20260812_140930 recorded: game presenting at ~720 ECL/s, zero CE presents, no overlay.
//
// The view CE takes instead is a deep hook in the function body, past the five entry bytes
// Steam and RTSS keep restoring/re-patching around each other. They never read or write
// those body bytes, and their trampolines resume exactly at the offset CE patches, so the
// foreign chain composes as if CE were absent and CE runs last, below all of them.
//
// Returns true when CE ended up with a Present view. A false return must NOT latch the
// install: thread quiescence can legitimately refuse a body patch once (a peer thread sitting
// inside the displaced range), and a later real swapchain event is a free, timer-free retry —
// far better than running the whole session blind.
//
// `observedPresentEntryPatchSize` is the foreign patch span the caller saw when it decided to
// leave the entry. It has to be passed in: RTSS restores the original entry bytes, calls
// through, and re-patches on every present, so by the time the deep install samples byte 0 it
// can read clean — session 20260812_150918 refused the Present body hook on `byte=0x48`
// milliseconds after the caller had logged the E9 there, and the overlay never appeared
// because Present is the entry the game actually uses.
//
// `prependFallbackAvailable` says the caller can still take the ordinary entry prepend when the
// Present body patch is refused (a single foreign overlay — see
// MayPrependPresentEntryWhenBelowChainViewUnavailable). Present1 is then deliberately NOT
// attempted: a lone Present1 deep trampoline would make IsPresentInterceptedBelowForeignChain()
// true while CE also owns the Present entry bytes, and the two modes contradict each other
// (below the chain CE must never invoke a foreign handler; prepended it must).
bool InstallPresentBodyHooksBelowForeignChain(void* presentAddr, void* present1Addr,
                                              int observedPresentEntryPatchSize,
                                              bool prependFallbackAvailable) {
    using namespace DXGIShared;

    bool haveBodyView = false;
    if (InlineHook::InstallDeepHookPublished(presentAddr, (void*)DetourPresent, PublishDeepPresentBody, nullptr,
                                             observedPresentEntryPatchSize)) {
        // The DXGI bypass built above resumes at exactly the offset the deep hook now owns, so
        // every "skip the foreign entry hook" consumer would land back in CE's own detour. The
        // deep trampoline is the correct clean path: it skips the foreign entry AND CE's patch
        // and continues in the real body.
        dxgi_shared_oPresentBypass = dxgi_shared_oPresentDeepBody;
        haveBodyView = true;
        HookLogImportant(
            "InstallPresentInlineHooks: CE intercepts BELOW the foreign Present chain via a deep body hook "
            "(entry=%p trampoline=%p) — pre-existing swapchains are covered without touching the shared entry",
            presentAddr, (void*)dxgi_shared_oPresentDeepBody);
    } else {
        HookLogImportant(
            "InstallPresentInlineHooks: deep body hook on the foreign-owned Present entry %p FAILED — CE has no "
            "Present view at all unless it wraps the presenting swapchain (a swapchain created before injection "
            "cannot be wrapped retroactively, so the overlay would stay invisible); a later swapchain event retries",
            presentAddr);
    }

    if (!present1Addr || (!haveBodyView && prependFallbackAvailable)) {
        return haveBodyView;
    }

    // Present1 gets the same treatment, and the same sampling caveat applies to its entry: a
    // clean read is not proof that no foreign overlay owns it. Once CE knows the chain is
    // there, the deep body hook is the safe choice for both entries — it works whether or not
    // a foreign patch is present, whereas an ordinary prepend on an entry that turns out to be
    // shared is exactly what this mode exists to avoid.
    if (InlineHook::InstallDeepHookPublished(present1Addr, (void*)DetourPresent1, PublishDeepPresent1Body, nullptr,
                                             observedPresentEntryPatchSize)) {
        dxgi_shared_oPresent1Bypass = dxgi_shared_oPresent1DeepBody;
        HookLogImportant(
            "InstallPresentInlineHooks: Present1 deep body hook installed below the foreign chain "
            "(entry=%p trampoline=%p)",
            present1Addr, (void*)dxgi_shared_oPresent1DeepBody);
    } else {
        HookLogImportant("InstallPresentInlineHooks: Present1 deep body hook at %p failed", present1Addr);
    }
    return haveBodyView;
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

    // Two or more loaded overlay modules is CE's decisive evidence that the Present entry
    // belongs to a foreign chain — not the entry bytes, which are volatile: RTSS restores the
    // original bytes, calls through and re-patches on every present, so a sample taken inside
    // that window reads clean on an entry that is very much hooked (session 20260812_150918
    // caught exactly that gap between the detection and the deep install, milliseconds apart).
    // The module count does not flicker. When CE cannot see the patch it must still assume the
    // widest form it recognizes, so the body hook lands past whatever those tools rewrite.
    const size_t loadedOverlayCount =
        ce::overlay_compat::CountLoadedTrackedOverlayModules(ce::overlay_compat::TrackedOverlaySubset::kOverlay);
    const bool frameGenerationInterposerLoaded =
        ce::overlay_compat::IsStreamlineInterposerModuleLoaded() || g_FGCompat.IsNvPresentLoaded();

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
        // Check if the jump target leaves the module that actually OWNS this Present
        // implementation. Both the conventional relative jump and the x64 indirect jump used by
        // Detours are preserved.
        //
        // The owning module is resolved from presentAddr, never by name: a proxy DLL in the game
        // directory (ReShade, SpecialK, OptiScaler all ship as `dxgi.dll`) is loaded under the
        // same base name as the system image, so GetModuleHandleA("dxgi.dll") returns whichever
        // one the loader lists first and the range test then compared against an unrelated image.
        // Session 20260812_155205 shows exactly that: `E9 at 00007FFD5C049960 targets
        // 00007FFD1C040000 (outside dxgi.dll 00007FFCB8460000-00007FFCB9CF0000)` — presentAddr
        // is not inside the printed range at all, so the verdict was accidental either way.
        HMODULE hPresentModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(presentAddr), &hPresentModule);
        if (hPresentModule) {
            MODULEINFO dxgiInfo;
            if (GetModuleInformation(GetCurrentProcess(), hPresentModule, &dxgiInfo, sizeof(dxgiInfo))) {
                uintptr_t dxgiStart = (uintptr_t)hPresentModule;
                uintptr_t dxgiEnd = dxgiStart + dxgiInfo.SizeOfImage;
                const uintptr_t jumpTarget = reinterpret_cast<uintptr_t>(externalEntryTarget);

                if (jumpTarget < dxgiStart || jumpTarget >= dxgiEnd) {
                    externalJmpDetected = true;
                    char presentModulePath[MAX_PATH] = {};
                    GetModuleFileNameA(hPresentModule, presentModulePath, sizeof(presentModulePath));
                    HookLog("InstallPresentInlineHooks: External overlay detected!");
                    HookLog("InstallPresentInlineHooks: %s at %p targets %p (outside %s %p-%p)",
                            entryUsesE9 ? "E9" : "FF25", presentAddr, externalEntryTarget,
                            presentModulePath[0] ? presentModulePath : "the Present-owning module", (void*)dxgiStart,
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

    // Decided before the bypass machinery below, because that machinery needs a foreign jump to
    // be visible right now and this decision must not: a loaded overlay module owns that entry
    // whether or not this instant's sample shows its patch. Steam hooks Present only once the
    // game's first swapchain exists — seconds after CE's guarded temp swapchain installs its
    // Present hooks — and RTSS restores and re-patches those bytes around every call. Sampling
    // therefore decided nothing but who was first, and CE being first is what breaks the other
    // overlay (Cyberpunk + Steam, 20260816_154722).
    if (ce::overlay_compat::ShouldLeavePresentEntryToForeignOverlayChain(loadedOverlayCount)) {
        const PFN_Present previousPresent = dxgi_shared_oPresent;
        const PFN_Present1 previousPresent1 = dxgi_shared_oPresent1;
        dxgi_shared_s_presentEntryLeftToForeignChain.store(true, std::memory_order_release);
        dxgi_shared_oPresent = (PFN_Present)presentAddr;
        if (present1Addr) {
            dxgi_shared_oPresent1 = (PFN_Present1)present1Addr;
        }
        if (externalEntryTarget) {
            dxgi_shared_g_externalOverlayPresentHook = (PFN_Present)externalEntryTarget;
        }
        // The widest entry-patch form CE recognizes when it cannot see one: placing the body
        // hook deeper than the foreign patch is always safe, placing it inside one never is.
        const int observedEntryPatchSize = entryUsesFF25 ? 14 : (entryUsesE9 ? 5 : 14);
        HookLogImportant(
            "InstallPresentInlineHooks: %zu third-party overlay(s) own the Present entry "
            "(%s at %p -> %p, loadedOverlay=%s lastLoadedOverlay=%s foreignJumpVisibleNow=%d fgInterposer=%d) — CE "
            "stays out of the entry patch chain and intercepts below it, so its overlay composites after theirs",
            loadedOverlayCount, entryUsesE9 ? "E9" : (entryUsesFF25 ? "FF25" : "no visible jump"), presentAddr,
            externalEntryTarget,
            ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName()
                ? ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName()
                : "none",
            ce::overlay_compat::GetLastLoadedTrackedOverlayModuleName()
                ? ce::overlay_compat::GetLastLoadedTrackedOverlayModuleName()
                : "none",
            externalJmpDetected ? 1 : 0, frameGenerationInterposerLoaded ? 1 : 0);
        // The swapchain wrapper alone is not a complete view: it only exists for swapchains CE
        // itself created. Take the deep body hook below the foreign chain so a swapchain that
        // pre-dates injection is covered too. Only a view that was actually obtained latches
        // the install, so a refused body patch is retried by the next real swapchain event
        // instead of blinding the whole session.
        const bool prependFallbackAvailable =
            ce::overlay_compat::MayPrependPresentEntryWhenBelowChainViewUnavailable(loadedOverlayCount);
        const bool haveBodyView = InstallPresentBodyHooksBelowForeignChain(presentAddr, present1Addr,
                                                                          observedEntryPatchSize,
                                                                          prependFallbackAvailable);
        if (haveBodyView || !prependFallbackAvailable) {
            s_inlineHooksInstalled = haveBodyView;
            return true;
        }
        // Single foreign overlay and no body view: the prepend is the historical, validated
        // topology against one overlay and it composes with it, so take it rather than run the
        // session without any Present view. CE then draws BELOW that overlay — draw order is
        // what is lost here, not compatibility. Reverting the published state is safe at this
        // point precisely because no deep trampoline exists: nothing has observed CE as being
        // below the chain, and the entry still carries only the foreign patch.
        dxgi_shared_s_presentEntryLeftToForeignChain.store(false, std::memory_order_release);
        dxgi_shared_oPresent = previousPresent;
        dxgi_shared_oPresent1 = previousPresent1;
        HookLogImportant(
            "InstallPresentInlineHooks: below-the-chain body view unavailable at %p with a single foreign overlay — "
            "falling back to the entry prepend (CE keeps a full Present view but composites BEFORE that overlay, so "
            "its overlay is the bottom layer)",
            presentAddr);
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
        // The leave-the-entry decision already ran above, before this block, because it must
        // not depend on a foreign jump being visible at this instant. Reaching here means CE
        // may prepend: at most one foreign overlay owns the entry, so the chain survives a
        // second participant.
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
    if (!ce::overlay_compat::ShouldLeavePresentEntryToForeignOverlayChain(loadedOverlayCount)) {
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
            entryPatchIntact, loadedOverlayCount, /*alreadyLeftToForeignChain=*/false)) {
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

