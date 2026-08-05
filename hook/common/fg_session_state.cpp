#include "fg_session_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "../../common/shared_defs.h"
#include "dx12_fg_transition_model.h"
#include "dxgi_shared.h"
#include "fg_detection.h"
#include "hook_common.h"
#include "overlay_compat.h"

namespace ce::fg_session {
namespace {

struct SessionState {
    std::mutex mutex;
    DX12LegacyStateProvider dx12Provider = nullptr;
    FGSessionSnapshot latestSnapshot;
    FGActionPlan latestPlan;
    bool initialized = false;
    uint32_t sessionEpochCounter = 0;
    uint32_t runtimeEpochCounter = 0;
    uint32_t swapchainEpochCounter = 0;
    uint32_t queueEpochCounter = 0;
    fg_runtime::RuntimeMode lastRuntimeMode = fg_runtime::RuntimeMode::kOff;
    bool lastRuntimeOwnsSwapchain = false;
    const void* lastSwapchainQueuePtr = nullptr;
    const void* lastOriginalQueuePtr = nullptr;
    const void* lastCurrentQueuePtr = nullptr;
    FGAuthorityKind lastAuthority = FGAuthorityKind::kNone;
    FGStartupPhase lastStartupPhase = FGStartupPhase::kNone;
    bool lastManifestSteamOverlayLoaded = false;
    bool lastManifestStreamlineLoaded = false;
    bool lastManifestFFXLoaded = false;
    bool lastManifestShadowEnabled = false;
    uint32_t lastManifestSchemaVersion = 0;
    bool manifestInitialized = false;
    uint32_t presentDecisionLogsRemaining = 0;
    bool runtimeUpdateEventLogValid = false;
    fg_runtime::RuntimeMode lastRuntimeUpdateEventRuntime = fg_runtime::RuntimeMode::kUnknown;
    bool lastRuntimeUpdateEventActive = false;
    bool lastRuntimeUpdateEventExplicit = false;
    uint64_t runtimeUpdateEventLogCount = 0;
};

struct SnapshotBuildResult {
    FGSessionSnapshot snapshot;
    bool runtimeChanged = false;
    bool swapchainChanged = false;
    bool queueChanged = false;
    bool sessionChanged = false;
};

SessionState& GetState() {
    static SessionState state;
    return state;
}

const char* BoolName(bool value) {
    return value ? "1" : "0";
}

const char* SafeString(const char* value) {
    return value ? value : "none";
}

bool IsHighVolumeRuntimeUpdateEvent(FGEventKind kind) {
    return kind == FGEventKind::kStreamlineGetStateRuntimeUpdate ||
           kind == FGEventKind::kStreamlineSetOptionsRuntimeUpdate || kind == FGEventKind::kNativeFSRConfigureOff;
}

bool ShouldLogFGEventLocked(SessionState& state, const FGEvent& event) {
    if (!IsHighVolumeRuntimeUpdateEvent(event.kind)) {
        return true;
    }

    const uint64_t logCount = ++state.runtimeUpdateEventLogCount;
    const bool runtimeStateChanged = !state.runtimeUpdateEventLogValid ||
                                     state.lastRuntimeUpdateEventRuntime != event.hintedRuntimeMode ||
                                     state.lastRuntimeUpdateEventActive != event.hintedActive ||
                                     state.lastRuntimeUpdateEventExplicit != event.hintedExplicitActivation;
    if (runtimeStateChanged) {
        state.runtimeUpdateEventLogValid = true;
        state.lastRuntimeUpdateEventRuntime = event.hintedRuntimeMode;
        state.lastRuntimeUpdateEventActive = event.hintedActive;
        state.lastRuntimeUpdateEventExplicit = event.hintedExplicitActivation;
        return true;
    }

    return logCount <= 16 || (logCount % 512) == 0;
}

FGAuthorityKind ResolveAuthorityKind(const FGSessionSnapshot& snapshot) {
    if (snapshot.nativeFSRConfiguredOn) {
        return FGAuthorityKind::kNativeFSRConfigureAuthoritative;
    }
    if (snapshot.effectiveRuntimeMode == fg_runtime::RuntimeMode::kFSRFG && snapshot.ffxLoaded) {
        return FGAuthorityKind::kNativeFSRContextOnly;
    }
    if (snapshot.effectiveRuntimeMode == fg_runtime::RuntimeMode::kDLSSFG) {
        if (snapshot.explicitSetOptionsActivationForCurrentComeback) {
            return FGAuthorityKind::kStreamlineSetOptionsAuthoritative;
        }
        if (snapshot.streamlineFGSignal) {
            return FGAuthorityKind::kStreamlineGetStateProvisional;
        }
    }
    if (snapshot.effectiveRuntimeMode == fg_runtime::RuntimeMode::kFSRFG) {
        return snapshot.ffxLoaded ? FGAuthorityKind::kNativeFSRContextOnly : FGAuthorityKind::kHeuristic;
    }
    if (snapshot.streamlineLoaded || snapshot.ffxLoaded) {
        return FGAuthorityKind::kHeuristic;
    }
    return FGAuthorityKind::kNone;
}

FGOverlayBackendMode ResolveOverlayBackendMode(const FGSessionSnapshot& snapshot) {
    if (snapshot.observerOnly) {
        return FGOverlayBackendMode::kSuppressed;
    }

    if (snapshot.nativeFSRConfiguredOn && snapshot.runtimeOwnsSwapchain) {
        return FGOverlayBackendMode::kRuntimeOwnedFSRCallback;
    }

    if (snapshot.runtimeOwnsSwapchain && snapshot.hadFSRPhase &&
        snapshot.effectiveRuntimeMode == fg_runtime::RuntimeMode::kOff) {
        return FGOverlayBackendMode::kPostFSRRecovery;
    }

    if (snapshot.effectiveRuntimeMode == fg_runtime::RuntimeMode::kDLSSFG &&
        (snapshot.postSLCallbackInstalled || snapshot.postSLActive || snapshot.postSLConfirmedRendering)) {
        return FGOverlayBackendMode::kPostSL;
    }

    if (snapshot.startupWindowActive || snapshot.streamlineStartupHandoffPending) {
        return FGOverlayBackendMode::kStartupBypass;
    }

    return FGOverlayBackendMode::kNormalPreSL;
}

FGStartupPhase ResolveStartupPhase(const FGSessionSnapshot& snapshot) {
    if (snapshot.streamlineStartupHandoffPending && !snapshot.startupTopLevelPresentConsumed) {
        return FGStartupPhase::kHandoffPending;
    }
    if (snapshot.startupWindowActive) {
        return FGStartupPhase::kChurnWindow;
    }
    if (snapshot.postSLStartupActivationPending) {
        return FGStartupPhase::kActivationPending;
    }
    if (snapshot.postSLActiveButUnconfirmed) {
        return FGStartupPhase::kActiveUnconfirmed;
    }
    if (snapshot.postSLSettling) {
        return FGStartupPhase::kSettling;
    }
    if (snapshot.postSLConfirmedRendering || snapshot.nativeFSRConfiguredOn || snapshot.effectiveFGActive) {
        return FGStartupPhase::kStable;
    }
    return FGStartupPhase::kNone;
}

FGQueueProof MakeQueueProof(ID3D12CommandQueue* queue, const char* source, uint32_t epoch, bool runtimeOwned,
                            bool wrapperDerived, bool directBehindWrapper) {
    FGQueueProof proof;
    proof.ptr = queue;
    proof.valid = queue != nullptr;
    proof.runtimeOwned = runtimeOwned;
    proof.wrapperDerived = wrapperDerived;
    proof.directBehindWrapper = directBehindWrapper;
    proof.epoch = epoch;
    proof.source = source;
    return proof;
}

FGFunctionProof MakeFunctionProof(void* ptr, const char* source, uint32_t epoch) {
    FGFunctionProof proof;
    proof.ptr = ptr;
    proof.valid = ptr != nullptr;
    proof.epoch = epoch;
    proof.source = source;
    return proof;
}

void FillTransportRisk(FGTransportRisk* risk) {
    if (!risk) {
        return;
    }

    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    risk->thirdPartyOverlayLoaded = overlayModule != nullptr;
    risk->steamOverlayLoaded = overlayModule != nullptr && strstr(overlayModule, "gameoverlayrenderer") != nullptr;
    risk->cleanNonWrappedDX12Entry = true;
}

std::vector<std::string> ReadManifestLines(const char* path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

void UpsertManifestLine(std::vector<std::string>* lines, const char* key, const char* value) {
    if (!lines || !key || !value) {
        return;
    }

    const std::string prefix = std::string(key) + "=";
    for (std::string& line : *lines) {
        if (line.rfind(prefix, 0) == 0) {
            line = prefix + value;
            return;
        }
    }
    lines->push_back(prefix + value);
}

void WriteManifestLines(const char* path, const std::vector<std::string>& lines) {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    for (const std::string& line : lines) {
        output << line << '\n';
    }
}

void UpdateSessionManifestIfNeeded(SessionState& state, const FGSessionSnapshot& snapshot) {
    const bool steamOverlayLoaded = snapshot.transportRisk.steamOverlayLoaded;
    const bool streamlineLoaded = snapshot.streamlineLoaded;
    const bool ffxLoaded = snapshot.ffxLoaded;
    const bool shadowEnabled = IsFGShadowStateEnabled();
    const uint32_t schemaVersion = GetFGStateSchemaVersion();

    if (state.manifestInitialized && state.lastManifestSteamOverlayLoaded == steamOverlayLoaded &&
        state.lastManifestStreamlineLoaded == streamlineLoaded && state.lastManifestFFXLoaded == ffxLoaded &&
        state.lastManifestShadowEnabled == shadowEnabled && state.lastManifestSchemaVersion == schemaVersion) {
        return;
    }

    char manifestPath[MAX_PATH] = {};
    if (!BuildLogFilePathForModuleAddress(reinterpret_cast<const void*>(&UpdateSessionManifestIfNeeded),
                                          "session_manifest.txt", manifestPath, sizeof(manifestPath))) {
        return;
    }

    std::vector<std::string> lines = ReadManifestLines(manifestPath);
    UpsertManifestLine(&lines, "steam_overlay_loaded", BoolName(steamOverlayLoaded));
    UpsertManifestLine(&lines, "streamline_loaded", BoolName(streamlineLoaded));
    UpsertManifestLine(&lines, "ffx_loaded", BoolName(ffxLoaded));
    UpsertManifestLine(&lines, "fg_shadow_state_enabled", BoolName(shadowEnabled));
    char schemaBuffer[32] = {};
    _snprintf_s(schemaBuffer, sizeof(schemaBuffer), _TRUNCATE, "%u", schemaVersion);
    UpsertManifestLine(&lines, "fg_state_schema_version", schemaBuffer);
    WriteManifestLines(manifestPath, lines);

    state.lastManifestSteamOverlayLoaded = steamOverlayLoaded;
    state.lastManifestStreamlineLoaded = streamlineLoaded;
    state.lastManifestFFXLoaded = ffxLoaded;
    state.lastManifestShadowEnabled = shadowEnabled;
    state.lastManifestSchemaVersion = schemaVersion;
    state.manifestInitialized = true;
}

bool QueueProofEquals(const FGQueueProof& a, const FGQueueProof& b) {
    return a.ptr == b.ptr && a.valid == b.valid && a.runtimeOwned == b.runtimeOwned &&
           a.wrapperDerived == b.wrapperDerived && a.directBehindWrapper == b.directBehindWrapper;
}

void LogSnapshotLine(const FGSessionSnapshot& snapshot) {
    HookLogImportant(
        "FG SNAPSHOT sessionEpoch=%u runtimeEpoch=%u swapchainEpoch=%u queueEpoch=%u runtimeMode=%s authority=%s "
        "startupPhase=%s overlayMode=%s runtimeOwnsSwapchain=%d hadFSR=%d safeBootstrap=%d explicitSetOptions=%d "
        "postSLCallback=%d postSLActive=%d postSLConfirmed=%d postSLSettling=%d postSLPending=%d stableFrames=%d "
        "cooldown=%d origGame=%p primaryQ=%p scQueue=%p cmdQueue=%p slWrapperQ=%p realBehindWrapperQ=%p "
        "lockedQ=%p lastWorkingQ=%p realECL=%p steamRisk=%d observerOnly=%d observerPolicyOnly=%d "
        "observerStartupPresentOnly=%d",
        snapshot.sessionEpoch, snapshot.runtimeEpoch, snapshot.swapchainEpoch, snapshot.queueEpoch,
        fg_runtime::GetRuntimeModeName(snapshot.effectiveRuntimeMode), GetFGAuthorityKindName(snapshot.authority),
        GetFGStartupPhaseName(snapshot.startupPhase), GetFGOverlayBackendModeName(snapshot.overlayMode),
        snapshot.runtimeOwnsSwapchain ? 1 : 0, snapshot.hadFSRPhase ? 1 : 0, snapshot.safePostFSRBootstrapPath ? 1 : 0,
        snapshot.explicitSetOptionsActivationForCurrentComeback ? 1 : 0, snapshot.postSLCallbackInstalled ? 1 : 0,
        snapshot.postSLActive ? 1 : 0, snapshot.postSLConfirmedRendering ? 1 : 0, snapshot.postSLSettling ? 1 : 0,
        snapshot.postSLStartupActivationPending ? 1 : 0, snapshot.postSLStableFrameCount, snapshot.fgTransitionCooldown,
        snapshot.originalGameQueue.ptr, snapshot.primaryGameQueue.ptr, snapshot.swapchainQueue.ptr,
        snapshot.currentCommandQueue.ptr, snapshot.slWrapperQueue.ptr, snapshot.realQueueBehindWrapper.ptr,
        snapshot.postSLLockedQueue.ptr, snapshot.postSLLastWorkingQueue.ptr, snapshot.realECL.ptr,
        snapshot.transportRisk.steamOverlayLoaded ? 1 : 0, snapshot.observerOnly ? 1 : 0,
        snapshot.observerPolicyOnly ? 1 : 0, snapshot.observerStartupPresentOnly ? 1 : 0);
}

void LogPlanLine(const FGSessionSnapshot& snapshot, const FGActionPlan& plan) {
    HookLogImportant(
        "FG PLAN sessionEpoch=%u runtimeEpoch=%u startupPhase=%s route=%s transport=%s callbackInvoke=%d "
        "callbackKeep=%d backendMode=%s queueRole=%s selectedQueue=%p publishActive=%d publishRuntime=%s "
        "suppressHeuristics=%d preserveLastWorking=%d clearWrapperBootstrap=%d reprobeRealECL=%d reason=%s",
        snapshot.sessionEpoch, snapshot.runtimeEpoch, GetFGStartupPhaseName(snapshot.startupPhase),
        GetFGPresentRouteName(plan.route), GetFGPresentTransportName(plan.transport), plan.invokePostSLCallback ? 1 : 0,
        plan.keepPostSLCallbackInstalled ? 1 : 0, GetFGOverlayBackendModeName(plan.backendMode),
        GetFGQueueRoleName(plan.selectedQueueRole), plan.selectedQueue, plan.publishFGActive ? 1 : 0,
        fg_runtime::GetRuntimeModeName(plan.publishRuntimeMode), plan.suppressHeuristics ? 1 : 0,
        plan.preserveLastWorkingQueue ? 1 : 0, plan.clearWrapperBootstrapState ? 1 : 0,
        plan.reprobRealECLIfMissing ? 1 : 0, SafeString(plan.reason));
}

void LogPlanDiffIfNeeded(const FGSessionSnapshot& previousSnapshot, const FGActionPlan& previousPlan,
                         const FGSessionSnapshot& currentSnapshot, const FGActionPlan& currentPlan) {
    if (previousPlan.route == currentPlan.route && previousPlan.transport == currentPlan.transport &&
        previousPlan.invokePostSLCallback == currentPlan.invokePostSLCallback &&
        previousPlan.keepPostSLCallbackInstalled == currentPlan.keepPostSLCallbackInstalled &&
        previousPlan.backendMode == currentPlan.backendMode &&
        previousPlan.selectedQueueRole == currentPlan.selectedQueueRole &&
        previousPlan.selectedQueue == currentPlan.selectedQueue &&
        previousPlan.publishFGActive == currentPlan.publishFGActive &&
        previousPlan.publishRuntimeMode == currentPlan.publishRuntimeMode &&
        previousPlan.suppressHeuristics == currentPlan.suppressHeuristics) {
        return;
    }

    HookLogImportant(
        "FG PLAN DIFF prevSessionEpoch=%u sessionEpoch=%u prevRoute=%s route=%s prevTransport=%s transport=%s "
        "prevQueueRole=%s queueRole=%s prevQueue=%p queue=%p prevPublish=%d publish=%d prevRuntime=%s runtime=%s",
        previousSnapshot.sessionEpoch, currentSnapshot.sessionEpoch, GetFGPresentRouteName(previousPlan.route),
        GetFGPresentRouteName(currentPlan.route), GetFGPresentTransportName(previousPlan.transport),
        GetFGPresentTransportName(currentPlan.transport), GetFGQueueRoleName(previousPlan.selectedQueueRole),
        GetFGQueueRoleName(currentPlan.selectedQueueRole), previousPlan.selectedQueue, currentPlan.selectedQueue,
        previousPlan.publishFGActive ? 1 : 0, currentPlan.publishFGActive ? 1 : 0,
        fg_runtime::GetRuntimeModeName(previousPlan.publishRuntimeMode),
        fg_runtime::GetRuntimeModeName(currentPlan.publishRuntimeMode));
}

void LogLegacyDecisionLine(const FGSessionSnapshot& snapshot, const FGActionPlan& plan) {
    HookLogImportant(
        "FG LEGACY DECISION sessionEpoch=%u runtimeEpoch=%u startupPhase=%s route=%s transport=%s callback=%d "
        "queueRole=%s queue=%p steamRisk=%d threadSafeBypass=%d confirmed=%d settling=%d",
        snapshot.sessionEpoch, snapshot.runtimeEpoch, GetFGStartupPhaseName(snapshot.startupPhase),
        GetFGPresentRouteName(plan.route), GetFGPresentTransportName(plan.transport), plan.invokePostSLCallback ? 1 : 0,
        GetFGQueueRoleName(plan.selectedQueueRole), plan.selectedQueue,
        snapshot.transportRisk.steamOverlayLoaded ? 1 : 0, plan.transport == FGPresentTransport::kDirectBypass ? 1 : 0,
        snapshot.postSLConfirmedRendering ? 1 : 0, snapshot.postSLSettling ? 1 : 0);
}

void LogTransitionIfNeeded(const FGSessionSnapshot& previousSnapshot, const FGActionPlan& previousPlan,
                           const FGSessionSnapshot& currentSnapshot, const FGActionPlan& currentPlan,
                           const char* trigger) {
    if (previousSnapshot.effectiveRuntimeMode == currentSnapshot.effectiveRuntimeMode &&
        previousSnapshot.authority == currentSnapshot.authority &&
        previousSnapshot.startupPhase == currentSnapshot.startupPhase) {
        return;
    }

    HookLogImportant(
        "FG TRANSITION fromRuntimeMode=%s toRuntimeMode=%s fromAuthority=%s toAuthority=%s fromStartupPhase=%s "
        "toStartupPhase=%s trigger=%s sessionEpoch=%u runtimeEpoch=%u swapchainEpoch=%u queueEpoch=%u "
        "selectedQueueRole=%s selectedQueue=%p route=%s transport=%s callbackInstalled=%d callbackWillInvoke=%d "
        "backendMode=%s publishRuntimeMode=%s",
        fg_runtime::GetRuntimeModeName(previousSnapshot.effectiveRuntimeMode),
        fg_runtime::GetRuntimeModeName(currentSnapshot.effectiveRuntimeMode),
        GetFGAuthorityKindName(previousSnapshot.authority), GetFGAuthorityKindName(currentSnapshot.authority),
        GetFGStartupPhaseName(previousSnapshot.startupPhase), GetFGStartupPhaseName(currentSnapshot.startupPhase),
        SafeString(trigger), currentSnapshot.sessionEpoch, currentSnapshot.runtimeEpoch, currentSnapshot.swapchainEpoch,
        currentSnapshot.queueEpoch, GetFGQueueRoleName(currentPlan.selectedQueueRole), currentPlan.selectedQueue,
        GetFGPresentRouteName(currentPlan.route), GetFGPresentTransportName(currentPlan.transport),
        currentSnapshot.postSLCallbackInstalled ? 1 : 0, currentPlan.invokePostSLCallback ? 1 : 0,
        GetFGOverlayBackendModeName(currentPlan.backendMode),
        fg_runtime::GetRuntimeModeName(currentPlan.publishRuntimeMode));
}

SnapshotBuildResult BuildSnapshotNoLock(SessionState& state) {
    SnapshotBuildResult result;
    FGSessionSnapshot& snapshot = result.snapshot;
    snapshot.sessionEpoch = state.sessionEpochCounter;
    snapshot.runtimeEpoch = state.runtimeEpochCounter;
    snapshot.swapchainEpoch = state.swapchainEpochCounter;
    snapshot.queueEpoch = state.queueEpochCounter;

    snapshot.effectiveRuntimeMode = g_FGCompat.GetRuntimeMode();
    snapshot.effectiveFGActive = g_FGCompat.IsFGActive();
    snapshot.streamlineLoaded = g_FGCompat.HasStreamlineSupport();
    snapshot.streamlineFGSignal = g_FGCompat.IsStreamlineFGSignaled();
    snapshot.ffxLoaded = g_FGCompat.HasFSRFGSupport();
    snapshot.nativeFSRConfiguredOn = g_FGCompat.IsFSRFGApiActive();

    DX12LegacyStateView dx12View;
    if (state.dx12Provider) {
        state.dx12Provider(&dx12View);
    }

    snapshot.runtimeOwnsSwapchain = dx12View.runtimeOwnsSwapchain;
    snapshot.hadFSRPhase = dx12View.hadFSRPhase;
    snapshot.safePostFSRBootstrapPath = dx12View.safePostFSRBootstrapPath;
    snapshot.explicitSetOptionsActivationForCurrentComeback = dx12View.explicitSetOptionsActivationForCurrentComeback;
    snapshot.streamlineStartupHandoffPending = dx12View.streamlineStartupHandoffPending;
    snapshot.startupTopLevelPresentConsumed = dx12View.startupTopLevelPresentConsumed;
    snapshot.postSLCallbackInstalled = dx12View.postSLCallbackInstalled;
    snapshot.postSLActive = dx12View.postSLActive;
    snapshot.postSLConfirmedRendering = dx12View.postSLConfirmedRendering;
    snapshot.postSLSettling = dx12View.postSLSettling;
    snapshot.postSLStartupActivationPending = dx12View.postSLStartupActivationPending;
    snapshot.postSLActiveButUnconfirmed = dx12View.postSLActiveButUnconfirmed;
    snapshot.postSLStableFrameCount = dx12View.postSLStableFrameCount;
    snapshot.fgTransitionCooldown = dx12View.fgTransitionCooldown;
    snapshot.observerOnly = dx12View.observerOnly;
    snapshot.observerPolicyOnly = dx12View.observerPolicyOnly;
    snapshot.observerStartupPresentOnly = dx12View.observerStartupPresentOnly;

    const ULONGLONG startupUntil =
        DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire);
    const ULONGLONG nowMs = GetTickCount64();
    snapshot.startupWindowActive = startupUntil != 0 && startupUntil > nowMs;
    snapshot.startupWindowRemainingMs = snapshot.startupWindowActive ? (startupUntil - nowMs) : 0;

    snapshot.originalGameQueue =
        MakeQueueProof(dx12View.originalGameQueue, "origGame", snapshot.queueEpoch, false, false, false);
    snapshot.primaryGameQueue =
        MakeQueueProof(dx12View.primaryGameQueue, "primaryGame", snapshot.queueEpoch, false, false, false);
    snapshot.swapchainQueue = MakeQueueProof(dx12View.swapchainQueue, "swapchain", snapshot.queueEpoch,
                                             dx12View.runtimeOwnsSwapchain, false, false);
    snapshot.currentCommandQueue = MakeQueueProof(dx12View.currentCommandQueue, "command", snapshot.queueEpoch,
                                                  dx12View.runtimeOwnsSwapchain, false, false);
    snapshot.slWrapperQueue = MakeQueueProof(dx12View.slWrapperQueue, "slWrapper", snapshot.queueEpoch,
                                             dx12View.runtimeOwnsSwapchain, true, false);
    snapshot.realQueueBehindWrapper = MakeQueueProof(dx12View.realQueueBehindWrapper, "realBehindWrapper",
                                                     snapshot.queueEpoch, dx12View.runtimeOwnsSwapchain, true, true);
    snapshot.postSLLockedQueue = MakeQueueProof(dx12View.postSLLockedQueue, "postSLLocked", snapshot.queueEpoch,
                                                dx12View.runtimeOwnsSwapchain, false, false);
    snapshot.postSLLastWorkingQueue = MakeQueueProof(dx12View.postSLLastWorkingQueue, "postSLLastWorking",
                                                     snapshot.queueEpoch, dx12View.runtimeOwnsSwapchain, false, false);
    snapshot.postSLDedicatedQueue =
        MakeQueueProof(dx12View.postSLDedicatedQueue, "postSLDedicated", snapshot.queueEpoch, false, false, false);

    snapshot.realECL = MakeFunctionProof(dx12View.realECL, "realECL", snapshot.queueEpoch);
    snapshot.presentHookAnchor = MakeFunctionProof(nullptr, "presentHookAnchor", snapshot.swapchainEpoch);

    snapshot.authority = ResolveAuthorityKind(snapshot);
    snapshot.overlayMode = ResolveOverlayBackendMode(snapshot);
    snapshot.startupPhase = ResolveStartupPhase(snapshot);

    FillTransportRisk(&snapshot.transportRisk);

    if (state.initialized) {
        const FGSessionSnapshot& previous = state.latestSnapshot;
        result.runtimeChanged = snapshot.effectiveRuntimeMode != state.lastRuntimeMode;
        result.swapchainChanged = snapshot.runtimeOwnsSwapchain != state.lastRuntimeOwnsSwapchain ||
                                  snapshot.swapchainQueue.ptr != state.lastSwapchainQueuePtr;
        result.queueChanged = snapshot.currentCommandQueue.ptr != state.lastCurrentQueuePtr ||
                              snapshot.originalGameQueue.ptr != state.lastOriginalQueuePtr ||
                              snapshot.postSLLastWorkingQueue.ptr != previous.postSLLastWorkingQueue.ptr ||
                              snapshot.realQueueBehindWrapper.ptr != previous.realQueueBehindWrapper.ptr;
        result.sessionChanged =
            result.runtimeChanged || result.swapchainChanged || result.queueChanged ||
            snapshot.authority != previous.authority || snapshot.startupPhase != previous.startupPhase ||
            snapshot.overlayMode != previous.overlayMode || snapshot.effectiveFGActive != previous.effectiveFGActive ||
            snapshot.postSLCallbackInstalled != previous.postSLCallbackInstalled ||
            snapshot.postSLActive != previous.postSLActive ||
            snapshot.postSLConfirmedRendering != previous.postSLConfirmedRendering ||
            snapshot.postSLSettling != previous.postSLSettling ||
            snapshot.postSLStartupActivationPending != previous.postSLStartupActivationPending ||
            snapshot.postSLActiveButUnconfirmed != previous.postSLActiveButUnconfirmed ||
            snapshot.postSLStableFrameCount != previous.postSLStableFrameCount ||
            snapshot.fgTransitionCooldown != previous.fgTransitionCooldown ||
            snapshot.streamlineStartupHandoffPending != previous.streamlineStartupHandoffPending ||
            snapshot.startupTopLevelPresentConsumed != previous.startupTopLevelPresentConsumed ||
            snapshot.startupWindowActive != previous.startupWindowActive ||
            snapshot.startupWindowRemainingMs != previous.startupWindowRemainingMs ||
            snapshot.nativeFSRConfiguredOn != previous.nativeFSRConfiguredOn ||
            snapshot.streamlineFGSignal != previous.streamlineFGSignal ||
            snapshot.observerOnly != previous.observerOnly ||
            snapshot.observerPolicyOnly != previous.observerPolicyOnly ||
            snapshot.observerStartupPresentOnly != previous.observerStartupPresentOnly;
    } else {
        result.runtimeChanged = true;
        result.swapchainChanged = true;
        result.queueChanged = true;
        result.sessionChanged = true;
    }

    return result;
}

FGActionPlan BuildPlanNoLog(const FGSessionSnapshot& snapshot) {
    FGActionPlan plan;

    plan.publishFGActive = snapshot.effectiveFGActive;
    plan.publishRuntimeMode =
        snapshot.effectiveFGActive ? snapshot.effectiveRuntimeMode : fg_runtime::RuntimeMode::kOff;
    plan.backendMode = snapshot.overlayMode;
    plan.keepPostSLCallbackInstalled =
        snapshot.postSLCallbackInstalled || snapshot.effectiveRuntimeMode == fg_runtime::RuntimeMode::kDLSSFG;
    plan.invokePostSLCallback = snapshot.postSLActiveButUnconfirmed || snapshot.postSLSettling;
    plan.suppressHeuristics =
        snapshot.hadFSRPhase && (snapshot.effectiveRuntimeMode == fg_runtime::RuntimeMode::kOff ||
                                 snapshot.effectiveRuntimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG ||
                                 snapshot.startupPhase == FGStartupPhase::kActivationPending ||
                                 snapshot.startupPhase == FGStartupPhase::kActiveUnconfirmed);
    plan.preserveLastWorkingQueue = snapshot.hadFSRPhase && !snapshot.effectiveFGActive;
    plan.clearWrapperBootstrapState = snapshot.effectiveRuntimeMode != fg_runtime::RuntimeMode::kDLSSFG;
    plan.reprobRealECLIfMissing = snapshot.hadFSRPhase && !snapshot.realECL.valid;

    if (snapshot.observerOnly) {
        plan.route = FGPresentRoute::kPassiveBypass;
        plan.transport = FGPresentTransport::kNormalChain;
        plan.keepPostSLCallbackInstalled = false;
        plan.invokePostSLCallback = false;
        plan.selectedQueueRole = FGQueueRole::kNone;
        plan.reason = "observer-only";
        return plan;
    }

    if (snapshot.nativeFSRConfiguredOn && snapshot.runtimeOwnsSwapchain) {
        plan.route = FGPresentRoute::kConfirmedStandaloneNormalRoute;
        plan.transport = FGPresentTransport::kNormalChain;
        plan.selectedQueueRole = FGQueueRole::kFFXCallbackQueue;
        plan.selectedQueue =
            snapshot.swapchainQueue.ptr ? snapshot.swapchainQueue.ptr : snapshot.currentCommandQueue.ptr;
        plan.reason = "native-fsr-callback";
        return plan;
    }

    if (snapshot.effectiveRuntimeMode == fg_runtime::RuntimeMode::kDLSSFG) {
        if (snapshot.startupPhase == FGStartupPhase::kHandoffPending ||
            snapshot.startupPhase == FGStartupPhase::kChurnWindow) {
            plan.route = FGPresentRoute::kStartupHandoffNormalRoute;
            plan.transport = snapshot.transportRisk.staleSteamPresentHookRisk ? FGPresentTransport::kDirectBypass
                                                                              : FGPresentTransport::kNormalChain;
        } else if (snapshot.startupPhase == FGStartupPhase::kActivationPending ||
                   snapshot.startupPhase == FGStartupPhase::kActiveUnconfirmed) {
            plan.route = FGPresentRoute::kSyntheticReentrant;
            plan.transport = snapshot.transportRisk.staleSteamPresentHookRisk ? FGPresentTransport::kDirectBypass
                                                                              : FGPresentTransport::kTrampoline;
        } else {
            plan.route = FGPresentRoute::kConfirmedStandaloneNormalRoute;
            plan.transport = snapshot.transportRisk.staleSteamPresentHookRisk ? FGPresentTransport::kDirectBypass
                                                                              : FGPresentTransport::kNormalChain;
        }

        if (snapshot.postSLLockedQueue.valid) {
            plan.selectedQueueRole = FGQueueRole::kPostSLLastWorking;
            plan.selectedQueue = snapshot.postSLLockedQueue.ptr;
            plan.reason = "locked-postsl-queue";
        } else if (snapshot.realQueueBehindWrapper.valid) {
            plan.selectedQueueRole = FGQueueRole::kRealBehindWrapper;
            plan.selectedQueue = snapshot.realQueueBehindWrapper.ptr;
            plan.reason = "real-queue-behind-wrapper";
        } else if (snapshot.postSLLastWorkingQueue.valid && !snapshot.swapchainQueue.valid) {
            plan.selectedQueueRole = FGQueueRole::kPostSLLastWorking;
            plan.selectedQueue = snapshot.postSLLastWorkingQueue.ptr;
            plan.reason = "reuse-last-working";
        } else if (snapshot.slWrapperQueue.valid &&
                   !(snapshot.hadFSRPhase && snapshot.swapchainQueue.valid && snapshot.safePostFSRBootstrapPath) &&
                   !snapshot.explicitSetOptionsActivationForCurrentComeback) {
            plan.selectedQueueRole = FGQueueRole::kWrapperBootstrap;
            plan.selectedQueue = snapshot.slWrapperQueue.ptr;
            plan.reason = "wrapper-bootstrap";
        } else if (snapshot.swapchainQueue.valid) {
            plan.selectedQueueRole = FGQueueRole::kSwapchain;
            plan.selectedQueue = snapshot.swapchainQueue.ptr;
            plan.reason = "swapchain-queue";
        } else if (snapshot.originalGameQueue.valid) {
            plan.selectedQueueRole = FGQueueRole::kOriginalGame;
            plan.selectedQueue = snapshot.originalGameQueue.ptr;
            plan.reason = "original-game-queue";
        } else {
            plan.selectedQueueRole = FGQueueRole::kNone;
            plan.reason = "no-dlss-queue";
        }
        return plan;
    }

    if (snapshot.runtimeOwnsSwapchain && snapshot.hadFSRPhase && !snapshot.effectiveFGActive) {
        plan.route = FGPresentRoute::kPassiveBypass;
        plan.transport = FGPresentTransport::kNormalChain;
        plan.selectedQueueRole =
            snapshot.postSLLastWorkingQueue.valid ? FGQueueRole::kPostSLLastWorking : FGQueueRole::kOriginalGame;
        plan.selectedQueue = snapshot.postSLLastWorkingQueue.valid ? snapshot.postSLLastWorkingQueue.ptr
                                                                   : snapshot.originalGameQueue.ptr;
        plan.reason = "post-fsr-recovery";
        return plan;
    }

    plan.route = FGPresentRoute::kTopLevel;
    plan.transport = FGPresentTransport::kNormalChain;
    if (snapshot.swapchainQueue.valid) {
        plan.selectedQueueRole = FGQueueRole::kSwapchain;
        plan.selectedQueue = snapshot.swapchainQueue.ptr;
        plan.reason = "normal-swapchain";
    } else if (snapshot.originalGameQueue.valid) {
        plan.selectedQueueRole = FGQueueRole::kOriginalGame;
        plan.selectedQueue = snapshot.originalGameQueue.ptr;
        plan.reason = "normal-origgame";
    } else {
        plan.selectedQueueRole = FGQueueRole::kNone;
        plan.reason = "normal-no-queue";
    }
    return plan;
}

bool ValidateInvariant(bool passed, const char* name, const FGSessionSnapshot& before, const FGSessionSnapshot& after,
                       bool fatal) {
    if (passed) {
        return true;
    }

    HookLogImportant(
        "FG INVARIANT name=%s fatal=%d beforeRuntime=%s afterRuntime=%s beforePhase=%s afterPhase=%s beforeQueue=%p "
        "afterQueue=%p beforePostSLActive=%d afterPostSLActive=%d beforeConfirmed=%d afterConfirmed=%d",
        SafeString(name), fatal ? 1 : 0, fg_runtime::GetRuntimeModeName(before.effectiveRuntimeMode),
        fg_runtime::GetRuntimeModeName(after.effectiveRuntimeMode), GetFGStartupPhaseName(before.startupPhase),
        GetFGStartupPhaseName(after.startupPhase), before.swapchainQueue.ptr, after.swapchainQueue.ptr,
        before.postSLActive ? 1 : 0, after.postSLActive ? 1 : 0, before.postSLConfirmedRendering ? 1 : 0,
        after.postSLConfirmedRendering ? 1 : 0);
    return false;
}

bool ValidateSnapshotAgainstPrevious(const FGSessionSnapshot& current, const FGSessionSnapshot* previous) {
    const FGSessionSnapshot emptyBefore;
    const FGSessionSnapshot& before = previous ? *previous : emptyBefore;
    bool valid = true;
    valid &= ValidateInvariant(!current.postSLConfirmedRendering || current.postSLActive,
                               "postSLConfirmedRenderingImpliesPostSLActive", before, current, true);
    valid &=
        ValidateInvariant(!current.postSLActive || current.postSLCallbackInstalled || current.postSLConfirmedRendering,
                          "postSLActiveImpliesCallbackOrDocumentedDormantState", before, current, false);
    valid &= ValidateInvariant(!current.runtimeOwnsSwapchain ||
                                   current.swapchainQueue.ptr != current.originalGameQueue.ptr ||
                                   current.startupWindowActive,
                               "runtimeOwnsSwapchainImpliesDistinctSwapchainQueue", before, current, false);
    valid &=
        ValidateInvariant(!(current.hadFSRPhase && current.effectiveRuntimeMode == fg_runtime::RuntimeMode::kDLSSFG &&
                            current.postSLConfirmedRendering) ||
                              current.postSLLastWorkingQueue.valid || current.swapchainQueue.valid,
                          "postFSRConfirmedDlssNeedsRecoveryQueue", before, current, true);
    valid &= ValidateInvariant(current.effectiveRuntimeMode != fg_runtime::RuntimeMode::kFSRFG ||
                                   current.overlayMode != FGOverlayBackendMode::kPostSL,
                               "fsrRuntimeMayNotUseSeparateInjectedPostSLPath", before, current, true);
    valid &= ValidateInvariant(!current.explicitSetOptionsActivationForCurrentComeback || current.streamlineFGSignal,
                               "explicitSetOptionsActivationImpliesStreamlineSignal", before, current, false);

    valid &=
        ValidateInvariant(current.startupPhase != FGStartupPhase::kStable || !current.postSLStartupActivationPending,
                          "stableStartupPhaseClearsPendingActivation", before, current, false);
    if (previous) {
        const bool runtimeChanged = previous->effectiveRuntimeMode != current.effectiveRuntimeMode;
        valid &= ValidateInvariant(!runtimeChanged || current.postSLStableFrameCount == 0,
                                   "runtimeChangeResetsPostSLStableFrames", before, current, false);
    }
    return valid;
}

void UpdateEpochs(SessionState& state, SnapshotBuildResult* result) {
    if (!result) {
        return;
    }

    FGSessionSnapshot& snapshot = result->snapshot;

    if (!state.initialized) {
        state.sessionEpochCounter = 1;
        state.runtimeEpochCounter = 1;
        state.swapchainEpochCounter = 1;
        state.queueEpochCounter = 1;
        snapshot.sessionEpoch = state.sessionEpochCounter;
        snapshot.runtimeEpoch = state.runtimeEpochCounter;
        snapshot.swapchainEpoch = state.swapchainEpochCounter;
        snapshot.queueEpoch = state.queueEpochCounter;
        state.initialized = true;
        return;
    }

    if (result->sessionChanged) {
        state.sessionEpochCounter++;
    }
    if (result->runtimeChanged) {
        state.runtimeEpochCounter++;
    }
    if (result->swapchainChanged) {
        state.swapchainEpochCounter++;
    }
    if (result->queueChanged) {
        state.queueEpochCounter++;
    }

    snapshot.sessionEpoch = state.sessionEpochCounter;
    snapshot.runtimeEpoch = state.runtimeEpochCounter;
    snapshot.swapchainEpoch = state.swapchainEpochCounter;
    snapshot.queueEpoch = state.queueEpochCounter;

    const uint32_t queueEpoch = snapshot.queueEpoch;
    snapshot.originalGameQueue.epoch = queueEpoch;
    snapshot.primaryGameQueue.epoch = queueEpoch;
    snapshot.swapchainQueue.epoch = queueEpoch;
    snapshot.currentCommandQueue.epoch = queueEpoch;
    snapshot.slWrapperQueue.epoch = queueEpoch;
    snapshot.realQueueBehindWrapper.epoch = queueEpoch;
    snapshot.postSLLockedQueue.epoch = queueEpoch;
    snapshot.postSLLastWorkingQueue.epoch = queueEpoch;
    snapshot.postSLDedicatedQueue.epoch = queueEpoch;
    snapshot.realECL.epoch = queueEpoch;
    snapshot.presentHookAnchor.epoch = snapshot.swapchainEpoch;
}

void CommitState(SessionState& state, const FGSessionSnapshot& snapshot, const FGActionPlan& plan) {
    state.latestSnapshot = snapshot;
    state.latestPlan = plan;
    state.lastRuntimeMode = snapshot.effectiveRuntimeMode;
    state.lastRuntimeOwnsSwapchain = snapshot.runtimeOwnsSwapchain;
    state.lastSwapchainQueuePtr = snapshot.swapchainQueue.ptr;
    state.lastOriginalQueuePtr = snapshot.originalGameQueue.ptr;
    state.lastCurrentQueuePtr = snapshot.currentCommandQueue.ptr;
    state.lastAuthority = snapshot.authority;
    state.lastStartupPhase = snapshot.startupPhase;
}

void RefreshStateLocked(SessionState& state, const char* trigger, bool emitLogs) {
    const bool hadPrevious = state.initialized;
    SnapshotBuildResult buildResult = BuildSnapshotNoLock(state);
    UpdateEpochs(state, &buildResult);
    FGSessionSnapshot& snapshot = buildResult.snapshot;
    snapshot.authority = ResolveAuthorityKind(snapshot);
    snapshot.overlayMode = ResolveOverlayBackendMode(snapshot);
    snapshot.startupPhase = ResolveStartupPhase(snapshot);

    const FGSessionSnapshot previousSnapshot = state.latestSnapshot;
    const FGActionPlan previousPlan = state.latestPlan;

    const FGActionPlan plan = BuildPlanNoLog(snapshot);

    const bool transitionLike = !hadPrevious || buildResult.runtimeChanged ||
                                snapshot.startupPhase != previousSnapshot.startupPhase ||
                                snapshot.authority != previousSnapshot.authority;
    if (transitionLike) {
        state.presentDecisionLogsRemaining = 10;
    }

    if (emitLogs && buildResult.sessionChanged) {
        ValidateSnapshotAgainstPrevious(snapshot, hadPrevious ? &previousSnapshot : nullptr);
        LogSnapshotLine(snapshot);
        LogPlanLine(snapshot, plan);
        if (hadPrevious) {
            LogPlanDiffIfNeeded(previousSnapshot, previousPlan, snapshot, plan);
            LogTransitionIfNeeded(previousSnapshot, previousPlan, snapshot, plan, trigger);
        }
    }
    CommitState(state, snapshot, plan);
    UpdateSessionManifestIfNeeded(state, snapshot);
}

}  // namespace

void RegisterDX12LegacyStateProvider(DX12LegacyStateProvider provider) {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.dx12Provider = provider;
}

FGSessionSnapshot CaptureFGSessionSnapshot() {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    RefreshStateLocked(state, "snapshot", false);
    return state.latestSnapshot;
}

FGActionPlan BuildFGActionPlan(const FGSessionSnapshot& snapshot) {
    return BuildPlanNoLog(snapshot);
}

bool ValidateFGSessionSnapshot(const FGSessionSnapshot& current, const FGSessionSnapshot* previous) {
    return ValidateSnapshotAgainstPrevious(current, previous);
}

void EmitFGEvent(const FGEvent& event) {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);

