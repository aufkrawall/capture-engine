    }

    if (!previousExplicitSetOptionsActivation && updatedExplicitSetOptionsActivation && signalUpdate.effectiveActive &&
        explicitSetOptionsActivation && !signalUpdate.freshActivationEdge) {
        HookLogImportant(
            "Streamline Hook: Upgraded already-live DLSS comeback provenance to explicit SetOptions enable "
            "(source=%s startupWindow=%d hadFSR=%d safeBootstrap=%d pending=%d unconfirmed=%d settling=%d "
            "stabilizing=%d)",
            source ? source : "runtime-state", startupWindowActive ? 1 : 0, HookHasFSRFGHistory() ? 1 : 0,
            safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
    }

    if (previousSignalObserved != signalUpdate.effectiveActive) {
        if (signalUpdate.effectiveActive) {
            ce::dx12_streamline_ui_overlay::BeginActivation(
                static_cast<uint32_t>(std::clamp(signalUpdate.effectiveMultiplier, 1, 6)));
        } else {
            ce::dx12_streamline_ui_overlay::EndActivation("accepted Streamline FG OFF transition");
        }
        DX12_OnStreamlineFGStateChanged(signalUpdate.effectiveActive);
        HookLogImportant("Streamline Hook: FG state transition %s->%s via %s", previousSignalObserved ? "ON" : "OFF",
                         signalUpdate.effectiveActive ? "ON" : "OFF", source ? source : "runtime-state");
    }
    ce::fg_session::EmitFGEvent(sourceWasSetOptions ? ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate
                                                    : ce::fg_session::FGEventKind::kStreamlineGetStateRuntimeUpdate,
                                source ? source : "StreamlineRuntimeState", nullptr, nullptr,
                                signalUpdate.effectiveActive ? ce::fg_runtime::RuntimeMode::kDLSSFG
                                                             : ce::fg_runtime::RuntimeMode::kStreamlineNoFG,
                                signalUpdate.effectiveActive, updatedExplicitSetOptionsActivation);
    if (signalUpdate.deferredOffDuringStartupWindow && !startupWindowActive) {
        if (!active && !postSLConfirmedButStartupSettling && sourceWasGetState &&
            postSLConfirmedButStaleOffWarmupProtected && !postSLConfirmedButRuntimeStateStabilizingBase) {
            static std::atomic<bool> s_loggedGetStateWarmupProofSuppression{false};
            if (!s_loggedGetStateWarmupProofSuppression.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "Streamline Hook: Suppressing post-stabilization GetState OFF during PostSL warmup proof "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d stableProtectionWindow=%d-%d)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
                    HookGetPostSLStaleOffWarmupProtectionLastFrame());
            }
        } else if (!active && !postSLConfirmedButStartupSettling && sourceWasGetState &&
                   postSLConfirmedButOffChurnAwaitingActiveProof) {
            static std::atomic<bool> s_loggedGetStateActiveProofSuppression{false};
            if (!s_loggedGetStateActiveProofSuppression.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "Streamline Hook: Suppressing GetState OFF until startup OFF churn receives active proof "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d activeProof=%u/%u)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire),
                    ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold());
            }
        } else if (!active && !postSLConfirmedButStartupSettling && postSLConfirmedButRuntimeStateStabilizingBase &&
                   sourceWasGetState) {
            static std::atomic<bool> s_loggedPostSettlingGetStateSuppression{false};
            if (!s_loggedPostSettlingGetStateSuppression.exchange(true, std::memory_order_relaxed)) {
                const int runtimeStateStabilizationLastFrame = HookGetPostSLRuntimeStateStabilizationLastFrame();
                HookLogImportant(
                    "Streamline Hook: Suppressing first post-settling GetState OFF during runtime-state stabilization "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d stableProtectionWindow=%d-%d)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
                    runtimeStateStabilizationLastFrame);
            }
        }
        static std::atomic<int> s_halfArmedDeferredOffLogCount{0};
        const int logCount = s_halfArmedDeferredOffLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Keeping OFF churn deferred after startup window expiry because Streamline DLSS "
                "startup is still startup-protected (hadFSR=%d explicit=%d safeBootstrap=%d pending=%d "
                "unconfirmed=%d startupActivationEntered=%d confirmed=%d settling=%d stabilizing=%d "
                "activeProofPending=%d source=%s)",
                hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                postSLStartupActivationEntered ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0,
                (postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof) ? 1 : 0,
                postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0, source ? source : "runtime-state");
        }
    }
    if (signalUpdate.deferredOffDuringStartupWindow && signalUpdate.shouldExtendStartupTransitionWindow) {
        const bool shouldExtend = g_StartupWindowOffExtensionPending.exchange(false, std::memory_order_acq_rel);
        if (!shouldExtend) {
            static std::atomic<int> s_startupWindowOffExtensionAlreadyConsumedLogCount{0};
            const int logCount =
                s_startupWindowOffExtensionAlreadyConsumedLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "Streamline Hook: Deferring OFF signal during startup transition window "
                    "(g_StreamlineFGRunning stays ON, multiplier=%d source=%s) — extension already consumed for "
                    "current churn burst",
                    signalUpdate.effectiveMultiplier, source ? source : "unknown");
            }
            return;
        }
        DXGIShared::ExtendStreamlineStartupTransitionWindow();
        HookLogImportant(
            "Streamline Hook: Deferring OFF signal during startup transition window "
            "(g_StreamlineFGRunning stays ON, multiplier=%d source=%s) — extended startup window",
            signalUpdate.effectiveMultiplier, source ? source : "unknown");
    } else if (ce::streamline_runtime_policy::ShouldPrimeStartupWindowOffExtensionLatch(
                   signalUpdate.effectiveActive, signalUpdate.freshActivationEdge)) {
        g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    }
}

