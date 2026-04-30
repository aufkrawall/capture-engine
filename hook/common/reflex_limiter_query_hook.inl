#pragma once

inline bool ReflexLimiter::IsSystemModulePath(const char* path) {
    return ce::overlay_compat::detail::ContainsInsensitive(path, "\\system32\\") ||
           ce::overlay_compat::detail::ContainsInsensitive(path, "\\syswow64\\");
}

inline bool ReflexLimiter::IsCaptureHookModulePath(const char* path) {
    return ce::overlay_compat::detail::ContainsInsensitive(path, "capture_hook") ||
           ce::overlay_compat::detail::ContainsInsensitive(path, "d3d12_wrappers") ||
           ce::overlay_compat::detail::ContainsInsensitive(path, "vk_layer_ce_overlay");
}

inline void ReflexLimiter::SetManualLimiterConfiguredOrActive(bool configured) {
    const bool wasConfigured = manualLimiterConfiguredOrActive_.exchange(configured, std::memory_order_acq_rel);
    if (configured && !wasConfigured) {
        EnsureGameOwnedReflexHooks();
    }
}

inline bool ReflexLimiter::IsManualLimiterConfiguredOrActive() const {
    if (manualLimiterConfiguredOrActive_.load(std::memory_order_acquire)) {
        return true;
    }

#ifndef VK_LAYER_CE_OVERLAY
    auto sharedMemoryWantsManualReflex = [](SharedMemoryLayout* shm) {
        if (!shm) {
            return false;
        }
        constexpr uint32_t kNativeMode = static_cast<uint32_t>(LimiterMode::kNative);
        return (shm->fpsLimiter.GetGeneralEnabled() && shm->fpsLimiter.GetGeneralFps() > 0 &&
                shm->fpsLimiter.GetGeneralLimiterMode() == kNativeMode) ||
               (shm->fpsLimiter.GetCaptureSyncEnabled() &&
                shm->fpsLimiter.GetCaptureSyncLimiterMode() == kNativeMode);
    };

    if (g_IPC && sharedMemoryWantsManualReflex(g_IPC->GetSharedMem())) {
        return true;
    }
    if (sharedMemoryWantsManualReflex(g_pSharedMem)) {
        return true;
    }
    if (g_pLocalConfig) {
        const auto& fps = g_pLocalConfig->fpsLimiter;
        return (fps.generalEnabled && fps.generalFps > 0 && fps.generalLimiterMode == LimiterMode::kNative) ||
               (fps.captureSyncEnabled && fps.captureSyncLimiterMode == LimiterMode::kNative);
    }
#endif

    return false;
}

inline void ReflexLimiter::EnsureGameOwnedReflexHooks() {
#if REFLEX_IAT_HOOK_AVAILABLE
    if (IsManualLimiterConfiguredOrActive()) {
        RegisterQueryInterfaceHook();
    }
#endif
}

#if REFLEX_IAT_HOOK_AVAILABLE

inline void ReflexLimiter::RegisterQueryInterfaceHook() {
    static std::atomic<bool> s_dynamicHookRegistered{false};
    if (!s_dynamicHookRegistered.exchange(true, std::memory_order_acq_rel)) {
        IATHook::RegisterDynamicHook("nvapi_QueryInterface", reinterpret_cast<void*>(&ReflexDetour_QueryInterface),
                                     reinterpret_cast<void**>(&origQueryInterface_));
        IATHook::InitializeGetProcAddressHook();
        HookLogImportant("ReflexLimiter: Registered filtered nvapi_QueryInterface GetProcAddress hook");
    }

    HMODULE hNvApi64 = GetModuleHandleW(L"nvapi64.dll");
    if (hNvApi64 && !origQueryInterface_) {
        origQueryInterface_ =
            reinterpret_cast<PFN_NvAPI_QueryInterface>(GetProcAddress(hNvApi64, "nvapi_QueryInterface"));
    }

    static std::atomic<bool> s_iatPatchAttempted{false};
    const bool anyNvApiLoaded = hNvApi64 || GetModuleHandleW(L"nvapi.dll");
    if (anyNvApiLoaded && !s_iatPatchAttempted.exchange(true, std::memory_order_acq_rel)) {
        void* original = nullptr;
        const bool patched64 = IATHook::PatchIATAllModules(
            "nvapi64.dll", "nvapi_QueryInterface", reinterpret_cast<void*>(&ReflexDetour_QueryInterface), &original);
        if (original && !origQueryInterface_) {
            origQueryInterface_ = reinterpret_cast<PFN_NvAPI_QueryInterface>(original);
        }
        original = nullptr;
        const bool patched32 = IATHook::PatchIATAllModules(
            "nvapi.dll", "nvapi_QueryInterface", reinterpret_cast<void*>(&ReflexDetour_QueryInterface), &original);
        if (original && !origQueryInterface_) {
            origQueryInterface_ = reinterpret_cast<PFN_NvAPI_QueryInterface>(original);
        }
        HookLogImportant("ReflexLimiter: Filtered nvapi_QueryInterface IAT hook attempted (nvapi64=%d nvapi=%d)",
                         patched64 ? 1 : 0, patched32 ? 1 : 0);
    }
}

