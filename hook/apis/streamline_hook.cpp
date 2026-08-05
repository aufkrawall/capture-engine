#include "streamline_hook_internal.h"

namespace StreamlineHook {
ExternalOverlayPresentGuard::ExternalOverlayPresentGuard() {
    ++streamline_hook_g_ExternalOverlayPresentGuardDepth;
}
}

namespace StreamlineHook {
ExternalOverlayPresentGuard::~ExternalOverlayPresentGuard() {
    if (streamline_hook_g_ExternalOverlayPresentGuardDepth > 0) {
        --streamline_hook_g_ExternalOverlayPresentGuardDepth;
    }
}
}

namespace StreamlineHook {
bool IsExternalOverlayPresentGuardActive() {
    return streamline_hook_g_ExternalOverlayPresentGuardDepth > 0;
}
}

namespace StreamlineHook {
bool IsExternalOverlayPluginLookupGuardReady() {
    return streamline_hook_g_SLGetPluginFunctionHooked.load(std::memory_order_acquire);
}
}

namespace StreamlineHook {
bool IsAcceptedD3D12Device(IUnknown* device) {
    if (!device) {
        return false;
    }

    IUnknown* candidateIdentity = nullptr;
    IUnknown* acceptedIdentity = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&candidateIdentity))) || !candidateIdentity) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_AcceptedD3D12DeviceMutex);
        if (streamline_hook_g_AcceptedD3D12Device) {
            streamline_hook_g_AcceptedD3D12Device->QueryInterface(IID_PPV_ARGS(&acceptedIdentity));
        }
    }
    const bool matches = acceptedIdentity && candidateIdentity == acceptedIdentity;
    if (acceptedIdentity) {
        acceptedIdentity->Release();
    }
    candidateIdentity->Release();
    return matches;
}
}

namespace StreamlineHook {
bool HasExplicitSetOptionsActivationForCurrentComeback() {
    // Provenance of the current comeback is tracked explicitly. Startup-window
    // OFF churn can temporarily re-arm provisional GetState suppression without
    // changing the fact that the live comeback itself was activated by a fresh
    // OFF->ON SetOptions edge.
    return streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
}
}

namespace StreamlineHook {
void Init() {
    std::lock_guard<std::mutex> lock(streamline_hook_g_InitMutex);
    RegisterDynamicHooksOnce();

    const bool foundModule = ScanLoadedStreamlineModules();

    if (!foundModule) {
        if (!streamline_hook_g_NoModulesLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLog("Streamline Hook: No Streamline modules loaded yet; waiting for module load");
        }
    } else {
        streamline_hook_g_NoModulesLogged.store(false, std::memory_order_release);
    }
}
}

