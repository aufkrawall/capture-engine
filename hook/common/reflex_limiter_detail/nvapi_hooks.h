#pragma once

// Out-of-line ReflexLimiter member definitions. Included by reflex_limiter.h
// after the class body; kept inline so the hot path is unchanged.

#include "../reflex_limiter.h"

inline bool ReflexLimiter::Init() {
    // If already successfully initialized, return cached result
    if (available_.load(std::memory_order_acquire))
        return true;

    // If we already attempted init this session and nvapi wasn't there,
    // still allow re-checking (game may load nvapi64.dll later when user
    // enables Reflex in settings). Reset inited_ to permit re-entry.
    if (inited_.load(std::memory_order_acquire)) {
        // Re-check: has nvapi64.dll been loaded since our last attempt?
        HMODULE hNvApi = GetModuleHandleW(L"nvapi64.dll");
        if (!hNvApi)
            return false;
        // DLL is now loaded — reset so we can initialize
        inited_.store(false, std::memory_order_release);
    }

    inited_.store(true, std::memory_order_release);

    HMODULE hNvApi = GetModuleHandleW(L"nvapi64.dll");
    if (!hNvApi) {
        return false;
    }

    origQueryInterface_ =
        reinterpret_cast<PFN_NvAPI_QueryInterface>(GetProcAddress(hNvApi, "nvapi_QueryInterface"));
    if (!origQueryInterface_) {
        HookLogImportant("ReflexLimiter: nvapi64.dll loaded but nvapi_QueryInterface not found in exports");
        return false;
    }

    // Resolve original function pointers
    origSetSleepMode_ =
        reinterpret_cast<PFN_NvAPI_D3D_SetSleepMode>(origQueryInterface_(NVAPI_ID_D3D_SetSleepMode));
    origSleep_ = reinterpret_cast<PFN_NvAPI_D3D_Sleep>(origQueryInterface_(NVAPI_ID_D3D_Sleep));

    if (!origSetSleepMode_ || !origSleep_) {
        HookLogImportant("ReflexLimiter: nvapi_QueryInterface resolved but SetSleepMode=%p Sleep=%p (incomplete)",
                         (void*)origSetSleepMode_, (void*)origSleep_);
        return false;
    }

    // Cache the real SetSleepMode/Sleep entrypoints for direct inline hook forwarding.
    realSetSleepModeForHook_ = origSetSleepMode_;
    realSleepForHook_ = origSleep_;

    // NOTE: We intentionally do NOT inline-hook nvapi_QueryInterface.
    // In some titles (e.g. GTA V Enhanced) a code-byte patch on
    // nvapi_QueryInterface is detected by Streamline/DLSS FG or anti-tamper
    // validation, causing Reflex init to abort and DLSS FG to fail with the
    // pink-tint diagnostic. Explicit/manual Reflex limiting can still use a
    // caller-filtered IAT/GetProcAddress hook so game-owned Sleep calls see
    // wrappers while Streamline/DLSS FG modules keep original driver pointers.
    //
    // We also do not install the direct SetSleepMode/Sleep inline hooks
    // merely because CE's limiter is configured. The explicit limiter can
    // push and sleep through the original function pointers, while
    // activation/sleep observation for Streamline Reflex happens through
    // Streamline feature hooks. Keeping nvapi64.dll unmodified is the safer
    // default for DLSS FG startup validation.

    available_.store(true, std::memory_order_release);
    EnsureGameOwnedReflexHooks();
    HookLogImportant("ReflexLimiter: Ready (SetSleepMode=%p, Sleep=%p, inlineHooks=%d)", (void*)origSetSleepMode_,
                     (void*)origSleep_, AreInlineHooksInstalled() ? 1 : 0);
    return true;
}

