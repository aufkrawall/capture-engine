#include "dxgi_shared_internal.h"

namespace DXGIShared {
// Detect if SL has hooked the Present function with an E9 JMP or FF 25
// indirect JMP.  If so, set up routing so our final Present call goes
// through SL's hook chain instead of bypassing it via the trampoline.
void DetectSLPresentHook() {
    if (dxgi_shared_s_slRoutingActive.load(std::memory_order_acquire))
        return;
    // Below a foreign Present chain `oPresent` IS the live entry, and SL routing forwards
    // through it directly (dxgi_shared_present_core.cpp) instead of through CallOriginalPresent,
    // which is the only place that prefers the deep trampoline. Activating it there would send
    // the call back through the whole foreign chain and straight into CE's own body hook again —
    // unbounded recursion. Below the chain SL has already run by construction, so there is
    // nothing to route to. This was previously unreachable only by accident (the mode leaves
    // oPresentTrampoline null and the check below returns); with FG interposers now allowed into
    // this mode it is stated outright.
    if (IsPresentEntryLeftToForeignChain() || IsPresentInterceptedBelowForeignChain()) {
        static std::atomic<uint32_t> s_belowChainLogCount{0};
        if (s_belowChainLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant(
                "DetectSLPresentHook: Skipping — CE intercepts below the foreign Present chain, so Streamline has "
                "already processed this present and there is no chain above CE to route into (oPresent=%p)",
                dxgi_shared_oPresent);
        }
        return;
    }
    if (!dxgi_shared_oPresent || !dxgi_shared_oPresentTrampoline) {
        // Vtable hook path (externally hooked Present): oPresentTrampoline is
        // NULL because we use vtable hooking instead of inline hooking when an
        // external E9 JMP (e.g. Steam overlay) is detected on dxgi!Present.
        // In this path, oPresent is the vtable's Present entry (whose dxgi.dll
        // bytes may be owned by Steam/SL), so SL routing detection via E9 JMP on
        // oPresent bytes does not apply here.  CE uses guarded Steam-overlay
        // invocations for the bypass-only paths and otherwise lets normal
        // vtable routing decide the live Present chain.
        // Log once so post-mortem analysis can distinguish the vtable path from
        // a missing inline hook bug.
        static std::atomic<uint32_t> s_vtablePathLogCount{0};
        if (s_vtablePathLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant(
                "DetectSLPresentHook: Skipping — vtable hook path (oPresent=%p, oPresentTrampoline=NULL). "
                "SL routing detection is not applicable on the external-overlay vtable path.",
                dxgi_shared_oPresent);
        }
        return;
    }

    // If oPresent is our own trampoline, SL hasn't hooked the vtable yet.
    // The vtable repair code sets oPresent to SL's hook when detected.
    if (dxgi_shared_oPresent == dxgi_shared_oPresentTrampoline)
        return;

    auto* funcBytes = (const uint8_t*)dxgi_shared_oPresent;
    if (!IsReadableMemory(funcBytes, 16))
        return;

    // Rate-limit diagnostic logging to avoid per-frame spam.
    static int s_checkCount = 0;
    int checkNum = ++s_checkCount;
    if (checkNum <= 5 || (checkNum <= 50 && (checkNum % 10) == 0) || (checkNum % 500) == 0) {
        HookLogImportant("DetectSLPresentHook: oPresent=%p bytes: %02X %02X %02X %02X %02X %02X (check #%d)", dxgi_shared_oPresent,
                         funcBytes[0], funcBytes[1], funcBytes[2], funcBytes[3], funcBytes[4], funcBytes[5], checkNum);
    }

    // Detect SL hooks: E9 relative JMP or FF 25 indirect JMP (JMP [RIP+0]).
    // SL may use either pattern depending on version and game.
    bool isE9 = (funcBytes[0] == 0xE9);
    bool isFF25 = (funcBytes[0] == 0xFF && funcBytes[1] == 0x25);

    if (!isE9 && !isFF25) {
        return;
    }

    void* hookTarget = isE9 ? ResolveE9JmpTarget((void*)dxgi_shared_oPresent) : ResolveFF25JmpTarget((void*)dxgi_shared_oPresent);
    char hookTargetModulePath[MAX_PATH] = {};
    HMODULE hookTargetModule = nullptr;
    const bool hookTargetResolved =
        hookTarget && TryGetModulePathFromCodeAddress(hookTarget, hookTargetModulePath, sizeof(hookTargetModulePath),
                                                      &hookTargetModule);
    const bool hookTargetFromStreamline = hookTargetResolved && IsStreamlineModuleHandle(hookTargetModule);
    const bool hookTargetFromCaptureHook = hookTargetResolved && IsCaptureHookModulePath(hookTargetModulePath);
    if (!DXGIShared::ShouldActivateStreamlinePresentRoutingForHookTarget(
            true, hookTargetResolved, hookTargetFromStreamline, hookTargetFromCaptureHook)) {
        static std::atomic<uint32_t> s_rejectedHookTargetLogCount{0};
        const uint32_t rejectedLogCount = s_rejectedHookTargetLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (rejectedLogCount <= 20 || (rejectedLogCount % 500) == 0) {
            HookLogImportant(
                "DetectSLPresentHook: %s JMP at oPresent=%p rejected as non-Streamline target "
                "(target=%p resolved=%d module=%s captureHook=%d streamline=%d log=%u)",
                isE9 ? "E9" : "FF25", dxgi_shared_oPresent, hookTarget, hookTargetResolved ? 1 : 0,
                hookTargetModulePath[0] ? hookTargetModulePath : "unknown", hookTargetFromCaptureHook ? 1 : 0,
                hookTargetFromStreamline ? 1 : 0, rejectedLogCount);
        }
        return;
    }

    // Verify that our trampoline is different (it should have the original
    // function bytes, not a JMP).
    auto* trampolineBytes = (const uint8_t*)dxgi_shared_oPresentTrampoline;
    static std::atomic<uint32_t> s_trampolineBytesLogCount{0};
    const uint32_t trampolineLogCount = s_trampolineBytesLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (trampolineLogCount <= 5 || (trampolineLogCount % 500) == 0) {
        HookLogImportant("DetectSLPresentHook: trampoline=%p bytes: %02X %02X %02X %02X %02X %02X (trampolineLog=%u)",
                         dxgi_shared_oPresentTrampoline, trampolineBytes[0], trampolineBytes[1], trampolineBytes[2],
                         trampolineBytes[3], trampolineBytes[4], trampolineBytes[5], trampolineLogCount);
    }

    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
    bool runtimeOwnedNativeFGPresentPath = false;

    // Don't re-enable SL routing while the native/runtime-owned FSR path still
    // owns presentation. GTA showed that the old runtime=FSR_FG-only guard was
    // too narrow: during the explicit native-FSR OFF teardown window, the FFX
    // runtime can still own presentation even though ffxConfigure briefly
    // publishes Off. Re-attaching Streamline's Present hook chain in that window
    // reintroduces mixed-runtime routing on the native FSR path.
    if (ShouldKeepSLPresentRoutingDisabledNow(&runtimeMode, &runtimeOwnedNativeFGPresentPath)) {
        static int s_suppressedCount = 0;
        int suppressedNum = ++s_suppressedCount;
        if (suppressedNum <= 5 || (suppressedNum % 500) == 0) {
            HookLogImportant(
                "DetectSLPresentHook: SL %s JMP detected at oPresent=%p but SL routing NOT re-enabled "
                "(native FG path owns Present routing, suppressed #%d, runtime=%s "
                "runtimeOwnedNativeFG=%d)",
                isE9 ? "E9" : "FF25", dxgi_shared_oPresent, suppressedNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                runtimeOwnedNativeFGPresentPath ? 1 : 0);
        }
        return;
    }

    dxgi_shared_s_slRoutingActive.store(true, std::memory_order_release);
    HookLogImportant(
        "SL routing ACTIVE: Present calls will go through oPresent=%p "
        "(%s JMP target=%p module=%s) instead of trampoline=%p.  SL FG chain will execute.",
        dxgi_shared_oPresent, isE9 ? "E9" : "FF25", hookTarget, hookTargetModulePath[0] ? hookTargetModulePath : "unknown",
        dxgi_shared_oPresentTrampoline);
}
}

