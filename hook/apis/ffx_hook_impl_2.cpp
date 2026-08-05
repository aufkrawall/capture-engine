#include "ffx_hook_internal.h"


bool ffx_hook_InstallHooksForModule(HMODULE hModule,  const char* ffx_hook_moduleName) {


    if (!hModule)
        return false;

    const bool firstSeenModule = ffx_hook_g_HookedModule != hModule;
    if (firstSeenModule) {
        HookLog("FFX Hook: Installing hooks for module %s (%p)", ffx_hook_moduleName, hModule);
    }
    g_FGCompat.SetFSRFGSupportPresent(true);

    // Get the original functions
    PfnFfxCreateContext createCtx = (PfnFfxCreateContext)GetProcAddress(hModule, "ffxCreateContext");
    PfnFfxDestroyContext destroyCtx = (PfnFfxDestroyContext)GetProcAddress(hModule, "ffxDestroyContext");
    PfnFfxConfigure configureCtx = (PfnFfxConfigure)GetProcAddress(hModule, "ffxConfigure");

    if (!createCtx && !destroyCtx && !configureCtx) {
        static std::atomic<int> s_noExportLogCount{0};
        const int logCount = s_noExportLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 300) == 0) {
            HookLog("FFX Hook: No supported FFX exports found in %s - skipping (log=%d)", ffx_hook_moduleName, logCount);
        }
        return false;
    }

    // Do not retire a live durable route merely because a related FFX DLL without ffxConfigure was observed.
    // Only a genuinely different callable configure export requires a new breakpoint/routing epoch.
    const void* installedConfigureTarget = ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire);
    if (firstSeenModule && ffx_hook_g_HookedModule && configureCtx && installedConfigureTarget &&
        installedConfigureTarget != reinterpret_cast<void*>(configureCtx)) {
        ffx_hook_g_DurableCachedConfigureRouteActive.store(false, std::memory_order_release);
        ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.store(false, std::memory_order_release);
    }

    const bool allowInlineHooks = ce::ffx_api::ShouldInlineHookFFXExportsForModule(ffx_hook_moduleName);
    const bool allowIATHooks = ce::ffx_api::ShouldPatchFFXImportsForModule(ffx_hook_moduleName);

    const auto resolvedDefaultPresentCallback =
        reinterpret_cast<ce::ffx_api::PresentCallback>(GetProcAddress(hModule, "ffxFrameInterpolationUiComposition"));
    if (resolvedDefaultPresentCallback && resolvedDefaultPresentCallback != ffx_hook_g_DefaultPresentCallback) {
        ffx_hook_g_DefaultPresentCallback = resolvedDefaultPresentCallback;
        if (ffx_hook_g_DefaultPresentCallback) {
            HookLogImportant("FFX Hook: Resolved default frame-interpolation present callback at %p",
                             reinterpret_cast<void*>(ffx_hook_g_DefaultPresentCallback));
        }
    }

    // Direct-export originals must follow protected/dynamic FFX module reloads.
    // GTA can unload amd_fidelityfx_dx12.dll and map it again at a new base when
    // a save enables native FSR; keeping the old ffxConfigure pointer turns the
    // next VEH hook call into a DEP fault on the unmapped image.
    RefreshDirectOriginalForModuleReload(ffx_hook_g_Original_ffxCreateContext, createCtx, ffx_hook_g_ffxCreateContextInlineHooked,
                                         ffx_hook_g_ffxCreateContextTarget, "ffxCreateContext", ffx_hook_moduleName);
    RefreshDirectOriginalForModuleReload(ffx_hook_g_Original_ffxDestroyContext, destroyCtx, ffx_hook_g_ffxDestroyContextInlineHooked,
                                         ffx_hook_g_ffxDestroyContextTarget, "ffxDestroyContext", ffx_hook_moduleName);
    RefreshDirectOriginalForModuleReload(ffx_hook_g_Original_ffxConfigure, configureCtx, ffx_hook_g_ffxConfigureInlineHooked,
                                         ffx_hook_g_ffxConfigureTarget, "ffxConfigure", ffx_hook_moduleName);
    ffx_hook_g_HookedModule = hModule;

    ffx_hook_RegisterDynamicHooksOnce();

    const bool armProtectedConfigureBreakpoint =
        !allowInlineHooks && ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint(ffx_hook_moduleName);

    if (!allowInlineHooks && allowIATHooks && ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(ffx_hook_moduleName) &&
        firstSeenModule) {
        HookLogImportant(
            "FFX Hook: Using IAT/dynamic hooks for protected official FFX module %s; inline export JMP patching "
            "skipped; guarded ffxConfigure VEH fallback %s for SDK dispatch-table/intra-module calls",
            ffx_hook_moduleName, armProtectedConfigureBreakpoint ? "enabled" : "not eligible");
    } else if (!allowInlineHooks && !allowIATHooks && firstSeenModule) {
        HookLogImportant(
            "FFX Hook: Using GetProcAddress-only hooks for protected official FFX module %s; inline export patching "
            "and IAT import patching skipped to avoid startup fail-fast; code bytes left unmodified; waiting for "
            "a real ffxConfigure call to arm the native FSR present-callback bridge",
            ffx_hook_moduleName);
    }

    // Install IAT hooks in loaded non-system/non-overlay modules to intercept calls to FFX functions.
    // Official AMD runtime DLLs are intentionally not inline-patched. Import-table routing is allowed because it
    // changes caller thunks instead of the AMD runtime code page and lets statically importing games expose the real
    // ffxConfigure packet needed for the present-callback bridge.

    void* dummy = nullptr;
    bool routedAnything = false;
    bool inlineHookedAnything = false;
    bool iatPatchedAnything = false;
    bool vehHookedAnything = false;
    if (createCtx) {
        routedAnything = true;
        if (allowInlineHooks) {
            inlineHookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(createCtx),
                                      reinterpret_cast<void*>(Hooked_ffxCreateContext), ffx_hook_g_Original_ffxCreateContext,
                                      ffx_hook_g_ffxCreateContextInlineHooked, ffx_hook_g_ffxCreateContextTarget, "ffxCreateContext");
        }
        HookLog("FFX Hook: ffxCreateContext found at %p, hooking via %s (inline=%d)", createCtx,
                allowIATHooks ? "IAT/dynamic" : "dynamic-only", allowInlineHooks ? 1 : 0);
        if (allowIATHooks) {
            iatPatchedAnything |=
                IATHook::PatchIATAllModules(ffx_hook_moduleName, "ffxCreateContext", (void*)Hooked_ffxCreateContext, &dummy);
        }
    }

    if (destroyCtx) {
        routedAnything = true;
        if (allowInlineHooks) {
            inlineHookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(destroyCtx),
                                      reinterpret_cast<void*>(Hooked_ffxDestroyContext), ffx_hook_g_Original_ffxDestroyContext,
                                      ffx_hook_g_ffxDestroyContextInlineHooked, ffx_hook_g_ffxDestroyContextTarget, "ffxDestroyContext");
        }
        HookLog("FFX Hook: ffxDestroyContext found at %p, hooking via %s (inline=%d)", destroyCtx,
                allowIATHooks ? "IAT/dynamic" : "dynamic-only", allowInlineHooks ? 1 : 0);
        if (allowIATHooks) {
            iatPatchedAnything |=
                IATHook::PatchIATAllModules(ffx_hook_moduleName, "ffxDestroyContext", (void*)Hooked_ffxDestroyContext, &dummy);
        }
    }

    if (configureCtx) {
        routedAnything = true;
        if (allowInlineHooks) {
            inlineHookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(configureCtx), reinterpret_cast<void*>(Hooked_ffxConfigure),
                ffx_hook_g_Original_ffxConfigure, ffx_hook_g_ffxConfigureInlineHooked, ffx_hook_g_ffxConfigureTarget, "ffxConfigure");
        } else if (!allowIATHooks) {
            static std::atomic<int> s_protectedConfigureUnpatchedLogCount{0};
            const int logCount = s_protectedConfigureUnpatchedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "FFX Hook: Protected official FFX ffxConfigure export left unpatched "
                    "(module=%s target=%p log=%d); relying on GetProcAddress-visible API routing",
                    ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", reinterpret_cast<void*>(configureCtx), logCount);
            }
        }
        HookLog("FFX Hook: ffxConfigure found at %p, hooking via %s (inline=%d veh=%d)", configureCtx,
                allowIATHooks ? (armProtectedConfigureBreakpoint ? "IAT/dynamic+VEH" : "IAT/dynamic")
                              : (armProtectedConfigureBreakpoint ? "dynamic+VEH" : "dynamic-only"),
                allowInlineHooks ? 1 : 0, armProtectedConfigureBreakpoint ? 1 : 0);
        if (allowIATHooks) {
            iatPatchedAnything |=
                IATHook::PatchIATAllModules(ffx_hook_moduleName, "ffxConfigure", (void*)Hooked_ffxConfigure, &dummy);
        }
        if (armProtectedConfigureBreakpoint) {
            // Protected official AMD module: install a re-arming VEH hook to
            // intercept SDK dispatch-table or intra-module ffxConfigure calls
            // that bypass GetProcAddress and caller import thunks. The handler
            // restores the byte before forwarding to the real function, then
            // re-arms after the call returns.
            vehHookedAnything |= InstallFfxConfigureBreakpointHook(configureCtx, ffx_hook_moduleName);
            if (!vehHookedAnything) {
                if (ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.load(std::memory_order_acquire)) {
                    // Expected no-op: the one-shot VEH already detected no-callback mode and permanently
                    // disarmed for this FG-on window (re-armed on FG-off). The periodic module rescan used
                    // to log this as a failure every second (session 20260701_213656) — log once instead.
                    static std::atomic<int> s_disarmedRearmSkipLogCount{0};
                    if (s_disarmedRearmSkipLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                        HookLog(
                            "FFX Hook: ffxConfigure VEH re-arm skipped — one-shot breakpoint permanently "
                            "disarmed for this FG-on window (module=%s target=%p)",
                            ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", reinterpret_cast<void*>(configureCtx));
                    }
                } else {
                    static std::atomic<int> s_protectedConfigureVehFailureLogCount{0};
                    const int logCount =
                        s_protectedConfigureVehFailureLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (logCount <= 20 || (logCount % 300) == 0) {
                        HookLogImportant(
                            "FFX Hook: Failed to arm guarded ffxConfigure VEH fallback "
                            "(module=%s target=%p log=%d); relying on IAT/dynamic routing only",
                            ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", reinterpret_cast<void*>(configureCtx), logCount);
                    }
                }
            }
        }
    }

    if (routedAnything) {
        static std::atomic<int> s_hooksInstalledLogCount{0};
        const int logCount = s_hooksInstalledLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (firstSeenModule || logCount <= 20 || (logCount % 300) == 0) {
            HookLog(
                "FFX Hook: Hooks installed successfully for %s (inline=%d iat=%d veh=%d dynamic=1 protected=%d "
                "log=%d)",
                ffx_hook_moduleName, inlineHookedAnything ? 1 : 0, iatPatchedAnything ? 1 : 0, vehHookedAnything ? 1 : 0,
                !allowInlineHooks ? 1 : 0, logCount);
        }
    }

    // A game can resolve FFX exports before CE's GetProcAddress hook wins startup initialization, then keep
    // those addresses in writable SDK dispatch/global slots forever. Route those already-resolved client slots
    // after the original exports and protected VEH fallback are fully initialized. This keeps
    // create/configure/destroy observable (exact gameQueue capture and immediate suspend/resume state) without
    // patching AMD's executable code or sustaining the contended entry breakpoint.
    const ce::ffx_cached_pointer_router::Route cachedRoutes[] = {
        {"ffxCreateContext", reinterpret_cast<void*>(createCtx), reinterpret_cast<void*>(Hooked_ffxCreateContext)},
        {"ffxDestroyContext", reinterpret_cast<void*>(destroyCtx), reinterpret_cast<void*>(Hooked_ffxDestroyContext)},
        {"ffxConfigure", reinterpret_cast<void*>(configureCtx), reinterpret_cast<void*>(Hooked_ffxConfigure)},
    };
    const auto cachedRouteResult =
        ce::ffx_cached_pointer_router::Refresh(hModule, cachedRoutes, _countof(cachedRoutes));
    constexpr std::uint64_t kConfigureRouteBit = std::uint64_t{1} << 2;
    if ((cachedRouteResult.routedRouteMask & kConfigureRouteBit) != 0) {
        // Once a durable client-owned ffxConfigure pointer routes through Hooked_ffxConfigure, the protected
        // entry breakpoint is redundant. Retire it immediately. A caller that fetched the original before the
        // atomic slot replacement still reaches the armed VEH; a caller that fetched the replacement reaches
        // Hooked_ffxConfigure. Setting the permanent latch before restoring the byte also prevents an in-flight
        // guarded forward from re-arming after it returns.
        const bool durableRouteWasActive =
            ffx_hook_g_DurableCachedConfigureRouteActive.exchange(true, std::memory_order_acq_rel);
        const bool breakpointWasArmed = ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire);
        ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.store(true, std::memory_order_release);
        RestoreFfxConfigureBreakpointIfCurrent(reinterpret_cast<void*>(configureCtx),
                                               "durable cached ffxConfigure pointer route installed");
        ffx_hook_g_FfxConfigureDeferredRearm.store(false, std::memory_order_release);
        ffx_hook_g_FfxConfigureDeferredRearmTarget.store(nullptr, std::memory_order_release);
        if (!durableRouteWasActive || breakpointWasArmed) {
            HookLogImportant(
                "FFX Hook: Retired protected ffxConfigure VEH breakpoint after installing a durable cached-pointer "
                "route — all later enable/disable transitions remain observable without AMD code-page mutation");
        }
    }
    if (cachedRouteResult.pointerSlotsPatched != 0) {
        HookLogImportant(
            "FFX Hook: Routed %zu pre-resolved FFX export pointer slot(s) across %zu client module(s) "
            "(%zu writable non-executable sections) — startup-cached create/configure/destroy calls now retain "
            "exact queue and transition visibility without modifying AMD code",
            cachedRouteResult.pointerSlotsPatched, cachedRouteResult.modulesScanned,
            cachedRouteResult.writableSectionsScanned);
    }
    return true;

}

