    slResult setConstantsResult = kSlResultErrorInvalidState;
    void* sleepFunction = nullptr;
    void* setOptionsFunction = nullptr;
    void* setConstantsFunction = nullptr;

    if (!g_ReflexSleepHooked.load(std::memory_order_acquire)) {
        queriedSleep = true;
        sleepResult = originalGetFeatureFunction(kSLFeatureReflex, "slReflexSleep", sleepFunction);
        if (sleepResult == kSlResultOk && sleepFunction) {
            const bool hooked = MaybeHookReflexSleep(sleepFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !g_ReflexSleepHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_ReflexSleepProactiveFallbackLogged, "slReflexSleep", sleepFunction);
            }
        }
    }

    if (!g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        queriedSetOptions = true;
        setOptionsResult = originalGetFeatureFunction(kSLFeatureReflex, "slReflexSetOptions", setOptionsFunction);
        if (setOptionsResult == kSlResultOk && setOptionsFunction) {
            const bool hooked = MaybeHookReflexSetOptions(setOptionsFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_ReflexSetOptionsProactiveFallbackLogged, "slReflexSetOptions",
                                               setOptionsFunction);
            }
        }
    }

    if (!g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        queriedSetConstants = true;
        setConstantsResult = originalGetFeatureFunction(kSLFeatureReflex, "slReflexSetConstants", setConstantsFunction);
        if (setConstantsResult == kSlResultOk && setConstantsFunction) {
            const bool hooked = MaybeHookReflexSetConstants(setConstantsFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_ReflexSetConstantsProactiveFallbackLogged, "slReflexSetConstants",
                                               setConstantsFunction);
            }
        }
    }

    const bool hooksReady = hookedAnything || g_ReflexSleepHooked.load(std::memory_order_acquire) ||
                            g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ||
                            g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
    if (hooksReady) {
        static std::atomic<bool> s_loggedResolved{false};
        if (!s_loggedResolved.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "Streamline Hook: Reflex feature hooks resolved (sleepHooked=%d setOptionsHooked=%d "
                "setConstantsHooked=%d sleep=%p setOptions=%p setConstants=%p)",
                g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
                g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0, sleepFunction, setOptionsFunction,
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

uint32_t QueryCapabilityMax(const slViewportHandle& viewport, const slDLSSGOptions* options) {
    if (!GetCallableOriginalDLSSGGetState() && !TryResolveDLSSGFeatureHooks()) {
        return 0;
    }
    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return 0;
    }

    slDLSSGState state;
    const slResult result = originalGetState(viewport, state, options);
    if (result != kSlResultOk || state.numFramesToGenerateMax == 0) {
        return 0;
    }

    const uint32_t viewportKey = GetViewportKey(viewport);
    CacheCapabilityMax(viewportKey, state.numFramesToGenerateMax);
    return state.numFramesToGenerateMax;
}

void RegisterDynamicHooksOnce() {
    if (g_DynamicHooksRegistered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    IATHook::RegisterDynamicHookFiltered("slGetFeatureFunction", reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                         reinterpret_cast<void**>(&g_Original_slGetFeatureFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slGetPluginFunction", reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                         reinterpret_cast<void**>(&g_Original_slGetPluginFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetD3DDevice", reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                                         reinterpret_cast<void**>(&g_Original_slSetD3DDevice),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetTag", reinterpret_cast<void*>(Hooked_slSetTag),
                                         reinterpret_cast<void**>(&g_Original_slSetTag),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetTagForFrame", reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                                         reinterpret_cast<void**>(&g_Original_slSetTagForFrame),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slEvaluateFeature", reinterpret_cast<void*>(Hooked_slEvaluateFeature),
                                         reinterpret_cast<void**>(&g_Original_slEvaluateFeature),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                                         reinterpret_cast<void**>(&g_Original_slDLSSGSetOptions),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                                         reinterpret_cast<void**>(&g_Original_slDLSSGGetState),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                                         reinterpret_cast<void**>(&g_Original_slReflexSleep),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                         reinterpret_cast<void**>(&g_Original_slReflexSetOptions),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                                         reinterpret_cast<void**>(&g_Original_slReflexSetConstants),
                                         IsStreamlineReflexDynamicHookModule);
    HookLogImportant(
        "Streamline Hook: Registered module-filtered dynamic hooks for core Streamline exports and owned feature "
        "exports");
}

bool InstallHooksForModule(HMODULE module, const char* moduleNameOrPath) {
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

    if (moduleBit != 0 && (g_InstalledModuleMask.load(std::memory_order_acquire) & moduleBit) != 0) {
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
        const bool anyCoreHookTargetWithinModule = targetWithinModule(g_SLGetFeatureFunctionTarget, module) ||
                                                   targetWithinModule(g_SLGetPluginFunctionTarget, module) ||
                                                   targetWithinModule(g_SLSetD3DDeviceTarget, module) ||
                                                   targetWithinModule(g_SLSetTagTarget, module) ||
                                                   targetWithinModule(g_SLSetTagForFrameTarget, module) ||
                                                   targetWithinModule(g_SLEvaluateFeatureTarget, module);
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
            {"slGetFeatureFunction", &g_SLGetFeatureFunctionTarget, &g_SLGetFeatureFunctionHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slGetFeatureFunction)},
            {"slGetPluginFunction", &g_SLGetPluginFunctionTarget, &g_SLGetPluginFunctionHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slGetPluginFunction)},
            {"slSetD3DDevice", &g_SLSetD3DDeviceTarget, &g_SLSetD3DDeviceHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slSetD3DDevice)},
            {"slSetTag", &g_SLSetTagTarget, &g_SLSetTagHooked, reinterpret_cast<void* volatile*>(&g_Original_slSetTag)},
            {"slSetTagForFrame", &g_SLSetTagForFrameTarget, &g_SLSetTagForFrameHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slSetTagForFrame)},
            {"slEvaluateFeature", &g_SLEvaluateFeatureTarget, &g_SLEvaluateFeatureHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slEvaluateFeature)},
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
        g_InstalledModuleMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
        g_IATPatchesMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
    }

    if (!shouldHookCoreExports && (originalGetFeatureFunction || originalGetPluginFunction || originalSetD3DDevice)) {
        LogSkippedStreamlineCoreExportsOnce(moduleBaseName, module, originalGetFeatureFunction != nullptr,
                                            originalGetPluginFunction != nullptr, originalSetD3DDevice != nullptr);
    }

    bool hookedAnything = false;
    {
        std::lock_guard<std::mutex> lock(g_ModuleHookMutex);

        if (shouldHookCoreExports && originalGetFeatureFunction) {
            if (!g_Original_slGetFeatureFunction) {
                g_Original_slGetFeatureFunction = originalGetFeatureFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetFeatureFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                                    g_Original_slGetFeatureFunction, g_SLGetFeatureFunctionHooked,
                                                    g_SLGetFeatureFunctionTarget, "slGetFeatureFunction");
        }

        if (shouldHookCoreExports && originalGetPluginFunction) {
            if (!g_Original_slGetPluginFunction) {
                g_Original_slGetPluginFunction = originalGetPluginFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetPluginFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                                    g_Original_slGetPluginFunction, g_SLGetPluginFunctionHooked,
                                                    g_SLGetPluginFunctionTarget, "slGetPluginFunction");
        }

        if (shouldHookCoreExports && originalSetD3DDevice) {
            if (!g_Original_slSetD3DDevice) {
                g_Original_slSetD3DDevice = originalSetD3DDevice;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetD3DDevice), reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                g_Original_slSetD3DDevice, g_SLSetD3DDeviceHooked, g_SLSetD3DDeviceTarget, "slSetD3DDevice");
        }

        if (shouldHookCoreExports && originalSetTag) {
            if (!g_Original_slSetTag) {
                g_Original_slSetTag = originalSetTag;
            }

            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalSetTag), reinterpret_cast<void*>(Hooked_slSetTag),
                                      g_Original_slSetTag, g_SLSetTagHooked, g_SLSetTagTarget, "slSetTag");
        }

        if (shouldHookCoreExports && originalSetTagForFrame) {
            if (!g_Original_slSetTagForFrame) {
                g_Original_slSetTagForFrame = originalSetTagForFrame;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetTagForFrame), reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                g_Original_slSetTagForFrame, g_SLSetTagForFrameHooked, g_SLSetTagForFrameTarget, "slSetTagForFrame");
        }

        if (shouldHookCoreExports && originalEvaluateFeature) {
            if (!g_Original_slEvaluateFeature) {
                g_Original_slEvaluateFeature = originalEvaluateFeature;
            }

            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalEvaluateFeature),
                                      reinterpret_cast<void*>(Hooked_slEvaluateFeature), g_Original_slEvaluateFeature,
                                      g_SLEvaluateFeatureHooked, g_SLEvaluateFeatureTarget, "slEvaluateFeature");
        }

        if (shouldHookCoreExports && moduleBit != 0 &&
            (g_IATPatchesMask.load(std::memory_order_acquire) & moduleBit) == 0) {
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
            g_IATPatchesMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }

        if (originalDLSSGSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                           "slDLSSGSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                reinterpret_cast<void*>(originalDLSSGSetOptions),
                reinterpret_cast<void**>(&g_Original_slDLSSGSetOptions), "slDLSSGSetOptions");
        }

        if (originalDLSSGGetState &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGGetState", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                reinterpret_cast<void*>(originalDLSSGGetState), reinterpret_cast<void**>(&g_Original_slDLSSGGetState),
                "slDLSSGGetState");
        }

        if (originalReflexSleep &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSleep", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                reinterpret_cast<void*>(originalReflexSleep), reinterpret_cast<void**>(&g_Original_slReflexSleep),
                "slReflexSleep");
        }

        if (originalReflexSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                            "slReflexSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                reinterpret_cast<void*>(originalReflexSetOptions),
                reinterpret_cast<void**>(&g_Original_slReflexSetOptions), "slReflexSetOptions");
        }

        if (originalReflexSetConstants && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                              "slReflexSetConstants", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                reinterpret_cast<void*>(originalReflexSetConstants),
                reinterpret_cast<void**>(&g_Original_slReflexSetConstants), "slReflexSetConstants");
        }
    }

    if (hookedAnything) {
        if (moduleBit != 0) {
            g_InstalledModuleMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }
        HookLogImportant("Streamline Hook: Installed hooks for %s (%p)", moduleBaseName, module);
    }
    return true;
}