namespace DXGIShared {
void UpdateDXGIPresentMetricsAndPublish(bool isFirstHook, const char* publicationSource) {
    if (!isFirstHook) {
        return;
    }

    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPresentObserved, publicationSource);

    dxgi_shared_g_DXGIPerfMetrics.Update(PerfLogger::GetQpcUs());
    const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
    ce::overlay_metrics::PublishOverlayFGMetrics(&dxgi_shared_g_DXGIPerfMetrics, plan, g_FGCompat.GetOutputFPS(),
                                                 g_FGCompat.GetBaseFPS(), g_FGCompat.GetFGMultiplier(),
                                                 publicationSource);
}
}

namespace DXGIShared {
void RefreshLivePresentHooksForSwapchainIfNeeded(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain || !IsReadableMemory(pSwapChain, sizeof(void*))) {
        return;
    }

    void** vtable = *(void***)pSwapChain;
    const bool hasReadableVtable = vtable && IsReadableMemory(reinterpret_cast<const void*>(vtable), 23 * sizeof(void*));
    const bool trackedVtableMatchesCurrent = hasReadableVtable && dxgi_shared_s_hookedVTable == vtable;
    const bool presentHookInstalled = hasReadableVtable && vtable[8] == (void*)DetourPresent;
    const bool present1HookInstalled = hasReadableVtable && vtable[22] == (void*)DetourPresent1;