inline bool ReflexLimiter::ShouldReturnWrapperToCaller(const void* callerAddress, char* callerPath,
                                                       size_t callerPathLen, const char** reasonOut) const {
    if (callerPath && callerPathLen > 0) {
        callerPath[0] = '\0';
    }
    if (reasonOut) {
        *reasonOut = "unknown";
    }

    const bool haveCallerPath =
        ce::overlay_compat::TryGetModulePathFromCodeAddress(callerAddress, callerPath, callerPathLen);
    const bool callerIsStreamlineRuntime =
        haveCallerPath && (ce::overlay_compat::IsStreamlineFrameGenerationModulePath(callerPath) ||
                           ce::overlay_compat::IsFFXFrameGenerationModulePath(callerPath));
    const bool callerIsThirdPartyOverlay =
        haveCallerPath && ce::overlay_compat::IsThirdPartyOverlayModulePath(callerPath);
    const bool callerIsSystemModule = !haveCallerPath || IsSystemModulePath(callerPath);
    const bool callerIsCaptureHookModule = haveCallerPath && IsCaptureHookModulePath(callerPath);
    const bool manualReflex = IsManualLimiterConfiguredOrActive();
    const bool shouldReturnWrapper = ce::fps_limiter_policy::ShouldReturnNvApiReflexWrapper(
        manualReflex, callerIsStreamlineRuntime, callerIsThirdPartyOverlay, callerIsSystemModule,
        callerIsCaptureHookModule);

    if (reasonOut) {
        if (!manualReflex) {
            *reasonOut = "manual_reflex_off";
        } else if (callerIsStreamlineRuntime) {
            *reasonOut = "streamline_or_fg_runtime";
        } else if (callerIsThirdPartyOverlay) {
            *reasonOut = "third_party_overlay";
        } else if (callerIsSystemModule) {
            *reasonOut = haveCallerPath ? "system_module" : "unknown_caller";
        } else if (callerIsCaptureHookModule) {
            *reasonOut = "capture_hook_module";
        } else {
            *reasonOut = "game_caller";
        }
    }

    return shouldReturnWrapper;
}

inline void* __cdecl ReflexLimiter::ReflexDetour_QueryInterface(uint32_t functionId) {
    auto& limiter = g_ReflexLimiter;
    auto queryInterface = limiter.origQueryInterface_;
    if (!queryInterface || queryInterface == &ReflexDetour_QueryInterface) {
        return nullptr;
    }

    static thread_local bool s_insideQueryInterface = false;
    if (s_insideQueryInterface) {
        return queryInterface(functionId);
    }

    s_insideQueryInterface = true;
    void* result = queryInterface(functionId);
    s_insideQueryInterface = false;

    if (functionId != NVAPI_ID_D3D_SetSleepMode && functionId != NVAPI_ID_D3D_Sleep) {
        return result;
    }

    if (functionId == NVAPI_ID_D3D_SetSleepMode && result && !limiter.origSetSleepMode_) {
        limiter.origSetSleepMode_ = reinterpret_cast<PFN_NvAPI_D3D_SetSleepMode>(result);
        limiter.realSetSleepModeForHook_ = limiter.origSetSleepMode_;
    } else if (functionId == NVAPI_ID_D3D_Sleep && result && !limiter.origSleep_) {
        limiter.origSleep_ = reinterpret_cast<PFN_NvAPI_D3D_Sleep>(result);
        limiter.realSleepForHook_ = limiter.origSleep_;
    }
    if (limiter.origSetSleepMode_ && limiter.origSleep_ && !limiter.available_.load(std::memory_order_acquire)) {
        limiter.available_.store(true, std::memory_order_release);
        HookLogImportant("ReflexLimiter: Ready through filtered nvapi_QueryInterface hook (SetSleepMode=%p, Sleep=%p)",
                         (void*)limiter.origSetSleepMode_, (void*)limiter.origSleep_);
    }
    if (!result) {
        return nullptr;
    }

    char callerPath[MAX_PATH] = {};
    const char* reason = nullptr;
    const void* callerAddress = __builtin_return_address(0);
    const bool returnWrapper =
        limiter.ShouldReturnWrapperToCaller(callerAddress, callerPath, sizeof(callerPath), &reason);

    if (returnWrapper) {
        static std::atomic<int> s_wrapperLogCount{0};
        const int logCount = s_wrapperLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 12) {
            HookLogImportant("ReflexLimiter: Returning %s wrapper from nvapi_QueryInterface to %s",
                             functionId == NVAPI_ID_D3D_SetSleepMode ? "SetSleepMode" : "Sleep",
                             callerPath[0] ? callerPath : "unknown");
        }
        return functionId == NVAPI_ID_D3D_SetSleepMode ? reinterpret_cast<void*>(&ReflexDetour_SetSleepMode)
                                                       : reinterpret_cast<void*>(&ReflexDetour_Sleep);
    }

    if (limiter.IsManualLimiterConfiguredOrActive()) {
        static std::atomic<int> s_passthroughLogCount{0};
        const int logCount = s_passthroughLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 12) {
            HookLogImportant("ReflexLimiter: Passing through %s nvapi_QueryInterface caller (%s, %s)",
                             functionId == NVAPI_ID_D3D_SetSleepMode ? "SetSleepMode" : "Sleep",
                             reason ? reason : "unknown", callerPath[0] ? callerPath : "unknown");
        }
    }

    return result;
}

#endif  // REFLEX_IAT_HOOK_AVAILABLE
