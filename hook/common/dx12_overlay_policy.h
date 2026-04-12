#pragma once

#include <windows.h>

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

inline bool ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(bool d3d12DeviceCreated, bool thirdPartyOverlayLoaded) {
    // In no-wrapper builds we normally create a temp D3D12 device/swapchain to
    // install Present hooks eagerly. If a third-party overlay like Steam is
    // already hooked before the game's real D3D12 device exists, that temp
    // swapchain path can recurse through the overlay's startup hook chain and
    // stack overflow before the game reaches its first real swapchain. Defer to
    // the real-swapchain hook path in that specific startup window.
    return !d3d12DeviceCreated && thirdPartyOverlayLoaded;
}

inline bool ShouldUseStartupOverlayCompatibilityMode(bool startupBlockingOverlayLoaded,
                                                     bool actualFrameGenerationActive,
                                                     bool startupCompatSettled = false,
                                                     bool observedAnyFrameGenerationActivity = false,
                                                     bool runtimeOwnsSwapchain = false) {
    // Drive startup compatibility from observed startup-overlay/runtime state,
    // not the executable name. If a startup-blocking overlay is present while
    // real FG is inactive, keep the DX12 overlay on the conservative path until
    // queue ownership and render-module activity settle.
    //
    // Once we have already observed a stable real-game overlay render, later
    // third-party overlay popups (for example Rockstar Social Club appearing
    // mid-session) must not force the DX12 overlay back into its fragile
    // startup-only suppression path.
    //
    // Seeing real FG activity is also a one-way signal that the special startup
    // coexistence phase is over. Even if a late pre-FG runtime-owned handoff
    // cleared the settled latch earlier, later inactive windows after real FG
    // has already appeared are normal coexistence, not startup bootstrap.
    if (!startupBlockingOverlayLoaded || actualFrameGenerationActive || observedAnyFrameGenerationActivity) {
        return false;
    }

    // After a late pre-FG runtime-owned handoff, keep the conservative startup
    // compatibility path active until the live queue topology has fully exited
    // the runtime-owned bootstrap window. Otherwise a single successful draw on
    // that handoff queue immediately drops us back to the normal coexistence path
    // on the very next frame even though startup has not actually finished yet.
    return !startupCompatSettled || runtimeOwnsSwapchain;
}

inline bool ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
    bool startupBlockingOverlayLoaded, bool actualFrameGenerationActive, bool startupCompatSettled,
    bool runtimeOwnsSwapchain, bool observedAnyFrameGenerationActivity, bool runtimeOwnershipJustActivated) {
    if (!startupBlockingOverlayLoaded || actualFrameGenerationActive) {
        return false;
    }

    if (!startupCompatSettled || !runtimeOwnsSwapchain || !runtimeOwnershipJustActivated) {
        return false;
    }

    // GTA/EOS-style startup can briefly look settled on the original game queue,
    // then still hand ownership to a runtime-owned queue before any real FG has
    // been observed. That handoff edge is still startup bootstrap, so let it
    // re-arm the conservative startup path once per newly observed runtime-owned
    // takeover. Once any real FG activity has happened, do not re-enter the
    // startup-only suppression path.
    return !observedAnyFrameGenerationActivity;
}

inline bool ShouldAllowStartupOverlayRendering(bool startupOverlayCompatibilityActive, bool hasSwapchainQueue,
                                                bool runtimeOwnsSwapchain,
                                                ULONGLONG runtimeOwnedSwapchainActiveMs = 0,
                                                ULONGLONG runtimeOwnedSwapchainSettleMs = 0) {
    if (!startupOverlayCompatibilityActive) {
        return true;
    }

    // A captured queue is necessary but not sufficient during startup. If the
    // live swapchain is still runtime-owned, queue ownership is still churning
    // underneath us and pre-SL overlay work can still trip graphics-state
    // validation.
    if (!hasSwapchainQueue) {
        return false;
    }

    if (!runtimeOwnsSwapchain) {
        return true;
    }

    // Some startup overlay stacks temporarily hand the live swapchain to a
    // runtime-owned queue before actual FG activates. Once that runtime-owned
    // queue has stayed stable long enough, continuing to block all overlay work
    // strands the non-FG overlay indefinitely even though the startup handoff is
    // already complete.
    return runtimeOwnedSwapchainSettleMs != 0 && runtimeOwnedSwapchainActiveMs >= runtimeOwnedSwapchainSettleMs;
}