namespace StreamlineHook {
void OnModuleUnloaded(const void* moduleBase, size_t moduleSizeBytes, const char* moduleBaseName) {
    if (!moduleBase || moduleSizeBytes == 0 ||
        !ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload(moduleBaseName)) {
        return;
    }

    // Runs under the loader lock: interlocked/atomic writes and lightweight
    // logging only. Do NOT take g_ModuleHookMutex here (InstallHooksForModule
    // holds it across GetProcAddress, which needs the loader lock).
    struct HookSlotView {
        const char* name;
        std::atomic<void*>* target;
        std::atomic<bool>* installed;
        void* volatile* original;
    };
    HookSlotView slots[] = {
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
        {"slDLSSGSetOptions", &streamline_hook_g_DLSSGSetOptionsTarget, &streamline_hook_g_DLSSGSetOptionsHooked,
         reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slDLSSGSetOptions)},
        {"slDLSSGGetState", &streamline_hook_g_DLSSGGetStateTarget, &streamline_hook_g_DLSSGGetStateHooked,
         reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slDLSSGGetState)},
        {"slReflexSleep", &streamline_hook_g_ReflexSleepTarget, &streamline_hook_g_ReflexSleepHooked,
         reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slReflexSleep)},
        {"slReflexSetOptions", &streamline_hook_g_ReflexSetOptionsTarget, &streamline_hook_g_ReflexSetOptionsHooked,
         reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slReflexSetOptions)},
        {"slReflexSetConstants", &streamline_hook_g_ReflexSetConstantsTarget, &streamline_hook_g_ReflexSetConstantsHooked,
         reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slReflexSetConstants)},
    };

    int invalidatedSlots = 0;
    for (HookSlotView& slot : slots) {
        void* target = slot.target->load(std::memory_order_acquire);
        void* original = *slot.original;
        if (!ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(target, original, moduleBase,
                                                                                          moduleSizeBytes)) {
            continue;
        }
        // Clear the original first so detours fail safe (GetCallableOriginal*
        // returns null) before the installed flag re-arms installation.
        InterlockedExchangePointer(slot.original, nullptr);
        slot.target->store(nullptr, std::memory_order_release);
        slot.installed->store(false, std::memory_order_release);
        ++invalidatedSlots;
        HookLogImportant(
            "Streamline Hook: Invalidated %s hook slot for unloaded %s (target=%p original=%p base=%p size=0x%zX)",
            slot.name, moduleBaseName, target, original, moduleBase, moduleSizeBytes);
    }

    std::atomic<void*>* attemptedTargets[] = {
        &streamline_hook_g_DLSSGSetOptionsImportFallbackAttemptedTarget,    &streamline_hook_g_DLSSGGetStateImportFallbackAttemptedTarget,
        &streamline_hook_g_ReflexSleepImportFallbackAttemptedTarget,        &streamline_hook_g_ReflexSetOptionsImportFallbackAttemptedTarget,
        &streamline_hook_g_ReflexSetConstantsImportFallbackAttemptedTarget,
    };
    for (std::atomic<void*>* attempted : attemptedTargets) {
        void* target = attempted->load(std::memory_order_acquire);
        if (ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(target, nullptr, moduleBase,
                                                                                         moduleSizeBytes)) {
            attempted->store(nullptr, std::memory_order_release);
        }
    }

    const uint32_t moduleBit = GetModuleMaskBit(moduleBaseName);
    if (moduleBit != 0) {
        streamline_hook_g_InstalledModuleMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
        streamline_hook_g_IATPatchesMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
    }

    if (invalidatedSlots > 0 || moduleBit != 0) {
        HookLogImportant(
            "Streamline Hook: Module %s unloaded (base=%p size=0x%zX) — invalidated %d stale hook slot(s); the next "
            "load of this name re-installs hooks for the fresh instance",
            moduleBaseName, moduleBase, moduleSizeBytes, invalidatedSlots);
    }
}
}

namespace StreamlineHook {
void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad(moduleNameOrPath)) {
        return;
    }

    streamline_hook_g_NoModulesLogged.store(false, std::memory_order_release);
    const bool inspectedModule = InstallHooksForModule(module, moduleNameOrPath);
    bool resolvedDLSSG = false;
    bool resolvedReflex = false;
    const bool deferFeatureLookup =
        ce::streamline_runtime_policy::ShouldDeferStreamlineFeatureLookupDuringModuleLoad(true);
    if (!deferFeatureLookup) {
        resolvedDLSSG = TryResolveDLSSGFeatureHooks();
        resolvedReflex = TryResolveReflexFeatureHooks();
    } else if (inspectedModule) {
        static std::atomic<int> s_deferredLookupLogCount{0};
        const int logCount = s_deferredLookupLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Deferred proactive feature-function lookup during module load for %s (%p) "
                "(log=%d); direct exports/IAT hooks installed now, feature lookup will retry after slSetD3DDevice or "
                "app slGetFeatureFunction",
                GetModuleBaseName(moduleNameOrPath), module, logCount);
        }
    }

    if (inspectedModule || resolvedDLSSG || resolvedReflex) {
        HookLogImportant(
            "Streamline Hook: Fresh module load inspected %s (%p) "
            "slGetFeatureFunctionHooked=%d slGetPluginFunctionHooked=%d slSetD3DDeviceHooked=%d "
            "slSetTagHooked=%d slSetTagForFrameHooked=%d slEvaluateFeatureHooked=%d "
            "dlssgSetOptionsHooked=%d "
            "dlssgGetStateHooked=%d reflexSleepHooked=%d reflexSetOptionsHooked=%d reflexSetConstantsHooked=%d",
            GetModuleBaseName(moduleNameOrPath), module,
            streamline_hook_g_SLGetFeatureFunctionHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_SLGetPluginFunctionHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_SLSetD3DDeviceHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_SLSetTagHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_SLSetTagForFrameHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_SLEvaluateFeatureHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0);
    }
}
}