bool WasViewportRuntimeStateActive(uint32_t viewportKey) {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    const auto it = g_ViewportStates.find(viewportKey);
    return it != g_ViewportStates.end() && it->second.active;
}

bool ShouldSuppressNewGetStateActivation() {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        return false;
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    if (ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(
            g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.load(std::memory_order_acquire),
            safePostFSRBootstrapPath, runtimeMode)) {
        return true;
    }

    if (ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(
            g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.load(std::memory_order_acquire),
            DXGIShared::IsStreamlineStartupTransitionWindowActive(), runtimeMode)) {
        return true;
    }

    const ULONGLONG suppressUntilMs = g_SuppressNewGetStateActivationUntilMs.load(std::memory_order_acquire);
    return suppressUntilMs != 0 && GetTickCount64() < suppressUntilMs;
}

bool HasDLSSGRuntimeFenceEvidence(const slDLSSGState& state) {
    return state.inputsProcessingCompletionFence != nullptr ||
           state.lastPresentInputsProcessingCompletionFenceValue != 0;
}

void UpdateViewportRuntimeState(uint32_t viewportKey, bool active, int multiplier, uint32_t generatedFrames,
                                uint32_t capabilityMax, const char* source,
                                bool clearAllViewportStatesForDisable = false) {
    ViewportFGState previousState{};
    bool hadPreviousState = false;
    bool stateChanged = false;
    bool anyActive = false;
    int combinedMultiplier = 0;
    size_t clearedActiveViewportCount = 0;

    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        const auto existing = g_ViewportStates.find(viewportKey);
        if (existing != g_ViewportStates.end()) {
            previousState = existing->second;
            hadPreviousState = true;
        }

        if (active) {
            g_ViewportStates[viewportKey] = {true, multiplier, generatedFrames, capabilityMax};
        } else if (clearAllViewportStatesForDisable) {
            clearedActiveViewportCount = g_ViewportStates.size();
            g_ViewportStates.clear();
        } else {
            g_ViewportStates.erase(viewportKey);
        }

        const auto current = g_ViewportStates.find(viewportKey);
        const ViewportFGState currentState =
            current != g_ViewportStates.end() ? current->second : ViewportFGState{false, 0, 0, capabilityMax};

        stateChanged = !hadPreviousState || previousState.active != currentState.active ||
                       previousState.multiplier != currentState.multiplier ||
                       previousState.generatedFrames != currentState.generatedFrames ||
                       previousState.capabilityMax != currentState.capabilityMax;

        for (const auto& [_, state] : g_ViewportStates) {
            if (!state.active) {
                continue;
            }
            anyActive = true;
            combinedMultiplier = std::max(combinedMultiplier, state.multiplier);
        }
    }

    const bool explicitSetOptionsEnableSignal = source && strcmp(source, "SetOptions") == 0 && active;
    ApplyCombinedStreamlineRuntimeState(anyActive, combinedMultiplier, explicitSetOptionsEnableSignal, source);

    if (clearedActiveViewportCount > 0) {
        HookLogImportant(
            "Streamline Hook: Cleared %zu cached DLSSG viewport runtime state(s) after %s disable "
            "(triggerViewport=%u generatedFrames=%u capabilityMax=%u)",
            clearedActiveViewportCount, source ? source : "runtime", viewportKey, generatedFrames, capabilityMax);
    }

    if (stateChanged) {
        HookLog(
            "Streamline Hook: Viewport %u state active=%d multiplier=%dx generatedFrames=%u capabilityMax=%u "
            "source=%s clearAll=%d",
            viewportKey, active ? 1 : 0, active ? multiplier : 0, generatedFrames, capabilityMax,
            source ? source : "runtime", clearAllViewportStatesForDisable ? 1 : 0);
    }
}

