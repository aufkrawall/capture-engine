#pragma once

#include <windows.h>

#include <cstdint>

#include <dxgi1_6.h>

#include "../dxgi_presentation_color.h"
#include "../fg_runtime_state.h"

struct ID3D12CommandQueue;
struct ID3D12Fence;

#include "overlay_submission.h"

// Native FSR owner-queue routing, FFX presenter fallback, and UI overlay target selection.

namespace ce::dx12_overlay_policy {

enum class NativeFSROwnerQueueRoute {
    kExactDescriptorQueue,
    kStreamlineUnderlyingGameQueue,
    kUnavailable,
};

// An FFX swapchain created through Streamline can receive Streamline's command-queue wrapper. Its GetDevice
// identity is the wrapper device, while FFX UI/backbuffer resources and CE command lists belong to the real
// device. In that one proven topology, the already-captured original game queue is the underlying submission
// queue. Never substitute it for an arbitrary different-device FFX queue.
inline NativeFSROwnerQueueRoute ChooseNativeFSROwnerQueueRoute(bool exactQueueMatchesTargetDevice,
                                                               bool exactQueueUsesAcceptedStreamlineDevice,
                                                               bool originalGameQueueMatchesTargetDevice) {
    if (exactQueueMatchesTargetDevice) {
        return NativeFSROwnerQueueRoute::kExactDescriptorQueue;
    }
    if (exactQueueUsesAcceptedStreamlineDevice && originalGameQueueMatchesTargetDevice) {
        return NativeFSROwnerQueueRoute::kStreamlineUnderlyingGameQueue;
    }
    return NativeFSROwnerQueueRoute::kUnavailable;
}

// A protected official-FFX ffxCreateContext call can already be in flight when CE routes a cached export
// pointer. The nested DXGI swapchain create proves that missed-create topology, but its queue is FFX's newly
// created internal presentQueue, NOT the creation descriptor's gameQueue. The recoverable owner is the retained
// pre-FSR original game queue that produced the UI/backbuffers. Join that evidence to the first observed proxy
// only when the primary create hook did not already publish stronger exact descriptor evidence.
inline bool ShouldRecoverNativeFSRProxyBindingFromProtectedCreate(bool hasExistingBinding, bool hasContext,
                                                                  bool hasProxySwapChain,
                                                                  bool hasProtectedInnerPresentQueue,
                                                                  bool hasOriginalGameQueue) {
    return !hasExistingBinding && hasContext && hasProxySwapChain && hasProtectedInnerPresentQueue &&
           hasOriginalGameQueue;
}

// AMD can emit multiple real-swapchain Presents (real + generated outputs) for one accepted UI-resource
// registration. The presenter-thread compatibility route must blend only once into that shared input.
inline bool ShouldCompositeFFXPresenterFallback(uint64_t acceptedUiSequence, uint64_t lastCompositedUiSequence) {
    return acceptedUiSequence == 0 || acceptedUiSequence != lastCompositedUiSequence;
}

// Prefer the round-robin slot for cache locality, but never stall the Present thread merely because that
// one allocator is still in flight while another allocator in the pool is already reusable. A negative
// result means every slot is genuinely busy and the caller may use its bounded exceptional-path wait.
inline int ChooseReadyOverlayAllocatorSlot(const uint64_t* fenceValues, int slotCount, int preferredSlot,
                                           uint64_t completedFenceValue) {
    if (!fenceValues || slotCount <= 0 || preferredSlot < 0 || preferredSlot >= slotCount) {
        return -1;
    }
    for (int offset = 0; offset < slotCount; ++offset) {
        const int slot = (preferredSlot + offset) % slotCount;
        if (fenceValues[slot] == 0 || fenceValues[slot] <= completedFenceValue) {
            return slot;
        }
    }
    return -1;
}

// GTA Enhanced leaves UI composition ENABLED but registers a 1x1 placeholder UI texture, so AMD composites
// nothing usable. Drawing the overlay onto a 1x1 texture is useless (and trips CreateCommandList/Reset
// E_INVALIDARG), so when the game's UI texture is degenerate relative to the backbuffer, CE substitutes its
// own backbuffer-sized texture into the forwarded RegisterUiResource and AMD composites THAT. When the game
// registers a usable full-size UI texture (the test app, and games that composite their HUD this way), CE
// blends the overlay directly onto it (no substitution, no extra allocation).
//
// If the backbuffer size is not known yet (0), fall back to the clearly-degenerate (<=1px) test only, so we
// never substitute against a usable texture just because we could not size-compare it.
inline FFXUiOverlayTarget ChooseFFXUiOverlayTarget(uint32_t gameTexWidth, uint32_t gameTexHeight,
                                                   uint32_t backbufferWidth, uint32_t backbufferHeight) {
    const bool triviallyDegenerate = gameTexWidth <= 1 || gameTexHeight <= 1;
    const bool haveBackbufferSize = backbufferWidth > 0 && backbufferHeight > 0;
    // A usable UI texture must cover most of the backbuffer. Anything under half the backbuffer in either
    // dimension is treated as a placeholder, not the real HUD-composition surface.
    const bool muchSmallerThanBackbuffer =
        haveBackbufferSize && (gameTexWidth * 2u < backbufferWidth || gameTexHeight * 2u < backbufferHeight);
    if (triviallyDegenerate || muchSmallerThanBackbuffer) {
        return FFXUiOverlayTarget::kSubstituteCEFullSizeTexture;
    }
    return FFXUiOverlayTarget::kCompositeOntoGameTexture;
}

inline bool ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(bool hasOriginalGameQueue,
                                                                            bool queueMatchesOriginalGameQueue,
                                                                            bool streamlineRuntimeAvailable,
                                                                            bool hadFSRFGPhase, bool fsrFGApiActive,
                                                                            fg_runtime::RuntimeMode runtimeMode) {
    if (!hasOriginalGameQueue || queueMatchesOriginalGameQueue || !streamlineRuntimeAvailable) {
        return false;
    }

    // After native FSR has owned presentation, Streamline can create a fresh
    // runtime swapchain and immediately install its own Present hook on that
    // swapchain. Re-applying CE's vtable hook in that narrow window steals the
    // first Present away from Streamline and can crash inside the driver. Keep
    // CE's inline hook available for the re-entrant PostSL path, but let
    // Streamline establish the outer Present chain first.
    return hadFSRFGPhase || fsrFGApiActive || fg_runtime::RuntimeModeUsesFSR(runtimeMode);
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

enum class InactiveDLSSPresentRoute {
    kNormal,
    kConfirmedPostSLKeepAlive,
    kAwaitNormalOwnershipProof,
};

inline bool IsPostFSRNormalRouteOwnershipProven(bool hasSwapchainQueue, bool hasOriginalGameQueue,
                                                bool swapchainQueueMatchesOriginalGameQueue,
                                                bool currentSwapchainMatchesCapturedSwapchainQueue,
                                                bool currentSwapchainMatchesProvenOriginalQueueSwapchain) {
    // A swapchain pointer change is not ownership proof. FSR/DLSS transitions
    // can alternate between two already-existing runtime proxies without
    // creating a new swapchain at that boundary. Accept only a fresh captured
    // original-queue association, or the exact swapchain identity whose
    // original-queue association was proven before FG took ownership.
    return hasOriginalGameQueue && ((hasSwapchainQueue && swapchainQueueMatchesOriginalGameQueue &&
                                     currentSwapchainMatchesCapturedSwapchainQueue) ||
                                    currentSwapchainMatchesProvenOriginalQueueSwapchain);
}

inline InactiveDLSSPresentRoute DecideInactiveDLSSPresentRoute(
    bool routeProtectionPending, bool actualFGActive, bool streamlineFGRunning, bool normalRouteOwnershipProven,
    bool postSLKeepAliveArmed, bool postSLCallbackReady, bool hasPostSLRenderQueue,
    bool currentSwapchainMatchesLastSuccessfulPostSLSwapchain) {
    if (!routeProtectionPending || actualFGActive || streamlineFGRunning || normalRouteOwnershipProven) {
        return InactiveDLSSPresentRoute::kNormal;
    }

    // While DLSS is explicitly off, the Streamline proxy can remain the live
    // Present path without issuing another re-entrant callback. Keep that exact,
    // previously successful proxy on its already-proven PostSL route. ProcessFrame
    // performs one ordinary PostSL draw immediately before the pass-through
    // Present; any same-thread nested callback is de-duplicated for that Present.
    // This requires no copy, new queue, wait, or normal-route backbuffer access.
    if (postSLKeepAliveArmed && postSLCallbackReady && hasPostSLRenderQueue &&
        currentSwapchainMatchesLastSuccessfulPostSLSwapchain) {
        return InactiveDLSSPresentRoute::kConfirmedPostSLKeepAlive;
    }

    // Neither the historical PostSL queue nor the original queue is safe for an
    // unknown swapchain. Stay GPU-quiet until creation/identity evidence proves
    // one of those existing routes; do not guess from the pointer-change event.
    return InactiveDLSSPresentRoute::kAwaitNormalOwnershipProof;
}

inline bool ShouldRejectPostSLKeepAliveRenderForUnprovenSwapchain(
    bool postSLKeepAliveArmed, bool streamlineFGRunning, bool hasLastSuccessfulPostSLSwapchain,
    bool currentSwapchainMatchesLastSuccessfulPostSLSwapchain) {
    return postSLKeepAliveArmed && !streamlineFGRunning &&
           (!hasLastSuccessfulPostSLSwapchain || !currentSwapchainMatchesLastSuccessfulPostSLSwapchain);
}

inline SwapchainOverlayRoutingDecision DecideSwapchainOverlayRouting(
    bool runtimeOwnsSwapchain, bool streamlineFGActive, bool fsrFGActive, bool hadFSRFGPhase, bool hasSwapchainQueue,
    bool hasOriginalGameQueue, bool hasPostSLLastWorkingQueue, bool postFSRInactiveRecoveryPending,
    bool commandQueueMatchesPrimaryGameQueue, bool explicitNativeFSROffPendingRuntimeOwnedTeardown = false,
    bool nativeFSRInternalNoCallbackCompositionLive = false) {
    if (streamlineFGActive && hadFSRFGPhase) {
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue
                                 : SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue;
    }

    if (streamlineFGActive && hasOriginalGameQueue) {
        return SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue;
    }

    if (explicitNativeFSROffPendingRuntimeOwnedTeardown && nativeFSRInternalNoCallbackCompositionLive &&
        runtimeOwnsSwapchain) {
        // Suspended native FSR on AMD's internal no-callback composition route:
        // the FSR-owned swapchain is still the live present path and no
        // callback bridge exists that could draw the overlay, so keep rendering
        // through the runtime-owned swapchain queue exactly like the active
        // no-callback case. Routing the still-live FSR swapchain's backbuffers
        // to the original game queue would be exactly the cross-queue mismatch
        // the post-FSR-inactive recovery below exists to avoid.
        return hasSwapchainQueue ? SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue
                                 : SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue;
    }

    if (explicitNativeFSROffPendingRuntimeOwnedTeardown && !fsrFGActive && hadFSRFGPhase && hasOriginalGameQueue) {
        // A disabled native-FSR configure means the FFX runtime has suspended
        // generation and the live Present path can already be back on the game
        // queue while the FSR-owned swapchain queue latch is still draining.
        // Rendering the recovered normal overlay on that stale FSR queue has
        // been observed to DEVICE_REMOVED; route to the original Present queue
        // until a later clean swapchain/queue transition clears the teardown.
        return SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue;
    }

    if (!streamlineFGActive && !fsrFGActive && hadFSRFGPhase && !hasSwapchainQueue && hasOriginalGameQueue) {
        // After FSR->DLSS->off, ProcessFrame intentionally leaves g_SwapchainQueue
        // unset until a future clean non-FG swapchain transition can re-establish
        // queue ownership. The preserved PostSL last-working queue remains valid
        // only while that recovery epoch is explicitly pending. A clean normal
        // swapchain transition ends the epoch and makes the original Present
        // queue authoritative; recent traffic on the old PostSL queue is not
        // render-ownership proof for the replacement swapchain.
        if (postFSRInactiveRecoveryPending && hasPostSLLastWorkingQueue) {
            return SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue;
        }

        // During recovery this is the fallback when no PostSL proof exists. After
        // a clean normal return it is the required route even if a stale retained
        // PostSL pointer has not yet been retired.
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

inline bool ShouldStartFrameGenerationTransitionCooldown(fg_runtime::RuntimeMode previousRuntimeMode,
                                                         fg_runtime::RuntimeMode nextRuntimeMode,
                                                         bool previousEffectiveFGActive, bool nextEffectiveFGActive,
                                                         bool previousStreamlineFGSignal, bool nextStreamlineFGSignal,
                                                         bool liveNoCallbackNativeFSRSuspensionToggle = false) {
    if (liveNoCallbackNativeFSRSuspensionToggle) {
        // Native-FSR suspend/resume on AMD's internal no-callback composition
        // route flips only the FG flag: the runtime-owned swapchain stays the
        // live present path and the overlay backend already renders on that
        // exact queue. Arming the draw cooldown here is what visibly blanks
        // the overlay at every menu-style FG suspension toggle.
        return false;
    }

    if (previousStreamlineFGSignal != nextStreamlineFGSignal) {
        return true;
    }

    if (previousEffectiveFGActive != nextEffectiveFGActive) {
        return true;
    }

    const bool previousActualGenerated = fg_runtime::IsActualGeneratedFrameMode(previousRuntimeMode);
    const bool nextActualGenerated = fg_runtime::IsActualGeneratedFrameMode(nextRuntimeMode);
    if (previousActualGenerated || nextActualGenerated) {
        return previousRuntimeMode != nextRuntimeMode;
    }

    return false;
}

// Native-FSR suspend/resume/enable toggles (menu/loading style ffxConfigure
// frameGenerationEnabled flips and the post-takeover enable classification)
// on AMD's internal no-callback composition route do not move presentation:
// the runtime-owned swapchain and its queue stay live, and the overlay
// backend is already initialized on that queue. Only that exact shape may
// skip the FG transition draw cooldown. Requiring the backend queue to match
// the swapchain queue keeps the early enable edge (backend still on the
// original game queue before the FFX swapchain goes live) on the protected
// cooldown path.
//
// The non-FSR side accepts kStreamlineNoFG in addition to kOff: with
// Streamline DLLs merely loaded (no SL FG signal — enforced above), the
// classifier labels the non-FG state STREAMLINE_NO_FG. Session
// 20260612_215439 proved the gap: the finalized no-callback FFX takeover
// rebuilt the overlay on the runtime queue (`immediate overlay reinit ...
// prevCooldown=0`), then one frame later the STREAMLINE_NO_FG->FSR_FG
// classification re-armed the 60-frame draw cooldown and blanked the healthy
// overlay for 60 presents because this exemption only matched kOff.
inline bool IsLiveNoCallbackNativeFSRSuspensionToggle(fg_runtime::RuntimeMode previousRuntimeMode,
                                                      fg_runtime::RuntimeMode nextRuntimeMode, bool streamlineFGRunning,
                                                      bool nativeFSRInternalNoCallbackComposition,
                                                      bool runtimeOwnsSwapchain, bool hasSwapchainQueue,
                                                      bool overlayBackendInitialized,
                                                      bool overlayBackendQueueIsSwapchainQueue) {
    if (streamlineFGRunning || !nativeFSRInternalNoCallbackComposition || !runtimeOwnsSwapchain || !hasSwapchainQueue ||
        !overlayBackendInitialized || !overlayBackendQueueIsSwapchainQueue) {
        return false;
    }

    const bool previousIsFSR = previousRuntimeMode == fg_runtime::RuntimeMode::kFSRFG;
    const bool nextIsFSR = nextRuntimeMode == fg_runtime::RuntimeMode::kFSRFG;
    const bool previousIsNonFG = previousRuntimeMode == fg_runtime::RuntimeMode::kOff ||
                                 previousRuntimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG;
    const bool nextIsNonFG =
        nextRuntimeMode == fg_runtime::RuntimeMode::kOff || nextRuntimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG;
    return (previousIsFSR && nextIsNonFG) || (previousIsNonFG && nextIsFSR);
}

// The DX12 overlay adapter backend (root signature, PSOs, font atlas,
// vertex/index upload pools) is DEVICE+FORMAT scoped; its bound command queue
// is stored but never used for resource creation or submission (the hook
// records into its own command list and submits explicitly). A warm backend
// may therefore be reused across a QUEUE change — FG transitions hand the
// swapchain to a different queue every time — as long as device and RTV
// format still match. Queue-bound sync objects (allocators/fence) are rebuilt
// separately by InitOverlaySync after the transition GPU drains. Session
// 20260612_215439: requiring queue equality forced a full backend rebuild on
// every FG transition (2-4 uncovered presents each).
inline bool CanReuseWarmDX12OverlayBackend(bool preserveRequested, bool adapterInitialized, bool deviceMatches,
                                           bool formatMatches) {
    return preserveRequested && adapterInitialized && deviceMatches && formatMatches;
}

// A fresh Streamline proxy has a short, uniquely safe preparation window: the authoritative proxy queue and
// buffers already exist, but DLSS FG has not been enabled yet. If the overlay was live on the retiring route,
// prepare the new swapchain-scoped RTV/synchronization objects in that window so its FG-off passthrough Present
// and first PostSL Present both inherit a ready backend. Post-FSR handoffs have this proof through FSR history.
// Repeated pure-DLSS activation has equivalent proof only after this process already completed a device-healthy
// PostSL submit; first-ever pure-DLSS cold start retains its stricter initialization guards.
inline bool ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(bool freshAuthoritativeStreamlineHandoff, bool hadFSRFGPhase,
                                                           bool hadSuccessfulPostSLPhase, bool runtimeOwnsSwapchain,
                                                           bool streamlineFGRunning, bool overlayWasLive,
                                                           bool isDX12Swapchain) {
    const bool hasPriorRuntimeRouteProof = hadFSRFGPhase || hadSuccessfulPostSLPhase;
    return freshAuthoritativeStreamlineHandoff && hasPriorRuntimeRouteProof && runtimeOwnsSwapchain &&
           !streamlineFGRunning && overlayWasLive && isDX12Swapchain;
}

inline bool ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(
    bool exactPrewarmedSwapchainProof, bool overlayInit, bool syncInit, bool hasRTVHeap, bool hasCommandList,
    bool runtimeOwnsSwapchain, bool hasSwapchainQueue, bool queueCaptureMatchesSwapchain, bool fsrFGApiActive,
    bool nativeFSRPresentPathActive, bool deviceRemoved) {
    // Prewarming has already rebuilt the swapchain-scoped RTV/sync state for
    // this exact Streamline proxy and its captured queue. The proxy can issue a
    // passthrough Present before slDLSSGSetOptions(ON); treating that first
    // Present as an ordinary pointer change destroys the ready backend and
    // creates an uncovered source/output seam. Consume the exact creation-time
    // proof instead. A concurrent FSR takeover, stale queue association, partial
    // prewarm, or removed device retains normal teardown.
    return exactPrewarmedSwapchainProof && overlayInit && syncInit && hasRTVHeap && hasCommandList &&
           runtimeOwnsSwapchain && hasSwapchainQueue && queueCaptureMatchesSwapchain && !fsrFGApiActive &&
           !nativeFSRPresentPathActive && !deviceRemoved;
}

// PRINCIPLE: a live overlay is never blanked by an FG transition. The
// FG-transition draw cooldown suppresses the normal-route overlay draw for ~60
// frames "to let the runtime stabilize", but when the overlay backend is
// already live its sync resources (allocators/fence/cmdList) are device-level
// and work on ANY DIRECT queue, the per-frame submit-queue resolution already
// retargets to the live present queue, and (when the swapchain is reused) the
// RTVs are still valid — nothing needs rebuilding, so the cooldown is pure
// gratuitous draw suppression. Session 20260613_041204: an OFF->FSR no-callback
// takeover (swapchain reused, syncInit kept) blanked a fully-live overlay for
// 60 presents, then the normal route drew fine on that exact FSR queue when the
// cooldown expired.
//
// The normal route is the overlay's transport (so keep-drawing applies) only
// when: the backend is initialized AND sync-ready, we are NOT in the pre-enable
// protected official-FFX startup window (the one proven no-draw window —
// drawing there wedges AMD's presenter), Streamline FG is NOT running (DLSS
// routes the overlay through PostSL, which owns its own stabilization), and the
// app-callback FFX bridge is NOT the active FSR route (there the bridge renders
// the overlay and a separate normal ECL is the documented 0x887A002B
// device-removal). AMD's internal no-callback FSR composition explicitly allows
// the normal overlay route, so the no-callback takeover/suspend/resume edges all
// keep drawing.
inline bool ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown(bool overlayInit, bool syncInit,
                                                                    bool protectedOfficialFFXStartupActive,
                                                                    bool streamlineFGRunning,
                                                                    bool appCallbackBridgeFSRActive) {
    return overlayInit && syncInit && !protectedOfficialFFXStartupActive && !streamlineFGRunning &&
           !appCallbackBridgeFSRActive;
}

// NOTE (round 3, 2026-06-14): the round-2 `ShouldKeepDrawingPreSLOverlayDuringSameQueueDLSSStartup`
// predicate was REMOVED. It never engaged: on the DLSS-startup path the game's Present is consumed
// by Streamline's proxy, so every startup present is SL re-entrant (PostSL) and
// DX12_ProcessFrameExternal is dormant — there is no pre-SL frame to draw. The startup overlay can
// only come from PostSL, which is held off by the cold-start warmup (the documented GTA DLSS-init
// crash protection). See guardrails.md (D-round Talos notes).

// A runtime-mode flip that changes only the heuristic FG label, not the
// transport: no Streamline FG signal on either side, FG swapchain ownership
// and the live swapchain queue unchanged, no authoritative FSR API state, and
// the overlay backend initialized on the exact live queue. Such flips come
// from detection heuristics (ECL-count pattern, queue-change) re-evaluating —
// nothing about presentation moved, so arming the 60-frame draw cooldown only
// blanks a healthy overlay. Session 20260612_215439: stale ECL-pattern
// evidence latched phantom FSR_FG right after FSR->OFF swapchain recovery and
// the double flip (STREAMLINE_NO_FG->FSR_FG->STREAMLINE_NO_FG, ownership=0,
// sl_signal=0->0) armed two 60-frame cooldowns for a 61-present blank.
// Real transport changes always alter at least one of: SL signal, runtime
// ownership, the live queue, or authoritative FSR state — they keep the
// cooldown.
inline bool IsHeuristicOnlyRuntimeModeFlip(bool previousStreamlineFGSignal, bool nextStreamlineFGSignal,
                                           bool runtimeOwnsSwapchain, bool fsrFGApiActive, bool hasSwapchainQueue,
                                           bool overlayBackendInitialized, bool overlayBackendQueueIsSwapchainQueue) {
    return !previousStreamlineFGSignal && !nextStreamlineFGSignal && !runtimeOwnsSwapchain && !fsrFGApiActive &&
           hasSwapchainQueue && overlayBackendInitialized && overlayBackendQueueIsSwapchainQueue;
}

// A disabled native-FSR configure with no callback route is a suspension
// (menu/loading) as long as the runtime-owned swapchain is still the live
// present path. AMD keeps presenting through its internal no-callback
// composition, so the normal DX12 overlay route on the runtime-owned
// swapchain queue remains the only transport that can keep the overlay
// visible; dropping the latch would leave the suspension with no overlay
// route at all. Context destruction and real ownership unwind clear the
// latch through their own explicit paths.
inline bool ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(
    bool enabled, bool bridgeActive, bool appCallbackProvided, bool previousInternalNoCallbackComposition,
    bool runtimeOwnsLivePresentPath) {
    return !enabled && !bridgeActive && !appCallbackProvided && previousInternalNoCallbackComposition &&
           runtimeOwnsLivePresentPath;
}

// The first Present through AMD's runtime-owned swapchain right after an
// enabled ffxConfigure finalized the official FFX takeover on the internal
// no-callback composition route. The staged runtime queue is already applied
// and normal overlay rendering on it is the approved transport, so the
// generic 90-frame FG-transition reinit cooldown would only blank the
// overlay for ~1.5s. Pre-enable protected startup windows can never reach
// this gate because direct FFX confirmation requires the enabled-configure
// proof first.
inline bool ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(
    bool directFFXApiConfirmation, bool fsrFGApiActive, bool nativeFSRInternalNoCallbackComposition,
    bool runtimeOwnsSwapchain, bool hasSwapchainQueue, bool streamlineFGRunning) {
    return directFFXApiConfirmation && fsrFGApiActive && nativeFSRInternalNoCallbackComposition &&
           runtimeOwnsSwapchain && hasSwapchainQueue && !streamlineFGRunning;
}

// A game-created (non-runtime, non-third-party-overlay caller) swapchain
// creation while explicit native-FSR OFF/destroy evidence is pending is the
// "stronger off signal" the runtime-owned teardown was waiting for. The
// existing teardown end only fires when the swapchain queue returns to the
// original game queue; games that recreate their swapchain on a FRESH queue
// (observed in dx12_fg_switch_test FSR->OFF, 20260611_191950) otherwise stay
// misclassified as runtime-owned forever, which blanks the overlay through
// FG cooldowns and can re-latch FSR heuristics on plain game queues.
//
// nativeFSRInternalNoCallbackCompositionActive (added 2026-06-23, session
// 20260623_053805): under the no-callback composition route the explicit
// OFF/destroy signals can be MISSED ENTIRELY — ffxDestroyContext is bypassed by
// raw export pointers, and the one-shot ffxConfigure VEH is permanently disarmed
// after detection — so neither pending flag is ever set and (with the test app
// recreating on a FRESH queue) the origGame-return teardown never fires either.
// The no-callback composition being active IS the runtime-owned-native-FG state,
// so a game-created swapchain there is itself sufficient proof of OFF: the game
// has replaced AMD's FfxFrameInterpolationSwapchain with its own native
// swapchain. The present-callback toggle only RECONFIGURES (it does not recreate
// the swapchain), so an active FSR session produces no game-created swapchain and
// hence no false positive.
inline bool ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(
    bool gameCreatedSwapchain, bool explicitNativeFSROffPending, bool nativeFSRContextsDestroyedPending,
    bool nativeFSRInternalNoCallbackCompositionActive, bool streamlineFGRunning) {
    return gameCreatedSwapchain && !streamlineFGRunning &&
           (explicitNativeFSROffPending || nativeFSRContextsDestroyedPending ||
            nativeFSRInternalNoCallbackCompositionActive);
}

// FSR_FG -> non-FG classification flip right after the game-created swapchain
// recovery ended the runtime-owned native-FSR teardown. The live swapchain
// queue IS the recovery queue the game just created, so there is no
// present-path movement left for the draw cooldown to protect; arming it
// only blanks the overlay for ~60 frames after every real FSR FG -> off.
//
// The non-FSR side accepts kStreamlineNoFG in addition to kOff: after a prior
// DLSS phase the Streamline DLLs stay loaded (no SL FG signal — enforced
// here), so the recovered non-FG state classifies as STREAMLINE_NO_FG. Session
// 20260613_035221 proved the gap: the second FSR->OFF (after a DLSS phase)
// ran the recovery edge correctly but the `FSR_FG -> STREAMLINE_NO_FG`
// classification armed the 60-frame cooldown anyway because this exemption
// only matched kOff (same blind spot fixed for the no-callback suspension
// toggle in the 2026-06-12 round).
inline bool IsGameSwapchainRecoveryToggleAfterNativeFSROff(fg_runtime::RuntimeMode previousRuntimeMode,
                                                           fg_runtime::RuntimeMode nextRuntimeMode,
                                                           bool streamlineFGRunning,
                                                           bool recoveryQueueMatchesLiveSwapchainQueue) {
    if (streamlineFGRunning || !recoveryQueueMatchesLiveSwapchainQueue) {
        return false;
    }
    const bool nextIsNonFG =
        nextRuntimeMode == fg_runtime::RuntimeMode::kOff || nextRuntimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG;
    return previousRuntimeMode == fg_runtime::RuntimeMode::kFSRFG && nextIsNonFG;
}

// Swapchain change onto the game-created recovery swapchain after explicit
// native-FSR OFF/destroy. The recovery edge already ended runtime ownership
// and the new swapchain's queue was captured at creation, so the overlay can
// rebuild immediately instead of blanking through the recent-FG 90-frame
// reinit cooldown.
inline bool ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff(
    bool recoveryQueueMatchesCurrentSwapchainQueue, bool fsrFGApiActive, bool streamlineFGRunning) {
    return recoveryQueueMatchesCurrentSwapchainQueue && !fsrFGApiActive && !streamlineFGRunning;
}

// A DLSS-FG SUSPEND (slDLSSGSetOptions(off) while the DLSS-G proxy swapchain
// stays alive and keeps presenting) can surface a fresh swapchain POINTER on
// the SAME live runtime-owned queue. The active-FG preserve path
// (ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange) does NOT
// fire because streamlineFGRunning is already false at the suspend edge, so the
// change fell into the generic 90-frame swapchain-change cooldown and blanked a
// LIVE overlay for ~800ms (session 20260613_145008) — masked from the coverage
// gate only because zero-ECL proxy presents inherited coverage while overlayInit
// was false. The make-before-break keep-alive latch (g_PostSLExplicitOffKeepAlive)
// marks a CONFIRMED PostSL path that is merely SUSPENDED — it is never set while
// an FSR/native-FG takeover owns (or is about to own) presentation — so the warm
// device-scoped backend can be reinitialized IMMEDIATELY on its live queue instead
// of blanking through the cooldown. The immediate-reinit branch performs the exact
// same flag resets as the 90-frame cooldown branch it replaces; only the timing
// changes. The suspend has no Streamline teardown to wait for (the proxy is alive),
// so the documented post-teardown first-ECL DEVICE_REMOVED hazard does not apply.
//
// QUEUE PROOF (relaxed 2026-06-13, session 20260613_202646 90-present blank): the live
// confirmed-PostSL queue is satisfied by EITHER the live command queue OR the confirmed
// PostSL render queue (g_PostSLLastWorkingQueue). A DLSS-G proxy renders the overlay on
// its proxy queue (== scQueue == lastWorkingQueue, 180+ confirmed submits) which persists
// across a suspend, while the SL wrapper cmdQueue is a SEPARATE valid object. The original
// strict `scQueue==cmdQueue` test wrongly rejected the safe suspend when scQueue equalled
// the proxy/lastWorkingQueue but not the wrapper cmdQueue, dropping it into the 90-frame
// cooldown. Accepting the confirmed PostSL render queue closes that gap; the keep-alive +
// runtime-ownership + no-FSR/native-FG guards keep the strict cooldown for real takeovers.
inline bool ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
    bool postSLExplicitOffKeepAlive, bool streamlineFGRunning, bool fsrFGApiActive,
    bool nativeFSRInternalNoCallbackComposition, bool runtimeOwnsSwapchain, bool swapchainQueueIsLiveCommandQueue,
    bool swapchainQueueIsConfirmedPostSLRenderQueue) {
    return postSLExplicitOffKeepAlive && !streamlineFGRunning && !fsrFGApiActive &&
           !nativeFSRInternalNoCallbackComposition && runtimeOwnsSwapchain &&
           (swapchainQueueIsLiveCommandQueue || swapchainQueueIsConfirmedPostSLRenderQueue);
}

// DLSS-FG turned OFF over a runtime-owned (FSR-history) swapchain whose ownership latch is
// STALE (Talos menu FG-switch, session 20260614_023730: 89 + 90 presents / 828 ms each). Here
// DLSS-PostSL — not FSR — was the actual presenter (the swapchain-change queue equals the
// confirmed PostSL render queue g_PostSLLastWorkingQueue), but the make-before-break keep-alive
// could NOT arm because ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff requires
// !runtimeOwnedNativeFGPresentPath and the stale FSR-ownership latch is set. So the suspension
// swapchain-change predicate above (which needs the keep-alive) misses it and it falls into the
// generic 90-frame cooldown — yet the warm reinit on the SAME persisting queue is safe
// (devRemoved=0) and the native-FSR-fallback-proof + C1 both allow rendering right after.
// Reinit immediately when DLSS is off, FSR is NOT actually presenting (api inactive, no internal
// no-callback composition, present callback quiet/stalled), the runtime still owns the swapchain,
// the change queue is the confirmed PostSL render queue, and the device is healthy. This is
// strictly distinct from a REAL FSR takeover (fsrFGApiActive=1 or a live FFX present callback),
// which keeps the strict cooldown. GTA RISK: not GTA-validated (FSR-ownership path); independently
// revertible. See guardrails.md.
inline bool ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
    bool streamlineFGRunning, bool fsrFGApiActive, bool nativeFSRInternalNoCallbackComposition,
    bool ffxPresentCallbackActive, bool runtimeOwnsSwapchain, bool swapchainQueueIsConfirmedPostSLRenderQueue,
    bool deviceRemoved) {
    return !streamlineFGRunning && !fsrFGApiActive && !nativeFSRInternalNoCallbackComposition &&
           !ffxPresentCallbackActive && runtimeOwnsSwapchain && swapchainQueueIsConfirmedPostSLRenderQueue &&
           !deviceRemoved;
}

// DLSS-FG -> FSR-FG (no-callback internal composition) takeover after many switches (synthetic
// dx12_fg_switch_test session 20260615_020100: missed=60 / 422 ms). The native-FSR no-callback takeover
// path ALREADY warm-reinited the overlay on the runtime-owned FSR swapchain queue (the no-callback route
// explicitly ALLOWS normal overlay rendering there — see ShouldRetainNativeFSRInternalNoCallbackComposition...),
// but on the SAME frame the [outer] SL-FG-OFF teardown force-clears overlayInit + CleanupRTVs ("stale SL
// backbuffers") and arms the generic 60-frame reinit cooldown, blanking the just-reinited overlay. The SL
// backbuffers are NOT stale here: FSR took over the same swapchain and the overlay backend is already
// init/sync on that queue with a healthy device, so keep it live (skip the teardown + cooldown). This is
// strictly the no-callback route: the app-callback FFX bridge route (where a separate overlay ECL on the
// FSR queue is the documented 0x887A002B crash) has nativeFSRInternalNoCallbackComposition=false and keeps
// the teardown. The existing pure-Streamline / confirmed-PostSL bypasses deliberately EXCLUDE
// fsrFGApiActive; this one is the FSR-active counterpart, narrowed to the safe no-callback composition.
inline bool ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(bool slTurnedOff, bool fsrFGApiActive,
                                                                   bool nativeFSRInternalNoCallbackComposition,
                                                                   bool runtimeOwnedNativeFGPresentPath,
                                                                   bool overlayInit, bool syncInit,
                                                                   bool deviceRemoved) {
    return slTurnedOff && fsrFGApiActive && nativeFSRInternalNoCallbackComposition && runtimeOwnedNativeFGPresentPath &&
           overlayInit && syncInit && !deviceRemoved;
}

}  // namespace ce::dx12_overlay_policy
