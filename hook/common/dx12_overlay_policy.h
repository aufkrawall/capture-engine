#pragma once

#include <windows.h>

#include <cstdint>

#include <dxgi1_6.h>

#include "fg_runtime_state.h"

struct ID3D12CommandQueue;
struct ID3D12Fence;

namespace ce::dx12_overlay_policy {

enum class PostSLBackbufferBarrierMode {
    kUavBarrierOnly,
    kCommonToRenderTarget,
    kPresentToRenderTarget,
};

inline bool ShouldWaitForOverlayCompletion(bool hasFenceEvent, bool usingDedicatedQueue, bool hasStartupBlockingOverlay,
                                           fg_runtime::RuntimeMode runtimeMode, bool processHasForeground = true) {
    if (!hasFenceEvent) {
        return false;
    }

    if (runtimeMode == fg_runtime::RuntimeMode::kNvidiaSmoothMotion) {
        return false;
    }

    if (usingDedicatedQueue) {
        return true;
    }

    if (!processHasForeground) {
        return true;
    }

    if (!hasStartupBlockingOverlay) {
        return false;
    }

    return runtimeMode == fg_runtime::RuntimeMode::kOff || runtimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG;
}

inline bool ShouldFlushDeferredOverlaySignalAfterPresent(bool isD3D12Swapchain) {
    // The DX12 overlay deliberately queues its fence Signal after Present so the
    // NVIDIA driver does not see Signal wedged between CE's overlay ECL and the
    // game's Present. Every D3D12 Present path that can submit the overlay must
    // therefore flush that deferred Signal after the real Present returns.
    return isD3D12Swapchain;
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
    bool runtimeOwnsSwapchain, bool observedAnyFrameGenerationActivity, bool runtimeOwnershipJustActivated,
    bool preserveLiveOverlayDuringHandoff = false) {
    if (preserveLiveOverlayDuringHandoff) {
        return false;
    }

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
                                               bool runtimeOwnsSwapchain, ULONGLONG runtimeOwnedSwapchainActiveMs = 0,
                                               ULONGLONG runtimeOwnedSwapchainSettleMs = 0,
                                               bool startupCompatAlreadySettled = false,
                                               bool preserveLiveOverlayDuringHandoff = false) {
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

    if (startupCompatAlreadySettled || preserveLiveOverlayDuringHandoff) {
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
                                                     ULONGLONG runtimeOwnedSwapchainActiveMs, ULONGLONG settleDelayMs,
                                                     bool startupCompatAlreadySettled = false,
                                                     bool preserveLiveOverlayDuringHandoff = false) {
    if (!startupOverlayCompatibilityActive || !runtimeOwnsSwapchain) {
        return false;
    }

    if (startupCompatAlreadySettled || preserveLiveOverlayDuringHandoff) {
        return false;
    }

    return runtimeOwnedSwapchainActiveMs < settleDelayMs;
}

inline bool ShouldPrimeStartupOverlayResources(bool startupOverlayCompatibilityActive, bool hasPendingDX12Resources,
                                               bool preserveLiveOverlayDuringHandoff) {
    return startupOverlayCompatibilityActive && hasPendingDX12Resources && !preserveLiveOverlayDuringHandoff;
}

inline bool ShouldDelayAfterStartupOverlayResourcePrime(bool startupOverlayCompatibilityActive,
                                                        bool actualFrameGenerationActive,
                                                        ULONGLONG msSinceResourcePrime, ULONGLONG settleDelayMs,
                                                        bool preserveLiveOverlayDuringHandoff) {
    return startupOverlayCompatibilityActive && !actualFrameGenerationActive &&
           msSinceResourcePrime < settleDelayMs && !preserveLiveOverlayDuringHandoff;
}

inline bool ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
    bool startupCompatAlreadySettled, bool overlayBackendReady, bool runtimeOwnsSwapchain, bool streamlineFGRunning,
    fg_runtime::RuntimeMode runtimeMode, bool explicitSetOptionsActivation, bool observedAnyFrameGenerationActivity,
    bool hasOriginalGameQueue) {
    // A third-party startup overlay can create a Streamline-adjacent no-FG
    // swapchain after CE has already rendered stably on the real game swapchain.
    // That is not yet a DLSS-G activation. Keep the existing single-queue
    // overlay path alive until an explicit FG signal appears instead of
    // blanking the overlay for a speculative runtime-owned handoff.
    return startupCompatAlreadySettled && overlayBackendReady && runtimeOwnsSwapchain && !streamlineFGRunning &&
           runtimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG && !explicitSetOptionsActivation &&
           !observedAnyFrameGenerationActivity && hasOriginalGameQueue;
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

inline bool ShouldSkipDX12PresentProcessingForInvisibleWindowSwapchain(bool hasOutputWindow, bool outputWindowVisible) {
    // Games and overlay/runtime stacks can create helper swapchains on hidden
    // tool windows. Rendering CE's DX12 overlay into those auxiliary Presents
    // can perturb the real swapchain's runtime state, while a hidden window can
    // never display the overlay to the user.
    return hasOutputWindow && !outputWindowVisible;
}

inline bool ShouldHeavySuspendDX12OverlayForSwapchainState(bool zeroSizedSwapchain, bool iconicWindow) {
    // Only drawable-state loss should blank/release the overlay. Transition
    // cooldowns are routing hints; treating them as hard suspension made the
    // overlay disappear during focus and swapchain churn even though the game
    // still had a valid backbuffer.
    return zeroSizedSwapchain || iconicWindow;
}

inline bool ShouldSkipDX12CreateSwapchainSideEffectsForInvisibleWindowSwapchain(bool hasOutputWindow,
                                                                                bool outputWindowVisible) {
    // Hidden helper swapchains must not refresh the global Present hook target,
    // start transition cooldowns, or replace the authoritative swapchain queue.
    // The matching Present path also skips them, but create-time side effects
    // are enough to poison the next visible game Present if we do not filter
    // them here too.
    return hasOutputWindow && !outputWindowVisible;
}

inline bool ShouldSuppressQueueTrackingForCEOverlaySubmission(bool insideCEOverlaySubmission) {
    // CE overlay command lists are bookkeeping side effects, not proof of which
    // queue the game or FG runtime is using. Some runtimes unwrap or forward our
    // submit to an internal queue that re-enters the ECL hook, so accepting it as
    // authoritative can poison the next Present path.
    return insideCEOverlaySubmission;
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

inline bool ShouldSkipGlobalCreateSwapchainForHwndSideEffectsAfterInlineForward(bool inlineHookHandledForwardedCall) {
    // Global IDXGIFactory2 vtable calls can forward into the real DXGI export,
    // which our inline hook also owns. If the inline hook already handled the
    // forwarded CreateSwapChainForHwnd call, the global detour must not replay
    // queue capture, Present repair, transition cooldown, or wrapper decisions
    // for the same returned swapchain.
    return inlineHookHandledForwardedCall;
}

inline bool ShouldTreatSwapchainQueueAsAuthoritativeStreamlineRuntime(bool authoritativeStreamlineRuntimeCreator,
                                                                      bool hasOriginalGameQueue,
                                                                      bool queueMatchesOriginalGameQueue) {
    return authoritativeStreamlineRuntimeCreator && hasOriginalGameQueue && !queueMatchesOriginalGameQueue;
}

inline bool ShouldTreatSwapchainQueueAsAuthoritativeFFXRuntime(bool authoritativeFFXRuntimeCreator,
                                                               bool hasOriginalGameQueue,
                                                               bool queueMatchesOriginalGameQueue) {
    return authoritativeFFXRuntimeCreator && hasOriginalGameQueue && !queueMatchesOriginalGameQueue;
}

inline bool ShouldBypassDXGISwapchainWrapperForFrameGenerationRuntime(
    bool d3d12CommandQueueSwapchain, bool ffxFrameGenerationInStack, bool streamlineFrameGenerationInStack,
    bool streamlineSupportPresent, bool fsrSupportPresent, fg_runtime::RuntimeMode runtimeMode) {
    if (!d3d12CommandQueueSwapchain) {
        return false;
    }

    return ffxFrameGenerationInStack || streamlineFrameGenerationInStack || streamlineSupportPresent ||
           fsrSupportPresent || fg_runtime::RuntimeModeUsesStreamline(runtimeMode) ||
           fg_runtime::RuntimeModeUsesFSR(runtimeMode);
}

inline bool ShouldBypassDXGIFactoryWrapperForFrameGenerationRuntime(bool callerFromFrameGenerationRuntimeModule,
                                                                    bool streamlineSupportPresent,
                                                                    bool fsrSupportPresent,
                                                                    fg_runtime::RuntimeMode runtimeMode) {
    return callerFromFrameGenerationRuntimeModule || streamlineSupportPresent || fsrSupportPresent ||
           fg_runtime::RuntimeModeUsesStreamline(runtimeMode) || fg_runtime::RuntimeModeUsesFSR(runtimeMode);
}

inline bool ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(bool bypassFactoryWrapper,
                                                                    bool callerFromFrameGenerationRuntimeModule,
                                                                    bool liveExportAvailable,
                                                                    bool liveExportFromCaptureHookModule) {
    // FG runtime callers (especially Streamline during startup/DllMain) must get
    // the pristine DXGI export to avoid third-party overlay re-entrancy. App
    // thread handoffs are different: bypass CE's factory wrapper, but call the
    // live export chain so Streamline/other overlays can install their own
    // factory and swapchain interposers before the first runtime-owned Present.
    return bypassFactoryWrapper && !callerFromFrameGenerationRuntimeModule && liveExportAvailable &&
           !liveExportFromCaptureHookModule;
}

inline bool ShouldArmStreamlineStartupTransitionWindowForFreshAuthoritativeRuntimeQueue(
    bool authoritativeStreamlineRuntimeQueue, bool queueMatchesCurrentSwapchainQueue) {
    return authoritativeStreamlineRuntimeQueue && !queueMatchesCurrentSwapchainQueue;
}

inline bool ShouldInvalidateConfirmedPostSLForFreshAuthoritativeStreamlineHandoff(
    bool freshAuthoritativeStreamlineHandoff, bool postSLConfirmedRendering,
    bool newQueueMatchesPreviousSwapchainQueue) {
    // A PostSL confirmation proves only the swapchain/queue epoch it rendered on.
    // GTA can create a fresh runtime-owned Streamline swapchain after pure DLSS
    // has already confirmed on the original queue; carrying that proof forward
    // lets CE touch Streamline's startup path while the new DLSSG queue is still
    // initializing.
    return freshAuthoritativeStreamlineHandoff && postSLConfirmedRendering && !newQueueMatchesPreviousSwapchainQueue;
}

inline bool ShouldClearPostSLQueueProofForFreshAuthoritativeStreamlineHandoff(bool freshAuthoritativeStreamlineHandoff,
                                                                              bool hasPostSLQueueProof,
                                                                              bool queueProofMatchesNewSwapchainQueue) {
    return freshAuthoritativeStreamlineHandoff && hasPostSLQueueProof && !queueProofMatchesNewSwapchainQueue;
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
                                                                    bool authoritativeStreamlineRuntimeQueue,
                                                                    bool authoritativeFFXRuntimeQueue) {
    if (!hasOriginalGameQueue || queueMatchesOriginalGameQueue) {
        return true;
    }

    // If the current authoritative owner is FFX/native FSR, stale Streamline
    // provenance on the same non-origGame queue must not keep its vtable hook
    // alive. The added ECL detour overhead on that runtime-owned queue has been
    // observed to stall the FFX runtime inside ffxQuery.
    if (authoritativeFFXRuntimeQueue) {
        return false;
    }

    // Runtime-owned queues stay unhooked. Native FSR is timing-sensitive, and
    // Streamline can expose wrapped/proxy queues whose vtables are not safe to
    // patch during startup handoff. CE must observe these queues via swapchain,
    // Present, and SDK state instead of mutating their ECL vtables.
    (void)authoritativeStreamlineRuntimeQueue;
    return false;
}

inline bool ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(bool vtableFromStreamlineModule,
                                                                            bool executeFromStreamlineModule,
                                                                            bool vtableFromFFXModule,
                                                                            bool executeFromFFXModule) {
    // Streamline and native FSR expose proxy/runtime-owned command queues whose
    // vtables live inside the SDK runtime DLLs. Patching those vtables can race
    // the runtime's own ECL hooks during swapchain/FG handoff. CE can still track
    // state through Present and SDK hooks, so these queues stay pristine.
    return vtableFromStreamlineModule || executeFromStreamlineModule || vtableFromFFXModule || executeFromFFXModule;
}

inline bool ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
    bool hasOriginalGameQueue, bool queueMatchesOriginalGameQueue, bool streamlineRuntimeAvailable,
    bool hadFSRFGPhase, bool fsrFGApiActive, fg_runtime::RuntimeMode runtimeMode) {
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

inline SwapchainOverlayRoutingDecision DecideSwapchainOverlayRouting(
    bool runtimeOwnsSwapchain, bool streamlineFGActive, bool fsrFGActive, bool hadFSRFGPhase, bool hasSwapchainQueue,
    bool hasOriginalGameQueue, bool hasPostSLLastWorkingQueue,
    bool postSLLastWorkingQueueStillActiveDuringRecentTeardown, bool commandQueueMatchesPrimaryGameQueue,
    bool explicitNativeFSROffPendingRuntimeOwnedTeardown = false,
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

inline bool ShouldStartFrameGenerationTransitionCooldown(fg_runtime::RuntimeMode previousRuntimeMode,
                                                         fg_runtime::RuntimeMode nextRuntimeMode,
                                                         bool previousEffectiveFGActive,
                                                         bool nextEffectiveFGActive,
                                                         bool previousStreamlineFGSignal,
                                                         bool nextStreamlineFGSignal,
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
inline bool IsLiveNoCallbackNativeFSRSuspensionToggle(
    fg_runtime::RuntimeMode previousRuntimeMode, fg_runtime::RuntimeMode nextRuntimeMode, bool streamlineFGRunning,
    bool nativeFSRInternalNoCallbackComposition, bool runtimeOwnsSwapchain, bool hasSwapchainQueue,
    bool overlayBackendInitialized, bool overlayBackendQueueIsSwapchainQueue) {
    if (streamlineFGRunning || !nativeFSRInternalNoCallbackComposition || !runtimeOwnsSwapchain ||
        !hasSwapchainQueue || !overlayBackendInitialized || !overlayBackendQueueIsSwapchainQueue) {
        return false;
    }

    const bool previousIsFSR = previousRuntimeMode == fg_runtime::RuntimeMode::kFSRFG;
    const bool nextIsFSR = nextRuntimeMode == fg_runtime::RuntimeMode::kFSRFG;
    const bool previousIsNonFG = previousRuntimeMode == fg_runtime::RuntimeMode::kOff ||
                                 previousRuntimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG;
    const bool nextIsNonFG = nextRuntimeMode == fg_runtime::RuntimeMode::kOff ||
                             nextRuntimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG;
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
                                           bool runtimeOwnsSwapchain, bool fsrFGApiActive,
                                           bool hasSwapchainQueue, bool overlayBackendInitialized,
                                           bool overlayBackendQueueIsSwapchainQueue) {
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
inline bool ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(bool gameCreatedSwapchain,
                                                                         bool explicitNativeFSROffPending,
                                                                         bool nativeFSRContextsDestroyedPending,
                                                                         bool streamlineFGRunning) {
    return gameCreatedSwapchain && !streamlineFGRunning &&
           (explicitNativeFSROffPending || nativeFSRContextsDestroyedPending);
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
    const bool nextIsNonFG = nextRuntimeMode == fg_runtime::RuntimeMode::kOff ||
                             nextRuntimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG;
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
    bool nativeFSRInternalNoCallbackComposition, bool runtimeOwnsSwapchain,
    bool swapchainQueueIsLiveCommandQueue, bool swapchainQueueIsConfirmedPostSLRenderQueue) {
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
    return slTurnedOff && fsrFGApiActive && nativeFSRInternalNoCallbackComposition &&
           runtimeOwnedNativeFGPresentPath && overlayInit && syncInit && !deviceRemoved;
}

// Round 4 (Talos DLSS-FG toggle-ON, session 20260614_030417: ~437 ms / 4 presents blank). When the
// game toggles DLSS FG ON at runtime, Streamline starts intercepting presents and (with Steam loaded)
// CE routes them through a Steam-safe bypass that returns BEFORE the overlay draw; DLSS-G then freezes
// the present loop ~408 ms, holding that overlay-less frame on screen. RTSS keeps its overlay visible
// by drawing present-time on the game's own present queue every present. CE can do the same: keep the
// already-live pre-SL overlay drawing during the toggle-on window instead of suppressing it. Only safe
// when there is NO separate Streamline queue — i.e. the present swapchain queue is the game's own
// original queue (swapchainQueueIsOriginalGameQueue) — so the overlay ECL lands on the game's queue and
// cannot cause a cross-queue DEVICE_HUNG. Gated to opt-in (eagerEnabled), pure DLSS (no FSR history),
// the runtime not owning the swapchain, and the overlay backend already initialized. This is NOT the
// PostSL re-entrant ECL submitted into DLSS-G's pipeline (the documented init-hang hazard) — it is the
// same plain present-time ECL CE already submits on the no-FG normal route. See guardrails.md (Round 4).
inline bool ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(bool eagerEnabled, bool hadFSRFGPhase,
                                                            bool runtimeOwnsSwapchain, bool overlayInit, bool syncInit,
                                                            bool swapchainQueueIsOriginalGameQueue) {
    return eagerEnabled && !hadFSRFGPhase && !runtimeOwnsSwapchain && overlayInit && syncInit &&
           swapchainQueueIsOriginalGameQueue;
}

// Extended cooldown for post-FSR non-FG recovery.  Streamline's FG teardown
// leaves GPU resources in an indeterminate state; the overlay's first GPU
// submit (offscreen compositing on the original game queue) can trigger
// DEVICE_REMOVED even after the frame-based cooldown expires.  Return a
// generous time-based cooldown (in frames at ~60fps) when SL FG is off.
// When SL FG is running (re-entering DLSS FG), use the standard shorter
// cooldown so the overlay resumes promptly.
inline int ResolvePostFSRExtendedCooldownFrames(bool slFGRunning) {
    return slFGRunning ? 15 : 900;  // ~250ms for FG-on, ~15s for FG-off
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

inline bool DoOverlayFGPublishedTypesDiffer(bool lhsFGActive, fg_runtime::RuntimeMode lhsRuntimeMode, bool rhsFGActive,
                                            fg_runtime::RuntimeMode rhsRuntimeMode) {
    return ResolveOverlayFGMetricType(lhsFGActive, lhsRuntimeMode) !=
           ResolveOverlayFGMetricType(rhsFGActive, rhsRuntimeMode);
}

inline bool IsPostFSRNonFGRecovery(bool hadFSRFGPhase, bool needsOffscreenOverlayAfterPostFSRNonFG, bool actualFGActive,
                                   bool streamlineFGRunning, bool hasSwapchainQueue) {
    return hadFSRFGPhase && needsOffscreenOverlayAfterPostFSRNonFG && !actualFGActive && !streamlineFGRunning &&
           !hasSwapchainQueue;
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

inline bool ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(bool postFSRNonFGRecovery, bool recentStreamlineTeardown,
                                                               bool postSLRecentTeardownActivity) {
    return ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(
        postFSRNonFGRecovery, recentStreamlineTeardown, postSLRecentTeardownActivity);
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

inline bool ShouldResetBlockedECLPatternHeuristicEvidence(bool canUseFSRFGHeuristics, bool eclPatternHeuristicDetected,
                                                          bool hasRealFrameEvidence,
                                                          bool hasInterpolatedFrameEvidence) {
    // ECL-pattern evidence collected while FSR heuristics are blocked is stale.
    // DLSS/Streamline worker traffic can accumulate a valid-looking real/interp
    // mix that later re-fires as a false FSR activation once the block lifts.
    return !canUseFSRFGHeuristics &&
           (eclPatternHeuristicDetected || hasRealFrameEvidence || hasInterpolatedFrameEvidence);
}

inline bool ShouldSkipProcessFrameForZeroECLPresent(bool isInterpolatedFrame, bool hasDedicatedQueue,
                                                    bool heuristicFSRFG, bool runtimeOwnsSwapchain,
                                                    bool streamlineFGRunning, bool recentStreamlineTeardown,
                                                    bool postFSRNonFGRecovery,
                                                    ce::fg_runtime::RuntimeMode runtimeMode,
                                                    bool liveSwapchainQueueIsGameRecoveryQueue = false,
                                                    bool fgTransitionCooldownActive = false) {
    if (!isInterpolatedFrame) {
        return false;
    }

    if (hasDedicatedQueue || heuristicFSRFG) {
        return false;
    }

    // The FG transition cooldown counts down in ProcessFrame. If zero-ECL
    // classification starves ProcessFrame while a cooldown is armed (e.g. the
    // game retired its original render queue when switching FG modes, so no
    // present ever counts ECLs again), the cooldown can never complete and
    // PostSL/pre-SL rendering stays disabled forever (20260612_002523:
    // overlay never came back after all-FG-off -> DLSS FG). Armed cooldowns
    // must always be allowed to tick.
    if (fgTransitionCooldownActive) {
        return false;
    }

    // FSR/runtime-owned swapchain transitions can temporarily stop feeding
    // authoritative ECL counts even though top-level Presents are still the
    // frames that must drive normal ProcessFrame recovery.
    if (runtimeOwnsSwapchain && !streamlineFGRunning) {
        return false;
    }

    // The live swapchain is the game-created recovery swapchain after an
    // explicit native-FSR OFF/destroy. Its presents are real game frames by
    // construction even when ECL classification has not caught up with the
    // game's recreated queue; skipping them starves ProcessFrame forever
    // (20260612_000936: overlay never came back after FSR->off).
    if (liveSwapchainQueueIsGameRecoveryQueue) {
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
                                                                             bool runtimeOwnsSwapchain,
                                                                             bool runtimeOwnedNativeFGPresentPath) {
    if ((!actualFGActive && !runtimeOwnedNativeFGPresentPath) || streamlineFGRunning) {
        return false;
    }

    return fsrFGActive || runtimeOwnsSwapchain || runtimeOwnedNativeFGPresentPath;
}

inline bool ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
    bool runtimeOwnsSwapchain, bool streamlineFGRunning, fg_runtime::RuntimeMode runtimeMode,
    bool authoritativeFSRActive, bool runtimeOwnedNativeFGPresentPath,
    bool ffxPresentCallbackFallbackAllowed = false, bool nativeFSRInternalNoCallbackComposition = false) {
    if (streamlineFGRunning) {
        return false;
    }

    const bool nativeFSRPresentOwnership =
        authoritativeFSRActive || fg_runtime::RuntimeModeUsesFSR(runtimeMode) || runtimeOwnedNativeFGPresentPath;
    if (!nativeFSRPresentOwnership) {
        return false;
    }

    // If the game did not provide an FFX present callback, the AMD swapchain
    // runtime keeps its own internal blit/UI composition path. Installing CE's
    // callback in that configuration forces us to own the runtime's scene copy,
    // which has wedged real AMD presenter threads. Let the internal no-callback
    // path finish the frame and draw the overlay through the normal DX12 route
    // on the runtime-owned swapchain queue.
    if (nativeFSRInternalNoCallbackComposition) {
        return false;
    }

    // Native/runtime-owned FSR is stricter than the generic runtime-owned
    // non-FSR windows. Once official/native FSR is authoritative, CE must keep
    // overlay rendering on the runtime-cooperative present callback path even
    // if the later swapchain-ownership latch has not fired. GTA Enhanced can
    // expose exactly that shape: enabled ffxConfigure + live callback renders
    // with runtimeOwnsSwapchain still false, followed by device removal on the
    // first separate injected overlay ECL.
    (void)runtimeOwnsSwapchain;
    return !ffxPresentCallbackFallbackAllowed;
}

inline bool ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(bool runtimeOwnsSwapchain,
                                                                             bool streamlineFGRunning,
                                                                             fg_runtime::RuntimeMode runtimeMode,
                                                                             uint32_t observedPresentCount,
                                                                             uint32_t requiredSettlePresents) {
    (void)runtimeOwnsSwapchain;
    (void)streamlineFGRunning;
    (void)runtimeMode;
    (void)observedPresentCount;
    (void)requiredSettlePresents;
    // A visible Streamline-owned no-FG swapchain is still ordinary game
    // presentation and should get the overlay immediately. Hidden helper
    // swapchains are filtered at the top-level DXGI Present detour before any
    // CE state mutation, so no additional visible-swapchain settle blackout is
    // needed here.
    return false;
}

inline bool ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(bool runtimeOwnsSwapchain,
                                                                       bool streamlineFGRunning,
                                                                       fg_runtime::RuntimeMode runtimeMode,
                                                                       uint32_t observedPresentCount,
                                                                       uint32_t requiredSettlePresents) {
    return ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
        runtimeOwnsSwapchain, streamlineFGRunning, runtimeMode, observedPresentCount, requiredSettlePresents);
}

inline bool ShouldEvaluateFFXPresentCallbackFallback(bool ffxPresentCallbackStalled,
                                                      bool explicitNativeFSROffPendingRuntimeOwnedTeardown) {
    (void)explicitNativeFSROffPendingRuntimeOwnedTeardown;
    return ffxPresentCallbackStalled;
}

inline bool ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
    bool evaluateFFXPresentCallbackFallback, bool progressResolvedOfficialFFXPresentPath, bool directFFXApiConfirmation,
    bool currentFFXPresentCallbackProof, bool progressResolvedStableOverlayProof = false,
    ULONGLONG stallDurationMs = 0, bool explicitNativeFSROffPendingRuntimeOwnedTeardown = false) {
    (void)stallDurationMs;
    (void)progressResolvedOfficialFFXPresentPath;
    (void)progressResolvedStableOverlayProof;

    if (!evaluateFFXPresentCallbackFallback) {
        return false;
    }

    // When a game explicitly disables native FSR FG while keeping the FFX
    // context/callback bridge alive (GTA menu/suspend path), the runtime-owned
    // presentation path can still reject unexpected normal DX12 overlay
    // submissions. Keep drawing through the retained FFX callback until
    // ownership actually unwinds instead of using callback proof as permission
    // to wake the separate normal overlay path.
    if (explicitNativeFSROffPendingRuntimeOwnedTeardown) {
        return false;
    }

    // The normal DX12 overlay path is only safe after direct configure proof or
    // a current FFX callback has proven that the runtime has accepted CE's
    // callback bridge. Progress-only or same-queue evidence can be misleading:
    // GTA freeze dumps showed AMD presenter threads blocked in ffxQuery after
    // CE resumed normal overlay work from that weaker proof.
    if (!directFFXApiConfirmation && !currentFFXPresentCallbackProof) {
        return false;
    }

    return true;
}

inline bool IsFFXPresentCallbackProofCurrent(ULONGLONG lastCallbackTickMs, ULONGLONG swapchainQueueCaptureTimeMs,
                                              ULONGLONG progressAssumedSinceMs) {
    if (lastCallbackTickMs == 0) {
        return false;
    }
    ULONGLONG proofSinceMs = swapchainQueueCaptureTimeMs;
    if (progressAssumedSinceMs > proofSinceMs) {
        proofSinceMs = progressAssumedSinceMs;
    }
    return proofSinceMs == 0 || lastCallbackTickMs >= proofSinceMs;
}

inline bool ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(bool recognizedFrameGenerationConfigure,
                                                                     bool frameGenerationEnabled,
                                                                     bool hasExistingBridge,
                                                                     bool disabledStartupArmingConfigure) {
    // Disabled startup-arming packets before the first enabled configure must
    // stay unmodified. Once an enabled configure already installed the bridge,
    // though, later OFF/suspend configures should keep that same bridge alive
    // so menu/loading suspensions still render overlay through FFX's safe
    // callback point instead of falling back to normal DX12 overlay submits.
    return recognizedFrameGenerationConfigure && !frameGenerationEnabled && hasExistingBridge &&
           !disabledStartupArmingConfigure;
}

// FFX present-callback toggle wedge (synthetic dx12_fg_switch_test session 20260615_021242: ~1s
// AMD ffxQuery freeze). The app provided a present callback (CE wrapped it with its bridge), then
// re-enables FSR with a NULL callback (fsr_present_callback_toggle_stress / a game toggling its
// callback). AMD RETAINS CE's bridge across this toggle — it does NOT revert to internal composition —
// so CE's bridge keeps being called. The old code CLEARED the bridge's retained original here, leaving
// CE's bridge with no delegate; it then self-composed currentBackBuffer->output via CopyResource on
// AMD's command list, which wedges AMD's presenter (spin in ffxQuery / RtlQueryPerformanceCounter).
// Instead, when an installed bridge still has a non-null original to delegate to, KEEP the bridge
// delegating to that retained callback (the correct composition) across the enabled null-callback
// toggle. Only the genuine no-original case falls back to clear/self-compose. This is distinct from the
// disabled-configure retain above (that is for FG turning OFF; this is FG staying ENABLED with a
// dropped app callback).
inline bool ShouldRetainFFXPresentCallbackBridgeForEnabledNullCallbackToggle(
    bool recognizedFrameGenerationConfigure, bool frameGenerationEnabled, bool appPresentCallbackProvided,
    bool hasExistingBridgeWithOriginal) {
    return recognizedFrameGenerationConfigure && frameGenerationEnabled && !appPresentCallbackProvided &&
           hasExistingBridgeWithOriginal;
}

inline bool ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
    bool runtimeOwnedNativeFGPresentPath, bool nativeFSRActive, bool ffxPresentCallbackStalled,
    bool ffxPresentCallbackStallAllowsNormalOverlay) {
    // The timeout is a visibility backstop for ordinary transition stalls, but
    // it must not overrule native/runtime-owned FSR.  When the FFX present
    // callback is healthy, the overlay is already rendered on the runtime-owned
    // path; waking the separate normal DX12 overlay path can submit an
    // additional command list into the FSR-owned queue and remove the device.
    if (!runtimeOwnedNativeFGPresentPath && !nativeFSRActive) {
        return true;
    }
    if (!ffxPresentCallbackStalled) {
        return false;
    }
    return ffxPresentCallbackStallAllowsNormalOverlay;
}

inline bool ShouldProbePostSLStartupActivationSwapchainFromECL(bool activationPending, bool callbackInstalled,
                                                               bool postSLConfirmedRendering,
                                                               bool runtimeOwnedNativeFGPresentPath,
                                                               bool nativeFSRActive) {
    if (!activationPending || !callbackInstalled || postSLConfirmedRendering) {
        return false;
    }

    // The ECL startup probe exists only to unstick Streamline/PostSL startup.
    // During native FSR ownership or official FFX startup/takeover, retained
    // Streamline swapchains may already be stale.  Probing them by AddRef/
    // Release can trip a CRT _purecall before the policy check gets to reject
    // the activation path.
    if (runtimeOwnedNativeFGPresentPath || nativeFSRActive) {
        return false;
    }

    return true;
}

inline bool ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(bool queueMatchesOriginalGameQueue,
                                                                       bool explicitNativeFSROffPending,
                                                                       bool authoritativeFSRActive,
                                                                       fg_runtime::RuntimeMode runtimeMode,
                                                                       bool runtimeOwnedNativeFGPresentPath) {
    // An explicit native-FSR OFF configure is the stronger signal we were
    // waiting for. Once the live swapchain queue has returned to the original
    // game queue and no FSR runtime state remains active, a stale
    // runtimeOwnedNativeFGPresentPath latch must not keep CE classified as
    // runtime-owned FSR forever.
    return queueMatchesOriginalGameQueue && explicitNativeFSROffPending && runtimeOwnedNativeFGPresentPath &&
           !authoritativeFSRActive && !fg_runtime::RuntimeModeUsesFSR(runtimeMode);
}

inline bool ShouldTreatNativeFSRDisabledConfigureAsStartupArming(bool recognizedFrameGenerationConfigure,
                                                                 bool frameGenerationEnabled, bool startupArmingPending,
                                                                 bool runtimeOwnsSwapchain, bool authoritativeFSRActive,
                                                                 bool hasDirectFFXApiConfirmation) {
    // GTA can create the native FSR runtime-owned swapchain first, then send an
    // initial disabled ffxConfigure packet while the real enable path is still
    // arming. Treat that packet as setup, not as an explicit user OFF signal,
    // until the current FFX takeover has produced direct enabled API proof.
    // Official AMD FFX runtimes can fail fast if CE marks the takeover fully
    // active before the runtime accepts the enabled configure. In that staged
    // window `authoritativeFSRActive` is intentionally still false, so the
    // arming latch is the authority for treating the first disabled packet as
    // setup instead of user-requested OFF.
    (void)authoritativeFSRActive;
    return recognizedFrameGenerationConfigure && !frameGenerationEnabled && startupArmingPending &&
           runtimeOwnsSwapchain && !hasDirectFFXApiConfirmation;
}

inline bool ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(bool runtimeOwnsSwapchain,
                                                                               bool runtimeOwnedNativeFGPresentPath,
                                                                               bool retainedPresentCallbackBridge,
                                                                               bool hasDirectFFXApiConfirmation) {
    // A disabled configure can be a transient FSR suspension packet while the
    // official runtime still owns presentation through its present callback.
    // Preserve the native-FG Present ownership latch whenever the stronger
    // callback/progress proof, an already-installed callback bridge, or prior
    // direct enabled FFX API confirmation says that path is still in charge.
    // A cold disabled setup packet on a Streamline/no-FG runtime-owned
    // swapchain is not enough proof by itself; treating it as FSR teardown
    // hides the normal overlay path before FSR ever enabled.
    return runtimeOwnedNativeFGPresentPath || retainedPresentCallbackBridge ||
           (runtimeOwnsSwapchain && hasDirectFFXApiConfirmation);
}

inline bool ShouldInstallFFXPresentCallbackBridgeForConfigure(bool recognizedFrameGenerationConfigure,
                                                              bool frameGenerationEnabled,
                                                              bool presentCallbackAvailable = true) {
    // Disabled configure traffic can repeat rapidly during native-FSR teardown.
    // Installing a new CE present-callback bridge on those OFF packets keeps CE
    // entangled in the old runtime-owned Present path and adds log churn even
    // though FFX is explicitly disabling frame generation.
    // Fresh startup-arming disabled packets are also forwarded unmodified. GTA
    // can fail-fast if a disabled setup configure receives a synthetic callback
    // pointer before native FSR has accepted the real enabled configure.
    return recognizedFrameGenerationConfigure && frameGenerationEnabled && presentCallbackAvailable;
}

inline bool ShouldResetFFXPresentCallbackOverlayBackend(bool backendInitialized, bool deviceChanged,
                                                        bool formatChanged) {
    return backendInitialized && (deviceChanged || formatChanged);
}

inline bool ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(bool callbackBackendInitialized,
                                                                              bool runtimeOwnedNativeFGPresentPath) {
    // A temporary native-FSR suspension can make the normal pre-SL overlay path
    // rebuild its own sync state while the runtime-owned FFX Present path still
    // owns presentation. Tearing down the dedicated callback backend in that
    // window forces a fresh callback-backend re-init on resume. Talos already
    // proves the backend can safely stay warm across those transient
    // suspensions, and GTA resume stability depends on avoiding that extra
    // callback-path churn.
    return callbackBackendInitialized && runtimeOwnedNativeFGPresentPath;
}

inline bool ShouldBridgeOverlayViaFFXPresentCallback(bool runtimeOwnedNativeFGPresentPath,
                                                     bool authoritativeFSRActive,
                                                     bool hasDirectFFXApiConfirmation,
                                                     fg_runtime::RuntimeMode runtimeMode) {
    // The FFX present callback is the safest overlay injection point whenever
    // the official FFX runtime explicitly hands us a composition callback. Some
    // integrations expose that callback without also tripping the separate
    // runtime-owned-swapchain detector, so direct FFX/FSR evidence is enough.
    return runtimeOwnedNativeFGPresentPath || authoritativeFSRActive || hasDirectFFXApiConfirmation ||
           fg_runtime::RuntimeModeUsesFSR(runtimeMode);
}

inline bool ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(bool generatedFrame,
                                                                     bool currentBackBufferAvailable,
                                                                     bool outputBackBufferAvailable,
                                                                     bool currentDiffersFromOutput,
                                                                     bool nativeFSRSuspended) {
    // During native-FSR suspension/menu frames some integrations keep invoking
    // the FFX present callback, but present the current game backbuffer instead
    // of the callback output buffer. Do not mirror active native-FSR frames:
    // the current backbuffer is an input owned by the runtime, and touching it
    // from the callback can deadlock real presenter threads during mode
    // switches. Suspension is the only time the current buffer is treated as a
    // visible target.
    return nativeFSRSuspended && !generatedFrame && currentBackBufferAvailable && outputBackBufferAvailable &&
           currentDiffersFromOutput;
}

inline bool ShouldTreatFormatAsDefinitelyHDR(int dxgiFormat) {
    return dxgiFormat == static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
}

inline bool ShouldProbeDisplayColorSpaceForHDR(int dxgiFormat) {
    return dxgiFormat == static_cast<int>(DXGI_FORMAT_R10G10B10A2_UNORM);
}

inline bool IsHDRColorSpace(int colorSpace) {
    switch (colorSpace) {
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
        case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
        case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
            return true;
        default:
            return false;
    }
}

inline bool ResolveActualHDRStateForOverlayTarget(int dxgiFormat, bool hasDisplayColorSpace, int colorSpace) {
    if (ShouldTreatFormatAsDefinitelyHDR(dxgiFormat)) {
        return true;
    }

    if (!ShouldProbeDisplayColorSpaceForHDR(dxgiFormat)) {
        return false;
    }

    return hasDisplayColorSpace && IsHDRColorSpace(colorSpace);
}

inline bool ResolveRuntimeOwnedCallbackHDRStateFromCachedState(int dxgiFormat, bool hasCachedHDRState,
                                                               bool cachedHDRState) {
    if (ShouldTreatFormatAsDefinitelyHDR(dxgiFormat)) {
        return true;
    }

    if (!ShouldProbeDisplayColorSpaceForHDR(dxgiFormat)) {
        return false;
    }

    return hasCachedHDRState && cachedHDRState;
}

inline bool ShouldComposeFFXPresentSourceToOutput(bool originalPresentCallbackAvailable, bool hasCurrentBackBuffer,
                                                  bool outputDiffersFromCurrent) {
    // Installing CE's FFX present callback makes CE responsible for the same
    // finalization step the app/default callback would otherwise perform:
    // currentBackBuffer (rendered or generated, per the SDK contract) must be
    // composed into outputSwapChainBuffer before UI/overlay rendering. Skip it
    // only when a real callback already ran or the resources are identical.
    return !originalPresentCallbackAvailable && hasCurrentBackBuffer && outputDiffersFromCurrent;
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

inline bool ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(bool streamlineFGRunning, bool runtimeOwnsSwapchain,
                                                                   fg_runtime::RuntimeMode runtimeMode,
                                                                   bool hasOriginalGameQueue,
                                                                   bool commandQueueUsesOriginalGameQueue,
                                                                   bool isInterpolatedFrame) {
    // A late Streamline-owned startup handoff can create a runtime-owned
    // swapchain/queue that never becomes the live non-FG Present path. If top-
    // level real-frame rendering keeps running on the original game queue while
    // Streamline never re-activates, the latched runtime-owned ownership and
    // captured runtime queue are stale and poison later startup/non-FG routing.
    return !streamlineFGRunning && runtimeOwnsSwapchain && runtimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG &&
           hasOriginalGameQueue && commandQueueUsesOriginalGameQueue && !isInterpolatedFrame;
}

inline bool ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(int realFrameRunLength) {
    // Require a sustained run so short queue/Present ownership wobble during a
    // genuine startup handoff does not collapse runtime-owned state too early.
    return realFrameRunLength >= 120;
}

inline bool ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(bool retainedSwapchainAvailable,
                                                                                 bool staleNoFGCleanupActive) {
    return retainedSwapchainAvailable && staleNoFGCleanupActive;
}

inline bool ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(
    bool authoritativeFFXRuntimeCreator, bool retainedSwapchainAvailable) {
    // A retained Streamline startup swapchain is an owned CE AddRef used only to
    // wake PostSL during DLSS startup. Once AMD FFX/FSR is authoritatively
    // creating a swapchain for the same window, that retained reference becomes
    // stale and can prevent DXGI from allowing the FSR runtime to take ownership
    // of the HWND.
    return authoritativeFFXRuntimeCreator && retainedSwapchainAvailable;
}

inline bool ShouldShutdownDescFreeBackendViaOverlayAdapter(bool descFreeBackendPresent, bool adapterInitialized,
                                                           bool adapterBackendMatchesDescFree) {
    return descFreeBackendPresent && adapterInitialized && adapterBackendMatchesDescFree;
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

inline bool ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
    bool runtimeOwnsSwapchain, bool streamlineStartupHandoffPending, fg_runtime::RuntimeMode runtimeMode) {
    // A fresh authoritative Streamline startup handoff can move presentation onto
    // a runtime-owned queue while Streamline still reports Off/NoFG. Treating that
    // queue change as heuristic FSR FG poisons the later DLSS comeback before any
    // real FSR proof exists.
    const bool runtimeStillInactive =
        runtimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG || runtimeMode == fg_runtime::RuntimeMode::kOff;
    return runtimeOwnsSwapchain && streamlineStartupHandoffPending && runtimeStillInactive;
}

inline bool ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(bool canUseFSRHeuristics, bool runtimeOwnsSwapchain,
                                                                    bool hadFSRFGPhase,
                                                                    bool streamlineStartupHandoffPending) {
    // Preserve heuristic FSR only for transient runtime-owned teardown windows
    // that still plausibly belong to native FSR. A fresh authoritative
    // Streamline startup handoff is the opposite case: preserving stale
    // heuristic FSR there lets the later DLSS comeback bypass the repo's own
    // unsafe-GetState startup guards.
    return !canUseFSRHeuristics && runtimeOwnsSwapchain && hadFSRFGPhase && !streamlineStartupHandoffPending;
}

inline bool ShouldPreserveRuntimeOwnedFSRTeardown(bool targetIsNone, bool hadFSRFGPhase, bool runtimeOwnsSwapchain,
                                                  bool streamlineFGRunning) {
    return targetIsNone && hadFSRFGPhase && runtimeOwnsSwapchain && !streamlineFGRunning;
}

inline bool ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(bool explicitNativeFSROffPending,
                                                                bool runtimeOwnsSwapchain) {
    // A real native-FSR off signal can arrive before the runtime-owned swapchain
    // and queue topology have unwound back to the normal non-FG path. In that
    // teardown window, queue-change/ECL heuristics must not immediately relatch
    // FSR_FG or the overlay status flips back to stale FSR even though the
    // runtime already disabled frame generation explicitly.
    (void)runtimeOwnsSwapchain;
    return explicitNativeFSROffPending;
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

inline bool ShouldUnregisterSwapchainDestructionCallbackDuringWrapperDestructor(bool wrapperReleasing,
                                                                                bool realSwapchainAvailable,
                                                                                bool hasDestructionCookie) {
    // Once wrapper Release() has reached zero external refs, the destructor is
    // already on the real swapchain teardown path. Optional DXGI mutation such
    // as notifier unregister can trip DXGI internal debug breakpoints in games
    // that combine Streamline/FFX/third-party overlays during startup. Release
    // our COM refs, but do not poke optional side channels in that edge.
    return !wrapperReleasing && realSwapchainAvailable && hasDestructionCookie;
}

inline bool ShouldClearSwapchainWrapperPrivateDataDuringWrapperDestructor(bool wrapperReleasing,
                                                                          bool realSwapchainAvailable) {
    // Clearing private data is only a hygiene step. During final wrapper
    // release the real swapchain's private-data table may already be inside
    // DXGI teardown, so avoid a crash-prone SetPrivateData call there.
    return !wrapperReleasing && realSwapchainAvailable;
}

inline bool ShouldStartDX12FocusLossOverlayCooldown(bool previousGameForeground, bool currentGameForeground) {
    (void)previousGameForeground;
    (void)currentGameForeground;
    // Focus ownership changes must not blank the injected DX12 overlay. The
    // renderer has per-frame allocator/fence guards now, so ordinary Alt+Tab
    // should keep drawing like third-party overlays instead of hiding for a
    // timed cooldown.
    return false;
}

inline bool ShouldKeepDX12FocusLossOverlayCooldown(bool cooldownTimerActive, bool currentGameForeground) {
    (void)cooldownTimerActive;
    (void)currentGameForeground;
    return false;
}

struct D3D12DeferredOverlaySignalFlushInfo {
    bool hadDeferredSignal = false;
    bool hasFence = false;
    bool hasFenceEvent = false;
    bool signalSucceeded = false;
    HRESULT signalHr = S_OK;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    UINT64 completedValue = 0;
};

struct D3D12FocusLossOverlayFenceWaitContext {
    const char* presentName = nullptr;
    int callCount = 0;
    bool isD3D12Swapchain = false;
    bool isFullscreen = false;
    bool processHasForeground = true;
    bool isIconic = false;
    bool hasZeroSize = false;
    bool presentSucceeded = false;
    bool presentDeviceLost = false;
    bool frameGenerationActive = false;
    bool runtimeOwnedPresentation = false;
    bool usingDedicatedQueue = false;
    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    HWND gameWindow = nullptr;
    DWORD processId = 0;
    UINT syncInterval = 0;
    UINT presentFlags = 0;
    HRESULT presentHr = S_OK;
};

inline bool ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
    bool isD3D12Swapchain, bool isFullscreen, bool processHasForeground, bool isIconic, bool hasZeroSize,
    bool presentSucceeded, bool presentDeviceLost, bool frameGenerationActive, bool runtimeOwnedPresentation,
    bool usingDedicatedQueue, bool hadDeferredOverlaySignal, bool signalSucceeded, bool hasFence, bool hasFenceEvent,
    UINT64 fenceValue) {
    return isD3D12Swapchain && !isFullscreen && !processHasForeground && !isIconic && !hasZeroSize &&
           presentSucceeded && !presentDeviceLost && !frameGenerationActive && !runtimeOwnedPresentation &&
           !usingDedicatedQueue && hadDeferredOverlaySignal && signalSucceeded && hasFence && hasFenceEvent &&
           fenceValue != 0;
}

inline bool ShouldSignalD3D12FocusLossOverlayFenceImmediately(
    bool isWrappedD3D12Present, bool isFullscreen, bool processHasForeground, bool isIconic, bool hasZeroSize,
    bool overlaySubmitSucceeded, bool deviceLost, bool frameGenerationActive, bool runtimeOwnedPresentation,
    bool usingDedicatedQueue, bool steamDeferredOverlaySubmit, bool hasFence, bool hasFenceEvent, bool hasQueue,
    UINT64 fenceValue) {
    return isWrappedD3D12Present && !isFullscreen && !processHasForeground && !isIconic && !hasZeroSize &&
           overlaySubmitSucceeded && !deviceLost && !frameGenerationActive && !runtimeOwnedPresentation &&
           !usingDedicatedQueue && !steamDeferredOverlaySubmit && hasFence && hasFenceEvent && hasQueue &&
           fenceValue != 0;
}

inline bool ShouldWaitForD3D12FocusLossImmediateOverlayFence(bool immediateFencePolicyAccepted,
                                                             bool signalSucceeded, bool hasFence,
                                                             bool hasFenceEvent, bool hasQueue, UINT64 fenceValue) {
    return immediateFencePolicyAccepted && signalSucceeded && hasFence && hasFenceEvent && hasQueue &&
           fenceValue != 0;
}

inline bool ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(bool fenceWaitCompleted,
                                                                          bool dumpAlreadyRequested) {
    return !fenceWaitCompleted && !dumpAlreadyRequested;
}

// --- DescFree overlay UPLOAD-ring per-slot GPU-completion guard ---------------
//
// The DX12 DescFree overlay backend round-robins through a small pool of
// persistently-mapped UPLOAD vertex/index buffers.  The CPU must not memcpy new
// geometry into a ring slot while the GPU is still reading the previous frame's
// geometry from that same slot.  While the GPU keeps up this never happens, but
// during the iflip<->composited mode switch triggered by Alt+Tab the GPU stops
// retiring for hundreds of ms while the CPU keeps drawing the overlay at full
// rate; within poolSize frames the CPU wraps and overwrites in-flight data,
// corrupting the draw and wedging the GPU (DXGI_ERROR_DEVICE_HUNG / 2s TDR,
// observed x86/WoW64 only, captured via DRED + NtGdiDdDDICreateAllocation).
//
// DecideOverlayUploadSlotGuardValue() returns the overlay-fence value the GPU
// must reach before the slot used this frame may be reused, or 0 to disable the
// guard.  ShouldWaitForOverlayUploadSlot() decides whether the CPU must block
// before reusing a slot, given that slot's recorded guard and the fence's
// current completed value.  The fence is the real synchronization; pacing the
// CPU to the GPU here keeps the overlay visible every frame (never hidden) while
// preventing the upload-ring data race.
inline uint64_t DecideOverlayUploadSlotGuardValue(bool fgActive, bool hasOverlayFence, uint64_t currentFenceValue) {
    // FG paths advance a separate completion fence (not the overlay fence) and
    // already synchronize per frame, so a guard keyed on the overlay fence would
    // never be reached there -> disable it.  Without an overlay fence there is
    // nothing to wait on.
    if (fgActive || !hasOverlayFence) {
        return 0;
    }
    return currentFenceValue + 1;
}

inline bool ShouldWaitForOverlayUploadSlot(uint64_t slotGuardFenceValue, uint64_t gpuCompletedFenceValue) {
    return slotGuardFenceValue != 0 && gpuCompletedFenceValue < slotGuardFenceValue;
}

inline bool ShouldRecordDescFreeFontUpload(bool uploadPending, bool hasDefaultFontBuffer, bool hasUploadBuffer) {
    return uploadPending && hasDefaultFontBuffer && hasUploadBuffer;
}

inline bool ShouldUseTextureDx12OverlayBackendForProcess(bool is32BitProcess) {
    // Keep x86 on the standard native DX12 backend so the 32-bit path no longer
    // uses DescFree's separate root-SRV text path.  Text itself is emitted as
    // solid glyph spans by ShouldUseSolidDx12TextGeometryForProcess().
    return is32BitProcess;
}

inline bool ShouldUseSolidDx12TextGeometryForProcess(bool is32BitProcess) {
    // x86/WoW64 NVIDIA can hang on native DX12 overlay text draws that sample a
    // CE-owned font resource.  Preserve the native overlay and direct backbuffer
    // path, but encode glyph coverage as solid alpha geometry so text uses the
    // same proven solid PSO path as rectangles/graphs.
    return is32BitProcess;
}

// v8 visibility-gated hold (supersedes the v3-v7 focus-based holds and the
// focus-loss offscreen composite).
//
// CE holds its swapchain backbuffer overlay/capture GPU work ONLY when the
// swapchain is genuinely not presentable: DXGI Present returned OCCLUDED, or the
// window is minimized (iconic), or the swapchain is zero-sized. In every one of
// those states the overlay is invisible to the user anyway, and it is the state
// in which the single-monitor Alt+Tab device-hung historically occurred (DXGI
// tears down the independent-flip surfaces). A merely-unfocused but STILL-VISIBLE
// window (e.g. a borderless background window, or a window on another monitor)
// keeps presenting S_OK and MUST keep rendering the overlay directly to the
// backbuffer — that is the behavior a proper inject overlay provides and what the user expects.
// Focus is NOT an input here: losing focus never hides the overlay.
inline bool ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
    bool isWrappedD3D12Present, bool isFullscreen, bool isOccluded, bool isIconic, bool hasZeroSize,
    bool frameGenerationActive, bool runtimeOwnedPresentation, bool usingDedicatedQueue,
    bool steamDeferredOverlaySubmit, bool deviceLost, bool hasQueue) {
    const bool notPresentable = isOccluded || isIconic || hasZeroSize;
    return isWrappedD3D12Present && !isFullscreen && notPresentable && !frameGenerationActive &&
           !runtimeOwnedPresentation && !usingDedicatedQueue && !steamDeferredOverlaySubmit && !deviceLost && hasQueue;
}

inline bool ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(bool deviceLost,
                                                                           bool focusTransitionRecentlyActive,
                                                                           bool dumpAlreadyRequested) {
    return deviceLost && focusTransitionRecentlyActive && !dumpAlreadyRequested;
}

// v10 focus-transition backbuffer-work hold.
//
// DRED proved the x86 DX12 Alt+Tab device-hung is a PURE GPU hang (pageFaultVA=0)
// inside CE's OWN overlay command list, and that it happens for ANY backbuffer
// touch during the iflip<->composited mode switch around a focus change:
//   - v8 direct draw: hung on DRAWINDEXEDINSTANCED (logs/20260603_020053).
//   - v9 offscreen composite: hung on the bb->offscreen COPYTEXTUREREGION
//     (logs/20260603_150241) — the offscreen path still reads the live backbuffer.
// The hangs were observed at the refocus edge (composited->iflip). So during the
// brief transition window after a foreground-change edge, CE must not touch the
// swapchain backbuffer AT ALL (no draw, no copy) — this matches v7, which never
// hung. Unlike v7, the hold is armed only by the focus-change EDGE and clears after
// the mode switch settles, so steady states (focused AND unfocused-but-visible)
// render directly, exactly like a lightweight inject overlay, and the overlay is only briefly absent during
// the actual mode switch (when the screen is transitioning anyway). FG /
// runtime-owned / dedicated-queue / Steam-deferred routes manage their own
// submission and are excluded. `transitionHoldFramesRemaining` is the edge-armed
// per-Present countdown (see g_FocusTransitionHoldFrames).
inline bool ShouldHoldD3D12OverlayBackbufferWorkDuringFocusTransition(bool isWindowed,
                                                                      int transitionHoldFramesRemaining,
                                                                      bool frameGenerationActive,
                                                                      bool runtimeOwnedPresentation,
                                                                      bool usingDedicatedQueue,
                                                                      bool steamDeferredOverlaySubmit,
                                                                      bool deviceLost, bool hasQueue) {
    return isWindowed && transitionHoldFramesRemaining > 0 && !frameGenerationActive && !runtimeOwnedPresentation &&
           !usingDedicatedQueue && !steamDeferredOverlaySubmit && !deviceLost && hasQueue;
}

inline bool ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(bool processHasForeground,
                                                               bool hasPendingFocusLossFence,
                                                               bool pendingFenceComplete) {
    return !processHasForeground && hasPendingFocusLossFence && !pendingFenceComplete;
}

inline bool ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(bool processHasForeground,
                                                                  bool hasPendingFocusLossFence,
                                                                  bool pendingFenceComplete) {
    return ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(processHasForeground, hasPendingFocusLossFence,
                                                              pendingFenceComplete);
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
        //
        // However, multi-queue games (e.g. Talos Principle) use separate DIRECT
        // queues for ECL (render) and Present (swapchain).  The primary game queue
        // is the first DIRECT queue observed and is always a valid non-wrapper queue.
        // If the command queue has already settled back to the primary queue, that
        // queue is proven safe even when the preserved PostSL last-working queue was
        // cleared by an earlier failed comeback.  Treating it as "unsettled" strands
        // the overlay for the entire grace period.
        if (commandQueueMatchesPrimaryGameQueue) {
            return false;
        }
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

inline bool ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
    bool hadFSRFGPhase, bool hasPostSLLastWorkingQueue, bool hasSwapchainQueue,
    bool swapchainQueueMatchesPostSLLastWorkingQueue, bool runtimeOwnedNoFGStateWasClearedAfterLongOrigGameRun,
    bool hasOriginalGameQueue, bool commandQueueMatchesOriginalGameQueue) {
    // Pure-DLSS resume after a real DLSS suspension should reuse the already
    // proven PostSL topology when the live swapchain queue still matches the last
    // queue that successfully rendered PostSL. Reopening the old startup
    // bootstrap seam in that case sends the first resumed Streamline Present back
    // down the synthetic/bypass route and reproduces the GTA menu-close crash.
    if (hadFSRFGPhase) {
        return false;
    }

    if (hasPostSLLastWorkingQueue && hasSwapchainQueue && swapchainQueueMatchesPostSLLastWorkingQueue) {
        return true;
    }

    // A late in-session DLSS enable can also happen after CE already proved that
    // the runtime-owned no-FG handoff was auxiliary/stale and explicitly cleared
    // it after a long real-frame run back on origGame. In that case the next
    // DLSS-on edge should reuse the now-proven top-level game Present path rather
    // than reopening the old fragile startup bootstrap seam.
    return runtimeOwnedNoFGStateWasClearedAfterLongOrigGameRun && hasOriginalGameQueue &&
           commandQueueMatchesOriginalGameQueue;
}

inline bool ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
    bool hadFSRFGPhase, bool hasPostSLLastWorkingQueue, bool hasSwapchainQueue, bool explicitSetOptionsActivation,
    bool safePostFSRBootstrapPath) {
    // After a mixed FSR->DLSS epoch goes fully FG-off, the recovered non-FG path
    // can intentionally keep scQueue unset while it reuses the already validated
    // PostSL last-working queue. If a later DLSS-only resume happens before a
    // fresh non-FG swapchain proof re-establishes scQueue, that validated queue
    // is still the strongest evidence for the live topology.
    return hadFSRFGPhase && hasPostSLLastWorkingQueue && !hasSwapchainQueue &&
           (explicitSetOptionsActivation || safePostFSRBootstrapPath);
}

inline bool ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
    bool hadFSRFGPhase, bool hasSwapchainQueue, bool swapchainQueueDiffersFromOriginalGameQueue,
    bool streamlineStartupHandoffPending, bool hasPostSLLastWorkingQueue,
    bool swapchainQueueMatchesPostSLLastWorkingQueue) {
    // A fresh post-FSR Streamline handoff to a different runtime-owned swapchain
    // queue invalidates the old PostSL last-working queue proof. Reusing proof from
    // the previous DLSS epoch after a newer authoritative handoff can drive the
    // next off-recovery back onto a stale queue if the new comeback tears down
    // before first confirmation.
    return hadFSRFGPhase && hasSwapchainQueue && swapchainQueueDiffersFromOriginalGameQueue &&
           streamlineStartupHandoffPending && hasPostSLLastWorkingQueue && !swapchainQueueMatchesPostSLLastWorkingQueue;
}

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
                                                       bool useTopLevelHandoffWrapperProgress) {
    return startupActivationPending && streamlineFGRunning && !postSLActive &&
           (!processFrameRecentlySeen || useTopLevelHandoffWrapperProgress);
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
inline bool ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute(
    bool runtimeOwnsSwapchain, bool fsrFGApiActive, bool runtimeOwnedNativeFGPresentPath,
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

inline bool ShouldKeepPostSLActiveWhenRealECLUnavailable(bool hasRealD3D12ECL,
                                                         bool hasSelectedQueueOriginalSubmitPath,
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
inline bool ShouldRenderOverlayDirectlyOnPostSLTransitionProbe(bool selectedQueueIsSwapchainQueue,
                                                               bool deviceHealthy) {
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
           (!hadFSRFGPhase &&
            (confirmedPureStreamlineResumeProof || explicitEnablePureDLSSColdStartProof ||
             sameQueuePureDLSSColdStartSafe));
}

// Same-queue pure-DLSS cold start (Talos startup, session 20260615_162947: 29-present/437ms blank,
// gate=postsl-inactive->postsl-reactivation-warmup). The pure-DLSS cold-start countdown + 15-frame
// warmup protect DLSS-G's fragile init against CE's first overlay ECL. The DOCUMENTED GTA hang family
// (GetState-only) corrupts DLSS-G because GTA creates a SEPARATE runtime-owned swapchain/queue during
// init and CE's ECL lands on that separate proxy-init pipeline (guardrails.md: GTA pure-DLSS startup
// "moves to a runtime-owned swapchain"). When DLSS FG instead runs entirely on the GAME'S OWN single
// queue (scQueue==origGame, no separate command/SL-wrapper queue — observed in Talos:
// `PostSL locked to queue X (origGame=X scQueue=X cmdQueue=X slWrapper=0)`), there is NO separate
// proxy-init pipeline for CE's ECL to corrupt: the overlay ECL is just another submit on the game's
// own queue (the no-FG route, what RTSS does). Render from the first callback for this topology only.
// Re-evaluated every callback, so if a title transiently looks same-queue and then creates a separate
// runtime queue (GTA), this flips false and the countdown/warmup resume — no init-corruption window.
// device-removed is still caught by the PostSL render's pre-submit GetDeviceRemovedReason bail; a pure
// GPU hang is caught by the freeze watchdog. GTA-unvalidated; excludes the documented separate-queue
// hang by construction.
inline bool ShouldTreatSameQueuePureDLSSColdStartAsSafe(bool hadFSRFGPhase, bool swapchainQueueIsOriginalGameQueue,
                                                        bool noSeparateCommandQueue, bool hasSeparateSLWrapperQueue,
                                                        bool deviceRemoved) {
    return !hadFSRFGPhase && swapchainQueueIsOriginalGameQueue && noSeparateCommandQueue &&
           !hasSeparateSLWrapperQueue && !deviceRemoved;
}

// Make-before-break for explicit Streamline FG OFF. slDLSSGSetOptions(off)
// leaves the DLSS-G proxy swapchain and its pacer alive (DRED-proven Reflex
// invariant) and real games keep presenting it through menus; tearing PostSL
// down at the off edge blanks those presents until the normal route's first
// confirmed draw. A CONFIRMED PostSL path may stay armed-and-rendering across
// the off edge — it renders exactly what it rendered one present earlier on
// the same proven queue/swapchain. Never while an FSR/native-FG takeover owns
// or is about to own presentation (the quiesce invariant wins: stale DLSS
// callbacks must not submit into an AMD takeover).
inline bool ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(bool postSLConfirmedRendering, bool fsrFGApiActive,
                                                              bool runtimeOwnedNativeFGPresentPath,
                                                              bool protectedOfficialFFXStartupPending) {
    return postSLConfirmedRendering && !fsrFGApiActive && !runtimeOwnedNativeFGPresentPath &&
           !protectedOfficialFFXStartupPending;
}

// Streamline FG ON while the keep-alive latch is set and PostSL is still
// confirmed is a RESUME of a continuously-live path (suspend -> resume cycle),
// not a cold start: skip the synthetic-startup pending dance, countdown
// re-arm, and lifecycle reset so the resume seam has no uncovered presents.
inline bool ShouldResumeConfirmedPostSLFromKeepAliveOnStreamlineOn(bool keepAliveLatched,
                                                                   bool postSLConfirmedRendering) {
    return keepAliveLatched && postSLConfirmedRendering;
}

// Render permission during keep-alive. Requires the SL stack to still be
// loaded: once the modules unload, the proxy queue is gone and any late
// callback invocation must retire the latch instead of rendering.
inline bool ShouldAllowPostSLKeepAliveRenderAfterExplicitOff(bool keepAliveLatched, bool streamlineFGRunning,
                                                             bool streamlineModulesLoaded) {
    return keepAliveLatched && !streamlineFGRunning && streamlineModulesLoaded;
}

inline bool ShouldClearStreamlineStartupTransitionWindowAfterConfirmedPostSLRendering(
    bool streamlineStartupTransitionWindowActive, int stablePostSLFrameCount) {
    // Keep the startup churn window alive until the freshly activated PostSL path
    // has proved it is stable for more than the very first re-entrant frame.
    // Some pure-DLSS startups still bounce ON->OFF->ON immediately after the
    // first successful PostSL submit, and clearing the window too early turns
    // that transient OFF into a full teardown.
    return streamlineStartupTransitionWindowActive && stablePostSLFrameCount >= 2;
}

inline bool ShouldTreatConfirmedPostSLRenderingAsStartupSettling(bool postSLConfirmedRendering,
                                                                 int stablePostSLFrameCount) {
    // The first successful PostSL submit proves the render path works, but some
    // pure-DLSS startup families still emit a short burst of fragile
    // Streamline-originated Presents immediately afterward before the normal
    // long-running FG callback pattern settles. Keep the startup-family routing
    // guard alive until several consecutive confirmed PostSL frames have
    // completed, not just the first couple. GTA's latest DLSS FG trace still
    // falls back to the synthetic/bypass seam right as the eighth confirmed
    // frame completes if we clear the guard too early. Keep protecting the
    // startup family through the first eight confirmed frames, and only let the
    // ninth frame become the first fully settled one.
    constexpr int kConfirmedPostSLStartupSettleFrames = 8;
    return postSLConfirmedRendering && stablePostSLFrameCount <= kConfirmedPostSLStartupSettleFrames;
}

inline constexpr int GetConfirmedPostSLWarmupProofFrameThreshold() {
    return 30;
}

inline bool ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(int previousStablePostSLFrameCount) {
    // A reactivation that interrupts confirmed PostSL startup before the same
    // repo-wide warmup proof threshold is reached means the older epoch never
    // fully matured into the long-running FG callback pattern. The next epoch
    // can still receive stale OFF churn from that earlier half-proven startup,
    // so keep only the stale-OFF stabilization window alive until the new epoch
    // reaches the same proof threshold.
    return previousStablePostSLFrameCount > 0 &&
           previousStablePostSLFrameCount < GetConfirmedPostSLWarmupProofFrameThreshold();
}

inline constexpr int GetConfirmedPostSLRuntimeStateStabilizationFirstFrame() {
    return 9;
}

inline constexpr int GetConfirmedPostSLRuntimeStateStabilizationLastFrame(bool extendForChurnedReactivation = false) {
    return extendForChurnedReactivation ? GetConfirmedPostSLWarmupProofFrameThreshold() : 12;
}

inline bool ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(bool postSLConfirmedRendering,
                                                                         int stablePostSLFrameCount,
                                                                         bool extendForChurnedReactivation = false) {
    // GTA's latest pure-DLSS startup still emits one more stale OFF churn burst on
    // the first frame after the older settling guard ends. Keep only the
    // Streamline stale-OFF protection alive for a few more confirmed PostSL
    // frames so that short post-settling runtime-state jitter cannot collapse
    // the just-proven DLSS FG session. If the current epoch was itself preceded
    // by a churned reactivation before the same startup ever reached the repo's
    // 30-frame warmup proof threshold, stretch only this narrow stale-OFF guard
    // to that proof threshold for the new epoch. This intentionally does NOT
    // extend the wider DX12 startup-routing / handoff-pending protection.
    return postSLConfirmedRendering &&
           stablePostSLFrameCount >= GetConfirmedPostSLRuntimeStateStabilizationFirstFrame() &&
           stablePostSLFrameCount <= GetConfirmedPostSLRuntimeStateStabilizationLastFrame(extendForChurnedReactivation);
}

inline constexpr int GetConfirmedPostSLStaleOffWarmupProtectionLastFrame() {
    return GetConfirmedPostSLWarmupProofFrameThreshold();
}

inline bool ShouldDeferStaleOffDuringConfirmedPostSLWarmup(bool postSLConfirmedRendering, int stablePostSLFrameCount) {
    // GTA can keep reporting transient inactive Streamline DLSSG data after the
    // generic startup stale-OFF guard has done its job and PostSL is already
    // submitting successfully. Treat only startup-protected OFF churn as warmup
    // jitter until the same 30-frame proof threshold used by the PostSL stall
    // fallback is reached; a later OFF after proof still wins normally.
    return postSLConfirmedRendering &&
           stablePostSLFrameCount >= GetConfirmedPostSLRuntimeStateStabilizationFirstFrame() &&
           stablePostSLFrameCount <= GetConfirmedPostSLStaleOffWarmupProtectionLastFrame();
}

inline constexpr int GetConfirmedPostSLGetStateOffWarmupProtectionLastFrame() {
    return GetConfirmedPostSLStaleOffWarmupProtectionLastFrame();
}

inline bool ShouldDeferGetStateOffDuringConfirmedPostSLWarmup(bool postSLConfirmedRendering,
                                                              int stablePostSLFrameCount) {
    return ShouldDeferStaleOffDuringConfirmedPostSLWarmup(postSLConfirmedRendering, stablePostSLFrameCount);
}

inline bool ShouldTreatConfirmedPostSLBackendAsWarmupProtected(bool postSLConfirmedRendering,
                                                               int stablePostSLFrameCount) {
    // Confirmed PostSL rendering proves the overlay route works, but the first
    // few dozen callbacks are still the period where GTA / Streamline can churn
    // swapchain wrappers and stale OFF state. Keep the backend alive through the
    // same proof threshold used by the stale-OFF and stall-fallback guards.
    return postSLConfirmedRendering && stablePostSLFrameCount > 0 &&
           stablePostSLFrameCount <= GetConfirmedPostSLWarmupProofFrameThreshold();
}

inline bool ShouldDeferPostSLRenderingDuringStartupTransitionWindow(bool startupTransitionWindowActive,
                                                                    bool postSLConfirmedRendering,
                                                                    bool useTopLevelHandoffWrapperProgress,
                                                                    bool safePostFSRBootstrapPath = false,
                                                                    bool activeDLSSFGRuntimeSignalObserved = false,
                                                                    bool postSLWarmupComplete = false) {
    // While the startup transition window is active, DLSS FG is still initializing
    // its internal pipeline (queue setup, mutex tracking, fence state).  Our ECL
    // submission on the SL-owned swapchain queue during this phase can corrupt DLSS
    // FG's internal state.  Defer until the window expires.  Once PostSL has already
    // confirmed stable rendering (from a previous activation cycle), the guard is
    // unnecessary — the pipeline is proven safe.
    //
    // Previously the pure-DLSS wrapper-progress family bypassed this deferral, but
    // multi-device DLSS FG startup (e.g. GTA V Enhanced) can exhibit
    // OFF->ON->OFF->ON churn that corrupts sl_dlss_g's internal threading/mutex
    // state.  Wrapper ECL progress only proves queue topology stability, not that
    // SL's internal pipeline has settled.  Defer rendering for ALL families until
    // the startup transition window expires or PostSL has confirmed stable rendering.
    //
    // The one exception is the stronger post-FSR bootstrap proof: it requires the
    // fresh runtime-owned Streamline swapchain queue, an active/handing-off
    // Streamline signal, and a tracked submit path for that queue. That path is
    // specifically what real games expose when DLSS FG is enabled from a menu
    // after an FSR FG phase; waiting for the generic startup window can hide the
    // overlay for the entire short mode-switch interval.
    //
    // A current DLSS-G runtime-active signal after PostSL has survived its warmup
    // is also stronger than mere Streamline presence. Talos can report real
    // DLSS-G state through slDLSSGGetState well before it later returns a cached
    // slDLSSGSetOptions function pointer, so requiring SetOptions here leaves the
    // overlay blank for the whole startup timer even though the PostSL route is
    // already live.
    (void)useTopLevelHandoffWrapperProgress;
    const bool activeRuntimeWarmupProof = activeDLSSFGRuntimeSignalObserved && postSLWarmupComplete;
    return startupTransitionWindowActive && !postSLConfirmedRendering && !safePostFSRBootstrapPath &&
           !activeRuntimeWarmupProof;
}

inline bool ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
    bool hadFSRFGPhase, bool startupTopLevelPresentConsumed, int wrapperProgressCount, bool startupActivationPending,
    bool postSLActive, bool postSLConfirmedRendering, ULONGLONG processFrameDormantMs, bool dumpAlreadyRequested) {
    if (dumpAlreadyRequested || hadFSRFGPhase || !startupTopLevelPresentConsumed) {
        return false;
    }

    if (wrapperProgressCount < 4) {
        return false;
    }

    const bool startupStillHalfArmed = startupActivationPending || (postSLActive && !postSLConfirmedRendering);
    if (!startupStillHalfArmed) {
        return false;
    }

    return processFrameDormantMs >= 1000;
}

inline bool ShouldRetainStreamlineStartupActivationSwapchain(bool isD3D12SwapChain,
                                                             bool freshAuthoritativeStreamlineHandoff,
                                                             bool runtimeOwnedSwapchainActive) {
    return isD3D12SwapChain && freshAuthoritativeStreamlineHandoff && runtimeOwnedSwapchainActive;
}

inline bool ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(bool isD3D12SwapChain,
                                                                            bool postSLCallbackAvailable,
                                                                            bool startupActivationPending,
                                                                            bool postSLActiveButUnconfirmed,
                                                                            bool postSLConfirmedRendering) {
    // A normal-route PostSL callback is already scoped by the DXGI layer to a
    // real synthetic Streamline Present, not to mere Streamline DLL presence.
    // Retain that concrete swapchain while startup is half-armed so ECL-expiry
    // recovery does not fall back to stale ProcessFrame state.
    return isD3D12SwapChain && postSLCallbackAvailable && !postSLConfirmedRendering &&
           (startupActivationPending || postSLActiveButUnconfirmed);
}

inline bool ShouldPreferRetainedStreamlineStartupActivationSwapchain(bool retainedSwapchainAvailable,
                                                                     bool startupActivationPending,
                                                                     bool postSLActiveButUnconfirmed) {
    return retainedSwapchainAvailable && (startupActivationPending || postSLActiveButUnconfirmed);
}

inline bool ShouldServicePostSLStartupActivationWhileOffChurnDeferred(bool shouldKeepOffChurnDeferred,
                                                                      bool startupTransitionWindowActive,
                                                                      bool activationPending,
                                                                      bool postSLStartupActivationEntered,
                                                                      bool callbackInstalled) {
    return shouldKeepOffChurnDeferred && !startupTransitionWindowActive && callbackInstalled && activationPending &&
           !postSLStartupActivationEntered;
}

inline bool ShouldInvokeRetainedPostSLStartupActivationService(
    bool callbackInstalled, bool activationSwapchainAvailable, bool activationPending,
    bool postSLStartupActivationEntered, bool postSLConfirmedRendering, bool activationServiceInProgress,
    bool allowConfirmedWarmupService = false) {
    if (!callbackInstalled || !activationSwapchainAvailable || activationServiceInProgress) {
        return false;
    }

    if (postSLConfirmedRendering) {
        return allowConfirmedWarmupService;
    }

    return activationPending && !postSLStartupActivationEntered;
}

inline bool ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
    bool startupTransitionWindowActive, bool postSLConfirmedRendering, bool hadFSRFGPhase,
    bool startupTopLevelPresentConsumed, bool wrapperProgressObserved, bool explicitSetOptionsActivation,
    bool activeDLSSFGRuntimeSignalObserved, bool startupActivationPending, bool postSLActive) {
    if (!startupTransitionWindowActive || postSLConfirmedRendering || hadFSRFGPhase) {
        return false;
    }

    if (!startupTopLevelPresentConsumed || !wrapperProgressObserved) {
        return false;
    }

    // Explicit DLSSG SetOptions(ON), and active state from the official
    // slDLSSGGetState API, are both stronger evidence than generic Streamline
    // involvement. Once the current comeback has either signal, the callback
    // should advance instead of hiding the overlay until window expiry.
    if (explicitSetOptionsActivation || activeDLSSFGRuntimeSignalObserved) {
        return false;
    }

    // The pure-DLSS top-level-handoff wrapper-progress family can expose only one
    // decisive synthetic Present while Streamline is still inside its fragile
    // startup window. Even if CE ultimately defers warm-up / rendering inside
    // PostSL, simply entering the callback through SL's Present chain at that
    // point can still perturb the runtime. Keep the callback fully dormant until
    // the startup window expires; wrapper progress is still tracked separately so
    // activation can resume on a later safe callback.
    return startupActivationPending || postSLActive;
}

inline bool ShouldKeepSyntheticStartupStateUntilConfirmedRender(bool startupActivationPending,
                                                                bool postSLActiveButUnconfirmed,
                                                                bool postSLConfirmedRendering,
                                                                bool postSLConfirmedButStartupSettling) {
    // Historical name retained: the same startup-protection contract must now
    // survive not only until first confirmation, but also through the short
    // confirmed-startup-settling window. GTA's fresh runtime-owned pure-DLSS
    // handoff can otherwise clear the one-shot normal-route protection exactly at
    // first confirmation and fall back into synthetic re-entrant routing on the
    // next Streamline Present.
    if (postSLConfirmedRendering) {
        return postSLConfirmedButStartupSettling;
    }

    return startupActivationPending || postSLActiveButUnconfirmed;
}

inline bool ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
    bool startupActivationPending, bool postSLActiveButUnconfirmed, bool postSLConfirmedRendering,
    bool postSLConfirmedButStartupSettling) {
    return ShouldKeepSyntheticStartupStateUntilConfirmedRender(startupActivationPending, postSLActiveButUnconfirmed,
                                                               postSLConfirmedRendering,
                                                               postSLConfirmedButStartupSettling);
}

inline bool ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(
    bool streamlineFGRunning, bool startupActivationPending, bool postSLActiveButUnconfirmed,
    bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling) {
    // The FG transition cooldown still protects the unsafe pre-SL/reinit path,
    // but a half-armed or freshly confirmed PostSL startup route is already on
    // Streamline's own Present callback timing. Re-applying the generic cooldown
    // to PostSL itself blanks the overlay during DLSS resume even though the only
    // remaining work is rebuilding resources on the new authoritative SL
    // swapchain from inside PostSL.
    return streamlineFGRunning &&
           ShouldKeepSyntheticStartupStateUntilConfirmedRender(startupActivationPending, postSLActiveButUnconfirmed,
                                                               postSLConfirmedRendering,
                                                               postSLConfirmedButStartupSettling);
}

inline bool ShouldContinueECLDrivenPostSLStartupProgress(bool overlayVisible, bool startupActivationPending,
                                                         bool postSLStartupActivationEntered,
                                                         bool postSLConfirmedRendering, bool callbackInstalled,
                                                         bool cachedSwapchainAvailable, bool hadFSRFGPhase,
                                                         bool safePostFSRBootstrapPath) {
    if (!overlayVisible || !callbackInstalled || !cachedSwapchainAvailable || postSLConfirmedRendering) {
        return false;
    }

    if (hadFSRFGPhase && !safePostFSRBootstrapPath) {
        return false;
    }

    // The retained-swapchain service exists to wake a dormant startup activation
    // path. ProcessFrame may pre-arm PostSL before the startup callback ever
    // enters; that state still needs this wake path. Once the callback has
    // actually entered, repeated direct service callbacks can re-enter DLSSG
    // startup/pacing workers; continued rendering progress must come from the
    // normal Present callback route.
    return startupActivationPending && !postSLStartupActivationEntered;
}

inline bool ShouldTriggerExpiryDrivenECLPostSLStartupActivation(bool startupTransitionWindowJustExpired,
                                                                bool startupActivationPending, bool callbackInstalled,
                                                                bool hadFSRFGPhase, bool safePostFSRBootstrapPath) {
    if (!startupTransitionWindowJustExpired || !startupActivationPending || !callbackInstalled) {
        return false;
    }

    return !hadFSRFGPhase || safePostFSRBootstrapPath;
}

inline bool ShouldPreserveConfirmedPostSLDuringFGCooldown(bool streamlineFGRunning, bool postSLConfirmedRendering) {
    return streamlineFGRunning && postSLConfirmedRendering;
}

inline bool ShouldPreserveActivePostSLDuringFGCooldown(bool streamlineFGRunning, bool postSLConfirmedRendering,
                                                       bool postSLActiveButUnconfirmed) {
    return streamlineFGRunning && (postSLConfirmedRendering || postSLActiveButUnconfirmed);
}

inline bool ShouldPreserveActivePostSLWhenPreSLDrawIsSkipped(bool streamlineFGRunning, bool postSLConfirmedRendering,
                                                             bool postSLActiveButUnconfirmed) {
    return ShouldPreserveActivePostSLDuringFGCooldown(streamlineFGRunning, postSLConfirmedRendering,
                                                      postSLActiveButUnconfirmed);
}

inline bool ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(bool streamlineTurnedOff, bool hadFSRFGPhase,
                                                                 bool fsrFGApiActive,
                                                                 bool runtimeOwnedNativeFGPresentPath,
                                                                 bool hasOverlayBackend, bool hasSyncBackend,
                                                                 bool hasSwapchainQueue, bool hasOriginalGameQueue,
                                                                 bool deviceRemoved) {
    // Pure DLSS-G menu suspend/resume is not a mixed-runtime takeover. Once the
    // old PostSL work has been drained, waiting through the generic FG handoff
    // cooldown only blanks the overlay while the game is back on the same
    // Streamline-owned swapchain path. Keep the stricter cooldown for any FSR
    // history/native ownership, because those paths still need teardown proof.
    return streamlineTurnedOff && !hadFSRFGPhase && !fsrFGApiActive && !runtimeOwnedNativeFGPresentPath &&
           hasOverlayBackend && hasSyncBackend && hasSwapchainQueue && hasOriginalGameQueue && !deviceRemoved;
}

// A DLSS-FG SUSPEND (slDLSSGSetOptions(off), proxy stays live) where PostSL was
// CONFIRMED rendering this epoch and the make-before-break keep-alive is covering
// the proxy presents is safe to rebuild immediately even WITH FSR history. The
// sticky hadFSRFGPhase gate on the pure-DLSS bypass above is too strict here: by
// the time DLSS is being suspended the FSR phase ended long ago, DLSS-G has been
// presenting stably, and PostSL confirmed rendering means the overlay ECL on the
// runtime-owned SL queue ALREADY succeeded many times this epoch (the device is
// demonstrably healthy on that exact path). The generic 60-frame FG-off reinit
// cooldown therefore blanks a provably-live overlay for ~672 ms with nothing to
// wait for (session 20260613_150750: post-FSR DLSS suspend, gate=overlay-backend-
// uninitialized, confirmedDuringStreak=1). The confirmed-PostSL-this-epoch +
// keep-alive proof replaces the hadFSRFGPhase exclusion; the FSR/native-FG and
// device-health guards are unchanged so a genuine FSR/AMD takeover still keeps the
// stricter cooldown.
inline bool ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
    bool streamlineTurnedOff, bool postSLExplicitOffKeepAlive, bool postSLConfirmedRendering, bool fsrFGApiActive,
    bool runtimeOwnedNativeFGPresentPath, bool hasOverlayBackend, bool hasSyncBackend, bool hasSwapchainQueue,
    bool hasOriginalGameQueue, bool deviceRemoved) {
    return streamlineTurnedOff && postSLExplicitOffKeepAlive && postSLConfirmedRendering && !fsrFGApiActive &&
           !runtimeOwnedNativeFGPresentPath && hasOverlayBackend && hasSyncBackend && hasSwapchainQueue &&
           hasOriginalGameQueue && !deviceRemoved;
}

inline bool ShouldEnterSyntheticPostSLStartupActivation(bool startupActivationPending, bool postSLActiveButUnconfirmed,
                                                        bool postSLConfirmedRendering) {
    return startupActivationPending && !postSLActiveButUnconfirmed && !postSLConfirmedRendering;
}

inline bool ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
    bool streamlineFGRunning, bool postSLConfirmedRendering, bool confirmedPostSLBackendWarmupProtected,
    bool hadFSRFGPhase, bool runtimeOwnsSwapchain, bool hasSwapchainQueue, bool hasOriginalGameQueue,
    bool swapchainQueueDiffersFromOriginalGameQueue) {
    // GTA can briefly report a swapchain pointer change after PostSL has
    // already rendered successfully on Streamline's runtime-owned queue
    // (FSR -> DLSS handoff, 20260531_232108). Treating that as an ordinary
    // reinit destroys the only proven-safe path and can trip ERR_GFX_STATE.
    // 20260612_002523 proved the same for the PURE-DLSS startup (no FSR
    // history): PostSL confirmed on the live Streamline swapchain, then the
    // bookkeeping pointer-change catch-up armed the 90-frame cooldown,
    // deactivated PostSL, and the overlay never came back because the game
    // had retired its original queue and zero-ECL classification starved the
    // cooldown ticks. PostSL confirmation is by definition proof on the LIVE
    // swapchain (the callback hands CE the presenting swapchain), so it is
    // preserve-worthy with or without FSR history.
    (void)hadFSRFGPhase;
    return streamlineFGRunning && postSLConfirmedRendering && confirmedPostSLBackendWarmupProtected &&
           runtimeOwnsSwapchain && hasSwapchainQueue && hasOriginalGameQueue &&
           swapchainQueueDiffersFromOriginalGameQueue;
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

inline bool ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(bool runtimeOwnsSwapchain,
                                                                           bool callerFromFFXFGModule,
                                                                           bool officialAMDFFXRuntimeCreator,
                                                                           bool hasDirectFFXApiConfirmation) {
    // GTA's official AMD runtime creates its runtime-owned swapchain before the
    // enabled ffxConfigure packet. Treat the swapchain as native-FSR-owned for
    // routing/overlay suppression, but defer the heavier Streamline/PostSL
    // teardown until direct API proof arrives.
    return runtimeOwnsSwapchain && callerFromFFXFGModule && officialAMDFFXRuntimeCreator &&
           !hasDirectFFXApiConfirmation;
}

inline bool ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(bool authoritativeFFXRuntimeCreator,
                                                                            bool officialAMDFFXRuntimeCreator,
                                                                            bool ffxStartupAlreadyResolved) {
    // The official AMD runtime can fail fast immediately after creating its
    // startup swapchain, before the enabled ffxConfigure packet is visible.
    // During that window CE must not refresh Present hooks, inspect the AMD
    // export table, or otherwise mutate the runtime-owned swapchain path.
    return authoritativeFFXRuntimeCreator && officialAMDFFXRuntimeCreator && !ffxStartupAlreadyResolved;
}

inline bool ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(bool protectedOfficialFFXStartupPath,
                                                                           bool hasDirectQueue) {
    // Capturing the queue pointer itself is cheap and keeps the post-configure
    // overlay route from going blind, but applying ownership/Present-hook side
    // effects must still wait for enabled ffxConfigure.
    return protectedOfficialFFXStartupPath && hasDirectQueue;
}

inline bool ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(bool protectedOfficialFFXStartupPending,
                                                                        bool ffxStartupAlreadyResolved) {
    // The protected startup-create path is only useful if the rest of CE also
    // stays out of the runtime's way until the official AMD runtime has reached
    // its enabled ffxConfigure packet. ECL probes, queue registration, normal
    // fallback overlay submissions, and late export inspection can all be too
    // invasive in the narrow pre-configure window; sustained render progress is
    // only a diagnostic signal, not proof that CE may resume GPU side effects.
    return protectedOfficialFFXStartupPending && !ffxStartupAlreadyResolved;
}

inline bool ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(bool protectedOfficialFFXStartupPending,
                                                                    bool ffxStartupAlreadyResolved,
                                                                    bool hasStagedDirectQueue) {
    (void)protectedOfficialFFXStartupPending;
    (void)ffxStartupAlreadyResolved;
    (void)hasStagedDirectQueue;
    // The staged DIRECT queue is useful deferred takeover state, but it is not
    // proof that separate CE overlay submissions are safe before AMD reaches an
    // enabled ffxConfigure / present-callback packet. GTA's DLSS->FSR handoff
    // showed that even "overlay-only" ECLs in this pre-enable window can leave
    // the AMD presenter/interpolation threads waiting inside ffxQuery.
    return false;
}

// 2D-MENU FSR-FG ARMING (Talos): switching to FSR FG in a 2D menu makes AMD create its FFX
// swapchain, arming protected official-FFX startup, while AMD's FFX runtime stays DORMANT (no
// enabled `ffxConfigure`, no present callback) until 3D resumes — so the blanket quiesce blanks
// the overlay for the whole menu (8+ s observed). The game is, however, presenting real menu
// frames on the LIVE FSR swapchain, so CE keeps the overlay visible by rebuilding against that
// swapchain and drawing on its CREATION queue (the staged takeover queue) — the only queue
// authorized to touch its backbuffers (cross-queue = DEVICE_REMOVED). The caller resolves the
// submit queue to that staged takeover queue and rebuilds the RTVs against the live swapchain.
//
// Gated ONLY on: protected-FFX startup pending, the overlay backend live (overlayInit/syncInit,
// bootstrappable during the fresh rebuild), a DISTINCT staged takeover queue existing (the live
// swapchain's creation queue we submit on), AMD provably DORMANT (no `directFFXApiConfirmation`,
// no live FFX present callback), and `sustainedGameProgress` (a few stable present frames so the
// fragile AMD swapchain-create instant stays quiesced). The instant the runtime enables (enabled
// `ffxConfigure` / FFX present callback fires) this returns false and the full quiesce + FFX
// present-callback bridge resume unchanged.
//
// GTA RISK (per user decision 2026-06-16, NOT GTA-validated): the earlier `!runtimeOwnsSwapchain
// && swapchainQueueIsOriginalGameQueue` guardrail — which kept this false for GTA's runtime-owned/
// staged-queue menu topology — has been DROPPED so the same Talos menu works after FG-switching
// sequences (topology becomes runtimeOwns=1, scQueue!=origGame). This widens the documented
// `20260525_195848_gtafreeze` ffxQuery-wedge risk surface; GTA must be re-validated. Independently
// revertible (restore the two conjuncts). See guardrails.md.
inline bool ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
    bool protectedOfficialFFXStartupPending, bool overlayInit, bool syncInit, bool hasDistinctStagedTakeoverQueue,
    bool hasDirectFFXApiConfirmation, bool ffxPresentCallbackActive, bool sustainedGameProgress) {
    return protectedOfficialFFXStartupPending && overlayInit && syncInit && hasDistinctStagedTakeoverQueue &&
           !hasDirectFFXApiConfirmation && !ffxPresentCallbackActive && sustainedGameProgress;
}

inline bool ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(bool protectedOverlayOnlyEligible,
                                                                               bool hasStagedDirectQueue) {
    (void)protectedOverlayOnlyEligible;
    (void)hasStagedDirectQueue;
    // Pre-enable protected official FFX startup must stay GPU-quiet. Visibility
    // resumes through the FFX present callback once enabled configure/callback
    // proof exists, not through a generic cooldown bypass.
    return false;
}

inline bool ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(
    bool protectedOfficialFFXStartupPending, bool ffxStartupAlreadyResolved) {
    // The protected swapchain is not yet a drawable CE target, but tearing down
    // the old backend and immediately rebuilding against the staged FFX queue is
    // worse: it creates the exact pre-enable GPU traffic that can wedge AMD FSR.
    // Preserve backend/resources and let the callback path take over once proof
    // arrives.
    return ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(protectedOfficialFFXStartupPending,
                                                                       ffxStartupAlreadyResolved);
}

inline bool ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
    bool protectedOfficialFFXStartupPending, bool ffxStartupAlreadyResolved, bool postSLCallbackInstalled,
    bool postSLActive, bool postSLConfirmedRendering, bool streamlineFGRunning, bool startupActivationPending) {
    // Heavy official-FFX takeover side effects wait for ffxConfigure(enable), but
    // an already-live Streamline/PostSL path must stop immediately. Otherwise a
    // re-entrant Streamline callback can submit overlay work to the old DLSS path
    // after the AMD runtime has begun replacing the swapchain.
    return ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(protectedOfficialFFXStartupPending,
                                                                       ffxStartupAlreadyResolved) &&
           (postSLCallbackInstalled || postSLActive || postSLConfirmedRendering || streamlineFGRunning ||
            startupActivationPending);
}

