#include "streamline_hook_internal.h"


bool IsObserverOnlyModeActive() {


    return HookOverlayObserverOnlyEnabled();

}

void LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
    uint32_t viewportKey,  bool hadFSRFGPhase,  bool explicitSetOptionsActivationForCurrentComeback, 
    bool safePostFSRBootstrapPath,  bool startupActivationPending,  bool postSLActiveButUnconfirmed, 
    bool postSLConfirmedRendering,  bool postSLConfirmedButStartupSettling, 
    bool postSLConfirmedButRuntimeStateStabilizing) {


    static std::atomic<int> s_dropLogCount{0};
    const int logCount = s_dropLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "Streamline Hook: Dropping stale suppressed slDLSSGSetOptions(OFF) after startup window expiry because "
            "Streamline DLSS startup is already stably active (viewport=%u hadFSR=%d explicit=%d safeBootstrap=%d "
            "pending=%d unconfirmed=%d confirmed=%d settling=%d stabilizing=%d)",
            viewportKey, hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
            safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
            postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
            postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
    }

}

bool IsObserverPolicyOnlyModeActive() {


    return HookOverlayObserverPolicyOnlyEnabled();

}

bool ShouldKeepPureObserverOnlyStreamlineBehavior() {


    return ce::streamline_runtime_policy::ShouldKeepPureObserverOnlyStreamlineBehavior(
        IsObserverOnlyModeActive(), IsObserverPolicyOnlyModeActive());

}

bool TryServicePostSLStartupActivation(const char* source,  bool clearStartupWindow) {


    auto service = DXGIShared::g_PostSLStartupActivationService.load(std::memory_order_acquire);
    if (!service) {
        static std::atomic<int> s_missingServiceLogCount{0};
        const int logCount = s_missingServiceLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: PostSL startup activation service unavailable "
                "(source=%s clearWindow=%d)",
                source ? source : "unknown", clearStartupWindow ? 1 : 0);
        }
        return false;
    }

    return service(source, clearStartupWindow);

}

void ResetStartupProtectedOffChurnActiveProof(const char* reason) {


    const bool wasPending = streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(false, std::memory_order_acq_rel);
    const uint32_t previousProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel);
    if (wasPending || previousProof > 0) {
        static std::atomic<int> s_resetLogCount{0};
        const int logCount = s_resetLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Reset startup-protected OFF quiet proof "
                "(reason=%s wasPending=%d activeProof=%u)",
                reason ? reason : "unknown", wasPending ? 1 : 0, previousProof);
        }
    }

}

void LogAcceptedOffDuringActivatedUnconfirmedResume(const char* source,  bool startupWindowActive,  bool hadFSRFGPhase, 
                                                    bool explicitSetOptionsActivationForCurrentComeback, 
                                                    bool safePostFSRBootstrapPath,  bool startupActivationPending, 
                                                    bool postSLActiveButUnconfirmed, 
                                                    bool postSLStartupActivationEntered,  bool postSLConfirmedRendering, 
                                                    bool postSLConfirmedButStartupSettling, 
                                                    bool postSLConfirmedButRuntimeStateStabilizing) {


    static std::atomic<int> s_acceptLogCount{0};
    const int logCount = s_acceptLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 100) == 0) {
        HookLogImportant(
            "Streamline Hook: Accepting Streamline OFF during activated-but-unconfirmed startup resume "
            "(source=%s startupWindow=%d hadFSR=%d explicit=%d safeBootstrap=%d pending=%d unconfirmed=%d "
            "startupActivationEntered=%d confirmed=%d settling=%d stabilizing=%d) — forwarding real suspend instead "
            "of treating it as stale startup churn",
            source ? source : "runtime-state", startupWindowActive ? 1 : 0, hadFSRFGPhase ? 1 : 0,
            explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
            startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
            postSLStartupActivationEntered ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
    }

}

void MarkStartupProtectedOffChurnObserved(const char* source,  bool postSLConfirmedRendering, 
                                          bool postSLConfirmedButStartupSettling, 
                                          bool postSLConfirmedButRuntimeStateStabilizing) {


    const bool wasPending = streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(true, std::memory_order_acq_rel);
    const uint32_t previousProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel);
    if (!wasPending || previousProof > 0) {
        static std::atomic<int> s_churnLogCount{0};
        const int logCount = s_churnLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup-protected OFF churn requires fresh active proof before accepting disable "
                "(source=%s previousProof=%u required=%u confirmed=%d settling=%d stabilizing=%d)",
                source ? source : "runtime-state", previousProof,
                ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(),
                postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
        }
    }

}

