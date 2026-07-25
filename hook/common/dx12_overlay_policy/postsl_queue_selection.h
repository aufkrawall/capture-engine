#pragma once

#include <windows.h>

#include <cstdint>

#include <dxgi1_6.h>

#include "../dxgi_presentation_color.h"
#include "../fg_runtime_state.h"

struct ID3D12CommandQueue;
struct ID3D12Fence;

#include "streamline_ownership.h"

// Post-SL queue ownership, wrapper detection, and overlay state bootstrap.

namespace ce::dx12_overlay_policy {

inline bool ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(bool hadFSRFGPhase, bool hasSwapchainQueue,
                                                                       bool swapchainQueueDiffersFromOriginalGameQueue,
                                                                       bool streamlineStartupHandoffPending,
                                                                       bool warmPostSLResumeFromKeepAlive = false) {
    // After an FSR phase, a non-origGame swapchain queue often does need to be
    // cleared before DLSS/Streamline PostSL re-establishes its own topology.
    // But if the queue was just re-captured as the fresh authoritative
    // Streamline runtime handoff for the new DLSS activation, clearing it
    // immediately destroys the very proof the handoff logic needs and forces the
    // riskier post-FSR wrapper-bootstrap path.
    //
    // A WARM PostSL resume (make-before-break keep-alive: a DLSS-FG suspend ->
    // resume where PostSL stayed confirmed-and-rendering the whole time) is NOT an
    // FSR->DLSS transition: the non-origGame swapchain queue is the LIVE DLSS-G
    // proxy queue PostSL has been submitting on (proven by the confirmed-rendering
    // keep-alive), and the proxy persists across the suspend. Clearing it strands
    // the warm resume — it skips the synthetic-startup re-arm, so with scQueue=null
    // and FSR history PostSL refuses the wrapper bootstrap and drops every frame
    // forever (session 20260613_151646). Preserve the queue so PostSL resumes on
    // the proven scQueue path. The cold FSR->DLSS transition (no warm resume) still
    // clears, preventing the documented DEVICE_REMOVED.
    return hadFSRFGPhase && hasSwapchainQueue && swapchainQueueDiffersFromOriginalGameQueue &&
           !streamlineStartupHandoffPending && !warmPostSLResumeFromKeepAlive;
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
                                                       bool postSLActive, bool processFrameRecentlySeen,
                                                       bool useTopLevelHandoffWrapperProgress,
                                                       bool sameQueuePureDLSSColdStartSafe = false) {
    // A proven same-queue Streamline callback is the handoff event: PostSL can
    // take over immediately without waiting for the last ProcessFrame timestamp
    // to age out. The caller preserves make-before-break when the normal route
    // already drew in this present. Separate-queue startup retains the dormant
    // proof because its early ECL is the documented GTA init-hang family.
    return startupActivationPending && streamlineFGRunning && !postSLActive &&
           (!processFrameRecentlySeen || useTopLevelHandoffWrapperProgress || sameQueuePureDLSSColdStartSafe);
}

inline bool ShouldBootstrapPostSLOverlayState(bool streamlineFGRunning, bool postSLActive, bool overlayInit,
                                              bool processFrameRecentlySeen, bool startupActivationPending,
                                              bool postSLActiveButUnconfirmed, bool postSLConfirmedRendering) {
    const bool syntheticStartupHalfArmed =
        !postSLConfirmedRendering && (startupActivationPending || postSLActiveButUnconfirmed);
    return streamlineFGRunning && postSLActive && !overlayInit &&
           (!processFrameRecentlySeen || syntheticStartupHalfArmed);
}

inline bool ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(bool streamlineFGRunning,
                                                                              bool startupActivationPending,
                                                                              bool postSLActiveButUnconfirmed,
                                                                              bool postSLConfirmedRendering,
                                                                              bool postSLConfirmedButStartupSettling) {
    // A fresh pure-DLSS runtime-owned handoff can still be in the fragile startup
    // family for a few confirmed PostSL frames after first confirmation. Arming a
    // separate scene-gap cooldown there can tear the route back off the proven
    // confirmed-standalone path before startup has really settled.
    if (!streamlineFGRunning) {
        return false;
    }

    if (postSLConfirmedRendering) {
        return postSLConfirmedButStartupSettling;
    }

    return startupActivationPending || postSLActiveButUnconfirmed;
}

inline bool ShouldSuppressSceneTransitionCooldownForStablePostSLGap(bool streamlineFGRunning,
                                                                    bool postSLConfirmedRendering,
                                                                    bool hasPostSLLastWorkingQueue,
                                                                    bool swapchainInvalid, bool deviceRemoved) {
    // A long Present/ProcessFrame gap by itself is not evidence that the proven
    // PostSL queue topology became unsafe.  Once PostSL has confirmed rendering
    // and remembered a working queue, keep drawing after pause/load gaps unless
    // the swapchain/device has actually entered a reset path.
    return streamlineFGRunning && postSLConfirmedRendering && hasPostSLLastWorkingQueue && !swapchainInvalid &&
           !deviceRemoved;
}

// The scene-transition cooldown measures the frame-to-frame delta of the scene
// block inside DX12_ProcessFrameExternal. That block only runs when CE renders
// the overlay through its NORMAL separate-GPU route. While the overlay is
// presented via a runtime-owned / FSR-callback / PostSL route (the runtime owns
// the swapchain, authoritative FSR is active, a runtime-owned native-FG present
// path is live, or protected official-FFX startup is in progress), CE's normal
// scene block is reached at a reduced cadence (≈1 Hz on the FSR callback route),
// so its delta is a MEASUREMENT ARTIFACT, not a real loading-screen stall. Arming
// the cooldown there produces a phantom (observed: repeated `gap=1001ms` every
// ~1.2 s during FSR, session 20260613_202646) that never ticks during the
// non-normal route and then blanks the NORMAL-route overlay for ~14 presents on
// the next FG transition. Suppress arming on these routes; a gap that spans such
// a route must also be discarded by the caller (track the previous run's route).
inline bool ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute(bool runtimeOwnsSwapchain,
                                                                             bool fsrFGApiActive,
                                                                             bool runtimeOwnedNativeFGPresentPath,
                                                                             bool protectedOfficialFFXStartupActive) {
    return runtimeOwnsSwapchain || fsrFGApiActive || runtimeOwnedNativeFGPresentPath ||
           protectedOfficialFFXStartupActive;
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
                                                                  bool selectedQueueOrigECLMatchesRealECL,
                                                                  bool selectedQueueIsOriginalGameQueue = false) {
    if (streamlineFGActive && !hasSLWrapperQueue && !hasRealQueueBehindWrapper && hasRealD3D12ECL &&
        selectedQueueIsSwapchainQueue && selectedQueueOrigECLMatchesRealECL) {
        return true;
    }

    // When a stale runtime-owned swapchain was cleaned up after a long no-FG run,
    // the original game queue becomes the effective swapchain queue. If DLSS FG
    // later activates without a fresh authoritative handoff, PostSL must still be
    // able to bootstrap on the original game queue.
    if (streamlineFGActive && !hasSLWrapperQueue && !hasRealQueueBehindWrapper && selectedQueueIsOriginalGameQueue) {
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

inline bool ShouldReprobeRealD3D12ECLOnFreshAuthoritativeStreamlineHandoff(bool freshAuthoritativeStreamlineHandoff,
                                                                           bool hadFSRFGPhase, bool hasRealD3D12ECL) {
    // Mixed FSR->DLSS sessions can intentionally clear the earlier realECL
    // pointer during FG-off recovery. If the next authoritative Streamline
    // handoff arrives before the later outer SL-ON path re-probes it, the
    // post-FSR safe-bootstrap gate can stay falsely unsafe forever even though
    // the recovered Streamline queue topology is already moving again.
    return freshAuthoritativeStreamlineHandoff && hadFSRFGPhase && !hasRealD3D12ECL;
}

inline bool ShouldDelayPostSLActivationUntilSafeBootstrapPath(bool hadFSRFGPhase, bool hasRealQueueBehindWrapper,
                                                              bool hasRealD3D12ECL, bool hasSLWrapperQueue) {
    if (!hadFSRFGPhase) {
        return false;
    }

    // If we have neither realECL nor SL wrapper, we have no viable submit path.
    // But if the SL wrapper is already known, PostSL can bootstrap through it
    // even before realECL has been reprobed (e.g. after FSR->OFF->DLSS where
    // realECL was cleared during teardown).  The wrapper submit will capture
    // g_RealQueueBehindSLWrapper, enabling the direct path on the next frame.
    if (!hasRealD3D12ECL && !hasSLWrapperQueue) {
        return true;
    }

    return !hasRealQueueBehindWrapper && !hasSLWrapperQueue;
}

inline bool ShouldDelayPostSLActivationUntilSafeBootstrapPath(bool hadFSRFGPhase, bool hasRealQueueBehindWrapper,
                                                              bool hasRealD3D12ECL, bool hasSLWrapperQueue,
                                                              bool safePostFSRBootstrapPath) {
    if (safePostFSRBootstrapPath) {
        return false;
    }

    return ShouldDelayPostSLActivationUntilSafeBootstrapPath(hadFSRFGPhase, hasRealQueueBehindWrapper, hasRealD3D12ECL,
                                                             hasSLWrapperQueue);
}

inline bool ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(bool hadFSRFGPhase,
                                                                        bool hasRuntimeOwnedSwapchainQueue,
                                                                        bool streamlineHandoffOrActive,
                                                                        bool hasTrackedSwapchainQueueSubmitPath) {
    // In 2D/menu-only FSR->DLSS handoffs Streamline can create the live
    // runtime-owned swapchain queue before any wrapper ECL traffic appears. If
    // CE has a Streamline handoff/active signal and a usable submit path for
    // the swapchain queue, PostSL can submit there instead of waiting for a
    // wrapper bootstrap that may not occur until 3D rendering resumes. Do not
    // require the current command queue to equal the swapchain queue: real games
    // commonly keep render/present/runtime queues distinct during FG handoffs.
    return hadFSRFGPhase && hasRuntimeOwnedSwapchainQueue && streamlineHandoffOrActive &&
           hasTrackedSwapchainQueueSubmitPath;
}

inline bool ShouldUsePostSLScQueueVirtualSubmit(bool hadFSRFGPhase, bool scQueueDiffers) {
    return scQueueDiffers && !hadFSRFGPhase;
}

inline bool ShouldUseSelectedSwapchainQueueDirectSubmitForPureDLSS(bool hadFSRFGPhase,
                                                                   bool selectedQueueIsSwapchainQueue,
                                                                   bool hasSelectedQueueOrigECL,
                                                                   bool selectedQueueOrigECLMatchesRealECL) {
    // Pure-DLSS startup can reach a state where the selected live swapchain queue
    // already exposes the real/original D3D12 ECL entrypoint directly. In that
    // case, routing PostSL through the queue's current virtual dispatch needlessly
    // re-enters the hooked path and can perturb Streamline's startup Present path.
    // Prefer the direct/original ECL submit instead. Post-FSR has its own more
    // conservative routing/probe logic and is handled elsewhere.
    return !hadFSRFGPhase && selectedQueueIsSwapchainQueue && hasSelectedQueueOrigECL &&
           selectedQueueOrigECLMatchesRealECL;
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

inline bool ShouldResetPostSLStartupProgressOnReactivation(bool postSLConfirmedRendering, int stablePostSLFrameCount,
                                                           int postSLStallCount, bool runtimeStateStabilizationLogged) {
    // A second PostSL reactivation inside the same DLSS startup family must
    // start a fresh per-epoch settling/stabilization window. Reusing confirmed
    // progress from an older epoch lets the stale-OFF guard expire too early on
    // the newly reactivated path.
    return postSLConfirmedRendering || stablePostSLFrameCount > 0 || postSLStallCount > 0 ||
           runtimeStateStabilizationLogged;
}

inline bool ShouldKeepPostSLActiveWhenRealECLUnavailable(bool hasRealD3D12ECL, bool hasSelectedQueueOriginalSubmitPath,
                                                         bool postSLConfirmedRendering,
                                                         bool postSLActiveButUnconfirmed) {
    if (hasRealD3D12ECL) {
        return true;
    }

    // The broad ProcessFrame guard must not blank a live PostSL route just
    // because the raw d3d12core ECL probe is deferred behind Streamline startup.
    // PostSLOverlayRender has the detailed submit-path safety checks; here we
    // only need to avoid knowingly enabling a route with no submit path at all.
    return hasSelectedQueueOriginalSubmitPath || postSLConfirmedRendering || postSLActiveButUnconfirmed;
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
                                                         bool hasDirectQueueBehindWrapper, bool hasSLWrapperQueue,
                                                         bool hasRuntimeOwnedSwapchainQueue,
                                                         bool explicitSetOptionsActivation,
                                                         bool safePostFSRBootstrapPath) {
    if (!hadFSRFGPhase || !streamlineFGActive) {
        return false;
    }
    if (hasDirectQueueBehindWrapper) {
        return false;
    }

    // Once the post-FSR comeback already has both pieces of stronger recovery
    // evidence - a preserved runtime-owned swapchain queue and a bootstrap
    // topology that is safe enough to leave the synthetic/bypass family -
    // continuing to bootstrap through the SL wrapper is too conservative and can
    // reopen the old DEVICE_REMOVED seam on the first post-FSR probe.
    if (hasRuntimeOwnedSwapchainQueue && safePostFSRBootstrapPath) {
        return false;
    }

    return hasSLWrapperQueue && !explicitSetOptionsActivation;
}

inline bool ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(bool hadFSRFGPhase, bool streamlineFGActive,
                                                                   bool hasDirectQueueBehindWrapper,
                                                                   bool commandQueueIsWrapper,
                                                                   bool hasRuntimeOwnedSwapchainQueue,
                                                                   bool explicitSetOptionsActivation) {
    // During a fresh FSR->DLSS handoff, g_CommandQueue can still point at the
    // previous FSR-owned queue until Streamline's new wrapper/direct traffic has
    // fully surfaced. Once a fresh runtime-owned swapchain queue is already
    // captured, prefer that authoritative handoff evidence over the last seen
    // command queue instead of treating the latter as a wrapper-bootstrap source.
    return hadFSRFGPhase && streamlineFGActive && !hasDirectQueueBehindWrapper && commandQueueIsWrapper &&
           !hasRuntimeOwnedSwapchainQueue && !explicitSetOptionsActivation;
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

// Fast post-FSR DLSS GPU-health probe under safe-bootstrap proof. The graduated
// post-FSR probe (scratch-barrier ×N frames + a separate empty-ECL probe)
// exists because the first overlay ECL on the post-FSR DLSS path could
// DEVICE_REMOVED (FSR->DLSS crash family). But session 20260613_035221 showed
// every FSR->DLSS engage burning ~4 presents on probes that always pass — a
// ~25ms overlay seam on each DLSS engage.
//
// When the safe post-FSR bootstrap proof holds AND the overlay submits on the
// runtime-owned swapchain queue itself (selectedQueueIsSwapchainQueue: SL owns
// that queue's backbuffer state — NOT the documented origGame first-ECL crash
// case) while Streamline FG is running, one scratch-barrier health frame is
// enough: collapse the level-0 dwell to a single frame and skip the redundant
// empty-ECL probe, so the first confirmed overlay draw lands one present after
// the single probe instead of four. Any path lacking the proof, or submitting
// off the swapchain queue, keeps the full graduated probe. This only changes
// HOW FAST a proven-safe route reaches its first draw, not whether a route is
// allowed to draw.
inline bool ShouldUseFastPostFSRDLSSProbeForSafeBootstrap(bool hadFSRFGPhase, bool safePostFSRBootstrapPath,
                                                          bool selectedQueueIsSwapchainQueue,
                                                          bool streamlineFGRunning) {
    return hadFSRFGPhase && safePostFSRBootstrapPath && selectedQueueIsSwapchainQueue && streamlineFGRunning;
}

// Zero-frame post-FSR DLSS reactivation (synthetic dx12_fg_switch_test session 20260615_010145 +
// Talos FSR->DLSS). Even with the fast probe above, the FIRST post-FSR DLSS reactivation present
// still spends itself on the single scratch-barrier health probe and renders the overlay only on the
// NEXT present, leaving a documented 1-present `postsl-bootstrap-reactivation` flicker on every DLSS
// engage (missed=1). But when the fast-bootstrap proof holds, the real overlay render is ITSELF a
// sufficient device-health proof: the PostSL render path already does a pre-submit
// GetDeviceRemovedReason() bail (so it never submits into an already-removed device) and re-checks
// device-removed after the submit. On the SL-owned swapchain queue (the non-origGame, non-crash case
// the fast proof requires) the separate scratch-barrier probe is therefore redundant and only costs
// the frame. Render the overlay directly on the first reactivation present instead. Fires only on the
// first present (probe level 0); after the caller advances to full-render level it is false. The
// slower graduated probe is retained for the unproven / off-swapchain-queue (genuinely fragile) paths.
inline bool ShouldRenderOverlayDirectlyOnFirstPostFSRDLSSReactivation(bool fastPostFSRDLSSProbe,
                                                                      int postFSRProbeLevel) {
    return fastPostFSRDLSSProbe && postFSRProbeLevel == 0;
}

// Zero-frame PURE-DLSS reactivation (off->DLSS, synthetic dx12_fg_switch_test session 20260615_014832).
// On a pure-DLSS reactivation (hadFSR=0, so the fast post-FSR probe above does NOT apply), epoch>1 still
// spends the first reactivation present on a single empty-ECL queue-health probe
// (gate=postsl-transition-probe) and renders the overlay only on the NEXT present — a confirmed
// 1-present off->DLSS overlay blank (coverage drawObserved=0 covered=0 currentStreak=1). The empty-ECL
// probe is redundant when the overlay submits on the SL-owned swapchain queue (the non-origGame,
// non-crash case): the PostSL render path already bails pre-submit on GetDeviceRemovedReason() and
// re-checks device-removed after the submit, so the real overlay render is itself the queue-health
// proof. Render directly on the first reactivation present instead. The empty-ECL probe is retained for
// off-swapchain-queue (origGame first-ECL, genuinely fragile) reactivations.
inline bool ShouldRenderOverlayDirectlyOnPostSLTransitionProbe(bool selectedQueueIsSwapchainQueue, bool deviceHealthy) {
    return selectedQueueIsSwapchainQueue && deviceHealthy;
}

inline bool ShouldSyntheticPostSLRefreshMetrics(bool streamlineFGRunning, bool processFrameRecentlySeen) {
    return streamlineFGRunning && !processFrameRecentlySeen;
}

inline bool ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(bool hadFSRFGPhase,
                                                                        bool safePostFSRBootstrapPath) {
    // Post-FSR recovery still benefits from letting Streamline's recovered queue
    // path stabilize across multiple runtime callbacks until the stronger safe
    // bootstrap proof is available. Once that proof exists, waiting through the
    // generic countdown only creates a visible overlay gap during FSR->DLSS mode
    // switches.
    return hadFSRFGPhase && !safePostFSRBootstrapPath;
}

inline bool ShouldUseTopLevelHandoffWrapperProgressForSyntheticPostSLActivation(bool hadFSRFGPhase,
                                                                                bool startupTopLevelPresentConsumed,
                                                                                bool wrapperProgressObserved) {
    return !hadFSRFGPhase && startupTopLevelPresentConsumed && wrapperProgressObserved;
}

inline bool HasConfirmedPureStreamlinePostSLResumeProof(bool hadFSRFGPhase, bool streamlineFGRunning,
                                                        bool startupTopLevelPresentConsumed,
                                                        bool hasPostSLLastWorkingQueue, bool hasSwapchainQueue,
                                                        bool swapchainQueueMatchesPostSLLastWorkingQueue) {
    // This is stronger than generic wrapper-progress proof: DLSS is currently
    // running, the startup handoff has already been consumed, and the live
    // Streamline swapchain queue is the same queue that previously submitted
    // visible PostSL overlay work. That means this is a proven resume, not a
    // cold DLSS startup.
    return !hadFSRFGPhase && streamlineFGRunning && startupTopLevelPresentConsumed && hasPostSLLastWorkingQueue &&
           hasSwapchainQueue && swapchainQueueMatchesPostSLLastWorkingQueue;
}

// Pure-DLSS engage proof for the synthetic-startup countdown and the
// cold-start reactivation warmup. The strongest available evidence for a
// pure-DLSS enable: the CURRENT comeback was activated by an explicit
// slDLSSGSetOptions(ON) edge (per-comeback provenance, not a sticky session
// latch), the runtime-owned startup activation swapchain is retained (CE saw
// the live Streamline present and holds it), and the PostSL callback is
// installed — by construction the gates consuming this proof run inside a
// PostSL callback, i.e. Streamline's present pipeline is already presenting.
// GetState-only enables (weaker evidence; the historical GTA startup-churn
// family) keep the full countdown + warmup. Session 20260612_215439: without
// this proof the 8-callback countdown plus the 15-callback cold-start warmup
// ran back-to-back and blanked the overlay for 22 presents (~150 ms) on every
// OFF->DLSS engage.
inline bool HasExplicitEnablePureDLSSColdStartProof(bool hadFSRFGPhase, bool explicitSetOptionsActivation,
                                                    bool hasRetainedStartupActivationSwapchain,
                                                    bool postSLCallbackInstalled) {
    return !hadFSRFGPhase && explicitSetOptionsActivation && hasRetainedStartupActivationSwapchain &&
           postSLCallbackInstalled;
}

inline bool ShouldBypassPostSLReactivationWarmup(bool hadFSRFGPhase, bool useTopLevelHandoffWrapperProgress,
                                                 bool safePostFSRBootstrapPath,
                                                 bool confirmedPureStreamlineResumeProof = false,
                                                 bool explicitEnablePureDLSSColdStartProof = false,
                                                 bool postSLConfirmedRenderInCurrentEpoch = false,
                                                 bool sameQueuePureDLSSColdStartSafe = false) {
    // Do not bypass warm-up for an UNPROVEN pure DLSS cold start. DLSS FG's
    // multi-device initialization is fragile: submitting overlay ECL on the FG
    // queue during the first few callbacks can corrupt DLSS FG's internal
    // mutex/fence state. The warm-up period lets DLSS FG stabilize before our
    // first GPU work lands on its queue. PostSL still activates and logs
    // progress during warm-up - only the ECL submit is deferred.
    //
    // A confirmed pure-DLSS resume is different from a cold start: the same
    // live swapchain queue has already submitted visible PostSL work before the
    // suspend/resume edge, so forcing another 30-frame warm-up creates a visible
    // overlay blink without adding real safety.
    //
    // An explicit-enable cold start (HasExplicitEnablePureDLSSColdStartProof)
    // is the proof-gated no-blank engage path: explicit slDLSSGSetOptions(ON)
    // provenance + retained startup activation swapchain. GetState-only
    // enables never reach it and keep the warmup.
    //
    // STRONGEST proof: a CONFIRMED render already happened in THIS reactivation
    // epoch. The warm-up only exists to protect the FIRST ECL submit; once a
    // confirmed render landed (devRemoved=0) the first ECL already succeeded, so
    // the hazard is past and continuing the warm-up would only re-blank a live
    // overlay (the no-blank principle). This is route-agnostic and survives the
    // release of the retained startup-activation swapchain (which drops the
    // explicit-enable proof after frame 1). It cannot widen the GTA GetState-only
    // cold-start hang family: that family gets no frame-1 bypass, so it produces
    // no confirmed render during the warm-up and this leg stays false until the
    // warm-up has naturally completed.
    (void)useTopLevelHandoffWrapperProgress;
    return postSLConfirmedRenderInCurrentEpoch || (hadFSRFGPhase && safePostFSRBootstrapPath) ||
           (!hadFSRFGPhase && (confirmedPureStreamlineResumeProof || explicitEnablePureDLSSColdStartProof ||
                               sameQueuePureDLSSColdStartSafe));
}

}  // namespace ce::dx12_overlay_policy