inline uint32_t GetProtectedOfficialFFXStartupProcessFrameProgressThreshold() {
    return 120;
}

inline uint32_t GetProtectedOfficialFFXStartupECLProgressThreshold() {
    return 4096;
}

inline bool ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
    bool protectedOfficialFFXStartupPending, bool ffxStartupAlreadyResolved, uint32_t processFrameSkips,
    uint32_t eclPassThroughs) {
    (void)protectedOfficialFFXStartupPending;
    (void)ffxStartupAlreadyResolved;
    (void)processFrameSkips;
    (void)eclPassThroughs;
    // GTA freeze dumps showed the old progress-only graduation could resume CE
    // overlay/capture side effects while the official AMD presenter thread was
    // still inside its private startup/query path. Only a real direct configure
    // or present-callback proof is authoritative.
    return false;
}

inline bool ShouldApplySwapchainDescriptorOverridesForCreate(bool callerFromThirdPartyOverlay,
                                                             bool authoritativeFrameGenerationRuntimeCreator) {
    // Runtime FG components treat swapchain creation as part of their own
    // startup handshake. Preserve their descriptor byte-for-byte; even
    // "safe" CE additions such as the waitable-object flag can change that
    // handshake before the runtime has accepted its configure packet.
    return !callerFromThirdPartyOverlay && !authoritativeFrameGenerationRuntimeCreator;
}