inline void ReflexLimiter::EnsureNvAPIHooksInstalled() {
#if !REFLEX_IAT_HOOK_AVAILABLE
    (void)realSetSleepModeForHook_;
    (void)realSleepForHook_;
    return;
#else
    // This method is intentionally not called by the default CE-owned Reflex
    // limiter path. It remains available for direct NvAPI native-call
    // diagnostics, but installing it patches nvapi64.dll code bytes and can
    // be visible to DLSS FG / anti-tamper validation.
    if (targetIntervalUs_.load(std::memory_order_acquire) == 0) {
        static std::atomic<bool> s_loggedSkip{false};
        if (!s_loggedSkip.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("ReflexLimiter: Deferring SetSleepMode/Sleep inline hooks — no FPS cap configured");
        }
        return;
    }

    if (origSetSleepMode_ && !directSetSleepModeHooked_) {
        void* trampoline = nullptr;
        struct SetSleepModePublication {
            ReflexLimiter* limiter;
            PFN_NvAPI_D3D_SetSleepMode fallback;
        } publication{this, realSetSleepModeForHook_};
        auto publishSetSleepMode = [](void* published, void* context) {
            if (!context)
                return;
            auto* state = static_cast<SetSleepModePublication*>(context);
            auto* limiter = state->limiter;
            limiter->directSetSleepModeTrampoline_ =
                reinterpret_cast<PFN_NvAPI_D3D_SetSleepMode>(published);
            limiter->realSetSleepModeForHook_ = published ? limiter->directSetSleepModeTrampoline_ : state->fallback;
            limiter->directSetSleepModeHooked_ = published != nullptr;
        };
        if (InlineHook::InstallPublished(reinterpret_cast<void*>(origSetSleepMode_),
                                         reinterpret_cast<void*>(&ReflexDetour_SetSleepMode), &trampoline,
                                         publishSetSleepMode, &publication)) {
            HookLogImportant(
                "ReflexLimiter: Inline hook installed on NvAPI_D3D_SetSleepMode (target=%p, detour=%p, "
                "trampoline=%p)",
                (void*)origSetSleepMode_, (void*)&ReflexDetour_SetSleepMode, trampoline);
        } else {
            HookLogImportant("ReflexLimiter: Failed to install inline hook on NvAPI_D3D_SetSleepMode");
        }
    }

    if (origSleep_ && !directSleepHooked_) {
        void* trampoline = nullptr;
        struct SleepPublication {
            ReflexLimiter* limiter;
            PFN_NvAPI_D3D_Sleep fallback;
        } publication{this, realSleepForHook_};
        auto publishSleep = [](void* published, void* context) {
            if (!context)
                return;
            auto* state = static_cast<SleepPublication*>(context);
            auto* limiter = state->limiter;
            limiter->directSleepTrampoline_ = reinterpret_cast<PFN_NvAPI_D3D_Sleep>(published);
            limiter->realSleepForHook_ = published ? limiter->directSleepTrampoline_ : state->fallback;
            limiter->directSleepHooked_ = published != nullptr;
        };
        if (InlineHook::InstallPublished(reinterpret_cast<void*>(origSleep_),
                                         reinterpret_cast<void*>(&ReflexDetour_Sleep), &trampoline,
                                         publishSleep, &publication)) {
            HookLogImportant(
                "ReflexLimiter: Inline hook installed on NvAPI_D3D_Sleep (target=%p, detour=%p, trampoline=%p)",
                (void*)origSleep_, (void*)&ReflexDetour_Sleep, trampoline);
        } else {
            HookLogImportant("ReflexLimiter: Failed to install inline hook on NvAPI_D3D_Sleep");
        }
    }
#endif
}