template <typename T>
bool InstallInlineHookOnce(void* target, void* detour, T& original, std::atomic<bool>& installedFlag,
                           std::atomic<void*>& targetSlot, const char* hookName) {
    if (!target) {
        return false;
    }

    if (target == detour) {
        original = nullptr;
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        return true;
    }

    const void* installedTarget = targetSlot.load(std::memory_order_acquire);
    if (installedFlag.load(std::memory_order_acquire) && installedTarget == target) {
        return false;
    }

    void* trampoline = nullptr;
    if (!InlineHook::Install(target, detour, &trampoline)) {
        HookLogImportant("Streamline Hook: Failed to inline hook %s at %p", hookName, target);
        return false;
    }

    original = reinterpret_cast<T>(trampoline);
    targetSlot.store(target, std::memory_order_release);
    installedFlag.store(true, std::memory_order_release);
    HookLogImportant("Streamline Hook: Inline hook installed for %s at %p (trampoline=%p)", hookName, target,
                     trampoline);
    return true;
}

void LogFeatureImportFallbackUnavailableOnce(const char* moduleBaseName, const char* functionName, void* exportedProc,
                                             const char* hookName, const char* reason) {
    const char* effectiveHookName = hookName ? hookName : functionName;
    if (!moduleBaseName || !effectiveHookName) {
        return;
    }

    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedUnavailable;

    std::string key = effectiveHookName;
    key += '|';
    key += moduleBaseName;
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(exportedProc));

    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_loggedUnavailable.find(key) != s_loggedUnavailable.end()) {
            return;
        }
        s_loggedUnavailable.emplace(key, true);
    }

    HookLogImportant("Streamline Hook: Direct import fallback unavailable for %s via %s (export=%p): %s",
                     effectiveHookName, moduleBaseName, exportedProc, reason ? reason : "no matching loaded import");
}