inline bool ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(bool runtimeOwnsSwapchain,
                                                                    bool protectedOfficialFFXStartupPending) {
    // The first disabled startup-arming ffxConfigure can arrive before CE has
    // safely claimed the official AMD runtime swapchain queue. Keep that packet
    // in the startup-arming path instead of treating it as a real OFF.
    return runtimeOwnsSwapchain || protectedOfficialFFXStartupPending;
}

inline bool ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(bool runtimeOwnsSwapchain,
                                                                   bool directFFXApiConfirmation,
                                                                   bool nativeFSRStartupArmingPending) {
    // FSR context creation alone is not proof that native FSR owns presentation:
    // some games create a frame-generation context while still configured OFF.
    // Treat a runtime-owned swapchain as native-FSR presentation only after a
    // direct enabled configure, or while an explicit native-FSR startup arming
    // path is pending.
    return runtimeOwnsSwapchain && (directFFXApiConfirmation || nativeFSRStartupArmingPending);
}

inline bool ShouldClearStaleNativeFGPresentOwnershipOnExplicitStreamlineComeback(
    bool hadFSRFGPhase, bool explicitSetOptionsActivation, bool hasSwapchainQueue,
    bool swapchainQueueDiffersFromOriginalGameQueue, bool streamlineStartupHandoffPending,
    bool runtimeOwnedNativeFGPresentPath) {
    // After a real FSR -> DLSS comeback, the preserved non-origGame swapchain queue
    // can already belong to the new authoritative Streamline handoff. In that
    // state, a stale native-FSR Present-ownership latch from the prior runtime must
    // not keep the later DLSS startup path classified as still runtime-owned native
    // FG. Clear only that stale native-FSR ownership latch; the generic runtime-
    // owned swapchain fact can still remain true for the new Streamline-owned
    // queue topology.
    return hadFSRFGPhase && explicitSetOptionsActivation && hasSwapchainQueue &&
           swapchainQueueDiffersFromOriginalGameQueue && streamlineStartupHandoffPending &&
           runtimeOwnedNativeFGPresentPath;
}

