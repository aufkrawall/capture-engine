                DWORD oldProtect = 0;
                if (VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    *targetByte = g_ffxConfigureOriginalFirstByte;
                    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
                    VirtualProtect(target, 1, PAGE_EXECUTE_READ, &oldProtect);
                    g_ffxConfigureVehArmed.store(false, std::memory_order_release);
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

    const ffxReturnCode_t result = originalConfigure(context, desc);

    if (pausedBreakpoint) {
        g_FfxConfigureDeferredRearmTarget.store(reinterpret_cast<void*>(originalConfigure), std::memory_order_release);
        g_FfxConfigureDeferredRearm.store(true, std::memory_order_release);
    }

    const int remainingDepth = g_FfxConfigureOriginalForwardDepth.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remainingDepth == 0 && g_FfxConfigureDeferredRearm.exchange(false, std::memory_order_acq_rel)) {
        void* deferredTarget = g_FfxConfigureDeferredRearmTarget.exchange(nullptr, std::memory_order_acq_rel);
        if (!deferredTarget) {
            deferredTarget = reinterpret_cast<void*>(originalConfigure);
        }
        ArmFfxConfigureBreakpoint(reinterpret_cast<PfnFfxConfigure>(deferredTarget), "protected official FFX runtime",
                                  "forward-call rearm");
    }
    return result;
}

static LONG WINAPI FfxConfigureBreakpointVEH(EXCEPTION_POINTERS* ep) {
    if (!ep) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto* ctx = ep->ContextRecord;
    auto* rec = ep->ExceptionRecord;

    void* target = g_ffxConfigureTarget.load(std::memory_order_acquire);
    const uintptr_t instructionPointer =
#ifdef _WIN64
        ctx ? static_cast<uintptr_t>(ctx->Rip) : 0;
#else
        ctx ? static_cast<uintptr_t>(ctx->Eip) : 0;
#endif
    if (!ctx || !rec || rec->ExceptionCode != STATUS_BREAKPOINT || !target ||
        !FFXHook::detail::IsEntryBreakpointHit(rec->ExceptionAddress, instructionPointer, target) ||
        !g_ffxConfigureVehArmed.load(std::memory_order_acquire)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    *static_cast<uint8_t*>(reinterpret_cast<LPVOID>(target)) = g_ffxConfigureOriginalFirstByte;
    FlushInstructionCache(GetCurrentProcess(), target, 1);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READ, &oldProtect);
    g_ffxConfigureVehArmed.store(false, std::memory_order_release);

    auto contextPtr = reinterpret_cast<ffxContext*>(
#ifdef _WIN64
        ctx->Rcx
#else
        ctx->Ecx
#endif
    );
    auto* desc = reinterpret_cast<const ffxConfigureDescHeader*>(
#ifdef _WIN64
        ctx->Rdx
#else
        ctx->Edx
#endif
    );
    PfnFfxConfigure previousOverride = t_FfxConfigureOriginalOverride;
    t_FfxConfigureOriginalOverride = reinterpret_cast<PfnFfxConfigure>(target);
    ffxReturnCode_t result = Hooked_ffxConfigure(contextPtr, desc);
    t_FfxConfigureOriginalOverride = previousOverride;

    static std::atomic<int> s_vehHitLogCount{0};
    const int hitCount = s_vehHitLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hitCount <= 20 || (hitCount % 300) == 0) {
        const auto* parsedDesc = reinterpret_cast<const ce::ffx_api::ApiHeader*>(desc);
        HookLogImportant(
            "FFX Hook: VEH ffxConfigure breakpoint handled call #%d (context=%p desc=%p type=0x%llx result=%u)",
            hitCount, contextPtr, desc, parsedDesc ? static_cast<unsigned long long>(parsedDesc->type) : 0ULL,
            static_cast<unsigned>(result));
    }

    // One-shot VEH detection: disarm only after BOTH (1) the no-callback flag is set AND (2) the UI
    // texture has been cached from a RegisterUiResource call. If we disarm before the cache is populated,
    // an otherwise-unrouted caller runs natively and the cache is never filled → the bundle never fires.
    // The RegisterUiResource (type=0x30002) may arrive before or after the enabled configure (type=0x20002)
    // that sets the no-callback flag, so we must wait for both conditions.
    if (desc && DX12_IsNativeFSRInternalNoCallbackCompositionActive() && DX12_IsFFXUiResourceCachedForBundle() &&
        !g_ffxConfigureVehPermanentlyDisarmed.exchange(true, std::memory_order_acq_rel)) {
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

static bool InstallFfxConfigureBreakpointHook(PfnFfxConfigure target, const char* moduleName) {
    if (!g_ffxConfigureVehHandle) {
        g_ffxConfigureVehHandle = AddVectoredExceptionHandler(1, FfxConfigureBreakpointVEH);
        if (!g_ffxConfigureVehHandle) {
            return false;
        }
    }

    const bool alreadyInstalledAndArmed =
        g_ffxConfigureVehInstalled && g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
        g_ffxConfigureTarget.load(std::memory_order_acquire) == reinterpret_cast<void*>(target);
    if (!ArmFfxConfigureBreakpoint(target, moduleName, "init")) {
        return false;
    }
    g_ffxConfigureVehInstalled = true;
    if (!alreadyInstalledAndArmed) {
        HookLogImportant("FFX Hook: Installed re-arming VEH hook for %s!ffxConfigure at %p", moduleName,
                         reinterpret_cast<void*>(target));
    }
    return true;
}

// Retroactive ffxConfigure call: when FSR FG activates through Streamline's
// authoritative takeover (no direct ffxConfigure intercepted), CE calls
// g_Original_ffxConfigure on all tracked FG contexts to install the present
// callback bridge.  This is necessary because ffxConfigure is called during
// AMD module init, before CE can intercept it.
static bool InstallBridgeOnTrackedContextsImpl(void* swapChain) {
    (void)swapChain;
    HookLogImportant(
        "FFX Hook: Skipping retroactive present-callback bridge install because synthetic partial ffxConfigure "
        "packets are unsafe for the official FSR runtime; waiting for a real enabled ffxConfigure instead");
    return false;
}

}  // anonymous namespace