    const bool isPresentObserved = event.kind == FGEventKind::kPresentObserved;
    RefreshStateLocked(state, event.source ? event.source : GetFGEventKindName(event.kind), !isPresentObserved);

    const FGSessionSnapshot& snapshot = state.latestSnapshot;
    if (isPresentObserved) {
        if (state.presentDecisionLogsRemaining > 0) {
            LogLegacyDecisionLine(snapshot, state.latestPlan);
            state.presentDecisionLogsRemaining--;
        }
        return;
    }

    if (!ShouldLogFGEventLocked(state, event)) {
        return;
    }

    HookLogImportant(
        "FG EVENT kind=%s source=%s ptrA=%p ptrB=%p runtime=%s active=%d explicit=%d ts=%llu sessionEpoch=%u "
        "runtimeEpoch=%u swapchainEpoch=%u queueEpoch=%u",
        GetFGEventKindName(event.kind), SafeString(event.source), event.ptrA, event.ptrB,
        fg_runtime::GetRuntimeModeName(event.hintedRuntimeMode), event.hintedActive ? 1 : 0,
        event.hintedExplicitActivation ? 1 : 0, static_cast<unsigned long long>(event.timestampMs),
        snapshot.sessionEpoch, snapshot.runtimeEpoch, snapshot.swapchainEpoch, snapshot.queueEpoch);
}