void MarkStartupProtectedActiveRuntimeProof(const char* source,  int multiplier) {


    if (!streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t previousProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire);
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(previousProof)) {
        return;
    }

    const uint32_t newProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(newProof)) {
        const bool wasPending = streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(false, std::memory_order_acq_rel);
        if (wasPending) {
            HookLogImportant(
                "Streamline Hook: Startup-protected OFF churn quiet proof reached "
                "(source=%s activeProof=%u required=%u multiplier=%dx) — future OFF edges may be accepted",
                source ? source : "runtime-state", newProof,
                ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(), multiplier);
        }
    } else {
        static std::atomic<int> s_activeProofLogCount{0};
        const int logCount = s_activeProofLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup-protected OFF churn active proof progress "
                "(source=%s activeProof=%u/%u multiplier=%dx)",
                source ? source : "runtime-state", newProof,
                ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(), multiplier);
        }
    }

}

bool IsStartupProtectedOffChurnAwaitingActiveProof(bool startupProtectedComebackProof,  bool postSLConfirmedRendering, 
                                                   bool postSLConfirmedButStartupSettling) {


    return ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
        streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.load(std::memory_order_acquire),
        streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire), startupProtectedComebackProof,
        postSLConfirmedRendering, postSLConfirmedButStartupSettling);

}

void FormatDLSSGStatusFlags(uint32_t status,  char* buffer,  size_t bufferSize) {


    if (!buffer || bufferSize == 0) {
        return;
    }
    if (status == 0) {
        snprintf(buffer, bufferSize, "ok");
        return;
    }
    buffer[0] = '\0';
    size_t used = 0;
    auto append = [&](const char* text) {
        const int written =
            snprintf(buffer + used, bufferSize > used ? bufferSize - used : 0, "%s%s", used ? "|" : "", text);
        if (written > 0) {
            used += static_cast<size_t>(written);
        }
    };
    if (status & streamline_hook_kDLSSGStatusFailResolutionTooLow) {
        append("resolutionTooLow");
    }
    if (status & streamline_hook_kDLSSGStatusFailReflexNotDetectedAtRuntime) {
        append("REFLEX-NOT-DETECTED");
    }
    if (status & streamline_hook_kDLSSGStatusFailHDRFormatNotSupported) {
        append("hdrFormatNotSupported");
    }
    if (status & streamline_hook_kDLSSGStatusFailCommonConstantsInvalid) {
        append("commonConstantsInvalid");
    }
    if (status & streamline_hook_kDLSSGStatusFailGetCurrentBackBufferIndex) {
        append("getCurrentBackBufferIndexFail");
    }
    const uint32_t knownMask = streamline_hook_kDLSSGStatusFailResolutionTooLow | streamline_hook_kDLSSGStatusFailReflexNotDetectedAtRuntime |
                               streamline_hook_kDLSSGStatusFailHDRFormatNotSupported | streamline_hook_kDLSSGStatusFailCommonConstantsInvalid |
                               streamline_hook_kDLSSGStatusFailGetCurrentBackBufferIndex;
    if (status & ~knownMask) {
        char unknownText[32];
        snprintf(unknownText, sizeof(unknownText), "unknown(0x%X)", status & ~knownMask);
        append(unknownText);
    }

}

const char* GetDLSSGModeName(uint32_t mode) {


    switch (mode) {
        case 0:
            return "off";
        case 1:
            return "on";
        case 2:
            return "auto";
        default:
            return "unknown";
    }

}

const char* GetModuleBaseName(const char* moduleNameOrPath) {


    if (!moduleNameOrPath || !moduleNameOrPath[0]) {
        return nullptr;
    }

    const char* baseName = moduleNameOrPath;
    for (const char* cursor = moduleNameOrPath; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            baseName = cursor + 1;
        }
    }
    return baseName;

}

bool IsStreamlineModuleName(const char* moduleNameOrPath) {


    return ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath);

}

bool ShouldHookStreamlineCoreExports(const char* moduleNameOrPath) {


    return ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad(moduleNameOrPath);

}

bool IsStreamlineCoreDynamicHookModule(const char* moduleBaseName,  HMODULE) {


    return ShouldHookStreamlineCoreExports(moduleBaseName);

}

bool IsStreamlineDLSSGDynamicHookModule(const char* moduleBaseName,  HMODULE) {


    return ce::streamline_runtime_policy::IsStreamlineDLSSGFeatureModuleName(moduleBaseName);

}