inline bool ShouldDeferStartupOverlayWorkAfterResume(bool startupOverlayCompatibilityActive, bool runtimeOwnsSwapchain,
                                                     ULONGLONG runtimeOwnedSwapchainActiveMs, ULONGLONG settleDelayMs) {
    if (!startupOverlayCompatibilityActive || !runtimeOwnsSwapchain) {
        return false;
    }

    return runtimeOwnedSwapchainActiveMs < settleDelayMs;
}

inline bool ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(bool callerFromThirdPartyOverlay,
                                                               bool hasOriginalGameQueue,
                                                               bool capturedQueueMatchesOriginalGameQueue) {
    if (!callerFromThirdPartyOverlay) {
        return false;
    }

    // A foreign overlay's private DX12 swapchain/queue must not become the
    // authoritative routing path for the game's overlay. If the real game queue
    // is already known, only allow the capture when the foreign caller happens
    // to use that same queue. If the real game queue is not known yet, preserve
    // the current state and wait for a non-overlay caller to establish it.
    return !hasOriginalGameQueue || !capturedQueueMatchesOriginalGameQueue;
}

inline bool ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(bool swapchainKnownThirdPartyOverlay) {
    // Third-party overlays can create their own auxiliary swapchains on the
    // game's HWND. Those Presents are not authoritative game-swapchain traffic
    // and must not perturb live swapchain, queue, or FG tracking.
    return swapchainKnownThirdPartyOverlay;
}

inline bool ShouldKeepStartupBlockingOverlaySwapchainBypass(bool swapchainKnownThirdPartyOverlay,
                                                            bool swapchainTaggedByStartupBlockingOverlay,
                                                            bool presentCallerFromTaggedStartupBlockingOverlay) {
    if (!swapchainKnownThirdPartyOverlay || !swapchainTaggedByStartupBlockingOverlay) {
        return false;
    }

    // EOS/Social Club can wrap the game's authoritative CreateSwapChainForHwnd
    // call, which makes the resulting live swapchain look foreign at creation
    // time. Keep the bypass only while the same startup-blocking overlay still
    // owns the live Present stack; once Present traffic is coming from the game
    // / Streamline instead, this tracked swapchain must be treated as the real
    // game swapchain again so overlay bootstrap can proceed.
    return presentCallerFromTaggedStartupBlockingOverlay;
}

inline bool ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(bool callerFromFFXFGModule,
                                                               bool ffxFrameGenerationInStack) {
    // Native FFX FG swapchain creation can be wrapped by EOS / Social Club, so
    // the immediate return address looks like a third-party overlay even though
    // the live runtime-owned swapchain really belongs to FFX FG. In that case,
    // stack evidence must win so we capture the runtime queue and transition into
    // the guarded FSR path instead of rebuilding on the stale game queue.
    return callerFromFFXFGModule || ffxFrameGenerationInStack;
}

inline bool ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
    bool callerFromFFXFGModule, bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule,
    bool streamlineFrameGenerationInStack) {
    return ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(callerFromFFXFGModule, ffxFrameGenerationInStack) ||
           callerFromStreamlineFGModule || streamlineFrameGenerationInStack;
}