inline bool ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(bool streamlineModuleLoaded,
                                                                      bool streamlineFGRunning,
                                                                      bool streamlineStartupHandoffPending,
                                                                      bool callerFromFFXFGModule,
                                                                      bool ffxFrameGenerationInStack) {
    if (!streamlineModuleLoaded) {
        return false;
    }

    // During mixed DLSS/FSR sessions, authoritative FFX takeover is also a
    // runtime-managed swapchain lifecycle boundary. CE must not tear down
    // overlay resources or retry CreateSwapChainForHwnd on that path.
    if (callerFromFFXFGModule || ffxFrameGenerationInStack) {
        return true;
    }

    return streamlineFGRunning || streamlineStartupHandoffPending;
}

// DRED arming level for CE_DX12_DRED.
//
// Full DRED auto-breadcrumbs force the application's command-list Reset() to do a
// per-frame kernel GPU allocation; that stalls Present during the Alt+Tab mode
// switch (logs/20260606_145929) AND shifts steady-state timing enough to mask a
// timing-sensitive GPU hang. Page-fault-only arming does NOT enable
// auto-breadcrumbs, so it adds no per-Reset allocation — it still records the
// faulting GPU virtual address + existing/recently-freed allocation nodes on a
// device-removal, which is the smoking gun for a GPU-side DEVICE_HUNG. Prefer
// page-fault-only for the uncapped steady-state DEVICE_HUNG repro.
enum class DredArmMode : int {
    kOff = 0,
    kPageFaultOnly = 1,  // page-fault output only — low perturbation
    kFull = 2,           // auto-breadcrumbs + page-fault + context — high perturbation
};

