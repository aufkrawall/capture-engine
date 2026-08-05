#include "streamline_hook_internal.h"


bool MaybeHookDLSSGSetOptions(void*& streamline_hook_function,  bool fallbackToReturnedWrapper) {


    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slDLSSGSetOptions)) {
        streamline_hook_g_DLSSGSetOptionsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_DLSSGSetOptionsTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function), reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                                  streamline_hook_g_Original_slDLSSGSetOptions, streamline_hook_g_DLSSGSetOptionsHooked, streamline_hook_g_DLSSGSetOptionsTarget,
                                  "slDLSSGSetOptions");
            if (!streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGSetOptions),
                    streamline_hook_g_DLSSGSetOptionsImportFallbackAttemptedTarget, "slDLSSGSetOptions");
            }
        }
    }

    const bool hookReady = streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire);
    if (fallbackToReturnedWrapper && streamline_hook_function != reinterpret_cast<void*>(Hooked_slDLSSGSetOptions)) {
        if (!GetCallableOriginalDLSSGSetOptions() && !hookReady && !streamline_hook_g_Original_slDLSSGSetOptions) {
            streamline_hook_g_Original_slDLSSGSetOptions = reinterpret_cast<PFN_slDLSSGSetOptions>(streamline_hook_function);
        }
        if (ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(
                fallbackToReturnedWrapper, false, GetCallableOriginalDLSSGSetOptions() != nullptr)) {
            LogReturnedWrapperFallbackOnce(streamline_hook_g_DLSSGSetOptionsReturnedWrapperFallbackLogged, "slDLSSGSetOptions",
                                           streamline_hook_function, reinterpret_cast<void*>(Hooked_slDLSSGSetOptions), hookReady);
            streamline_hook_function = reinterpret_cast<void*>(Hooked_slDLSSGSetOptions);
            return true;
        }
    }

    return hookReady;

}

bool MaybeHookDLSSGGetState(void*& streamline_hook_function,  bool fallbackToReturnedWrapper) {


    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slDLSSGGetState)) {
        streamline_hook_g_DLSSGGetStateHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_DLSSGGetStateTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function), reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                                  streamline_hook_g_Original_slDLSSGGetState, streamline_hook_g_DLSSGGetStateHooked, streamline_hook_g_DLSSGGetStateTarget,
                                  "slDLSSGGetState");
            if (!streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGGetState), streamline_hook_g_DLSSGGetStateImportFallbackAttemptedTarget,
                    "slDLSSGGetState");
            }
        }
    }

    const bool hookReady = streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire);
    if (fallbackToReturnedWrapper && streamline_hook_function != reinterpret_cast<void*>(Hooked_slDLSSGGetState)) {
        if (!GetCallableOriginalDLSSGGetState() && !hookReady && !streamline_hook_g_Original_slDLSSGGetState) {
            streamline_hook_g_Original_slDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(streamline_hook_function);
        }
        if (ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(
                fallbackToReturnedWrapper, false, GetCallableOriginalDLSSGGetState() != nullptr)) {
            LogReturnedWrapperFallbackOnce(streamline_hook_g_DLSSGGetStateReturnedWrapperFallbackLogged, "slDLSSGGetState", streamline_hook_function,
                                           reinterpret_cast<void*>(Hooked_slDLSSGGetState), hookReady);
            streamline_hook_function = reinterpret_cast<void*>(Hooked_slDLSSGGetState);
            return true;
        }
    }

    return hookReady;

}

bool MaybeHookReflexSleep(void*& streamline_hook_function,  bool fallbackToReturnedWrapper) {


    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slReflexSleep)) {
        streamline_hook_g_ReflexSleepHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_ReflexSleepTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function), reinterpret_cast<void*>(Hooked_slReflexSleep),
                                  streamline_hook_g_Original_slReflexSleep, streamline_hook_g_ReflexSleepHooked, streamline_hook_g_ReflexSleepTarget, "slReflexSleep");
            if (!streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSleep), streamline_hook_g_ReflexSleepImportFallbackAttemptedTarget,
                    "slReflexSleep");
            }
        }
    }

    if (fallbackToReturnedWrapper && !streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire)) {
        if (!streamline_hook_g_Original_slReflexSleep) {
            streamline_hook_g_Original_slReflexSleep = reinterpret_cast<PFN_slReflexSleep>(streamline_hook_function);
        }
        LogReturnedWrapperFallbackOnce(streamline_hook_g_ReflexSleepReturnedWrapperFallbackLogged, "slReflexSleep", streamline_hook_function,
                                       reinterpret_cast<void*>(Hooked_slReflexSleep),
                                       streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire));
        streamline_hook_function = reinterpret_cast<void*>(Hooked_slReflexSleep);
        return true;
    }

    return streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire);

}