bool IsStreamlineReflexDynamicHookModule(const char* moduleBaseName,  HMODULE) {


    return ce::streamline_runtime_policy::IsStreamlineReflexFeatureModuleName(moduleBaseName);

}

uint32_t GetModuleMaskBit(const char* moduleNameOrPath) {


    const char* baseName = GetModuleBaseName(moduleNameOrPath);
    if (!baseName) {
        return 0;
    }
    if (!_stricmp(baseName, "sl.interposer.dll")) {
        return 1u << 0;
    }
    if (!_stricmp(baseName, "sl.common.dll")) {
        return 1u << 1;
    }
    return 0;

}

void LogSkippedStreamlineCoreExportsOnce(const char* moduleBaseName,  HMODULE module,  bool hasGetFeature, 
                                         bool hasGetPlugin,  bool hasSetD3DDevice) {


    if (!moduleBaseName || !moduleBaseName[0]) {
        return;
    }

    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedModules;

    std::string key = moduleBaseName;
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(module));

    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_loggedModules.find(key) != s_loggedModules.end()) {
            return;
        }
        s_loggedModules.emplace(key, true);
    }

    HookLogImportant(
        "Streamline Hook: Skipping generic core exports for unloadable feature module %s (%p) "
        "getFeature=%d getPlugin=%d setD3DDevice=%d",
        moduleBaseName, module, hasGetFeature ? 1 : 0, hasGetPlugin ? 1 : 0, hasSetD3DDevice ? 1 : 0);

}

size_t GetModuleImageSizeBytes(HMODULE module) {


    if (!module) {
        return 0;
    }
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto* ntHeaders =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return ntHeaders->OptionalHeader.SizeOfImage;

}