void RestoreFfxConfigureBreakpointIfCurrent(void* target,  const char* ffx_hook_reason) {


    std::lock_guard<std::recursive_mutex> lock(ffx_hook_g_FfxConfigureBreakpointMutex);
    if (!target || !ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire) ||
        ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire) != target) {
        return;
    }

    if (!IsCommittedReadableCodeAddress(target)) {
        ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);
        HookLogImportant("FFX Hook: Dropping stale VEH breakpoint state for unloaded ffxConfigure target %p (%s)",
                         target, ffx_hook_reason && ffx_hook_reason[0] ? ffx_hook_reason : "target changed");
        return;
    }

    auto* targetByte = static_cast<uint8_t*>(target);
    if (*targetByte != 0xCC) {
        ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLogImportant("FFX Hook: Failed to restore stale VEH breakpoint at %p before retargeting (err=%lu)", target,
                         GetLastError());
        return;
    }
    *targetByte = ffx_hook_g_ffxConfigureOriginalFirstByte;
    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
    VirtualProtect(target, 1, PAGE_EXECUTE_READ, &oldProtect);
    ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);
    HookLogImportant("FFX Hook: Restored stale VEH breakpoint at %p before retargeting (%s)", target,
                     ffx_hook_reason && ffx_hook_reason[0] ? ffx_hook_reason : "target changed");

}

