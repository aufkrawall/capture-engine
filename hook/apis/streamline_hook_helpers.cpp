#include "streamline_hook_internal.h"


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