inline NvAPI_Status ReflexLimiter::InterceptSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams) {
    auto forwardSetSleepMode = GetForwardSetSleepMode();
    if (!forwardSetSleepMode || !pParams)
        return NVAPI_ERROR;

    // Diagnostic: detect struct version mismatches early
    if (pParams->version != NV_SET_SLEEP_MODE_PARAMS_VER) {
        static std::atomic<int> s_versionMismatchLogCount{0};
        int logCount = s_versionMismatchLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 5) {
            HookLogImportant("ReflexLimiter: SetSleepMode version mismatch (got=0x%08X expected=0x%08X size=%zu)",
                             pParams->version, NV_SET_SLEEP_MODE_PARAMS_VER, sizeof(NV_SET_SLEEP_MODE_PARAMS));
        }
    }

    const uint32_t ourInterval = targetIntervalUs_.load(std::memory_order_acquire);

    // Version-aware copy: the game may pass a smaller struct than ours
    // (e.g. version 0x1002C = 44 bytes).  Copy only what the caller
    // provided and zero the rest to avoid reading adjacent stack memory.
    uint32_t callerSize = pParams->version & 0xFFFF;
    uint32_t ourSize = sizeof(NV_SET_SLEEP_MODE_PARAMS);
    if (callerSize != ourSize) {
        static std::atomic<int> s_sizeMismatchLogCount{0};
        int logCount = s_sizeMismatchLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 5) {
            HookLogImportant("ReflexLimiter: SetSleepMode struct size mismatch (caller=%u our=%u version=0x%08X)",
                             callerSize, ourSize, pParams->version);
        }
    }
    uint32_t copySize = (callerSize < ourSize) ? callerSize : ourSize;
    CopyMemory(&lastSleepModeParams_, pParams, copySize);
    if (copySize < ourSize) {
        ZeroMemory(reinterpret_cast<uint8_t*>(&lastSleepModeParams_) + copySize, ourSize - copySize);
    }
    hasLastSleepModeParams_.store(true, std::memory_order_release);

    // Detect game activation: game called with low-latency mode enabled
    if (pParams->bLowLatencyMode && !gameActivated_.load(std::memory_order_acquire)) {
        gameActivated_.store(true, std::memory_order_release);
        HookLogImportant(
            "ReflexLimiter: Game activated Reflex (minimumIntervalUs=%u, override=%u, boost=%u, markers=%u)",
            pParams->minimumIntervalUs, ourInterval, pParams->bLowLatencyBoost, pParams->bUseMarkersToOptimize);
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        const bool runtimeModeIsFSRFG = runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
        const bool runtimeOwnsSwapchain = DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
        if (ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(
                true, g_FGCompat.IsFSRFGApiActive(), runtimeModeIsFSRFG, runtimeOwnsSwapchain)) {
            HookLogImportant(
                "ReflexLimiter: Reflex activation requesting Streamline enable preparation "
                "(runtime=%s apiFSR=%d fgOwned=%d)",
                ce::fg_runtime::GetRuntimeModeName(runtimeMode), g_FGCompat.IsFSRFGApiActive() ? 1 : 0,
                runtimeOwnsSwapchain ? 1 : 0);
            DX12_PrepareForStreamlineEnableTransition();
        }
    } else if (!pParams->bLowLatencyMode && gameActivated_.load(std::memory_order_acquire)) {
        gameActivated_.store(false, std::memory_order_release);
        HookLogImportant("ReflexLimiter: Game deactivated Reflex");
    }

    // Store device for PushFpsLimit / Sleep
    if (pDev && lastDevice_ != pDev) {
        lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
    }
    lastDevice_ = pDev;

    MarkNativePacingSignal();

    // Override minimumIntervalUs if we have a target
    if (ourInterval > 0) {
        pParams->minimumIntervalUs = ourInterval;
    }

    if (!forwardSetSleepMode) {
        HookLogImportant("ReflexLimiter: SetSleepMode forward missing — no real or original pointer available");
        return NVAPI_ERROR;
    }

    NvAPI_Status status = forwardSetSleepMode(pDev, pParams);
    if (status == NVAPI_OK) {
        static std::atomic<bool> s_loggedSuccess{false};
        if (!s_loggedSuccess.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant(
                "ReflexLimiter: SetSleepMode forward succeeded (version=0x%08X intervalUs=%u boost=%u markers=%u)",
                pParams->version, pParams->minimumIntervalUs, pParams->bLowLatencyBoost,
                pParams->bUseMarkersToOptimize);
        }
    } else {
        static std::atomic<int> s_failLogCount{0};
        int failCount = s_failLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant(
                "ReflexLimiter: SetSleepMode forward failed (status=%d version=0x%08X intervalUs=%u boost=%u "
                "markers=%u)",
                status, pParams->version, pParams->minimumIntervalUs, pParams->bLowLatencyBoost,
                pParams->bUseMarkersToOptimize);
        }
    }
    return status;
}

inline NV_SET_SLEEP_MODE_PARAMS ReflexLimiter::BuildSleepModeParams(uint32_t intervalUs, bool forceLowLatency) const {
    NV_SET_SLEEP_MODE_PARAMS params{};
    if (hasLastSleepModeParams_.load(std::memory_order_acquire)) {
        params = lastSleepModeParams_;
    } else {
        params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        params.bLowLatencyMode = gameActivated_.load(std::memory_order_acquire) ? 1 : 0;
    }

    if (params.version == 0) {
        params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    }

    if (forceLowLatency) {
        params.bLowLatencyMode = 1;
    }
    params.minimumIntervalUs = intervalUs;
    return params;
}

inline void ReflexLimiter::ForceLowLatencyResetBeforeManualPush(PFN_NvAPI_D3D_SetSleepMode forwardSetSleepMode, uint32_t nextIntervalUs) {
    if (!forwardSetSleepMode || !lastDevice_) {
        return;
    }

    NV_SET_SLEEP_MODE_PARAMS params = BuildSleepModeParams(0, false);
    params.bLowLatencyMode = 0;
    params.bLowLatencyBoost = 0;
    params.bUseMarkersToOptimize = 0;
    params.minimumIntervalUs = 0;

    const NvAPI_Status status = forwardSetSleepMode(lastDevice_, &params);
    if (status == NVAPI_OK) {
        HookLogImportant(
            "ReflexLimiter: Re-armed manual FPS limit with low-latency reset before push "
            "(device=%p nextIntervalUs=%u version=0x%08X)",
            lastDevice_, nextIntervalUs, params.version);
    } else {
        HookLogImportant(
            "ReflexLimiter: Manual FPS-limit low-latency reset failed before push "
            "(status=%d device=%p nextIntervalUs=%u version=0x%08X)",
            status, lastDevice_, nextIntervalUs, params.version);
    }
}
