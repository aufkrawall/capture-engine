#pragma once

#include <windows.h>

#include <cstdint>

#include <dxgi1_6.h>

#include "../dxgi_presentation_color.h"
#include "../fg_runtime_state.h"

struct ID3D12CommandQueue;
struct ID3D12Fence;

// Barrier modes, overlay completion/signal ordering, and third-party startup coexistence.

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

inline bool ShouldPostponeDeferredTempSwapchainPresentHookInstall(bool presentHooksInstalled,
                                                                  bool earlyInstallDeferred,
                                                                  bool d3d12DeviceCreated,
                                                                  bool thirdPartyOverlayLoaded) {
    // Same startup window as above, re-evaluated where the deferred install is
    // actually attempted. Without this the deferral is a no-op: DX12Hook::Init
    // skips the eager temp swapchain because a third-party overlay is loaded and
    // then calls FindAndWrapPreExistingSwapchains on the next line, which built
    // the temp swapchain anyway "because the overlay's startup hook chain should
    // be settled" - in session 20260815_203836 both decisions were logged in the
    // same millisecond, so nothing had settled, and the creation recursed to
    // death inside gameoverlayrenderer64. The install now waits for the game's
    // own D3D12 device, which is the same evidence Init already trusts.
    if (presentHooksInstalled) {
        return false;
    }
    if (!earlyInstallDeferred || !thirdPartyOverlayLoaded) {
        return false;
    }
    return !d3d12DeviceCreated;
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
    return startupOverlayCompatibilityActive && !actualFrameGenerationActive && msSinceResourcePrime < settleDelayMs &&
           !preserveLiveOverlayDuringHandoff;
}