// Re-assert bracket (freeze diagnosis): non-zero QPC + tid while a re-assert's ffxConfigure forward is
// in flight. A freeze dump showing a stuck bracket pinpoints a registerUiResource lock wedge immediately
// (the session 20260701_213656 deadlock signature).
static std::atomic<uint64_t> g_SubstReRegInFlightQpc{0};
static std::atomic<uint32_t> g_SubstReRegInFlightTid{0};

// Called ONLY from the FFX proxy-present prework (game thread, before AMD's Present) after the overlay was
// composited onto CE's substitute, so AMD's Present snapshots CE's substitute (with the overlay) instead of
// GTA's per-frame 1x1. No-op for the game-tex path (never stored) and when no-callback FSR FG is inactive.
// Re-asserting at proxy-present entry (after the game's per-frame RegisterUiResource, before AMD's
// criticalSection-guarded UI-resource snapshot) keeps CE's substitute as the effective registration; it is
// the same ffxConfigure(RegisterUiResource) call, thread, and lock order the game itself uses per frame.
//
// DEADLOCK BOUNDARY (session 20260701_213656 — permanent GTA freeze on the FIRST FSR-FG frame): this
// forward enters AMD's FrameInterpolationSwapchain criticalSection (registerUiResource). AMD's Present
// HOLDS that criticalSection on the game thread while spin-waiting WITHOUT timeout on compositionFenceCPU,
// which only advances when AMD's presenter thread completes the real present. DetourPresent for the real
// swapchain runs ON that presenter thread — calling this from there closes the cycle and freezes the game
// permanently. Hence the hard prework-context guard below (policy: MayReassertSubstituteUiResource).
FFXSubstituteUiReRegistrationResult FFXHook_ReRegisterSubstituteUiResource() {
    if (!g_SubstReRegActive.load(std::memory_order_acquire) || !DX12_IsNativeFSRInternalNoCallbackCompositionActive()) {
        return FFXSubstituteUiReRegistrationResult::kNotNeeded;
    }
    const auto driver =
        ce::dx12_overlay_policy::ChooseFFXUiCompositeDriver(DX12_IsCurrentThreadInsideFFXProxyPresentPrework());
    if (!ce::dx12_overlay_policy::MayReassertSubstituteUiResource(driver)) {
        static std::atomic<int> s_refusedLog{0};
        const int n = s_refusedLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 600) == 0) {
            HookLogImportant(
                "FFX Hook: REFUSED substitute UI-resource re-assert outside the proxy-present prework "
                "(tid=0x%04X log=%d) — registerUiResource takes AMD's swapchain criticalSection, which "
                "deadlocks from the presenter thread (session 20260701_213656)",
                GetCurrentThreadId(), n + 1);
        }
        return FFXSubstituteUiReRegistrationResult::kFailed;
    }
    std::lock_guard<std::mutex> lock(g_SubstReRegMutex);
    if (!g_SubstReRegConfigure || !g_SubstReRegContext) {
        return FFXSubstituteUiReRegistrationResult::kFailed;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_SubstReRegInFlightTid.store(GetCurrentThreadId(), std::memory_order_release);
    g_SubstReRegInFlightQpc.store(static_cast<uint64_t>(qpc.QuadPart), std::memory_order_release);
    const ffxReturnCode_t result =
        g_SubstReRegConfigure(&g_SubstReRegContext, reinterpret_cast<const ffxConfigureDescHeader*>(&g_SubstReRegDesc));
    g_SubstReRegInFlightQpc.store(0, std::memory_order_release);
    g_SubstReRegInFlightTid.store(0, std::memory_order_release);
    if (result != FFX_API_RETURN_OK) {
        HookLogImportant("FFX Hook: substitute UI-resource re-registration FAILED (ctx=%p result=%d)",
                         (void*)g_SubstReRegContext, static_cast<int>(result));
        return FFXSubstituteUiReRegistrationResult::kFailed;
    }
    static std::atomic<int> s_reRegLog{0};
    const int n = s_reRegLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 10 || (n % 600) == 0) {
        HookLogImportant(
            "FFX Hook: re-registered CE substitute UI resource %p (ctx=%p tid=0x%04X) so AMD composites it over "
            "GTA's per-frame 1x1 (log=%d)",
            g_SubstReRegDesc.uiResource.resource, (void*)g_SubstReRegContext, GetCurrentThreadId(), n + 1);
    }
    return FFXSubstituteUiReRegistrationResult::kSucceeded;
}