void EmitFGEvent(FGEventKind kind, const char* source, void* ptrA, void* ptrB,
                 fg_runtime::RuntimeMode hintedRuntimeMode, bool hintedActive, bool hintedExplicitActivation) {
    const FGSessionSnapshot snapshot = CaptureFGSessionSnapshot();
    FGEvent event;
    event.kind = kind;
    event.source = source;
    event.ptrA = ptrA;
    event.ptrB = ptrB;
    event.hintedRuntimeMode = hintedRuntimeMode;
    event.hintedActive = hintedActive;
    event.hintedExplicitActivation = hintedExplicitActivation;
    event.timestampMs = GetTickCount64();
    event.sessionEpoch = snapshot.sessionEpoch;
    event.runtimeEpoch = snapshot.runtimeEpoch;
    event.swapchainEpoch = snapshot.swapchainEpoch;
    event.queueEpoch = snapshot.queueEpoch;
    EmitFGEvent(event);
}

FGSessionSnapshot GetLatestFGSessionSnapshot() {
    return CaptureFGSessionSnapshot();
}

FGActionPlan GetLatestFGActionPlan() {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    RefreshStateLocked(state, "latest-plan", false);
    return state.latestPlan;
}

bool IsFGShadowStateEnabled() {
    return true;
}

uint32_t GetFGStateSchemaVersion() {
    return kFGStateSchemaVersion;
}

