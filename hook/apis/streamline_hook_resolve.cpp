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

    if (!streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) &&
        streamline_hook_g_ReflexSetConstantsUnavailableQueries.load(std::memory_order_acquire) <
            kReflexSetConstantsUnavailableQueryLimit) {
        queriedSetConstants = true;
        setConstantsResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSetConstants", setConstantsFunction);
        if (setConstantsResult == streamline_hook_kSlResultOk && setConstantsFunction) {
            streamline_hook_g_ReflexSetConstantsUnavailableQueries.store(0, std::memory_order_release);
            const bool hooked = MaybeHookReflexSetConstants(setConstantsFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_ReflexSetConstantsProactiveFallbackLogged, "slReflexSetConstants",
                                               setConstantsFunction);
            }
        } else {
            // The runtime never provides this export; bound the failed queries
            // so the late-inject retry loop does not re-scan forever.
            streamline_hook_g_ReflexSetConstantsUnavailableQueries.fetch_add(1, std::memory_order_acq_rel);
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