bool ArmFfxConfigureBreakpoint(PfnFfxConfigure target,  const char* ffx_hook_moduleName,  const char* ffx_hook_reason) {


    std::lock_guard<std::recursive_mutex> lock(ffx_hook_g_FfxConfigureBreakpointMutex);
    if (!target) {
        return false;
    }

    // One-shot detection: if the VEH was permanently disarmed (after the first enabled no-callback
    // ffxConfigure), do NOT re-arm. The byte stays as the original — ffxConfigure runs natively.
    if (ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.load(std::memory_order_acquire)) {
        return false;
    }

    if (ffx_hook_g_FfxConfigureOriginalForwardDepth.load(std::memory_order_acquire) > 0) {
        ffx_hook_g_FfxConfigureDeferredRearmTarget.store(reinterpret_cast<void*>(target), std::memory_order_release);
        ffx_hook_g_FfxConfigureDeferredRearm.store(true, std::memory_order_release);
        if (!ffx_hook_g_ffxConfigureInlineHooked.load(std::memory_order_acquire) && ffx_hook_g_Original_ffxConfigure != target) {
            HookLogImportant(
                "FFX Hook: Updating protected ffxConfigure original while deferring VEH arm "
                "(old=%p new=%p module=%s reason=%s)",
                reinterpret_cast<void*>(ffx_hook_g_Original_ffxConfigure), reinterpret_cast<void*>(target),
                ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", ffx_hook_reason ? ffx_hook_reason : "unknown");
            ffx_hook_g_Original_ffxConfigure = target;
        }
        static std::atomic<int> s_deferredArmLogCount{0};
        const int logCount = s_deferredArmLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "FFX Hook: Deferring ffxConfigure VEH breakpoint arm while original forwarding is active "
                "(target=%p module=%s reason=%s depth=%d log=%d)",
                reinterpret_cast<void*>(target), ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", ffx_hook_reason ? ffx_hook_reason : "unknown",
                ffx_hook_g_FfxConfigureOriginalForwardDepth.load(std::memory_order_acquire), logCount);
        }
        return true;
    }

    if (!IsCommittedReadableCodeAddress(reinterpret_cast<void*>(target))) {
        HookLogImportant("FFX Hook: Refusing to arm VEH breakpoint for unreadable ffxConfigure target %p (%s)",
                         reinterpret_cast<void*>(target), ffx_hook_moduleName ? ffx_hook_moduleName : "FFX");
        return false;
    }

    void* previousTarget = ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire);
    if (previousTarget && previousTarget != reinterpret_cast<void*>(target)) {
        RestoreFfxConfigureBreakpointIfCurrent(previousTarget, "ffxConfigure target changed");
    }

    auto* targetByte = reinterpret_cast<uint8_t*>(target);
    const uint8_t currentByte = *targetByte;
    const bool alreadyArmed = ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
                              ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire) == reinterpret_cast<void*>(target) &&
                              currentByte == 0xCC;
    if (alreadyArmed) {
        return true;
    }

    if (currentByte != 0xCC) {
        ffx_hook_g_ffxConfigureOriginalFirstByte = currentByte;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    *targetByte = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READ, &oldProtect);

    ffx_hook_g_ffxConfigureTarget.store(reinterpret_cast<void*>(target), std::memory_order_release);
    ffx_hook_g_ffxConfigureVehArmed.store(true, std::memory_order_release);
    if (!ffx_hook_g_ffxConfigureInlineHooked.load(std::memory_order_acquire) && ffx_hook_g_Original_ffxConfigure != target) {
        HookLogImportant("FFX Hook: Updating protected ffxConfigure original for VEH target (old=%p new=%p)",
                         reinterpret_cast<void*>(ffx_hook_g_Original_ffxConfigure), reinterpret_cast<void*>(target));
        ffx_hook_g_Original_ffxConfigure = target;
    }

    const bool postCallRearm = ffx_hook_reason && std::strcmp(ffx_hook_reason, "post-call rearm") == 0;
    bool shouldLogArm = true;
    int rearmLogCount = 0;
    if (postCallRearm) {
        static std::atomic<int> s_postCallRearmLogCount{0};
        rearmLogCount = s_postCallRearmLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        shouldLogArm = rearmLogCount <= 20 || (rearmLogCount % 300) == 0;
    }
    if (shouldLogArm) {
        if (postCallRearm) {
            HookLogImportant("FFX Hook: %s VEH breakpoint for %s!ffxConfigure at %p (%s #%d)",
                             currentByte == 0xCC ? "Confirmed" : "Armed", ffx_hook_moduleName ? ffx_hook_moduleName : "FFX",
                             reinterpret_cast<void*>(target), ffx_hook_reason, rearmLogCount);
        } else {
            HookLogImportant("FFX Hook: %s VEH breakpoint for %s!ffxConfigure at %p (%s)",
                             currentByte == 0xCC ? "Confirmed" : "Armed", ffx_hook_moduleName ? ffx_hook_moduleName : "FFX",
                             reinterpret_cast<void*>(target), ffx_hook_reason ? ffx_hook_reason : "init");
        }
    }
    return true;

}