inline bool ShouldTreatSwapchainQueueAsAuthoritativeStreamlineRuntime(bool authoritativeStreamlineRuntimeCreator,
                                                                      bool hasOriginalGameQueue,
                                                                      bool queueMatchesOriginalGameQueue) {
    return authoritativeStreamlineRuntimeCreator && hasOriginalGameQueue && !queueMatchesOriginalGameQueue;
}

inline bool ShouldArmStreamlineStartupTransitionWindowForFreshAuthoritativeRuntimeQueue(
    bool authoritativeStreamlineRuntimeQueue, bool queueMatchesCurrentSwapchainQueue) {
    return authoritativeStreamlineRuntimeQueue && !queueMatchesCurrentSwapchainQueue;
}

inline bool ShouldIgnoreThirdPartyOverlayQueueForGameTracking(bool callerFromThirdPartyOverlay,
                                                              bool hasOriginalGameQueue, bool queueMatchesPrimaryQueue,
                                                              bool queueMatchesOriginalGameQueue,
                                                              bool queueMatchesSwapchainQueue) {
    if (!callerFromThirdPartyOverlay) {
        return false;
    }

    // Third-party overlays can submit ECL work on private helper queues after
    // we have already captured the real game queue. Because all ID3D12CommandQueue
    // instances share the same hooked vtable, those foreign ECLs still reach our
    // detour. Never let that helper queue become authoritative game tracking
    // unless it is already one of the known live game queues.
    if (!hasOriginalGameQueue) {
        return false;
    }

    return !queueMatchesPrimaryQueue && !queueMatchesOriginalGameQueue && !queueMatchesSwapchainQueue;
}