bool InstallFeatureImportFallbackIfPresent(const char* moduleBaseName, const char* functionName, void* detour,
                                           void* exportedProc, void** originalSlot, const char* hookName) {
    if (!moduleBaseName || !functionName || !detour || !exportedProc) {
        return false;
    }

    if (originalSlot && *originalSlot == nullptr) {
        *originalSlot = exportedProc;
    }

    void* patchedOriginal = nullptr;
    if (!IATHook::PatchIATAllModules(moduleBaseName, functionName, detour, &patchedOriginal)) {
        LogFeatureImportFallbackUnavailableOnce(
            moduleBaseName, functionName, exportedProc, hookName,
            "no loaded module currently imports this feature directly; retrying on later Streamline module scans");
        return false;
    }

    if (originalSlot && *originalSlot == nullptr) {
        *originalSlot = patchedOriginal ? patchedOriginal : exportedProc;
    }

    HookLogImportant("Streamline Hook: Installed direct import fallback for %s via %s (export=%p original=%p)",
                     hookName ? hookName : functionName, moduleBaseName, exportedProc,
                     originalSlot ? *originalSlot : patchedOriginal);
    return true;
}

bool TryGetOwningModulePath(void* address, char* modulePath, DWORD modulePathCapacity, DWORD* outError) {
    if (outError) {
        *outError = ERROR_SUCCESS;
    }
    if (!address || !modulePath || modulePathCapacity == 0) {
        if (outError) {
            *outError = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    HMODULE ownerModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &ownerModule) ||
        !ownerModule) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }

    const DWORD pathLen = GetModuleFileNameA(ownerModule, modulePath, modulePathCapacity);
    if (pathLen == 0 || pathLen >= modulePathCapacity) {
        if (outError) {
            *outError = pathLen >= modulePathCapacity ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        }
        return false;
    }
    return true;
}

bool TryInstallFeatureImportFallbackForOwningModule(void* function, const char* functionName, void* detour,
                                                    void** originalSlot, std::atomic<void*>& attemptedTarget,
                                                    const char* hookName) {
    if (!function || !functionName || !detour) {
        return false;
    }

    if (attemptedTarget.load(std::memory_order_acquire) == function) {
        return false;
    }

    char ownerPath[MAX_PATH] = {};
    DWORD ownerError = ERROR_SUCCESS;
    if (!TryGetOwningModulePath(function, ownerPath, MAX_PATH, &ownerError)) {
        attemptedTarget.store(function, std::memory_order_release);
        HookLogImportant("Streamline Hook: Direct import fallback owner resolution failed for %s target=%p error=%lu",
                         hookName ? hookName : functionName, function, static_cast<unsigned long>(ownerError));
        return false;
    }

    attemptedTarget.store(function, std::memory_order_release);

    const char* ownerBaseName = GetModuleBaseName(ownerPath);
    if (!ownerBaseName || !ownerBaseName[0]) {
        return false;
    }

    return InstallFeatureImportFallbackIfPresent(ownerBaseName, functionName, detour, function, originalSlot, hookName);
}

void LogReturnedWrapperFallbackOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target, void* wrapper,
                                    bool hookReady) {
    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: Using returned-pointer wrapper fallback for %s (target=%p wrapper=%p hookReady=%d). "
        "Callers that cache slGetFeatureFunction results remain intercepted even if Streamline later reloads, "
        "repairs, or bypasses the feature export patch.",
        hookName ? hookName : "<unknown>", target, wrapper, hookReady ? 1 : 0);
}

void LogProactiveFeatureHookGapOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target) {
    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: Proactively resolved %s at %p but could not patch the export/import path yet; "
        "waiting for an intercepted slGetFeatureFunction lookup to return the wrapper fallback or for a later module "
        "scan to find a direct import.",
        hookName ? hookName : "<unknown>", target);
}

void LogFeatureLookupOutcomeOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* originalTarget,
                                 void* returnedTarget, bool hookReady) {
    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: slGetFeatureFunction returned %s target=%p delivered=%p hookReady=%d "
        "wrapperSubstituted=%d",
        hookName ? hookName : "<unknown>", originalTarget, returnedTarget, hookReady ? 1 : 0,
        originalTarget != returnedTarget ? 1 : 0);
}