bool MaybeHookReflexSetOptions(void*& streamline_hook_function,  bool fallbackToReturnedWrapper) {


    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slReflexSetOptions)) {
        streamline_hook_g_ReflexSetOptionsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_ReflexSetOptionsTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function), reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                  streamline_hook_g_Original_slReflexSetOptions, streamline_hook_g_ReflexSetOptionsHooked, streamline_hook_g_ReflexSetOptionsTarget,
                                  "slReflexSetOptions");
            if (!streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetOptions),
                    streamline_hook_g_ReflexSetOptionsImportFallbackAttemptedTarget, "slReflexSetOptions");
            }
        }
    }

    if (fallbackToReturnedWrapper && !streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        if (!streamline_hook_g_Original_slReflexSetOptions) {
            streamline_hook_g_Original_slReflexSetOptions = reinterpret_cast<PFN_slReflexSetOptions>(streamline_hook_function);
        }
        LogReturnedWrapperFallbackOnce(streamline_hook_g_ReflexSetOptionsReturnedWrapperFallbackLogged, "slReflexSetOptions", streamline_hook_function,
                                       reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                       streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire));
        streamline_hook_function = reinterpret_cast<void*>(Hooked_slReflexSetOptions);
        return true;
    }

    return streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire);

}

bool MaybeHookReflexSetConstants(void*& streamline_hook_function,  bool fallbackToReturnedWrapper) {


    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slReflexSetConstants)) {
        streamline_hook_g_ReflexSetConstantsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_ReflexSetConstantsTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function),
                                  reinterpret_cast<void*>(Hooked_slReflexSetConstants), streamline_hook_g_Original_slReflexSetConstants,
                                  streamline_hook_g_ReflexSetConstantsHooked, streamline_hook_g_ReflexSetConstantsTarget, "slReflexSetConstants");
            if (!streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetConstants),
                    streamline_hook_g_ReflexSetConstantsImportFallbackAttemptedTarget, "slReflexSetConstants");
            }
        }
    }

    if (fallbackToReturnedWrapper && !streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        if (!streamline_hook_g_Original_slReflexSetConstants) {
            streamline_hook_g_Original_slReflexSetConstants = reinterpret_cast<PFN_slReflexSetConstants>(streamline_hook_function);
        }
        LogReturnedWrapperFallbackOnce(streamline_hook_g_ReflexSetConstantsReturnedWrapperFallbackLogged, "slReflexSetConstants",
                                       streamline_hook_function, reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                                       streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire));
        streamline_hook_function = reinterpret_cast<void*>(Hooked_slReflexSetConstants);
        return true;
    }

    return streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire);

}

bool TryResolveDLSSGFeatureHooks() {


    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        return false;
    }

    bool hookedAnything = false;

    if (!streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
        void* streamline_hook_function = nullptr;
        const slResult result = originalGetFeatureFunction(streamline_hook_kSLFeatureDLSSG, "slDLSSGSetOptions", streamline_hook_function);
        if (result == streamline_hook_kSlResultOk && streamline_hook_function) {
            const bool hooked = MaybeHookDLSSGSetOptions(streamline_hook_function, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_DLSSGSetOptionsProactiveFallbackLogged, "slDLSSGSetOptions", streamline_hook_function);
            }
        }
    }

    if (!streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
        void* streamline_hook_function = nullptr;
        const slResult result = originalGetFeatureFunction(streamline_hook_kSLFeatureDLSSG, "slDLSSGGetState", streamline_hook_function);
        if (result == streamline_hook_kSlResultOk && streamline_hook_function) {
            const bool hooked = MaybeHookDLSSGGetState(streamline_hook_function, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_DLSSGGetStateProactiveFallbackLogged, "slDLSSGGetState", streamline_hook_function);
            }
        }
    }

    return hookedAnything || streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ||
           streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire);

}

