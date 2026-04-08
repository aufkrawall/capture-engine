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
    kUsePostFSRInactiveLastWorkingQueue,
    kUsePostFSRInactiveOriginalQueue,
    kUseFSRSwapchainQueue,
    kUseRuntimeOwnedSwapchainQueue,
    kSkipRuntimeOwnedSwapchainWithoutQueue,
    kSkipFSRWithoutSwapchainQueue,
};

inline SwapchainOverlayRoutingDecision DecideSwapchainOverlayRouting(bool runtimeOwnsSwapchain, bool streamlineFGActive,
                                                                     bool fsrFGActive, bool hadFSRFGPhase,
                                                                     bool hasSwapchainQueue, bool hasOriginalGameQueue,
                                                                     bool hasPostSLLastWorkingQueue) {
    if (streamlineFGActive && hadFSRFGPhase) {
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue
                                 : SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue;
    }

    if (streamlineFGActive && hasOriginalGameQueue) {
        return SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue;
    }

    if (!streamlineFGActive && !fsrFGActive && hadFSRFGPhase && !hasSwapchainQueue && hasOriginalGameQueue) {
        if (hasPostSLLastWorkingQueue) {
            return SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue;
        }

        // After FSR->DLSS->off, ProcessFrame intentionally leaves g_SwapchainQueue
        // unset until a fresh non-FG Present path can prove the live swapchain
        // queue again. While that recapture window is open, falling through to the
        // last observed command queue can pick a multi-queue game's render queue
        // instead of its Present queue, which reproduced Talos DEVICE_REMOVED on
        // the first recovered non-FG overlay submit.
        return SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue;
    }

    if (fsrFGActive) {
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue
                                 : SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue;
    }

    if (runtimeOwnsSwapchain) {
        // A runtime-owned swapchain does not by itself prove FSR FG. GTA 5
        // Enhanced can temporarily keep DLSS/Streamline's swapchain on a
        // non-game queue while FG suspends on loading screens. Treating that
        // generic runtime-owned window as FSR forces the Talos-only post-FSR
        // recovery path and strands PostSL when DLSS resumes.
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUseRuntimeOwnedSwapchainQueue
                                 : SwapchainOverlayRoutingDecision::kSkipRuntimeOwnedSwapchainWithoutQueue;
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

inline bool ShouldSkipProcessFrameForZeroECLPresent(bool isInterpolatedFrame, bool hasDedicatedQueue,
                                                    bool heuristicFSRFG, bool runtimeOwnsSwapchain,
                                                    bool streamlineFGRunning, bool recentStreamlineTeardown) {
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

    // Final Streamline teardown after an FSR->DLSS handoff can also briefly
    // stop delivering authoritative ECLs on the trusted classification queue
    // while top-level Presents continue on the live swapchain. Keep ProcessFrame
    // running through that grace window so the non-FG overlay path can recover.
    if (recentStreamlineTeardown && !streamlineFGRunning) {
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

inline bool ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
    bool recoveringPostFSRNonFG, bool actualFGActive, bool streamlineFGRunning, bool hasSwapchainQueue,
    bool hasOriginalGameQueue, bool hasPrimaryGameQueue, bool originalQueueMatchesPrimaryQueue) {
    if (!recoveringPostFSRNonFG || actualFGActive || streamlineFGRunning || hasSwapchainQueue) {
        return false;
    }

    if (!hasOriginalGameQueue || !hasPrimaryGameQueue) {
        return false;
    }

    // After FSR->DLSS->off, Talos can resume presenting real non-FG frames while
    // authoritative ECL traffic settles on the primary DIRECT queue instead of the
    // original Present queue. If frame classification stays pinned to origGame in
    // that window, top-level Presents flip back to "zero ECL" as soon as the
    // recent-Streamline-teardown grace expires, and ProcessFrame stops driving the
    // recovered overlay even though rendering itself is still healthy.
    return !originalQueueMatchesPrimaryQueue;
}

inline bool ShouldGuardSwapchainReinitAfterChange(bool fgCurrentlyActive, bool fgRecentlyWasActive,
                                                  bool hasFGTransitionCooldown, bool recentStreamlineTeardown,
                                                  bool runtimeOwnsSwapchain, bool hasSwapchainQueue,
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

inline bool ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(bool actualFGActive, bool streamlineFGRunning,
                                                                bool runtimeOwnsSwapchain, bool hasSwapchainQueue,
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

inline bool ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
    bool actualFGActive, bool streamlineFGRunning, bool recentStreamlineTeardown, bool hasSwapchainQueue,
    bool hasOriginalGameQueue, bool hasPostSLLastWorkingQueue, bool hasCommandQueue,
    bool commandQueueMatchesSwapchainQueue,
    bool commandQueueMatchesOriginalGameQueue, bool commandQueueMatchesPrimaryGameQueue) {
    if (actualFGActive || streamlineFGRunning) {
        return false;
    }

    if (!recentStreamlineTeardown || !hasOriginalGameQueue) {
        return false;
    }

    if (!hasSwapchainQueue) {
        // After the post-FSR DLSS teardown path we intentionally leave
        // g_SwapchainQueue unset until command traffic proves which non-wrapper
        // queue is actually live again. Both origGame and the primary ECL queue
        // reproduced Talos DEVICE_REMOVED on the first resumed non-FG overlay
        // submit. The only safe immediate recovery path is the last PostSL queue
        // that already proved it could render the live swapchain successfully.
        return !hasPostSLLastWorkingQueue;
    }

    // After the post-FSR DLSS path turns FG off, late Streamline teardown ECLs
    // can keep repopulating g_CommandQueue with a departed wrapper queue even
    // after the live non-FG swapchain queue has been restored. Rebuilding the
    // pre-SL overlay path before command tracking settles back onto the live
    // queue reproduces Talos DEVICE_REMOVED on the first non-FG submit.
    //
    // However, multi-queue games (e.g. Talos Principle) use separate DIRECT
    // queues for ECL (render) and Present (swapchain).  The primary game queue
    // is the first DIRECT queue observed and is always a valid non-wrapper queue.
    // Treating it as "unsettled" defers overlay reinit for the entire grace
    // period, which itself causes DEVICE_REMOVED from stale GPU state.
    if (commandQueueMatchesPrimaryGameQueue) {
        return false;
    }

    return !hasCommandQueue || (!commandQueueMatchesSwapchainQueue && !commandQueueMatchesOriginalGameQueue);
}

inline bool ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
    bool recentStreamlineTeardown, bool queueMatchesPrimaryQueue, bool queueMatchesOriginalGameQueue,
    bool queueMatchesSwapchainQueue) {
    if (!recentStreamlineTeardown) {
        return false;
    }

    // During final Streamline teardown after a post-FSR DLSS phase, helper ECLs
    // can keep arriving on a departed wrapper queue for a short time even though
    // Present has already returned to the non-FG swapchain queue. Do not let
    // those teardown queues repollute g_CommandQueue.
    return !queueMatchesPrimaryQueue && !queueMatchesOriginalGameQueue && !queueMatchesSwapchainQueue;
}

inline bool ShouldRealignInactiveCommandQueueToSwapchainQueue(bool actualFGActive, bool streamlineFGRunning,
                                                              bool hasSwapchainQueue, bool hasOriginalGameQueue,
                                                              bool hasCommandQueue,
                                                              bool commandQueueMatchesSwapchainQueue,
                                                              bool commandQueueMatchesOriginalGameQueue,
                                                              bool commandQueueMatchesPrimaryGameQueue) {
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
    //
    // However, multi-queue games use separate DIRECT queues for ECL and Present.
    // The primary game queue (first DIRECT queue seen) is always valid and must
    // NOT be realigned away from, or the ECL ignore logic loses its anchor and
    // the game's legitimate ECL queue gets misclassified as departed.
    if (commandQueueMatchesPrimaryGameQueue) {
        return false;
    }

    return !commandQueueMatchesSwapchainQueue && !commandQueueMatchesOriginalGameQueue;
}
inline bool ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(bool hadFSRFGPhase, bool overlayInit,
                                                                          bool syncInit) {
    // The direct Streamline teardown path can invalidate overlay state before
    // ProcessFrame reaches its own SL transition tracking block. In that case,
    // ProcessFrame misses the OFF edge and would otherwise rebuild immediately
    // on the same teardown Present.
    return hadFSRFGPhase && (!overlayInit || !syncInit);
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

inline bool ShouldTreatPostSLSelectedQueueAsWrapper(bool queueMatchesOriginalGameQueue,
                                                    bool queueMatchesDedicatedQueue,
                                                    bool queueMatchesSwapchainQueue,
                                                    bool selectedQueueOrigECLMatchesRealECL) {
    if (queueMatchesOriginalGameQueue || queueMatchesDedicatedQueue || queueMatchesSwapchainQueue) {
        return false;
    }

    // A queue whose captured "original" ExecuteCommandLists already matches the
    // real D3D12 entrypoint has effectively proven it is a direct submission path,
    // not just a transient Streamline wrapper. Remembering that queue is what
    // lets the post-FSR FG-off recovery stay on the last queue that actually
    // worked for the live swapchain.
    return !selectedQueueOrigECLMatchesRealECL;
}

inline bool ShouldPreservePostSLLastWorkingQueueForPostFSROffRecovery(bool hadFSRFGPhase, bool previousWasFG,
                                                                      bool targetIsFGOff) {
    return hadFSRFGPhase && previousWasFG && targetIsFGOff;
}

inline bool ShouldSyntheticPostSLAdvanceDormantStartup(bool startupActivationPending, bool streamlineFGRunning,
                                                       bool postSLActive, bool processFrameRecentlySeen) {
    return startupActivationPending && streamlineFGRunning && !postSLActive && !processFrameRecentlySeen;
}

inline bool ShouldBootstrapPostSLOverlayState(bool streamlineFGRunning, bool postSLActive, bool overlayInit,
                                              bool processFrameRecentlySeen) {
    return streamlineFGRunning && postSLActive && !overlayInit && !processFrameRecentlySeen;
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

inline bool ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(bool hadFSRFGPhase, bool selectedQueueIsSwapchainQueue,
                                                                bool queueIsSLWrapper, bool hasSelectedQueueSubmitPath,
                                                                bool hasWrapperDerivedDirectPath) {
    return hadFSRFGPhase && selectedQueueIsSwapchainQueue && !queueIsSLWrapper && hasSelectedQueueSubmitPath &&
           !hasWrapperDerivedDirectPath;
}

inline bool ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(bool hadFSRFGPhase, bool selectedQueueIsSwapchainQueue,
                                                             bool hasSelectedQueueOrigECL,
                                                             bool selectedQueueOrigECLMatchesRealECL,
                                                             bool hasDirectQueueBehindWrapper) {
    return hadFSRFGPhase && !selectedQueueIsSwapchainQueue && !hasDirectQueueBehindWrapper && hasSelectedQueueOrigECL &&
           selectedQueueOrigECLMatchesRealECL;
}

inline bool ShouldUsePostSLRealQueueBehindWrapperAfterFSR(bool hadFSRFGPhase, bool streamlineFGActive,
                                                          bool hasDirectQueueBehindWrapper) {
    return hadFSRFGPhase && streamlineFGActive && hasDirectQueueBehindWrapper;
}

inline bool ShouldPreferValidatedDirectQueueForPostFSRLock(bool hadFSRFGPhase, bool streamlineFGActive,
                                                           bool hasDirectQueueBehindWrapper) {
    return hadFSRFGPhase && streamlineFGActive && hasDirectQueueBehindWrapper;
}

inline bool IsUsableValidatedPostSLDirectQueueCandidate(bool queueLooksDirect, bool matchesCapturedSLWrapperQueue,
                                                        bool matchesCurrentCommandQueue, bool matchesOriginalGameQueue,
                                                        bool matchesSwapchainQueue) {
    return queueLooksDirect && !matchesCapturedSLWrapperQueue && !matchesCurrentCommandQueue &&
           !matchesOriginalGameQueue && !matchesSwapchainQueue;
}

inline bool ShouldUsePostSLWrapperBootstrapQueueAfterFSR(bool hadFSRFGPhase, bool streamlineFGActive,
                                                         bool hasDirectQueueBehindWrapper, bool hasSLWrapperQueue) {
    if (!hadFSRFGPhase || !streamlineFGActive) {
        return false;
    }
    if (hasDirectQueueBehindWrapper) {
        return false;
    }
    return hasSLWrapperQueue;
}

inline bool ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(bool hadFSRFGPhase, bool streamlineFGActive,
                                                                   bool hasDirectQueueBehindWrapper,
                                                                   bool commandQueueIsWrapper) {
    return hadFSRFGPhase && streamlineFGActive && !hasDirectQueueBehindWrapper && commandQueueIsWrapper;
}

inline bool ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(bool hadFSRFGPhase, bool streamlineFGActive,
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

inline bool ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
    bool hasLockedQueue, bool hadFSRFGPhase, bool streamlineFGActive, bool lockedQueueIsSLWrapper,
    bool hasSwapchainQueue, bool swapchainQueueDiffersFromOriginalGameQueue, bool hasSwapchainQueueSubmitPath,
    bool hasWrapperDerivedDirectPath) {
    return hasLockedQueue && hadFSRFGPhase && streamlineFGActive && lockedQueueIsSLWrapper && hasSwapchainQueue &&
           swapchainQueueDiffersFromOriginalGameQueue && hasSwapchainQueueSubmitPath && !hasWrapperDerivedDirectPath;
}

inline bool ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
    bool hadFSRFGPhase, bool streamlineFGActive, int postFSRProbeLevel, bool hasDirectQueueBehindWrapper,
    bool hasSLWrapperQueue, bool hasSelectedQueueSubmitPath, bool selectedQueueIsSLWrapper) {
    if (!hadFSRFGPhase || !streamlineFGActive || postFSRProbeLevel != 0) {
        return false;
    }
    if (hasDirectQueueBehindWrapper) {
        return false;
    }
    if (!hasSLWrapperQueue) {
        return false;
    }
    return !hasSelectedQueueSubmitPath || selectedQueueIsSLWrapper;
}

inline bool ShouldUseWrapperQueueForPostFSRProbeFallback(bool hadFSRFGPhase, int postFSRProbeLevel,
                                                         bool hasSLWrapperQueue,
                                                         bool preferSelectedSwapchainQueueSubmitAfterFSR,
                                                         bool hasValidatedDirectQueueBehindWrapper) {
    return !hadFSRFGPhase && postFSRProbeLevel >= 1 && hasSLWrapperQueue &&
           !preferSelectedSwapchainQueueSubmitAfterFSR && hasValidatedDirectQueueBehindWrapper;
}

inline bool ShouldPinPostSLWrapperQueueAfterFSR(bool hadFSRFGPhase, bool usePostSLOffscreenComposite,
                                                bool selectedQueueIsSwapchainQueue, bool hasPinnedWrapperQueue,
                                                bool hasCapturedSLWrapperQueue,
                                                bool preferSelectedSwapchainQueueSubmitAfterFSR) {
    return false;
}

inline bool ShouldUsePostSLWrapperSubmitAfterFSR(bool hadFSRFGPhase, bool usePostSLOffscreenComposite,
                                                 bool selectedQueueIsSwapchainQueue, bool hasSLWrapperQueue,
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

inline bool ShouldUsePostSLOffscreenCompositeAfterFSR(bool hadFSRFGPhase, bool streamlineFGActive,
                                                      bool selectedQueueIsSwapchainQueue, bool queueIsSLWrapper) {
    // The direct post-FSR swapchain-queue path already proved that queue can
    // handle explicit PRESENT<->RT traffic. Reintroducing copy-render-copy on
    // that same path only adds the copy operations that previously hung Talos.
    return false;
}

inline bool ShouldUseExplicitBackbufferCopyTransitionsForPostFSROffscreenComposite(
    bool usePostSLOffscreenComposite, bool useExplicitPostFSRSwapchainTransitions) {
    return usePostSLOffscreenComposite && useExplicitPostFSRSwapchainTransitions;
}

inline bool ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(bool hadFSRFGPhase, int postFSRProbeLevel,
                                                          bool usePostSLOffscreenComposite,
                                                          bool selectedQueueIsSwapchainQueue) {
    return hadFSRFGPhase && usePostSLOffscreenComposite && postFSRProbeLevel == 2 && !selectedQueueIsSwapchainQueue;
}

inline bool ShouldSyntheticPostSLRefreshMetrics(bool streamlineFGRunning, bool processFrameRecentlySeen) {
    return streamlineFGRunning && !processFrameRecentlySeen;
}

inline bool ShouldPreserveConfirmedPostSLDuringFGCooldown(bool streamlineFGRunning, bool postSLConfirmedRendering) {
    return streamlineFGRunning && postSLConfirmedRendering;
}

inline bool ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(bool streamlineFGRunning, bool postSLActive,
                                                              bool postSLConfirmedRendering,
                                                              bool startupActivationPending) {
    return !streamlineFGRunning && !postSLActive && !postSLConfirmedRendering && !startupActivationPending;
}

inline bool ShouldForceEndStreamlineOwnershipForSwapchainTakeover(bool runtimeOwnsSwapchain, bool callerFromFFXFGModule,
                                                                  bool streamlineFGRunning,
                                                                  bool streamlineStartupHandoffPending,
                                                                  bool runtimeOwnershipJustActivated) {
    if (!runtimeOwnsSwapchain) {
        return false;
    }

    // Only authoritative FFX FG traffic should tear down Streamline/PostSL
    // ownership. A fresh runtime-owned swapchain queue by itself is not enough:
    // GTA V Enhanced can bounce CreateSwapChainForHwnd through our own detours
    // during DLSS startup, and treating that as an FFX takeover clears the
    // active PostSL path even though no real FFX FG module is involved.
    (void)streamlineFGRunning;
    (void)streamlineStartupHandoffPending;
    (void)runtimeOwnershipJustActivated;
    return callerFromFFXFGModule;
}

inline bool ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(bool streamlineModuleLoaded,
                                                                      bool streamlineFGRunning,
                                                                      bool streamlineStartupHandoffPending,
                                                                      bool callerFromFFXFGModule,
                                                                      bool ffxFrameGenerationInStack) {
    if (!streamlineModuleLoaded) {
        return false;
    }

    if (callerFromFFXFGModule || ffxFrameGenerationInStack) {
        return false;
    }

    return streamlineFGRunning || streamlineStartupHandoffPending;
}

}  // namespace ce::dx12_overlay_policy
