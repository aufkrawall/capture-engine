        return false;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        !module) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }

    if (ownerModule) {
        *ownerModule = module;
    }
    if (ownerPath && ownerPathCapacity > 0) {
        const DWORD pathLen = GetModuleFileNameA(module, ownerPath, ownerPathCapacity);
        if (pathLen == 0 || pathLen >= ownerPathCapacity) {
            if (outError) {
                *outError = pathLen >= ownerPathCapacity ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
            }
            ownerPath[0] = '\0';
        }
    }
    return true;
}

void LogStaleStreamlineOriginalBlockedOnce(const char* functionName, void* original, void* validationAddress,
                                           const char* expectedModuleRole, DWORD error) {
    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedOriginals;

    std::string key = functionName ? functionName : "<unknown>";
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(original));
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(validationAddress));

    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_loggedOriginals.find(key) != s_loggedOriginals.end()) {
            return;
        }
        s_loggedOriginals.emplace(key, true);
    }

    HookLogImportant(
        "Streamline Hook: Blocking stale original forward for %s original=%p validation=%p expected=%s "
        "ownerLoaded=0 error=%lu",
        functionName ? functionName : "<unknown>", original, validationAddress,
        expectedModuleRole ? expectedModuleRole : "loaded Streamline module", static_cast<unsigned long>(error));
}

bool IsSavedStreamlineOriginalCallable(const char* functionName, void* original, void* validationAddress,
                                       const char* expectedModuleRole) {
    const void* addressToValidate = validationAddress ? validationAddress : original;
    DWORD ownerError = ERROR_SUCCESS;
    const bool ownerLoaded =
        DoesAddressBelongToLoadedModule(const_cast<void*>(addressToValidate), nullptr, nullptr, 0, &ownerError);
    if (ce::streamline_runtime_policy::ShouldForwardSavedStreamlineOriginal(original != nullptr, ownerLoaded)) {
        return true;
    }

    if (original) {
        LogStaleStreamlineOriginalBlockedOnce(functionName, original, const_cast<void*>(addressToValidate),
                                              expectedModuleRole, ownerError);
    }
    return false;
}

