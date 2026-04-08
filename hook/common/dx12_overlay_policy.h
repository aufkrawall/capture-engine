#pragma once

#include "fg_runtime_state.h"

namespace ce::dx12_overlay_policy {

enum class PostSLBackbufferBarrierMode {
    kUavBarrierOnly,
    kCommonToRenderTarget,
    kPresentToRenderTarget,
};

inline bool ShouldWaitForOverlayCompletion(bool hasFenceEvent, bool usingDedicatedQueue, bool hasStartupBlockingOverlay,
                                           fg_runtime::RuntimeMode runtimeMode) {
    if (!hasFenceEvent) {
        return false;
    }

    if (runtimeMode == fg_runtime::RuntimeMode::kNvidiaSmoothMotion) {
        return false;
    }

    if (usingDedicatedQueue) {
        return true;
    }

    if (!hasStartupBlockingOverlay) {
        return false;
    }

    return runtimeMode == fg_runtime::RuntimeMode::kOff || runtimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG;
}

enum class SwapchainOverlayRoutingDecision {
    kUseNormalRouting,
    kUsePostFSRStreamlineQueue,
    kUseStreamlineOriginalQueue,
    kUseFSRSwapchainQueue,
    kSkipRuntimeOwnedSwapchainWithoutQueue,
    kSkipFSRWithoutSwapchainQueue,
};

inline SwapchainOverlayRoutingDecision DecideSwapchainOverlayRouting(bool runtimeOwnsSwapchain,
                                                                    bool streamlineFGActive,
                                                                    bool fsrFGActive,
                                                                    bool hadFSRFGPhase,
                                                                    bool hasSwapchainQueue,
                                                                    bool hasOriginalGameQueue) {
    if (streamlineFGActive && hadFSRFGPhase) {
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue
                                 : SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue;
    }

    if (streamlineFGActive && hasOriginalGameQueue) {
        return SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue;
    }

    if (runtimeOwnsSwapchain) {
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue
                                 : SwapchainOverlayRoutingDecision::kSkipRuntimeOwnedSwapchainWithoutQueue;
    }

    if (fsrFGActive) {
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue
                                 : SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue;
    }

    return SwapchainOverlayRoutingDecision::kUseNormalRouting;
}

struct OverlayMetricsBindingDecision {
    bool bindMetrics;
    bool refreshFrameMetadata;
};

inline OverlayMetricsBindingDecision DecideOverlayMetricsBinding(bool isRealFrame) {
    return OverlayMetricsBindingDecision{
        true,
        isRealFrame,
    };
}

inline bool ShouldSkipProcessFrameForZeroECLPresent(bool isInterpolatedFrame,
                                                    bool hasDedicatedQueue,
                                                    bool heuristicFSRFG,
                                                    bool runtimeOwnsSwapchain,
                                                    bool streamlineFGRunning) {
    if (!isInterpolatedFrame) {
        return false;
    }

    if (hasDedicatedQueue || heuristicFSRFG) {
        return false;
    }

    // FSR/runtime-owned swapchain transitions can temporarily stop feeding
    // authoritative ECL counts even though top-level Presents are still the
    // frames that must drive normal ProcessFrame recovery.
    if (runtimeOwnsSwapchain && !streamlineFGRunning) {
        return false;
    }

    return true;
}

inline bool ShouldSuppressLikelyDuplicateTopLevelPresent(bool runtimeOwnsSwapchain, bool streamlineFGRunning) {
    // Runtime-owned non-Streamline swapchain transitions can temporarily depend
    // on repeated top-level Presents to drive normal ProcessFrame recovery.
    if (runtimeOwnsSwapchain && !streamlineFGRunning) {
        return false;
    }

    return true;
}

inline bool ShouldGuardSwapchainReinitAfterChange(bool fgCurrentlyActive,
                                                  bool fgRecentlyWasActive,
                                                  bool hasFGTransitionCooldown,
                                                  bool recentStreamlineTeardown,
                                                  bool runtimeOwnsSwapchain,
                                                  bool hasSwapchainQueue,
                                                  bool hasOriginalGameQueue,
                                                  bool swapchainQueueDiffersFromOriginalGameQueue) {
    if (fgCurrentlyActive || fgRecentlyWasActive || hasFGTransitionCooldown) {
        return true;
    }

    // Final DLSS FG teardown can briefly look like "FG fully off" even though
    // the replacement swapchain still belongs to the departing runtime and is
    // bound to a non-game queue. Immediate pre-SL reinit in that window can
    // recreate sync resources on the wrong queue and crash the game.
    return recentStreamlineTeardown && runtimeOwnsSwapchain && hasSwapchainQueue && hasOriginalGameQueue &&
           swapchainQueueDiffersFromOriginalGameQueue;
}

inline bool ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(bool actualFGActive,
                                                                 bool streamlineFGRunning,
                                                                 bool runtimeOwnsSwapchain,
                                                                 bool hasSwapchainQueue,
                                                                 bool hasCommandQueue,
                                                                 bool commandQueueMatchesSwapchainQueue) {
    if (actualFGActive || streamlineFGRunning) {
        return false;
    }

    if (!runtimeOwnsSwapchain || !hasSwapchainQueue) {
        return false;
    }

    // After FG shuts off, the swapchain can persist on a runtime-owned queue
    // while command-list tracking is still null or stuck on a departed wrapper.
    // Reinitializing pre-SL overlay resources before command traffic settles
    // onto the live swapchain queue reproduces the Talos crash path.
    return !hasCommandQueue || !commandQueueMatchesSwapchainQueue;
}

inline bool ShouldRealignInactiveCommandQueueToSwapchainQueue(bool actualFGActive,
                                                              bool streamlineFGRunning,
                                                              bool hasSwapchainQueue,
                                                              bool hasOriginalGameQueue,
                                                              bool hasCommandQueue,
                                                              bool commandQueueMatchesSwapchainQueue,
                                                              bool commandQueueMatchesOriginalGameQueue) {
    if (actualFGActive || streamlineFGRunning) {
        return false;
    }

    if (!hasSwapchainQueue || !hasOriginalGameQueue || !hasCommandQueue) {
        return false;
    }

    // Once Streamline is fully off, a command queue that matches neither the
    // live swapchain queue nor the original game queue is just stale wrapper
    // state. Realign it to the swapchain queue so pre-SL init stops observing a
    // departed wrapper topology.
    return !commandQueueMatchesSwapchainQueue && !commandQueueMatchesOriginalGameQueue;
}

inline bool ShouldMutatePostSLLockedQueue(bool hasLockedQueue, bool selectedQueueMatchesLockedQueue,
                                          bool shouldReplaceLockedQueue) {
    if (!hasLockedQueue) {
        return true;
    }

    if (selectedQueueMatchesLockedQueue) {
        return false;
    }

    return shouldReplaceLockedQueue;
}

inline bool ShouldRememberPostSLLastWorkingQueue(bool queueIsSLWrapper) {
    return !queueIsSLWrapper;
}

inline bool ShouldSyntheticPostSLAdvanceDormantStartup(bool startupActivationPending, bool streamlineFGRunning,
                                                         bool postSLActive, bool processFrameRecentlySeen) {
    return startupActivationPending && streamlineFGRunning && !postSLActive && !processFrameRecentlySeen;
}

inline bool ShouldAllowPostSLWrapperBootstrap(bool hadFSRFGPhase, bool hasRealQueueBehindWrapper,
                                              bool hasRealD3D12ECL) {
    if (hadFSRFGPhase) {
        return false;
    }

    return hasRealQueueBehindWrapper || hasRealD3D12ECL;
}

inline bool ShouldDelayPostSLActivationUntilSafeBootstrapPath(bool hadFSRFGPhase, bool hasRealQueueBehindWrapper,
                                                              bool hasRealD3D12ECL, bool hasSLWrapperQueue) {
    if (!hadFSRFGPhase) {
        return false;
    }

    if (!hasRealD3D12ECL) {
        return true;
    }

    return !hasRealQueueBehindWrapper && !hasSLWrapperQueue;
}

inline bool ShouldUsePostSLScQueueVirtualSubmit(bool hadFSRFGPhase, bool scQueueDiffers) {
    return scQueueDiffers && !hadFSRFGPhase;
}

inline bool ShouldLatchFSRFGHistory(bool fsrFGApiActive, bool sawAuthoritativeFSRRuntimeTraffic) {
    return fsrFGApiActive || sawAuthoritativeFSRRuntimeTraffic;
}

inline bool ShouldTreatPostSLAsReactivated(bool postSLActive, bool wasActiveInCurrentCallbackLifetime,
                                           bool postSLLifecycleChanged) {
    if (!postSLActive) {
        return false;
    }

    return !wasActiveInCurrentCallbackLifetime || postSLLifecycleChanged;
}

inline bool ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(bool hadFSRFGPhase,
                                                                 bool selectedQueueIsSwapchainQueue,
                                                                 bool queueIsSLWrapper,
                                                                 bool hasSelectedQueueSubmitPath,
                                                                 bool hasWrapperDerivedDirectPath) {
    return hadFSRFGPhase && selectedQueueIsSwapchainQueue && !queueIsSLWrapper && hasSelectedQueueSubmitPath &&
           !hasWrapperDerivedDirectPath;
}

inline bool ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(bool hadFSRFGPhase,
                                                             bool selectedQueueIsSwapchainQueue,
                                                             bool hasSelectedQueueOrigECL,
                                                             bool selectedQueueOrigECLMatchesRealECL,
                                                             bool hasDirectQueueBehindWrapper) {
    return hadFSRFGPhase && !selectedQueueIsSwapchainQueue && !hasDirectQueueBehindWrapper &&
           hasSelectedQueueOrigECL && selectedQueueOrigECLMatchesRealECL;
}

inline bool ShouldUsePostSLRealQueueBehindWrapperAfterFSR(bool hadFSRFGPhase,
                                                          bool streamlineFGActive,
                                                          bool hasDirectQueueBehindWrapper) {
    return hadFSRFGPhase && streamlineFGActive && hasDirectQueueBehindWrapper;
}

inline bool ShouldUsePostSLWrapperBootstrapQueueAfterFSR(bool hadFSRFGPhase,
                                                         bool streamlineFGActive,
                                                         bool hasDirectQueueBehindWrapper,
                                                         bool hasSLWrapperQueue) {
    return false;
}

inline bool ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(bool hadFSRFGPhase,
                                                                   bool streamlineFGActive,
                                                                   bool hasDirectQueueBehindWrapper,
                                                                   bool commandQueueIsWrapper) {
    return hadFSRFGPhase && streamlineFGActive && !hasDirectQueueBehindWrapper && commandQueueIsWrapper;
}

inline bool ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(bool hadFSRFGPhase,
                                                                 bool streamlineFGActive,
                                                                 bool queueIsSLWrapper,
                                                                 bool hasDirectQueueBehindWrapper) {
    return hadFSRFGPhase && streamlineFGActive && queueIsSLWrapper && !hasDirectQueueBehindWrapper;
}

inline bool ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(bool hasLockedQueue,
                                                                                  bool hadFSRFGPhase,
                                                                                  bool streamlineFGActive,
                                                                                  bool lockedQueueIsSLWrapper,
                                                                                  bool hasDirectQueueBehindWrapper) {
    return hasLockedQueue && hadFSRFGPhase && streamlineFGActive && lockedQueueIsSLWrapper &&
           hasDirectQueueBehindWrapper;
}

inline bool ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(bool hasLockedQueue,
                                                                            bool hadFSRFGPhase,
                                                                            bool streamlineFGActive,
                                                                            bool lockedQueueIsSLWrapper,
                                                                            bool hasSwapchainQueue,
                                                                            bool swapchainQueueDiffersFromOriginalGameQueue,
                                                                            bool hasSwapchainQueueSubmitPath,
                                                                            bool hasWrapperDerivedDirectPath) {
    return hasLockedQueue && hadFSRFGPhase && streamlineFGActive && lockedQueueIsSLWrapper && hasSwapchainQueue &&
           swapchainQueueDiffersFromOriginalGameQueue && hasSwapchainQueueSubmitPath && !hasWrapperDerivedDirectPath;
}

inline bool ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(bool hadFSRFGPhase,
                                                                          bool streamlineFGActive,
                                                                          int postFSRProbeLevel,
                                                                          bool hasDirectQueueBehindWrapper,
                                                                          bool hasSLWrapperQueue) {
    return hadFSRFGPhase && streamlineFGActive && postFSRProbeLevel == 0 && !hasDirectQueueBehindWrapper &&
           hasSLWrapperQueue;
}

inline bool ShouldPinPostSLWrapperQueueAfterFSR(bool hadFSRFGPhase,
                                                 bool usePostSLOffscreenComposite,
                                                 bool selectedQueueIsSwapchainQueue,
                                                 bool hasPinnedWrapperQueue,
                                                 bool hasCapturedSLWrapperQueue,
                                                 bool preferSelectedSwapchainQueueSubmitAfterFSR) {
    return false;
}

inline bool ShouldUsePostSLWrapperSubmitAfterFSR(bool hadFSRFGPhase,
                                                  bool usePostSLOffscreenComposite,
                                                  bool selectedQueueIsSwapchainQueue,
                                                  bool hasSLWrapperQueue,
                                                  bool preferSelectedSwapchainQueueSubmitAfterFSR) {
    return false;
}

inline bool ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(bool hadFSRFGPhase,
                                                                                bool streamlineFGActive,
                                                                                bool selectedQueueIsSwapchainQueue,
                                                                                bool queueIsSLWrapper) {
    return hadFSRFGPhase && streamlineFGActive && selectedQueueIsSwapchainQueue && !queueIsSLWrapper;
}

inline PostSLBackbufferBarrierMode DecidePostSLBackbufferBarrierMode(bool streamlineFGActive,
                                                                     bool useExplicitPostFSRSwapchainTransitions) {
    if (streamlineFGActive && !useExplicitPostFSRSwapchainTransitions) {
        return PostSLBackbufferBarrierMode::kUavBarrierOnly;
    }

    if (useExplicitPostFSRSwapchainTransitions) {
        return PostSLBackbufferBarrierMode::kPresentToRenderTarget;
    }

    return PostSLBackbufferBarrierMode::kCommonToRenderTarget;
}

inline bool ShouldUsePostSLOffscreenCompositeAfterFSR(bool hadFSRFGPhase,
                                                       bool streamlineFGActive,
                                                       bool selectedQueueIsSwapchainQueue,
                                                       bool queueIsSLWrapper) {
    return hadFSRFGPhase && streamlineFGActive && selectedQueueIsSwapchainQueue && !queueIsSLWrapper;
}

inline bool ShouldUseExplicitBackbufferCopyTransitionsForPostFSROffscreenComposite(
    bool usePostSLOffscreenComposite, bool useExplicitPostFSRSwapchainTransitions) {
    return usePostSLOffscreenComposite && useExplicitPostFSRSwapchainTransitions;
}

inline bool ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(bool hadFSRFGPhase,
                                                          int postFSRProbeLevel,
                                                          bool usePostSLOffscreenComposite) {
    return hadFSRFGPhase && usePostSLOffscreenComposite && postFSRProbeLevel == 2;
}

inline bool ShouldSyntheticPostSLRefreshMetrics(bool streamlineFGRunning, bool processFrameRecentlySeen) {
    return streamlineFGRunning && !processFrameRecentlySeen;
}

inline bool ShouldPreserveConfirmedPostSLDuringFGCooldown(bool streamlineFGRunning,
                                                          bool postSLConfirmedRendering) {
    return streamlineFGRunning && postSLConfirmedRendering;
}

inline bool ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(bool streamlineFGRunning,
                                                              bool postSLActive,
                                                              bool postSLConfirmedRendering,
                                                              bool startupActivationPending) {
    return !streamlineFGRunning && !postSLActive && !postSLConfirmedRendering && !startupActivationPending;
}

inline bool ShouldForceEndStreamlineOwnershipForSwapchainTakeover(bool runtimeOwnsSwapchain,
                                                                  bool callerFromFFXFGModule,
                                                                  bool streamlineFGRunning,
                                                                  bool streamlineStartupHandoffPending,
                                                                  bool runtimeOwnershipJustActivated) {
    if (!runtimeOwnsSwapchain) {
        return false;
    }

    if (callerFromFFXFGModule) {
        return true;
    }

    return streamlineFGRunning && !streamlineStartupHandoffPending && runtimeOwnershipJustActivated;
}

}  // namespace ce::dx12_overlay_policy