ffxReturnCode_t CallFfxConfigureOriginalGuarded(PfnFfxConfigure originalConfigure,  ffxContext* ffx_hook_context, 
                                                       const ffxConfigureDescHeader* ffx_hook_desc) {


    if (!originalConfigure) {
        return 1;
    }

    ffx_hook_g_FfxConfigureOriginalForwardDepth.fetch_add(1, std::memory_order_acq_rel);
    bool pausedBreakpoint = false;
    {
        std::lock_guard<std::recursive_mutex> lock(ffx_hook_g_FfxConfigureBreakpointMutex);
        void* target = reinterpret_cast<void*>(originalConfigure);
        if (!ffx_hook_g_ffxConfigureInlineHooked.load(std::memory_order_acquire) &&
            ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
            ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire) == target && IsCommittedReadableCodeAddress(target)) {
            auto* targetByte = static_cast<uint8_t*>(target);
            if (*targetByte == 0xCC) {

                DWORD oldProtect = 0;
                if (VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    *targetByte = ffx_hook_g_ffxConfigureOriginalFirstByte;
                    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
                    VirtualProtect(target, 1, PAGE_EXECUTE_READ, &oldProtect);
                    ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);
                    pausedBreakpoint = true;

                    static std::atomic<int> s_forwardPauseLogCount{0};
                    const int logCount = s_forwardPauseLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 20 || (logCount % 300) == 0) {
                        HookLogImportant(
                            "FFX Hook: Temporarily paused protected ffxConfigure VEH breakpoint for guarded "
                            "original forwarding (target=%p log=%d)",
                            target, logCount + 1);
                    }
                } else {
                    HookLogImportant(
                        "FFX Hook: Failed to pause protected ffxConfigure VEH breakpoint before forwarding "
                        "(target=%p err=%lu)",
                        target, GetLastError());
                }
            }
        }
    }

    const ffxReturnCode_t result = originalConfigure(ffx_hook_context, ffx_hook_desc);

    if (pausedBreakpoint) {
        ffx_hook_g_FfxConfigureDeferredRearmTarget.store(reinterpret_cast<void*>(originalConfigure), std::memory_order_release);
        ffx_hook_g_FfxConfigureDeferredRearm.store(true, std::memory_order_release);
    }

    const int remainingDepth = ffx_hook_g_FfxConfigureOriginalForwardDepth.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remainingDepth == 0 && ffx_hook_g_FfxConfigureDeferredRearm.exchange(false, std::memory_order_acq_rel)) {
        void* deferredTarget = ffx_hook_g_FfxConfigureDeferredRearmTarget.exchange(nullptr, std::memory_order_acq_rel);
        if (!deferredTarget) {
            deferredTarget = reinterpret_cast<void*>(originalConfigure);
        }
        ArmFfxConfigureBreakpoint(reinterpret_cast<PfnFfxConfigure>(deferredTarget), "protected official FFX runtime",
                                  "forward-call rearm");
    }
    return result;

}