bool OpenLoadedModuleSnapshotWithRetry(HANDLE& snapshot, MODULEENTRY32& firstEntry, DWORD& error, int& attempts,
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

bool ScanLoadedStreamlineModules() {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    MODULEENTRY32 entry = {};
    DWORD error = ERROR_SUCCESS;
    int attempts = 0;
    bool failedOnFirstEntry = false;
    if (!OpenLoadedModuleSnapshotWithRetry(snapshot, entry, error, attempts, failedOnFirstEntry)) {
        if (!g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                failedOnFirstEntry
                    ? "Streamline Hook: Loaded-module enumeration was empty for feature hooks error=%lu attempts=%d "
                      "retryable=%d"
                    : "Streamline Hook: Failed to enumerate loaded modules for feature hooks error=%lu attempts=%d "
                      "retryable=%d",
                static_cast<unsigned long>(error), attempts,
                ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) ? 1
                                                                                                                  : 0);
        }
        return false;
    }

    g_ModuleSnapshotFailureLogged.store(false, std::memory_order_release);

    bool foundModule = false;
    size_t streamlineModuleCount = 0;
    size_t hookedModuleCount = 0;
    do {
        const char* moduleNameOrPath = entry.szExePath[0] != '\0' ? entry.szExePath : entry.szModule;
        if (!ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath)) {
            continue;
        }

        foundModule = true;
        ++streamlineModuleCount;
        g_FGCompat.SetStreamlineSupportPresent(true);
        if (InstallHooksForModule(entry.hModule, moduleNameOrPath)) {
            ++hookedModuleCount;
        }
    } while (Module32Next(snapshot, &entry));

    const DWORD iterationError = GetLastError();
    CloseHandle(snapshot);

    if (attempts > 1 && !g_ModuleSnapshotRetrySuccessLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module snapshot recovered after transient retry (attempts=%d modules=%zu "
            "hooked=%zu)",
            attempts, streamlineModuleCount, hookedModuleCount);
    }
    if (iterationError != ERROR_SUCCESS && iterationError != ERROR_NO_MORE_FILES &&
        !g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module enumeration ended unexpectedly for feature hooks error=%lu "
            "(modules=%zu hooked=%zu)",
            static_cast<unsigned long>(iterationError), streamlineModuleCount, hookedModuleCount);
    }
    return foundModule;
}