namespace StreamlineHook {
bool IsInitialized() {
    return streamline_hook_g_DynamicHooksRegistered.load(std::memory_order_acquire);
}
}

namespace StreamlineHook {
bool IsDLSSFGRequestedViaStreamline() {
    return DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
}
}

namespace StreamlineHook {
void OnAuthoritativeFFXTakeover() {
    ce::dx12_streamline_ui_overlay::EndActivation("authoritative FFX takeover");
    size_t resetViewportCount = 0;
    size_t preservedCapabilityCount = 0;
    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
        resetViewportCount = streamline_hook_g_ViewportStates.size();
        preservedCapabilityCount = streamline_hook_g_ViewportCapabilityMax.size();
        for (auto& [viewportKey, runtimeState] : streamline_hook_g_ViewportStates) {
            runtimeState.active = false;
            runtimeState.multiplier = 0;
            runtimeState.generatedFrames = 0;
        }
    }

    {
        std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);
        if (streamline_hook_g_SuppressedSetOptionsOffDuringStartup) {
            HookLogImportant(
                "Streamline Hook: Clearing suppressed slDLSSGSetOptions(OFF) due to authoritative FFX takeover");
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    streamline_hook_g_SuppressNewGetStateActivationUntilMs.store(GetTickCount64() + streamline_hook_kAuthoritativeFFXTakeoverGetStateSuppressMs,
                                                 std::memory_order_release);
    streamline_hook_g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.store(true, std::memory_order_release);
    streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.store(false, std::memory_order_release);
    streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
    streamline_hook_g_ConfirmedDLSSReflexSuspendPending.store(false, std::memory_order_release);
    streamline_hook_g_StartupWindowOffExtensionPending.store(false, std::memory_order_release);
    ResetStartupProtectedOffChurnActiveProof("authoritative FFX takeover");
    // A new FSR takeover resets the entire FG session context; any stale DLSS-only
    // reactivation block from a previous epoch must not outlive the FSR phase.
    const bool hadStaleExplicitSetOptionsBlock =
        streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(false, std::memory_order_acq_rel);
    HookLogImportant(
        "Streamline Hook: Authoritative FFX takeover reset %zu viewport states and preserved %zu capability caches; "
        "suppressing GetState-only reactivation for %llums and until safe post-FSR bootstrap or explicit enable "
        "(clearedStaleBlock=%d)",
        resetViewportCount, preservedCapabilityCount, (unsigned long long)streamline_hook_kAuthoritativeFFXTakeoverGetStateSuppressMs,
        hadStaleExplicitSetOptionsBlock ? 1 : 0);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeFFXTakeover,
                                "StreamlineHook::OnAuthoritativeFFXTakeover", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kFSRFG, true, true);
}
}

namespace StreamlineHook {
void OnAuthoritativeStreamlineStartupHandoff() {
    streamline_hook_g_SuppressNewGetStateActivationUntilMs.store(GetTickCount64() + streamline_hook_kAuthoritativeFFXTakeoverGetStateSuppressMs,
                                                 std::memory_order_release);
    streamline_hook_g_ConfirmedDLSSReflexSuspendPending.store(false, std::memory_order_release);
    streamline_hook_g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    ResetStartupProtectedOffChurnActiveProof("authoritative Streamline startup handoff");
    HookLogImportant(
        "Streamline Hook: Authoritative Streamline startup handoff observed — suppressing fresh GetState-only "
        "reactivation for %llums until explicit enable or stable startup evidence arrives",
        (unsigned long long)streamline_hook_kAuthoritativeFFXTakeoverGetStateSuppressMs);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeStreamlineStartupHandoff,
                                "StreamlineHook::OnAuthoritativeStreamlineStartupHandoff", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false);
}
}