inline bool ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(bool hasOriginalGameQueue,
                                                                    bool queueMatchesOriginalGameQueue,
                                                                    bool authoritativeStreamlineRuntimeQueue) {
    if (!hasOriginalGameQueue || queueMatchesOriginalGameQueue) {
        return true;
    }

    // Native FSR runtime queues stay unhooked to preserve the timing-sensitive
    // path that previously froze inside ffxQuery. Authoritative Streamline
    // runtime handoffs are different: later DLSS activation can migrate the
    // live swapchain onto a new queue/device, and without ECL visibility on that
    // queue CE can get stranded on wrapper-only state.
    return authoritativeStreamlineRuntimeQueue;
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

enum class PostFSRInactiveRecoveryQueueSource {
    kOriginalPresentQueue,
    kCurrentCommandQueueFallback,
};

inline SwapchainOverlayRoutingDecision DecideSwapchainOverlayRouting(
    bool runtimeOwnsSwapchain, bool streamlineFGActive, bool fsrFGActive, bool hadFSRFGPhase, bool hasSwapchainQueue,
    bool hasOriginalGameQueue, bool hasPostSLLastWorkingQueue,
    bool postSLLastWorkingQueueStillActiveDuringRecentTeardown, bool commandQueueMatchesPrimaryGameQueue) {
    if (streamlineFGActive && hadFSRFGPhase) {
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue
                                 : SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue;
    }

    if (streamlineFGActive && hasOriginalGameQueue) {
        return SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue;
    }

    if (!streamlineFGActive && !fsrFGActive && hadFSRFGPhase && !hasSwapchainQueue && hasOriginalGameQueue) {
        // After FSR->DLSS->off, ProcessFrame intentionally leaves g_SwapchainQueue
        // unset until a future clean non-FG swapchain transition can re-establish
        // queue ownership. The preserved PostSL last-working queue is the only
        // queue that already proved it can render the recovered live swapchain,
        // so keep routing through it for the whole recovery window rather than
        // falling back to origGame after a short teardown-activity pulse.
        if (hasPostSLLastWorkingQueue) {
            (void)postSLLastWorkingQueueStillActiveDuringRecentTeardown;
            return SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue;
        }

        // After FSR->DLSS->off, ProcessFrame intentionally leaves g_SwapchainQueue
        // unset until a fresh non-FG Present path can prove the live swapchain
        // queue again. If we no longer have a previously validated PostSL queue,
        // fall back to the non-PostSL recovery path.
        (void)commandQueueMatchesPrimaryGameQueue;
        return SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue;
    }

    if (fsrFGActive) {
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue
                                 : SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue;
    }

    if (runtimeOwnsSwapchain && hadFSRFGPhase && !streamlineFGActive) {
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

inline PostFSRInactiveRecoveryQueueSource DecidePostFSRInactiveRecoveryQueueSource(bool hasOriginalGameQueue) {
    // Even after command ownership settles back to the primary/ECL queue, the
    // recovered non-FG swapchain can still belong to the original Present
    // queue. Talos device-removed on the first recovered offscreen composite
    // when we submitted it on the primary queue instead.
    return hasOriginalGameQueue ? PostFSRInactiveRecoveryQueueSource::kOriginalPresentQueue
                                : PostFSRInactiveRecoveryQueueSource::kCurrentCommandQueueFallback;
}

inline int ResolveTransitionCooldownFrames(int existingCooldownFrames, int requestedCooldownFrames,
                                           bool overrideExistingCooldown) {
    // When the post-FSR DLSS->off path has already settled command ownership
    // back to the game's primary queue, the earlier long FG-on cooldown mostly
    // leaves the last FG-on overlay frame stuck on screen. In that narrow case,
    // replace the stale long cooldown with the shorter post-FSR recovery delay.
    if (overrideExistingCooldown) {
        return requestedCooldownFrames;
    }

    return existingCooldownFrames > requestedCooldownFrames ? existingCooldownFrames : requestedCooldownFrames;
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

inline int ResolveOverlayFGMetricType(bool effectiveFGActive, fg_runtime::RuntimeMode effectiveRuntimeMode) {
    if (!effectiveFGActive) {
        return 0;
    }

    switch (effectiveRuntimeMode) {
        case fg_runtime::RuntimeMode::kDLSSFG:
            return 1;
        case fg_runtime::RuntimeMode::kFSRFG:
            return 2;
        case fg_runtime::RuntimeMode::kNvidiaSmoothMotion:
            return 3;
        case fg_runtime::RuntimeMode::kOff:
        case fg_runtime::RuntimeMode::kStreamlineNoFG:
        case fg_runtime::RuntimeMode::kUnknown:
        default:
            return 0;
    }
}

inline bool IsPostFSRNonFGRecovery(bool hadFSRFGPhase, bool needsOffscreenOverlayAfterPostFSRNonFG, bool actualFGActive,
                                   bool streamlineFGRunning, bool hasSwapchainQueue) {
    return hadFSRFGPhase && needsOffscreenOverlayAfterPostFSRNonFG && !actualFGActive && !streamlineFGRunning &&
           !hasSwapchainQueue;
}

inline bool ShouldReserveInactiveFGOverlaySpace(bool postFSRNonFGRecovery) {
    return postFSRNonFGRecovery;
}

inline bool ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(bool postFSRNonFGRecovery,
                                                                           bool recentStreamlineTeardown,
                                                                           bool postSLRecentTeardownActivity) {
    // Keep the FG rows' background area reserved only while the recovered non-FG
    // overlay is still compositing over teardown-era backbuffer content. Once the
    // immediate teardown traffic settles, continuing to reserve those rows leaves
    // visible empty gaps after FSR->DLSS->off even though the live overlay text is
    // already correct.
    // The coarse Streamline-off heuristic grace is intentionally much longer and
    // exists to suppress stale queue/heuristic state. Treating that whole grace
    // window as a layout reservation keeps two blank FG rows visible long after
    // the live overlay has already returned to its smaller non-FG shape.
    (void)recentStreamlineTeardown;
    return postFSRNonFGRecovery && postSLRecentTeardownActivity;
}

inline bool ShouldResetQueueChangeHeuristicAfterCleanNonFGSwapchainChange(bool endingPostFSRNonFGRecovery) {
    // Once post-FSR non-FG recovery reaches a clean swapchain transition, the
    // queue-change heuristic's old initial/current anchors belong to the
    // departed recovery topology. Force a recapture so the new stable non-FG
    // menu/present queue cannot be mistaken for fresh FSR FG.
    return endingPostFSRNonFGRecovery;
}

inline bool ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(bool endingPostFSRNonFGRecovery,
                                                                       bool hasSwapchainQueue,
                                                                       bool hasOriginalGameQueue,
                                                                       bool swapchainQueueMatchesOriginalGameQueue) {
    // A fresh swapchain recreation captured on the original Present queue is the
    // strongest non-heuristic signal we have that ownership has returned to the
    // normal non-FG topology. End the post-FSR recovery immediately in that case
    // instead of waiting for later cleanup paths to notice indirectly.
    return endingPostFSRNonFGRecovery && hasSwapchainQueue && hasOriginalGameQueue &&
           swapchainQueueMatchesOriginalGameQueue;
}

inline bool ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
    bool postFSRNonFGRecovery, bool recentStreamlineTeardown,
    bool postSLLastWorkingQueueStillActiveDuringRecentTeardown) {
    // After FSR->DLSS->off, the preserved PostSL queue can keep surfacing
    // teardown-era ECL traffic after the coarse SL-off grace has expired.
    // Treat that window as unsafe for heuristic FSR reactivation or the overlay
    // gets stranded on the "FSR active but scQueue=null" skip path.
    return postFSRNonFGRecovery && (recentStreamlineTeardown || postSLLastWorkingQueueStillActiveDuringRecentTeardown);
}

inline bool ShouldSkipProcessFrameForZeroECLPresent(bool isInterpolatedFrame, bool hasDedicatedQueue,
                                                    bool heuristicFSRFG, bool runtimeOwnsSwapchain,
                                                    bool streamlineFGRunning, bool recentStreamlineTeardown,
                                                    bool postFSRNonFGRecovery,
                                                    ce::fg_runtime::RuntimeMode runtimeMode) {
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

    // After FSR turns off, Talos can resume non-FG rendering while the live
    // Present path is still being recovered and g_SwapchainQueue is
    // intentionally left null. Top-level Presents in that window still need to
    // drive ProcessFrame even if authoritative ECLs have not yet landed on the
    // currently trusted queue.
    if (postFSRNonFGRecovery) {
        return false;
    }

    // Final Streamline teardown after an FSR->DLSS handoff can also briefly
    // stop delivering authoritative ECLs on the trusted classification queue
    // while top-level Presents continue on the live swapchain. Keep ProcessFrame
    // running through that grace window so the non-FG overlay path can recover.
    if (recentStreamlineTeardown && !streamlineFGRunning) {
        return false;
    }

    // In some Steam + Streamline-no-FG startup paths, top-level Presents are
    // already stable but authoritative ECL registration never reaches the
    // trusted queue. If we keep treating those Presents as interpolated-only
    // zero-ECL noise, ProcessFrame never bootstraps the non-FG overlay.
    if (runtimeMode == ce::fg_runtime::RuntimeMode::kStreamlineNoFG && !streamlineFGRunning) {
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

inline bool ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(bool actualFGActive, bool fsrFGActive,
                                                                             bool streamlineFGRunning,
                                                                             bool runtimeOwnsSwapchain) {
    if (!actualFGActive || streamlineFGRunning) {
        return false;
    }

    // Native/runtime-owned FG swapchains are already routed through the live
    // swapchain queue. Spinning up a second overlay queue in that window
    // reintroduces cross-queue synchronization against the runtime-owned path
    // and has been observed to stall inside native FFX frame-generation work.
    return fsrFGActive || runtimeOwnsSwapchain;
}

inline bool ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(bool runtimeOwnsSwapchain,
                                                                           bool streamlineFGRunning,
                                                                           fg_runtime::RuntimeMode runtimeMode,
                                                                           bool authoritativeFSRActive) {
    if (!runtimeOwnsSwapchain || streamlineFGRunning) {
        return false;
    }

    // Native/runtime-owned FSR is stricter than the generic runtime-owned
    // non-FSR windows. Once FFX owns the live swapchain queue, any injected
    // DX12 GPU work we submit on that queue can deadlock inside the native FFX
    // runtime (observed in amd_fidelityfx_dx12!ffxQuery after takeover).
    // Suppress the injected overlay GPU path until a non-FSR topology returns.
    //
    // Keep the generic runtime-owned non-FSR path alive though. DLSS /
    // Streamline suspension windows can keep the swapchain runtime-owned
    // without native FSR ownership, and those windows still need normal
    // ProcessFrame recovery instead of blanket overlay suppression.
    return authoritativeFSRActive || fg_runtime::RuntimeModeUsesFSR(runtimeMode);
}

inline bool ShouldTrackAuthoritativeFSRRealFrameOnlyRun(bool streamlineFGRunning, bool runtimeOwnsSwapchain,
                                                        bool authoritativeFSRActive, bool isInterpolatedFrame,
                                                        bool recentStreamlineTeardown) {
    return !streamlineFGRunning && runtimeOwnsSwapchain && authoritativeFSRActive && !isInterpolatedFrame &&
           !recentStreamlineTeardown;
}

inline bool ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(int realFrameOnlyRunLength,
                                                             bool hasDirectFFXApiConfirmation) {
    // Native FSR can leave a runtime-owned swapchain alive after FG is turned
    // off. If we keep seeing only real frames for an extended run, the
    // authoritative "FSR FG active" latch is stale and should fall back to the
    // generic runtime-owned non-FG path.
    // Once we have observed direct FFX API traffic for the current activation,
    // keep trusting that authoritative signal. GTA V Enhanced can continue to
    // present only real top-level frames while native FSR FG stays live on its
    // runtime-owned swapchain and worker threads.
    if (hasDirectFFXApiConfirmation) {
        return false;
    }

    return realFrameOnlyRunLength >= 120;
}

inline bool ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(bool authoritativeFSRActive,
                                                                   bool runtimeTargetIsNone, int fgTransitionCooldown) {
    // Talos can briefly suspend native FSR FG during startup/menu transitions
    // while the runtime-owned swapchain and queue topology are still settling.
    // Treating that transient None edge as a real teardown immediately clears
    // authoritative FSR and collapses the overlay path before the runtime turns
    // FSR back on a few frames later.
    return authoritativeFSRActive && runtimeTargetIsNone && fgTransitionCooldown > 0;
}

inline bool ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(bool canUseFSRHeuristics, bool runtimeOwnsSwapchain,
                                                                    bool hadFSRFGPhase) {
    return !canUseFSRHeuristics && runtimeOwnsSwapchain && hadFSRFGPhase;
}

inline bool ShouldPreserveRuntimeOwnedFSRTeardown(bool targetIsNone, bool hadFSRFGPhase, bool runtimeOwnsSwapchain,
                                                  bool streamlineFGRunning) {
    return targetIsNone && hadFSRFGPhase && runtimeOwnsSwapchain && !streamlineFGRunning;
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

    // After FSR tears down, Talos can resume presenting real non-FG frames while
    // authoritative ECL traffic settles on the primary DIRECT queue instead of
    // the original Present queue. If frame classification stays pinned to
    // origGame in that window, top-level Presents flip back to "zero ECL" and
    // ProcessFrame stops driving the recovered overlay even though rendering
    // itself is still healthy.
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

inline bool ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(bool active, bool hadRecentHeuristicGrace,
                                                                      bool hadRecentSwapchainReinitGrace) {
    return active && (hadRecentHeuristicGrace || hadRecentSwapchainReinitGrace);
}

inline bool ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
    bool actualFGActive, bool streamlineFGRunning, bool recentStreamlineTeardown, bool hasSwapchainQueue,
    bool hasOriginalGameQueue, bool hasPostSLLastWorkingQueue, bool hasCommandQueue,
    bool commandQueueMatchesSwapchainQueue, bool commandQueueMatchesOriginalGameQueue,
    bool commandQueueMatchesPrimaryGameQueue) {
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
    bool recentStreamlineTeardown, bool postFSRNonFGRecovery,
    bool postSLLastWorkingQueueStillActiveDuringRecentTeardown, bool queueMatchesPrimaryQueue,
    bool queueMatchesOriginalGameQueue, bool queueMatchesSwapchainQueue, bool queueMatchesPostSLLastWorkingQueue) {
    // During final Streamline teardown after a post-FSR DLSS phase, helper ECLs
    // can keep arriving on a departed wrapper queue for a short time even though
    // Present has already returned to the non-FG swapchain queue. Do not let
    // those teardown queues repollute g_CommandQueue.
    // The preserved PostSL last-working queue is also teardown-era state in this
    // window. It stays valuable for immediate non-FG overlay recovery, but it
    // must not be re-registered as the live game command queue or the queue-change
    // heuristic will briefly re-detect FSR FG when menus reopen.
    //
    // The preserved PostSL last-working queue can keep surfacing teardown-era
    // ECL traffic for a short time after the coarse Streamline-off grace
    // counter reaches zero. Keep ignoring that exact queue while it is still
    // actively resurfacing so it cannot repollute command tracking right before
    // the non-FG overlay resumes.
    //
    // During the longer post-FSR non-FG recovery window, the overlay itself can
    // continue submitting on that preserved queue because it is the only queue
    // that already proved safe for the recovered swapchain. Those recovery
    // submits must not repollute g_CommandQueue, or routing immediately falls
    // off the only positive-proof queue and back onto unsafe best-guess queues.
    if (queueMatchesPostSLLastWorkingQueue && !queueMatchesPrimaryQueue && !queueMatchesOriginalGameQueue &&
        !queueMatchesSwapchainQueue) {
        return recentStreamlineTeardown || postSLLastWorkingQueueStillActiveDuringRecentTeardown ||
               postFSRNonFGRecovery;
    }

    if (!recentStreamlineTeardown) {
        return false;
    }

    return !queueMatchesPrimaryQueue && !queueMatchesOriginalGameQueue && !queueMatchesSwapchainQueue;
}

inline bool ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
    bool recentStreamlineTeardown, bool postFSRNonFGRecovery,
    bool postSLLastWorkingQueueStillActiveDuringRecentTeardown, bool queueMatchesPrimaryQueue,
    bool queueMatchesOriginalGameQueue, bool queueMatchesSwapchainQueue, bool queueMatchesPostSLLastWorkingQueue) {
    // The preserved PostSL queue can still resurface as teardown traffic after
    // DLSS FG turns off. Treat it like a departed runtime queue for heuristic
    // purposes so a late menu transition cannot blip back into heuristic FSR FG.
    // Keep honoring that rule while that preserved queue is still actively
    // resurfacing as teardown traffic, not just while the coarse Streamline-off
    // grace counter is still positive.
    //
    // While the post-FSR non-FG recovery path is still active, keep ignoring
    // that preserved queue for heuristic purposes as well. The recovered overlay
    // can legitimately keep using it long after the short teardown pulse ends,
    // and treating those submits as fresh queue-change evidence would falsely
    // re-detect FSR FG.
    if (queueMatchesPostSLLastWorkingQueue && !queueMatchesPrimaryQueue && !queueMatchesOriginalGameQueue &&
        !queueMatchesSwapchainQueue) {
        return recentStreamlineTeardown || postSLLastWorkingQueueStillActiveDuringRecentTeardown ||
               postFSRNonFGRecovery;
    }

    if (!recentStreamlineTeardown) {
        return false;
    }

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

inline bool ShouldTreatPostSLSelectedQueueAsWrapper(bool queueMatchesOriginalGameQueue, bool queueMatchesDedicatedQueue,
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

inline bool ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(bool streamlineFGActive, bool hasSLWrapperQueue,
                                                                  bool hasRealQueueBehindWrapper, bool hasRealD3D12ECL,
                                                                  bool selectedQueueIsSwapchainQueue,
                                                                  bool selectedQueueOrigECLMatchesRealECL) {
    if (streamlineFGActive && !hasSLWrapperQueue && !hasRealQueueBehindWrapper && hasRealD3D12ECL &&
        selectedQueueIsSwapchainQueue && selectedQueueOrigECLMatchesRealECL) {
        return true;
    }

    return !streamlineFGActive && !hasSLWrapperQueue && !hasRealQueueBehindWrapper && !hasRealD3D12ECL;
}

inline bool ShouldRefreshRecentPostSLTeardownActivity(bool recentStreamlineTeardown, bool queueMatchesPostSLLastWorking,
                                                      bool streamlineFGRunning, bool postSLActive) {
    return recentStreamlineTeardown && queueMatchesPostSLLastWorking && (streamlineFGRunning || postSLActive);
}

inline bool ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(bool commandQueueSettledToPrimary, bool hadFSRFGPhase) {
    return commandQueueSettledToPrimary && hadFSRFGPhase;
}

inline bool ShouldUseShortPostFSRInactiveCooldown(bool commandQueueSettledToPrimary, bool hadFSRFGPhase,
                                                  bool recentStreamlineTeardown) {
    // Once command ownership has already returned to the primary game queue,
    // the long swapchain-change cooldown mainly leaves the last FG-on overlay
    // frame stuck on screen. The dedicated recent-teardown gates still block the
    // truly unsafe tail of Streamline teardown, so a shorter cooldown is enough
    // to let the recovered non-FG overlay redraw promptly.
    return commandQueueSettledToPrimary && hadFSRFGPhase && recentStreamlineTeardown;
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
    // Intentionally retired: the current post-FSR path keeps queue ownership on
    // the selected live queue instead of pinning the legacy wrapper queue.
    (void)hadFSRFGPhase;
    (void)usePostSLOffscreenComposite;
    (void)selectedQueueIsSwapchainQueue;
    (void)hasPinnedWrapperQueue;
    (void)hasCapturedSLWrapperQueue;
    (void)preferSelectedSwapchainQueueSubmitAfterFSR;
    return false;
}

inline bool ShouldUsePostSLWrapperSubmitAfterFSR(bool hadFSRFGPhase, bool usePostSLOffscreenComposite,
                                                 bool selectedQueueIsSwapchainQueue, bool hasSLWrapperQueue,
                                                 bool preferSelectedSwapchainQueueSubmitAfterFSR) {
    // Intentionally retired: post-FSR submissions should use the selected queue
    // or validated direct path, not route back through the legacy wrapper.
    (void)hadFSRFGPhase;
    (void)usePostSLOffscreenComposite;
    (void)selectedQueueIsSwapchainQueue;
    (void)hasSLWrapperQueue;
    (void)preferSelectedSwapchainQueueSubmitAfterFSR;
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
    (void)hadFSRFGPhase;
    (void)streamlineFGActive;
    (void)selectedQueueIsSwapchainQueue;
    (void)queueIsSLWrapper;
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

inline bool ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(bool hadFSRFGPhase) {
    // Post-FSR recovery still benefits from letting Streamline's recovered queue
    // path stabilize across multiple runtime callbacks. Pure DLSS startup does
    // not: some runtimes only emit a very short synthetic Present burst before
    // switching to their live Present path, and counting down by callback can
    // strand PostSL permanently inactive.
    return hadFSRFGPhase;
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
