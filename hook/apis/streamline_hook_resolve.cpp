#include "streamline_hook_internal.h"

namespace {
constexpr size_t kMaxPinnedStreamlineFeatureQueryModules = 16;

// Pins an already-loaded module (refcount++) so it cannot be unmapped while CE calls into the
// Streamline runtime. Returns the same HMODULE on success, null when the module is gone or a
// fresh instance had to be loaded (a fresh instance would leave the interposer's plugin table
// stale, so it must never be used for the query).
HMODULE PinLoadedStreamlineModule(HMODULE module) {
    if (!module) {
        return nullptr;
    }
    char modulePath[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameA(module, modulePath, sizeof(modulePath));
    if (pathLength == 0 || pathLength >= sizeof(modulePath)) {
        return nullptr;
    }
    HMODULE pinned = LoadLibraryA(modulePath);
    if (pinned != module) {
        if (pinned) {
            FreeLibrary(pinned);
        }
        return nullptr;
    }
    return pinned;
}

// Guards a proactive feature-function query against the Streamline runtime being torn down.
// dx12_fg_switch_test unloads Streamline plugins around sl.interposer when switching FG modes
// (sessions 20260812_042259 and 20260813_160845), so sl.interposer's slGetFeatureFunction can
// dispatch into an already-unmapped plugin -> DEP execute violation. The guard pins EVERY loaded
// sl.* module for the duration of the query, fails closed while a teardown is in flight, and
// rejects the query when a tracked sl.* unload started between the liveness check and the pins
// (generation counter).
class ScopedStreamlineFeatureQueryGuard {
public:
    explicit ScopedStreamlineFeatureQueryGuard(const char* featureModuleName) {
        if (!featureModuleName || !featureModuleName[0]) {
            return;
        }
        if (streamline_hook_g_StreamlineTeardownInFlight.load(std::memory_order_acquire)) {
            return;
        }
        const uint64_t generationBefore =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire);
        HMODULE featureModule = GetModuleHandleA(featureModuleName);
        HMODULE interposerModule = GetModuleHandleA("sl.interposer.dll");
        if (!featureModule || !interposerModule) {
            return;  // no plugin / no interposer: nothing to resolve through
        }
        // Pin EVERY loaded Streamline runtime module, not only the interposer and the queried
        // feature plugin: sl.interposer's slGetFeatureFunction walks its registered plugin table,
        // and any unpinned sl.* plugin can be unmapped mid-query (session 20260813_160845
        // dispatched into unmapped memory while a Reflex query held only sl.reflex + interposer
        // pins). Pinning the full loaded set makes any dispatch target unable to unmap for the
        // duration of the query.
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        MODULEENTRY32 entry = {};
        DWORD snapshotError = ERROR_SUCCESS;
        int snapshotAttempts = 0;
        bool failedOnFirstEntry = false;
        if (!OpenLoadedModuleSnapshotWithRetry(snapshot, entry, snapshotError, snapshotAttempts,
                                               failedOnFirstEntry)) {
            return;
        }
        bool interposerPinned = false;
        bool featureModulePinned = false;
        do {
            const char* moduleNameOrPath = entry.szExePath[0] != '\0' ? entry.szExePath : entry.szModule;
            if (!ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath)) {
                continue;
            }
            if (pinCount_ >= kMaxPinnedStreamlineFeatureQueryModules) {
                ReleasePins();
                CloseHandle(snapshot);
                return;
            }
            HMODULE pinned = PinLoadedStreamlineModule(entry.hModule);
            if (!pinned) {
                ReleasePins();
                CloseHandle(snapshot);
                return;
            }
            pins_[pinCount_++] = pinned;
            if (pinned == interposerModule) {
                interposerPinned = true;
            }
            if (pinned == featureModule) {
                featureModulePinned = true;
            }
        } while (Module32Next(snapshot, &entry));
        CloseHandle(snapshot);
        if (!interposerPinned || !featureModulePinned) {
            ReleasePins();
            return;
        }
        if (streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire) !=
            generationBefore) {
            // A teardown began between the liveness check and the pins; the pinned instances may
            // already be stale. Fail closed and let the next module-load retry re-attempt.
            ReleasePins();
            return;
        }
        valid_ = true;
    }

    ~ScopedStreamlineFeatureQueryGuard() {
        ReleasePins();
    }

    bool IsValid() const {
        return valid_;
    }

private:
    void ReleasePins() {
        for (size_t i = 0; i < pinCount_; ++i) {
            FreeLibrary(pins_[i]);
            pins_[i] = nullptr;
        }
        pinCount_ = 0;
        valid_ = false;
    }

    HMODULE pins_[kMaxPinnedStreamlineFeatureQueryModules] = {};
    size_t pinCount_ = 0;
    bool valid_ = false;
};

void LogSkippedFeatureResolutionForTeardownOnce(const char* featureModuleName, const char* source) {
    static std::atomic<int> s_teardownSkipLogCount{0};
    const int logCount = s_teardownSkipLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 100) == 0) {
        HookLogImportant(
            "Streamline Hook: Skipping %s feature resolution — Streamline runtime is unloading or "
            "already unloaded (featureModule=%s log=%d); the next module load re-attempts",
            source ? source : "proactive", featureModuleName ? featureModuleName : "unknown", logCount + 1);
    }
}
}  // namespace


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