namespace StreamlineHook {
void Shutdown() {
    ce::dx12_streamline_ui_overlay::EndActivation("Streamline shutdown");
    std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
    streamline_hook_g_ViewportStates.clear();
    streamline_hook_g_ViewportCapabilityMax.clear();
    streamline_hook_g_SuppressNewGetStateActivationUntilMs.store(0, std::memory_order_release);
    streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.store(false, std::memory_order_release);
    streamline_hook_g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.store(false, std::memory_order_release);
    streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.store(false, std::memory_order_release);
    streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
    streamline_hook_g_ConfirmedDLSSReflexSuspendPending.store(false, std::memory_order_release);
    streamline_hook_g_StartupWindowOffExtensionPending.store(false, std::memory_order_release);
    ResetStartupProtectedOffChurnActiveProof("Streamline shutdown");
    {
        std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);
        streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
    }
    g_FGCompat.SetStreamlineFGSignal(false);
    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
    g_FGCompat.SetStreamlineSupportPresent(false);
    DXGIShared::g_StreamlineFGRunning.store(false, std::memory_order_release);
    streamline_hook_g_StreamlineUsesD3D12.store(false, std::memory_order_release);
    ID3D12Device* acceptedDevice = nullptr;
    {
        std::lock_guard<std::mutex> deviceLock(streamline_hook_g_AcceptedD3D12DeviceMutex);
        acceptedDevice = streamline_hook_g_AcceptedD3D12Device;
        streamline_hook_g_AcceptedD3D12Device = nullptr;
    }
    if (acceptedDevice) {
        acceptedDevice->Release();
    }
}
}

namespace StreamlineHook {
void FlushSuppressedSetOptionsOffIfNeeded() {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        return;
    }


