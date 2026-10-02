#include "ffx_hook_internal.h"

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
    if (!ffx_hook_g_SubstReRegActive.load(std::memory_order_acquire) || !DX12_IsNativeFSRInternalNoCallbackCompositionActive()) {
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
    std::lock_guard<std::mutex> lock(ffx_hook_g_SubstReRegMutex);
    if (!ffx_hook_g_SubstReRegConfigure || !ffx_hook_g_SubstReRegContext) {
        return FFXSubstituteUiReRegistrationResult::kFailed;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_SubstReRegInFlightTid.store(GetCurrentThreadId(), std::memory_order_release);
    g_SubstReRegInFlightQpc.store(static_cast<uint64_t>(qpc.QuadPart), std::memory_order_release);
    const ffxReturnCode_t result =
        ffx_hook_g_SubstReRegConfigure(&ffx_hook_g_SubstReRegContext, reinterpret_cast<const ffxConfigureDescHeader*>(&ffx_hook_g_SubstReRegDesc));
    g_SubstReRegInFlightQpc.store(0, std::memory_order_release);
    g_SubstReRegInFlightTid.store(0, std::memory_order_release);
    if (result != ffx_hook_FFX_API_RETURN_OK) {
        HookLogImportant("FFX Hook: substitute UI-resource re-registration FAILED (ctx=%p result=%d)",
                         (void*)ffx_hook_g_SubstReRegContext, static_cast<int>(result));
        return FFXSubstituteUiReRegistrationResult::kFailed;
    }
    static std::atomic<int> s_reRegLog{0};
    const int n = s_reRegLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 10 || (n % 600) == 0) {
        HookLogImportant(
            "FFX Hook: re-registered CE substitute UI resource %p (ctx=%p tid=0x%04X) so AMD composites it over "
            "GTA's per-frame 1x1 (log=%d)",
            ffx_hook_g_SubstReRegDesc.uiResource.resource, (void*)ffx_hook_g_SubstReRegContext, GetCurrentThreadId(), n + 1);
    }
    return FFXSubstituteUiReRegistrationResult::kSucceeded;
}

// Freeze-dump snapshot of the re-assert bracket (paired with DX12_LogFFXProxyPresentHookFreezeDiagnostics).
void FFXHook_LogSubstituteReRegFreezeDiagnostics(const char* ffx_hook_reason) {
    HookLogImportant("FFX Hook: [subst-rereg-freeze-diag] %s — active=%d inFlightQpc=%llu inFlightTid=0x%04X",
                     ffx_hook_reason ? ffx_hook_reason : "freeze", ffx_hook_g_SubstReRegActive.load(std::memory_order_acquire) ? 1 : 0,
                     static_cast<unsigned long long>(g_SubstReRegInFlightQpc.load(std::memory_order_acquire)),
                     g_SubstReRegInFlightTid.load(std::memory_order_acquire));
}

// Stop re-registering when CE's substitute texture is released (device change / teardown) — the stored desc's
// resource pointer is then dangling. Called from ReleaseFFXUiCompositeInfra (dx12_hook.cpp).
void FFXHook_ClearSubstituteUiReRegistration() {
    std::lock_guard<std::mutex> lock(ffx_hook_g_SubstReRegMutex);
    ffx_hook_g_SubstReRegActive.store(false, std::memory_order_release);
    ffx_hook_g_SubstReRegContext = nullptr;
    ffx_hook_g_SubstReRegConfigure = nullptr;
    ffx_hook_g_SubstReRegDesc = {};
}

// Reset the one-shot VEH disarm and re-arm the breakpoint for the next FG-on transition.
// Called from DX12_OnNativeFSRFrameGenerationContextsDestroyed / ForceClearNativeFSRInternalNoCallbackComposition
// when FG turns off and no durable cached-pointer route exists. The next enabled ffxConfigure will fire the VEH
// once, detect no-callback mode, and disarm again — one VEH hit per FG-on transition, no sustained contention.
void FFXHook_ResetVehDisarmAndRearm() {
    if (ffx_hook_g_DurableCachedConfigureRouteActive.load(std::memory_order_acquire)) {
        ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.store(true, std::memory_order_release);
        void* durableTarget = ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire);
        RestoreFfxConfigureBreakpointIfCurrent(durableTarget, "durable cached ffxConfigure route remains active");
        HookLogImportant(
            "FFX Hook: Kept protected ffxConfigure VEH retired across FG context destruction because the durable "
            "cached-pointer route remains active (target=%p)",
            durableTarget);
        return;
    }
    ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.store(false, std::memory_order_release);
    void* target = ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire);
    if (target) {
        ArmFfxConfigureBreakpoint(reinterpret_cast<PfnFfxConfigure>(target), "protected official FFX runtime",
                                  "FG-off re-arm for next on-transition");
        HookLogImportant("FFX Hook: VEH disarm reset + re-armed for next FG-on transition (target=%p)", target);
    }
}