PFN_slGetFeatureFunction GetCallableOriginalGetFeatureFunction() {
    auto original = g_Original_slGetFeatureFunction;
    return IsSavedStreamlineOriginalCallable("slGetFeatureFunction", reinterpret_cast<void*>(original),
                                             g_SLGetFeatureFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slGetPluginFunction GetCallableOriginalGetPluginFunction() {
    auto original = g_Original_slGetPluginFunction;
    return IsSavedStreamlineOriginalCallable("slGetPluginFunction", reinterpret_cast<void*>(original),
                                             g_SLGetPluginFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slSetD3DDevice GetCallableOriginalSetD3DDevice() {
    auto original = g_Original_slSetD3DDevice;
    return IsSavedStreamlineOriginalCallable("slSetD3DDevice", reinterpret_cast<void*>(original),
                                             g_SLSetD3DDeviceTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slSetTag GetCallableOriginalSetTag() {
    auto original = g_Original_slSetTag;
    return IsSavedStreamlineOriginalCallable("slSetTag", reinterpret_cast<void*>(original),
                                             g_SLSetTagTarget.load(std::memory_order_acquire), "core Streamline module")
               ? original
               : nullptr;
}

PFN_slSetTagForFrame GetCallableOriginalSetTagForFrame() {
    auto original = g_Original_slSetTagForFrame;
    return IsSavedStreamlineOriginalCallable("slSetTagForFrame", reinterpret_cast<void*>(original),
                                             g_SLSetTagForFrameTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slEvaluateFeature GetCallableOriginalEvaluateFeature() {
    auto original = g_Original_slEvaluateFeature;
    return IsSavedStreamlineOriginalCallable("slEvaluateFeature", reinterpret_cast<void*>(original),
                                             g_SLEvaluateFeatureTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slDLSSGSetOptions GetCallableOriginalDLSSGSetOptions() {
    auto original = g_Original_slDLSSGSetOptions;
    return IsSavedStreamlineOriginalCallable("slDLSSGSetOptions", reinterpret_cast<void*>(original),
                                             g_DLSSGSetOptionsTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;
}

PFN_slDLSSGGetState GetCallableOriginalDLSSGGetState() {
    auto original = g_Original_slDLSSGGetState;
    return IsSavedStreamlineOriginalCallable("slDLSSGGetState", reinterpret_cast<void*>(original),
                                             g_DLSSGGetStateTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;
}

PFN_slReflexSleep GetCallableOriginalReflexSleep() {
    auto original = g_Original_slReflexSleep;
    return IsSavedStreamlineOriginalCallable("slReflexSleep", reinterpret_cast<void*>(original),
                                             g_ReflexSleepTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;
}

PFN_slReflexSetOptions GetCallableOriginalReflexSetOptions() {
    auto original = g_Original_slReflexSetOptions;
    return IsSavedStreamlineOriginalCallable("slReflexSetOptions", reinterpret_cast<void*>(original),
                                             g_ReflexSetOptionsTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;
}

PFN_slReflexSetConstants GetCallableOriginalReflexSetConstants() {
    auto original = g_Original_slReflexSetConstants;
    return IsSavedStreamlineOriginalCallable("slReflexSetConstants", reinterpret_cast<void*>(original),
                                             g_ReflexSetConstantsTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;
}

uint32_t GetViewportKey(const slViewportHandle& viewport) {
    return viewport.value;
}

slDLSSGOptions CloneDLSSGOptions(const slDLSSGOptions& source) {
    slDLSSGOptions copy;
    copy.next = source.next;
    copy.structType = source.structType;
    copy.structVersion = source.structVersion;
    copy.mode = source.mode;
    copy.numFramesToGenerate = source.numFramesToGenerate;
    copy.flags = source.flags;
    copy.dynamicResWidth = source.dynamicResWidth;
    copy.dynamicResHeight = source.dynamicResHeight;
    copy.numBackBuffers = source.numBackBuffers;
    copy.mvecDepthWidth = source.mvecDepthWidth;
    copy.mvecDepthHeight = source.mvecDepthHeight;
    copy.colorWidth = source.colorWidth;
    copy.colorHeight = source.colorHeight;
    copy.colorBufferFormat = source.colorBufferFormat;
    copy.mvecBufferFormat = source.mvecBufferFormat;
    copy.depthBufferFormat = source.depthBufferFormat;
    copy.hudLessBufferFormat = source.hudLessBufferFormat;
    copy.uiBufferFormat = source.uiBufferFormat;
    copy.onErrorCallback = source.onErrorCallback;
    if (source.structVersion >= kSLStructVersion2) {
        copy.bReserved15 = source.bReserved15;
    }
    if (source.structVersion >= kSLStructVersion3) {
        copy.queueParallelismMode = source.queueParallelismMode;
    }
    if (source.structVersion >= kSLStructVersion4) {
        copy.enableUserInterfaceRecomposition = source.enableUserInterfaceRecomposition;
    }
    if (source.structVersion >= kSLStructVersion5) {
        copy.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
    }
    if (source.structVersion > kSLStructVersion5) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true)) {
            HookLogImportant("SL: DLSSG options structVersion=%zu exceeds CE's max (5); forwarding v5 prefix only",
                             source.structVersion);
        }
        copy.structVersion = kSLStructVersion5;
    }
    return copy;
}

int GetEffectiveMultiplier(const slDLSSGOptions& options) {
    return ce::streamline_runtime_policy::ResolveDLSSFGMultiplier(
        ce::streamline_runtime_policy::IsDLSSGModeEnabled(options.mode), options.numFramesToGenerate);
}

struct DLSSGSetOptionsLogState {
    bool valid = false;
    bool requestedEnabled = false;
    bool forwarded = false;
    uint32_t requestMode = 0;
    uint32_t forwardedMode = 0;
    uint32_t requestedGeneratedFrames = 0;
    uint32_t forwardedGeneratedFrames = 0;
    uint32_t capabilityMax = 0;
    slResult result = kSlResultOk;
    bool overrideApplied = false;
    bool overrideClamped = false;
    bool startupWindowActive = false;
    bool hadFSRFGPhase = false;
    bool explicitSetOptionsActivationForCurrentComeback = false;
    bool safePostFSRBootstrapPath = false;
    bool startupActivationPending = false;
    bool postSLActiveButUnconfirmed = false;
    bool postSLConfirmedRendering = false;
    bool postSLConfirmedButStartupSettling = false;
    bool postSLConfirmedButRuntimeStateStabilizing = false;
    bool streamlineFGSignalActive = false;
    bool pureObserverOnly = false;
    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kUnknown;
};

void LogDLSSGSetOptionsTransition(uint32_t viewportKey, const slDLSSGOptions& requestedOptions,
                                  const slDLSSGOptions& forwardedOptions, uint32_t requestedGeneratedFrames,
                                  uint32_t capabilityMax, bool requestedEnabled, bool setOptionsCallSuppressed,
                                  bool overrideApplied, bool overrideClamped, slResult result, bool pureObserverOnly,
                                  bool startupWindowActive, bool hadFSRFGPhase,
                                  bool explicitSetOptionsActivationForCurrentComeback, bool safePostFSRBootstrapPath,
                                  bool startupActivationPending, bool postSLActiveButUnconfirmed,
                                  bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling,
                                  bool postSLConfirmedButRuntimeStateStabilizing) {
    const bool forwarded = !setOptionsCallSuppressed;
    const bool streamlineFGSignalActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();

    static std::mutex s_setOptionsLogMutex;
    static std::unordered_map<uint32_t, DLSSGSetOptionsLogState> s_setOptionsLogState;

    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(s_setOptionsLogMutex);
        auto& last = s_setOptionsLogState[viewportKey];
        shouldLog =
            !last.valid || last.requestedEnabled != requestedEnabled || last.forwarded != forwarded ||
            last.requestMode != requestedOptions.mode || last.forwardedMode != forwardedOptions.mode ||
            last.requestedGeneratedFrames != requestedGeneratedFrames ||
            last.forwardedGeneratedFrames != forwardedOptions.numFramesToGenerate ||
            last.capabilityMax != capabilityMax || last.result != result || last.overrideApplied != overrideApplied ||
            last.overrideClamped != overrideClamped || last.startupWindowActive != startupWindowActive ||
            last.hadFSRFGPhase != hadFSRFGPhase ||
            last.explicitSetOptionsActivationForCurrentComeback != explicitSetOptionsActivationForCurrentComeback ||
            last.safePostFSRBootstrapPath != safePostFSRBootstrapPath ||
            last.startupActivationPending != startupActivationPending ||
            last.postSLActiveButUnconfirmed != postSLActiveButUnconfirmed ||
            last.postSLConfirmedRendering != postSLConfirmedRendering ||
            last.postSLConfirmedButStartupSettling != postSLConfirmedButStartupSettling ||
            last.postSLConfirmedButRuntimeStateStabilizing != postSLConfirmedButRuntimeStateStabilizing ||
            last.streamlineFGSignalActive != streamlineFGSignalActive || last.pureObserverOnly != pureObserverOnly ||
            last.runtimeMode != runtimeMode;
        if (shouldLog) {
            last.valid = true;
            last.requestedEnabled = requestedEnabled;
            last.forwarded = forwarded;
            last.requestMode = requestedOptions.mode;
            last.forwardedMode = forwardedOptions.mode;
            last.requestedGeneratedFrames = requestedGeneratedFrames;
            last.forwardedGeneratedFrames = forwardedOptions.numFramesToGenerate;
            last.capabilityMax = capabilityMax;
            last.result = result;
            last.overrideApplied = overrideApplied;
            last.overrideClamped = overrideClamped;
            last.startupWindowActive = startupWindowActive;
            last.hadFSRFGPhase = hadFSRFGPhase;
            last.explicitSetOptionsActivationForCurrentComeback = explicitSetOptionsActivationForCurrentComeback;
            last.safePostFSRBootstrapPath = safePostFSRBootstrapPath;
            last.startupActivationPending = startupActivationPending;
            last.postSLActiveButUnconfirmed = postSLActiveButUnconfirmed;
            last.postSLConfirmedRendering = postSLConfirmedRendering;
            last.postSLConfirmedButStartupSettling = postSLConfirmedButStartupSettling;
            last.postSLConfirmedButRuntimeStateStabilizing = postSLConfirmedButRuntimeStateStabilizing;
            last.streamlineFGSignalActive = streamlineFGSignalActive;
            last.pureObserverOnly = pureObserverOnly;
            last.runtimeMode = runtimeMode;
        }
    }

    if (shouldLog) {
        HookLogImportant(
            "Streamline Hook: slDLSSGSetOptions %s viewport=%u requested=%s(%u) forwardedMode=%s(%u) "
            "generated=%u->%u capabilityMax=%u result=%d override=%d clamped=%d startupWindow=%d hadFSR=%d "
            "explicitComeback=%d safeBootstrap=%d pending=%d unconfirmed=%d confirmed=%d settling=%d "
            "stabilizing=%d runtime=%s slSignal=%d observerOnly=%d",
            forwarded ? "forwarded" : "suppressed", viewportKey, GetDLSSGModeName(requestedOptions.mode),
            requestedOptions.mode, GetDLSSGModeName(forwardedOptions.mode), forwardedOptions.mode,
            requestedGeneratedFrames, forwardedOptions.numFramesToGenerate, capabilityMax, result,
            overrideApplied ? 1 : 0, overrideClamped ? 1 : 0, startupWindowActive ? 1 : 0, hadFSRFGPhase ? 1 : 0,
            explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
            startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0,
            ce::fg_runtime::GetRuntimeModeName(runtimeMode), streamlineFGSignalActive ? 1 : 0,
            pureObserverOnly ? 1 : 0);
    }
}

struct ReflexSignalLogState {
    bool valid = false;
    int32_t mode = 0;
    uint32_t incomingFrameLimitUs = 0;
    uint32_t forwardedFrameLimitUs = 0;
    uint32_t targetIntervalUs = 0;
    bool frameLimitOverrideApplied = false;
    bool pacingSignalActive = false;
    bool runtimeDLSSFGApiActive = false;
    bool runtimeFSRFGApiActive = false;
    bool streamlineFGSignalActive = false;
    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kUnknown;
};

void LogStreamlineReflexSignalChange(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
                                     uint32_t forwardedFrameLimitUs, uint32_t targetIntervalUs) {
    const bool pacingSignalActive =
        ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(mode, incomingFrameLimitUs);
    const bool lowLatencyModeEnabled = ce::streamline_runtime_policy::IsStreamlineReflexLowLatencyModeEnabled(mode);
    const bool frameLimitActive =
        ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(incomingFrameLimitUs);
    const bool forwardedFrameLimitActive =
        ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(forwardedFrameLimitUs);
    const bool frameLimitOverrideApplied = incomingFrameLimitUs != forwardedFrameLimitUs;
    const bool runtimeDLSSFGApiActive = g_FGCompat.IsDLSSFGApiActive();
    const bool runtimeFSRFGApiActive = g_FGCompat.IsFSRFGApiActive();
    const bool streamlineFGSignalActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();

    static std::mutex s_reflexSignalLogMutex;
    static ReflexSignalLogState s_optionsState;
    static ReflexSignalLogState s_constantsState;
    const bool isOptionsSource = sourceName && strcmp(sourceName, "slReflexSetOptions") == 0;

    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(s_reflexSignalLogMutex);
        auto& last = isOptionsSource ? s_optionsState : s_constantsState;
        shouldLog = !last.valid || last.mode != mode || last.incomingFrameLimitUs != incomingFrameLimitUs ||
                    last.forwardedFrameLimitUs != forwardedFrameLimitUs || last.targetIntervalUs != targetIntervalUs ||
                    last.frameLimitOverrideApplied != frameLimitOverrideApplied ||
                    last.pacingSignalActive != pacingSignalActive ||
                    last.runtimeDLSSFGApiActive != runtimeDLSSFGApiActive ||
                    last.runtimeFSRFGApiActive != runtimeFSRFGApiActive ||
                    last.streamlineFGSignalActive != streamlineFGSignalActive || last.runtimeMode != runtimeMode;
        if (shouldLog) {
            last.valid = true;
            last.mode = mode;
            last.incomingFrameLimitUs = incomingFrameLimitUs;
            last.forwardedFrameLimitUs = forwardedFrameLimitUs;
            last.targetIntervalUs = targetIntervalUs;
            last.frameLimitOverrideApplied = frameLimitOverrideApplied;
            last.pacingSignalActive = pacingSignalActive;
            last.runtimeDLSSFGApiActive = runtimeDLSSFGApiActive;
            last.runtimeFSRFGApiActive = runtimeFSRFGApiActive;
            last.streamlineFGSignalActive = streamlineFGSignalActive;
            last.runtimeMode = runtimeMode;
        }
    }

    if (shouldLog) {
        HookLogImportant(
            "Streamline Hook: Reflex signal via %s mode=%d lowLatency=%d frameLimitUs=%u frameLimitActive=%d "
            "forwardedFrameLimitUs=%u forwardedFrameLimitActive=%d override=%d pacingActive=%d ceCapActive=%d "
            "ceTargetIntervalUs=%u runtime=%s dlssApi=%d fsrApi=%d slSignal=%d",
            sourceName ? sourceName : "unknown", mode, lowLatencyModeEnabled ? 1 : 0, incomingFrameLimitUs,
            frameLimitActive ? 1 : 0, forwardedFrameLimitUs, forwardedFrameLimitActive ? 1 : 0,
            frameLimitOverrideApplied ? 1 : 0, pacingSignalActive ? 1 : 0, targetIntervalUs > 0 ? 1 : 0,
            targetIntervalUs, ce::fg_runtime::GetRuntimeModeName(runtimeMode), runtimeDLSSFGApiActive ? 1 : 0,
            runtimeFSRFGApiActive ? 1 : 0, streamlineFGSignalActive ? 1 : 0);
    }
}

void MaybePrepareForStreamlineEnableTransitionFromReflex(const char* sourceName) {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeModeIsFSRFG = runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
    const bool runtimeOwnsSwapchain = DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
    if (ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(
            true, g_FGCompat.IsFSRFGApiActive(), runtimeModeIsFSRFG, runtimeOwnsSwapchain)) {
        HookLogImportant(
            "Streamline Hook: Reflex activation requesting Streamline enable preparation via %s "
            "(runtime=%s apiFSR=%d fgOwned=%d)",
            sourceName ? sourceName : "unknown", ce::fg_runtime::GetRuntimeModeName(runtimeMode),
            g_FGCompat.IsFSRFGApiActive() ? 1 : 0, runtimeOwnsSwapchain ? 1 : 0);
        DX12_PrepareForStreamlineEnableTransition();
    }
}

void HandleStreamlineReflexPacingSignal(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
                                        uint32_t forwardedFrameLimitUs, uint32_t targetIntervalUs) {
    const bool lowLatencyModeEnabled = ce::streamline_runtime_policy::IsStreamlineReflexLowLatencyModeEnabled(mode);
    const bool frameLimitActive =
        ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(incomingFrameLimitUs);
    const bool pacingSignalActive =
        ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(mode, incomingFrameLimitUs);

    LogStreamlineReflexSignalChange(sourceName, mode, incomingFrameLimitUs, forwardedFrameLimitUs, targetIntervalUs);

    if (pacingSignalActive) {
        const bool activationEdge = !g_ReflexLimiter.IsGameActivated();
        const bool clearedSuspendIntent =
            g_ConfirmedDLSSReflexSuspendPending.exchange(false, std::memory_order_acq_rel);
        if (activationEdge) {
            HookLogImportant(
                "Streamline Hook: Game ACTIVATED Reflex pacing via %s (mode=%d lowLatency=%d frameLimitUs=%u "
                "frameLimitActive=%d)",
                sourceName ? sourceName : "unknown", mode, lowLatencyModeEnabled ? 1 : 0, incomingFrameLimitUs,
                frameLimitActive ? 1 : 0);
            if (lowLatencyModeEnabled) {
                MaybePrepareForStreamlineEnableTransitionFromReflex(sourceName);
            }
        }
        if (clearedSuspendIntent) {
            HookLogImportant("Streamline Hook: Cleared confirmed Reflex suspend intent on pacing reactivation via %s",
                             sourceName ? sourceName : "unknown");
        }
        g_ReflexLimiter.SetGameActivated(true);
        g_ReflexLimiter.MarkNativePacingSignal();
    } else {
        const bool deactivationEdge = g_ReflexLimiter.IsGameActivated();
        if (deactivationEdge) {
            HookLogImportant("Streamline Hook: Game DEACTIVATED Reflex pacing via %s (mode=%d frameLimitUs=%u)",
                             sourceName ? sourceName : "unknown", mode, incomingFrameLimitUs);
        }
        if (ce::streamline_runtime_policy::ShouldArmConfirmedDLSSReflexSuspendIntent(
                deactivationEdge, g_FGCompat.IsDLSSFGApiActive(),
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                HookIsPostSLOverlayConfirmedRendering(),
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire),
                HookIsPostSLOverlayActiveButUnconfirmed())) {
            const bool wasPending = g_ConfirmedDLSSReflexSuspendPending.exchange(true, std::memory_order_acq_rel);
            const bool startupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
            const bool runtimeStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
                                            HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
            ResetStartupProtectedOffChurnActiveProof("confirmed Reflex suspend intent");
            if (!wasPending) {
                HookLogImportant(
                    "Streamline Hook: Confirmed DLSS-G epoch observed game-owned Reflex OFF via %s "
                    "(startupWindow=%d settling=%d stabilizing=%d) — next inactive GetState/SetOptions edge is "
                    "authoritative so DLSS-G cannot remain active without Reflex (manual limiter target remains "
                    "unchanged)",
                    sourceName ? sourceName : "unknown",
                    DXGIShared::IsStreamlineStartupTransitionWindowActive() ? 1 : 0, startupSettling ? 1 : 0,
                    runtimeStabilizing ? 1 : 0);
            }
        }
        g_ReflexLimiter.SetGameActivated(false);
    }
}

uint32_t GetCachedCapabilityMax(uint32_t viewportKey) {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    const auto it = g_ViewportCapabilityMax.find(viewportKey);
    return it != g_ViewportCapabilityMax.end() ? it->second : 0u;
}

void CacheCapabilityMax(uint32_t viewportKey, uint32_t capabilityMax) {
    if (capabilityMax == 0) {
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        auto& cached = g_ViewportCapabilityMax[viewportKey];
        if (cached != capabilityMax) {
            cached = capabilityMax;
            changed = true;
        }
    }

    if (changed && HookDebugLoggingEnabled()) {
        HookLog("Streamline Hook: Viewport %u reports max generated frames=%u (%dx max)", viewportKey, capabilityMax,
                capabilityMax + 1);
    }
}

void ApplyCombinedDLSSFGState(bool active, int multiplier) {
    if (active) {
        const int effectiveMultiplier = std::clamp(multiplier, 2, 4);
        g_FGCompat.SetDLSSFGMultiplier(effectiveMultiplier);
        g_FGCompat.SetDLSSFGActive(true);

        if (g_IPC && g_IPC->GetSharedMem()) {
            g_IPC->GetSharedMem()->dlssState.fgActive = true;
            g_IPC->GetSharedMem()->dlssState.mfgMultiplier = effectiveMultiplier;
        }
    } else {
        g_FGCompat.SetDLSSFGActive(false);
        g_FGCompat.SetDLSSFGMultiplier(0);

        if (g_IPC && g_IPC->GetSharedMem()) {
            g_IPC->GetSharedMem()->dlssState.fgActive = false;
            g_IPC->GetSharedMem()->dlssState.mfgMultiplier = 0;
        }
    }
}

void ApplyCombinedStreamlineRuntimeState(bool active, int multiplier, bool explicitSetOptionsEnableSignal,
                                         const char* source) {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        const bool previousSignalObserved =
            DXGIShared::g_StreamlineFGRunning.exchange(active, std::memory_order_acq_rel);
        g_FGCompat.SetStreamlineFGSignal(active);
        ApplyCombinedDLSSFGState(active, active ? std::clamp(multiplier, 2, 4) : 0);
        if (previousSignalObserved != active) {
            DX12_OnStreamlineFGStateChanged(active);
            HookLogImportant("Streamline Hook: FG state transition %s->%s via %s (observer-only pass-through)",
                             previousSignalObserved ? "ON" : "OFF", active ? "ON" : "OFF",
                             source ? source : "runtime-state");
        }
        return;
    }

    const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool sourceWasSetOptions = source && strcmp(source, "SetOptions") == 0;
    const bool sourceWasGetState = source && strcmp(source, "GetState") == 0;
    const bool postSLConfirmedButRuntimeStateStabilizingBase = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
    const bool postSLConfirmedButStaleOffWarmupProtected =
        !active && HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
    const bool postSLConfirmedButRuntimeStateStabilizing =
        postSLConfirmedButRuntimeStateStabilizingBase || postSLConfirmedButStaleOffWarmupProtected;
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool hadFSRFGPhase = HookHasFSRFGHistory();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool startupProtectedComebackProof =
        explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
    const bool postSLConfirmedButOffChurnAwaitingActiveProof = IsStartupProtectedOffChurnAwaitingActiveProof(
        startupProtectedComebackProof, postSLConfirmedRendering, postSLConfirmedButStartupSettling);
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            !active, startupWindowActive, startupProtectedComebackProof, startupActivationPending,
            postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
    const bool explicitSetOptionsDisableIsAuthoritative =
        ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
            !active, sourceWasSetOptions, postSLConfirmedRendering, startupActivationPending,
            postSLActiveButUnconfirmed, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof,
            g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
    const bool previousSignal = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool confirmedReflexSuspendIsAuthoritative =
        ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(
            g_ConfirmedDLSSReflexSuspendPending.load(std::memory_order_acquire), !active, previousSignal);
    const bool deferOffSignal =
        !active && !explicitSetOptionsDisableIsAuthoritative && !acceptActivatedUnconfirmedResumeOff &&
        !confirmedReflexSuspendIsAuthoritative &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
    const auto signalUpdate = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(
        active, deferOffSignal, previousSignal, multiplier);
    const bool previousExplicitSetOptionsActivation =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);

    const bool previousSignalObserved =
        DXGIShared::g_StreamlineFGRunning.exchange(signalUpdate.effectiveActive, std::memory_order_acq_rel);
    if (ce::streamline_runtime_policy::ShouldArmStartupTransitionWindowOnFreshActiveSignal(active, previousSignal)) {
        DXGIShared::ArmStreamlineStartupTransitionWindow();
        g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    }
    const bool explicitSetOptionsActivation = explicitSetOptionsEnableSignal;
    const bool updatedExplicitSetOptionsActivation =
        ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
            previousExplicitSetOptionsActivation, signalUpdate.effectiveActive, signalUpdate.freshActivationEdge,
            explicitSetOptionsActivation);
    g_CurrentComebackActivatedViaExplicitSetOptions.store(updatedExplicitSetOptionsActivation,
                                                          std::memory_order_release);
    const bool acceptedRuntimeOffAwaitingSetOptions =
        ce::streamline_runtime_policy::ShouldLatchAcceptedRuntimeOffAwaitingSetOptions(
            previousSignalObserved, signalUpdate.effectiveActive, sourceWasGetState);
    if (acceptedRuntimeOffAwaitingSetOptions) {
        g_AcceptedRuntimeOffAwaitingSetOptions.store(true, std::memory_order_release);
        const bool wasBlockingGetStateOnlyReactivation =
            g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(true, std::memory_order_acq_rel);
        HookLogImportant(
            "Streamline Hook: Accepted runtime OFF via %s before matching SetOptions — forwarding the next "
            "SetOptions(OFF) despite stale PostSL startup proof and blocking GetState-only reactivation "
            "(previousBlock=%d)",
            source ? source : "runtime-state", wasBlockingGetStateOnlyReactivation ? 1 : 0);
    }
    g_FGCompat.SetStreamlineFGSignal(signalUpdate.effectiveActive);
    ApplyCombinedDLSSFGState(signalUpdate.effectiveActive, signalUpdate.effectiveMultiplier);
    if (confirmedReflexSuspendIsAuthoritative && !signalUpdate.deferredOffDuringStartupWindow) {
        const bool consumedSuspendIntent =
            g_ConfirmedDLSSReflexSuspendPending.exchange(false, std::memory_order_acq_rel);
        ResetStartupProtectedOffChurnActiveProof("accepted confirmed Reflex suspend runtime OFF");
        if (consumedSuspendIntent) {
            HookLogImportant(
                "Streamline Hook: Accepted %s OFF as authoritative after confirmed Reflex suspend — "
                "startup churn protection remains armed for future cold starts",
                source ? source : "runtime-state");
        }
    } else if (acceptActivatedUnconfirmedResumeOff) {
        LogAcceptedOffDuringActivatedUnconfirmedResume(
            source, startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
            postSLStartupActivationEntered, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
        ResetStartupProtectedOffChurnActiveProof("accepted activated-unconfirmed startup suspend");
    } else if (explicitSetOptionsDisableIsAuthoritative) {
        ResetStartupProtectedOffChurnActiveProof("accepted authoritative SetOptions disable");
    } else if (!active && signalUpdate.deferredOffDuringStartupWindow) {
        MarkStartupProtectedOffChurnObserved(
            source, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
    } else if (active) {
        MarkStartupProtectedActiveRuntimeProof(source, signalUpdate.effectiveMultiplier);
    } else if (!signalUpdate.effectiveActive) {
        ResetStartupProtectedOffChurnActiveProof("accepted inactive Streamline runtime signal");