void ResetFGSessionStateForTests() {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.dx12Provider = nullptr;
    state.latestSnapshot = FGSessionSnapshot{};
    state.latestPlan = FGActionPlan{};
    state.initialized = false;
    state.sessionEpochCounter = 0;
    state.runtimeEpochCounter = 0;
    state.swapchainEpochCounter = 0;
    state.queueEpochCounter = 0;
    state.lastRuntimeMode = fg_runtime::RuntimeMode::kOff;
    state.lastRuntimeOwnsSwapchain = false;
    state.lastSwapchainQueuePtr = nullptr;
    state.lastOriginalQueuePtr = nullptr;
    state.lastCurrentQueuePtr = nullptr;
    state.lastAuthority = FGAuthorityKind::kNone;
    state.lastStartupPhase = FGStartupPhase::kNone;
    state.lastManifestSteamOverlayLoaded = false;
    state.lastManifestStreamlineLoaded = false;
    state.lastManifestFFXLoaded = false;
    state.lastManifestShadowEnabled = false;
    state.lastManifestSchemaVersion = 0;
    state.manifestInitialized = false;
    state.presentDecisionLogsRemaining = 0;
    state.runtimeUpdateEventLogValid = false;
    state.lastRuntimeUpdateEventRuntime = fg_runtime::RuntimeMode::kUnknown;
    state.lastRuntimeUpdateEventActive = false;
    state.lastRuntimeUpdateEventExplicit = false;
    state.runtimeUpdateEventLogCount = 0;
}