bool TryResolveReflexFeatureHooks() {


    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        return false;
    }

    bool hookedAnything = false;
    bool queriedSleep = false;
    bool queriedSetOptions = false;
    bool queriedSetConstants = false;
    slResult sleepResult = streamline_hook_kSlResultErrorInvalidState;
    slResult setOptionsResult = streamline_hook_kSlResultErrorInvalidState;

    slResult setConstantsResult = streamline_hook_kSlResultErrorInvalidState;
    void* sleepFunction = nullptr;
    void* setOptionsFunction = nullptr;
    void* setConstantsFunction = nullptr;

    if (!streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire)) {
        queriedSleep = true;
        sleepResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSleep", sleepFunction);
        if (sleepResult == streamline_hook_kSlResultOk && sleepFunction) {
            const bool hooked = MaybeHookReflexSleep(sleepFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_ReflexSleepProactiveFallbackLogged, "slReflexSleep", sleepFunction);
            }
        }
    }

    if (!streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        queriedSetOptions = true;
        setOptionsResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSetOptions", setOptionsFunction);
        if (setOptionsResult == streamline_hook_kSlResultOk && setOptionsFunction) {
            const bool hooked = MaybeHookReflexSetOptions(setOptionsFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_ReflexSetOptionsProactiveFallbackLogged, "slReflexSetOptions",
                                               setOptionsFunction);
            }
        }
    }

    if (!streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        queriedSetConstants = true;
        setConstantsResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSetConstants", setConstantsFunction);
        if (setConstantsResult == streamline_hook_kSlResultOk && setConstantsFunction) {
            const bool hooked = MaybeHookReflexSetConstants(setConstantsFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_ReflexSetConstantsProactiveFallbackLogged, "slReflexSetConstants",
                                               setConstantsFunction);
            }
        }
    }

    const bool hooksReady = hookedAnything || streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ||
                            streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ||
                            streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
    if (hooksReady) {
        static std::atomic<bool> s_loggedResolved{false};
        if (!s_loggedResolved.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "Streamline Hook: Reflex feature hooks resolved (sleepHooked=%d setOptionsHooked=%d "
                "setConstantsHooked=%d sleep=%p setOptions=%p setConstants=%p)",
                streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
                streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0, sleepFunction, setOptionsFunction,
                setConstantsFunction);
        }
    } else if (queriedSleep || queriedSetOptions || queriedSetConstants) {
        static std::atomic<bool> s_loggedUnavailable{false};
        if (!s_loggedUnavailable.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "Streamline Hook: Reflex feature functions not available yet via slGetFeatureFunction "
                "(sleep: queried=%d result=%d ptr=%p, setOptions: queried=%d result=%d ptr=%p, "
                "setConstants: queried=%d result=%d ptr=%p)",
                queriedSleep ? 1 : 0, sleepResult, sleepFunction, queriedSetOptions ? 1 : 0, setOptionsResult,
                setOptionsFunction, queriedSetConstants ? 1 : 0, setConstantsResult, setConstantsFunction);
        }
    }

    return hooksReady;

}

uint32_t QueryCapabilityMax(const slViewportHandle& viewport,  const slDLSSGOptions* streamline_hook_options) {


    if (!GetCallableOriginalDLSSGGetState() && !TryResolveDLSSGFeatureHooks()) {
        return 0;
    }
    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return 0;
    }

    slDLSSGState state;
    const slResult result = originalGetState(viewport, state, streamline_hook_options);
    if (result != streamline_hook_kSlResultOk || state.numFramesToGenerateMax == 0) {
        return 0;
    }

    const uint32_t viewportKey = GetViewportKey(viewport);
    CacheCapabilityMax(viewportKey, state.numFramesToGenerateMax);
    return state.numFramesToGenerateMax;

}

void RegisterDynamicHooksOnce() {


    if (streamline_hook_g_DynamicHooksRegistered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    IATHook::RegisterDynamicHookFiltered("slGetFeatureFunction", reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slGetFeatureFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slGetPluginFunction", reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slGetPluginFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetD3DDevice", reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slSetD3DDevice),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetTag", reinterpret_cast<void*>(Hooked_slSetTag),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slSetTag),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetTagForFrame", reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slSetTagForFrame),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slEvaluateFeature", reinterpret_cast<void*>(Hooked_slEvaluateFeature),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slEvaluateFeature),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGSetOptions),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGGetState),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSleep),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetOptions),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetConstants),
                                         IsStreamlineReflexDynamicHookModule);
    HookLogImportant(
        "Streamline Hook: Registered module-filtered dynamic hooks for core Streamline exports and owned feature "
        "exports");

}