bool MaybeHookDLSSGSetOptions(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slDLSSGSetOptions)) {
        g_DLSSGSetOptionsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ||
            g_DLSSGSetOptionsTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function), reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                                  g_Original_slDLSSGSetOptions, g_DLSSGSetOptionsHooked, g_DLSSGSetOptionsTarget,
                                  "slDLSSGSetOptions");
            if (!g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                    reinterpret_cast<void**>(&g_Original_slDLSSGSetOptions),
                    g_DLSSGSetOptionsImportFallbackAttemptedTarget, "slDLSSGSetOptions");
            }
        }
    }

    const bool hookReady = g_DLSSGSetOptionsHooked.load(std::memory_order_acquire);
    if (fallbackToReturnedWrapper && function != reinterpret_cast<void*>(Hooked_slDLSSGSetOptions)) {
        if (!GetCallableOriginalDLSSGSetOptions() && !hookReady && !g_Original_slDLSSGSetOptions) {
            g_Original_slDLSSGSetOptions = reinterpret_cast<PFN_slDLSSGSetOptions>(function);
        }
        if (ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(
                fallbackToReturnedWrapper, false, GetCallableOriginalDLSSGSetOptions() != nullptr)) {
            LogReturnedWrapperFallbackOnce(g_DLSSGSetOptionsReturnedWrapperFallbackLogged, "slDLSSGSetOptions",
                                           function, reinterpret_cast<void*>(Hooked_slDLSSGSetOptions), hookReady);
            function = reinterpret_cast<void*>(Hooked_slDLSSGSetOptions);
            return true;
        }
    }

    return hookReady;
}

bool MaybeHookDLSSGGetState(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slDLSSGGetState)) {
        g_DLSSGGetStateHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_DLSSGGetStateHooked.load(std::memory_order_acquire) ||
            g_DLSSGGetStateTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function), reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                                  g_Original_slDLSSGGetState, g_DLSSGGetStateHooked, g_DLSSGGetStateTarget,
                                  "slDLSSGGetState");
            if (!g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                    reinterpret_cast<void**>(&g_Original_slDLSSGGetState), g_DLSSGGetStateImportFallbackAttemptedTarget,
                    "slDLSSGGetState");
            }
        }
    }

    const bool hookReady = g_DLSSGGetStateHooked.load(std::memory_order_acquire);
    if (fallbackToReturnedWrapper && function != reinterpret_cast<void*>(Hooked_slDLSSGGetState)) {
        if (!GetCallableOriginalDLSSGGetState() && !hookReady && !g_Original_slDLSSGGetState) {
            g_Original_slDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(function);
        }
        if (ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(
                fallbackToReturnedWrapper, false, GetCallableOriginalDLSSGGetState() != nullptr)) {
            LogReturnedWrapperFallbackOnce(g_DLSSGGetStateReturnedWrapperFallbackLogged, "slDLSSGGetState", function,
                                           reinterpret_cast<void*>(Hooked_slDLSSGGetState), hookReady);
            function = reinterpret_cast<void*>(Hooked_slDLSSGGetState);
            return true;
        }
    }

    return hookReady;
}

bool MaybeHookReflexSleep(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slReflexSleep)) {
        g_ReflexSleepHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_ReflexSleepHooked.load(std::memory_order_acquire) ||
            g_ReflexSleepTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function), reinterpret_cast<void*>(Hooked_slReflexSleep),
                                  g_Original_slReflexSleep, g_ReflexSleepHooked, g_ReflexSleepTarget, "slReflexSleep");
            if (!g_ReflexSleepHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                    reinterpret_cast<void**>(&g_Original_slReflexSleep), g_ReflexSleepImportFallbackAttemptedTarget,
                    "slReflexSleep");
            }
        }
    }

    if (fallbackToReturnedWrapper && !g_ReflexSleepHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slReflexSleep) {
            g_Original_slReflexSleep = reinterpret_cast<PFN_slReflexSleep>(function);
        }
        LogReturnedWrapperFallbackOnce(g_ReflexSleepReturnedWrapperFallbackLogged, "slReflexSleep", function,
                                       reinterpret_cast<void*>(Hooked_slReflexSleep),
                                       g_ReflexSleepHooked.load(std::memory_order_acquire));
        function = reinterpret_cast<void*>(Hooked_slReflexSleep);
        return true;
    }

    return g_ReflexSleepHooked.load(std::memory_order_acquire);
}