    std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);

    const bool windowStillActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool activationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool callbackInstalled = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool postSLConfirmedButRuntimeStateStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
                                                           HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
    const bool explicitSetOptionsActivationForCurrentComeback =
        streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool hadFSRFGPhase = HookHasFSRFGHistory();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool startupProtectedComebackProof =
        explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
    const bool postSLConfirmedButOffChurnAwaitingActiveProof = IsStartupProtectedOffChurnAwaitingActiveProof(
        startupProtectedComebackProof, postSLConfirmedRendering, postSLConfirmedButStartupSettling);
    const bool effectivePostSLRuntimeStateStabilizing =
        postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof;
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            true, windowStillActive, startupProtectedComebackProof, activationPending, postSLActiveButUnconfirmed,
            postSLStartupActivationEntered, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            effectivePostSLRuntimeStateStabilizing);
    const bool shouldKeepDeferred =
        !acceptActivatedUnconfirmedResumeOff &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            windowStillActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
            activationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            effectivePostSLRuntimeStateStabilizing);
    if (shouldKeepDeferred) {
        if (ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(
                shouldKeepDeferred, windowStillActive, activationPending, postSLStartupActivationEntered,
                callbackInstalled)) {
            const bool serviced = TryServicePostSLStartupActivation(
                "StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded deferred OFF churn", true);
            static std::atomic<int> s_deferredOffServiceLogCount{0};
            const int logCount = s_deferredOffServiceLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "Streamline Hook: Startup-protected OFF churn serviced PostSL startup activation before "
                    "remaining deferred (serviced=%d pending=%d activeButUnconfirmed=%d "
                    "startupActivationEntered=%d)",
                    serviced ? 1 : 0, activationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                    postSLStartupActivationEntered ? 1 : 0);
            }
        }
        return;
    }

    const bool shouldTriggerDirectCallback =
        ce::streamline_runtime_policy::ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(
            activationPending, postSLStartupActivationEntered);

    auto logSkippedDirectCallbackAfterActivation = [&]() {
        static std::atomic<int> s_skipLogCount{0};
        const int logCount = s_skipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup window expired but PostSL startup activation callback already entered — "
                "skipping redundant direct callback until first confirmed render "
                "(activeButUnconfirmed=%d startupActivationEntered=%d)",
                postSLActiveButUnconfirmed ? 1 : 0, postSLStartupActivationEntered ? 1 : 0);
        }
    };

    // Case 1: Suppressed OFF exists — either forward it to Streamline for a real
    // inactive edge, or drop it if a newer post-FSR comeback is already
    // startup-protected and this OFF is now stale churn.
    if (streamline_hook_g_SuppressedSetOptionsOffDuringStartup) {
        if (acceptActivatedUnconfirmedResumeOff) {
            LogAcceptedOffDuringActivatedUnconfirmedResume(
                "periodic suppressed-off flush", windowStillActive, hadFSRFGPhase,
                explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath, activationPending,
                postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        }
        if (!acceptActivatedUnconfirmedResumeOff &&
            ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), postSLConfirmedButStartupSettling,
                effectivePostSLRuntimeStateStabilizing)) {
            LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                streamline_hook_g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, activationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
            ResetStartupProtectedOffChurnActiveProof("dropped stale suppressed OFF after active proof");
        } else {
            auto originalSetOptions = GetCallableOriginalDLSSGSetOptions();
            if (!originalSetOptions) {
                return;
            }
            HookLogImportant(
                "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via periodic flush — startup window "
                "expired (viewport=%u, activationPending=%d settling=%d stabilizing=%d activeProofPending=%d)",
                streamline_hook_g_SuppressedOffViewportKey, activationPending ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                effectivePostSLRuntimeStateStabilizing ? 1 : 0, postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
            const slResult offResult = originalSetOptions(streamline_hook_g_SuppressedOffViewport, streamline_hook_g_SuppressedOffOptions);
            if (offResult != streamline_hook_kSlResultOk) {
                HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) via periodic flush returned %d",
                                 offResult);
            }
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
            ResetStartupProtectedOffChurnActiveProof("forwarded suppressed OFF after startup expiry");
        }

        // When the startup-handoff Present was promoted to top-level and bypassed
        // the synthetic Present path, the PostSL callback may never fire through
        // DetourPresent/DetourPresent1 — or it may be deferred by the startup
        // transition window guard.  In either case, activation remains pending.
        //
        // If activation is still pending, the suppressed OFF we just forwarded to
        // Streamline will tear down FG without PostSL ever completing.  Trigger the
        // PostSL callback directly so CE can at least attempt to complete activation
        // before Streamline receives the OFF signal and potentially destabilizes its
        // FG pipeline.
        //
        // We intentionally distinguish ProcessFrame pre-arming PostSL from the
        // startup callback actually entering.  Pre-armed-but-unentered still needs
        // the retained activation wake path; once the callback entered, repeated
        // direct callbacks stay blocked until the normal Present route confirms.
        //
        // This is critical for GTA V Enhanced DLSS FG startup, where only one
        // startup-handoff Present arrives via the top-level path (bypassing the
        // synthetic route), and then the game's present thread stalls inside Streamline
        // before any synthetic Presents can drive PostSL activation.
        if (shouldTriggerDirectCallback && callbackInstalled) {
            HookLogImportant(
                "Streamline Hook: Activation still pending after OFF flush — "
                "PostSL callback never entered (deferred or bypassed); trigger direct "
                "callback to attempt activation before Streamline processes OFF");
            const bool serviced = TryServicePostSLStartupActivation(
                "StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded after OFF flush", true);
            HookLogImportant("Streamline Hook: PostSL startup activation service after OFF flush returned %d",
                             serviced ? 1 : 0);
        } else if (activationPending && callbackInstalled && postSLStartupActivationEntered) {
            logSkippedDirectCallbackAfterActivation();
        }
        return;
    }

    // Case 2: No suppressed OFF, but startup window just expired and activation
    // is still pending.  This covers the scenario where ON re-arrived and cleared
    // the suppressed OFF before the window expired — ProcessFrame has stalled,
    // so the deferred PostSL callback in ProcessFrame will never fire.  Trigger
    // it here to complete activation before Streamline times out.
    if (shouldTriggerDirectCallback && callbackInstalled) {
        HookLogImportant(
            "Streamline Hook: Startup window expired with activation pending but no "
            "suppressed OFF — triggering PostSL callback directly to complete "
            "activation before Streamline times out");
        const bool serviced =
            TryServicePostSLStartupActivation("StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded expiry", true);
        HookLogImportant("Streamline Hook: PostSL startup activation service after startup expiry returned %d",
                         serviced ? 1 : 0);
    } else if (activationPending && callbackInstalled && postSLStartupActivationEntered) {
        logSkippedDirectCallbackAfterActivation();
    }
}
}