// Freeze-dump snapshot of the re-assert bracket (paired with DX12_LogFFXProxyPresentHookFreezeDiagnostics).
void FFXHook_LogSubstituteReRegFreezeDiagnostics(const char* reason) {
    HookLogImportant("FFX Hook: [subst-rereg-freeze-diag] %s — active=%d inFlightQpc=%llu inFlightTid=0x%04X",
                     reason ? reason : "freeze", g_SubstReRegActive.load(std::memory_order_acquire) ? 1 : 0,
                     static_cast<unsigned long long>(g_SubstReRegInFlightQpc.load(std::memory_order_acquire)),
                     g_SubstReRegInFlightTid.load(std::memory_order_acquire));
}

// Stop re-registering when CE's substitute texture is released (device change / teardown) — the stored desc's
// resource pointer is then dangling. Called from ReleaseFFXUiCompositeInfra (dx12_hook.cpp).
void FFXHook_ClearSubstituteUiReRegistration() {
    std::lock_guard<std::mutex> lock(g_SubstReRegMutex);
    g_SubstReRegActive.store(false, std::memory_order_release);
    g_SubstReRegContext = nullptr;
    g_SubstReRegConfigure = nullptr;
    g_SubstReRegDesc = {};
}

// Reset the one-shot VEH disarm and re-arm the breakpoint for the next FG-on transition.
// Called from DX12_OnNativeFSRFrameGenerationContextsDestroyed / ForceClearNativeFSRInternalNoCallbackComposition
// when FG turns off and no durable cached-pointer route exists. The next enabled ffxConfigure will fire the VEH
// once, detect no-callback mode, and disarm again — one VEH hit per FG-on transition, no sustained contention.
void FFXHook_ResetVehDisarmAndRearm() {
    if (g_DurableCachedConfigureRouteActive.load(std::memory_order_acquire)) {
        g_ffxConfigureVehPermanentlyDisarmed.store(true, std::memory_order_release);
        void* durableTarget = g_ffxConfigureTarget.load(std::memory_order_acquire);
        RestoreFfxConfigureBreakpointIfCurrent(durableTarget, "durable cached ffxConfigure route remains active");
        HookLogImportant(
            "FFX Hook: Kept protected ffxConfigure VEH retired across FG context destruction because the durable "
            "cached-pointer route remains active (target=%p)",
            durableTarget);
        return;
    }
    g_ffxConfigureVehPermanentlyDisarmed.store(false, std::memory_order_release);
    void* target = g_ffxConfigureTarget.load(std::memory_order_acquire);
    if (target) {
        ArmFfxConfigureBreakpoint(reinterpret_cast<PfnFfxConfigure>(target), "protected official FFX runtime",
                                  "FG-off re-arm for next on-transition");
        HookLogImportant("FFX Hook: VEH disarm reset + re-armed for next FG-on transition (target=%p)", target);
    }
}