const char* GetFGAuthorityKindName(FGAuthorityKind kind) {
    switch (kind) {
        case FGAuthorityKind::kNone:
            return "none";
        case FGAuthorityKind::kStreamlineGetStateProvisional:
            return "sl-getstate";
        case FGAuthorityKind::kStreamlineSetOptionsAuthoritative:
            return "sl-setoptions";
        case FGAuthorityKind::kNativeFSRConfigureAuthoritative:
            return "native-fsr-configure";
        case FGAuthorityKind::kNativeFSRContextOnly:
            return "native-fsr-context";
        case FGAuthorityKind::kHeuristic:
            return "heuristic";
        default:
            return "unknown";
    }
}

const char* GetFGStartupPhaseName(FGStartupPhase phase) {
    switch (phase) {
        case FGStartupPhase::kNone:
            return "none";
        case FGStartupPhase::kHandoffPending:
            return "handoffPending";
        case FGStartupPhase::kChurnWindow:
            return "churnWindow";
        case FGStartupPhase::kActivationPending:
            return "activationPending";
        case FGStartupPhase::kActiveUnconfirmed:
            return "activeUnconfirmed";
        case FGStartupPhase::kSettling:
            return "settling";
        case FGStartupPhase::kStable:
            return "stable";
        default:
            return "unknown";
    }
}