    if (!DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(hasReadableVtable, trackedVtableMatchesCurrent,
                                                                   presentHookInstalled, present1HookInstalled)) {
        return;
    }

    HookLogImportant(
        "DXGIShared: Refreshing live Present hook path via %s swapchain %p (oldVtable=%p newVtable=%p hooked8=%d "
        "hooked22=%d)",
        source ? source : "runtime", pSwapChain, dxgi_shared_s_hookedVTable, vtable, presentHookInstalled ? 1 : 0,
        present1HookInstalled ? 1 : 0);

    InstallHooks(pSwapChain, true);
    RepairVTableHooksIfNeeded();

    if (IsSLInterposerLoaded() && !ce::fg_runtime::RuntimeModeUsesFSR(g_FGCompat.GetRuntimeMode())) {
        DetectSLPresentHook();
    }
}
}

namespace DXGIShared {
// RTSS-style: draw the overlay present-time before a Streamline-startup bypass present so the
// toggle-on window between the FG-ON edge and the first PostSL callback still carries the overlay
// (session 20260813_170318: 150-203 ms blank on every switch-to-DLSS under FG-switch spam).
// HandleDX12ProcessFrame resolves the submit queue and does the same-queue safety check internally
// (see the pre-SL un-gate in dx12_hook.cpp). Gated so steady-state FG and the round-1..3 wins are
// untouched: D3D12 only, DLSS FG turning on, PostSL not yet confirmed, and a live overlay backend -
// post-FSR requires the prewarmed/preserved backend bound to the exact proxy, pure DLSS requires the
// explicit-enable cold-start proof (or the legacy CE_DLSS_TOGGLE_OVERLAY_EAGER opt-in).
void MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(IDXGISwapChain* pSwapChain, bool isD3D12,
                                                               bool streamlineFGRunning, bool postSLConfirmedRendering,
                                                               bool hadFSRFGPhase, bool explicitSetOptionsActivation,
                                                               const char* site) {
    if (!DX12_ShouldEagerDrawOverlayBeforeStreamlineStartupBypass(pSwapChain, isD3D12, streamlineFGRunning,
                                                                  postSLConfirmedRendering, hadFSRFGPhase,
                                                                  explicitSetOptionsActivation))
        return;
    static std::atomic<int> s_log{0};
    const int n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 10 || (n % 120) == 0)
        HookLogImportant(
            "DX12: Eager present-time overlay draw before Streamline-startup bypass (RTSS-style, site=%s sc=%p)", site,
            (void*)pSwapChain);
    HandleDX12ProcessFrame(pSwapChain, false, true);
}
}

namespace DXGIShared {
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
bool AttemptSteamDX12OverlayInit(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                        PFN_Present presentOriginal, PFN_Present presentBypass, HRESULT* resultOut) {
    if (!pSwapChain || !resultOut || !dxgi_shared_s_hookedVTable || !presentOriginal || dxgi_shared_s_steamInitCrashed) {
        return false;
    }

    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 9 * sizeof(void*))) {
        return false;
    }

    // Only one thread wins the init race
    bool expected = false;
    if (!dxgi_shared_s_steamDX12InitAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
        return false;  // Another thread is already handling init
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: VirtualProtect failed to unhook vtable[8] — will retry on next frame");
        dxgi_shared_s_steamDX12InitAttempted.store(false, std::memory_order_release);
        return false;
    }

    // Restore only CE's exact slot. A foreign injector may have replaced the
    // entry after the initial check and must never be overwritten here.
    void* savedVtable8 = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&dxgi_shared_s_hookedVTable[8]), (void*)presentOriginal,
        (void*)DetourPresent);
    VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
    if (savedVtable8 != (void*)DetourPresent) {
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: Preserving concurrent foreign vtable[8]=%p; Steam init handoff skipped",
            savedVtable8);
        dxgi_shared_s_steamDX12InitAttempted.store(false, std::memory_order_release);
        return false;
    }

    HookLogImportant(
        "AttemptSteamDX12OverlayInit: vtable[8] temporarily restored to dxgi!Present=%p — "
        "calling through E9 JMP for Steam overlay init (with VEH protection) "
        "[s_originalVtable8Present=%p, same=%d]",
        (void*)presentOriginal, (void*)dxgi_shared_s_originalVtable8Present, dxgi_shared_s_originalVtable8Present == presentOriginal ? 1 : 0);

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
    if (VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        void* replaced = InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&dxgi_shared_s_hookedVTable[8]), (void*)DetourPresent,
            (void*)presentOriginal);
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
        if (replaced != (void*)presentOriginal) {
            HookLogImportant(
                "AttemptSteamDX12OverlayInit: Preserving foreign vtable[8]=%p installed during Steam init",
                replaced);
        }
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
            if (IsReadableMemory(reinterpret_cast<const void*>(steamCallbackPtr), sizeof(void*))) {
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
}