// ============================================================================
// Public API
// ============================================================================

namespace FFXHook {

void* GetPresentCallbackBridgeKey(void* context) {
    return GetOrCreatePresentCallbackBridgeKey(context);
}

void RegisterDynamicHooks() {
    RegisterDynamicHooksOnce();
}

bool InstallBridgeOnTrackedContexts(void* swapChain) {
    return InstallBridgeOnTrackedContextsImpl(swapChain);
}

void Init() {
    std::lock_guard<std::mutex> lock(g_InitMutex);

    static int s_initCallCount = 0;
    ++s_initCallCount;

    if (!g_Initialized.load(std::memory_order_acquire) && !g_NoModulesLogged.load(std::memory_order_acquire)) {
        HookLog("FFX Hook: Initializing...");
    }

    RegisterDynamicHooksOnce();

    // Try to find FFX modules.
    // These cover both the older explicit FG DLL names and newer generic
    // FidelityFX runtime DLL names observed in GTA V Enhanced.
    // Also includes dlssg-to-fsr3 mod DLLs that redirect DLSS FG to FSR FG.
    const wchar_t* ffxModules[] = {
        // FSR 4 / FSR 3.1 DLLs (UE5 native integration) - check first.
        L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_framegeneration_vk.dll",
        // GTA V Enhanced can load the generic FidelityFX runtime DLL name while
        // still routing native frame generation through the FFX API exports.
        L"amd_fidelityfx_dx12.dll",
        L"amd_fidelityfx_vk.dll",
        // Standard AMD FSR FG DLLs.
        L"amd_fidelityfx_fg.dll",
        L"ffx_frameinterpolation_x64.dll",
        L"amd_fidelityfx_framegeneration.dll",
        L"ffx_framegeneration.dll",
        // dlssg-to-fsr3 mod - uses nvngx_dlssg.dll as a proxy that calls FFX API.
        L"nvngx_dlssg.dll",
        // FSR3 FG mod common names.
        L"fsr3fg.dll",
        L"fsr3mod.dll",
    };

    const char* ffxModuleNames[] = {
        "amd_fidelityfx_framegeneration_dx12.dll",
        "amd_fidelityfx_framegeneration_vk.dll",
        "amd_fidelityfx_dx12.dll",
        "amd_fidelityfx_vk.dll",
        "amd_fidelityfx_fg.dll",
        "ffx_frameinterpolation_x64.dll",
        "amd_fidelityfx_framegeneration.dll",
        "ffx_framegeneration.dll",
        "nvngx_dlssg.dll",
        "fsr3fg.dll",
        "fsr3mod.dll",
    };

    bool foundSupportedModule = false;
    for (size_t i = 0; i < _countof(ffxModules); ++i) {
        HMODULE hMod = GetModuleHandleW(ffxModules[i]);
        if (hMod) {
            if (!g_Initialized.load(std::memory_order_acquire) || g_HookedModule != hMod) {
                HookLog("FFX Hook: Found module %s at %p", ffxModuleNames[i], hMod);
            }
            g_FGCompat.SetFSRFGSupportPresent(true);
            if (InstallHooksForModule(hMod, ffxModuleNames[i])) {
                foundSupportedModule = true;
                continue;
            }
            // Module exists but has no FFX exports (e.g. real nvngx_dlssg.dll)
            static std::atomic<int> s_moduleWithoutExportsLogCount{0};
            const int logCount = s_moduleWithoutExportsLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 300) == 0) {
                HookLog("FFX Hook: Module %s has no FFX exports, continuing search (log=%d)", ffxModuleNames[i],
                        logCount);
            }
        }
    }

    if (foundSupportedModule) {
        g_Initialized.store(true, std::memory_order_release);
        g_NoModulesLogged.store(false, std::memory_order_release);
        return;
    }

    g_Initialized.store(false, std::memory_order_release);
    g_HookedModule = nullptr;
    g_DefaultPresentCallback = nullptr;

    if (!g_NoModulesLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLog("FFX Hook: No FFX modules found, hooks not installed");
    }

    // One-time diagnostic at 30th retry: enumerate loaded modules for debugging
    if (s_initCallCount == 30) {
        HookLogImportant("FFX Hook: Module enumeration diagnostic (call #%d):", s_initCallCount);
        std::vector<HMODULE> hMods;
        if (ce::EnumerateProcessModules(GetCurrentProcess(), hMods)) {
            int found = 0;
            for (size_t i = 0; i < hMods.size(); i++) {
                wchar_t modName[MAX_PATH];
                if (GetModuleFileNameW(hMods[i], modName, MAX_PATH)) {
                    std::wstring lower(modName);
                    for (auto& c : lower)
                        c = towlower(c);
                    if (lower.find(L"fidelity") != std::wstring::npos || lower.find(L"ffx") != std::wstring::npos ||
                        lower.find(L"framegen") != std::wstring::npos || lower.find(L"fsr") != std::wstring::npos ||
                        lower.find(L"amd_") != std::wstring::npos) {
                        char narrowName[MAX_PATH];
                        WideCharToMultiByte(CP_UTF8, 0, modName, -1, narrowName, MAX_PATH, NULL, NULL);
                        HookLogImportant("FFX Hook:   Loaded: %s", narrowName);
                        found++;
                    }
                }
            }
            if (found == 0) {
                HookLogImportant("FFX Hook:   No AMD/FFX/FSR modules among %zu inspected", hMods.size());
            }
        }
    }
}