bool DoesAddressBelongToLoadedModule(void* address,  HMODULE* ownerModule,  char* ownerPath,  DWORD ownerPathCapacity, 
                                     DWORD* outError) {


    if (ownerModule) {
        *ownerModule = nullptr;
    }
    if (ownerPath && ownerPathCapacity > 0) {
        ownerPath[0] = '\0';
    }
    if (outError) {
        *outError = ERROR_SUCCESS;
    }
    if (!address) {
        if (outError) {
            *outError = ERROR_INVALID_ADDRESS;
        }

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

void LogStaleStreamlineOriginalBlockedOnce(const char* streamline_hook_functionName,  void* original,  void* validationAddress, 
                                           const char* expectedModuleRole,  DWORD error) {


    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedOriginals;

    std::string key = streamline_hook_functionName ? streamline_hook_functionName : "<unknown>";
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
        streamline_hook_functionName ? streamline_hook_functionName : "<unknown>", original, validationAddress,
        expectedModuleRole ? expectedModuleRole : "loaded Streamline module", static_cast<unsigned long>(error));

}

bool IsSavedStreamlineOriginalCallable(const char* streamline_hook_functionName,  void* original,  void* validationAddress, 
                                       const char* expectedModuleRole) {


    const void* addressToValidate = validationAddress ? validationAddress : original;
    DWORD ownerError = ERROR_SUCCESS;
    const bool ownerLoaded =
        DoesAddressBelongToLoadedModule(const_cast<void*>(addressToValidate), nullptr, nullptr, 0, &ownerError);
    if (ce::streamline_runtime_policy::ShouldForwardSavedStreamlineOriginal(original != nullptr, ownerLoaded)) {
        return true;
    }

    if (original) {
        LogStaleStreamlineOriginalBlockedOnce(streamline_hook_functionName, original, const_cast<void*>(addressToValidate),
                                              expectedModuleRole, ownerError);
    }
    return false;

}

PFN_slGetFeatureFunction GetCallableOriginalGetFeatureFunction() {


    auto original = streamline_hook_g_Original_slGetFeatureFunction;
    return IsSavedStreamlineOriginalCallable("slGetFeatureFunction", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLGetFeatureFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}

PFN_slGetPluginFunction GetCallableOriginalGetPluginFunction() {


    auto original = streamline_hook_g_Original_slGetPluginFunction;
    return IsSavedStreamlineOriginalCallable("slGetPluginFunction", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLGetPluginFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}

PFN_slSetD3DDevice GetCallableOriginalSetD3DDevice() {


    auto original = streamline_hook_g_Original_slSetD3DDevice;
    return IsSavedStreamlineOriginalCallable("slSetD3DDevice", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLSetD3DDeviceTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}

PFN_slSetTag GetCallableOriginalSetTag() {


    auto original = streamline_hook_g_Original_slSetTag;
    return IsSavedStreamlineOriginalCallable("slSetTag", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLSetTagTarget.load(std::memory_order_acquire), "core Streamline module")
               ? original
               : nullptr;

}

PFN_slSetTagForFrame GetCallableOriginalSetTagForFrame() {


    auto original = streamline_hook_g_Original_slSetTagForFrame;
    return IsSavedStreamlineOriginalCallable("slSetTagForFrame", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLSetTagForFrameTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}

PFN_slEvaluateFeature GetCallableOriginalEvaluateFeature() {


    auto original = streamline_hook_g_Original_slEvaluateFeature;
    return IsSavedStreamlineOriginalCallable("slEvaluateFeature", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLEvaluateFeatureTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}

PFN_slDLSSGSetOptions GetCallableOriginalDLSSGSetOptions() {


    auto original = streamline_hook_g_Original_slDLSSGSetOptions;
    return IsSavedStreamlineOriginalCallable("slDLSSGSetOptions", reinterpret_cast<void*>(original),
                                             streamline_hook_g_DLSSGSetOptionsTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;

}

PFN_slDLSSGGetState GetCallableOriginalDLSSGGetState() {


    auto original = streamline_hook_g_Original_slDLSSGGetState;
    return IsSavedStreamlineOriginalCallable("slDLSSGGetState", reinterpret_cast<void*>(original),
                                             streamline_hook_g_DLSSGGetStateTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;

}

PFN_slReflexSleep GetCallableOriginalReflexSleep() {


    auto original = streamline_hook_g_Original_slReflexSleep;
    return IsSavedStreamlineOriginalCallable("slReflexSleep", reinterpret_cast<void*>(original),
                                             streamline_hook_g_ReflexSleepTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;

}

PFN_slReflexSetOptions GetCallableOriginalReflexSetOptions() {


    auto original = streamline_hook_g_Original_slReflexSetOptions;
    return IsSavedStreamlineOriginalCallable("slReflexSetOptions", reinterpret_cast<void*>(original),
                                             streamline_hook_g_ReflexSetOptionsTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;

}

PFN_slReflexSetConstants GetCallableOriginalReflexSetConstants() {


    auto original = streamline_hook_g_Original_slReflexSetConstants;
    return IsSavedStreamlineOriginalCallable("slReflexSetConstants", reinterpret_cast<void*>(original),
                                             streamline_hook_g_ReflexSetConstantsTarget.load(std::memory_order_acquire),
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
    if (source.structVersion >= streamline_hook_kSLStructVersion2) {
        copy.bReserved15 = source.bReserved15;
    }
    if (source.structVersion >= streamline_hook_kSLStructVersion3) {
        copy.queueParallelismMode = source.queueParallelismMode;
    }
    if (source.structVersion >= streamline_hook_kSLStructVersion4) {
        copy.enableUserInterfaceRecomposition = source.enableUserInterfaceRecomposition;
    }
    if (source.structVersion >= streamline_hook_kSLStructVersion5) {
        copy.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
    }
    if (source.structVersion > streamline_hook_kSLStructVersion5) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true)) {
            HookLogImportant("SL: DLSSG options structVersion=%zu exceeds CE's max (5); forwarding v5 prefix only",
                             source.structVersion);
        }
        copy.structVersion = streamline_hook_kSLStructVersion5;
    }
    return copy;

}

int GetEffectiveMultiplier(const slDLSSGOptions& streamline_hook_options) {


    return ce::streamline_runtime_policy::ResolveDLSSFGMultiplier(
        ce::streamline_runtime_policy::IsDLSSGModeEnabled(streamline_hook_options.mode), streamline_hook_options.numFramesToGenerate);

}

void LogDLSSGSetOptionsTransition(uint32_t viewportKey,  const slDLSSGOptions& requestedOptions, 
                                  const slDLSSGOptions& forwardedOptions,  uint32_t requestedGeneratedFrames, 
                                  uint32_t capabilityMax,  bool requestedEnabled,  bool setOptionsCallSuppressed, 
                                  bool overrideApplied,  bool overrideClamped,  slResult result,  bool pureObserverOnly, 
                                  bool startupWindowActive,  bool hadFSRFGPhase, 
                                  bool explicitSetOptionsActivationForCurrentComeback,  bool safePostFSRBootstrapPath, 
                                  bool startupActivationPending,  bool postSLActiveButUnconfirmed, 
                                  bool postSLConfirmedRendering,  bool postSLConfirmedButStartupSettling, 
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