bool MaybeHookReflexSetOptions(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slReflexSetOptions)) {
        g_ReflexSetOptionsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ||
            g_ReflexSetOptionsTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function), reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                  g_Original_slReflexSetOptions, g_ReflexSetOptionsHooked, g_ReflexSetOptionsTarget,
                                  "slReflexSetOptions");
            if (!g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                    reinterpret_cast<void**>(&g_Original_slReflexSetOptions),
                    g_ReflexSetOptionsImportFallbackAttemptedTarget, "slReflexSetOptions");
            }
        }
    }

    if (fallbackToReturnedWrapper && !g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slReflexSetOptions) {
            g_Original_slReflexSetOptions = reinterpret_cast<PFN_slReflexSetOptions>(function);
        }
        LogReturnedWrapperFallbackOnce(g_ReflexSetOptionsReturnedWrapperFallbackLogged, "slReflexSetOptions", function,
                                       reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                       g_ReflexSetOptionsHooked.load(std::memory_order_acquire));
        function = reinterpret_cast<void*>(Hooked_slReflexSetOptions);
        return true;
    }

    return g_ReflexSetOptionsHooked.load(std::memory_order_acquire);
}

bool MaybeHookReflexSetConstants(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slReflexSetConstants)) {
        g_ReflexSetConstantsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ||
            g_ReflexSetConstantsTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function),
                                  reinterpret_cast<void*>(Hooked_slReflexSetConstants), g_Original_slReflexSetConstants,
                                  g_ReflexSetConstantsHooked, g_ReflexSetConstantsTarget, "slReflexSetConstants");
            if (!g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                    reinterpret_cast<void**>(&g_Original_slReflexSetConstants),
                    g_ReflexSetConstantsImportFallbackAttemptedTarget, "slReflexSetConstants");
            }
        }
    }

    if (fallbackToReturnedWrapper && !g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slReflexSetConstants) {
            g_Original_slReflexSetConstants = reinterpret_cast<PFN_slReflexSetConstants>(function);
        }
        LogReturnedWrapperFallbackOnce(g_ReflexSetConstantsReturnedWrapperFallbackLogged, "slReflexSetConstants",
                                       function, reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                                       g_ReflexSetConstantsHooked.load(std::memory_order_acquire));
        function = reinterpret_cast<void*>(Hooked_slReflexSetConstants);
        return true;
    }

    return g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
}

bool TryResolveDLSSGFeatureHooks() {
    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        return false;
    }

    bool hookedAnything = false;

    if (!g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
        void* function = nullptr;
        const slResult result = originalGetFeatureFunction(kSLFeatureDLSSG, "slDLSSGSetOptions", function);
        if (result == kSlResultOk && function) {
            const bool hooked = MaybeHookDLSSGSetOptions(function, false);
            hookedAnything |= hooked;
            if (!hooked && !g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_DLSSGSetOptionsProactiveFallbackLogged, "slDLSSGSetOptions", function);
            }
        }
    }

    if (!g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
        void* function = nullptr;
        const slResult result = originalGetFeatureFunction(kSLFeatureDLSSG, "slDLSSGGetState", function);
        if (result == kSlResultOk && function) {
            const bool hooked = MaybeHookDLSSGGetState(function, false);
            hookedAnything |= hooked;
            if (!hooked && !g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_DLSSGGetStateProactiveFallbackLogged, "slDLSSGGetState", function);
            }
        }
    }

    return hookedAnything || g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ||
           g_DLSSGGetStateHooked.load(std::memory_order_acquire);
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
    slResult sleepResult = kSlResultErrorInvalidState;
    slResult setOptionsResult = kSlResultErrorInvalidState;