LONG WINAPI FfxConfigureBreakpointVEH(EXCEPTION_POINTERS* ep) {


    if (!ep) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto* ctx = ep->ContextRecord;
    auto* rec = ep->ExceptionRecord;

    void* target = ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire);
    const uintptr_t instructionPointer =
#ifdef _WIN64
        ctx ? static_cast<uintptr_t>(ctx->Rip) : 0;
#else
        ctx ? static_cast<uintptr_t>(ctx->Eip) : 0;
#endif
    if (!ctx || !rec || rec->ExceptionCode != STATUS_BREAKPOINT || !target ||
        !FFXHook::detail::IsEntryBreakpointHit(rec->ExceptionAddress, instructionPointer, target) ||
        !ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    *static_cast<uint8_t*>(reinterpret_cast<LPVOID>(target)) = ffx_hook_g_ffxConfigureOriginalFirstByte;
    FlushInstructionCache(GetCurrentProcess(), target, 1);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READ, &oldProtect);
    ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);

    auto contextPtr = reinterpret_cast<ffxContext*>(
#ifdef _WIN64
        ctx->Rcx
#else
        ctx->Ecx
#endif
    );
    auto* ffx_hook_desc = reinterpret_cast<const ffxConfigureDescHeader*>(
#ifdef _WIN64
        ctx->Rdx
#else
        ctx->Edx
#endif
    );
    PfnFfxConfigure previousOverride = ffx_hook_t_FfxConfigureOriginalOverride;
    ffx_hook_t_FfxConfigureOriginalOverride = reinterpret_cast<PfnFfxConfigure>(target);
    ffxReturnCode_t result = Hooked_ffxConfigure(contextPtr, ffx_hook_desc);
    ffx_hook_t_FfxConfigureOriginalOverride = previousOverride;

    static std::atomic<int> s_vehHitLogCount{0};
    const int hitCount = s_vehHitLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hitCount <= 20 || (hitCount % 300) == 0) {
        const auto* parsedDesc = reinterpret_cast<const ce::ffx_api::ApiHeader*>(ffx_hook_desc);
        HookLogImportant(
            "FFX Hook: VEH ffxConfigure breakpoint handled call #%d (context=%p desc=%p type=0x%llx result=%u)",
            hitCount, contextPtr, ffx_hook_desc, parsedDesc ? static_cast<unsigned long long>(parsedDesc->type) : 0ULL,
            static_cast<unsigned>(result));
    }

    // One-shot VEH detection: disarm only after BOTH (1) the no-callback flag is set AND (2) the UI
    // texture has been cached from a RegisterUiResource call. If we disarm before the cache is populated,
    // an otherwise-unrouted caller runs natively and the cache is never filled → the bundle never fires.
    // The RegisterUiResource (type=0x30002) may arrive before or after the enabled configure (type=0x20002)
    // that sets the no-callback flag, so we must wait for both conditions.
    if (ffx_hook_desc && DX12_IsNativeFSRInternalNoCallbackCompositionActive() && DX12_IsFFXUiResourceCachedForBundle() &&
        !ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "FFX Hook: VEH permanently disarmed after one-shot no-callback detection (context=%p) — "
            "AMD's code page stays native; IAT/GetProcAddress/cached-pointer routes remain observable without "
            "multi-threaded 0xCC contention that desyncs ffxQuery pacing",
            contextPtr);
    } else {
        // Re-arm after the real ffxConfigure returns. This keeps protected official
        // runtimes on a breakpoint hook without installing a permanent JMP, and
        // prevents early non-FG/disabled configures from consuming the only chance
        // to catch a later save-load FG enable.
        ArmFfxConfigureBreakpoint(reinterpret_cast<PfnFfxConfigure>(target), "protected official FFX runtime",
                                  "post-call rearm");
    }

    // Return directly to the caller: pop the return address from the stack
    // and skip the patched ffxConfigure body entirely (Hooked_ffxConfigure
    // already called g_Original_ffxConfigure, so re-executing would double-call).