// Decide DRED arming level from the CE_DX12_DRED env/flag value. `isSet` is whether
// the value is present; `value` its (possibly null) contents.
//   page-fault-only: "pf" / "pagefault" / "page-fault" / "2"
//   full:            "1" / "on" / "true" / "yes" / "full"
//   off:             unset / "0" / "off" / "false" / anything unrecognized
inline DredArmMode DecideDredArmMode(const char* value, bool isSet) {
    if (!isSet || value == nullptr || value[0] == '\0') {
        return DredArmMode::kOff;
    }
    if (_stricmp(value, "pf") == 0 || _stricmp(value, "pagefault") == 0 || _stricmp(value, "page-fault") == 0 ||
        _stricmp(value, "2") == 0) {
        return DredArmMode::kPageFaultOnly;
    }
    if (_stricmp(value, "1") == 0 || _stricmp(value, "on") == 0 || _stricmp(value, "true") == 0 ||
        _stricmp(value, "yes") == 0 || _stricmp(value, "full") == 0) {
        return DredArmMode::kFull;
    }
    return DredArmMode::kOff;
}

// Pure decision for whether DRED arming is enabled at all (either mode). Kept as a
// thin wrapper over DecideDredArmMode so DRED stays OFF unless explicitly requested.
inline bool ShouldEnableDredFromEnv(const char* value, bool isSet) {
    return DecideDredArmMode(value, isSet) != DredArmMode::kOff;
}