const char* GetFGOverlayBackendModeName(FGOverlayBackendMode mode) {
    switch (mode) {
        case FGOverlayBackendMode::kSuppressed:
            return "suppressed";
        case FGOverlayBackendMode::kNormalPreSL:
            return "normalPreSL";
        case FGOverlayBackendMode::kStartupBypass:
            return "startupBypass";
        case FGOverlayBackendMode::kPostSL:
            return "postSL";
        case FGOverlayBackendMode::kRuntimeOwnedFSRCallback:
            return "runtimeOwnedFSRCallback";
        case FGOverlayBackendMode::kPostFSRRecovery:
            return "postFSRRecovery";
        default:
            return "unknown";
    }
}

const char* GetFGPresentRouteName(FGPresentRoute route) {
    switch (route) {
        case FGPresentRoute::kTopLevel:
            return "topLevel";
        case FGPresentRoute::kSyntheticReentrant:
            return "syntheticReentrant";
        case FGPresentRoute::kStartupHandoffNormalRoute:
            return "startupHandoffNormalRoute";
        case FGPresentRoute::kConfirmedStandaloneNormalRoute:
            return "confirmedStandaloneNormalRoute";
        case FGPresentRoute::kPassiveBypass:
            return "passiveBypass";
        default:
            return "unknown";
    }
}

