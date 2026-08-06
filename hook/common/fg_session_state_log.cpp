#include "fg_session_state.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "../../common/shared_defs.h"
#include "fg_detection.h"
#include "fg_session_state_internal.h"
#include "hook_common.h"

namespace ce::fg_session {

const char* BoolName(bool value) {
    return value ? "1" : "0";
}

const char* SafeString(const char* value) {
    return value ? value : "none";
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

}  // namespace ce::fg_session