// When the D3D12 device is already removed/hung, forwarding the application's
// command lists into the torn-down driver dereferences freed UMD state and crashes
// inside the driver (observed: a 32-bit DEVICE_HUNG TDR is followed ~1s later by an
// nvwgf2um access violation while the app's render loop keeps calling
// ExecuteCommandLists). A D3D12 device is permanently lost once removed, so dropping
// the submission is the only safe action — the app still learns of the loss when its
// next Present returns DXGI_ERROR_DEVICE_*. Returns true only while the device is
// healthy.
inline bool ShouldForwardAppCommandListsToDriver(bool deviceRemoved) {
    return !deviceRemoved;
}

// ---------------------------------------------------------------------------
// [OVERLAY COVERAGE] per-present overlay-coverage accounting.
// ---------------------------------------------------------------------------
// Goal: zero presented-frames-without-overlay across all FG transitions. Each
// accounted present is either covered (an overlay draw of any route — normal,
// PostSL, FFX present callback — was observed for it) or uncovered; uncovered
// presents form streaks that are the direct measure of visible overlay blanks.
//
// `drawObserved` is the caller's draw-counter delta since the previous
// accounted present. `inheritCoverageIfNoDraw` handles presents whose overlay
// content is composed by the FG runtime from a previous covered present
// (zero-ECL interpolated frames, SL-owned top-level transport presents): they
// count as covered while the current uncovered streak is zero, and extend the
// streak otherwise. That keeps healthy FG sessions free of false 1-present
// streak noise while real blank windows still grow one continuous streak.
//
// Thread-safety is the caller's job (the hook serializes calls with a tiny
// spin lock); this type stays plain so it is unit-testable.
struct OverlayPresentCoverageResult {
    bool covered = false;
    bool uncoveredStreakStarted = false;
    bool uncoveredStreakEnded = false;
    uint64_t endedStreakLength = 0;
    bool newLongestStreak = false;
};