const char* GetFGPresentTransportName(FGPresentTransport transport) {
    switch (transport) {
        case FGPresentTransport::kNormalChain:
            return "normalChain";
        case FGPresentTransport::kTrampoline:
            return "trampoline";
        case FGPresentTransport::kDirectBypass:
            return "directBypass";
        default:
            return "unknown";
    }
}

const char* GetFGQueueRoleName(FGQueueRole role) {
    switch (role) {
        case FGQueueRole::kNone:
            return "none";
        case FGQueueRole::kOriginalGame:
            return "originalGame";
        case FGQueueRole::kSwapchain:
            return "swapchain";
        case FGQueueRole::kWrapperBootstrap:
            return "wrapperBootstrap";
        case FGQueueRole::kRealBehindWrapper:
            return "realBehindWrapper";
        case FGQueueRole::kDedicatedOverlayQueue:
            return "dedicatedOverlayQueue";
        case FGQueueRole::kPostSLLastWorking:
            return "postSLLastWorking";
        case FGQueueRole::kFFXCallbackQueue:
            return "ffxCallbackQueue";
        default:
            return "unknown";
    }
}

const char* GetFGEventKindName(FGEventKind kind) {
    switch (kind) {
        case FGEventKind::kUnknown:
            return "unknown";
        case FGEventKind::kStreamlineGetStateRuntimeUpdate:
            return "streamline-getstate-runtime-update";
        case FGEventKind::kStreamlineSetOptionsRuntimeUpdate:
            return "streamline-setoptions-runtime-update";
        case FGEventKind::kAuthoritativeStreamlineStartupHandoff:
            return "authoritative-streamline-startup-handoff";
        case FGEventKind::kAuthoritativeFFXTakeover:
            return "authoritative-ffx-takeover";
        case FGEventKind::kNativeFSRConfigureOn:
            return "native-fsr-configure-on";
        case FGEventKind::kNativeFSRConfigureOff:
            return "native-fsr-configure-off";
        case FGEventKind::kFFXContextDestroy:
            return "ffx-context-destroy";
        case FGEventKind::kSwapchainInvalidation:
            return "swapchain-invalidation";
        case FGEventKind::kPresentObserved:
            return "present-observed";
        case FGEventKind::kPostSLCallbackInstalled:
            return "postsl-callback-installed";
        case FGEventKind::kPostSLCallbackRemoved:
            return "postsl-callback-removed";
        case FGEventKind::kPostSLActivationComplete:
            return "postsl-activation-complete";
        case FGEventKind::kPostSLFirstConfirmedRender:
            return "postsl-first-confirmed-render";
        case FGEventKind::kStartupWindowExpired:
            return "startup-window-expired";
        case FGEventKind::kStaleOwnershipCleanupComplete:
            return "stale-ownership-cleanup-complete";
        case FGEventKind::kTransitionCooldownComplete:
            return "transition-cooldown-complete";
        default:
            return "unknown";
    }
}

}  // namespace ce::fg_session