bool AreReflexFeatureHooksComplete() {
    return g_ReflexSleepHooked.load(std::memory_order_acquire) &&
           g_ReflexSetOptionsHooked.load(std::memory_order_acquire) &&
           g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
}

void RetryResolveReflexFeatureHooksForRuntimeActivity(const char* source) {
    if (AreReflexFeatureHooksComplete()) {
        return;
    }

    constexpr ULONGLONG kRetryIntervalMs = 2500;
    const ULONGLONG nowMs = GetTickCount64();
    ULONGLONG previousMs = g_ReflexFeatureHookRetryLastMs.load(std::memory_order_acquire);
    if (previousMs != 0 && nowMs >= previousMs && (nowMs - previousMs) < kRetryIntervalMs) {
        return;
    }

    if (!g_ReflexFeatureHookRetryLastMs.compare_exchange_strong(previousMs, nowMs, std::memory_order_acq_rel,
                                                                std::memory_order_acquire)) {
        return;
    }

    const bool foundModule = ScanLoadedStreamlineModules();
    const bool resolved = TryResolveReflexFeatureHooks();
    static std::atomic<int> s_lateReflexRetryLogCount{0};
    const int logCount = s_lateReflexRetryLogCount.fetch_add(1, std::memory_order_relaxed);
    if (resolved || logCount < 10 || (logCount % 24) == 0) {
        HookLogImportant(
            "Streamline Hook: Late Reflex feature hook retry during DLSSG runtime activity "
            "(source=%s foundModule=%d resolved=%d sleepHooked=%d setOptionsHooked=%d setConstantsHooked=%d "
            "manualLimiter=%d targetIntervalUs=%u)",
            source ? source : "unknown", foundModule ? 1 : 0, resolved ? 1 : 0,
            g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexLimiter.IsManualLimiterConfiguredOrActive() ? 1 : 0, g_ReflexLimiter.GetTargetIntervalUs());
    }
}

slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* options) {
    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return kSlResultErrorInvalidState;
    }

    // Newer integrations can configure DLSS-G by passing options directly to GetState, after
    // slSetTagForFrame has already made the activation input volatile. Keep the latest inactive
    // DX12 UI tag covered before entering GetState so a late OFF->ON observation can adopt it.
    if (!ShouldKeepPureObserverOnlyStreamlineBehavior() && g_StreamlineUsesD3D12.load(std::memory_order_acquire) &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        const uint32_t requestedOutputs = options ? std::clamp(options->numFramesToGenerate + 1u, 1u, 6u) : 2u;
        ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(requestedOutputs);
    }

    const slResult result = originalGetState(viewport, state, options);
    RetryResolveReflexFeatureHooksForRuntimeActivity("slDLSSGGetState");
    const uint32_t viewportKey = GetViewportKey(viewport);
    const bool viewportWasActive = WasViewportRuntimeStateActive(viewportKey);
    const bool hasRuntimeFenceEvidence = HasDLSSGRuntimeFenceEvidence(state);
    const bool suppressNewActivation = ShouldSuppressNewGetStateActivation();
    if (result == kSlResultOk && state.numFramesToGenerateMax > 0) {
        CacheCapabilityMax(viewportKey, state.numFramesToGenerateMax);
    }

    const uint32_t capabilityMax =
        state.numFramesToGenerateMax > 0 ? state.numFramesToGenerateMax : GetCachedCapabilityMax(viewportKey);
    const auto runtimeEvaluation = ce::streamline_runtime_policy::EvaluateViewportRuntimeUpdateFromGetState(
        result == kSlResultOk, options != nullptr, viewportWasActive, hasRuntimeFenceEvidence, suppressNewActivation,
        options ? options->mode : 0, options ? options->numFramesToGenerate : 0u, capabilityMax);
    const bool clearAllViewportStatesForDisable =
        runtimeEvaluation.update.shouldUpdate &&
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForGetStateDisable(
            result == kSlResultOk, options != nullptr, hasRuntimeFenceEvidence, options ? options->mode : 0u,
            capabilityMax);
    if (result == kSlResultOk && options != nullptr) {
        static std::atomic<int> s_getStateTraceLogCount{0};
        const int logCount = s_getStateTraceLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 8 || (logCount % 512) == 0) {
            char statusText[160];
            FormatDLSSGStatusFlags(state.status, statusText, sizeof(statusText));
            HookLogImportant(
                "Streamline Hook: slDLSSGGetState observed viewport=%u optionsMode=%s(%u) generated=%u "
                "capabilityMax=%u presented=%u status=0x%X(%s) minWH=%u vsyncOk=%d dynMFG=%d vramMB=%llu "
                "fence=%p fenceValue=%llu viewportWasActive=%d update=%d "
                "updateActive=%d clearAll=%d suppressNew=%d fenceEvidence=%d setOptionsHooked=%d "
                "setOptionsOriginal=%p",
                viewportKey, GetDLSSGModeName(options->mode), options->mode, options->numFramesToGenerate,
                capabilityMax, state.numFramesActuallyPresented, state.status, statusText, state.minWidthOrHeight,
                static_cast<int>(state.bIsVsyncSupportAvailable), static_cast<int>(state.bIsDynamicMFGSupported),
                (unsigned long long)(state.estimatedVRAMUsageInBytes / (1024ull * 1024ull)),
                state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue, viewportWasActive ? 1 : 0,
                runtimeEvaluation.update.shouldUpdate ? 1 : 0, runtimeEvaluation.update.active ? 1 : 0,
                clearAllViewportStatesForDisable ? 1 : 0, suppressNewActivation ? 1 : 0,
                hasRuntimeFenceEvidence ? 1 : 0, g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                reinterpret_cast<void*>(g_Original_slDLSSGSetOptions));
        }
    }

    // [DLSSG HEALTH] — session 20260702_094955: GTA reported DLSSG ON (optionsMode=on, updateActive=1) but
    // presents stayed at base rate all session (numFramesActuallyPresented==1, no fps gain). sl.dlss_g
    // publishes WHY it declines to interpolate in DLSSGState.status; log every status transition, and while
    // the game requests ON without interpolation evidence, emit a deterministic streak warning that pairs
    // NVIDIA's status decode with Reflex call-activity evidence (DLSSG hard-requires Reflex, and GTA's
    // Reflex is historically flaky even without CE).
    if (result == kSlResultOk) {
        const uint32_t previousStatus = g_DLSSGLastObservedStatus.exchange(state.status, std::memory_order_relaxed);
        if (previousStatus != state.status) {
            char prevText[160];
            char nowText[160];
            FormatDLSSGStatusFlags(previousStatus, prevText, sizeof(prevText));
            FormatDLSSGStatusFlags(state.status, nowText, sizeof(nowText));
            HookLogImportant(
                "Streamline Hook: [DLSSG HEALTH] status TRANSITION 0x%X(%s) -> 0x%X(%s) (viewport=%u "
                "optionsMode=%s presented=%u minWH=%u vsyncOk=%d dynMFG=%d)",
                previousStatus, prevText, state.status, nowText, viewportKey,
                options ? GetDLSSGModeName(options->mode) : "n/a", state.numFramesActuallyPresented,
                state.minWidthOrHeight, static_cast<int>(state.bIsVsyncSupportAvailable),
                static_cast<int>(state.bIsDynamicMFGSupported));
        }
    }
    const bool optionsRequestOn = options != nullptr && options->mode != 0;
    if (ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(result == kSlResultOk,
                                                                              optionsRequestOn)) {
        const bool interpolationEvidence =
            ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(state.numFramesActuallyPresented);
        uint64_t streak = 0;
        if (interpolationEvidence && state.status == 0) {
            g_DLSSGNotInterpolatingStreak.store(0, std::memory_order_relaxed);
        } else {
            streak = g_DLSSGNotInterpolatingStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        if (ce::streamline_runtime_policy::ShouldWarnDLSSGActiveButNotInterpolating(streak, kDLSSGHealthWarnStreak,
                                                                                    kDLSSGHealthWarnRepeat)) {
            const uint64_t nowMs = GetTickCount64();
            const uint64_t sleepCount = g_ReflexSleepObservedCount.load(std::memory_order_relaxed);
            const uint64_t sleepCountAtLastLog =
                g_ReflexSleepCountAtLastHealthLog.exchange(sleepCount, std::memory_order_relaxed);
            const uint64_t sleepLastMs = g_ReflexSleepLastTickMs.load(std::memory_order_relaxed);
            const uint64_t reflexOptCount = g_ReflexSetOptionsObservedCount.load(std::memory_order_relaxed);
            const uint64_t reflexOptLastMs = g_ReflexSetOptionsLastTickMs.load(std::memory_order_relaxed);
            char statusText[160];
            FormatDLSSGStatusFlags(state.status, statusText, sizeof(statusText));
            HookLogImportant(
                "Streamline Hook: [DLSSG HEALTH] ON but NOT interpolating for %llu consecutive GetState samples — "
                "status=0x%X(%s) presented=%u generatedReq=%u capabilityMax=%u minWH=%u vsyncOk=%d dynMFG=%d "
                "vramMB=%llu fence=%p fenceValue=%llu | Reflex evidence: sleepCalls=%llu (+%llu since last warn) "
                "sleepAge=%llums setOptionsCalls=%llu setOptionsAge=%llums lastMode=%d sleepHooked=%d | "
                "REFLEX-NOT-DETECTED in status = the game's Reflex pipeline is not running (DLSSG requires it); "
                "status=ok with presented==1 and dynMFG=1 can be hardware flip metering — correlate with the "
                "displayed fps",
                static_cast<unsigned long long>(streak), state.status, statusText, state.numFramesActuallyPresented,
                options->numFramesToGenerate, capabilityMax, state.minWidthOrHeight,
                static_cast<int>(state.bIsVsyncSupportAvailable), static_cast<int>(state.bIsDynamicMFGSupported),
                (unsigned long long)(state.estimatedVRAMUsageInBytes / (1024ull * 1024ull)),