inline bool ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
    bool startupCompatAlreadySettled, bool overlayBackendReady, bool runtimeOwnsSwapchain, bool streamlineFGRunning,
    fg_runtime::RuntimeMode runtimeMode, bool explicitSetOptionsActivation, bool observedAnyFrameGenerationActivity,
    bool hadFSRFGPhase, bool hasOriginalGameQueue) {
    // A third-party startup overlay can create a Streamline-adjacent no-FG
    // swapchain after CE has already rendered stably on the real game swapchain.
    // That is not yet a DLSS-G activation. Keep the existing single-queue
    // overlay path alive until an explicit FG signal appears instead of
    // blanking the overlay for a speculative runtime-owned handoff. FSR history
    // is authoritative evidence that this is a real FG mode switch: the old
    // swapchain-scoped RTV/sync state must be retired so the new Streamline
    // proxy can be prewarmed before DLSS is enabled.
    return startupCompatAlreadySettled && overlayBackendReady && runtimeOwnsSwapchain && !streamlineFGRunning &&
           runtimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG && !explicitSetOptionsActivation &&
           !observedAnyFrameGenerationActivity && !hadFSRFGPhase && hasOriginalGameQueue;
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

inline bool ShouldTreatOriginalQueueCreateWithStreamlineStackAsNormalReturn(
    bool authoritativeFFXRuntimeCreator, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack,
    bool streamlineEnableCallInFlight, bool hasOriginalGameQueue, bool queueMatchesOriginalGameQueue) {
    // sl.interposer remains on wrapped DXGI call stacks while a third-party
    // overlay forwards the game's ordinary swapchain recreation. Stack presence
    // alone is therefore candidate evidence: a distinct queue proves an actual
    // Streamline takeover, while the known original game queue proves the normal
    // presentation topology has returned.
    return !authoritativeFFXRuntimeCreator && !callerFromStreamlineFGModule && streamlineFrameGenerationInStack &&
           !streamlineEnableCallInFlight && hasOriginalGameQueue && queueMatchesOriginalGameQueue;
}

inline bool ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn(bool gameCreatedSwapchain,
                                                                             bool postSLExplicitOffKeepAlive,
                                                                             bool actualFrameGenerationActive,
                                                                             bool streamlineFGRunning) {
    // Explicit OFF alone is not a normal-route boundary: menu suspension can
    // leave the proven Streamline proxy presenting indefinitely. A swapchain
    // created by the game after that OFF edge is stronger evidence. It proves
    // that the app has replaced the proxy with a native Present topology, even
    // when the app also rebuilt its device/queue and therefore cannot return to
    // the retired original queue pointer.
    return gameCreatedSwapchain && postSLExplicitOffKeepAlive && !actualFrameGenerationActive && !streamlineFGRunning;
}

inline bool ShouldProcessLogicalSwapchainReplacement(bool pointerAddressChanged, bool exactNewSwapchainLifetimeProof) {
    // COM allocators may reuse the same interface address after a runtime-owned
    // swapchain is destroyed. Creation-time proof identifies a new lifetime even
    // when raw pointer comparison alone observes an ABA-equal address.
    return pointerAddressChanged || exactNewSwapchainLifetimeProof;
}

// A game-created replacement swapchain after authoritative DLSS OFF is the
// make-before-break boundary from the retired PostSL proxy to the native game
// route. The exact swapchain/queue association is already proven at creation,
// so a residual "FG was active one frame ago" bit must not impose the generic
// 90-frame reinit cooldown on its first Present. Re-check every live ownership
// and device guard so a resumed FG mode, FSR takeover, mismatched route, or
// removed device still keeps the protective cooldown.
inline bool ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(
    bool exactNormalReturnSwapchainProof, bool normalRouteOwnershipProven, bool frameGenerationCurrentlyActive,
    bool streamlineFGRunning, bool fsrFGApiActive, bool nativeFSRInternalNoCallbackComposition,
    bool runtimeOwnsSwapchain, bool deviceRemoved) {
    return exactNormalReturnSwapchainProof && normalRouteOwnershipProven && !frameGenerationCurrentlyActive &&
           !streamlineFGRunning && !fsrFGApiActive && !nativeFSRInternalNoCallbackComposition &&
           !runtimeOwnsSwapchain && !deviceRemoved;
}

inline bool ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(bool streamlineTurnedOff,
                                                                        bool exactNormalReturnReinitializedThisPresent,
                                                                        bool normalRouteOwnershipProven,
                                                                        bool overlayInit, bool syncInit,
                                                                        bool deviceRemoved) {
    // The first native Present has already rebuilt RTV/sync state on the exact
    // authoritative game swapchain. The late outer SL OFF observer must not
    // immediately tear that new state down as if it still referenced the old
    // proxy; doing so creates one uncovered Present before the next rebuild.
    return streamlineTurnedOff && exactNormalReturnReinitializedThisPresent && normalRouteOwnershipProven &&
           overlayInit && syncInit && !deviceRemoved;
}

inline bool ShouldRetirePostSLRouteForNormalSwapchainReturn(bool normalSwapchainReturn, bool postSLRouteArmed,
                                                            bool hasDistinctPostSLQueueProof) {
    return normalSwapchainReturn && postSLRouteArmed && hasDistinctPostSLQueueProof;
}

inline bool ShouldAbortPostSLSubmitAfterLifecycleChange(uint32_t entryEpoch, uint32_t currentEpoch) {
    return entryEpoch != currentEpoch;
}

inline bool ShouldAwaitAuthoritativeQueueChangeBaseline(bool hasAuthoritativeBaseline,
                                                        bool rawQueueMatchesAuthoritativeBaseline) {
    // A proven return to the game's original swapchain queue is a topology
    // boundary, not an FSR activation. Ignore leftover Streamline ECL traffic
    // until the authoritative queue itself is observed, then use it as the new
    // queue-change baseline. This is event-driven and does not delay a later
    // genuine FSR queue change.
    return hasAuthoritativeBaseline && !rawQueueMatchesAuthoritativeBaseline;
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

// How DetourPresent renders the overlay during native no-callback FSR FG (GTA-style: AMD composites a
// registered UI texture onto every real + interpolated frame with no app present callback).
enum class NoCallbackFSRFGOverlayRoute {
    kSkipBundleCovers,   // overlay rides AMD's UI-resource composition (game-ECL bundle) — skip the backbuffer
                         // ProcessFrame
    kMinimalBackbuffer,  // draw via minimal ProcessFrame (ONLY safe when AMD does NOT own the swapchain)
};

// CRASH BOUNDARY (session 20260621_191028 — amd_fidelityfx_dx12!ffxQuery null-deref AV):
// When AMD's FfxFrameInterpolationSwapchain owns presentation (runtimeOwnsSwapchain), CE must perform ZERO
// GPU work on AMD's backbuffer / runtime present queue. The minimal backbuffer ProcessFrame submits overlay
// work on exactly that queue, which corrupts AMD's presentation state and null-derefs it inside ffxQuery.
// So under runtime-owned FSR FG the overlay's ONLY channel is AMD's UI-resource composition: the game-ECL
// bundle draws the overlay onto the registered (or CE-substituted) UI texture on the GAME queue, which AMD
// composites post-interpolation on its own queue with the same game-queue->present sync the game's own HUD
// already rides. That path never touches AMD's runtime queue, so it is always selected here — never the
// backbuffer submit.
//
// The kMinimalBackbuffer route remains ONLY for a no-callback composition window where AMD does NOT own the
// swapchain (defensive escape hatch / non-runtime-owned harness paths). There, the backbuffer submit is safe;
// skip only when the bundle is actually firing so the overlay is never blank — never a silent skip-with-no-draw.
inline NoCallbackFSRFGOverlayRoute ChooseNoCallbackFSRFGOverlayRoute(bool runtimeOwnsSwapchain,
                                                                     bool liveSwapchainQueueIsOriginalGameQueue,
                                                                     bool fsrFGDisabledSuspendPending,
                                                                     bool uiResourceCachedForBundle,
                                                                     bool bundleOverlayActivelyFiring) {
    // STALE-LATCH RECOVERY (FSR->off, session 20260622_180830): the no-callback composition latch (and the
    // runtime-owned latch) can OUTLIVE the FSR session when the off-signal is missed — ffxDestroyContext can
    // be bypassed, the one-shot ffxConfigure VEH is permanently disarmed, and CE deliberately preserves
    // runtime ownership "until a stronger off signal arrives". When the game has recreated a NATIVE swapchain
    // and presents on its OWN queue again (the live swapchain queue is the original game queue), AMD's
    // FfxFrameInterpolationSwapchain is gone — AMD is no longer compositing the UI texture, so the bundle is
    // invisible. The backbuffer route is SAFE here (the crash boundary is submitting on AMD's SEPARATE FG
    // queue, which by definition != the original game queue), so draw via the backbuffer to keep the overlay
    // visible until the stale latch clears. This takes precedence over everything else.
    if (liveSwapchainQueueIsOriginalGameQueue) {
        return NoCallbackFSRFGOverlayRoute::kMinimalBackbuffer;
    }
    // WHILE AMD OWNS THE SWAPCHAIN, THE BACKBUFFER SUBMIT IS NEVER A SAFE OVERLAY ROUTE — ride the bundle
    // composite (CE's own fenced queue) instead. Two proven failure modes on AMD's runtime-owned swapchain:
    //   1. ACTIVE interpolation (session 20260621_191028): the backbuffer submit null-derefs AMD inside
    //      `ffxQuery` (the interpolation pacing wedge / crash boundary).
    //   2. SUSPENSION (ffxConfigure frameGenerationEnabled=0, AMD keeps the swapchain — session
    //      20260703_210021): the backbuffer submit's GPU-completion fence NEVER signals (AMD stops flushing
    //      its runtime queue while suspended), so DetourPresent's overlay wait TIMES OUT ~1s EVERY present →
    //      the app collapses to ~1 fps. This DISPROVED the earlier "suspension backbuffer is safe" assumption
    //      (the old `&& !fsrFGDisabledSuspendPending` relaxation); `fsrFGDisabledSuspendPending` no longer
    //      gates the route. Both interpolating and suspended runtime-owned states take the bundle here.
    // The overlay's ONLY AMD-safe, non-stalling channel while AMD owns the swapchain is the UI-resource
    // composite (bundle). If AMD is not compositing the UI resource during the suspension the overlay is at
    // worst blank — never a crash, never a 1 fps stall. The backbuffer is reachable again only once the game
    // has recreated its OWN native swapchain (liveSwapchainQueueIsOriginalGameQueue, handled above).
    (void)fsrFGDisabledSuspendPending;
    if (runtimeOwnsSwapchain) {
        return NoCallbackFSRFGOverlayRoute::kSkipBundleCovers;
    }
    // Non-runtime-owned (AMD does NOT own the swapchain — the backbuffer submit is safe and does not stall):
    // skip only when the bundle is actually covering the overlay; otherwise draw on the backbuffer so the
    // overlay is never blank.
    if (uiResourceCachedForBundle && bundleOverlayActivelyFiring) {
        return NoCallbackFSRFGOverlayRoute::kSkipBundleCovers;
    }
    return NoCallbackFSRFGOverlayRoute::kMinimalBackbuffer;
}

// Who drives the per-present FFX UI-resource composite (and the substitute UI-resource re-registration)
// during no-callback FSR FG.
enum class FFXUiCompositeDriver {
    // CE's vtable hook on the game-facing FFX FrameInterpolation PROXY swapchain Present — runs on the
    // GAME's present thread BEFORE AMD's Present executes. The only driver allowed to re-assert the
    // substitute UI resource (see MayReassertSubstituteUiResource).
    kProxyPresentPrework,
    // DetourPresent on AMD's INTERNAL real swapchain — runs on AMD's presenter thread. Composite-only
    // fallback for the window before the proxy hook is installed (or when it could not be installed).
    kRealPresentFallback,
};

inline FFXUiCompositeDriver ChooseFFXUiCompositeDriver(bool proxyPresentHookDriving) {
    return proxyPresentHookDriving ? FFXUiCompositeDriver::kProxyPresentPrework
                                   : FFXUiCompositeDriver::kRealPresentFallback;
}

// DEADLOCK BOUNDARY (session 20260701_213656 — GTA froze permanently on the FIRST FSR-FG frame):
// Re-asserting CE's substitute calls AMD's ffxConfigure(RegisterUiResource), and AMD's
// FrameInterpolationSwapChainDX12::registerUiResource takes the swapchain criticalSection. AMD's Present
// (game thread) HOLDS that criticalSection while spin-waiting WITHOUT TIMEOUT on compositionFenceCPU,
// which only advances once AMD's presenter thread completes the real present. DetourPresent for the real
// swapchain runs ON that presenter thread, so calling the re-assert from there closes the cycle:
//   game thread: Present -> EnterCS -> spin on compositionFenceCPU (holds CS forever)
//   presenter thread: real Present -> CE DetourPresent -> re-assert -> registerUiResource -> EnterCS (blocked)
// The re-assert is therefore ONLY legal from the proxy-present prework (game thread, BEFORE AMD's Present
// enters the CS) — the same thread + lock order as the game's own per-frame RegisterUiResource calls.
inline bool MayReassertSubstituteUiResource(FFXUiCompositeDriver driver) {
    return driver == FFXUiCompositeDriver::kProxyPresentPrework;
}

// Whether the swapchain handed to CE in ffxConfigure(FrameGeneration).swapChain is the game-facing FFX
// FrameInterpolation PROXY whose Present vtable entry CE may hook for the game-thread composite driver.
// The proxy's Present implementation lives inside the FFX runtime module (amd_fidelityfx_dx12.dll et al);
// a real DXGI swapchain resolves into dxgi.dll and an sl_interposer/CE-wrapped chain into those modules —
// none of which are the proxy, so hooking them would run the prework on the WRONG thread (the deadlock
// this driver exists to prevent) or double-hook CE's own detour.
inline bool ShouldInstallFFXProxyPresentHook(bool presentEntryInFFXRuntimeModule, bool presentEntryIsCEDetour,
                                             bool alreadyInstalledOnThisVtableEntry) {
    if (alreadyInstalledOnThisVtableEntry) {
        return false;  // idempotent: the class vtable entry is already routed to CE
    }
    if (presentEntryIsCEDetour) {
        return false;  // never stack CE hooks (also covers re-entry after install)
    }
    return presentEntryInFFXRuntimeModule;
}

// Which texture the overlay is drawn onto under no-callback FSR FG UI-resource composition.
enum class FFXUiOverlayTarget {
    kCompositeOntoGameTexture,     // game registered a usable, ~backbuffer-sized UI texture — blend overlay on top
    kSubstituteCEFullSizeTexture,  // game registered a degenerate placeholder (GTA's 1x1) — substitute CE's own texture
};

}  // namespace ce::dx12_overlay_policy