bool IsInitialized() {
    return g_Initialized.load(std::memory_order_acquire);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_InitMutex);

    if (!g_Initialized.load(std::memory_order_acquire)) {
        return;
    }

    HookLog("FFX Hook: Shutting down...");

    // Remove IAT hooks
    if (g_HookedModule) {
        // Note: IATHook::RemoveHook would need to be implemented
        // For now, we just clear the state - hooks will naturally be cleaned up
        // when the DLL unloads
    }

    // Restore the FFX proxy-swapchain Present vtable hook (guarded: only touches the class vtable when it
    // is still readable and still points at CE's detour — the FFX module may already be unloaded here).
    DX12_RemoveFFXProxyPresentHook("FFX hook shutdown");

    // Restore client-owned pre-resolved pointer slots before clearing original export addresses.
    ce::ffx_cached_pointer_router::Shutdown();

    // Cleanup VEH breakpoint hook
    if (g_ffxConfigureVehHandle) {
        RemoveVectoredExceptionHandler(g_ffxConfigureVehHandle);
        g_ffxConfigureVehHandle = nullptr;
    }
    RestoreFfxConfigureBreakpointIfCurrent(g_ffxConfigureTarget.load(std::memory_order_acquire), "FFX hook shutdown");
    g_ffxConfigureVehInstalled = false;
    g_ffxConfigureVehArmed.store(false, std::memory_order_release);
    g_ffxConfigureTarget.store(nullptr, std::memory_order_release);

    g_Original_ffxCreateContext = nullptr;
    g_Original_ffxDestroyContext = nullptr;
    g_Original_ffxConfigure = nullptr;
    g_DurableCachedConfigureRouteActive.store(false, std::memory_order_release);
    g_HookedModule = nullptr;
    g_DefaultPresentCallback = nullptr;
    FFXHook_ClearSubstituteUiReRegistration();
    DX12_UnregisterNativeFSRSwapchainPresentationQueue(nullptr, "FFX hook shutdown");
    {
        std::lock_guard<std::mutex> contextLock(g_ContextMapMutex);
        g_ContextTypeMap.clear();
        g_FrameGenerationRoutingByContext.clear();
    }
    {
        std::lock_guard<std::mutex> bridgeLock(g_PresentCallbackBridgeMutex);
        g_PresentCallbackBridgeKeys.clear();
    }
    g_FGContextCount.store(0, std::memory_order_release);
    DX12_ClearNativeFSRStartupConfigureArming("FFX hook shutdown");
    g_FGCompat.SetFSRFGActive(false);
    g_FGCompat.SetFSRFGSupportPresent(false);
    g_NoModulesLogged.store(false, std::memory_order_release);
    g_Initialized.store(false, std::memory_order_release);

    HookLog("FFX Hook: Shutdown complete");
}

}  // namespace FFXHook