#ifdef _WIN64
    ctx->Rax = result;
    ctx->Rip = *reinterpret_cast<ULONG64*>(ctx->Rsp);
    ctx->Rsp += 8;
#else
    ctx->Eax = result;
    ctx->Eip = *reinterpret_cast<DWORD*>(ctx->Esp);
    ctx->Esp += 4;
#endif
    return EXCEPTION_CONTINUE_EXECUTION;

}

bool InstallFfxConfigureBreakpointHook(PfnFfxConfigure target,  const char* ffx_hook_moduleName) {


    if (!ffx_hook_g_ffxConfigureVehHandle) {
        ffx_hook_g_ffxConfigureVehHandle = AddVectoredExceptionHandler(1, FfxConfigureBreakpointVEH);
        if (!ffx_hook_g_ffxConfigureVehHandle) {
            return false;
        }
    }

    const bool alreadyInstalledAndArmed =
        ffx_hook_g_ffxConfigureVehInstalled && ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
        ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire) == reinterpret_cast<void*>(target);
    if (!ArmFfxConfigureBreakpoint(target, ffx_hook_moduleName, "init")) {
        return false;
    }
    ffx_hook_g_ffxConfigureVehInstalled = true;
    if (!alreadyInstalledAndArmed) {
        HookLogImportant("FFX Hook: Installed re-arming VEH hook for %s!ffxConfigure at %p", ffx_hook_moduleName,
                         reinterpret_cast<void*>(target));
    }
    return true;

}

bool InstallBridgeOnTrackedContextsImpl(void* swapChain) {


    (void)swapChain;
    HookLogImportant(
        "FFX Hook: Skipping retroactive present-callback bridge install because synthetic partial ffxConfigure "
        "packets are unsafe for the official FSR runtime; waiting for a real enabled ffxConfigure instead");
    return false;

}