bool TryResolveDLSSGFeatureHooks(bool proactiveScan) {


    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        return false;
    }
    if (streamline_hook_g_StreamlineTeardownInFlight.load(std::memory_order_acquire)) {
        return false;
    }
    // Function scope is intentional: every sl.* module must remain pinned until all
    // slGetFeatureFunction dispatches and returned-target validation below have finished.
    // Keeping this guard inside the conditional would destroy it before the first query.
    ScopedStreamlineFeatureQueryGuard teardownGuard(proactiveScan ? "sl.dlss_g.dll" : nullptr);
    if (proactiveScan) {
        // The HookThread's loaded-module scan can race with the app tearing the Streamline
        // runtime down (DLSS -> FSR switch): the interposer's slGetFeatureFunction dispatches
        // into sl.dlss_g, which the runtime unloads BEFORE the interposer (crash 20260812_042259:
        // DEP at a freed sl.dlss_g address from sl_interposer!slGetFeatureFunction+0x162). Pin
        // every Streamline module for the queries and fail closed when a teardown is in flight.
        if (!teardownGuard.IsValid()) {
            LogSkippedFeatureResolutionForTeardownOnce("sl.dlss_g.dll", "DLSS-G");
            return false;
        }
    } else if (!GetModuleHandleA("sl.dlss_g.dll")) {
        // Runtime-internal callers (no loader calls allowed): if the DLSS-G plugin is not
        // loaded there is nothing to resolve and the interposer query would be pointless (and,
        // with a stale plugin table, unsafe). GetModuleHandle is DllMain-safe.
        return false;
    }

    bool hookedAnything = false;

    if (!streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
        void* streamline_hook_function = nullptr;
        const uint64_t queryGenerationBefore =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire);
        const slResult result = originalGetFeatureFunction(streamline_hook_kSLFeatureDLSSG, "slDLSSGSetOptions", streamline_hook_function);
        const bool teardownObservedDuringQuery =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire) != queryGenerationBefore;
        if (result == streamline_hook_kSlResultOk && streamline_hook_function && !teardownObservedDuringQuery &&
            DoesAddressBelongToLoadedModule(streamline_hook_function, nullptr, nullptr, 0, nullptr)) {
            const bool hooked = MaybeHookDLSSGSetOptions(streamline_hook_function, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_DLSSGSetOptionsProactiveFallbackLogged, "slDLSSGSetOptions", streamline_hook_function);
            }
        }
    }

    if (!streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
        void* streamline_hook_function = nullptr;
        const uint64_t queryGenerationBefore =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire);
        const slResult result = originalGetFeatureFunction(streamline_hook_kSLFeatureDLSSG, "slDLSSGGetState", streamline_hook_function);
        const bool teardownObservedDuringQuery =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire) != queryGenerationBefore;
        if (result == streamline_hook_kSlResultOk && streamline_hook_function && !teardownObservedDuringQuery &&
            DoesAddressBelongToLoadedModule(streamline_hook_function, nullptr, nullptr, 0, nullptr)) {
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


bool TryResolveReflexFeatureHooks(bool proactiveScan) {


    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        return false;
    }
    if (streamline_hook_g_StreamlineTeardownInFlight.load(std::memory_order_acquire)) {
        return false;
    }
    // See the DLSS-G resolver: this guard must outlive every feature query in this function.
    ScopedStreamlineFeatureQueryGuard teardownGuard(proactiveScan ? "sl.reflex.dll" : nullptr);
    if (proactiveScan) {
        if (!teardownGuard.IsValid()) {
            LogSkippedFeatureResolutionForTeardownOnce("sl.reflex.dll", "Reflex");
            return false;
        }
    } else if (!GetModuleHandleA("sl.reflex.dll")) {
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
        const uint64_t queryGenerationBefore =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire);
        sleepResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSleep", sleepFunction);
        const bool teardownObservedDuringQuery =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire) != queryGenerationBefore;
        if (sleepResult == streamline_hook_kSlResultOk && sleepFunction && !teardownObservedDuringQuery &&
            DoesAddressBelongToLoadedModule(sleepFunction, nullptr, nullptr, 0, nullptr)) {
            const bool hooked = MaybeHookReflexSleep(sleepFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_ReflexSleepProactiveFallbackLogged, "slReflexSleep", sleepFunction);
            }
        }
    }

    if (!streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        queriedSetOptions = true;
        const uint64_t queryGenerationBefore =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire);
        setOptionsResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSetOptions", setOptionsFunction);
        const bool teardownObservedDuringQuery =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire) != queryGenerationBefore;
        if (setOptionsResult == streamline_hook_kSlResultOk && setOptionsFunction && !teardownObservedDuringQuery &&
            DoesAddressBelongToLoadedModule(setOptionsFunction, nullptr, nullptr, 0, nullptr)) {
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
        const uint64_t queryGenerationBefore =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire);
        setConstantsResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSetConstants", setConstantsFunction);
        const bool teardownObservedDuringQuery =
            streamline_hook_g_StreamlineModuleUnloadGeneration.load(std::memory_order_acquire) != queryGenerationBefore;
        if (setConstantsResult == streamline_hook_kSlResultOk && setConstantsFunction && !teardownObservedDuringQuery &&
            DoesAddressBelongToLoadedModule(setConstantsFunction, nullptr, nullptr, 0, nullptr)) {
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
