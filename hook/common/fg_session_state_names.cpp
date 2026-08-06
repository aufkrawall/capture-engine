#include "fg_session_state.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "../../common/shared_defs.h"
#include "fg_detection.h"
#include "fg_session_state_internal.h"
#include "hook_common.h"

namespace ce::fg_session {

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