bool InstallHooksForModule(HMODULE module,  const char* moduleNameOrPath) {


    if (!module || !IsStreamlineModuleName(moduleNameOrPath)) {
        return false;
    }

    g_FGCompat.SetStreamlineSupportPresent(true);

    RegisterDynamicHooksOnce();

    const char* moduleBaseName = GetModuleBaseName(moduleNameOrPath);
    const bool shouldHookCoreExports = ShouldHookStreamlineCoreExports(moduleBaseName);
    const uint32_t moduleBit = GetModuleMaskBit(moduleBaseName);
    const auto originalGetFeatureFunction =
        reinterpret_cast<PFN_slGetFeatureFunction>(GetProcAddress(module, "slGetFeatureFunction"));
    const auto originalGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(GetProcAddress(module, "slGetPluginFunction"));
    const auto originalSetD3DDevice = reinterpret_cast<PFN_slSetD3DDevice>(GetProcAddress(module, "slSetD3DDevice"));
    const auto originalSetTag = reinterpret_cast<PFN_slSetTag>(GetProcAddress(module, "slSetTag"));
    const auto originalSetTagForFrame =
        reinterpret_cast<PFN_slSetTagForFrame>(GetProcAddress(module, "slSetTagForFrame"));
    const auto originalEvaluateFeature =
        reinterpret_cast<PFN_slEvaluateFeature>(GetProcAddress(module, "slEvaluateFeature"));
    const auto originalDLSSGSetOptions =
        reinterpret_cast<PFN_slDLSSGSetOptions>(GetProcAddress(module, "slDLSSGSetOptions"));
    const auto originalDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(GetProcAddress(module, "slDLSSGGetState"));
    const auto originalReflexSleep = reinterpret_cast<PFN_slReflexSleep>(GetProcAddress(module, "slReflexSleep"));
    const auto originalReflexSetOptions =
        reinterpret_cast<PFN_slReflexSetOptions>(GetProcAddress(module, "slReflexSetOptions"));
    const auto originalReflexSetConstants =
        reinterpret_cast<PFN_slReflexSetConstants>(GetProcAddress(module, "slReflexSetConstants"));

    if (!originalGetFeatureFunction && !originalGetPluginFunction && !originalSetD3DDevice && !originalSetTag &&
        !originalSetTagForFrame && !originalEvaluateFeature && !originalDLSSGSetOptions && !originalDLSSGGetState &&
        !originalReflexSleep && !originalReflexSetOptions && !originalReflexSetConstants) {
        return false;
    }

    if (moduleBit != 0 && (streamline_hook_g_InstalledModuleMask.load(std::memory_order_acquire) & moduleBit) != 0) {
        // Self-heal for unload/reload generations when the unload notification
        // was unavailable: the mask claims this core module is hooked, but the
        // stored core targets must belong to the ARRIVING instance. If none
        // do, the mask refers to a previous unloaded generation (whose address
        // range may since have been re-mapped by a different module —
        // 20260612_003407 crash) and the fresh instance must be re-hooked.
        const auto targetWithinModule = [](std::atomic<void*>& targetSlot, HMODULE candidate) {
            if (!candidate) {
                return false;
            }
            void* target = targetSlot.load(std::memory_order_acquire);
            return ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(
                target, nullptr, reinterpret_cast<const void*>(candidate), GetModuleImageSizeBytes(candidate));
        };
        const bool anyCoreHookTargetWithinModule = targetWithinModule(streamline_hook_g_SLGetFeatureFunctionTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLGetPluginFunctionTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLSetD3DDeviceTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLSetTagTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLSetTagForFrameTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLEvaluateFeatureTarget, module);
        if (!ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(
                true, anyCoreHookTargetWithinModule)) {
            return false;
        }

        // Clear only the core slots that no longer belong to ANY live core
        // module instance; a still-loaded sibling core module's valid slots
        // must survive this self-heal.
        const HMODULE liveInterposer = GetModuleHandleA("sl.interposer.dll");
        const HMODULE liveCommon = GetModuleHandleA("sl.common.dll");
        struct CoreSlotView {
            const char* name;
            std::atomic<void*>* target;
            std::atomic<bool>* installed;
            void* volatile* original;
        };
        CoreSlotView coreSlots[] = {
            {"slGetFeatureFunction", &streamline_hook_g_SLGetFeatureFunctionTarget, &streamline_hook_g_SLGetFeatureFunctionHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slGetFeatureFunction)},
            {"slGetPluginFunction", &streamline_hook_g_SLGetPluginFunctionTarget, &streamline_hook_g_SLGetPluginFunctionHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slGetPluginFunction)},
            {"slSetD3DDevice", &streamline_hook_g_SLSetD3DDeviceTarget, &streamline_hook_g_SLSetD3DDeviceHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slSetD3DDevice)},
            {"slSetTag", &streamline_hook_g_SLSetTagTarget, &streamline_hook_g_SLSetTagHooked, reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slSetTag)},
            {"slSetTagForFrame", &streamline_hook_g_SLSetTagForFrameTarget, &streamline_hook_g_SLSetTagForFrameHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slSetTagForFrame)},
            {"slEvaluateFeature", &streamline_hook_g_SLEvaluateFeatureTarget, &streamline_hook_g_SLEvaluateFeatureHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slEvaluateFeature)},
        };
        int healedSlots = 0;
        for (CoreSlotView& slot : coreSlots) {
            void* target = slot.target->load(std::memory_order_acquire);
            if (!target || targetWithinModule(*slot.target, module) ||
                targetWithinModule(*slot.target, liveInterposer) || targetWithinModule(*slot.target, liveCommon)) {
                continue;
            }
            InterlockedExchangePointer(slot.original, nullptr);
            slot.target->store(nullptr, std::memory_order_release);
            slot.installed->store(false, std::memory_order_release);
            ++healedSlots;
        }
        HookLogImportant(
            "Streamline Hook: %s reloaded at %p but the installed-module mask refers to a previous unloaded "
            "generation — cleared %d stale core slot(s) and re-hooking the fresh instance (liveInterposer=%p "
            "liveCommon=%p)",
            moduleBaseName, module, healedSlots, liveInterposer, liveCommon);
        streamline_hook_g_InstalledModuleMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
        streamline_hook_g_IATPatchesMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
    }

    if (!shouldHookCoreExports && (originalGetFeatureFunction || originalGetPluginFunction || originalSetD3DDevice)) {
        LogSkippedStreamlineCoreExportsOnce(moduleBaseName, module, originalGetFeatureFunction != nullptr,
                                            originalGetPluginFunction != nullptr, originalSetD3DDevice != nullptr);
    }

    bool hookedAnything = false;
    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_ModuleHookMutex);

        if (shouldHookCoreExports && originalGetFeatureFunction) {
            if (!streamline_hook_g_Original_slGetFeatureFunction) {
                streamline_hook_g_Original_slGetFeatureFunction = originalGetFeatureFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetFeatureFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                                    streamline_hook_g_Original_slGetFeatureFunction, streamline_hook_g_SLGetFeatureFunctionHooked,
                                                    streamline_hook_g_SLGetFeatureFunctionTarget, "slGetFeatureFunction");
        }

        if (shouldHookCoreExports && originalGetPluginFunction) {
            if (!streamline_hook_g_Original_slGetPluginFunction) {
                streamline_hook_g_Original_slGetPluginFunction = originalGetPluginFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetPluginFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                                    streamline_hook_g_Original_slGetPluginFunction, streamline_hook_g_SLGetPluginFunctionHooked,
                                                    streamline_hook_g_SLGetPluginFunctionTarget, "slGetPluginFunction");
        }

        if (shouldHookCoreExports && originalSetD3DDevice) {
            if (!streamline_hook_g_Original_slSetD3DDevice) {
                streamline_hook_g_Original_slSetD3DDevice = originalSetD3DDevice;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetD3DDevice), reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                streamline_hook_g_Original_slSetD3DDevice, streamline_hook_g_SLSetD3DDeviceHooked, streamline_hook_g_SLSetD3DDeviceTarget, "slSetD3DDevice");
        }

        if (shouldHookCoreExports && originalSetTag) {
            if (!streamline_hook_g_Original_slSetTag) {
                streamline_hook_g_Original_slSetTag = originalSetTag;
            }

            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalSetTag), reinterpret_cast<void*>(Hooked_slSetTag),
                                      streamline_hook_g_Original_slSetTag, streamline_hook_g_SLSetTagHooked, streamline_hook_g_SLSetTagTarget, "slSetTag");
        }

        if (shouldHookCoreExports && originalSetTagForFrame) {
            if (!streamline_hook_g_Original_slSetTagForFrame) {
                streamline_hook_g_Original_slSetTagForFrame = originalSetTagForFrame;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetTagForFrame), reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                streamline_hook_g_Original_slSetTagForFrame, streamline_hook_g_SLSetTagForFrameHooked, streamline_hook_g_SLSetTagForFrameTarget, "slSetTagForFrame");
        }

        if (shouldHookCoreExports && originalEvaluateFeature) {
            if (!streamline_hook_g_Original_slEvaluateFeature) {
                streamline_hook_g_Original_slEvaluateFeature = originalEvaluateFeature;
            }

            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalEvaluateFeature),
                                      reinterpret_cast<void*>(Hooked_slEvaluateFeature), streamline_hook_g_Original_slEvaluateFeature,
                                      streamline_hook_g_SLEvaluateFeatureHooked, streamline_hook_g_SLEvaluateFeatureTarget, "slEvaluateFeature");
        }

        if (shouldHookCoreExports && moduleBit != 0 &&
            (streamline_hook_g_IATPatchesMask.load(std::memory_order_acquire) & moduleBit) == 0) {
            void* dummy = nullptr;
            if (originalGetFeatureFunction) {
                IATHook::PatchIATAllModules(moduleBaseName, "slGetFeatureFunction",
                                            reinterpret_cast<void*>(Hooked_slGetFeatureFunction), &dummy);
            }
            if (originalGetPluginFunction) {
                IATHook::PatchIATAllModules(moduleBaseName, "slGetPluginFunction",
                                            reinterpret_cast<void*>(Hooked_slGetPluginFunction), &dummy);
            }
            if (originalSetD3DDevice) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetD3DDevice",
                                            reinterpret_cast<void*>(Hooked_slSetD3DDevice), &dummy);
            }
            if (originalSetTag) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetTag", reinterpret_cast<void*>(Hooked_slSetTag),
                                            &dummy);
            }
            if (originalSetTagForFrame) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetTagForFrame",
                                            reinterpret_cast<void*>(Hooked_slSetTagForFrame), &dummy);
            }
            if (originalEvaluateFeature) {
                IATHook::PatchIATAllModules(moduleBaseName, "slEvaluateFeature",
                                            reinterpret_cast<void*>(Hooked_slEvaluateFeature), &dummy);
            }
            streamline_hook_g_IATPatchesMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }

        if (originalDLSSGSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                           "slDLSSGSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                reinterpret_cast<void*>(originalDLSSGSetOptions),
                reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGSetOptions), "slDLSSGSetOptions");
        }

        if (originalDLSSGGetState &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGGetState", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                reinterpret_cast<void*>(originalDLSSGGetState), reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGGetState),
                "slDLSSGGetState");
        }

        if (originalReflexSleep &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSleep", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                reinterpret_cast<void*>(originalReflexSleep), reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSleep),
                "slReflexSleep");
        }

        if (originalReflexSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                            "slReflexSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                reinterpret_cast<void*>(originalReflexSetOptions),
                reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetOptions), "slReflexSetOptions");
        }

        if (originalReflexSetConstants && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                              "slReflexSetConstants", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                reinterpret_cast<void*>(originalReflexSetConstants),
                reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetConstants), "slReflexSetConstants");
        }
    }

    if (hookedAnything) {
        if (moduleBit != 0) {
            streamline_hook_g_InstalledModuleMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }
        HookLogImportant("Streamline Hook: Installed hooks for %s (%p)", moduleBaseName, module);
    }
    return true;

}

bool OpenLoadedModuleSnapshotWithRetry(HANDLE& snapshot,  MODULEENTRY32& firstEntry,  DWORD& error,  int& attempts, 
                                       bool& failedOnFirstEntry) {


    snapshot = INVALID_HANDLE_VALUE;
    error = ERROR_SUCCESS;
    attempts = 0;
    failedOnFirstEntry = false;

    constexpr int kMaxSnapshotAttempts = 4;
    for (int attempt = 1; attempt <= kMaxSnapshotAttempts; ++attempt) {
        attempts = attempt;
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) &&
                attempt < kMaxSnapshotAttempts) {
                continue;
            }
            return false;
        }

        firstEntry = {};
        firstEntry.dwSize = sizeof(firstEntry);
        if (Module32First(snapshot, &firstEntry)) {
            error = ERROR_SUCCESS;
            return true;
        }

        error = GetLastError();
        failedOnFirstEntry = true;
        CloseHandle(snapshot);
        snapshot = INVALID_HANDLE_VALUE;
        if (!ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) ||
            attempt == kMaxSnapshotAttempts) {
            return false;
        }
    }

    return false;

}