class OverlayPresentCoverageTracker {
public:
    OverlayPresentCoverageResult NotePresent(bool drawObserved, bool inheritCoverageIfNoDraw) {
        OverlayPresentCoverageResult result;
        ++totalPresents_;
        result.covered = drawObserved || (inheritCoverageIfNoDraw && currentUncoveredStreak_ == 0);
        if (result.covered) {
            if (currentUncoveredStreak_ > 0) {
                result.uncoveredStreakEnded = true;
                result.endedStreakLength = currentUncoveredStreak_;
                currentUncoveredStreak_ = 0;
            }
            return result;
        }
        ++uncoveredPresents_;
        result.uncoveredStreakStarted = (currentUncoveredStreak_ == 0);
        ++currentUncoveredStreak_;
        if (currentUncoveredStreak_ > longestUncoveredStreak_) {
            longestUncoveredStreak_ = currentUncoveredStreak_;
            result.newLongestStreak = true;
        }
        return result;
    }

    uint64_t TotalPresents() const {
        return totalPresents_;
    }
    uint64_t UncoveredPresents() const {
        return uncoveredPresents_;
    }
    uint64_t CurrentUncoveredStreak() const {
        return currentUncoveredStreak_;
    }
    uint64_t LongestUncoveredStreak() const {
        return longestUncoveredStreak_;
    }

private:
    uint64_t totalPresents_ = 0;
    uint64_t uncoveredPresents_ = 0;
    uint64_t currentUncoveredStreak_ = 0;
    uint64_t longestUncoveredStreak_ = 0;
};

}  // namespace ce::dx12_overlay_policy
